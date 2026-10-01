// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/textproto.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>

#include "dccp/cooling_observatory/clock.hpp"
#include "dccp/cooling_observatory/units.hpp"

namespace dccp::cooling_observatory {
namespace {

bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r'; }

std::string trim(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && is_space(text[begin])) {
    ++begin;
  }
  while (end > begin && is_space(text[end - 1])) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

/// Split a statement value on whitespace. A field value never contains a
/// space: a label that needs one is written with an underscore, which keeps
/// every statement parseable without quoting rules.
std::vector<std::string_view> split_fields(std::string_view text) {
  std::vector<std::string_view> out;
  std::size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && is_space(text[index])) {
      ++index;
    }
    const std::size_t begin = index;
    while (index < text.size() && !is_space(text[index])) {
      ++index;
    }
    if (index > begin) {
      out.push_back(text.substr(begin, index - begin));
    }
  }
  return out;
}

Error at(const Statement& statement, Code code, std::string reason, std::string detail) {
  return Error(code, std::move(reason),
               "line " + std::to_string(statement.line) + ": " + std::move(detail));
}

Result<std::int64_t> to_i64(std::string_view text) {
  if (text.empty()) {
    return fail(Code::MalformedInput, "integer_empty", "an integer value is required");
  }
  std::string buffer(text);
  char* end = nullptr;
  errno = 0;
  const long long value = std::strtoll(buffer.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0') {
    return fail(Code::MalformedInput, "integer_syntax", "the value is not a base-10 integer");
  }
  return static_cast<std::int64_t>(value);
}

Result<std::uint64_t> to_u64(std::string_view text) {
  if (!text.empty() && text.front() == '-') {
    return fail(Code::MalformedInput, "unsigned_negative", "the value must not be negative");
  }
  auto value = to_i64(text);
  if (!value) {
    return value.error();
  }
  return static_cast<std::uint64_t>(value.value());
}

Result<bool> to_bool(std::string_view text) {
  if (text == "true" || text == "1" || text == "yes" || text.empty()) {
    return true;
  }
  if (text == "false" || text == "0" || text == "no") {
    return false;
  }
  return fail(Code::MalformedInput, "boolean_syntax", "expected true or false");
}

/// Statement lookup that reports a missing required key by name, so a
/// diagnostic names the field rather than the parser's internal state.
class RecordBuilder {
 public:
  RecordBuilder(std::vector<Statement> statements, std::size_t first_line)
      : statements_(std::move(statements)), first_line_(first_line) {}

  [[nodiscard]] const std::vector<Statement>& statements() const noexcept { return statements_; }
  [[nodiscard]] std::size_t first_line() const noexcept { return first_line_; }

