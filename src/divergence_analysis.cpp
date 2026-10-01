// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "analysis.hpp"
#include "dccp/cooling_observatory/checked.hpp"

namespace dccp::cooling_observatory {
namespace {

constexpr std::array<std::pair<DivergenceState, std::string_view>, 7> kDivergenceStates{{
    {DivergenceState::Unknown, "unknown"},
    {DivergenceState::Consistent, "consistent"},
    {DivergenceState::UnderDelivery, "under_delivery"},
    {DivergenceState::OverDelivery, "over_delivery"},
    {DivergenceState::Undeclared, "undeclared"},
    {DivergenceState::Dry, "dry"},
    {DivergenceState::Contradictory, "contradictory"},
}};

constexpr std::array<std::pair<ContradictionKind, std::string_view>, 8> kContradictionKinds{{
    {ContradictionKind::FlowWithoutPressure, "flow_without_pressure"},
    {ContradictionKind::PressureWithoutFlow, "pressure_without_flow"},
    {ContradictionKind::DeliveryWhileStopped, "delivery_while_stopped"},
    {ContradictionKind::NoDeliveryWhileRunning, "no_delivery_while_running"},
    {ContradictionKind::SupplyWarmerThanReturn, "supply_warmer_than_return"},
    {ContradictionKind::RemovalAboveCapability, "removal_above_capability"},
    {ContradictionKind::NegativeFlow, "negative_flow"},
    {ContradictionKind::NotCooling, "not_cooling"},
}};

}  // namespace

std::string_view to_token(DivergenceState state) noexcept {
  for (const auto& entry : kDivergenceStates) {
    if (entry.first == state) {
      return entry.second;
    }
  }
  return "unknown";
}

std::optional<DivergenceState> parse_divergence_state(std::string_view token) noexcept {
  for (const auto& entry : kDivergenceStates) {
    if (entry.second == token) {
      return entry.first;
    }
  }
  return std::nullopt;
}

std::string_view to_token(ContradictionKind kind) noexcept {
  for (const auto& entry : kContradictionKinds) {
    if (entry.first == kind) {
      return entry.second;
    }
  }
  return "unknown";
}

std::optional<ContradictionKind> parse_contradiction_kind(std::string_view token) noexcept {
  for (const auto& entry : kContradictionKinds) {
    if (entry.second == token) {
      return entry.first;
    }
  }
  return std::nullopt;
}

namespace {

/// Fold one element's declared flow capability into the binding expectation.
///
/// The binding figure is the smallest current declaration, because a chain
/// carries no more than its narrowest declared element can pass. Every
/// declaration considered is recorded as evidence whether or not it is current,
/// so an explanation can show what was weighed.
void consider_capability(const AnalysisContext& context, const SubjectRef& element,
                         Maybe<Quantity>& binding, std::vector<Freshness>& freshness,
                         Judged<Maybe<Quantity>>& out) {
  for (const Observation& observation : context.evidence_for(element)) {
    if (observation.kind != ObservationKind::Capability) {
      continue;
    }
    const Freshness classified = classify(observation, context.state().freshness_policy,
                                          context.now_ms, context.state().epoch,
                                          context.state().generation);
    out.evidence.push_back(make_reference(observation));
    freshness.push_back(classified);
    if (!is_current(classified) || observation.capability_dimension != Dimension::Flow) {
      continue;
    }
    const std::optional<std::int64_t> derated =
        checked_mul_div_round(observation.declared_capacity, 1000000 - observation.derate_ppm, 1000000);
    if (!derated.has_value()) {
      continue;
    }
    if (!binding.has_value() || derated.value() < binding.value().value) {
      binding = Maybe<Quantity>::of(Quantity(Dimension::Flow, derated.value()));
    }
  }
}

/// The expectation a point's measured flow is compared against.
///
/// The figure is the smallest current declared flow capability among the
/// elements on the point's own delivery path, because a path carries no more
/// than its narrowest declared element can pass. This is deliberately not a
/// capacity sum: summing declared capabilities is capacity accounting, which
/// belongs to the accounting authority and must not be invented here. When no
/// element on the path declares a capability the point has no expectation, and
/// the divergence is reported as undeclared rather than against a guess.
Judged<Maybe<Quantity>> expected_delivery(const AnalysisContext& context, const DeliveryPoint& point,
                                         const std::vector<DeliveryPoint>& points,
                                         const std::vector<DeliveryObservation>& observations) {
  (void)points;
  (void)observations;
  Judged<Maybe<Quantity>> out;
  const PlantModel& structure = context.structure();

  SubjectRef cursor;
  if (!point.zone.empty()) {
    cursor = SubjectRef(SubjectKind::Zone, rebind<StrongId>(point.zone));
  } else if (!point.loop.empty()) {
    cursor = SubjectRef(SubjectKind::Loop, rebind<StrongId>(point.loop));
  }

  std::vector<SubjectRef> visited;
  Maybe<Quantity> binding;
  std::vector<Freshness> freshness;
  while (!cursor.id.empty() &&
         std::find(visited.begin(), visited.end(), cursor) == visited.end()) {
    visited.push_back(cursor);
    const std::vector<PlantLink> incoming = structure.incoming(cursor, true);
    if (incoming.empty()) {
      break;
    }
    // A loop's own equipment is what moves coolant through it, and its declared
    // capability bounds what the loop can pass downstream, so the pumps of a
    // primary loop are in scope for a point on the secondary side even though
    // they are not themselves on the path.
    if (cursor.kind == SubjectKind::Loop) {
      for (const PlantComponent& component : structure.components_in_loop(rebind<LoopId>(cursor.id))) {
        consider_capability(context, component.subject(), binding, freshness, out);
      }
    }
    for (const PlantLink& link : incoming) {
      consider_capability(context, link.from, binding, freshness, out);
    }
    // The chain continues through the first incoming link in canonical order, as
    // in the delivery path.
    cursor = incoming.front().from;
  }

  out.value = binding;
  out.freshness = binding.has_value() ? fold_worst_of(freshness)
                                      : (freshness.empty() ? Freshness::Unknown
                                                           : fold_worst_of(freshness));
  return out;
}

/// The declared capability of the equipment that feeds a point, in watts, from
/// a heat-removal capability declaration. Used only for the removal-above-
/// capability contradiction.
Judged<Maybe<Quantity>> expected_removal(const AnalysisContext& context, const DeliveryPoint& point) {
  Judged<Maybe<Quantity>> out;
  if (point.loop.empty()) {
    return out;
  }
  std::int64_t total = 0;
  bool any = false;
  std::vector<Freshness> freshness;
  for (const PlantComponent& component : context.structure().components()) {
    if (component.loop != point.loop) {
      continue;
    }
    for (const Observation& observation : context.evidence_for(component.subject())) {
      if (observation.kind != ObservationKind::Capability ||
          observation.capability_dimension != Dimension::Power) {
        continue;
      }
      const Freshness classified = classify(observation, context.state().freshness_policy, context.now_ms,
                                            context.state().epoch, context.state().generation);
      out.evidence.push_back(make_reference(observation, context.state().revision, 0));
      freshness.push_back(classified);
      if (!is_current(classified)) {
        continue;
      }
      const std::optional<std::int64_t> next = checked_add(total, observation.declared_capacity);
      if (!next.has_value()) {
        continue;
      }
      total = next.value();
      any = true;
    }
  }
  if (any) {
    out.value = Maybe<Quantity>::of(Quantity(Dimension::Power, total));
    out.freshness = fold_worst_of(freshness);
  }
  return out;
}

void add_contradiction(DivergenceFinding& finding, ContradictionKind kind, const SubjectRef& subject,
                       std::string detail, Maybe<Quantity> first, Maybe<Quantity> second,
                       const std::vector<EvidenceRef>& evidence) {
  Contradiction contradiction;
  contradiction.kind = kind;
  contradiction.subject = subject;
  contradiction.detail = std::move(detail);
  contradiction.first = first;
  contradiction.second = second;
  contradiction.evidence = evidence;
  finding.contradictions.push_back(std::move(contradiction));
}

}  // namespace

DivergenceReport analyse_divergence(const AnalysisContext& context, const QueryFilter& filter) {
  DivergenceReport out;
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

  std::vector<Freshness> parts;
  for (std::size_t i = 0; i < points.size(); ++i) {
    const DeliveryPoint& point = points[i];
    if (filter.point.has_value() && point.id != filter.point.value()) {
      continue;
    }
    if (filter.loop.has_value() && point.loop != filter.loop.value()) {
      continue;
    }
    if (filter.zone.has_value() && point.zone != filter.zone.value()) {
      continue;
    }

    DivergenceFinding finding;
    finding.point = point.id;
    const DeliveryObservation& observation = observations[i];
    finding.observed = observation.flow;
    finding.expected = expected_delivery(context, point, points, observations);
    finding.tolerance = Quantity(Dimension::Flow, 0);

    // Contradictions first: a finding that contains a contradiction is reported
    // as contradictory whatever the arithmetic says, because two observations
    // that cannot both be true do not have a meaningful difference.
    const bool have_flow = observation.flow.value.has_value();
    const bool have_pressure = observation.differential_pressure.value.has_value();
    const bool flow_is_zero = have_flow && observation.flow.value.value().value == 0;
    const bool pressure_is_zero = have_pressure && observation.differential_pressure.value.value().value == 0;

    const SubjectRef point_subject(SubjectKind::Measurement, StrongId(point.id));
    if (have_flow && !flow_is_zero && !have_pressure) {
      add_contradiction(finding, ContradictionKind::FlowWithoutPressure, point_subject,
                        "flow is evidenced through this point and no differential pressure is "
                        "reported across it",
                        observation.flow.value, Maybe<Quantity>::missing(), observation.flow.evidence);
    }
    if (have_pressure && !pressure_is_zero && !have_flow) {
      add_contradiction(finding, ContradictionKind::PressureWithoutFlow, point_subject,
                        "a differential pressure is evidenced across this point and no flow is "
                        "reported through it",
                        observation.differential_pressure.value, Maybe<Quantity>::missing(),
                        observation.differential_pressure.evidence);
    }
    if (have_flow && observation.flow.value.value().value < 0) {
      add_contradiction(finding, ContradictionKind::NegativeFlow, point_subject,
                        "a volumetric flow is reported negative", observation.flow.value,
                        Maybe<Quantity>::missing(), observation.flow.evidence);
    }
    if (have_flow && observation.flow.value.value().value == 0 && have_pressure && !pressure_is_zero) {
      add_contradiction(finding, ContradictionKind::PressureWithoutFlow, point_subject,
                        "a differential pressure is evidenced across a point whose measured flow is "
                        "exactly zero",
                        observation.differential_pressure.value, observation.flow.value,
                        observation.differential_pressure.evidence);
    }
    if (observation.heat_removal.value.has_value() && observation.heat_removal.value.value().value < 0) {
      add_contradiction(finding, ContradictionKind::NotCooling, point_subject,
                        "the computed heat removal is negative, so the delivery point is adding heat",
                        observation.heat_removal.value, Maybe<Quantity>::missing(),
                        observation.heat_removal.evidence);
    }

    // A point whose loop contains equipment reported stopped while flow is
    // measured is a contradiction: the measurement and the reported state
    // cannot both describe the same moment.
    if (have_flow && !flow_is_zero && !point.loop.empty()) {
      for (const PlantComponent& component : context.structure().components()) {
        if (component.loop != point.loop) {
          continue;
        }
        for (const Observation& state : context.evidence_for(component.subject())) {
          if (state.kind != ObservationKind::EquipmentState) {
            continue;
          }
          const Freshness classified = classify(state, context.state().freshness_policy, context.now_ms,
                                                context.state().epoch, context.state().generation);
          if (!is_current(classified)) {
            continue;
          }
          if (state.state == LifecycleState::Off || state.state == LifecycleState::Faulted ||
              state.state == LifecycleState::Maintenance) {
            add_contradiction(finding, ContradictionKind::DeliveryWhileStopped, component.subject(),
                              "flow is measured through a point fed by equipment reported " +
                                  std::string(to_token(state.state)),
                              observation.flow.value, Maybe<Quantity>::missing(), observation.flow.evidence);
          }
          if ((state.state == LifecycleState::Running || state.state == LifecycleState::Degraded) &&
              flow_is_zero) {
            add_contradiction(finding, ContradictionKind::NoDeliveryWhileRunning, component.subject(),
                              "equipment is reported " + std::string(to_token(state.state)) +
                                  " and the point it feeds measures no flow",
                              observation.flow.value, Maybe<Quantity>::missing(),
                              observation.flow.evidence);
          }
        }
      }
    }

    // Temperature pair with the wrong sign.
    if (observation.temperature_difference.value.has_value() &&
        observation.temperature_difference.value.value().value < 0) {
      add_contradiction(finding, ContradictionKind::SupplyWarmerThanReturn, point_subject,
                        "the supply temperature is above the return temperature",
                        observation.temperature_difference.value, Maybe<Quantity>::missing(),
                        observation.temperature_difference.evidence);
    }

    // Removal above declared capability.
    if (observation.heat_removal.value.has_value()) {
      const Judged<Maybe<Quantity>> capability = expected_removal(context, point);
      if (capability.value.has_value() && is_current(capability.freshness) &&
          observation.heat_removal.value.value().value > capability.value.value().value) {
        add_contradiction(finding, ContradictionKind::RemovalAboveCapability, point_subject,
                          "measured heat removal exceeds the heat-removal capability declared for the "
                          "equipment feeding this point",
                          observation.heat_removal.value, capability.value, capability.evidence);
      }
    }

    // The state verdict.
    if (!finding.contradictions.empty()) {
      finding.state = DivergenceState::Contradictory;
    } else if (!have_flow || !is_current(observation.flow.freshness)) {
      finding.state = DivergenceState::Unknown;
      Indeterminacy indeterminacy;
      indeterminacy.code = have_flow ? Code::StaleEvidence : Code::MissingEvidence;
      indeterminacy.reason = have_flow ? "flow_evidence_stale" : "flow_evidence_missing";
      indeterminacy.detail = "divergence needs a current measured flow at the point";
      finding.indeterminacies.push_back(std::move(indeterminacy));
    } else if (!finding.expected.value.has_value() || !is_current(finding.expected.freshness)) {
      finding.state = DivergenceState::Undeclared;
      Indeterminacy indeterminacy;
      indeterminacy.code = Code::MissingEvidence;
      indeterminacy.reason = "no_declared_expectation";
      indeterminacy.detail =
          "no current declared capability covers the equipment feeding this point, so no comparison "
          "is possible";
      finding.indeterminacies.push_back(std::move(indeterminacy));
    } else {
      const std::int64_t observed_value = observation.flow.value.value().value;
      const std::int64_t expected_value = finding.expected.value.value().value;
      const std::optional<std::int64_t> delta = checked_sub(observed_value, expected_value);
      if (!delta.has_value()) {
        finding.state = DivergenceState::Unknown;
      } else {
        finding.delta.value = Maybe<Quantity>::of(Quantity(Dimension::Flow, delta.value()));
        finding.delta.freshness =
            worst(observation.flow.freshness, finding.expected.freshness);
        finding.delta.evidence = observation.flow.evidence;
        const std::int64_t relative =
            checked_mul_div(expected_value, filter.relative_tolerance_ppm, 1000000).value_or(0);
        const std::int64_t tolerance = relative > 0 ? relative : 0;
        finding.tolerance = Quantity(Dimension::Flow, tolerance);
        if (observed_value == 0 && expected_value > 0) {
          finding.state = DivergenceState::Dry;
        } else if (delta.value() > tolerance) {
          finding.state = DivergenceState::OverDelivery;
        } else if (-delta.value() > tolerance) {
          finding.state = DivergenceState::UnderDelivery;
        } else {
          finding.state = DivergenceState::Consistent;
        }
      }
    }

    std::vector<Freshness> local{observation.freshness, finding.expected.freshness};
    finding.freshness = fold_worst_of(local);

    std::sort(finding.contradictions.begin(), finding.contradictions.end());
    out.contradiction_count += finding.contradictions.size();
    if (finding.state != DivergenceState::Consistent) {
      out.divergent_points.push_back(finding.point);
    }
    parts.push_back(finding.freshness);
    if (finding.state == DivergenceState::Consistent && !filter.include_consistent) {
      continue;
    }
    out.findings.push_back(std::move(finding));
  }

  std::sort(out.findings.begin(), out.findings.end(),
            [](const DivergenceFinding& a, const DivergenceFinding& b) { return a.point < b.point; });
  std::sort(out.divergent_points.begin(), out.divergent_points.end());
  out.divergent_points.erase(std::unique(out.divergent_points.begin(), out.divergent_points.end()),
                             out.divergent_points.end());
  out.freshness = parts.empty() ? Freshness::Unknown : fold_worst_of(parts);
  return out;
}

}  // namespace dccp::cooling_observatory