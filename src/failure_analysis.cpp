// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <algorithm>
#include <utility>
#include <vector>

#include "analysis.hpp"
#include "dccp/cooling_observatory/checked.hpp"

namespace dccp::cooling_observatory {

std::string_view to_token(EffectState state) noexcept {
  switch (state) {
    case EffectState::Confirmed:
      return "confirmed";
    case EffectState::Unconfirmed:
      return "unconfirmed";
    case EffectState::Contradicted:
      return "contradicted";
    case EffectState::Residual:
      return "residual";
    case EffectState::Indeterminate:
      return "indeterminate";
  }
  return "indeterminate";
}

std::optional<EffectState> parse_effect_state(std::string_view token) noexcept {
  if (token == "confirmed") {
    return EffectState::Confirmed;
  }
  if (token == "unconfirmed") {
    return EffectState::Unconfirmed;
  }
  if (token == "contradicted") {
    return EffectState::Contradicted;
  }
  if (token == "residual") {
    return EffectState::Residual;
  }
  if (token == "indeterminate") {
    return EffectState::Indeterminate;
  }
  return std::nullopt;
}

namespace {

/// The capability declared for one element, in flows per second when the
/// declaration is a flow. A capability declared in another dimension does not
/// compare to a flow and is reported as unusable rather than converted.
Maybe<Quantity> declared_capability(const AnalysisContext& context, const SubjectRef& subject,
                                    Dimension dimension, Freshness& freshness,
                                    std::vector<EvidenceRef>& evidence,
                                    std::vector<Indeterminacy>& indeterminacies) {
  Maybe<Quantity> out;
  freshness = Freshness::Unknown;
  for (const Observation& observation : context.evidence_for(subject)) {
    if (observation.kind != ObservationKind::Capability) {
      continue;
    }
    const Freshness classified = classify(observation, context.state().freshness_policy, context.now_ms,
                                          context.state().epoch, context.state().generation);
    evidence.push_back(make_reference(observation, context.state().revision, 0));
    freshness = worst(freshness, classified);
    if (!is_current(classified)) {
      continue;
    }
    if (observation.capability_dimension != dimension) {
      Indeterminacy indeterminacy;
      indeterminacy.code = Code::UnsupportedValue;
      indeterminacy.reason = "capability_dimension_mismatch";
      indeterminacy.detail = "a declared capability in " +
                             std::string(to_token(observation.capability_dimension)) +
                             " cannot be compared with a measured " + std::string(to_token(dimension));
      indeterminacies.push_back(std::move(indeterminacy));
      continue;
    }
    const std::optional<std::int64_t> derated =
        checked_mul_div(observation.declared_capacity, 1000000 - observation.derate_ppm, 1000000);
    if (!derated.has_value()) {
      Indeterminacy indeterminacy;
      indeterminacy.code = Code::LimitExceeded;
      indeterminacy.reason = "capability_out_of_range";
      indeterminacy.detail = "the derated capability does not fit a signed 64-bit value";
      indeterminacies.push_back(std::move(indeterminacy));
      continue;
    }
    if (!out.has_value()) {
      out = Maybe<Quantity>::of(Quantity(dimension, derated.value()));
      freshness = Freshness::Fresh;
    } else {
      const std::optional<Quantity> total = add(out.value(), Quantity(dimension, derated.value()));
      if (total.has_value()) {
        out = Maybe<Quantity>::of(total.value());
      }
    }
  }
  return out;
}

/// Where a failure's effect should be visible: the delivery points that depend
/// on the affected element.
struct EffectScope {
  std::vector<StrongId> points;
  std::vector<ZoneId> zones;
  std::vector<ZoneId> unevidenced;
};

EffectScope effect_scope(const AnalysisContext& context, const SubjectRef& element,
                         const std::vector<DeliveryPoint>& points,
                         const std::vector<DeliveryObservation>& observations) {
  EffectScope out;
  const std::vector<StrongId> reached = points_served_by(context, element, points);
  out.points = reached;
  for (std::size_t i = 0; i < points.size(); ++i) {
    const bool in_scope =
        reached.empty() || std::find(reached.begin(), reached.end(), points[i].id) != reached.end();
    if (!in_scope || points[i].zone.empty()) {
      continue;
    }
    out.zones.push_back(points[i].zone);
    if (observations[i].freshness == Freshness::Unknown ||
        (!observations[i].flow.value.has_value() && !observations[i].heat_removal.value.has_value())) {
      out.unevidenced.push_back(points[i].zone);
    }
  }
  std::sort(out.zones.begin(), out.zones.end());
  out.zones.erase(std::unique(out.zones.begin(), out.zones.end()), out.zones.end());
  std::sort(out.unevidenced.begin(), out.unevidenced.end());
  out.unevidenced.erase(std::unique(out.unevidenced.begin(), out.unevidenced.end()), out.unevidenced.end());
  return out;
}

FailureAssessment assess(const AnalysisContext& context, const Observation& declaration,
                         const std::vector<DeliveryPoint>& points,
                         const std::vector<DeliveryObservation>& observations) {
  FailureAssessment out;
  out.failure_id = declaration.subject.id;
  out.kind = declaration.failure_kind;
  out.severity = declaration.severity;
  out.declared_impact = declaration.impact;
  out.subject = declaration.subject;
  out.declaring_authority = declaration.authority;
  out.declared_freshness =
      classify(declaration, context.state().freshness_policy, context.now_ms, context.state().epoch,
               context.state().generation);
  out.freshness = out.declared_freshness;

  if (context.structure().knows(declaration.subject)) {
    out.element = Maybe<SubjectRef>::of(declaration.subject);
    out.element_resolved = true;
  } else {
    Indeterminacy indeterminacy;
    indeterminacy.code = Code::MissingEvidence;
    indeterminacy.reason = "failure_element_absent";
    indeterminacy.detail = "the declared failure names " + render_subject_plain(declaration.subject) +
                           ", which the adopted structure does not contain";
    out.indeterminacies.push_back(std::move(indeterminacy));
  }

  const EffectScope scope = effect_scope(context, declaration.subject, points, observations);
  out.affected_points = scope.points;
  out.affected_zones = scope.zones;
  out.unevidenced_zones = scope.unevidenced;

  // Declared capability of the affected element, for comparison.
  Dimension dimension = Dimension::Flow;
  Maybe<Quantity> capability = declared_capability(context, declaration.subject, dimension,
                                                   out.declared_capability.freshness,
                                                   out.declared_capability.evidence, out.indeterminacies);
  out.declared_capability.value = capability;

  // The measured delivery that speaks for the element: the largest measured
  // flow among the points that depend on it. A failure that claims no delivery
  // while one of its points still has flow is contradicted by that flow.
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (!scope.points.empty() &&
        std::find(scope.points.begin(), scope.points.end(), points[i].id) == scope.points.end()) {
      continue;
    }
    const Maybe<Quantity> flow = observations[i].flow.value;
    if (!flow.has_value()) {
      continue;
    }
    if (!out.observed_delivery.value.has_value() ||
        flow.value().value > out.observed_delivery.value.value().value) {
      out.observed_delivery.value = flow;
      out.observed_delivery.freshness = observations[i].flow.freshness;
      out.observed_delivery.evidence = observations[i].flow.evidence;
    }
  }

