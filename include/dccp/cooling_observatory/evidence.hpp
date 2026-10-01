// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_EVIDENCE_HPP
#define DCCP_COOLING_OBSERVATORY_EVIDENCE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/cooling_observatory/clock.hpp"
#include "dccp/cooling_observatory/identity.hpp"
#include "dccp/cooling_observatory/limits.hpp"
#include "dccp/cooling_observatory/semantics.hpp"
#include "dccp/cooling_observatory/units.hpp"

namespace dccp::cooling_observatory {

/// What a record asserts. The kind selects which fields of Observation carry
/// meaning; the remaining fields must be empty, and the reader refuses a record
/// whose unused fields are populated rather than quietly ignoring them.
enum class ObservationKind : std::uint8_t {
  Measurement = 0,     ///< a measured quantity at a subject
  EquipmentState = 1,  ///< a reported lifecycle state of a piece of equipment
  Capability = 2,      ///< a declared delivery capability and its derates
  ReserveClaim = 3,    ///< a declared reserve with the conditions it assumes
  Constraint = 4,      ///< a declared or measured constraint on delivery
  Failure = 5,         ///< a declared failure
};

[[nodiscard]] std::string_view to_token(ObservationKind kind) noexcept;
[[nodiscard]] std::optional<ObservationKind> parse_observation_kind(std::string_view token) noexcept;

/// The evidence axis a kind belongs to. The kind determines the axis; a record
/// that states a different axis is refused, because a mismatch means the
/// producer and the consumer disagree about what the record means.
[[nodiscard]] constexpr EvidenceAxis axis_of(ObservationKind kind) noexcept {
  switch (kind) {
    case ObservationKind::Measurement:
      return EvidenceAxis::Delivery;
    case ObservationKind::EquipmentState:
      return EvidenceAxis::Condition;
    case ObservationKind::Capability:
      return EvidenceAxis::Capability;
    case ObservationKind::ReserveClaim:
      return EvidenceAxis::Reserve;
    case ObservationKind::Constraint:
    case ObservationKind::Failure:
      return EvidenceAxis::Fault;
  }
  return EvidenceAxis::Delivery;
}

/// Quality of a measured value, as reported alongside it.
enum class MeasurementQuality : std::uint8_t {
  Good = 0,
  Uncertain = 1,
  Bad = 2,           ///< the sensor reports its own value as not trustworthy
  NotAvailable = 3,  ///< the sensor exists and reports nothing usable
};

[[nodiscard]] std::string_view to_token(MeasurementQuality quality) noexcept;
[[nodiscard]] std::optional<MeasurementQuality> parse_measurement_quality(std::string_view token) noexcept;

/// True when a value at this quality may be used as a measured fact. A bad or
/// unavailable sensor reports silence, not zero.
[[nodiscard]] constexpr bool is_usable(MeasurementQuality quality) noexcept {
  return quality == MeasurementQuality::Good || quality == MeasurementQuality::Uncertain;
}

// ---------------------------------------------------------------------------
// Constraint and failure vocabulary
// ---------------------------------------------------------------------------

/// What a constraint restricts.
enum class ConstraintKind : std::uint8_t {
  FlowLimit = 0,
  PressureLimit = 1,
  DifferentialPressure = 2,
  AirflowLimit = 3,
  HeatRemovalLimit = 4,
  SupplyTemperature = 5,
  ValvePosition = 6,
  PumpSpeed = 7,
};

[[nodiscard]] std::string_view to_token(ConstraintKind kind) noexcept;
[[nodiscard]] std::optional<ConstraintKind> parse_constraint_kind(std::string_view token) noexcept;

/// The dimension a constraint kind is expressed in. A constraint stated in the
/// wrong dimension is refused, never coerced.
[[nodiscard]] std::optional<Dimension> dimension_of(ConstraintKind kind) noexcept;

/// Whether a constraint is a ceiling or a floor. Direction is explicit because
/// a pressure constraint is meaningless without it: a minimum pressure and a
/// maximum pressure fail in opposite directions.
enum class ConstraintDirection : std::uint8_t {
  Maximum = 0,
  Minimum = 1,
};

[[nodiscard]] std::string_view to_token(ConstraintDirection direction) noexcept;
[[nodiscard]] std::optional<ConstraintDirection> parse_constraint_direction(std::string_view token) noexcept;

/// Why a constraint is in force.
enum class ConstraintState : std::uint8_t {
  Active = 0,    ///< restricting delivery now
  Inactive = 1,  ///< declared and not restricting now
  Unknown = 2,   ///< declared, state not reported
  Resolved = 3,  ///< was restricting, reported cleared
};

[[nodiscard]] std::string_view to_token(ConstraintState state) noexcept;
[[nodiscard]] std::optional<ConstraintState> parse_constraint_state(std::string_view token) noexcept;

/// The physical class of a failure, as reported by whatever observed it.
enum class FailureKind : std::uint8_t {
  PumpStopped = 0,
  PumpSpeedLost = 1,
  ValveStuck = 2,
  ValveLeak = 3,
  ChillerTrip = 4,
  ChillerDerate = 5,
  CduFault = 6,
  CrahFanFailure = 7,
  CrahCoilFouling = 8,
  SensorLoss = 9,
  CoolantLeak = 10,
  AirflowBlockage = 11,
  FilterLoading = 12,
  InstrumentDrift = 13,
  Unknown = 14,
};

[[nodiscard]] std::string_view to_token(FailureKind kind) noexcept;
[[nodiscard]] std::optional<FailureKind> parse_failure_kind(std::string_view token) noexcept;

/// Reported failure severity. Declaring incident state is not this runtime's
/// authority: this value is consumed from the failure authority and echoed.
enum class FailureSeverity : std::uint8_t {
  Advisory = 0,
  Minor = 1,
  Major = 2,
  Critical = 3,
};

[[nodiscard]] std::string_view to_token(FailureSeverity severity) noexcept;
[[nodiscard]] std::optional<FailureSeverity> parse_failure_severity(std::string_view token) noexcept;

/// How a failure affects delivery.
enum class FailureImpact : std::uint8_t {
  NoDelivery = 0,        ///< the affected element delivers nothing
  ReducedDelivery = 1,   ///< the affected element delivers less than declared
  ResidualDelivery = 2,  ///< reported failed while still passing flow
  UnknownImpact = 3,
};

[[nodiscard]] std::string_view to_token(FailureImpact impact) noexcept;
[[nodiscard]] std::optional<FailureImpact> parse_failure_impact(std::string_view token) noexcept;

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------

/// One asserted fact, with everything needed to judge it and to explain an
/// answer that used it.
struct Observation {
  ObservationKind kind = ObservationKind::Measurement;

