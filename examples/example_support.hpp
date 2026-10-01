// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Shared scaffolding for the examples. Every example builds its facility from
// this one synthetic description, so the examples differ in what they show, not
// in what they are about.

#ifndef COOLING_OBSERVATORY_EXAMPLE_SUPPORT_HPP
#define COOLING_OBSERVATORY_EXAMPLE_SUPPORT_HPP

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "dccp/cooling_observatory/engine.hpp"
#include "dccp/cooling_observatory/textproto.hpp"

namespace example {

using namespace dccp::cooling_observatory;

/// A synthetic facility: one plant, a primary and a secondary loop, two pumps on
/// the primary loop, a chiller, a CDU and two CRAH units feeding two zones.
///
/// Nothing here is real hardware, real telemetry or a real BMS. The structure
/// and the numbers are constructed so that each example can make its point
/// without a facility.
inline PlantModel synthetic_plant() {
  PlantModel model;
  model.add_facility(FacilityId(StrongId::from_validated("dc1")), "dc1");
  CoolingPlant plant;
  plant.id = PlantId(StrongId::from_validated("plant.a"));
  plant.facility = FacilityId(StrongId::from_validated("dc1"));
  plant.label = "synthetic_plant_a";
  model.add_plant(plant);

  Loop primary;
  primary.id = LoopId(StrongId::from_validated("loop.primary"));
  primary.plant = plant.id;
  primary.secondary = false;
  primary.label = "primary";
  model.add_loop(primary);

  Loop secondary;
  secondary.id = LoopId(StrongId::from_validated("loop.secondary"));
  secondary.plant = plant.id;
  secondary.secondary = true;
  secondary.label = "secondary";
  model.add_loop(secondary);

  auto component = [&model](ComponentKind kind, const char* id, const char* loop, const char* label) {
    PlantComponent entry;
    entry.kind = kind;
    entry.id = StrongId::from_validated(id);
    entry.loop = LoopId(StrongId::from_validated(loop));
    entry.label = label;
    model.add_component(entry);
  };
  component(ComponentKind::Pump, "pump.p1", "loop.primary", "primary_pump_1");
  component(ComponentKind::Pump, "pump.p2", "loop.primary", "primary_pump_2");
  component(ComponentKind::Chiller, "chiller.c1", "loop.primary", "chiller_1");
  component(ComponentKind::Cdu, "cdu.d1", "loop.secondary", "cdu_1");
  component(ComponentKind::Crah, "crah.h1", "loop.secondary", "crah_1");
  component(ComponentKind::Crah, "crah.h2", "loop.secondary", "crah_2");

  auto zone = [&model](const char* id, const char* label, std::int64_t load_watts) {
    ThermalZone entry;
    entry.id = ZoneId(StrongId::from_validated(id));
    entry.facility = FacilityId(StrongId::from_validated("dc1"));
    entry.label = label;
    entry.declared_load = Maybe<Quantity>::of(Quantity::power(load_watts));
    model.add_zone(entry);
  };
  zone("zone.a", "hall_a", 180'000);
  zone("zone.b", "hall_b", 120'000);

  auto link = [&model](SubjectKind from_kind, const char* from, SubjectKind to_kind, const char* to,
                       LinkRelation relation) {
    PlantLink entry;
    entry.from = SubjectRef(from_kind, StrongId::from_validated(from));
    entry.to = SubjectRef(to_kind, StrongId::from_validated(to));
    entry.relation = relation;
    model.add_link(entry);
  };
  link(SubjectKind::Plant, "plant.a", SubjectKind::Loop, "loop.primary", LinkRelation::Supply);
  link(SubjectKind::Loop, "loop.primary", SubjectKind::Loop, "loop.secondary", LinkRelation::Supply);
  link(SubjectKind::Loop, "loop.primary", SubjectKind::Chiller, "chiller.c1", LinkRelation::Supply);
  link(SubjectKind::Loop, "loop.primary", SubjectKind::Pump, "pump.p1", LinkRelation::Supply);
  link(SubjectKind::Loop, "loop.primary", SubjectKind::Pump, "pump.p2", LinkRelation::Supply);
  link(SubjectKind::Loop, "loop.secondary", SubjectKind::Cdu, "cdu.d1", LinkRelation::Supply);
  link(SubjectKind::Cdu, "cdu.d1", SubjectKind::Crah, "crah.h1", LinkRelation::Supply);
  link(SubjectKind::Cdu, "cdu.d1", SubjectKind::Crah, "crah.h2", LinkRelation::Supply);
  link(SubjectKind::Crah, "crah.h1", SubjectKind::Zone, "zone.a", LinkRelation::Supply);
  link(SubjectKind::Crah, "crah.h2", SubjectKind::Zone, "zone.b", LinkRelation::Supply);

  model.reindex();
  return model;
}

/// A clock that starts at a fixed instant, so every example is reproducible.
class ExampleClock final : public Clock {
 public:
  explicit ExampleClock(TimestampMs start) : instant_(start) {}
  [[nodiscard]] TimestampMs now_ms() const override { return instant_; }
  void advance(DurationMs delta) { instant_ += delta; }

 private:
  TimestampMs instant_ = 0;
};

inline void report(const Status& status, const char* what) {
  if (!status.ok()) {
    std::cerr << "example: " << what << " failed: " << to_token(status.code()) << ": "
              << status.reason() << ": " << status.detail() << "\n";
  }
}

template <typename T>
inline void report(const Result<T>& result, const char* what) {
  if (!result) {
    std::cerr << "example: " << what << " failed: " << to_token(result.error().code) << ": "
              << result.error().reason << ": " << result.error().detail << "\n";
  }
}

/// A measurement record for one zone role, stamped with the engine epoch.
inline IngestRecord zone_measurement(std::uint64_t sequence, EpochId epoch, const char* zone_id,
                                     MeasurementId measurement, const char* sensor, Quantity value,
                                     TimestampMs observed_at_ms) {
  return make_zone_measurement(RecordSeq(sequence), epoch, ZoneId(StrongId::from_validated(zone_id)),
                               measurement, sensor, value, observed_at_ms);
}

/// A capability declaration for one element.
inline IngestRecord capability(std::uint64_t sequence, EpochId epoch, SubjectKind kind,
                               const char* element, Quantity capacity, std::int64_t derate_ppm,
                               TimestampMs observed_at_ms) {
  IngestRecord record;
  record.kind = RecordKind::AddObservation;
  record.record_seq = RecordSeq(sequence);
  record.epoch = epoch;
  record.received_at_ms = observed_at_ms;
  record.observation.kind = ObservationKind::Capability;
  record.observation.subject = SubjectRef(kind, StrongId::from_validated(element));
  record.observation.capability_dimension = capacity.dimension;
  record.observation.declared_capacity = capacity.value;
  record.observation.derate_ppm = derate_ppm;
  record.observation.observed_at_ms = observed_at_ms;
  record.observation.received_at_ms = observed_at_ms;
  record.observation.record_seq = RecordSeq(sequence);
  record.observation.epoch = epoch;
  record.observation.authority.domain = AuthorityDomain::CoolingCapacity;
  record.observation.authority.authority = "dccp-cooling-capacity-accounting/1.0.0";
  return record;
}

/// A reported lifecycle state for one element. This is an observation of what
/// the control plane reports, not a command and not an acknowledgement.
inline IngestRecord equipment_state(std::uint64_t sequence, EpochId epoch, SubjectKind kind,
                                    const char* element, LifecycleState state,
                                    TimestampMs observed_at_ms) {
  IngestRecord record;
  record.kind = RecordKind::AddObservation;
  record.record_seq = RecordSeq(sequence);
  record.epoch = epoch;
  record.received_at_ms = observed_at_ms;
  record.observation.kind = ObservationKind::EquipmentState;
  record.observation.subject = SubjectRef(kind, StrongId::from_validated(element));
  record.observation.state = state;
  record.observation.observed_at_ms = observed_at_ms;
  record.observation.received_at_ms = observed_at_ms;
  record.observation.record_seq = RecordSeq(sequence);
  record.observation.epoch = epoch;
  record.observation.authority.domain = AuthorityDomain::CoolingControl;
  record.observation.authority.authority = "dccp-cooling-control/1.0.0";
  return record;
}

}  // namespace example

#endif  // COOLING_OBSERVATORY_EXAMPLE_SUPPORT_HPP
