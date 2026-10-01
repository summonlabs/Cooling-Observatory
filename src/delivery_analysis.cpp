// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "analysis.hpp"
#include "dccp/cooling_observatory/checked.hpp"

namespace dccp::cooling_observatory {
namespace {

struct MeasurementRole {
  const char* name;
  MeasurementId DeliveryPoint::*member;
};

/// The measurement roles a delivery point can carry, in the order the path
/// structure names them. Declared once so that the analysis, the text protocol
/// and the documentation cannot drift apart.
const MeasurementRole kRoles[] = {
    {"flow", &DeliveryPoint::flow_measurement},
    {"diff_pressure", &DeliveryPoint::differential_pressure},
    {"supply_temp", &DeliveryPoint::supply_temperature},
    {"return_temp", &DeliveryPoint::return_temperature},
    {"airflow", &DeliveryPoint::airflow},
};

/// The temperature difference between return and supply, when both are current.
Judged<Maybe<Quantity>> temperature_difference(const AnalysisContext& context, const DeliveryPoint& point,
                                               bool& inverted) {
  inverted = false;
  Judged<Maybe<Quantity>> out;
  const Judged<Maybe<Quantity>> supply = judge_measurement(context, point.supply_temperature,
                                                           tolerance_for(Dimension::Temperature));
  const Judged<Maybe<Quantity>> returning = judge_measurement(context, point.return_temperature,
                                                              tolerance_for(Dimension::Temperature));
  out.freshness = worst(supply.freshness, returning.freshness);
  out.evidence = supply.evidence;
  out.evidence.insert(out.evidence.end(), returning.evidence.begin(), returning.evidence.end());

  if (!supply.value.has_value() || !returning.value.has_value()) {
    return out;
  }
  const Quantity supply_value = supply.value.value();
  const Quantity return_value = returning.value.value();
  if (supply_value.dimension != Dimension::Temperature || return_value.dimension != Dimension::Temperature) {
    out.freshness = Freshness::Unknown;
    return out;
  }
  const std::optional<Quantity> drop = sub(return_value, supply_value);
  if (!drop.has_value()) {
    out.freshness = Freshness::Unknown;
    return out;
  }
  // A supply warmer than the return is a physically impossible temperature
  // difference for a cooling path. The observation reports the measured
  // magnitude and the caller records the contradiction; the value is not
  // silently made positive and then treated as cooling.
  inverted = drop.value().value < 0;
  out.value = Maybe<Quantity>::of(Quantity(Dimension::Temperature, drop.value().value));
  return out;
}

Judged<Maybe<Quantity>> heat_removal_from(const AnalysisContext& context, const Quantity& flow,
                                          const Quantity& difference, CoolantMedium medium,
                                          bool& clamped) {
  clamped = false;
  Judged<Maybe<Quantity>> out;
  const ThermalPolicy& policy = context.state().thermal_policy;
  const std::int64_t capacity = medium == CoolantMedium::Air ? policy.air_heat_capacity_uj_per_l_k
                                                             : policy.coolant_heat_capacity_uj_per_l_k;
  // The unit algebra, done once here so that every caller states watts without
  // repeating it:
  //
  //   flow[ul/s] x capacity[uJ/(l*K)] x difference[mK]
  //     = 1e-6 (l/s) x 1e-6 (J/(l*K)) x 1e-3 K
  //     = 1e-9 J/s = 1e-9 W
  //
  // The two multiplies are checked separately so that an overflow is reported
  // rather than wrapped, and the single division by 1e9 is the last operation so
  // that the intermediate stays exact for as long as possible.
  const std::optional<std::int64_t> first = checked_mul(flow.value, capacity);
  if (!first.has_value()) {
    Indeterminacy indeterminacy;
    indeterminacy.code = Code::LimitExceeded;
    indeterminacy.reason = "heat_removal_out_of_range";
    indeterminacy.detail =
        "the product of the measured flow and the coolant capacity does not fit a signed "
        "64-bit value, so no heat removal is computed";
    out.freshness = Freshness::Unknown;
    return out;
  }
  const std::optional<std::int64_t> second = checked_mul(first.value(), difference.value);
  if (!second.has_value()) {
    Indeterminacy indeterminacy;
    indeterminacy.code = Code::LimitExceeded;
    indeterminacy.reason = "heat_removal_out_of_range";
    indeterminacy.detail = "the computed heat removal does not fit a signed 64-bit watt value";
    out.freshness = Freshness::Unknown;
    return out;
  }
  // The quotient is a physical figure derived from measurements, so it is
  // reported at the nearest watt rather than truncated.
  // flow is microlitres per second (1e-6 L/s), the stored capacity is
  // micro-joules per litre per kelvin (1e-6 J/(L*K)) and the difference is
  // millikelvin (1e-3 K), so the product carries a factor of 1e-15.
  const std::optional<std::int64_t> watts = checked_div_round(second.value(), 1000000000000000);
  if (!watts.has_value()) {
    out.freshness = Freshness::Unknown;
    return out;
  }
  std::int64_t value = watts.value();
  if (value < 0 && policy.clamp_negative_removal) {
    value = 0;
    clamped = true;
  }
  out.value = Maybe<Quantity>::of(Quantity(Dimension::Power, value));
  return out;
}
}  // namespace

namespace {

/// True when the image holds any evidence at all for one of the measurement
/// roles a point declares. This is what separates a point whose sensors are
/// silent from a point that has no sensors: the first answered "unknown", the
/// second never answered.
/// The subject a zone is addressed by.
[[nodiscard]] SubjectRef implied_zone_subject(const ZoneId& zone) {
  return SubjectRef(SubjectKind::Zone, rebind<StrongId>(zone));
}

bool point_has_evidence(const AnalysisContext& context, const DeliveryPoint& point) {
  const MeasurementId roles[] = {point.flow_measurement, point.airflow, point.supply_temperature,
                                 point.return_temperature, point.differential_pressure};
  for (const MeasurementId& role : roles) {
    if (role.empty()) {
      continue;
    }
    const SubjectRef subject = measurement_subject(role);
    // The image is searched directly rather than through evidence_for(), so that
    // a retired sensor still counts as an instrument this runtime has heard
    // from. Withdrawing a sensor's standing is a decision about currency, not a
    // claim that the plant was never instrumented.
    for (const Observation& observation : context.state().evidence) {
      if (observation.subject == subject) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

std::vector<DeliveryPoint> delivery_points(const AnalysisContext& context) {
  std::vector<DeliveryPoint> points = context.state().delivery_points;
  std::vector<StrongId> known;
  known.reserve(points.size());
  for (const DeliveryPoint& point : points) {
    known.push_back(point.id);
  }
  std::sort(known.begin(), known.end());

  for (const ThermalZone& zone : context.structure().zones()) {
    if (zone.id.empty()) {
      continue;
    }
    DeliveryPoint implied = implied_delivery_point(zone);
    const auto it = std::lower_bound(known.begin(), known.end(), implied.id);
    if (it != known.end() && *it == implied.id) {
      continue;
    }
    if (implied.loop.empty()) {
      // The zone is fed by an element that belongs to a loop. Recording that loop
      // is what lets the loop-scoped analyses - reserve, divergence and the
      // contradiction checks - see this point at all.
      const std::vector<PlantLink> incoming = context.structure().incoming(implied_zone_subject(zone.id), true);
      for (const PlantLink& link : incoming) {
        const std::optional<PlantComponent> source =
            context.structure().component(link.from.kind, link.from.id);
        if (source.has_value() && !source.value().loop.empty()) {
          implied.loop = source.value().loop;
          break;
        }
      }
    }
    points.push_back(std::move(implied));
  }
  std::sort(points.begin(), points.end());
  return points;
}

DeliveryObservation observe_delivery_point(const AnalysisContext& context, const DeliveryPoint& point,
                                           const QueryFilter& filter) {
  DeliveryObservation out;
  out.point = point;
  (void)filter;

  const Judged<Maybe<Quantity>> flow =
      judge_measurement(context, point.flow_measurement, tolerance_for(Dimension::Flow));
  const Judged<Maybe<Quantity>> airflow =
      judge_measurement(context, point.airflow, tolerance_for(Dimension::Flow));
  // Airflow and liquid flow are the same arithmetic; the medium decides which
  // constant turns the flow into heat, not which code path measures it.
  // The role the point declared decides which sensor speaks for it, not whether
  // that sensor currently has a usable value. Choosing by value would report a
  // stale or conflicting liquid flow as the airflow's silence, which is a
  // different fact about a different instrument.
  out.flow = point.flow_measurement.empty() ? airflow : flow;
  out.differential_pressure =
      judge_measurement(context, point.differential_pressure, tolerance_for(Dimension::Pressure));

  bool inverted = false;
  out.temperature_difference = temperature_difference(context, point, inverted);

  const CoolantMedium medium = point.medium;
  if (medium == CoolantMedium::Undeclared) {
    out.medium_assumed = true;
  }

  bool removal_clamped = false;
  if (out.flow.value.has_value() && out.temperature_difference.value.has_value()) {
    out.heat_removal = heat_removal_from(context, out.flow.value.value(),
                                        out.temperature_difference.value.value(), medium, removal_clamped);
    if (out.heat_removal.value.has_value()) {
      out.heat_removal.freshness =
          fold_worst_of({out.flow.freshness, out.temperature_difference.freshness});
    }
    if (inverted) {
      // The temperature pair is internally contradictory. Heat removal is still
      // computed from the measured values so that the contradiction can be
      // reported against a figure, and the figure is labelled with the
      // contradiction rather than presented as an ordinary reading.
      Indeterminacy indeterminacy;
      indeterminacy.code = Code::ConflictingEvidence;
      indeterminacy.reason = "supply_warmer_than_return";
      indeterminacy.detail =
          "the supply temperature is above the return temperature, so the computed removal is not "
          "cooling";
      out.indeterminacies.push_back(std::move(indeterminacy));
    }
    if (removal_clamped) {
      Indeterminacy indeterminacy;
      indeterminacy.code = Code::ConflictingEvidence;
      indeterminacy.reason = "negative_removal_clamped";
      indeterminacy.detail = "policy clamps a negative computed heat removal to zero";
      out.indeterminacies.push_back(std::move(indeterminacy));
    }
    out.heat_removal.evidence = out.flow.evidence;
    out.heat_removal.evidence.insert(out.heat_removal.evidence.end(),
                                     out.temperature_difference.evidence.begin(),
                                     out.temperature_difference.evidence.end());
  } else {
    // No value was produced, so the figure has no freshness at all: reporting the
    // freshness of inputs that were themselves fine would describe a number that
    // does not exist.
    out.heat_removal.freshness = Freshness::Unknown;
    Indeterminacy indeterminacy;
    indeterminacy.code = out.flow.freshness == Freshness::Unknown ? Code::MissingEvidence
                                                                 : Code::StaleEvidence;
    indeterminacy.reason = out.flow.value.has_value() ? "temperature_difference_unavailable"
                                                      : "flow_unavailable";
    indeterminacy.detail =
        "heat removal needs both a flow and a supply/return pair; a partial instrument set leaves "
        "it unknown rather than zero";
    out.indeterminacies.push_back(std::move(indeterminacy));
  }

  if (point.declared_load.has_value()) {
    Judged<Maybe<Quantity>> declared;
    declared.value = point.declared_load;
    // A declared load is configuration evidence, so its freshness is the
    // freshness of the policy that carries it rather than of a measurement.
    declared.freshness = Freshness::Fresh;
    out.declared_load = declared;
  }
  if (out.declared_load.value.has_value() && out.heat_removal.value.has_value()) {
    const std::optional<Quantity> gap =
        difference(out.declared_load.value.value(), out.heat_removal.value.value());
    if (gap.has_value()) {
      out.load_mismatch.value = Maybe<Quantity>::of(gap.value());
      out.load_mismatch.freshness = worst(out.declared_load.freshness, out.heat_removal.freshness);
      out.load_mismatch.evidence = out.heat_removal.evidence;
    }
  }

  // The delivery path, from the plant towards the point.
  const PlantModel& structure = context.structure();
  SubjectRef cursor;
  if (!point.zone.empty()) {
    cursor = SubjectRef(SubjectKind::Zone, rebind<StrongId>(point.zone));
  } else if (!point.loop.empty()) {
    cursor = SubjectRef(SubjectKind::Loop, rebind<StrongId>(point.loop));
  }
  std::map<SubjectRef, bool> visited;
  while (!cursor.id.empty() && visited.find(cursor) == visited.end()) {
    visited[cursor] = true;
    out.path.push_back(cursor);
    const std::vector<PlantLink> incoming = structure.incoming(cursor, true);
    if (incoming.empty()) {
      break;
    }
    // The chain continues through the first incoming link in canonical order.
    // A convergent structure has several; the path is a representative route to
    // the point, and the full reachable set is what the dependency query
    // reports, so no structure is hidden by choosing one route here.
    cursor = incoming.front().from;
  }

  std::vector<Freshness> parts{out.flow.freshness, out.differential_pressure.freshness,
                               out.heat_removal.freshness};
  out.freshness = fold_worst_of(parts);
  return out;
}

DeliveryReport analyse_delivery(const AnalysisContext& context, const QueryFilter& filter) {
  DeliveryReport out;
  out.generation = context.state().generation;
  out.epoch = context.state().epoch;
  out.revision = context.state().revision;
  out.as_of_ms = context.now_ms;

  const std::vector<DeliveryPoint> points = delivery_points(context);
  std::vector<Quantity> removals;
  std::vector<Freshness> removal_freshness;

  for (const DeliveryPoint& point : points) {
    if (filter.point.has_value() && point.id != filter.point.value()) {
      continue;
    }
    if (filter.loop.has_value() && point.loop != filter.loop.value()) {
      continue;
    }
    if (filter.zone.has_value() && point.zone != filter.zone.value()) {
      continue;
    }
    DeliveryObservation observation = observe_delivery_point(context, point, filter);
    // A point whose sensors are silent belongs in the report as an unknown
    // reading: it answered, and the answer is that nothing arrived. A point for
    // which the image holds no evidence at all cannot answer, and is reported
    // separately so that the difference between silence and absence is visible.
    if (observation.freshness == Freshness::Unknown && !observation.flow.value.has_value() &&
        !observation.differential_pressure.value.has_value() &&
        !observation.temperature_difference.value.has_value() &&
        !point_has_evidence(context, point)) {
      out.unevidenced_points.push_back(point);
      continue;
    }
    if (observation.heat_removal.value.has_value()) {
      removals.push_back(observation.heat_removal.value.value());
      removal_freshness.push_back(observation.heat_removal.freshness);
    }
    out.points.push_back(std::move(observation));
  }

  std::sort(out.unevidenced_points.begin(), out.unevidenced_points.end());

  if (!removals.empty()) {
    std::int64_t total = 0;
    bool overflowed = false;
    for (const Quantity& removal : removals) {
      const std::optional<std::int64_t> next = checked_add(total, removal.value);
      if (!next.has_value()) {
        overflowed = true;
        break;
      }
      total = next.value();
    }
    if (!overflowed) {
      out.total_heat_removal.value = Maybe<Quantity>::of(Quantity(Dimension::Power, total));
      out.total_heat_removal.freshness = fold_worst_of(removal_freshness);
      for (const DeliveryObservation& observation : out.points) {
        if (observation.heat_removal.value.has_value()) {
          out.total_heat_removal.evidence.insert(out.total_heat_removal.evidence.end(),
                                                 observation.heat_removal.evidence.begin(),
                                                 observation.heat_removal.evidence.end());
        }
      }
    }
  }

  std::vector<Freshness> parts;
  for (const DeliveryObservation& observation : out.points) {
    parts.push_back(observation.freshness);
  }
  out.freshness = parts.empty() ? Freshness::Unknown : fold_worst_of(parts);
  return out;
}

namespace {

}  // namespace

namespace {

/// The entry subject of a delivery point: the zone it cools, or the loop it
/// observes.
SubjectRef entry_subject(const DeliveryPoint& point) {
  if (!point.zone.empty()) {
    return SubjectRef(SubjectKind::Zone, rebind<StrongId>(point.zone));
  }
  if (!point.loop.empty()) {
    return SubjectRef(SubjectKind::Loop, rebind<StrongId>(point.loop));
  }
  return SubjectRef();
}

/// One traversal, reused for every point: reachability from a subject is a
/// property of the structure, so computing it per point would be the same
/// answer recomputed.
std::vector<SubjectRef> reachable_set(const AnalysisContext& context, const SubjectRef& subject) {
  std::vector<SubjectRef> reached;
  if (subject.id.empty()) {
    return reached;
  }
  const Result<DependencyTraversal> result =
      traverse(context.structure(), subject,
               TraversalOptions{TraversalOptions::Direction::Downstream, true, true, true,
                                static_cast<std::uint32_t>(context.limits->max_traversal_depth),
                                context.limits->max_traversal_nodes},
               context.state().generation, context.state().revision);
  if (!result.ok()) {
    return reached;
  }
  for (const DependencyNode& node : result.value().nodes) {
    reached.push_back(node.subject);
  }
  std::sort(reached.begin(), reached.end());
  reached.erase(std::unique(reached.begin(), reached.end()), reached.end());
  return reached;
}

}  // namespace

std::vector<ZoneId> zones_served_by(const AnalysisContext& context, const SubjectRef& subject,
                                    const std::vector<DeliveryPoint>& points) {
  const std::vector<SubjectRef> reached = reachable_set(context, subject);
  std::vector<ZoneId> out;
  for (const DeliveryPoint& point : points) {
    if (point.zone.empty()) {
      continue;
    }
    if (!subject.id.empty()) {
      const SubjectRef entry = entry_subject(point);
      if (!std::binary_search(reached.begin(), reached.end(), entry)) {
        continue;
      }
    }
    out.push_back(point.zone);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<StrongId> points_served_by(const AnalysisContext& context, const SubjectRef& subject,
                                       const std::vector<DeliveryPoint>& points) {
  std::vector<StrongId> out;
  const std::vector<SubjectRef> reached = reachable_set(context, subject);
  for (const DeliveryPoint& point : points) {
    if (!subject.id.empty()) {
      const SubjectRef entry = entry_subject(point);
      if (entry.id.empty() || !std::binary_search(reached.begin(), reached.end(), entry)) {
        continue;
      }
    }
    out.push_back(point.id);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}  // namespace dccp::cooling_observatory