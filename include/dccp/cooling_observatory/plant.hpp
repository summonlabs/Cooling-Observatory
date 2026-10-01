// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_PLANT_HPP
#define DCCP_COOLING_OBSERVATORY_PLANT_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/cooling_observatory/error.hpp"
#include "dccp/cooling_observatory/identity.hpp"
#include "dccp/cooling_observatory/limits.hpp"
#include "dccp/cooling_observatory/semantics.hpp"
#include "dccp/cooling_observatory/units.hpp"

namespace dccp::cooling_observatory {

/// The kinds of cooling equipment this runtime can name.
///
/// The list is deliberately closed. An observatory that accepts an open
/// vocabulary of equipment kinds cannot localise a constraint to a class of
/// equipment, and every downstream consumer would have to re-derive the same
/// taxonomy. Unknown future equipment is named by its closest existing kind and
/// carries its own label.
enum class ComponentKind : std::uint8_t {
  Pump = 0,
  Valve = 1,
  Chiller = 2,
  Cdu = 3,      ///< coolant distribution unit
  Crah = 4,     ///< computer room air handler (CRAH or CRAC)
  Manifold = 5,
  Branch = 6,
  HeatExchanger = 7,
  CoolingTower = 8,
  Reservoir = 9,
  RackGroup = 10,  ///< a heat source: the load being cooled, observed but not owned
};

[[nodiscard]] std::string_view to_token(ComponentKind kind) noexcept;
[[nodiscard]] std::optional<ComponentKind> parse_component_kind(std::string_view token) noexcept;
[[nodiscard]] constexpr SubjectKind subject_kind_of(ComponentKind kind) noexcept {
  switch (kind) {
    case ComponentKind::Pump:
      return SubjectKind::Pump;
    case ComponentKind::Valve:
      return SubjectKind::Valve;
    case ComponentKind::Chiller:
      return SubjectKind::Chiller;
    case ComponentKind::Cdu:
      return SubjectKind::Cdu;
    case ComponentKind::Crah:
      return SubjectKind::Crah;
    case ComponentKind::Manifold:
      return SubjectKind::Manifold;
    case ComponentKind::Branch:
      return SubjectKind::Branch;
    case ComponentKind::HeatExchanger:
    case ComponentKind::CoolingTower:
    case ComponentKind::Reservoir:
      return SubjectKind::Plant;
    case ComponentKind::RackGroup:
      return SubjectKind::RackGroup;
  }
  return SubjectKind::Plant;
}

/// What a component needs in order to deliver.
enum class NeedKind : std::uint8_t {
  CoolantFlow = 0,
  Airflow = 1,
  ElectricalPower = 2,
  HeatRejection = 3,
};

[[nodiscard]] std::string_view to_token(NeedKind kind) noexcept;
[[nodiscard]] std::optional<NeedKind> parse_need_kind(std::string_view token) noexcept;

/// How two elements are connected.
///
/// Direction is physical, not logical: Supply means "the source delivers into
/// the target". Traversal follows these edges, and the observatory never
/// reverses an edge unless the caller explicitly asks for the reverse view.
enum class LinkRelation : std::uint8_t {
  Supply = 0,       ///< source feeds target
  Return = 1,       ///< target returns to source
  AirSupply = 2,
  AirReturn = 3,
  ThermalCoupling = 4,  ///< conducts heat without carrying coolant
  ElectricalFeed = 5,
  ControlBinding = 6,   ///< control relationship, recorded and never traversed for delivery
};

[[nodiscard]] std::string_view to_token(LinkRelation relation) noexcept;
[[nodiscard]] std::optional<LinkRelation> parse_link_relation(std::string_view token) noexcept;

/// True when following a relation carries delivered coolant or air, and so
/// belongs in a delivery path.
[[nodiscard]] constexpr bool carries_delivery(LinkRelation relation) noexcept {
  switch (relation) {
    case LinkRelation::Supply:
    case LinkRelation::Return:
    case LinkRelation::AirSupply:
    case LinkRelation::AirReturn:
      return true;
    default:
      return false;
  }
}

/// One piece of cooling equipment, as observed.
struct PlantComponent {
  ComponentKind kind = ComponentKind::Pump;
  StrongId id{};
  /// The loop this element participates in, when the topology places it in one.
  LoopId loop{};
  /// Human-facing name from the topology authority. Explanation only.
  std::string label{};
  /// What this element needs in order to deliver anything.
  std::vector<NeedKind> needs{};