  [[nodiscard]] const Statement* find(std::string_view key) const {
    for (const Statement& statement : statements_) {
      if (statement.key == key) {
        return &statement;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::vector<const Statement*> all(std::string_view key) const {
    std::vector<const Statement*> out;
    for (const Statement& statement : statements_) {
      if (statement.key == key) {
        out.push_back(&statement);
      }
    }
    return out;
  }

  [[nodiscard]] Error missing(std::string_view key) const {
    Statement placeholder;
    placeholder.line = first_line_;
    return at(placeholder, Code::MalformedInput, "required_field_missing",
              "the record needs a '" + std::string(key) + "' field");
  }

 private:
  std::vector<Statement> statements_;
  std::size_t first_line_ = 0;
};

}  // namespace

Result<std::vector<Statement>> parse_statements(std::string_view text) {
  std::vector<Statement> out;
  std::size_t line_number = 0;
  std::size_t position = 0;
  while (position <= text.size()) {
    const std::size_t newline = text.find('\n', position);
    const std::string_view line =
        newline == std::string_view::npos ? text.substr(position) : text.substr(position, newline - position);
    ++line_number;

    std::size_t begin = 0;
    while (begin < line.size() && is_space(line[begin])) {
      ++begin;
    }
    if (begin < line.size() && line[begin] != '#') {
      std::size_t split = begin;
      while (split < line.size() && !is_space(line[split])) {
        ++split;
      }
      std::size_t value_begin = split;
      while (value_begin < line.size() && is_space(line[value_begin])) {
        ++value_begin;
      }
      Statement statement;
      statement.line = line_number;
      statement.key = std::string(line.substr(begin, split - begin));
      statement.value = trim(line.substr(value_begin));
      if (!statement.key.empty()) {
        out.push_back(std::move(statement));
      }
    }

    if (newline == std::string_view::npos) {
      break;
    }
    position = newline + 1;
  }
  return out;
}

Result<std::int64_t> parse_i64(const Statement& statement) {
  auto value = to_i64(statement.value);
  if (!value) {
    return at(statement, value.error().code, value.error().reason, value.error().detail);
  }
  return value.value();
}

Result<std::uint64_t> parse_u64(const Statement& statement) {
  auto value = to_u64(statement.value);
  if (!value) {
    return at(statement, value.error().code, value.error().reason, value.error().detail);
  }
  return value.value();
}

Result<bool> parse_bool(const Statement& statement) {
  auto value = to_bool(statement.value);
  if (!value) {
    return at(statement, value.error().code, value.error().reason, value.error().detail);
  }
  return value.value();
}

Result<TimestampMs> parse_timestamp_statement(const Statement& statement) {
  if (statement.value.empty()) {
    return at(statement, Code::MalformedInput, "timestamp_empty", "a timestamp is required");
  }
  // An integer is accepted as milliseconds since the epoch, so that a synthetic
  // feed can state instants without formatting them.
  bool all_digits = statement.value.front() == '-' || (statement.value.front() >= '0' && statement.value.front() <= '9');
  for (const char c : statement.value) {
    if (c != '-' && (c < '0' || c > '9')) {
      all_digits = false;
      break;
    }
  }
  if (all_digits) {
    auto value = to_i64(statement.value);
    if (value) {
      return value.value();
    }
  }
  auto parsed = parse_timestamp(statement.value);
  if (!parsed) {
    return at(statement, parsed.error().code, parsed.error().reason, parsed.error().detail);
  }
  return parsed.value();
}

Result<Quantity> parse_quantity_statement(const Statement& statement) {
  auto parsed = parse_quantity(statement.value);
  if (!parsed) {
    return at(statement, parsed.error().code, parsed.error().reason, parsed.error().detail);
  }
  return parsed.value();
}

namespace {

Result<StrongId> id_of(const Statement& statement, std::string_view what) {
  auto parsed = StrongId::parse(statement.value);
  if (!parsed) {
    return at(statement, parsed.error().code, parsed.error().reason,
              std::string("the ") + std::string(what) + " is not a valid identity: " +
                  parsed.error().detail);
  }
  return parsed.value();
}

Result<SubjectRef> subject_of(const Statement& statement) {
  const std::size_t colon = statement.value.find(':');
  if (colon == std::string::npos) {
    return at(statement, Code::MalformedInput, "subject_syntax",
              "a subject is written as kind:id, for example loop:primary");
  }
  const std::string_view kind_text(statement.value.data(), colon);
  const std::string_view id_text(statement.value.data() + colon + 1, statement.value.size() - colon - 1);
  auto kind = parse_subject_kind(kind_text);
  if (!kind) {
    return at(statement, Code::UnknownSubjectKind, "unknown_subject_kind",
              "unknown subject kind '" + std::string(kind_text) + "'");
  }
  auto id = StrongId::parse(id_text);
  if (!id) {
    return at(statement, id.error().code, id.error().reason, id.error().detail);
  }
  return SubjectRef(kind.value(), id.value());
}

}  // namespace

Result<std::vector<IngestRecord>> parse_records(std::string_view text) {
  auto statements = parse_statements(text);
  if (!statements) {
    return statements.error();
  }

  std::vector<IngestRecord> records;
  std::vector<Statement> current;
  std::size_t current_line = 0;

  auto flush = [&]() -> std::optional<Error> {
    if (current.empty()) {
      return std::nullopt;
    }
    RecordBuilder builder(current, current_line);
    const Statement& head = current.front();
    auto record_kind = parse_record_kind(head.value);
    if (!record_kind) {
      return at(head, Code::UnknownToken, "unknown_record_kind",
                "unknown record kind '" + head.value + "'");
    }

    IngestRecord record;
    record.kind = record_kind.value();

    if (const Statement* statement = builder.find("seq")) {
      auto value = parse_u64(*statement);
      if (!value) {
        return value.error();
      }
      record.record_seq = RecordSeq(value.value());
    }
    if (const Statement* statement = builder.find("epoch")) {
      auto value = parse_u64(*statement);
      if (!value) {
        return value.error();
      }
      record.epoch = EpochId(value.value());
    }
    if (const Statement* statement = builder.find("received_at")) {
      auto value = parse_timestamp_statement(*statement);
      if (!value) {
        return value.error();
      }
      record.received_at_ms = value.value();
    }
    if (const Statement* statement = builder.find("generation")) {
      auto value = parse_u64(*statement);
      if (!value) {
        return value.error();
      }
      record.generation = GenerationId(value.value());
    }
    if (const Statement* statement = builder.find("witness")) {
      record.generation_witness = statement->value;
    }

    if (record.kind == RecordKind::AdoptStructure) {
      PlantModel model;
      for (const Statement* statement : builder.all("facility")) {
        auto id = id_of(*statement, "facility identity");
        if (!id) {
          return id.error();
        }
        model.add_facility(FacilityId(id.value()), std::string());
      }
      for (const Statement* statement : builder.all("plant")) {
        const std::vector<std::string_view> parts = split_fields(statement->value);
        if (parts.size() < 3) {
          return at(*statement, Code::MalformedInput, "plant_syntax",
                    "a plant is written as id facility label");
        }
        auto id = StrongId::parse(parts[0]);
        auto facility = StrongId::parse(parts[1]);
        if (!id || !facility) {
          return at(*statement, Code::MalformedInput, "plant_identity", "a plant identity is invalid");
        }
        CoolingPlant plant;
        plant.id = PlantId(id.value());
        plant.facility = FacilityId(facility.value());
        plant.label = std::string(parts[2]);
        model.add_plant(std::move(plant));
      }
      for (const Statement* statement : builder.all("loop")) {
        const std::vector<std::string_view> parts = split_fields(statement->value);
        if (parts.size() < 3) {
          return at(*statement, Code::MalformedInput, "loop_syntax",
                    "a loop is written as id plant primary|secondary [label]");
        }
        auto id = StrongId::parse(parts[0]);
        auto plant = StrongId::parse(parts[1]);
        if (!id || !plant) {
          return at(*statement, Code::MalformedInput, "loop_identity", "a loop identity is invalid");
        }
        if (parts[2] != "primary" && parts[2] != "secondary") {
          return at(*statement, Code::MalformedInput, "loop_class",
                    "a loop is primary or secondary");
        }
        Loop loop;
        loop.id = LoopId(id.value());
        loop.plant = PlantId(plant.value());
        loop.secondary = parts[2] == "secondary";
        if (parts.size() > 3) {
          loop.label = std::string(parts[3]);
        }
        model.add_loop(std::move(loop));
      }
      for (const Statement* statement : builder.all("component")) {
        const std::vector<std::string_view> parts = split_fields(statement->value);
        if (parts.size() < 3) {
          return at(*statement, Code::MalformedInput, "component_syntax",
                    "a component is written as kind id loop [label]");
        }
        auto component_kind = parse_component_kind(parts[0]);
        if (!component_kind) {
          return at(*statement, Code::UnknownToken, "unknown_component_kind",
                    "unknown component kind '" + std::string(parts[0]) + "'");
        }
        auto id = StrongId::parse(parts[1]);
        auto loop = StrongId::parse(parts[2]);
        if (!id || !loop) {
          return at(*statement, Code::MalformedInput, "component_identity",
                    "a component identity is invalid");
        }
        PlantComponent component;
        component.kind = component_kind.value();
        component.id = id.value();
        component.loop = LoopId(loop.value());
        if (parts.size() > 3) {
          component.label = std::string(parts[3]);
        }
        model.add_component(std::move(component));
      }
      for (const Statement* statement : builder.all("zone")) {
        const std::vector<std::string_view> parts = split_fields(statement->value);
        if (parts.size() < 2) {
          return at(*statement, Code::MalformedInput, "zone_syntax",
                    "a zone is written as id facility [label] [load] [max_supply_temp]");
        }
        auto id = StrongId::parse(parts[0]);
        auto facility = StrongId::parse(parts[1]);
        if (!id || !facility) {
          return at(*statement, Code::MalformedInput, "zone_identity", "a zone identity is invalid");
        }
        ThermalZone zone;
        zone.id = ZoneId(id.value());
        zone.facility = FacilityId(facility.value());
        if (parts.size() > 2) {
          zone.label = std::string(parts[2]);
        }
        if (parts.size() > 3 && parts[3] != "-") {
          auto load = parse_quantity(parts[3]);
          if (!load) {
            return at(*statement, load.error().code, load.error().reason, load.error().detail);
          }
          zone.declared_load = Maybe<Quantity>::of(load.value());
        }
        if (parts.size() > 4 && parts[4] != "-") {
          auto bound = parse_quantity(parts[4]);
          if (!bound) {
            return at(*statement, bound.error().code, bound.error().reason, bound.error().detail);
          }
          zone.max_supply_temperature = Maybe<Quantity>::of(bound.value());
        }
        model.add_zone(std::move(zone));
      }
      for (const Statement* statement : builder.all("link")) {
        const std::vector<std::string_view> parts = split_fields(statement->value);
        if (parts.size() != 3) {
          return at(*statement, Code::MalformedInput, "link_syntax",
                    "a link is written as from_subject to_subject relation");
        }
        Statement from = *statement;
        from.value = std::string(parts[0]);
        Statement to = *statement;
        to.value = std::string(parts[1]);
        auto from_subject = subject_of(from);
        auto to_subject = subject_of(to);
        if (!from_subject || !to_subject) {
          return at(*statement, Code::MalformedInput, "link_subject",
                    "a link endpoint is not a valid subject");
        }
        auto relation = parse_link_relation(parts[2]);
        if (!relation) {
          return at(*statement, Code::UnknownToken, "unknown_link_relation",
                    "unknown link relation '" + std::string(parts[2]) + "'");
        }
        PlantLink link;
        link.from = from_subject.value();
        link.to = to_subject.value();
        link.relation = relation.value();
        model.add_link(std::move(link));
      }
      record.structure = std::move(model);
      records.push_back(std::move(record));
      return std::nullopt;
    }

    if (record.kind == RecordKind::RetireEvidence) {
      if (const Statement* statement = builder.find("sensor")) {
        auto id = id_of(*statement, "sensor identity");
        if (!id) {
          return id.error();
        }
        record.retire_sensor = SensorId(id.value());
      }
      if (const Statement* statement = builder.find("axis")) {
        auto axis = parse_evidence_axis(statement->value);
        if (!axis) {
          return at(*statement, Code::UnknownToken, "unknown_axis", "unknown evidence axis");
        }
        record.retire_axis = axis.value();
      }
      if (const Statement* statement = builder.find("subject")) {
        auto subject = subject_of(*statement);
        if (!subject) {
          return subject.error();
        }
        record.retire_subject = Maybe<SubjectRef>::of(subject.value());
      }
      records.push_back(std::move(record));
      return std::nullopt;
    }

    if (record.kind == RecordKind::ParsePolicy) {
      for (const Statement& entry : builder.statements()) {
        const Statement* statement = &entry;
        if (statement->key == "accept_other_epoch") {
          auto value = parse_bool(*statement);
          if (!value) {
            return value.error();
          }
          record.freshness_policy.accept_other_epoch_as_current = value.value();
          continue;
        }
        if (statement->key == "accept_other_generation") {
          auto value = parse_bool(*statement);
          if (!value) {
            return value.error();
          }
          record.freshness_policy.accept_other_generation_as_current = value.value();
          continue;
        }
        if (statement->key != "max_age") {
          continue;
        }
        const std::vector<std::string_view> parts = split_fields(statement->value);
        if (parts.size() != 2) {
          return at(*statement, Code::MalformedInput, "max_age_syntax",
                    "max_age is written as axis milliseconds");
        }
        auto axis = parse_evidence_axis(parts[0]);
        if (!axis) {
          return at(*statement, Code::UnknownToken, "unknown_axis",
                    "unknown evidence axis '" + std::string(parts[0]) + "'");
        }
        auto value = to_i64(parts[1]);
        if (!value) {
          return at(*statement, value.error().code, value.error().reason, value.error().detail);
        }
        switch (axis.value()) {
          case EvidenceAxis::Structure:
            record.freshness_policy.structure_max_age_ms = value.value();
            break;
          case EvidenceAxis::Delivery:
            record.freshness_policy.delivery_max_age_ms = value.value();
            break;
          case EvidenceAxis::Condition:
            record.freshness_policy.condition_max_age_ms = value.value();
            break;
          case EvidenceAxis::Capability:
            record.freshness_policy.capability_max_age_ms = value.value();
            break;
          case EvidenceAxis::Reserve:
            record.freshness_policy.reserve_max_age_ms = value.value();
            break;
          case EvidenceAxis::Fault:
            record.freshness_policy.fault_max_age_ms = value.value();
            break;
        }
      }
      records.push_back(std::move(record));
      return std::nullopt;
    }

    if (record.kind == RecordKind::ThermalPolicyUpdate) {
      if (const Statement* statement = builder.find("coolant_capacity")) {
        auto value = parse_quantity_statement(*statement);
        if (!value) {
          return value.error();
        }
        record.thermal_policy.coolant_heat_capacity_uj_per_l_k = value.value().value;
      }
      if (const Statement* statement = builder.find("air_capacity")) {
        auto value = parse_quantity_statement(*statement);
        if (!value) {
          return value.error();
        }
        record.thermal_policy.air_heat_capacity_uj_per_l_k = value.value().value;
      }
      if (const Statement* statement = builder.find("clamp_negative_removal")) {
        auto value = parse_bool(*statement);
        if (!value) {
          return value.error();
        }
        record.thermal_policy.clamp_negative_removal = value.value();
      }
      records.push_back(std::move(record));
      return std::nullopt;
    }

    if (record.kind == RecordKind::RegisterDeliveryPoint) {
      DeliveryPoint point;
      if (const Statement* statement = builder.find("point")) {
        auto id = id_of(*statement, "delivery point identity");
        if (!id) {
          return id.error();
        }
        point.id = id.value();
      }
      if (const Statement* statement = builder.find("zone")) {
        auto id = id_of(*statement, "zone identity");
        if (!id) {
          return id.error();
        }
        point.zone = ZoneId(id.value());
      }
      if (const Statement* statement = builder.find("loop")) {
        auto id = id_of(*statement, "loop identity");
        if (!id) {
          return id.error();
        }
        point.loop = LoopId(id.value());
      }
      if (const Statement* statement = builder.find("medium")) {
        auto medium = parse_coolant_medium(statement->value);
        if (!medium) {
          return at(*statement, Code::UnknownToken, "unknown_medium", "unknown coolant medium");
        }
        point.medium = medium.value();
      }
      const struct {
        const char* key;
        MeasurementId DeliveryPoint::*member;
      } roles[] = {
          {"flow_measurement", &DeliveryPoint::flow_measurement},
          {"supply_temperature", &DeliveryPoint::supply_temperature},
          {"return_temperature", &DeliveryPoint::return_temperature},
          {"differential_pressure", &DeliveryPoint::differential_pressure},
          {"loop_differential_pressure", &DeliveryPoint::loop_differential_pressure},
          {"airflow", &DeliveryPoint::airflow},
      };
      for (const auto& role : roles) {
        if (const Statement* statement = builder.find(role.key)) {
          auto id = id_of(*statement, "measurement identity");
          if (!id) {
            return id.error();
          }
          point.*(role.member) = MeasurementId(id.value());
        }
      }
      if (const Statement* statement = builder.find("declared_load")) {
        auto value = parse_quantity_statement(*statement);
        if (!value) {
          return value.error();
        }
        point.declared_load = Maybe<Quantity>::of(value.value());
      }
      record.delivery_point = std::move(point);
      records.push_back(std::move(record));
      return std::nullopt;
    }

    if (record.kind == RecordKind::ForgetDeliveryPoint) {
      if (const Statement* statement = builder.find("point")) {
        auto id = id_of(*statement, "delivery point identity");
        if (!id) {
          return id.error();
        }
        record.forget_point = id.value();
      }
      records.push_back(std::move(record));
      return std::nullopt;
    }

    // AddObservation
    Observation& observation = record.observation;
    if (const Statement* statement = builder.find("observation")) {
      auto value = parse_observation_kind(statement->value);
      if (!value) {
        return at(*statement, Code::UnknownToken, "unknown_observation_kind",
                  "unknown observation kind '" + statement->value + "'");
      }
      observation.kind = value.value();
    } else {
      return builder.missing("observation");
    }
    if (const Statement* statement = builder.find("subject")) {
      auto value = subject_of(*statement);
      if (!value) {
        return value.error();
      }
      observation.subject = value.value();
    } else {
      return builder.missing("subject");
    }
    if (const Statement* statement = builder.find("sensor")) {
      auto value = id_of(*statement, "sensor identity");
      if (!value) {
        return value.error();
      }
      observation.sensor = SensorId(value.value());
    }
    if (const Statement* statement = builder.find("observed_at")) {
      auto value = parse_timestamp_statement(*statement);
      if (!value) {
        return value.error();
      }
      observation.observed_at_ms = value.value();
    }
    if (const Statement* statement = builder.find("authority")) {
      const std::vector<std::string_view> parts = split_fields(statement->value);
      auto domain = parse_authority_domain(parts.empty() ? std::string_view{} : parts[0]);
      if (!domain) {
        return at(*statement, Code::UnknownToken, "unknown_authority_domain",
                  "unknown authority domain");
      }
      observation.authority.domain = domain.value();
      if (parts.size() > 1) {
        observation.authority.authority = std::string(parts[1]);
      }
    }
    if (const Statement* statement = builder.find("authority_generation")) {
      auto value = parse_u64(*statement);
      if (!value) {
        return value.error();
      }
      observation.authority.generation = value.value();
    }
    if (const Statement* statement = builder.find("authority_digest")) {
      observation.authority.digest = statement->value;
    }
    if (const Statement* statement = builder.find("origin")) {
      auto value = parse_evidence_origin(statement->value);
      if (!value) {
        return at(*statement, Code::UnknownToken, "unknown_origin", "unknown evidence origin");
      }
      observation.origin = value.value();
    }
    if (const Statement* statement = builder.find("value")) {
      auto value = parse_quantity_statement(*statement);
      if (!value) {
        return value.error();
      }
      observation.measured = value.value();
      observation.unit_text = statement->value;
    }
    if (const Statement* statement = builder.find("quality")) {
      auto value = parse_measurement_quality(statement->value);
      if (!value) {
        return at(*statement, Code::UnknownToken, "unknown_quality",
                  "unknown measurement quality");
      }
      observation.quality = value.value();
    }
    if (const Statement* statement = builder.find("state")) {
      auto value = parse_lifecycle_state(statement->value);
      if (!value) {
        return at(*statement, Code::UnknownToken, "unknown_lifecycle_state",
                  "unknown lifecycle state");
      }
      observation.state = value.value();
    }
    if (const Statement* statement = builder.find("capacity")) {
      auto value = parse_quantity_statement(*statement);
      if (!value) {
        return value.error();
      }
      observation.capability_dimension = value.value().dimension;
      observation.declared_capacity = value.value().value;
    }
    if (const Statement* statement = builder.find("derate")) {
      auto value = parse_percent_as_ppm(statement->value);
      if (!value) {
        return at(*statement, value.error().code, value.error().reason, value.error().detail);
      }
      observation.derate_ppm = value.value();
    }
    if (const Statement* statement = builder.find("depends_on_redundancy")) {
      auto value = parse_bool(*statement);
      if (!value) {
        return value.error();
      }
      observation.depends_on_redundancy = value.value();
    }
    if (const Statement* statement = builder.find("reserve")) {
      auto value = parse_quantity_statement(*statement);
      if (!value) {
        return value.error();
      }
      observation.declared_reserve = value.value();
    }
    for (const Statement* statement : builder.all("assumes")) {
      auto id = id_of(*statement, "assumed element identity");
      if (!id) {
        return id.error();
      }
      observation.assumes_available.push_back(id.value());
    }
    if (const Statement* statement = builder.find("constraint_kind")) {
      auto value = parse_constraint_kind(statement->value);
      if (!value) {
        return at(*statement, Code::UnknownToken, "unknown_constraint_kind",
                  "unknown constraint kind");
      }
      observation.constraint_kind = value.value();
    }
    if (const Statement* statement = builder.find("direction")) {
      auto value = parse_constraint_direction(statement->value);
      if (!value) {
        return at(*statement, Code::UnknownToken, "unknown_direction",
                  "a direction is maximum or minimum");
      }
      observation.direction = value.value();
    }
    if (const Statement* statement = builder.find("constraint_state")) {
      auto value = parse_constraint_state(statement->value);
      if (!value) {
        return at(*statement, Code::UnknownToken, "unknown_constraint_state",
                  "unknown constraint state");
      }
      observation.constraint_state = value.value();
    }
    if (const Statement* statement = builder.find("limit")) {
      auto value = parse_quantity_statement(*statement);
      if (!value) {
        return value.error();
      }
      observation.limit = value.value();
    }
    if (const Statement* statement = builder.find("origin_element")) {
      auto value = subject_of(*statement);
      if (!value) {
        return value.error();
      }
      observation.origin_element = Maybe<SubjectRef>::of(value.value());
    }
    if (const Statement* statement = builder.find("failure_kind")) {
      auto value = parse_failure_kind(statement->value);
      if (!value) {
        return at(*statement, Code::UnknownToken, "unknown_failure_kind", "unknown failure kind");
      }
      observation.failure_kind = value.value();
    }
    if (const Statement* statement = builder.find("severity")) {
      auto value = parse_failure_severity(statement->value);
      if (!value) {
        return at(*statement, Code::UnknownToken, "unknown_severity", "unknown failure severity");
      }
      observation.severity = value.value();
    }
    if (const Statement* statement = builder.find("impact")) {
      auto value = parse_failure_impact(statement->value);
      if (!value) {
        return at(*statement, Code::UnknownToken, "unknown_impact", "unknown failure impact");
      }
      observation.impact = value.value();
    }
    if (const Statement* statement = builder.find("residual")) {
      auto value = parse_quantity_statement(*statement);
      if (!value) {
        return value.error();
      }
      observation.residual_delivery = Maybe<Quantity>::of(value.value());
    }
    if (const Statement* statement = builder.find("detail")) {
      observation.detail = statement->value;
    }
    observation.record_seq = record.record_seq;
    observation.epoch = record.epoch;
    observation.received_at_ms = record.received_at_ms != 0 ? record.received_at_ms : observation.observed_at_ms;
    records.push_back(std::move(record));
    return std::nullopt;
  };

  for (const Statement& statement : statements.value()) {
    if (statement.key == "record") {
      if (const std::optional<Error> error = flush(); error.has_value()) {
        return error.value();
      }
      current.clear();
      current_line = statement.line;
      current.push_back(statement);
      continue;
    }
    if (current.empty()) {
      return at(statement, Code::MalformedInput, "statement_outside_record",
                "every statement must belong to a record that starts with 'record <kind>'");
    }
    current.push_back(statement);
  }
  if (const std::optional<Error> error = flush(); error.has_value()) {
    return error.value();
  }
  return records;
}

Result<ObserveRequest> parse_request(std::string_view text) {
  auto statements = parse_statements(text);
  if (!statements) {
    return statements.error();
  }
  ObserveRequest request;
  bool have_kind = false;
  for (const Statement& statement : statements.value()) {
    if (statement.key == "query") {
      auto kind = parse_query_kind(statement.value);
      if (!kind) {
        return at(statement, Code::UnknownToken, "unknown_query_kind",
                  "unknown query kind '" + statement.value + "'");
      }
      request.kind = kind.value();
      have_kind = true;
      continue;
    }
    if (statement.key == "point") {
      auto id = StrongId::parse(statement.value);
      if (!id) {
        return at(statement, id.error().code, id.error().reason, id.error().detail);
      }
      request.filter.point = Maybe<StrongId>::of(id.value());
      continue;
    }
    if (statement.key == "loop") {
      auto id = StrongId::parse(statement.value);
      if (!id) {
        return at(statement, id.error().code, id.error().reason, id.error().detail);
      }
      request.filter.loop = Maybe<LoopId>::of(LoopId(id.value()));
      continue;
    }
    if (statement.key == "plant") {
      auto id = StrongId::parse(statement.value);
      if (!id) {
        return at(statement, id.error().code, id.error().reason, id.error().detail);
      }
      request.filter.plant = Maybe<PlantId>::of(PlantId(id.value()));
      continue;
    }
    if (statement.key == "zone") {
      auto id = StrongId::parse(statement.value);
      if (!id) {
        return at(statement, id.error().code, id.error().reason, id.error().detail);
      }
      request.filter.zone = Maybe<ZoneId>::of(ZoneId(id.value()));
      continue;
    }
    if (statement.key == "subject" || statement.key == "root") {
      auto subject = subject_of(statement);
      if (!subject) {
        return subject.error();
      }
      if (statement.key == "root") {
        request.filter.root = Maybe<SubjectRef>::of(subject.value());
      } else {
        request.filter.subject = Maybe<SubjectRef>::of(subject.value());
      }
      continue;
    }
    if (statement.key == "direction") {
      if (statement.value == "upstream") {
        request.filter.traversal.direction = TraversalOptions::Direction::Upstream;
      } else if (statement.value == "downstream") {
        request.filter.traversal.direction = TraversalOptions::Direction::Downstream;
      } else {
        return at(statement, Code::MalformedInput, "traversal_direction",
                  "a traversal direction is upstream or downstream");
      }
      continue;
    }
    if (statement.key == "all_relations") {
      auto value = parse_bool(statement);
      if (!value) {
        return value.error();
      }
      request.filter.traversal.delivery_only = !value.value();
      continue;
    }
    if (statement.key == "max_depth") {
      auto value = parse_u64(statement);
      if (!value) {
        return value.error();
      }
      request.filter.traversal.max_depth = static_cast<std::uint32_t>(value.value());
      continue;
    }
    if (statement.key == "include_stale") {
      auto value = parse_bool(statement);
      if (!value) {
        return value.error();
      }
      request.filter.include_stale = value.value();
      continue;
    }
    if (statement.key == "include_consistent") {
      auto value = parse_bool(statement);
      if (!value) {
        return value.error();
      }
      request.filter.include_consistent = value.value();
      continue;
    }
    if (statement.key == "include_covered") {
      auto value = parse_bool(statement);
      if (!value) {
        return value.error();
      }
      request.filter.include_covered = value.value();
      continue;
    }
    if (statement.key == "require_evidenced") {
      auto value = parse_bool(statement);
      if (!value) {
        return value.error();
      }
      request.filter.require_evidenced = value.value();
      continue;
    }
    if (statement.key == "minimum_severity") {
      auto value = parse_failure_severity(statement.value);
      if (!value) {
        return at(statement, Code::UnknownToken, "unknown_severity", "unknown failure severity");
      }
      request.filter.minimum_severity = Maybe<FailureSeverity>::of(value.value());
      continue;
    }
    if (statement.key == "tolerance") {
      auto value = parse_i64(statement);
      if (!value) {
        return value.error();
      }
      request.filter.relative_tolerance_ppm = value.value();
      continue;
    }
    if (statement.key == "limit") {
      auto value = parse_u64(statement);
      if (!value) {
        return value.error();
      }
      request.filter.limit = static_cast<std::size_t>(value.value());
      continue;
    }
    return at(statement, Code::UnknownToken, "unknown_request_field",
              "unknown request field '" + statement.key + "'");
  }
  if (!have_kind) {
    return fail(Code::MalformedInput, "query_kind_missing",
                "a request must start with 'query <kind>'");
  }
  return request;
}

}  // namespace dccp::cooling_observatory