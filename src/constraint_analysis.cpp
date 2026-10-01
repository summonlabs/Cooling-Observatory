// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <algorithm>
#include <utility>
#include <vector>

#include "analysis.hpp"
#include "dccp/cooling_observatory/checked.hpp"

namespace dccp::cooling_observatory {

std::string_view to_token(AttributionState state) noexcept {
  switch (state) {
    case AttributionState::Direct:
      return "direct";
    case AttributionState::Inherited:
      return "inherited";
    case AttributionState::Ambiguous:
      return "ambiguous";
    case AttributionState::Unattributed:
      return "unattributed";
    case AttributionState::Unknown:
      return "unknown";
  }
  return "unknown";
}

std::optional<AttributionState> parse_attribution_state(std::string_view token) noexcept {
  if (token == "direct") {
    return AttributionState::Direct;
  }
  if (token == "inherited") {
    return AttributionState::Inherited;
  }
  if (token == "ambiguous") {
    return AttributionState::Ambiguous;
  }
  if (token == "unattributed") {
    return AttributionState::Unattributed;
  }
  if (token == "unknown") {
    return AttributionState::Unknown;
  }
  return std::nullopt;
}

namespace {

/// The value a constraint of this kind is measured against, taken from a
/// delivery observation. A flow limit is binding against a measured flow and
/// nothing else; the analysis never substitutes a declared capacity for a
/// measurement, because a constraint compared against a plan is not evidence of
/// a restriction.
Maybe<Quantity> observed_in_dimension(const DeliveryObservation& observation, ConstraintKind kind) {
  switch (kind) {
    case ConstraintKind::FlowLimit:
    case ConstraintKind::AirflowLimit:
      return observation.flow.value;
    case ConstraintKind::PressureLimit:
    case ConstraintKind::DifferentialPressure:
      return observation.differential_pressure.value;
    case ConstraintKind::HeatRemovalLimit:
      return observation.heat_removal.value;
    case ConstraintKind::SupplyTemperature:
      // The supply temperature itself is not carried on the delivery
      // observation; the difference is. A supply-temperature constraint is
      // therefore reported without a measured value rather than against the
      // wrong one.
      return Maybe<Quantity>::missing();
    case ConstraintKind::ValvePosition:
    case ConstraintKind::PumpSpeed:
      return Maybe<Quantity>::missing();
  }
  return Maybe<Quantity>::missing();
}

/// The signed distance from the limit, positive when the observed value is on
/// the side the constraint forbids.
Maybe<Quantity> margin_of(const Quantity& observed, const Quantity& limit, ConstraintDirection direction) {
  if (observed.dimension != limit.dimension) {
    return Maybe<Quantity>::missing();
  }
  const std::optional<std::int64_t> gap = checked_sub(observed.value, limit.value);
  if (!gap.has_value()) {
    return Maybe<Quantity>::missing();
  }
  if (direction == ConstraintDirection::Maximum) {
    // A maximum is violated when the observation is above it.
    return Maybe<Quantity>::of(Quantity(observed.dimension, gap.value()));
  }
  // A minimum is violated when the observation is below it.
  const std::optional<std::int64_t> inverted = checked_sub(limit.value, observed.value);
  if (!inverted.has_value()) {
    return Maybe<Quantity>::missing();
  }
  return Maybe<Quantity>::of(Quantity(observed.dimension, inverted.value()));
}

ConstraintAttribution attribute(const AnalysisContext& context, const Observation& declaration,
                               const std::vector<DeliveryPoint>& points,
                               const std::vector<DeliveryObservation>& observations,
                               const QueryFilter& filter) {
  ConstraintAttribution out;
  out.constraint_id = declaration.subject.id;
  out.kind = declaration.constraint_kind;
  out.direction = declaration.direction;
  out.state = declaration.constraint_state;
  out.freshness = classify(declaration, context.state().freshness_policy, context.now_ms,
                           context.state().epoch, context.state().generation);

  out.limit.value = Maybe<Quantity>::of(declaration.limit);
  out.limit.freshness = out.freshness;
  out.limit.evidence.push_back(make_reference(declaration, context.state().revision, 0));

  const std::optional<Dimension> expected = dimension_of(declaration.constraint_kind);
  if (expected.has_value() && declaration.limit.dimension != expected.value()) {
    Indeterminacy indeterminacy;
    indeterminacy.code = Code::UnsupportedValue;
    indeterminacy.reason = "constraint_dimension_mismatch";
    indeterminacy.detail = std::string("a ") + std::string(to_token(declaration.constraint_kind)) +
                           " is expressed in " + std::string(to_token(expected.value())) + ", not in " +
                           std::string(to_token(declaration.limit.dimension));
    out.indeterminacies.push_back(std::move(indeterminacy));
    out.attribution = AttributionState::Unknown;
    return out;
  }

  // Attribution, in order of confidence.
  if (declaration.origin_element.has_value()) {
    const SubjectRef named = declaration.origin_element.value();
    if (context.structure().knows(named)) {
      out.element = Maybe<SubjectRef>::of(named);
      out.attribution = AttributionState::Direct;
    } else {
      Indeterminacy indeterminacy;
      indeterminacy.code = Code::MissingEvidence;
      indeterminacy.reason = "constraint_element_absent";
      indeterminacy.detail = "the declared constraint names " + render_subject_plain(named) +
                             ", which the adopted structure does not contain";
      out.indeterminacies.push_back(std::move(indeterminacy));
    }
  }

  // Which points are in scope for this constraint.
  std::vector<std::size_t> scope;
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (filter.point.has_value() && points[i].id != filter.point.value()) {
      continue;
    }
    if (filter.loop.has_value() && points[i].loop != filter.loop.value()) {
      continue;
    }
    if (filter.zone.has_value() && points[i].zone != filter.zone.value()) {
      continue;
    }
    scope.push_back(i);
  }