  if (declaration.residual_delivery.has_value()) {
    out.observed_residual.value = declaration.residual_delivery;
    out.observed_residual.freshness = out.declared_freshness;
    out.observed_residual.evidence.push_back(
        make_reference(declaration, context.state().revision, 0));
  }

  // The effect verdict.
  const bool have_delivery = out.observed_delivery.value.has_value();
  const bool delivery_current = is_current(out.observed_delivery.freshness);
  if (!have_delivery || !delivery_current) {
    out.effect = EffectState::Indeterminate;
    Indeterminacy indeterminacy;
    indeterminacy.code = have_delivery ? Code::StaleEvidence : Code::MissingEvidence;
    indeterminacy.reason = have_delivery ? "delivery_evidence_stale" : "delivery_evidence_missing";
    indeterminacy.detail =
        "the failure is declared and no current delivery evidence shows or denies its effect";
    out.indeterminacies.push_back(std::move(indeterminacy));
    return out;
  }

  const Quantity delivery = out.observed_delivery.value.value();
  switch (declaration.impact) {
    case FailureImpact::NoDelivery:
      if (delivery.value > 0) {
        out.effect = EffectState::Residual;
      } else {
        out.effect = EffectState::Confirmed;
      }
      break;
    case FailureImpact::ReducedDelivery:
      if (out.declared_capability.value.has_value() && is_current(out.declared_capability.freshness)) {
        if (delivery.value >= out.declared_capability.value.value().value) {
          out.effect = EffectState::Contradicted;
        } else if (delivery.value > 0) {
          out.effect = EffectState::Confirmed;
        } else {
          out.effect = EffectState::Residual;
        }
      } else {
        out.effect = delivery.value > 0 ? EffectState::Confirmed : EffectState::Indeterminate;
      }
      break;
    case FailureImpact::ResidualDelivery:
      out.effect = delivery.value > 0 ? EffectState::Residual : EffectState::Contradicted;
      break;
    case FailureImpact::UnknownImpact:
      out.effect = delivery.value > 0 ? EffectState::Residual : EffectState::Unconfirmed;
      break;
  }
  return out;
}

}  // namespace