  /// The entity this is about. A measurement observation addresses the
  /// measurement identity; every other kind addresses the equipment identity.
  SubjectRef subject{};

  /// For a measurement: which sensor produced it. Recorded so that a
  /// disagreement can be attributed to two named sensors rather than to the
  /// system as a whole.
  SensorId sensor{};

  /// Which authority produced the record, and which of its generations.
  AuthorityRef authority{};

  /// The control-plane epoch that produced it. Evidence from a superseded epoch
  /// is retained for history and never promoted to current.
  EpochId epoch{};

  /// When the value was true at the point of measurement.
  TimestampMs observed_at_ms = 0;
  /// When the record reached this runtime. Used for arrival-order decisions
  /// that must not depend on a producer's clock.
  TimestampMs received_at_ms = 0;

  EvidenceOrigin origin = EvidenceOrigin::Observed;

  /// The producer's own record identity, for idempotent ingestion. Optional for
  /// declarations that are not retried; required for measurements.
  RecordSeq record_seq{};
  GenerationId generation{};  ///< structure generation this record was stated against

  // Measurement
  Quantity measured{};
  MeasurementQuality quality = MeasurementQuality::Good;
  std::string unit_text{};  ///< original unit text, kept for explanation only

  // EquipmentState
  LifecycleState state = LifecycleState::Unknown;

