// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_QUERY_HPP
#define DCCP_COOLING_OBSERVATORY_QUERY_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/cooling_observatory/constraint.hpp"
#include "dccp/cooling_observatory/coverage.hpp"
#include "dccp/cooling_observatory/delivery.hpp"
#include "dccp/cooling_observatory/dependency.hpp"
#include "dccp/cooling_observatory/divergence.hpp"
#include "dccp/cooling_observatory/evidence.hpp"
#include "dccp/cooling_observatory/failure.hpp"
#include "dccp/cooling_observatory/ingest.hpp"
#include "dccp/cooling_observatory/reserve.hpp"

namespace dccp::cooling_observatory {

/// What a caller wants to know. Each kind populates exactly one section of the
/// report, so an answer is never a mixture of things the caller did not ask for.
enum class QueryKind : std::uint8_t {
  Image = 0,        ///< the whole observable state
  Delivery = 1,     ///< what cooling is actually arriving, per point
  Constraints = 2,  ///< where delivery is constrained, and what is responsible
  Failures = 3,     ///< what has failed, and whether delivery confirms it
  Reserve = 4,      ///< claimed and evidenced reserve
  Divergence = 5,   ///< declared expectation against measurement
  Coverage = 6,     ///< what the observatory cannot speak about
  Dependencies = 7, ///< the delivery path and what hangs off it
  History = 8,      ///< what the engine did, in commit order
};

[[nodiscard]] std::string_view to_token(QueryKind kind) noexcept;
[[nodiscard]] std::optional<QueryKind> parse_query_kind(std::string_view token) noexcept;

/// Filters shared by the query kinds. A filter that a kind does not use is
/// ignored, and the report says which kind answered.
struct QueryFilter {
  Maybe<StrongId> point{};
  Maybe<LoopId> loop{};
  Maybe<PlantId> plant{};
  Maybe<ZoneId> zone{};
  Maybe<SubjectRef> subject{};
  Maybe<SubjectRef> root{};  ///< Dependencies: the traversal root

  bool include_stale = false;
  bool include_consistent = true;
  bool include_covered = false;
  bool include_advisory = true;
  /// Reserve: report only scopes whose figure rests on measurement.
  bool require_evidenced = false;
  /// Failures: the lowest declared severity to report.
  Maybe<FailureSeverity> minimum_severity{};
  TraversalOptions traversal{};
  std::int64_t relative_tolerance_ppm = 50'000;
  std::size_t limit = 0;  ///< 0 means use the engine limit
};

/// A request. The engine answers it against one committed revision: every
/// section of the answer describes the same instant, which is what makes an
/// answer internally consistent.
struct ObserveRequest {
  QueryKind kind = QueryKind::Image;
  QueryFilter filter{};
};

/// One history entry, as committed.
struct HistoryEntry {
  Revision revision{};
  RecordSeq record_sequence{};
  std::uint64_t ordinal = 0;
  RecordKind kind = RecordKind::AddObservation;
  ObservationKind observation_kind = ObservationKind::Measurement;
  SubjectRef subject{};
  SensorId sensor{};
  Freshness freshness = Freshness::Unknown;
  EpochId epoch{};
  GenerationId generation{};
  TimestampMs committed_at_ms = 0;
  std::string authority{};
};

/// What the engine answers with.
struct ObservationReport {
  QueryKind kind = QueryKind::Image;
  Revision revision{};
  RecordSeq sequence{};
  EpochId epoch{};
  GenerationId generation{};
  TimestampMs as_of_ms = 0;
  Freshness freshness = Freshness::Unknown;
  bool recovered = false;

  /// Present exactly when the requested kind produces it. The flag is what a
  /// consumer checks, so an empty section is never mistaken for nothing to
  /// report when it in fact means not computed.
  struct Sections {
    bool image = false;
    bool delivery = false;
    bool constraints = false;
    bool failures = false;
    bool reserve = false;
    bool divergence = false;
    bool coverage = false;
    bool dependencies = false;
    bool history = false;
  } sections{};

  ObservationImage image{};
  DeliveryReport delivery{};
  ConstraintReport constraints{};
  FailureReport failures{};
  ReserveReport reserve{};
  DivergenceReport divergence{};
  CoverageReport coverage{};
  DependencyTraversal dependencies{};
  std::vector<HistoryEntry> history{};

  /// Reasons parts of the answer could not be produced. An answer with
  /// indeterminacies is still an answer: it says what is known and names what
  /// is not, instead of failing whole.
  std::vector<Indeterminacy> indeterminacies{};
};

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_QUERY_HPP
