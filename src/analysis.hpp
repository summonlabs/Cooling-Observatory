// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal analysis entry points. Every analysis takes the same read-only
// context, so all of them answer against one committed revision, one adopted
// generation, one policy and one instant. Nothing here mutates an image, and
// nothing here takes a lock: the engine holds its lock for the duration of a
// query and calls in with data that cannot change underneath.

#ifndef DCCP_COOLING_OBSERVATORY_ANALYSIS_HPP
#define DCCP_COOLING_OBSERVATORY_ANALYSIS_HPP

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
#include "dccp/cooling_observatory/limits.hpp"
#include "dccp/cooling_observatory/plant.hpp"
#include "dccp/cooling_observatory/query.hpp"
#include "dccp/cooling_observatory/reserve.hpp"

namespace dccp::cooling_observatory {

/// Read-only inputs every analysis shares.
struct AnalysisContext {
  const ObservationImage* image = nullptr;
  const Limits* limits = nullptr;
  TimestampMs now_ms = 0;

  [[nodiscard]] const ObservationImage& state() const { return *image; }
  [[nodiscard]] const PlantModel& structure() const { return image->structure; }

  /// A reducer configured for this context. Tolerance is expressed per call
  /// because two quantities in different dimensions need different tolerances.
  [[nodiscard]] Reducer reducer(std::int64_t tolerance) const;

  /// True when a sensor's evidence has been explicitly withdrawn. A retirement
  /// removes an observation's standing without deleting it, so history still
  /// shows what was seen.
  [[nodiscard]] bool retired(const SensorId& sensor) const;

  /// Every observation that addresses a subject and has not been retired, in
  /// committed order.
  [[nodiscard]] std::vector<Observation> evidence_for(const SubjectRef& subject) const;

  /// Every live observation of one kind, in committed order.
  [[nodiscard]] std::vector<Observation> evidence_of_kind(ObservationKind kind) const;

  /// Why a subject has no usable evidence, in the caller's own words.
  [[nodiscard]] Indeterminacy no_evidence(const SubjectRef& subject, std::string_view what) const;
};

/// Compact rendering of a subject for a diagnostic string.
[[nodiscard]] std::string render_subject_plain(const SubjectRef& subject);

/// The subject an observation of a measurement carries.
[[nodiscard]] SubjectRef measurement_subject(const MeasurementId& measurement);

/// Build the pseudo-subject this library uses for a value it derived itself,
/// for example the heat removal delivered into a loop. The identities are
/// internal and stable; a derived value is never written into the durable log,
/// so no generated identity can collide with a producer's.
[[nodiscard]] SubjectRef derived_subject(std::string_view text);

/// The tolerance applied when two current measurements in one dimension are
/// compared. Per dimension, because a tolerance is only meaningful in the unit
/// it is stated in.
[[nodiscard]] std::int64_t tolerance_for(Dimension dimension) noexcept;

/// Reduce one measurement subject to a judged value.
[[nodiscard]] Judged<Maybe<Quantity>> judge_measurement(const AnalysisContext& context,
                                                        const MeasurementId& measurement,
                                                        std::int64_t tolerance_value);

/// Every delivery point this image can describe: declared points first, then
/// points implied by the zones in the adopted structure.
[[nodiscard]] std::vector<DeliveryPoint> delivery_points(const AnalysisContext& context);

/// Compute what one delivery point evidences.
[[nodiscard]] DeliveryObservation observe_delivery_point(const AnalysisContext& context,
                                                         const DeliveryPoint& point,
                                                         const QueryFilter& filter);

/// Analyse delivery across the image.
[[nodiscard]] DeliveryReport analyse_delivery(const AnalysisContext& context, const QueryFilter& filter);

/// Traverse the structure from a root.
[[nodiscard]] Result<DependencyTraversal> analyse_dependencies(const AnalysisContext& context,
                                                               const SubjectRef& root,
                                                               const TraversalOptions& options,
                                                               GenerationId generation, Revision revision);

/// Which zones the given subject delivers into, in canonical order.
[[nodiscard]] std::vector<ZoneId> zones_served_by(const AnalysisContext& context, const SubjectRef& subject,
                                                  const std::vector<DeliveryPoint>& points);

/// Which delivery points the given subject delivers into, in canonical order.
[[nodiscard]] std::vector<StrongId> points_served_by(const AnalysisContext& context,
                                                     const SubjectRef& subject,
                                                     const std::vector<DeliveryPoint>& points);

/// Analyse constraints.
[[nodiscard]] ConstraintReport analyse_constraints(const AnalysisContext& context, const QueryFilter& filter);

/// Analyse failures.
[[nodiscard]] FailureReport analyse_failures(const AnalysisContext& context, const QueryFilter& filter);

/// Analyse reserve.
[[nodiscard]] ReserveReport analyse_reserve(const AnalysisContext& context, const QueryFilter& filter);

/// Analyse divergence.
[[nodiscard]] DivergenceReport analyse_divergence(const AnalysisContext& context, const QueryFilter& filter);

/// Analyse coverage.
[[nodiscard]] CoverageReport analyse_coverage(const AnalysisContext& context, const QueryFilter& filter);

/// The freshness of an answer assembled from named parts.
///
/// The worst known part decides, and Unknown is the identity of the join rather
/// than the worst case: a value that was never computed does not make a stated
/// part of the same answer less determined. An answer with nothing known at all
/// folds to Unknown, which is exactly what "no evidence anywhere" means.
[[nodiscard]] Freshness fold_worst_of(const std::vector<Freshness>& values);

/// The worst known freshness, or Unknown when nothing is known.
[[nodiscard]] Freshness worst_known(Freshness a, Freshness b) noexcept;

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_ANALYSIS_HPP