  // Capability
  Dimension capability_dimension = Dimension::Flow;
  std::int64_t declared_capacity = 0;
  /// Derate applied to the declared capacity, in parts per million. Positive
  /// values reduce it. Bounded to [0, 1000000].
  std::int64_t derate_ppm = 0;
  /// True when the declared capability assumes equipment elsewhere in the plant
  /// is available. The observatory records the assumption and does not verify it
  /// on the declaring authority's behalf.
  bool depends_on_redundancy = false;

  // ReserveClaim
  Quantity declared_reserve{};
  /// Identities the claim assumes are available. Held as text: the observatory
  /// resolves them against the topology generation it holds, and reports an
  /// unresolvable assumption rather than dropping it.
  std::vector<StrongId> assumes_available{};

  // Constraint
  ConstraintKind constraint_kind = ConstraintKind::FlowLimit;
  ConstraintDirection direction = ConstraintDirection::Maximum;
  ConstraintState constraint_state = ConstraintState::Active;
  Quantity limit{};
  /// Where the constraint originates, when the producer names a location.
  Maybe<SubjectRef> origin_element{};

  // Failure
  FailureKind failure_kind = FailureKind::Unknown;
  FailureSeverity severity = FailureSeverity::Advisory;
  FailureImpact impact = FailureImpact::UnknownImpact;
  /// Residual delivery the observer measured while the failure is in force.
  /// A failure with residual delivery is not the same claim as a dead branch.
  Maybe<Quantity> residual_delivery{};
  std::string detail{};  ///< free text, explanation only

  /// A monotonic sequence assigned by the engine, so that an explanation for a
  /// derived value can cite an internal identity as well as a durable revision.
  std::uint64_t internal_sequence = 0;

  /// The durable revision and log position at which this observation was
  /// committed. Set by the engine, never by a producer: a producer that could
  /// name its own commit point could claim durability it does not have. These
  /// two values are what an explanation cites.
  Revision committed_revision{};
  std::uint64_t committed_ordinal = 0;
};

/// A generation-stamped reference to one piece of evidence an answer used.
///
/// Every public answer carries these, so a consumer can see exactly which
/// evidence and which revisions produced it. An explanation that cannot name
/// its inputs is not an explanation.
struct EvidenceRef {
  /// The durable revision at which this evidence was committed.
  Revision revision{};
  /// The producer's record sequence, when the evidence came from a record.
  RecordSeq record_seq{};
  EpochId epoch{};
  GenerationId generation{};
  ObservationKind kind = ObservationKind::Measurement;
  AuthorityRef authority{};
  Freshness freshness = Freshness::Unknown;
  SubjectRef subject{};
  /// Position of the record in the committed evidence, so that two records from
  /// the same producer with the same sequence are still distinguishable.
  std::uint64_t ordinal = 0;
};

/// A value together with the freshness of the evidence behind it.
template <typename T>
struct Judged {
  T value{};
  Freshness freshness = Freshness::Unknown;
  std::vector<EvidenceRef> evidence{};

  [[nodiscard]] bool current() const noexcept { return is_current(freshness); }
  [[nodiscard]] bool known() const noexcept { return is_known(freshness); }
};

/// The reason a value is not usable, in a form that can be rendered and
/// asserted on without parsing prose.
struct Indeterminacy {
  Code code = Code::Ok;
  std::string reason{};
  std::string detail{};
  std::vector<EvidenceRef> evidence{};

  [[nodiscard]] bool empty() const noexcept { return code == Code::Ok; }
};

/// Build the explanation reference for one observation. The revision and
/// ordinal are filled in by the engine when the observation is committed.
[[nodiscard]] EvidenceRef make_reference(const Observation& observation, Revision revision = {},
                                         std::uint64_t ordinal = 0);

// ---------------------------------------------------------------------------
// Freshness policy
// ---------------------------------------------------------------------------

/// Age bounds per evidence axis, in milliseconds, plus the epoch and generation
/// rules that decide what current means.
///
/// An age bound is a decision, not a physical constant, so it is configuration
/// with an explicit default rather than a magic number inside the analysis.
struct FreshnessPolicy {
  DurationMs structure_max_age_ms = 24 * 60 * 60 * 1000;  ///< a day
  DurationMs delivery_max_age_ms = 30 * 1000;             ///< 30 seconds
  DurationMs condition_max_age_ms = 5 * 60 * 1000;        ///< 5 minutes
  DurationMs capability_max_age_ms = 60 * 60 * 1000;      ///< an hour
  DurationMs reserve_max_age_ms = 5 * 60 * 1000;          ///< 5 minutes
  DurationMs fault_max_age_ms = 10 * 60 * 1000;           ///< 10 minutes