FailureReport analyse_failures(const AnalysisContext& context, const QueryFilter& filter) {
  FailureReport out;
  out.generation = context.state().generation;
  out.epoch = context.state().epoch;
  out.revision = context.state().revision;
  out.as_of_ms = context.now_ms;

  const std::vector<DeliveryPoint> points = delivery_points(context);
  std::vector<DeliveryObservation> observations;
  observations.reserve(points.size());
  for (const DeliveryPoint& point : points) {
    observations.push_back(observe_delivery_point(context, point, filter));
  }

  const std::vector<Observation> declarations = context.evidence_of_kind(ObservationKind::Failure);
  std::vector<Freshness> parts;
  for (const Observation& declaration : declarations) {
    FailureAssessment assessment = assess(context, declaration, points, observations);

    if (!filter.include_advisory && assessment.severity == FailureSeverity::Advisory) {
      ++out.excluded_count;
      continue;
    }
    if (filter.minimum_severity.has_value() &&
        static_cast<std::uint8_t>(assessment.severity) <
            static_cast<std::uint8_t>(filter.minimum_severity.value())) {
      ++out.excluded_count;
      continue;
    }
    if (filter.subject.has_value() && assessment.subject != filter.subject.value()) {
      ++out.excluded_count;
      continue;
    }
    if (filter.zone.has_value() &&
        std::find(assessment.affected_zones.begin(), assessment.affected_zones.end(),
                  filter.zone.value()) == assessment.affected_zones.end()) {
      ++out.excluded_count;
      continue;
    }
    if (!is_current(assessment.declared_freshness) && !filter.include_stale) {
      ++out.excluded_count;
      continue;
    }
    parts.push_back(assessment.declared_freshness);
    out.failures.push_back(std::move(assessment));
  }

  std::sort(out.failures.begin(), out.failures.end(),
            [](const FailureAssessment& a, const FailureAssessment& b) {
              if (a.failure_id != b.failure_id) {
                return a.failure_id < b.failure_id;
              }
              return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
            });

  out.freshness = parts.empty() ? Freshness::Unknown : fold_worst_of(parts);
  return out;
}

}  // namespace dccp::cooling_observatory