  [[nodiscard]] SubjectRef subject() const noexcept { return SubjectRef(subject_kind_of(kind), id); }
  friend bool operator==(const PlantComponent& a, const PlantComponent& b) noexcept {
    return a.kind == b.kind && a.id == b.id && a.loop == b.loop;
  }
  friend bool operator<(const PlantComponent& a, const PlantComponent& b) noexcept {
    if (a.kind != b.kind) {
      return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
    }
    return a.id < b.id;
  }
};

/// A hydronic or air loop: the unit at which coolant is circulated and measured.
struct Loop {
  LoopId id{};
  PlantId plant{};
  std::string label{};
  /// True for a secondary (facility-water or air) loop, false for a primary
  /// (plant-water) loop. The distinction matters because reserve in the
  /// secondary loop does not imply reserve in the primary loop.
  bool secondary = false;

  friend bool operator==(const Loop& a, const Loop& b) noexcept { return a.id == b.id; }
  friend bool operator<(const Loop& a, const Loop& b) noexcept { return a.id < b.id; }
};

/// A cooling plant: chillers, towers, primary pumps and their shared headers.
struct CoolingPlant {
  PlantId id{};
  FacilityId facility{};
  std::string label{};

  friend bool operator==(const CoolingPlant& a, const CoolingPlant& b) noexcept { return a.id == b.id; }
  friend bool operator<(const CoolingPlant& a, const CoolingPlant& b) noexcept { return a.id < b.id; }
};

/// A thermal zone: the region whose heat has to be removed, and the place a
/// consumer actually cares about.
struct ThermalZone {
  ZoneId id{};
  FacilityId facility{};
  std::string label{};
  /// Declared heat load of the zone, when the placement authority states one.
  Maybe<Quantity> declared_load{};
  /// The maximum supply temperature the zone tolerates. A thermal-removal
  /// constraint is only meaningful against a bound like this one.
  Maybe<Quantity> max_supply_temperature{};

  friend bool operator==(const ThermalZone& a, const ThermalZone& b) noexcept { return a.id == b.id; }
  friend bool operator<(const ThermalZone& a, const ThermalZone& b) noexcept { return a.id < b.id; }
};

/// A directed connection between two named subjects.
struct PlantLink {
  SubjectRef from{};
  SubjectRef to{};
  LinkRelation relation = LinkRelation::Supply;

  friend bool operator==(const PlantLink& a, const PlantLink& b) noexcept {
    return a.from == b.from && a.to == b.to && a.relation == b.relation;
  }
  friend bool operator<(const PlantLink& a, const PlantLink& b) noexcept {
    if (a.from != b.from) {
      return a.from < b.from;
    }
    if (a.to != b.to) {
      return a.to < b.to;
    }
    return static_cast<std::uint8_t>(a.relation) < static_cast<std::uint8_t>(b.relation);
  }
};

/// The structure the observatory holds, adopted from the cooling topology
/// authority as one generation.
///
/// Holding a structure is not owning it. Every answer derived from this model
/// cites the generation it was computed against, so a consumer can see that the
/// answer describes the plant as of that generation.
class PlantModel {
 public:
  PlantModel() = default;

  /// Appending is O(1) and does not look for duplicates: structure arrives in
  /// batches, and a duplicate check per element would make adoption quadratic.
  /// reindex() canonicalises the batch, which collapses duplicates.
  void add_facility(FacilityId id, std::string label);
  void add_plant(CoolingPlant plant);
  void add_loop(Loop loop);
  void add_component(PlantComponent component);
  void add_zone(ThermalZone zone);
  void add_link(PlantLink link);

