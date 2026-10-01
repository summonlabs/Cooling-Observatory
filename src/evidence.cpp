// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/evidence.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace dccp::cooling_observatory {
namespace {

constexpr std::array<std::pair<ObservationKind, std::string_view>, 6> kObservationKinds{{
    {ObservationKind::Measurement, "measurement"},
    {ObservationKind::EquipmentState, "equipment_state"},
    {ObservationKind::Capability, "capability"},
    {ObservationKind::ReserveClaim, "reserve_claim"},
    {ObservationKind::Constraint, "constraint"},
    {ObservationKind::Failure, "failure"},
}};

constexpr std::array<std::pair<MeasurementQuality, std::string_view>, 4> kMeasurementQualities{{
    {MeasurementQuality::Good, "good"},
    {MeasurementQuality::Uncertain, "uncertain"},
    {MeasurementQuality::Bad, "bad"},
    {MeasurementQuality::NotAvailable, "not_available"},
}};

constexpr std::array<std::pair<ConstraintKind, std::string_view>, 8> kConstraintKinds{{
    {ConstraintKind::FlowLimit, "flow_limit"},
    {ConstraintKind::PressureLimit, "pressure_limit"},
    {ConstraintKind::DifferentialPressure, "differential_pressure"},
    {ConstraintKind::AirflowLimit, "airflow_limit"},
    {ConstraintKind::HeatRemovalLimit, "heat_removal_limit"},
    {ConstraintKind::SupplyTemperature, "supply_temperature"},
    {ConstraintKind::ValvePosition, "valve_position"},
    {ConstraintKind::PumpSpeed, "pump_speed"},
}};

constexpr std::array<std::pair<ConstraintDirection, std::string_view>, 2> kConstraintDirections{{
    {ConstraintDirection::Maximum, "maximum"},
    {ConstraintDirection::Minimum, "minimum"},
}};

constexpr std::array<std::pair<ConstraintState, std::string_view>, 4> kConstraintStates{{
    {ConstraintState::Active, "active"},
    {ConstraintState::Inactive, "inactive"},
    {ConstraintState::Unknown, "unknown"},
    {ConstraintState::Resolved, "resolved"},
}};

constexpr std::array<std::pair<FailureKind, std::string_view>, 15> kFailureKinds{{
    {FailureKind::PumpStopped, "pump_stopped"},
    {FailureKind::PumpSpeedLost, "pump_speed_lost"},
    {FailureKind::ValveStuck, "valve_stuck"},
    {FailureKind::ValveLeak, "valve_leak"},
    {FailureKind::ChillerTrip, "chiller_trip"},
    {FailureKind::ChillerDerate, "chiller_derate"},
    {FailureKind::CduFault, "cdu_fault"},
    {FailureKind::CrahFanFailure, "crah_fan_failure"},
    {FailureKind::CrahCoilFouling, "crah_coil_fouling"},
    {FailureKind::SensorLoss, "sensor_loss"},
    {FailureKind::CoolantLeak, "coolant_leak"},
    {FailureKind::AirflowBlockage, "airflow_blockage"},
    {FailureKind::FilterLoading, "filter_loading"},
    {FailureKind::InstrumentDrift, "instrument_drift"},
    {FailureKind::Unknown, "unknown"},
}};

constexpr std::array<std::pair<FailureSeverity, std::string_view>, 4> kFailureSeverities{{
    {FailureSeverity::Advisory, "advisory"},
    {FailureSeverity::Minor, "minor"},
    {FailureSeverity::Major, "major"},
    {FailureSeverity::Critical, "critical"},
}};

constexpr std::array<std::pair<FailureImpact, std::string_view>, 4> kFailureImpacts{{
    {FailureImpact::NoDelivery, "no_delivery"},
    {FailureImpact::ReducedDelivery, "reduced_delivery"},
    {FailureImpact::ResidualDelivery, "residual_delivery"},
    {FailureImpact::UnknownImpact, "unknown_impact"},
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

}  // namespace

std::string_view to_token(ObservationKind kind) noexcept { return token_of(kObservationKinds, kind); }
std::optional<ObservationKind> parse_observation_kind(std::string_view token) noexcept {
  return parse_of(kObservationKinds, token);
}

std::string_view to_token(MeasurementQuality quality) noexcept { return token_of(kMeasurementQualities, quality); }
std::optional<MeasurementQuality> parse_measurement_quality(std::string_view token) noexcept {
  return parse_of(kMeasurementQualities, token);
}

std::string_view to_token(ConstraintKind kind) noexcept { return token_of(kConstraintKinds, kind); }
std::optional<ConstraintKind> parse_constraint_kind(std::string_view token) noexcept {
  return parse_of(kConstraintKinds, token);
}

std::optional<Dimension> dimension_of(ConstraintKind kind) noexcept {
  switch (kind) {
    case ConstraintKind::FlowLimit:
    case ConstraintKind::AirflowLimit:
      return Dimension::Flow;
    case ConstraintKind::PressureLimit:
    case ConstraintKind::DifferentialPressure:
      return Dimension::Pressure;
    case ConstraintKind::HeatRemovalLimit:
      return Dimension::Power;
    case ConstraintKind::SupplyTemperature:
      return Dimension::Temperature;
    case ConstraintKind::ValvePosition:
      return Dimension::Ratio;
    case ConstraintKind::PumpSpeed:
      return Dimension::Frequency;
  }
  return std::nullopt;
}

std::string_view to_token(ConstraintDirection direction) noexcept { return token_of(kConstraintDirections, direction); }
std::optional<ConstraintDirection> parse_constraint_direction(std::string_view token) noexcept {
  return parse_of(kConstraintDirections, token);
}

std::string_view to_token(ConstraintState state) noexcept { return token_of(kConstraintStates, state); }
std::optional<ConstraintState> parse_constraint_state(std::string_view token) noexcept {
  return parse_of(kConstraintStates, token);
}

std::string_view to_token(FailureKind kind) noexcept { return token_of(kFailureKinds, kind); }
std::optional<FailureKind> parse_failure_kind(std::string_view token) noexcept {
  return parse_of(kFailureKinds, token);
}

std::string_view to_token(FailureSeverity severity) noexcept { return token_of(kFailureSeverities, severity); }
std::optional<FailureSeverity> parse_failure_severity(std::string_view token) noexcept {
  return parse_of(kFailureSeverities, token);
}

std::string_view to_token(FailureImpact impact) noexcept { return token_of(kFailureImpacts, impact); }
std::optional<FailureImpact> parse_failure_impact(std::string_view token) noexcept {
  return parse_of(kFailureImpacts, token);
}

// ---------------------------------------------------------------------------
// Freshness policy
// ---------------------------------------------------------------------------

DurationMs FreshnessPolicy::max_age_for(EvidenceAxis axis) const noexcept {
  switch (axis) {
    case EvidenceAxis::Structure:
      return structure_max_age_ms;
    case EvidenceAxis::Delivery:
      return delivery_max_age_ms;
    case EvidenceAxis::Condition:
      return condition_max_age_ms;
    case EvidenceAxis::Capability:
      return capability_max_age_ms;
    case EvidenceAxis::Reserve:
      return reserve_max_age_ms;
    case EvidenceAxis::Fault:
      return fault_max_age_ms;
  }
  return 0;
}

const FreshnessPolicy& default_freshness_policy() noexcept {
  static const FreshnessPolicy policy;
  return policy;
}

Status validate(const FreshnessPolicy& policy) {
  const DurationMs ages[] = {policy.structure_max_age_ms, policy.delivery_max_age_ms,
                             policy.condition_max_age_ms, policy.capability_max_age_ms,
                             policy.reserve_max_age_ms,   policy.fault_max_age_ms};
  for (const DurationMs age : ages) {
    if (age < 0) {
      return Status::failure(Code::InvalidArgument, "freshness_age_negative",
                             "an evidence age bound must not be negative");
    }
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

Freshness classify(const Observation& observation, const FreshnessPolicy& policy, TimestampMs now_ms,
                   EpochId current_epoch, GenerationId adopted_generation) {
  // Recovered evidence is never current, whatever else is true of it. A value
  // restored from durable state after a restart describes the world as it was
  // before the restart; only new evidence describes the world now.
  if (observation.origin == EvidenceOrigin::Recovered) {
    return Freshness::Recovered;
  }

  const DurationMs bound = policy.max_age_for(axis_of(observation.kind));
  const DurationMs age = elapsed_ms(now_ms, observation.observed_at_ms);
  if (age > bound || age < 0) {
    // A record stamped in the future is as unusable as an old one: either the
    // producer's clock or this host's clock is wrong, and neither can be
    // trusted to bound the other. Both are reported as stale rather than
    // silently accepted.
    return Freshness::Stale;
  }

  if (observation.kind == ObservationKind::Measurement) {
    if (!is_usable(observation.quality)) {
      // A sensor that reports its own value as bad or unavailable is reporting
      // silence. Silence is unknown, never zero.
      return Freshness::Unknown;
    }
  }

  if (!policy.accept_other_epoch_as_current && observation.epoch != current_epoch) {
    return Freshness::Stale;
  }

  if (!policy.accept_other_generation_as_current && observation.kind != ObservationKind::EquipmentState &&
      observation.kind != ObservationKind::Measurement && observation.generation != adopted_generation) {
    // Capability, reserve and fault declarations are stated against a structure.
    // One stated against a structure this runtime no longer holds describes a
    // plant that may no longer exist.
    return Freshness::Stale;
  }

  return Freshness::Fresh;
}

EvidenceRef make_reference(const Observation& observation, Revision revision, std::uint64_t ordinal) {
  EvidenceRef reference;
  // An observation carries the commit point the engine gave it. A caller that
  // does not state one gets that, so a reference can never silently claim
  // revision zero for evidence that has a real one.
  reference.revision = revision.empty() ? observation.committed_revision : revision;
  reference.ordinal = ordinal == 0 ? observation.committed_ordinal : ordinal;
  reference.record_seq = observation.record_seq;
  reference.epoch = observation.epoch;
  reference.generation = observation.generation;
  reference.kind = observation.kind;
  reference.authority = observation.authority;
  reference.subject = observation.subject;
  reference.ordinal = ordinal;
  return reference;
}

// ---------------------------------------------------------------------------
// Reduction
// ---------------------------------------------------------------------------

namespace {

struct Candidate {
  const Observation* observation = nullptr;
  Freshness freshness = Freshness::Unknown;
};

/// Canonical ordering of measurements that agree: by sensor identity, then by
/// the committed order they arrived in. Two identical records are collapsed by
/// the caller, so this order is total.
bool candidate_less(const Candidate& a, const Candidate& b) noexcept {
  if (a.observation->sensor != b.observation->sensor) {
    return a.observation->sensor < b.observation->sensor;
  }
  return a.observation->record_seq < b.observation->record_seq;
}

}  // namespace

Reduction Reducer::reduce(const std::vector<Observation>& observations, const SubjectRef& subject) const {
  std::vector<Observation> group;
  group.reserve(observations.size());
  for (const Observation& observation : observations) {
    if (observation.subject == subject) {
      group.push_back(observation);
    }
  }
  return reduce_group(group);
}

Reduction Reducer::reduce_group(const std::vector<Observation>& observations) const {
  Reduction out;
  if (observations.empty()) {
    return out;
  }

  const FreshnessPolicy& effective_policy = policy != nullptr ? *policy : default_freshness_policy();

  std::vector<Candidate> candidates;
  candidates.reserve(observations.size());
  for (const Observation& observation : observations) {
    Candidate candidate;
    candidate.observation = &observation;
    candidate.freshness = classify(observation, effective_policy, now_ms, current_epoch, adopted_generation);
    candidates.push_back(candidate);
  }

  // When at least one source is current, the answer is described by those
  // sources: an old or restored observation of the same quantity is superseded by
  // a current one and must not make a fresh reading look uncertain. When nothing
  // is current, the worst of what was considered is exactly the right answer.
  bool any_current = false;
  for (const Candidate& candidate : candidates) {
    if (is_current(candidate.freshness)) {
      any_current = true;
      break;
    }
  }
  out.freshness = Freshness::Fresh;
  for (const Candidate& candidate : candidates) {
    if (any_current && !is_current(candidate.freshness)) {
      continue;
    }
    out.freshness = worst(out.freshness, candidate.freshness);
  }

  std::vector<Candidate> current;
  for (const Candidate& candidate : candidates) {
    if (is_current(candidate.freshness)) {
      current.push_back(candidate);
    }
  }
  std::sort(current.begin(), current.end(), candidate_less);

  for (const Candidate& candidate : current) {
    out.evidence.push_back(make_reference(*candidate.observation));
  }

  if (current.empty()) {
    // Nothing current. The freshness stays at the worst observed, so a caller
    // can distinguish "the sensor is silent" from "the sensor reported an hour
    // ago" from "the value was restored after a restart".
    return out;
  }

  // Are all current values in agreement?
  const Quantity reference_value = current.front().observation->measured;
  const Quantity tolerance_quantity(reference_value.dimension, tolerance < 0 ? 0 : tolerance);
  bool conflicted = false;
  for (const Candidate& candidate : current) {
    const std::optional<bool> agrees = within_tolerance(candidate.observation->measured, reference_value,
                                                        tolerance_quantity);
    if (!agrees.has_value() || !agrees.value()) {
      conflicted = true;
      break;
    }
  }

  if (!conflicted) {
    out.value = Maybe<Quantity>::of(reference_value);
    out.freshness = Freshness::Fresh;
    return out;
  }

  out.freshness = Freshness::Conflicting;
  for (const Candidate& candidate : current) {
    if (std::find(out.conflicting_sensors.begin(), out.conflicting_sensors.end(),
                  candidate.observation->sensor) == out.conflicting_sensors.end()) {
      out.conflicting_sensors.push_back(candidate.observation->sensor);
    }
    if (std::find(out.conflicting_values.begin(), out.conflicting_values.end(),
                  candidate.observation->measured) == out.conflicting_values.end()) {
      out.conflicting_values.push_back(candidate.observation->measured);
    }
  }
  std::sort(out.conflicting_sensors.begin(), out.conflicting_sensors.end());
  std::sort(out.conflicting_values.begin(), out.conflicting_values.end(),
            [](const Quantity& a, const Quantity& b) {
              if (a.dimension != b.dimension) {
                return static_cast<std::uint8_t>(a.dimension) < static_cast<std::uint8_t>(b.dimension);
              }
              return a.value < b.value;
            });
  return out;
}

}  // namespace dccp::cooling_observatory