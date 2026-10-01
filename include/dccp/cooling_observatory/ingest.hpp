// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_INGEST_HPP
#define DCCP_COOLING_OBSERVATORY_INGEST_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/cooling_observatory/delivery.hpp"
#include "dccp/cooling_observatory/evidence.hpp"
#include "dccp/cooling_observatory/limits.hpp"
#include "dccp/cooling_observatory/plant.hpp"

namespace dccp::cooling_observatory {

/// What one ingested record asks the engine to do.
///
/// The kinds are separate verbs because they carry different authority. Adopting
/// a structure replaces what the observatory believes the plant is; recording an
/// observation adds one fact to a log; retiring a sensor removes a stale source
/// without asserting a new value. Folding them into one update call is how a
/// consumer ends up accidentally replacing a whole generation.
enum class RecordKind : std::uint8_t {
  AdoptStructure = 0,   ///< replace the held structure and advance the generation
  AddObservation = 1,   ///< append one fact
  RetireEvidence = 2,   ///< stop treating one sensor's evidence as current
  ParsePolicy = 3,      ///< replace the freshness policy
  ThermalPolicyUpdate = 4,  ///< replace the thermal constants
  RegisterDeliveryPoint = 5,  ///< declare how a point is instrumented
  ForgetDeliveryPoint = 6,    ///< withdraw a delivery point declaration
};

[[nodiscard]] std::string_view to_token(RecordKind kind) noexcept;
[[nodiscard]] std::optional<RecordKind> parse_record_kind(std::string_view token) noexcept;

/// One unit of ingestion, exactly as it was committed to the durable log.
///
/// The record, not a delta derived from it, is what persistence stores and what
/// recovery replays. Replaying the record through the same code path is what
/// makes the state after a restart and the state before a restart the same
/// object rather than two hopes that agree.
struct IngestRecord {
  RecordKind kind = RecordKind::AddObservation;

  /// Producer-supplied record identity. Required for AddObservation: it is the
  /// idempotency key, and an operation that can be retried without one cannot
  /// be made idempotent.
  RecordSeq record_seq{};
  EpochId epoch{};
  TimestampMs received_at_ms = 0;

  // Observable payload, by kind.
  PlantModel structure{};            ///< AdoptStructure
  GenerationId generation{};         ///< AdoptStructure: the generation being adopted
  std::string generation_witness{};  ///< AdoptStructure: descriptive witness, never interpreted
  Observation observation{};         ///< AddObservation
  SensorId retire_sensor{};          ///< RetireEvidence
  Maybe<SubjectRef> retire_subject{};  ///< RetireEvidence: absent means every subject
  EvidenceAxis retire_axis = EvidenceAxis::Delivery;  ///< RetireEvidence
  FreshnessPolicy freshness_policy{};  ///< ParsePolicy
  ThermalPolicy thermal_policy{};      ///< ThermalPolicyUpdate
  DeliveryPoint delivery_point{};      ///< RegisterDeliveryPoint
  StrongId forget_point{};             ///< ForgetDeliveryPoint

  friend bool operator==(const IngestRecord& a, const IngestRecord& b) noexcept;
};

/// The full observable state of the engine at one committed revision.
///
/// This is an explanation of the durable state, not the durable state itself.
/// It is produced on demand from the committed record log, so a consumer can
/// read it without holding any engine lock and without being able to mutate
/// anything it reads.
struct ObservationImage {
  Revision revision{};
  RecordSeq sequence{};      ///< highest producer record sequence accepted
  EpochId epoch{};
  GenerationId generation{};
  TimestampMs as_of_ms = 0;
  /// True when this image was rebuilt from durable state after a restart and no
  /// new evidence has arrived since. Recovered evidence is never current, and
  /// this flag is how a consumer sees that without inspecting every record.
  bool recovered = false;

  PlantModel structure{};
  std::vector<Observation> evidence{};
  std::vector<DeliveryPoint> delivery_points{};
  FreshnessPolicy freshness_policy{};
  ThermalPolicy thermal_policy{};
  /// Sensors explicitly retired, with the revision at which each retirement
  /// happened. Evidence predating a retirement is not current.
  std::vector<std::pair<SensorId, Revision>> retired_sensors{};
  /// Producer record sequences already applied, in commit order. This is the
  /// durable half of idempotency: after a restart, a producer that retries a
  /// record it already sent is recognised as a duplicate rather than counted
  /// as a second fact.
  std::vector<RecordSeq> applied_keys{};

  [[nodiscard]] bool knows_subject(const SubjectRef& subject) const { return structure.knows(subject); }
  [[nodiscard]] std::size_t evidence_count() const noexcept { return evidence.size(); }
};

/// What one commit changed.
struct Patch {
  Revision revision{};
  RecordSeq sequence{};
  GenerationId generation{};
  std::size_t records_applied = 0;
  std::size_t evidence_added = 0;
  std::size_t evidence_retired = 0;
  bool structure_replaced = false;
  bool policy_changed = false;
  std::string digest{};  ///< canonical digest of the committed image
};

/// Result of one ingestion call.
struct IngestReport {
  Patch patch{};
  /// Records refused, with the reason. A refused record is never partially
  /// applied: ingestion of one call is all or nothing.
  struct Rejection {
    RecordSeq record_seq{};
    std::string reason{};
    std::string detail{};
  };
  std::vector<Rejection> rejections{};
  /// Records accepted but recognised as an exact repeat of one already
  /// committed. Reported rather than silently dropped, because a caller that
  /// retries needs to tell already applied from applied now.
  std::vector<RecordSeq> duplicates{};
  /// True when the engine was reading recovered state and this call refreshed
  /// it. The first commit after a restart is the moment recovered evidence
  /// stops being the only evidence there is.
  bool refreshed_after_recovery = false;
};

/// Build the idempotency key for one observation record.
[[nodiscard]] RecordSeq key_of(const Observation& observation) noexcept;

/// Validate one record against the engine limits before it is applied.
[[nodiscard]] Status validate_record(const IngestRecord& record, const Limits& limits,
                                     std::size_t current_evidence_count);

/// Canonicalise a record's structure so that two records built in different
/// orders commit identically.
void canonicalise(IngestRecord& record);

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_INGEST_HPP
