// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "analysis.hpp"

#include <algorithm>

namespace dccp::cooling_observatory {

/// The tolerance applied when two current measurements of one dimension are
/// compared. It is per dimension because a tolerance is only meaningful in the
/// unit it is stated in: one pascal and one microlitre per second are not the
/// same allowance.
std::int64_t tolerance_for(Dimension dimension) noexcept {
  switch (dimension) {
    case Dimension::Flow:
      return 1000;  // 1 ml/s
    case Dimension::Pressure:
      return 500;  // 500 Pa
    case Dimension::Temperature:
      return 200;  // 0.2 K
    case Dimension::Power:
      return 1000;  // 1 kW
    case Dimension::Frequency:
      return 1000000;  // 1 Hz
    case Dimension::Ratio:
      return 1000;  // 0.1 percentage point
    case Dimension::Energy:
      return 1000;
    case Dimension::Volume:
      return 100;
    case Dimension::None:
      return 0;
  }
  return 0;
}

Reducer AnalysisContext::reducer(std::int64_t tolerance) const {
  Reducer out;
  out.policy = &image->freshness_policy;
  out.now_ms = now_ms;
  out.current_epoch = image->epoch;
  out.adopted_generation = image->generation;
  out.tolerance = tolerance;
  return out;
}

bool AnalysisContext::retired(const SensorId& sensor) const {
  for (const auto& entry : image->retired_sensors) {
    if (entry.first == sensor) {
      return true;
    }
  }
  return false;
}

std::vector<Observation> AnalysisContext::evidence_for(const SubjectRef& subject) const {
  std::vector<Observation> out;
  for (const Observation& observation : image->evidence) {
    if (observation.subject != subject) {
      continue;
    }
    // A retirement removes an observation's standing without deleting it: the
    // record stays in the image and stays visible to a history query, and stops
    // being a current source for an answer.
    if (!observation.sensor.empty() && retired(observation.sensor)) {
      continue;
    }
    out.push_back(observation);
  }
  return out;
}

std::vector<Observation> AnalysisContext::evidence_of_kind(ObservationKind kind) const {
  std::vector<Observation> out;
  for (const Observation& observation : image->evidence) {
    if (observation.kind != kind) {
      continue;
    }
    if (!observation.sensor.empty() && retired(observation.sensor)) {
      continue;
    }
    out.push_back(observation);
  }
  return out;
}

Indeterminacy AnalysisContext::no_evidence(const SubjectRef& subject, std::string_view what) const {
  Indeterminacy out;
  out.code = Code::MissingEvidence;
  out.reason = std::string(what);
  out.detail = "no evidence is held for " + render_subject_plain(subject);
  return out;
}

std::string render_subject_plain(const SubjectRef& subject) {
  std::string out;
  out += to_token(subject.kind);
  out += ':';
  out += subject.id.view();
  return out;
}

SubjectRef measurement_subject(const MeasurementId& measurement) {
  return SubjectRef(SubjectKind::Measurement, rebind<StrongId>(measurement));
}

SubjectRef derived_subject(std::string_view text) {
  return SubjectRef(SubjectKind::Measurement, StrongId::from_validated(std::string(text)));
}

Judged<Maybe<Quantity>> judge_measurement(const AnalysisContext& context, const MeasurementId& measurement,
                                          std::int64_t tolerance_value) {
  Judged<Maybe<Quantity>> out;
  if (measurement.empty()) {
    out.freshness = Freshness::Unknown;
    return out;
  }
  const std::vector<Observation> evidence = context.evidence_for(measurement_subject(measurement));
  if (evidence.empty()) {
    out.freshness = Freshness::Unknown;
    return out;
  }
  const Reducer reducer = context.reducer(tolerance_value);
  const Reduction reduction = reducer.reduce_group(evidence);
  out.freshness = reduction.freshness;
  out.value = reduction.value;
  out.evidence = reduction.evidence;
  return out;
}

Freshness worst_known(Freshness a, Freshness b) noexcept {
  if (a == Freshness::Unknown) {
    return b;
  }
  if (b == Freshness::Unknown) {
    return a;
  }
  return worst(a, b);
}

Freshness fold_worst_of(const std::vector<Freshness>& values) {
  Freshness out = Freshness::Unknown;
  for (const Freshness value : values) {
    out = worst_known(out, value);
  }
  return out;
}

}  // namespace dccp::cooling_observatory