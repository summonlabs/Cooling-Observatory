// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <algorithm>
#include <array>
#include <map>
#include <utility>
#include <vector>

#include "analysis.hpp"
#include "dccp/cooling_observatory/checked.hpp"

namespace dccp::cooling_observatory {
namespace {

constexpr std::array<EvidenceAxis, 6> kAxes{{
    EvidenceAxis::Structure, EvidenceAxis::Delivery,   EvidenceAxis::Condition,
    EvidenceAxis::Capability, EvidenceAxis::Reserve,   EvidenceAxis::Fault,
}};

/// Which evidence axes a subject kind is expected to carry. A subject is only
/// reported as a gap on an axis that could meaningfully have evidence, so a
/// valve is never counted as missing reserve and a zone is never counted as
/// missing a lifecycle state.
bool axis_applies(SubjectKind kind, EvidenceAxis axis) {
  if (axis == EvidenceAxis::Structure) {
    return false;  // structure is adopted as a whole, not per subject
  }
  switch (kind) {
    case SubjectKind::Facility:
    case SubjectKind::Plant:
      return axis == EvidenceAxis::Delivery || axis == EvidenceAxis::Condition ||
             axis == EvidenceAxis::Capability || axis == EvidenceAxis::Reserve ||
             axis == EvidenceAxis::Fault;
    case SubjectKind::Loop:
      return axis == EvidenceAxis::Delivery || axis == EvidenceAxis::Reserve ||
             axis == EvidenceAxis::Fault || axis == EvidenceAxis::Condition;
    case SubjectKind::Pump:
    case SubjectKind::Valve:
    case SubjectKind::Chiller:
    case SubjectKind::Cdu:
    case SubjectKind::Crah:
    case SubjectKind::Manifold:
    case SubjectKind::Branch:
      return axis == EvidenceAxis::Condition || axis == EvidenceAxis::Capability ||
             axis == EvidenceAxis::Delivery || axis == EvidenceAxis::Fault;
    case SubjectKind::Zone:
    case SubjectKind::RackGroup:
      return axis == EvidenceAxis::Delivery || axis == EvidenceAxis::Fault ||
             axis == EvidenceAxis::Capability;
    case SubjectKind::Measurement:
    case SubjectKind::Sensor:
      return axis == EvidenceAxis::Delivery;
  }
  return false;
}

std::string gap_reason(Freshness best, std::size_t records) {
  if (records == 0) {
    return "no_evidence";
  }
  switch (best) {
    case Freshness::Fresh:
      return {};
    case Freshness::Stale:
      return "stale";
    case Freshness::Recovered:
      return "recovered";
    case Freshness::Conflicting:
      return "conflicting";
    case Freshness::Unknown:
      return "unknown_value";
  }
  return "unknown_value";
}

}  // namespace

CoverageReport analyse_coverage(const AnalysisContext& context, const QueryFilter& filter) {
  CoverageReport out;
  out.generation = context.state().generation;
  out.epoch = context.state().epoch;
  out.revision = context.state().revision;
  out.as_of_ms = context.now_ms;

  // The subject universe: everything the adopted structure names, in canonical
  // order. A subject the structure does not name is not a coverage gap; it is
  // simply not part of this plant.
  std::vector<SubjectRef> subjects;
  for (const PlantComponent& component : context.structure().components()) {
    if (!component.id.empty()) {
      subjects.push_back(component.subject());
    }
  }
  for (const ThermalZone& zone : context.structure().zones()) {
    if (!zone.id.empty()) {
      subjects.push_back(SubjectRef(SubjectKind::Zone, rebind<StrongId>(zone.id)));
    }
  }
  for (const Loop& loop : context.structure().loops()) {
    if (!loop.id.empty()) {
      subjects.push_back(SubjectRef(SubjectKind::Loop, rebind<StrongId>(loop.id)));
    }
  }
  for (const CoolingPlant& plant : context.structure().plants()) {
    if (!plant.id.empty()) {
      subjects.push_back(SubjectRef(SubjectKind::Plant, rebind<StrongId>(plant.id)));
    }
  }
  // The measurements themselves are subjects too. A sensor that contradicts
  // another, or that has gone quiet, is a gap in the observatory's knowledge in
  // its own right: reporting it only through the zone it serves would hide which
  // instrument is at fault.
  for (const Observation& observation : context.state().evidence) {
    if (observation.subject.kind == SubjectKind::Measurement && !observation.subject.id.empty()) {
      subjects.push_back(observation.subject);
    }
  }
  std::sort(subjects.begin(), subjects.end());
  subjects.erase(std::unique(subjects.begin(), subjects.end()), subjects.end());

  struct Cell {
    std::size_t records = 0;
    Freshness best = Freshness::Unknown;
    bool have_age = false;
    DurationMs freshest_age = 0;
  };
  std::map<SubjectRef, std::array<Cell, kEvidenceAxisCount>> cells;
  /// The observations behind each cell, so that a cell whose sources disagree can
  /// be judged as a group. Classifying observation by observation would call two
  /// individually current but contradictory sensors a fresh reading.
  std::map<SubjectRef, std::array<std::vector<Observation>, kEvidenceAxisCount>> groups;

  for (const Observation& observation : context.state().evidence) {
    // A retired sensor's evidence is history. It is not current coverage and it
    // is not counted as a gap that fresh evidence could close, because the
    // retirement is a decision, not a failure.
    if (!observation.sensor.empty() && context.retired(observation.sensor)) {
      continue;
    }
    const EvidenceAxis axis = axis_of(observation.kind);
    if (!axis_applies(observation.subject.kind, axis)) {
      continue;
    }
    Cell& cell = cells[observation.subject][static_cast<std::size_t>(axis)];
    ++cell.records;
    groups[observation.subject][static_cast<std::size_t>(axis)].push_back(observation);
    const Freshness classified = classify(observation, context.state().freshness_policy, context.now_ms,
                                          context.state().epoch, context.state().generation);
    cell.best = worst_known(cell.best, classified);
    const DurationMs age = elapsed_ms(context.now_ms, observation.observed_at_ms);
    if (!cell.have_age || age < cell.freshest_age) {
      cell.have_age = true;
      cell.freshest_age = age;
    }
  }

  // Fold each group's own reduction into its cell, so a disagreement between
  // current sources is reported as a conflict rather than as freshness.
  for (auto& entry : groups) {
    for (std::size_t index = 0; index < kEvidenceAxisCount; ++index) {
      const std::vector<Observation>& group = entry.second[index];
      if (group.empty()) {
        continue;
      }
      const Dimension dimension = group.front().measured.dimension;
      const Reduction reduction = context.reducer(tolerance_for(dimension)).reduce_group(group);
      cells[entry.first][index].best = worst_known(cells[entry.first][index].best, reduction.freshness);
    }
  }

  for (EvidenceAxis axis : kAxes) {
    AxisCoverage coverage;
    coverage.axis = axis;
    for (const SubjectRef& subject : subjects) {
      if (!axis_applies(subject.kind, axis)) {
        continue;
      }
      ++coverage.subjects_total;
      const auto it = cells.find(subject);
      const Cell* cell = nullptr;
      if (it != cells.end()) {
        cell = &it->second[static_cast<std::size_t>(axis)];
      }
      const Freshness best = cell != nullptr ? cell->best : Freshness::Unknown;
      const std::size_t records = cell != nullptr ? cell->records : 0;
      switch (best) {
        case Freshness::Fresh:
          ++coverage.subjects_fresh;
          break;
        case Freshness::Stale:
          ++coverage.subjects_stale;
          break;
        case Freshness::Conflicting:
          ++coverage.subjects_conflicting;
          break;
        case Freshness::Recovered:
        case Freshness::Unknown:
          ++coverage.subjects_unknown;
          break;
      }
      if (best != Freshness::Fresh || filter.include_covered) {
        CoverageGap gap;
        gap.subject = subject;
        gap.axis = axis;
        gap.best = best;
        gap.record_count = records;
        gap.reason = gap_reason(best, records);
        if (cell != nullptr && cell->have_age) {
          gap.age_ms = Maybe<DurationMs>::of(cell->freshest_age);
        }
        out.gaps.push_back(std::move(gap));
      }
    }
    if (coverage.subjects_total > 0) {
      coverage.fresh_ratio_ppm =
          Maybe<std::int64_t>::of(static_cast<std::int64_t>(
              (coverage.subjects_fresh * 1000000ull) / coverage.subjects_total));
    }
    out.axes.push_back(coverage);
  }

  std::sort(out.gaps.begin(), out.gaps.end());
  if (out.gaps.size() > context.limits->max_report_rows) {
    out.gaps.resize(context.limits->max_report_rows);
    out.gaps_truncated = true;
  }

  // The two findings that matter most to a consumer: zones with no current
  // delivery evidence at all, and zones whose declared load has no measured
  // removal behind it.
  const std::vector<DeliveryPoint> points = delivery_points(context);
  for (const ThermalZone& zone : context.structure().zones()) {
    if (zone.id.empty()) {
      continue;
    }
    const SubjectRef subject(SubjectKind::Zone, rebind<StrongId>(zone.id));
    const auto it = cells.find(subject);
    const Freshness delivery = it != cells.end() ? it->second[static_cast<std::size_t>(EvidenceAxis::Delivery)].best
                                                 : Freshness::Unknown;
    if (delivery != Freshness::Fresh) {
      out.unobserved_zones.push_back(zone.id);
    }
    for (const DeliveryPoint& point : points) {
      if (point.zone != zone.id) {
        continue;
      }
      const DeliveryObservation observation = observe_delivery_point(context, point, filter);
      if (!observation.heat_removal.value.has_value() && point.declared_load.has_value()) {
        out.unverified_load_zones.push_back(zone.id);
      }
    }
  }
  std::sort(out.unobserved_zones.begin(), out.unobserved_zones.end());
  out.unobserved_zones.erase(std::unique(out.unobserved_zones.begin(), out.unobserved_zones.end()),
                             out.unobserved_zones.end());
  std::sort(out.unverified_load_zones.begin(), out.unverified_load_zones.end());
  out.unverified_load_zones.erase(
      std::unique(out.unverified_load_zones.begin(), out.unverified_load_zones.end()),
      out.unverified_load_zones.end());

  std::vector<Freshness> parts;
  for (const AxisCoverage& coverage : out.axes) {
    if (coverage.subjects_total == 0) {
      continue;
    }
    if (coverage.subjects_fresh == coverage.subjects_total) {
      parts.push_back(Freshness::Fresh);
    } else if (coverage.subjects_stale > 0) {
      parts.push_back(Freshness::Stale);
    } else if (coverage.subjects_conflicting > 0) {
      parts.push_back(Freshness::Conflicting);
    } else {
      parts.push_back(Freshness::Unknown);
    }
  }
  out.freshness = parts.empty() ? Freshness::Unknown : fold_worst_of(parts);
  return out;
}

}  // namespace dccp::cooling_observatory