  // The constraint's own subject is the element it was declared against; when
  // the declaration names an element, that is the element, and the analysis
  // localises it by asking which points that element feeds.
  SubjectRef localisation_root = declaration.subject;
  if (out.element.has_value()) {
    localisation_root = out.element.value();
  }

  for (const std::size_t index : scope) {
    const DeliveryObservation& observation = observations[index];
    const Maybe<Quantity> observed = observed_in_dimension(observation, declaration.constraint_kind);
    if (observed.has_value()) {
      out.affected_points.push_back(points[index].id);
      if (!points[index].zone.empty()) {
        out.affected_zones.push_back(points[index].zone);
      }
    }
  }

  const bool named_element_absent = declaration.origin_element.has_value() && !out.element.has_value();
  if (named_element_absent) {
    // The declaration named an element and this generation does not contain it.
    // Guessing at a neighbour would replace the producer's claim with this
    // runtime's speculation, so the constraint is reported as unattributed and
    // the missing element is named in the indeterminacies above.
    out.attribution = AttributionState::Unattributed;
  } else if (!out.element.has_value() && !scope.empty()) {
    // No element was named, or the named one is absent. Walk upstream from each
    // in-scope point and collect the elements that could explain the
    // restriction. One candidate is an inherited attribution; more than one is
    // reported as ambiguous rather than resolved by a tie-break rule that the
    // evidence does not support.
    std::vector<SubjectRef> candidates;
    for (const std::size_t index : scope) {
      const DeliveryPoint& point = points[index];
      SubjectRef cursor;
      if (!point.zone.empty()) {
        cursor = SubjectRef(SubjectKind::Zone, rebind<StrongId>(point.zone));
      } else if (!point.loop.empty()) {
        cursor = SubjectRef(SubjectKind::Loop, rebind<StrongId>(point.loop));
      }
      std::vector<SubjectRef> visited;
      while (!cursor.id.empty()) {
        if (std::find(visited.begin(), visited.end(), cursor) != visited.end()) {
          break;
        }
        visited.push_back(cursor);
        if (cursor != localisation_root && context.structure().knows(cursor)) {
          candidates.push_back(cursor);
        }
        const std::vector<PlantLink> incoming = context.structure().incoming(cursor, true);
        if (incoming.empty()) {
          break;
        }
        cursor = incoming.front().from;
      }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    if (candidates.size() == 1) {
      out.element = Maybe<SubjectRef>::of(candidates.front());
      out.attribution = AttributionState::Inherited;
    } else if (candidates.size() > 1) {
      out.candidates = candidates;
      out.attribution = AttributionState::Ambiguous;
      Indeterminacy indeterminacy;
      indeterminacy.code = Code::ConflictingEvidence;
      indeterminacy.reason = "constraint_element_ambiguous";
      indeterminacy.detail = std::to_string(candidates.size()) +
                             " upstream elements could equally explain this constraint; all are "
                             "reported and none is chosen";
      out.indeterminacies.push_back(std::move(indeterminacy));
    } else {
      out.attribution = AttributionState::Unattributed;
      Indeterminacy indeterminacy;
      indeterminacy.code = Code::MissingEvidence;
      indeterminacy.reason = "constraint_element_unknown";
      indeterminacy.detail = "no element upstream of the affected points could be attributed";
      out.indeterminacies.push_back(std::move(indeterminacy));
    }
  } else if (out.element.has_value() && out.attribution == AttributionState::Direct) {
    // A directly attributed constraint still has to say which points it reaches.
    const std::vector<StrongId> reached =
        points_served_by(context, out.element.value(), points);
    if (!reached.empty()) {
      out.affected_points = reached;
      out.affected_zones.clear();
      for (const DeliveryPoint& point : points) {
        if (std::find(reached.begin(), reached.end(), point.id) != reached.end() && !point.zone.empty()) {
          out.affected_zones.push_back(point.zone);
        }
      }
    }
  }

  std::sort(out.affected_points.begin(), out.affected_points.end());
  out.affected_points.erase(std::unique(out.affected_points.begin(), out.affected_points.end()),
                            out.affected_points.end());
  std::sort(out.affected_zones.begin(), out.affected_zones.end());
  out.affected_zones.erase(std::unique(out.affected_zones.begin(), out.affected_zones.end()),
                           out.affected_zones.end());

  if (out.attribution == AttributionState::Direct || out.attribution == AttributionState::Inherited ||
      out.attribution == AttributionState::Ambiguous) {
    // The most binding margin across the affected points, so that the reported
    // margin is the one a consumer has to act on.
    Maybe<Quantity> worst_margin;
    for (const DeliveryObservation& observation : observations) {
      const Maybe<Quantity> observed = observed_in_dimension(observation, declaration.constraint_kind);
      if (!observed.has_value()) {
        continue;
      }
      const Maybe<Quantity> margin = margin_of(observed.value(), declaration.limit, declaration.direction);
      if (!margin.has_value()) {
        continue;
      }
      if (!worst_margin.has_value() || margin.value().value > worst_margin.value().value) {
        worst_margin = margin;
        out.observed_at_point.value = observed;
        out.observed_at_point.freshness = observation.freshness;
        out.observed_at_point.evidence = observation.flow.evidence;
      }
    }
    out.margin.value = worst_margin;
    out.margin.freshness = out.observed_at_point.freshness;
    out.margin.evidence = out.observed_at_point.evidence;
  }

  return out;
}

}  // namespace

ConstraintReport analyse_constraints(const AnalysisContext& context, const QueryFilter& filter) {
  ConstraintReport out;
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

  const std::vector<Observation> declarations = context.evidence_of_kind(ObservationKind::Constraint);
  std::vector<Freshness> parts;
  for (const Observation& declaration : declarations) {
    ConstraintAttribution attribution = attribute(context, declaration, points, observations, filter);
    const bool stale = !is_current(attribution.freshness);
    if (stale) {
      ++out.stale_constraint_count;
    }
    if (stale && !filter.include_stale) {
      continue;
    }
    if (attribution.attribution == AttributionState::Unattributed ||
        attribution.attribution == AttributionState::Unknown) {
      ++out.unattributed_count;
    }
    parts.push_back(attribution.freshness);
    out.constraints.push_back(std::move(attribution));
  }

  std::sort(out.constraints.begin(), out.constraints.end(),
            [](const ConstraintAttribution& a, const ConstraintAttribution& b) {
              if (a.constraint_id != b.constraint_id) {
                return a.constraint_id < b.constraint_id;
              }
              return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
            });

  out.freshness = parts.empty() ? Freshness::Unknown : fold_worst_of(parts);
  return out;
}

}  // namespace dccp::cooling_observatory