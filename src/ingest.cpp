// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/ingest.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace dccp::cooling_observatory {
namespace {

constexpr std::array<std::pair<RecordKind, std::string_view>, 7> kRecordKinds{{
    {RecordKind::AdoptStructure, "adopt_structure"},
    {RecordKind::AddObservation, "add_observation"},
    {RecordKind::RetireEvidence, "retire_evidence"},
    {RecordKind::ParsePolicy, "parse_policy"},
    {RecordKind::ThermalPolicyUpdate, "thermal_policy_update"},
    {RecordKind::RegisterDeliveryPoint, "register_delivery_point"},
    {RecordKind::ForgetDeliveryPoint, "forget_delivery_point"},
}};

}  // namespace

std::string_view to_token(RecordKind kind) noexcept {
  for (const auto& entry : kRecordKinds) {
    if (entry.first == kind) {
      return entry.second;
    }
  }
  return "unknown";
}

std::optional<RecordKind> parse_record_kind(std::string_view token) noexcept {
  for (const auto& entry : kRecordKinds) {
    if (entry.second == token) {
      return entry.first;
    }
  }
  return std::nullopt;
}

bool operator==(const IngestRecord& a, const IngestRecord& b) noexcept {
  if (a.kind != b.kind || a.record_seq != b.record_seq || a.epoch != b.epoch ||
      a.received_at_ms != b.received_at_ms) {
    return false;
  }
  switch (a.kind) {
    case RecordKind::AdoptStructure:
      return a.generation == b.generation && a.generation_witness == b.generation_witness &&
             a.structure == b.structure;
    case RecordKind::AddObservation: {
      const Observation& x = a.observation;
      const Observation& y = b.observation;
      return x.kind == y.kind && x.subject == y.subject && x.sensor == y.sensor &&
             x.authority == y.authority && x.epoch == y.epoch && x.observed_at_ms == y.observed_at_ms &&
             x.received_at_ms == y.received_at_ms && x.origin == y.origin && x.record_seq == y.record_seq &&
             x.generation == y.generation && x.measured == y.measured && x.quality == y.quality &&
             x.unit_text == y.unit_text && x.state == y.state &&
             x.capability_dimension == y.capability_dimension &&
             x.declared_capacity == y.declared_capacity && x.derate_ppm == y.derate_ppm &&
             x.depends_on_redundancy == y.depends_on_redundancy &&
             x.declared_reserve == y.declared_reserve && x.assumes_available == y.assumes_available &&
             x.constraint_kind == y.constraint_kind && x.direction == y.direction &&
             x.constraint_state == y.constraint_state && x.limit == y.limit &&
             x.origin_element == y.origin_element && x.failure_kind == y.failure_kind &&
             x.severity == y.severity && x.impact == y.impact &&
             x.residual_delivery == y.residual_delivery && x.detail == y.detail;
    }
    case RecordKind::RetireEvidence:
      return a.retire_sensor == b.retire_sensor && a.retire_subject == b.retire_subject &&
             a.retire_axis == b.retire_axis;
    case RecordKind::ParsePolicy:
      return a.freshness_policy == b.freshness_policy;
    case RecordKind::ThermalPolicyUpdate:
      return a.thermal_policy == b.thermal_policy;
    case RecordKind::RegisterDeliveryPoint:
      return a.delivery_point == b.delivery_point;
    case RecordKind::ForgetDeliveryPoint:
      return a.forget_point == b.forget_point;
  }
  return false;
}

RecordSeq key_of(const Observation& observation) noexcept { return observation.record_seq; }

Status validate_record(const IngestRecord& record, const Limits& limits,
                       std::size_t current_evidence_count) {
  switch (record.kind) {
    case RecordKind::AddObservation: {
      const Observation& observation = record.observation;
      if (observation.record_seq.empty()) {
        return Status::failure(Code::InvalidArgument, "record_seq_required",
                               "an observation must carry a producer record sequence: without one, a "
                               "retried record cannot be told apart from a new one");
      }
      if (observation.subject.id.empty()) {
        return Status::failure(Code::InvalidArgument, "observation_subject_required",
                               "an observation must name the subject it is about");
      }
      if (observation.kind == ObservationKind::Measurement &&
          observation.quality == MeasurementQuality::Good && observation.measured.dimension == Dimension::None) {
        return Status::failure(Code::UnsupportedValue, "measurement_without_dimension",
                               "a measurement reported as good must carry a dimension");
      }
      if (observation.kind == ObservationKind::Capability && observation.derate_ppm < 0) {
        return Status::failure(Code::InvalidArgument, "derate_negative",
                               "a derate is a reduction; a negative derate is an increase and is refused");
      }
      if (observation.kind == ObservationKind::Capability && observation.derate_ppm > 1000000) {
        return Status::failure(Code::UnsupportedValue, "derate_above_full",
                               "a derate above 1000000ppm would declare negative capability");
      }
      if (current_evidence_count >= limits.max_measurements) {
        return Status::failure(limit_error("max_measurements", limits.max_measurements,
                                           current_evidence_count + 1));
      }
      return Status::success();
    }
    case RecordKind::AdoptStructure: {
      if (record.generation.empty()) {
        return Status::failure(Code::InvalidArgument, "generation_required",
                               "adopting a structure requires the generation it came from");
      }
      return record.structure.validate(limits);
    }
    case RecordKind::RetireEvidence: {
      if (record.retire_sensor.empty()) {
        return Status::failure(Code::InvalidArgument, "retire_sensor_required",
                               "retiring evidence requires the sensor whose evidence is withdrawn");
      }
      return Status::success();
    }
    case RecordKind::ParsePolicy:
      return validate(record.freshness_policy);
    case RecordKind::ThermalPolicyUpdate:
      return validate(record.thermal_policy);
    case RecordKind::RegisterDeliveryPoint: {
      if (record.delivery_point.id.empty()) {
        return Status::failure(Code::InvalidArgument, "delivery_point_id_required",
                               "a delivery point must have an identity");
      }
      return Status::success();
    }
    case RecordKind::ForgetDeliveryPoint: {
      if (record.forget_point.empty()) {
        return Status::failure(Code::InvalidArgument, "forget_point_id_required",
                               "forgetting a delivery point requires its identity");
      }
      return Status::success();
    }
  }
  return Status::failure(Code::InternalError, "unhandled_record_kind",
                         "the record kind is not handled by validation");
}

void canonicalise(IngestRecord& record) {
  if (record.kind == RecordKind::AdoptStructure) {
    record.structure.canonicalise();
  }
  if (record.kind == RecordKind::AddObservation) {
    std::sort(record.observation.assumes_available.begin(), record.observation.assumes_available.end());
    record.observation.assumes_available.erase(
        std::unique(record.observation.assumes_available.begin(), record.observation.assumes_available.end()),
        record.observation.assumes_available.end());
  }
}

}  // namespace dccp::cooling_observatory
