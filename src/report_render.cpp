// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <cstddef>
#include <string>
#include <vector>

#include "dccp/cooling_observatory/clock.hpp"
#include "dccp/cooling_observatory/render.hpp"
#include "dccp/cooling_observatory/textproto.hpp"

namespace dccp::cooling_observatory {
namespace {

/// A renderer that stops emitting rows past a budget and says so. A truncated
/// report is never presented as a complete one.
class TextWriter {
 public:
  explicit TextWriter(std::size_t limit) : limit_(limit) {}

  void line(std::string text) {
    if (lines_ >= limit_) {
      truncated_ = true;
      return;
    }
    out_ += text;
    out_ += '\n';
    ++lines_;
  }

  void section(const char* name) {
    if (!out_.empty()) {
      line("");
    }
    line(std::string("-- ") + name + " --");
  }

  [[nodiscard]] bool truncated() const noexcept { return truncated_; }
  [[nodiscard]] std::string take() { return std::move(out_); }

 private:
  std::string out_{};
  std::size_t lines_ = 0;
  std::size_t limit_ = 0;
  bool truncated_ = false;
};

void write_header(TextWriter& writer, const ObservationReport& report) {
  writer.line(std::string("query ") + std::string(to_token(report.kind)));
  writer.line("revision " + std::to_string(report.revision.value()));
  writer.line("record_sequence " + std::to_string(report.sequence.value()));
  writer.line("epoch " + std::to_string(report.epoch.value()));
  writer.line("generation " + std::to_string(report.generation.value()));
  writer.line("as_of " + format_timestamp(report.as_of_ms));
  writer.line(std::string("freshness ") + std::string(to_token(report.freshness)));
  writer.line(std::string("recovered ") + (report.recovered ? "true" : "false"));
}

void write_jul(TextWriter& writer, const char* name, const Judged<Maybe<Quantity>>& value) {
  writer.line(std::string(name) + " " + render_maybe_quantity(value.value) + " " +
              std::string(to_token(value.freshness)));
}

void write_indeterminacies(TextWriter& writer, const std::vector<Indeterminacy>& indeterminacies) {
  for (const Indeterminacy& indeterminacy : indeterminacies) {
    writer.line(std::string("undetermined ") + std::string(to_token(indeterminacy.code)) + " " +
                indeterminacy.reason + " :: " + indeterminacy.detail);
  }
}

void write_evidence(TextWriter& writer, const std::vector<EvidenceRef>& evidence) {
  for (const EvidenceRef& reference : evidence) {
    writer.line("  evidence " + render_evidence_ref(reference));
  }
}

void write_image(TextWriter& writer, const ObservationReport& report) {
  const ObservationImage& image = report.image;
  writer.section("image");
  writer.line("structure_digest " + image.structure.digest());
  writer.line("facilities " + std::to_string(image.structure.facilities().size()));
  writer.line("plants " + std::to_string(image.structure.plants().size()));
  writer.line("loops " + std::to_string(image.structure.loops().size()));
  writer.line("components " + std::to_string(image.structure.components().size()));
  writer.line("zones " + std::to_string(image.structure.zones().size()));
  writer.line("links " + std::to_string(image.structure.links().size()));
  writer.line("evidence " + std::to_string(image.evidence.size()));
  writer.line("delivery_points " + std::to_string(image.delivery_points.size()));
  writer.line("retired_sensors " + std::to_string(image.retired_sensors.size()));
  writer.line("applied_records " + std::to_string(image.applied_keys.size()));
  writer.line("accept_other_epoch " +
              std::string(image.freshness_policy.accept_other_epoch_as_current ? "true" : "false"));
  writer.line("accept_other_generation " +
              std::string(image.freshness_policy.accept_other_generation_as_current ? "true" : "false"));
  writer.line("coolant_heat_capacity_uj_per_l_k " +
              std::to_string(image.thermal_policy.coolant_heat_capacity_uj_per_l_k));
  writer.line("air_heat_capacity_uj_per_l_k " +
              std::to_string(image.thermal_policy.air_heat_capacity_uj_per_l_k));
  for (const DeliveryPoint& point : image.delivery_points) {
    writer.line("point " + point.id.str() + " zone=" + point.zone.str() + " loop=" + point.loop.str() +
                " medium=" + std::string(to_token(point.medium)));
  }
}

void write_delivery(TextWriter& writer, const ObservationReport& report) {
  const DeliveryReport& delivery = report.delivery;
  writer.section("delivery");
  writer.line("points " + std::to_string(delivery.points.size()));
  writer.line("unevidenced_points " + std::to_string(delivery.unevidenced_points.size()));
  writer.line("total_heat_removal " + render_maybe_quantity(delivery.total_heat_removal.value) + " " +
              std::string(to_token(delivery.total_heat_removal.freshness)));
  for (const DeliveryObservation& observation : delivery.points) {
    writer.line("point " + observation.point.id.str() + " zone=" + observation.point.zone.str() +
                " loop=" + observation.point.loop.str());
    write_jul(writer, "  flow", observation.flow);
    write_jul(writer, "  differential_pressure", observation.differential_pressure);
    write_jul(writer, "  temperature_difference", observation.temperature_difference);
    write_jul(writer, "  heat_removal", observation.heat_removal);
    if (observation.declared_load.value.has_value()) {
      write_jul(writer, "  declared_load", observation.declared_load);
      write_jul(writer, "  load_mismatch", observation.load_mismatch);
    }
    if (observation.medium_assumed) {
      writer.line("  medium_assumed true");
    }
    if (!observation.path.empty()) {
      std::string path;
      for (const SubjectRef& subject : observation.path) {
        if (!path.empty()) {
          path += " <- ";
        }
        path += render_subject(subject);
      }
      writer.line("  path " + path);
    }
    write_indeterminacies(writer, observation.indeterminacies);
  }
  for (const DeliveryPoint& point : delivery.unevidenced_points) {
    writer.line("unevidenced " + point.id.str() + " loop=" + point.loop.str());
  }
}

void write_constraints(TextWriter& writer, const ObservationReport& report) {
  const ConstraintReport& constraints = report.constraints;
  writer.section("constraints");
  writer.line("constraints " + std::to_string(constraints.constraints.size()));
  writer.line("stale_constraints " + std::to_string(constraints.stale_constraint_count));
  writer.line("unattributed " + std::to_string(constraints.unattributed_count));
  for (const ConstraintAttribution& attribution : constraints.constraints) {
    std::string head = "constraint " + attribution.constraint_id.str();
    head += " kind=";
    head += to_token(attribution.kind);
    head += " direction=";
    head += to_token(attribution.direction);
    head += " state=";
    head += to_token(attribution.state);
    head += " attribution=";
    head += to_token(attribution.attribution);
    head += " freshness=";
    head += to_token(attribution.freshness);
    writer.line(head);
    writer.line("  limit " + render_maybe_quantity(attribution.limit.value) + " " +
                std::string(to_token(attribution.limit.freshness)));
    if (attribution.element.has_value()) {
      writer.line("  element " + render_subject(attribution.element.value()));
    }
    for (const SubjectRef& candidate : attribution.candidates) {
      writer.line("  candidate " + render_subject(candidate));
    }
    write_jul(writer, "  observed", attribution.observed_at_point);
    write_jul(writer, "  margin", attribution.margin);
    for (const StrongId& point : attribution.affected_points) {
      writer.line("  affects_point " + point.str());
    }
    for (const ZoneId& zone : attribution.affected_zones) {
      writer.line("  affects_zone " + zone.str());
    }
    write_indeterminacies(writer, attribution.indeterminacies);
    write_evidence(writer, attribution.limit.evidence);
  }
}

void write_failures(TextWriter& writer, const ObservationReport& report) {
  const FailureReport& failures = report.failures;
  writer.section("failures");
  writer.line("failures " + std::to_string(failures.failures.size()));
  writer.line("excluded " + std::to_string(failures.excluded_count));
  for (const FailureAssessment& failure : failures.failures) {
    std::string head = "failure " + failure.failure_id.str();
    head += " kind=";
    head += to_token(failure.kind);
    head += " severity=";
    head += to_token(failure.severity);
    head += " declared_impact=";
    head += to_token(failure.declared_impact);
    head += " effect=";
    head += to_token(failure.effect);
    head += " element_resolved=";
    head += failure.element_resolved ? "true" : "false";
    writer.line(head);
    writer.line("  subject " + render_subject(failure.subject));
    writer.line("  declared_freshness " + std::string(to_token(failure.declared_freshness)));
    writer.line("  declaring_authority " + render_authority(failure.declaring_authority));
    write_jul(writer, "  observed_delivery", failure.observed_delivery);
    write_jul(writer, "  declared_capability", failure.declared_capability);
    if (failure.observed_residual.value.has_value()) {
      write_jul(writer, "  observed_residual", failure.observed_residual);
    }
    for (const StrongId& point : failure.affected_points) {
      writer.line("  affects_point " + point.str());
    }
    for (const ZoneId& zone : failure.affected_zones) {
      writer.line("  affects_zone " + zone.str());
    }
    for (const ZoneId& zone : failure.unevidenced_zones) {
      writer.line("  unevidenced_zone " + zone.str());
    }
    write_indeterminacies(writer, failure.indeterminacies);
  }
}

void write_reserve(TextWriter& writer, const ObservationReport& report) {
  const ReserveReport& reserve = report.reserve;
  writer.section("reserve");
  writer.line(std::string("dimension ") + std::string(to_token(reserve.dimension)));
  writer.line("scopes " + std::to_string(reserve.scopes.size()));
  for (const ReserveObservation& observation : reserve.scopes) {
    writer.line("scope " + render_subject(observation.scope) + " basis=" +
                std::string(to_token(observation.basis)) + " freshness=" +
                std::string(to_token(observation.freshness)));
    write_jul(writer, "  claimed", observation.claimed);
    write_jul(writer, "  evidenced", observation.evidenced);
    write_jul(writer, "  binding", observation.binding);
    write_jul(writer, "  capability", observation.capability);
    write_jul(writer, "  delivered", observation.delivered);
    for (const ReserveContribution& contribution : observation.contributions) {
      std::string line = "  contribution ";
      line += to_token(contribution.kind);
      line += " amount=";
      line += render_quantity(contribution.amount);
      line += " freshness=";
      line += to_token(contribution.freshness);
      if (contribution.element.has_value()) {
        line += " element=";
        line += render_subject(contribution.element.value());
      }
      if (!contribution.claim_id.empty()) {
        line += " claim=";
        line += contribution.claim_id.str();
      }
      writer.line(line);
      for (const StrongId& assumption : contribution.unverified_assumptions) {
        writer.line("    assumes_unverified " + assumption.str());
      }
    }
    write_indeterminacies(writer, observation.indeterminacies);
  }
}

void write_divergence(TextWriter& writer, const ObservationReport& report) {
  const DivergenceReport& divergence = report.divergence;
  writer.section("divergence");
  writer.line("findings " + std::to_string(divergence.findings.size()));
  writer.line("divergent_points " + std::to_string(divergence.divergent_points.size()));
  writer.line("contradictions " + std::to_string(divergence.contradiction_count));
  for (const DivergenceFinding& finding : divergence.findings) {
    writer.line("point " + finding.point.str() + " state=" + std::string(to_token(finding.state)) +
                " freshness=" + std::string(to_token(finding.freshness)));
    write_jul(writer, "  expected", finding.expected);
    write_jul(writer, "  observed", finding.observed);
    write_jul(writer, "  delta", finding.delta);
    writer.line("  tolerance " + render_quantity(finding.tolerance));
    for (const Contradiction& contradiction : finding.contradictions) {
      std::string line = "  contradiction ";
      line += to_token(contradiction.kind);
      line += " subject=";
      line += render_subject(contradiction.subject);
      line += " :: ";
      line += contradiction.detail;
      writer.line(line);
    }
    write_indeterminacies(writer, finding.indeterminacies);
  }
}

void write_coverage(TextWriter& writer, const ObservationReport& report) {
  const CoverageReport& coverage = report.coverage;
  writer.section("coverage");
  for (const AxisCoverage& axis : coverage.axes) {
    std::string line = "axis ";
    line += to_token(axis.axis);
    line += " total=";
    line += std::to_string(axis.subjects_total);
    line += " fresh=";
    line += std::to_string(axis.subjects_fresh);
    line += " stale=";
    line += std::to_string(axis.subjects_stale);
    line += " conflicting=";
    line += std::to_string(axis.subjects_conflicting);
    line += " unknown=";
    line += std::to_string(axis.subjects_unknown);
    line += " fresh_ratio=";
    line += axis.fresh_ratio_ppm.has_value() ? render_ppm_as_percent(axis.fresh_ratio_ppm.value())
                                             : std::string("undefined");
    writer.line(line);
  }
  writer.line("gaps " + std::to_string(coverage.gaps.size()) +
              (coverage.gaps_truncated ? " truncated" : ""));
  for (const CoverageGap& gap : coverage.gaps) {
    std::string line = "gap ";
    line += render_subject(gap.subject);
    line += " axis=";
    line += to_token(gap.axis);
    line += " best=";
    line += to_token(gap.best);
    line += " reason=";
    line += gap.reason;
    line += " records=";
    line += std::to_string(gap.record_count);
    if (gap.age_ms.has_value()) {
      line += " age_ms=";
      line += std::to_string(gap.age_ms.value());
    }
    writer.line(line);
  }
  for (const ZoneId& zone : coverage.unobserved_zones) {
    writer.line("unobserved_zone " + zone.str());
  }
  for (const ZoneId& zone : coverage.unverified_load_zones) {
    writer.line("unverified_load_zone " + zone.str());
  }
}

void write_dependencies(TextWriter& writer, const ObservationReport& report) {
  const DependencyTraversal& traversal = report.dependencies;
  writer.section("dependencies");
  writer.line("root " + render_subject(traversal.root));
  writer.line("nodes " + std::to_string(traversal.nodes.size()));
  writer.line(std::string("truncated ") + (traversal.truncated ? "true" : "false"));
  if (traversal.truncated) {
    writer.line("truncated_by " + traversal.truncated_by);
  }
  writer.line("dangling " + std::to_string(traversal.dangling.size()));
  for (const DependencyNode& node : traversal.nodes) {
    std::string line = "node ";
    line += render_subject(node.subject);
    line += " depth=";
    line += std::to_string(node.depth);
    if (node.via.has_value()) {
      line += " via=";
      line += to_token(node.via.value());
    }
    if (node.parent.has_value()) {
      line += " from=";
      line += render_subject(node.parent.value());
    }
    writer.line(line);
  }
  for (const PlantLink& link : traversal.dangling) {
    writer.line("dangling_link " + render_subject(link.from) + " -> " + render_subject(link.to));
  }
}

void write_history(TextWriter& writer, const ObservationReport& report) {
  writer.section("history");
  writer.line("entries " + std::to_string(report.history.size()));
  for (const HistoryEntry& entry : report.history) {
    std::string line = "entry r";
    line += std::to_string(entry.revision.value());
    line += " ordinal=";
    line += std::to_string(entry.ordinal);
    line += " record=";
    line += to_token(entry.kind);
    line += " subject=";
    line += render_subject(entry.subject);
    line += " sensor=";
    line += entry.sensor.str();
    line += " freshness=";
    line += to_token(entry.freshness);
    line += " epoch=";
    line += std::to_string(entry.epoch.value());
    line += " generation=";
    line += std::to_string(entry.generation.value());
    if (!entry.authority.empty()) {
      line += " authority=";
      line += entry.authority;
    }
    writer.line(line);
  }
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

void json_maybe(const Maybe<Quantity>& value, std::string& out) { out += json_maybe_quantity(value); }

void json_jul(const Judged<Maybe<Quantity>>& value, std::string& out) {
  out += "{\"value\":";
  json_maybe(value.value, out);
  out += ",\"freshness\":";
  out += json_string(to_token(value.freshness));
  out += ",\"evidence_count\":";
  out += std::to_string(value.evidence.size());
  out += '}';
}

void json_evidence(const std::vector<EvidenceRef>& evidence, std::string& out) {
  out += '[';
  for (std::size_t i = 0; i < evidence.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    const EvidenceRef& reference = evidence[i];
    out += "{\"revision\":";
    out += std::to_string(reference.revision.value());
    out += ",\"record_seq\":";
    out += std::to_string(reference.record_seq.value());
    out += ",\"epoch\":";
    out += std::to_string(reference.epoch.value());
    out += ",\"generation\":";
    out += std::to_string(reference.generation.value());
    out += ",\"kind\":";
    out += json_string(to_token(reference.kind));
    out += ",\"authority\":";
    out += json_string(render_authority(reference.authority));
    out += ",\"freshness\":";
    out += json_string(to_token(reference.freshness));
    out += ",\"subject\":";
    out += json_string(render_subject(reference.subject));
    out += ",\"ordinal\":";
    out += std::to_string(reference.ordinal);
    out += '}';
  }
  out += ']';
}

void json_indeterminacies(const std::vector<Indeterminacy>& indeterminacies, std::string& out) {
  out += '[';
  for (std::size_t i = 0; i < indeterminacies.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    out += "{\"code\":";
    out += json_string(to_token(indeterminacies[i].code));
    out += ",\"reason\":";
    out += json_string(indeterminacies[i].reason);
    out += ",\"detail\":";
    out += json_string(indeterminacies[i].detail);
    out += '}';
  }
  out += ']';
}

void json_delivery(const ObservationReport& report, std::string& out) {
  out += "\"delivery\":{\"points\":[";
  for (std::size_t i = 0; i < report.delivery.points.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    const DeliveryObservation& observation = report.delivery.points[i];
    out += "{\"point\":";
    out += json_string(observation.point.id.view());
    out += ",\"zone\":";
    out += json_string(observation.point.zone.view());
    out += ",\"loop\":";
    out += json_string(observation.point.loop.view());
    out += ",\"medium\":";
    out += json_string(to_token(observation.point.medium));
    out += ",\"medium_assumed\":";
    out += json_bool(observation.medium_assumed);
    out += ",\"freshness\":";
    out += json_string(to_token(observation.freshness));
    out += ",\"flow\":";
    json_jul(observation.flow, out);
    out += ",\"differential_pressure\":";
    json_jul(observation.differential_pressure, out);
    out += ",\"temperature_difference\":";
    json_jul(observation.temperature_difference, out);
    out += ",\"heat_removal\":";
    json_jul(observation.heat_removal, out);
    out += ",\"declared_load\":";
    json_jul(observation.declared_load, out);
    out += ",\"load_mismatch\":";
    json_jul(observation.load_mismatch, out);
    out += ",\"path\":[";
    for (std::size_t p = 0; p < observation.path.size(); ++p) {
      if (p != 0) {
        out += ',';
      }
      out += json_string(render_subject(observation.path[p]));
    }
    out += "],\"indeterminacies\":";
    json_indeterminacies(observation.indeterminacies, out);
    out += '}';
  }
  out += "],\"unevidenced_points\":[";
  for (std::size_t i = 0; i < report.delivery.unevidenced_points.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    out += json_string(report.delivery.unevidenced_points[i].id.view());
  }
  out += "],\"total_heat_removal\":";
  json_jul(report.delivery.total_heat_removal, out);
  out += '}';
}

void json_constraints(const ObservationReport& report, std::string& out) {
  out += "\"constraints\":{\"stale\":";
  out += std::to_string(report.constraints.stale_constraint_count);
  out += ",\"unattributed\":";
  out += std::to_string(report.constraints.unattributed_count);
  out += ",\"rows\":[";
  for (std::size_t i = 0; i < report.constraints.constraints.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    const ConstraintAttribution& attribution = report.constraints.constraints[i];
    out += "{\"id\":";
    out += json_string(attribution.constraint_id.view());
    out += ",\"kind\":";
    out += json_string(to_token(attribution.kind));
    out += ",\"direction\":";
    out += json_string(to_token(attribution.direction));
    out += ",\"state\":";
    out += json_string(to_token(attribution.state));
    out += ",\"attribution\":";
    out += json_string(to_token(attribution.attribution));
    out += ",\"freshness\":";
    out += json_string(to_token(attribution.freshness));
    out += ",\"limit\":";
    json_jul(attribution.limit, out);
    out += ",\"element\":";
    out += attribution.element.has_value() ? json_string(render_subject(attribution.element.value()))
                                          : json_null();
    out += ",\"candidates\":[";
    for (std::size_t c = 0; c < attribution.candidates.size(); ++c) {
      if (c != 0) {
        out += ',';
      }
      out += json_string(render_subject(attribution.candidates[c]));
    }
    out += "],\"observed\":";
    json_jul(attribution.observed_at_point, out);
    out += ",\"margin\":";
    json_jul(attribution.margin, out);
    out += ",\"affected_points\":[";
    for (std::size_t p = 0; p < attribution.affected_points.size(); ++p) {
      if (p != 0) {
        out += ',';
      }
      out += json_string(attribution.affected_points[p].view());
    }
    out += "],\"affected_zones\":[";
    for (std::size_t z = 0; z < attribution.affected_zones.size(); ++z) {
      if (z != 0) {
        out += ',';
      }
      out += json_string(attribution.affected_zones[z].view());
    }
    out += "],\"indeterminacies\":";
    json_indeterminacies(attribution.indeterminacies, out);
    out += ",\"evidence\":";
    json_evidence(attribution.limit.evidence, out);
    out += '}';
  }
  out += "]}";
}

void json_failures(const ObservationReport& report, std::string& out) {
  out += "\"failures\":{\"excluded\":";
  out += std::to_string(report.failures.excluded_count);
  out += ",\"rows\":[";
  for (std::size_t i = 0; i < report.failures.failures.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    const FailureAssessment& failure = report.failures.failures[i];
    out += "{\"id\":";
    out += json_string(failure.failure_id.view());
    out += ",\"kind\":";
    out += json_string(to_token(failure.kind));
    out += ",\"severity\":";
    out += json_string(to_token(failure.severity));
    out += ",\"declared_impact\":";
    out += json_string(to_token(failure.declared_impact));
    out += ",\"effect\":";
    out += json_string(to_token(failure.effect));
    out += ",\"subject\":";
    out += json_string(render_subject(failure.subject));
    out += ",\"element_resolved\":";
    out += json_bool(failure.element_resolved);
    out += ",\"declared_freshness\":";
    out += json_string(to_token(failure.declared_freshness));
    out += ",\"observed_delivery\":";
    json_jul(failure.observed_delivery, out);
    out += ",\"declared_capability\":";
    json_jul(failure.declared_capability, out);
    out += ",\"observed_residual\":";
    json_jul(failure.observed_residual, out);
    out += ",\"affected_zones\":[";
    for (std::size_t z = 0; z < failure.affected_zones.size(); ++z) {
      if (z != 0) {
        out += ',';
      }
      out += json_string(failure.affected_zones[z].view());
    }
    out += "],\"unevidenced_zones\":[";
    for (std::size_t z = 0; z < failure.unevidenced_zones.size(); ++z) {
      if (z != 0) {
        out += ',';
      }
      out += json_string(failure.unevidenced_zones[z].view());
    }
    out += "],\"indeterminacies\":";
    json_indeterminacies(failure.indeterminacies, out);
    out += '}';
  }
  out += "]}";
}

void json_reserve(const ObservationReport& report, std::string& out) {
  out += "\"reserve\":{\"dimension\":";
  out += json_string(to_token(report.reserve.dimension));
  out += ",\"scopes\":[";
  for (std::size_t i = 0; i < report.reserve.scopes.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    const ReserveObservation& observation = report.reserve.scopes[i];
    out += "{\"scope\":";
    out += json_string(render_subject(observation.scope));
    out += ",\"basis\":";
    out += json_string(to_token(observation.basis));
    out += ",\"freshness\":";
    out += json_string(to_token(observation.freshness));
    out += ",\"claimed\":";
    json_jul(observation.claimed, out);
    out += ",\"evidenced\":";
    json_jul(observation.evidenced, out);
    out += ",\"binding\":";
    json_jul(observation.binding, out);
    out += ",\"capability\":";
    json_jul(observation.capability, out);
    out += ",\"delivered\":";
    json_jul(observation.delivered, out);
    out += ",\"contributions\":";
    out += std::to_string(observation.contributions.size());
    out += ",\"indeterminacies\":";
    json_indeterminacies(observation.indeterminacies, out);
    out += '}';
  }
  out += "]}";
}

void json_divergence(const ObservationReport& report, std::string& out) {
  out += "\"divergence\":{\"contradictions\":";
  out += std::to_string(report.divergence.contradiction_count);
  out += ",\"rows\":[";
  for (std::size_t i = 0; i < report.divergence.findings.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    const DivergenceFinding& finding = report.divergence.findings[i];
    out += "{\"point\":";
    out += json_string(finding.point.view());
    out += ",\"state\":";
    out += json_string(to_token(finding.state));
    out += ",\"freshness\":";
    out += json_string(to_token(finding.freshness));
    out += ",\"expected\":";
    json_jul(finding.expected, out);
    out += ",\"observed\":";
    json_jul(finding.observed, out);
    out += ",\"delta\":";
    json_jul(finding.delta, out);
    out += ",\"tolerance\":";
    out += json_quantity(finding.tolerance);
    out += ",\"contradictions\":[";
    for (std::size_t c = 0; c < finding.contradictions.size(); ++c) {
      if (c != 0) {
        out += ',';
      }
      out += "{\"kind\":";
      out += json_string(to_token(finding.contradictions[c].kind));
      out += ",\"subject\":";
      out += json_string(render_subject(finding.contradictions[c].subject));
      out += ",\"detail\":";
      out += json_string(finding.contradictions[c].detail);
      out += '}';
    }
    out += "],\"indeterminacies\":";
    json_indeterminacies(finding.indeterminacies, out);
    out += '}';
  }
  out += "]}";
}

void json_coverage(const ObservationReport& report, std::string& out) {
  out += "\"coverage\":{\"axes\":[";
  for (std::size_t i = 0; i < report.coverage.axes.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    const AxisCoverage& axis = report.coverage.axes[i];
    out += "{\"axis\":";
    out += json_string(to_token(axis.axis));
    out += ",\"total\":";
    out += std::to_string(axis.subjects_total);
    out += ",\"fresh\":";
    out += std::to_string(axis.subjects_fresh);
    out += ",\"stale\":";
    out += std::to_string(axis.subjects_stale);
    out += ",\"conflicting\":";
    out += std::to_string(axis.subjects_conflicting);
    out += ",\"unknown\":";
    out += std::to_string(axis.subjects_unknown);
    out += ",\"fresh_ratio_ppm\":";
    out += axis.fresh_ratio_ppm.has_value() ? std::to_string(axis.fresh_ratio_ppm.value())
                                            : std::string("null");
    out += '}';
  }
  out += "],\"gaps_truncated\":";
  out += json_bool(report.coverage.gaps_truncated);
  out += ",\"gaps\":[";
  for (std::size_t i = 0; i < report.coverage.gaps.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    const CoverageGap& gap = report.coverage.gaps[i];
    out += "{\"subject\":";
    out += json_string(render_subject(gap.subject));
    out += ",\"axis\":";
    out += json_string(to_token(gap.axis));
    out += ",\"best\":";
    out += json_string(to_token(gap.best));
    out += ",\"reason\":";
    out += json_string(gap.reason);
    out += ",\"records\":";
    out += std::to_string(gap.record_count);
    out += ",\"age_ms\":";
    out += gap.age_ms.has_value() ? std::to_string(gap.age_ms.value()) : std::string("null");
    out += '}';
  }
  out += "],\"unobserved_zones\":[";
  for (std::size_t i = 0; i < report.coverage.unobserved_zones.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    out += json_string(report.coverage.unobserved_zones[i].view());
  }
  out += "],\"unverified_load_zones\":[";
  for (std::size_t i = 0; i < report.coverage.unverified_load_zones.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    out += json_string(report.coverage.unverified_load_zones[i].view());
  }
  out += "]}";
}

void json_dependencies(const ObservationReport& report, std::string& out) {
  out += "\"dependencies\":{\"root\":";
  out += json_string(render_subject(report.dependencies.root));
  out += ",\"truncated\":";
  out += json_bool(report.dependencies.truncated);
  out += ",\"truncated_by\":";
  out += json_string(report.dependencies.truncated_by);
  out += ",\"nodes\":[";
  for (std::size_t i = 0; i < report.dependencies.nodes.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    const DependencyNode& node = report.dependencies.nodes[i];
    out += "{\"subject\":";
    out += json_string(render_subject(node.subject));
    out += ",\"depth\":";
    out += std::to_string(node.depth);
    out += ",\"via\":";
    out += node.via.has_value() ? json_string(to_token(node.via.value())) : json_null();
    out += ",\"parent\":";
    out += node.parent.has_value() ? json_string(render_subject(node.parent.value())) : json_null();
    out += '}';
  }
  out += "],\"dangling\":[";
  for (std::size_t i = 0; i < report.dependencies.dangling.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    out += "{\"from\":";
    out += json_string(render_subject(report.dependencies.dangling[i].from));
    out += ",\"to\":";
    out += json_string(render_subject(report.dependencies.dangling[i].to));
    out += '}';
  }
  out += "]}";
}

void json_history(const ObservationReport& report, std::string& out) {
  out += "\"history\":[";
  for (std::size_t i = 0; i < report.history.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    const HistoryEntry& entry = report.history[i];
    out += "{\"revision\":";
    out += std::to_string(entry.revision.value());
    out += ",\"record_seq\":";
    out += std::to_string(entry.record_sequence.value());
    out += ",\"ordinal\":";
    out += std::to_string(entry.ordinal);
    out += ",\"record_kind\":";
    out += json_string(to_token(entry.kind));
    out += ",\"observation_kind\":";
    out += json_string(to_token(entry.observation_kind));
    out += ",\"subject\":";
    out += json_string(render_subject(entry.subject));
    out += ",\"sensor\":";
    out += json_string(entry.sensor.view());
    out += ",\"freshness\":";
    out += json_string(to_token(entry.freshness));
    out += ",\"epoch\":";
    out += std::to_string(entry.epoch.value());
    out += ",\"generation\":";
    out += std::to_string(entry.generation.value());
    out += ",\"authority\":";
    out += json_string(entry.authority);
    out += '}';
  }
  out += ']';
}

void json_image(const ObservationReport& report, std::string& out) {
  const ObservationImage& image = report.image;
  out += "\"image\":{\"structure_digest\":";
  out += json_string(image.structure.digest());
  out += ",\"facilities\":";
  out += std::to_string(image.structure.facilities().size());
  out += ",\"plants\":";
  out += std::to_string(image.structure.plants().size());
  out += ",\"loops\":";
  out += std::to_string(image.structure.loops().size());
  out += ",\"components\":";
  out += std::to_string(image.structure.components().size());
  out += ",\"zones\":";
  out += std::to_string(image.structure.zones().size());
  out += ",\"links\":";
  out += std::to_string(image.structure.links().size());
  out += ",\"evidence\":";
  out += std::to_string(image.evidence.size());
  out += ",\"delivery_points\":";
  out += std::to_string(image.delivery_points.size());
  out += ",\"retired_sensors\":";
  out += std::to_string(image.retired_sensors.size());
  out += ",\"applied_records\":";
  out += std::to_string(image.applied_keys.size());
  out += ",\"accept_other_epoch\":";
  out += json_bool(image.freshness_policy.accept_other_epoch_as_current);
  out += ",\"accept_other_generation\":";
  out += json_bool(image.freshness_policy.accept_other_generation_as_current);
  out += '}';
}

}  // namespace

std::string render_text(const ObservationReport& report) {
  TextWriter writer(4096);
  write_header(writer, report);
  if (report.sections.image) {
    write_image(writer, report);
  }
  if (report.sections.delivery) {
    write_delivery(writer, report);
  }
  if (report.sections.constraints) {
    write_constraints(writer, report);
  }
  if (report.sections.failures) {
    write_failures(writer, report);
  }
  if (report.sections.reserve) {
    write_reserve(writer, report);
  }
  if (report.sections.divergence) {
    write_divergence(writer, report);
  }
  if (report.sections.coverage) {
    write_coverage(writer, report);
  }
  if (report.sections.dependencies) {
    write_dependencies(writer, report);
  }
  if (report.sections.history) {
    write_history(writer, report);
  }
  if (!report.indeterminacies.empty()) {
    writer.section("indeterminacies");
    write_indeterminacies(writer, report.indeterminacies);
  }
  if (writer.truncated()) {
    writer.line("truncated true");
  }
  return writer.take();
}

std::string render_json(const ObservationReport& report) {
  std::string out;
  out.reserve(4096);
  out += "{\"query\":";
  out += json_string(to_token(report.kind));
  out += ",\"revision\":";
  out += std::to_string(report.revision.value());
  out += ",\"record_sequence\":";
  out += std::to_string(report.sequence.value());
  out += ",\"epoch\":";
  out += std::to_string(report.epoch.value());
  out += ",\"generation\":";
  out += std::to_string(report.generation.value());
  out += ",\"as_of\":";
  out += json_string(format_timestamp(report.as_of_ms));
  out += ",\"freshness\":";
  out += json_string(to_token(report.freshness));
  out += ",\"recovered\":";
  out += json_bool(report.recovered);
  out += ",\"sections\":{\"image\":";
  out += json_bool(report.sections.image);
  out += ",\"delivery\":";
  out += json_bool(report.sections.delivery);
  out += ",\"constraints\":";
  out += json_bool(report.sections.constraints);
  out += ",\"failures\":";
  out += json_bool(report.sections.failures);
  out += ",\"reserve\":";
  out += json_bool(report.sections.reserve);
  out += ",\"divergence\":";
  out += json_bool(report.sections.divergence);
  out += ",\"coverage\":";
  out += json_bool(report.sections.coverage);
  out += ",\"dependencies\":";
  out += json_bool(report.sections.dependencies);
  out += ",\"history\":";
  out += json_bool(report.sections.history);
  out += '}';

  if (report.sections.image) {
    out += ',';
    json_image(report, out);
  }
  if (report.sections.delivery) {
    out += ',';
    json_delivery(report, out);
  }
  if (report.sections.constraints) {
    out += ',';
    json_constraints(report, out);
  }
  if (report.sections.failures) {
    out += ',';
    json_failures(report, out);
  }
  if (report.sections.reserve) {
    out += ',';
    json_reserve(report, out);
  }
  if (report.sections.divergence) {
    out += ',';
    json_divergence(report, out);
  }
  if (report.sections.coverage) {
    out += ',';
    json_coverage(report, out);
  }
  if (report.sections.dependencies) {
    out += ',';
    json_dependencies(report, out);
  }
  if (report.sections.history) {
    out += ',';
    json_history(report, out);
  }
  out += ",\"indeterminacies\":";
  json_indeterminacies(report.indeterminacies, out);
  out += '}';
  out += '\n';
  return out;
}

std::string render_history_text(const std::vector<HistoryEntry>& history) {
  std::string out;
  for (const HistoryEntry& entry : history) {
    out += "entry r";
    out += std::to_string(entry.revision.value());
    out += " ordinal=";
    out += std::to_string(entry.ordinal);
    out += " record=";
    out += to_token(entry.kind);
    out += " subject=";
    out += render_subject(entry.subject);
    out += " freshness=";
    out += to_token(entry.freshness);
    out += '\n';
  }
  return out;
}

}  // namespace dccp::cooling_observatory