  [[nodiscard]] const std::vector<FacilityId>& facilities() const noexcept { return facilities_; }
  [[nodiscard]] const std::vector<CoolingPlant>& plants() const noexcept { return plants_; }
  [[nodiscard]] const std::vector<Loop>& loops() const noexcept { return loops_; }
  [[nodiscard]] const std::vector<PlantComponent>& components() const noexcept { return components_; }
  [[nodiscard]] const std::vector<ThermalZone>& zones() const noexcept { return zones_; }
  [[nodiscard]] const std::vector<PlantLink>& links() const noexcept { return links_; }

  [[nodiscard]] std::optional<CoolingPlant> plant(PlantId id) const;
  [[nodiscard]] std::optional<Loop> loop(LoopId id) const;
  [[nodiscard]] std::optional<PlantComponent> component(SubjectKind kind, const StrongId& id) const;
  [[nodiscard]] std::optional<ThermalZone> zone(ZoneId id) const;

  /// Every component of one kind, ordered by identity.
  [[nodiscard]] std::vector<PlantComponent> components_of_kind(ComponentKind kind) const;
  /// Every component in a loop, ordered by kind then identity.
  [[nodiscard]] std::vector<PlantComponent> components_in_loop(LoopId loop) const;

  /// True when this model names the subject. Used to decide whether a fact is
  /// about a structure this runtime actually holds, instead of silently
  /// treating every unknown identity as an isolated element.
  [[nodiscard]] bool knows(const SubjectRef& subject) const;

  /// Outgoing links from a subject, in canonical order, optionally filtered to
  /// relations that carry delivery.
  [[nodiscard]] std::vector<PlantLink> outgoing(const SubjectRef& from, bool delivery_only = false) const;
  [[nodiscard]] std::vector<PlantLink> incoming(const SubjectRef& to, bool delivery_only = false) const;

  /// Canonicalise the model and rebuild the indexes queries use: sorts every
  /// collection, collapses exact duplicate links, and rebuilds the per-subject
  /// link ranges. Queries fall back to a linear scan before the first reindex(),
  /// so a partially built model still answers correctly, only more slowly.
  void reindex();

  /// Structural checks: duplicate identities, dangling link endpoints, a
  /// component whose loop is not in the model, a loop whose plant is not in the
  /// model. Returns the first violation in canonical order.
  [[nodiscard]] Status validate(const Limits& limits) const;

  /// Ordering and equality are canonical, so a model built in any order
  /// compares and digests identically.
  void canonicalise();
  [[nodiscard]] bool operator==(const PlantModel& other) const noexcept;
  [[nodiscard]] std::string digest() const;

  [[nodiscard]] std::size_t size() const noexcept {
    return facilities_.size() + plants_.size() + loops_.size() + components_.size() + zones_.size() +
           links_.size();
  }

 private:
  std::vector<FacilityId> facilities_;
  std::vector<CoolingPlant> plants_;
  std::vector<Loop> loops_;
  std::vector<PlantComponent> components_;
  std::vector<ThermalZone> zones_;
  std::vector<PlantLink> links_;

  // Derived indexes, rebuilt by reindex().
  std::vector<std::size_t> component_index_;  ///< parallel to components_, sorted by (kind,id)
  std::vector<std::size_t> link_index_;       ///< parallel to links_, sorted by (from,to,relation)
  /// Every subject named by a component, zone, loop or plant, in canonical
  /// order, with the outgoing and incoming link ranges that belong to it.
  std::vector<SubjectRef> adjacency_subjects_;
  std::vector<std::uint32_t> outgoing_begin_;
  std::vector<std::uint32_t> outgoing_end_;
  std::vector<std::uint32_t> incoming_begin_;
  std::vector<std::uint32_t> incoming_end_;
  std::vector<std::size_t> outgoing_links_;
  std::vector<std::size_t> incoming_links_;
  bool indexed_ = false;
};

/// Render a component kind's canonical token list for diagnostics.
[[nodiscard]] std::string describe(ComponentKind kind);

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_PLANT_HPP
