// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/plant.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "dccp/cooling_observatory/checksum.hpp"

namespace dccp::cooling_observatory {
namespace {

constexpr std::array<std::pair<ComponentKind, std::string_view>, 11> kComponentKinds{{
    {ComponentKind::Pump, "pump"},
    {ComponentKind::Valve, "valve"},
    {ComponentKind::Chiller, "chiller"},
    {ComponentKind::Cdu, "cdu"},
    {ComponentKind::Crah, "crah"},
    {ComponentKind::Manifold, "manifold"},
    {ComponentKind::Branch, "branch"},
    {ComponentKind::HeatExchanger, "heat_exchanger"},
    {ComponentKind::CoolingTower, "cooling_tower"},
    {ComponentKind::Reservoir, "reservoir"},
    {ComponentKind::RackGroup, "rack_group"},
}};

constexpr std::array<std::pair<NeedKind, std::string_view>, 4> kNeedKinds{{
    {NeedKind::CoolantFlow, "coolant_flow"},
    {NeedKind::Airflow, "airflow"},
    {NeedKind::ElectricalPower, "electrical_power"},
    {NeedKind::HeatRejection, "heat_rejection"},
}};

constexpr std::array<std::pair<LinkRelation, std::string_view>, 7> kLinkRelations{{
    {LinkRelation::Supply, "supply"},
    {LinkRelation::Return, "return"},
    {LinkRelation::AirSupply, "air_supply"},
    {LinkRelation::AirReturn, "air_return"},
    {LinkRelation::ThermalCoupling, "thermal_coupling"},
    {LinkRelation::ElectricalFeed, "electrical_feed"},
    {LinkRelation::ControlBinding, "control_binding"},
}};

template <typename Enum, std::size_t N>
std::string_view token_of(const std::array<std::pair<Enum, std::string_view>, N>& table, Enum value) noexcept {
  for (const auto& entry : table) {
    if (entry.first == value) {
      return entry.second;
    }
  }
  return "unknown";
}

template <typename Enum, std::size_t N>
std::optional<Enum> parse_of(const std::array<std::pair<Enum, std::string_view>, N>& table,
                             std::string_view token) noexcept {
  for (const auto& entry : table) {
    if (entry.second == token) {
      return entry.first;
    }
  }
  return std::nullopt;
}

const SubjectRef* find_subject(const std::vector<SubjectRef>& subjects, const SubjectRef& subject) {
  const auto it = std::lower_bound(subjects.begin(), subjects.end(), subject);
  if (it == subjects.end() || *it != subject) {
    return nullptr;
  }
  return &*it;
}

std::size_t subject_position(const std::vector<SubjectRef>& subjects, const SubjectRef& subject) {
  const auto it = std::lower_bound(subjects.begin(), subjects.end(), subject);
  if (it == subjects.end() || *it != subject) {
    return subjects.size();
  }
  return static_cast<std::size_t>(it - subjects.begin());
}

/// Length-prefixed identity. A length prefix is what makes the canonical
/// encoding unambiguous: without it, two different models could produce the
/// same byte string by moving a delimiter into a value.
template <typename Id>
void append_identity(std::string& out, const Id& id) {
  out += std::to_string(id.view().size());
  out += ':';
  out += id.view();
  out += ';';
}

void append_text(std::string& out, std::string_view text) {
  out += std::to_string(text.size());
  out += ':';
  out += text;
  out += ';';
}

}  // namespace

std::string_view to_token(ComponentKind kind) noexcept { return token_of(kComponentKinds, kind); }
std::optional<ComponentKind> parse_component_kind(std::string_view token) noexcept {
  return parse_of(kComponentKinds, token);
}
std::string_view to_token(NeedKind kind) noexcept { return token_of(kNeedKinds, kind); }
std::optional<NeedKind> parse_need_kind(std::string_view token) noexcept { return parse_of(kNeedKinds, token); }
std::string_view to_token(LinkRelation relation) noexcept { return token_of(kLinkRelations, relation); }
std::optional<LinkRelation> parse_link_relation(std::string_view token) noexcept {
  return parse_of(kLinkRelations, token);
}

std::string describe(ComponentKind kind) {
  std::string out;
  out += to_token(kind);
  out += " (subject kind ";
  out += to_token(subject_kind_of(kind));
  out += ", needs";
  bool first = true;
  const NeedKind defaults[] = {NeedKind::CoolantFlow, NeedKind::Airflow, NeedKind::ElectricalPower,
                               NeedKind::HeatRejection};
  for (const NeedKind need : defaults) {
    const bool relevant = (kind == ComponentKind::Crah && need == NeedKind::Airflow) ||
                          (kind != ComponentKind::Crah && need == NeedKind::CoolantFlow) ||
                          need == NeedKind::ElectricalPower;
    if (!relevant) {
      continue;
    }
    out += first ? " " : ", ";
    out += to_token(need);
    first = false;
  }
  out += ")";
  return out;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void PlantModel::add_facility(FacilityId id, std::string label) {
  facilities_.push_back(std::move(id));
  (void)label;
  indexed_ = false;
}

void PlantModel::add_plant(CoolingPlant plant) {
  plants_.push_back(std::move(plant));
  indexed_ = false;
}

void PlantModel::add_loop(Loop loop) {
  loops_.push_back(std::move(loop));
  indexed_ = false;
}

void PlantModel::add_component(PlantComponent component) {
  components_.push_back(std::move(component));
  indexed_ = false;
}

void PlantModel::add_zone(ThermalZone zone) {
  zones_.push_back(std::move(zone));
  indexed_ = false;
}

void PlantModel::add_link(PlantLink link) {
  links_.push_back(std::move(link));
  indexed_ = false;
}

// ---------------------------------------------------------------------------
// Indexes
// ---------------------------------------------------------------------------

void PlantModel::reindex() {
  std::sort(plants_.begin(), plants_.end());
  plants_.erase(std::unique(plants_.begin(), plants_.end()), plants_.end());
  std::sort(loops_.begin(), loops_.end());
  loops_.erase(std::unique(loops_.begin(), loops_.end()), loops_.end());
  std::sort(components_.begin(), components_.end());
  components_.erase(std::unique(components_.begin(), components_.end()), components_.end());
  std::sort(zones_.begin(), zones_.end());
  zones_.erase(std::unique(zones_.begin(), zones_.end()), zones_.end());
  std::sort(facilities_.begin(), facilities_.end());
  facilities_.erase(std::unique(facilities_.begin(), facilities_.end()), facilities_.end());

  // Duplicate links are collapsed: the same edge stated twice is one edge, and
  // keeping both would make a traversal report the same neighbour twice.
  std::sort(links_.begin(), links_.end());
  links_.erase(std::unique(links_.begin(), links_.end()), links_.end());

  component_index_.clear();
  component_index_.reserve(components_.size());
  for (std::size_t i = 0; i < components_.size(); ++i) {
    component_index_.push_back(i);
  }
  std::sort(component_index_.begin(), component_index_.end(), [this](std::size_t a, std::size_t b) {
    return components_[a] < components_[b];
  });

  link_index_.clear();
  link_index_.reserve(links_.size());
  for (std::size_t i = 0; i < links_.size(); ++i) {
    link_index_.push_back(i);
  }
  std::sort(link_index_.begin(), link_index_.end(), [this](std::size_t a, std::size_t b) {
    return links_[a] < links_[b];
  });

  // Adjacency: one compact range per known subject. This is what keeps a
  // traversal linear in the number of edges instead of quadratic in the number
  // of links, which matters because a facility-wide generation can carry
  // hundreds of thousands of them.
  adjacency_subjects_.clear();
  adjacency_subjects_.reserve(components_.size() + zones_.size() + loops_.size() + plants_.size());
  for (const PlantComponent& candidate : components_) {
    if (!candidate.id.empty()) {
      adjacency_subjects_.push_back(candidate.subject());
    }
  }
  for (const ThermalZone& candidate : zones_) {
    if (!candidate.id.empty()) {
      adjacency_subjects_.push_back(SubjectRef(SubjectKind::Zone, rebind<StrongId>(candidate.id)));
    }
  }
  for (const Loop& candidate : loops_) {
    if (!candidate.id.empty()) {
      adjacency_subjects_.push_back(SubjectRef(SubjectKind::Loop, rebind<StrongId>(candidate.id)));
    }
  }
  for (const CoolingPlant& candidate : plants_) {
    if (!candidate.id.empty()) {
      adjacency_subjects_.push_back(SubjectRef(SubjectKind::Plant, rebind<StrongId>(candidate.id)));
    }
  }
  std::sort(adjacency_subjects_.begin(), adjacency_subjects_.end());
  adjacency_subjects_.erase(std::unique(adjacency_subjects_.begin(), adjacency_subjects_.end()),
                            adjacency_subjects_.end());

  const std::size_t subject_count = adjacency_subjects_.size();
  outgoing_begin_.assign(subject_count + 1, 0);
  outgoing_end_.assign(subject_count + 1, 0);
  incoming_begin_.assign(subject_count + 1, 0);
  incoming_end_.assign(subject_count + 1, 0);
  outgoing_links_.clear();
  incoming_links_.clear();

  auto index_of = [this](const SubjectRef& subject) -> std::size_t {
    const auto it = std::lower_bound(adjacency_subjects_.begin(), adjacency_subjects_.end(), subject);
    if (it == adjacency_subjects_.end() || *it != subject) {
      return adjacency_subjects_.size();
    }
    return static_cast<std::size_t>(it - adjacency_subjects_.begin());
  };

  // Outgoing ranges, bucketed by source subject. The buckets are built by
  // counting first, so no bucket ever reallocates, and the links inside each
  // bucket are sorted afterwards so a traversal sees a canonical order.
  std::vector<std::size_t> outgoing_counts(subject_count, 0);
  for (const PlantLink& link : links_) {
    const std::size_t index = index_of(link.from);
    if (index < subject_count) {
      ++outgoing_counts[index];
    }
  }
  for (std::size_t i = 0; i < subject_count; ++i) {
    outgoing_begin_[i] = static_cast<std::uint32_t>(outgoing_links_.size());
    outgoing_links_.insert(outgoing_links_.end(), outgoing_counts[i], 0);
    outgoing_end_[i] = static_cast<std::uint32_t>(outgoing_links_.size());
  }
  outgoing_begin_[subject_count] = static_cast<std::uint32_t>(outgoing_links_.size());
  outgoing_end_[subject_count] = static_cast<std::uint32_t>(outgoing_links_.size());
  {
    std::vector<std::size_t> cursor(outgoing_begin_.begin(), outgoing_begin_.end());
    for (std::size_t link_index = 0; link_index < links_.size(); ++link_index) {
      const std::size_t index = index_of(links_[link_index].from);
      if (index < subject_count) {
        outgoing_links_[cursor[index]++] = link_index;
      }
    }
  }
  for (std::size_t i = 0; i < subject_count; ++i) {
    std::sort(outgoing_links_.begin() + outgoing_begin_[i], outgoing_links_.begin() + outgoing_end_[i],
              [this](std::size_t a, std::size_t b) { return links_[a] < links_[b]; });
  }

  std::vector<std::size_t> incoming_counts(subject_count, 0);
  for (const PlantLink& link : links_) {
    const std::size_t index = index_of(link.to);
    if (index < subject_count) {
      ++incoming_counts[index];
    }
  }
  for (std::size_t i = 0; i < subject_count; ++i) {
    incoming_begin_[i] = static_cast<std::uint32_t>(incoming_links_.size());
    incoming_links_.insert(incoming_links_.end(), incoming_counts[i], 0);
    incoming_end_[i] = static_cast<std::uint32_t>(incoming_links_.size());
  }
  incoming_begin_[subject_count] = static_cast<std::uint32_t>(incoming_links_.size());
  incoming_end_[subject_count] = static_cast<std::uint32_t>(incoming_links_.size());
  {
    std::vector<std::size_t> cursor(incoming_begin_.begin(), incoming_begin_.end());
    for (std::size_t link_index = 0; link_index < links_.size(); ++link_index) {
      const std::size_t index = index_of(links_[link_index].to);
      if (index < subject_count) {
        incoming_links_[cursor[index]++] = link_index;
      }
    }
  }
  for (std::size_t i = 0; i < subject_count; ++i) {
    std::sort(incoming_links_.begin() + incoming_begin_[i], incoming_links_.begin() + incoming_end_[i],
              [this](std::size_t a, std::size_t b) { return links_[a] < links_[b]; });
  }

  indexed_ = true;
}

void PlantModel::canonicalise() { reindex(); }

bool PlantModel::operator==(const PlantModel& other) const noexcept {
  return facilities_ == other.facilities_ && plants_ == other.plants_ && loops_ == other.loops_ &&
         components_ == other.components_ && zones_ == other.zones_ && links_ == other.links_;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::optional<CoolingPlant> PlantModel::plant(PlantId id) const {
  const auto it = std::lower_bound(plants_.begin(), plants_.end(), id,
                                   [](const CoolingPlant& candidate, PlantId key) { return candidate.id < key; });
  if (it == plants_.end() || it->id != id) {
    return std::nullopt;
  }
  return *it;
}

std::optional<Loop> PlantModel::loop(LoopId id) const {
  const auto it = std::lower_bound(loops_.begin(), loops_.end(), id,
                                   [](const Loop& candidate, LoopId key) { return candidate.id < key; });
  if (it == loops_.end() || it->id != id) {
    return std::nullopt;
  }
  return *it;
}

std::optional<PlantComponent> PlantModel::component(SubjectKind kind, const StrongId& id) const {
  const SubjectRef key(kind, id);
  for (const PlantComponent& candidate : components_) {
    if (candidate.id.empty()) {
      continue;
    }
    if (candidate.subject() == key) {
      return candidate;
    }
  }
  return std::nullopt;
}

std::optional<ThermalZone> PlantModel::zone(ZoneId id) const {
  const auto it = std::lower_bound(zones_.begin(), zones_.end(), id,
                                   [](const ThermalZone& candidate, ZoneId key) { return candidate.id < key; });
  if (it == zones_.end() || it->id != id) {
    return std::nullopt;
  }
  return *it;
}

std::vector<PlantComponent> PlantModel::components_of_kind(ComponentKind kind) const {
  std::vector<PlantComponent> out;
  for (const PlantComponent& candidate : components_) {
    if (candidate.kind == kind) {
      out.push_back(candidate);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<PlantComponent> PlantModel::components_in_loop(LoopId loop_id) const {
  std::vector<PlantComponent> out;
  for (const PlantComponent& candidate : components_) {
    if (candidate.loop == loop_id) {
      out.push_back(candidate);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<PlantLink> PlantModel::outgoing(const SubjectRef& from, bool delivery_only) const {
  std::vector<PlantLink> out;
  if (indexed_) {
    const std::size_t index = subject_position(adjacency_subjects_, from);
    if (index < adjacency_subjects_.size()) {
      for (std::uint32_t i = outgoing_begin_[index]; i < outgoing_end_[index]; ++i) {
        const PlantLink& link = links_[outgoing_links_[i]];
        if (!delivery_only || carries_delivery(link.relation)) {
          out.push_back(link);
        }
      }
    }
    return out;
  }
  for (const PlantLink& link : links_) {
    if (link.from == from && (!delivery_only || carries_delivery(link.relation))) {
      out.push_back(link);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<PlantLink> PlantModel::incoming(const SubjectRef& to, bool delivery_only) const {
  std::vector<PlantLink> out;
  if (indexed_) {
    const std::size_t index = subject_position(adjacency_subjects_, to);
    if (index < adjacency_subjects_.size()) {
      for (std::uint32_t i = incoming_begin_[index]; i < incoming_end_[index]; ++i) {
        const PlantLink& link = links_[incoming_links_[i]];
        if (!delivery_only || carries_delivery(link.relation)) {
          out.push_back(link);
        }
      }
    }
    return out;
  }
  for (const PlantLink& link : links_) {
    if (link.to == to && (!delivery_only || carries_delivery(link.relation))) {
      out.push_back(link);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool PlantModel::knows(const SubjectRef& subject) const {
  if (subject.id.empty()) {
    return false;
  }
  if (indexed_) {
    return find_subject(adjacency_subjects_, subject) != nullptr;
  }
  for (const PlantComponent& candidate : components_) {
    if (!candidate.id.empty() && candidate.subject() == subject) {
      return true;
    }
  }
  for (const ThermalZone& candidate : zones_) {
    if (!candidate.id.empty() && SubjectRef(SubjectKind::Zone, rebind<StrongId>(candidate.id)) == subject) {
      return true;
    }
  }
  for (const Loop& candidate : loops_) {
    if (!candidate.id.empty() && SubjectRef(SubjectKind::Loop, rebind<StrongId>(candidate.id)) == subject) {
      return true;
    }
  }
  for (const CoolingPlant& candidate : plants_) {
    if (!candidate.id.empty() && SubjectRef(SubjectKind::Plant, rebind<StrongId>(candidate.id)) == subject) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Validation and digest
// ---------------------------------------------------------------------------

Status PlantModel::validate(const Limits& limits) const {
  if (facilities_.size() > limits.max_facilities) {
    return Status::failure(limit_error("max_facilities", limits.max_facilities, facilities_.size()));
  }
  if (plants_.size() > limits.max_plants) {
    return Status::failure(limit_error("max_plants", limits.max_plants, plants_.size()));
  }
  if (loops_.size() > limits.max_loops) {
    return Status::failure(limit_error("max_loops", limits.max_loops, loops_.size()));
  }
  if (components_.size() > limits.max_elements) {
    return Status::failure(limit_error("max_elements", limits.max_elements, components_.size()));
  }
  if (zones_.size() > limits.max_zones) {
    return Status::failure(limit_error("max_zones", limits.max_zones, zones_.size()));
  }
  if (links_.size() > limits.max_links) {
    return Status::failure(limit_error("max_links", limits.max_links, links_.size()));
  }

  // Identity uniqueness within a subject kind. Two elements of different kinds
  // may share a spelling; two of the same kind may not, because every answer
  // addresses a subject by (kind, id) and an ambiguous pair would make an
  // answer ambiguous.
  std::vector<SubjectRef> seen;
  seen.reserve(components_.size() + zones_.size());
  for (const PlantComponent& candidate : components_) {
    const SubjectRef subject = candidate.subject();
    if (!subject.id.empty()) {
      seen.push_back(subject);
    }
  }
  for (const ThermalZone& candidate : zones_) {
    if (!candidate.id.empty()) {
      seen.push_back(SubjectRef(SubjectKind::Zone, rebind<StrongId>(candidate.id)));
    }
  }
  std::sort(seen.begin(), seen.end());
  for (std::size_t i = 1; i < seen.size(); ++i) {
    if (seen[i] == seen[i - 1]) {
      return Status::failure(Code::InvalidArgument, "duplicate_subject",
                             std::string("the subject ") + std::string(to_token(seen[i].kind)) + " '" +
                                 seen[i].id.str() + "' is declared more than once");
    }
  }

  for (const Loop& candidate : loops_) {
    if (!candidate.plant.empty() && !plant(candidate.plant).has_value()) {
      return Status::failure(Code::MissingEvidence, "loop_plant_missing",
                             "loop '" + candidate.id.str() + "' names plant '" + candidate.plant.str() +
                                 "' which is not in this generation");
    }
  }
  for (const CoolingPlant& candidate : plants_) {
    if (!candidate.facility.empty()) {
      const bool known = std::find(facilities_.begin(), facilities_.end(), candidate.facility) !=
                         facilities_.end();
      if (!known) {
        return Status::failure(Code::MissingEvidence, "plant_facility_missing",
                               "plant '" + candidate.id.str() + "' names facility '" +
                                   candidate.facility.str() + "' which is not in this generation");
      }
    }
  }
  for (const PlantComponent& candidate : components_) {
    if (!candidate.loop.empty() && !loop(candidate.loop).has_value()) {
      return Status::failure(Code::MissingEvidence, "component_loop_missing",
                             "component '" + candidate.id.str() + "' names loop '" + candidate.loop.str() +
                                 "' which is not in this generation");
    }
  }

  // Links may name subjects that a consumer will later resolve; a link whose
  // endpoint is absent is reported by traversal as dangling rather than
  // rejected here, because a partially adopted generation is a real state and
  // refusing it would leave the observatory unable to describe the plant at all.
  return Status::success();
}

std::string PlantModel::digest() const {
  // A canonical, unambiguous encoding: every identity and string is
  // length-prefixed, so no two distinct models can produce the same bytes by
  // shifting a delimiter into a value.
  std::string canonical;
  canonical.reserve(256 + components_.size() * 32 + links_.size() * 32);
  canonical += "cooling-observatory/plant/1;";
  canonical += "f=" + std::to_string(facilities_.size()) + ";";
  for (const FacilityId& id : facilities_) {
    append_identity(canonical, id);
  }
  canonical += "p=" + std::to_string(plants_.size()) + ";";
  for (const CoolingPlant& candidate : plants_) {
    append_identity(canonical, candidate.id);
    append_identity(canonical, rebind<StrongId>(candidate.facility));
    append_text(canonical, candidate.label);
  }
  canonical += "l=" + std::to_string(loops_.size()) + ";";
  for (const Loop& candidate : loops_) {
    append_identity(canonical, candidate.id);
    append_identity(canonical, rebind<StrongId>(candidate.plant));
    canonical += candidate.secondary ? "1;" : "0;";
    append_text(canonical, candidate.label);
  }
  canonical += "c=" + std::to_string(components_.size()) + ";";
  for (const PlantComponent& candidate : components_) {
    canonical += to_token(candidate.kind);
    canonical += ';';
    append_identity(canonical, candidate.id);
    append_identity(canonical, rebind<StrongId>(candidate.loop));
    append_text(canonical, candidate.label);
    canonical += "n=" + std::to_string(candidate.needs.size()) + ";";
    for (const NeedKind need : candidate.needs) {
      canonical += to_token(need);
      canonical += ';';
    }
  }
  canonical += "z=" + std::to_string(zones_.size()) + ";";
  for (const ThermalZone& candidate : zones_) {
    append_identity(canonical, candidate.id);
    append_identity(canonical, rebind<StrongId>(candidate.facility));
    append_text(canonical, candidate.label);
    canonical += "load=";
    if (candidate.declared_load.has_value()) {
      canonical += to_token(candidate.declared_load.value().dimension);
      canonical += ':';
      canonical += std::to_string(candidate.declared_load.value().value);
    } else {
      canonical += "unknown";
    }
    canonical += ";temp=";
    if (candidate.max_supply_temperature.has_value()) {
      canonical += to_token(candidate.max_supply_temperature.value().dimension);
      canonical += ':';
      canonical += std::to_string(candidate.max_supply_temperature.value().value);
    } else {
      canonical += "unknown";
    }
    canonical += ';';
  }
  canonical += "k=" + std::to_string(links_.size()) + ";";
  for (const PlantLink& link : links_) {
    canonical += to_token(link.from.kind);
    canonical += ':';
    append_identity(canonical, link.from.id);
    canonical += to_token(link.to.kind);
    canonical += ':';
    append_identity(canonical, link.to.id);
    canonical += to_token(link.relation);
    canonical += ';';
  }
  return sha256_hex(canonical);
}

}  // namespace dccp::cooling_observatory