  /// When false, evidence stamped with an epoch other than the engine's current
  /// epoch is stale rather than fresh. The default refuses cross-epoch evidence
  /// as current, because a superseded control-plane incarnation's numbers
  /// describe a world that no longer exists.
  bool accept_other_epoch_as_current = false;

  /// When false, declarations stamped with a structure generation other than the
  /// adopted one are stale. A capability stated against a topology that has
  /// since changed is a statement about a different plant.
  bool accept_other_generation_as_current = false;

  [[nodiscard]] DurationMs max_age_for(EvidenceAxis axis) const noexcept;

  friend bool operator==(const FreshnessPolicy& a, const FreshnessPolicy& b) noexcept {
    return a.structure_max_age_ms == b.structure_max_age_ms &&
           a.delivery_max_age_ms == b.delivery_max_age_ms &&
           a.condition_max_age_ms == b.condition_max_age_ms &&
           a.capability_max_age_ms == b.capability_max_age_ms &&
           a.reserve_max_age_ms == b.reserve_max_age_ms && a.fault_max_age_ms == b.fault_max_age_ms &&
           a.accept_other_epoch_as_current == b.accept_other_epoch_as_current &&
           a.accept_other_generation_as_current == b.accept_other_generation_as_current;
  }
  friend bool operator!=(const FreshnessPolicy& a, const FreshnessPolicy& b) noexcept { return !(a == b); }
};

[[nodiscard]] const FreshnessPolicy& default_freshness_policy() noexcept;

/// Validate a policy: negative ages are refused.
[[nodiscard]] Status validate(const FreshnessPolicy& policy);

/// Classify one observation against a policy, a current instant, the engine's
/// epoch and the adopted structure generation.
///
/// This is the single place freshness is decided. Everything derived calls it,
/// so a value's freshness cannot drift from the rules that define it.
[[nodiscard]] Freshness classify(const Observation& observation, const FreshnessPolicy& policy,
                                 TimestampMs now_ms, EpochId current_epoch,
                                 GenerationId adopted_generation);

// ---------------------------------------------------------------------------
// Reduction
// ---------------------------------------------------------------------------

/// Several observations of the same thing, reduced to what can be said of it.
///
/// The reduction never averages disagreeing sensors into a middle value that no
/// instrument reported. It reports the value when the current evidence agrees,
/// conflicts when it does not, and unknown when there is none.
struct Reduction {
  Freshness freshness = Freshness::Unknown;
  /// Present when the current sources agree, or when exactly one current source
  /// exists. Absent on conflict, staleness and silence.
  Maybe<Quantity> value{};
  /// Every observation that contributed, in committed order.
  std::vector<EvidenceRef> evidence{};
  /// Sensors whose current evidence disagrees. Non-empty exactly when freshness
  /// is Conflicting.
  std::vector<SensorId> conflicting_sensors{};
  /// Distinct current values seen, ordered by value, when conflicting.
  std::vector<Quantity> conflicting_values{};
};

/// A set of observations of one subject, plus the policy they are judged
/// against, reduced to a single answer.
struct Reducer {
  const FreshnessPolicy* policy = nullptr;
  TimestampMs now_ms = 0;
  EpochId current_epoch{};
  GenerationId adopted_generation{};
  /// Tolerance applied when deciding whether two current values for the same
  /// subject agree, in the subject's own dimension.
  std::int64_t tolerance = 0;

  /// Reduce observations for one subject. Observations that do not address the
  /// subject are ignored.
  [[nodiscard]] Reduction reduce(const std::vector<Observation>& observations, const SubjectRef& subject) const;

  /// Reduce a group already known to share a subject.
  [[nodiscard]] Reduction reduce_group(const std::vector<Observation>& observations) const;
};

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_EVIDENCE_HPP
