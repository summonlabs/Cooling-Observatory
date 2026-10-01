// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The adversarial pass over evidence handling: sensor silence, stale topology
// and capacity, contradictory flow and pressure, failed equipment with residual
// delivery, duplicate events, numeric extremes and malformed input. Every case
// here is a real operating condition that a naive observatory reports as health.

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "dccp/cooling_observatory/render.hpp"
#include "dccp/cooling_observatory/textproto.hpp"
#include "support.hpp"
#include "test_harness.hpp"

using namespace dccp::cooling_observatory;

namespace {

bool has_contradiction(const DivergenceFinding& finding, ContradictionKind kind) {
  return std::any_of(finding.contradictions.begin(), finding.contradictions.end(),
                     [kind](const Contradiction& contradiction) { return contradiction.kind == kind; });
}

}  // namespace

CO_TEST("a silent sensor is unknown, and never zero and never healthy") {
  cotest::Fixture fixture("adversarial-silence", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  // One zone is instrumented with a sensor that reports NotAvailable, which is
  // what a dead but present sensor reports.
  IngestRecord silent = cotest::zone_measurement(1, epoch, "zone.a", "flow", "s.flow",
                                                 Quantity::flow(0), fixture.clock.now_ms());
  silent.observation.quality = MeasurementQuality::NotAvailable;
  CO_REQUIRE_OK(fixture.engine.ingest({silent}));

  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Delivery);
  const DeliveryObservation& zone_a = cotest::delivery_for(report.delivery, "zone.a");
  CO_CHECK(!zone_a.flow.value.has_value());
  CO_CHECK(zone_a.flow.freshness == Freshness::Unknown);
  CO_CHECK(zone_a.freshness == Freshness::Unknown);

  // A zone with no instrumentation at all is reported as unevidenced rather
  // than as a zone with zero flow.
  bool unevidenced = false;
  for (const DeliveryPoint& point : report.delivery.unevidenced_points) {
    if (point.id.view() == "dp.zone.b") {
      unevidenced = true;
    }
  }
  CO_CHECK(unevidenced);

  const ObservationReport coverage = cotest::query(fixture.engine, QueryKind::Coverage);
  CO_CHECK(std::find(coverage.coverage.unobserved_zones.begin(), coverage.coverage.unobserved_zones.end(),
                     ZoneId(StrongId::from_validated("zone.a"))) !=
           coverage.coverage.unobserved_zones.end());
}

CO_TEST("a stale measurement is retained and stops being current") {
  cotest::Fixture fixture("adversarial-stale-age", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::zone_measurement(1, epoch, "zone.a", "flow", "s.flow", Quantity::flow(30'000'000),
                                fixture.clock.now_ms())}));
  fixture.clock.advance(10 * 60 * 1000);
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Delivery);
  const DeliveryObservation& zone_a = cotest::delivery_for(report.delivery, "zone.a");
  CO_CHECK(zone_a.flow.freshness == Freshness::Stale);
  CO_CHECK(!zone_a.flow.value.has_value());

  // The evidence is still held: history is not deleted by time.
  const ObservationReport history = cotest::query(fixture.engine, QueryKind::History);
  CO_CHECK_EQ(history.history.size(), 1u);
}

CO_TEST("a capability declared against a superseded generation stops being current") {
  cotest::Fixture fixture("adversarial-stale-generation", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;

  IngestRecord stale_capability = cotest::capability(1, epoch, SubjectKind::Pump, "pump.p1",
                                                     Quantity::flow(40'000'000), 0,
                                                     fixture.clock.now_ms());
  // Stated against generation 99, which is not the adopted generation.
  stale_capability.observation.generation = GenerationId(99);
  CO_REQUIRE_OK(fixture.engine.ingest({stale_capability}));
  CO_REQUIRE_OK(fixture.engine.ingest({cotest::equipment_state(2, epoch, SubjectKind::Pump, "pump.p1",
                                                                LifecycleState::Running,
                                                                fixture.clock.now_ms())}));

  QueryFilter filter;
  filter.loop = Maybe<LoopId>::of(LoopId(StrongId::from_validated("loop.primary")));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Reserve, filter);
  CO_REQUIRE(!report.reserve.scopes.empty());
  for (const ReserveObservation& scope : report.reserve.scopes) {
    // The capability statement describes a plant this runtime no longer holds,
    // so it supports no reserve figure and is reported as an indeterminacy
    // rather than quietly ignored.
    CO_CHECK(!scope.evidenced.value.has_value());
  }
  std::string reasons;
  for (const ReserveObservation& scope : report.reserve.scopes) {
    for (const Indeterminacy& indeterminacy : scope.indeterminacies) {
      reasons += indeterminacy.reason;
      reasons += ' ';
    }
  }
  if (reasons.find("capability_not_current") == std::string::npos) {
    cotest::record_failure("expected capability_not_current, saw: " +
                               (reasons.empty() ? std::string("(no indeterminacies)") : reasons),
                           __FILE__, __LINE__);
  }
}

CO_TEST("flow with no differential pressure is reported as a contradiction") {
  cotest::Fixture fixture("adversarial-flow-no-dp", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::zone_measurement(1, epoch, "zone.a", "flow", "s.flow", Quantity::flow(30'000'000),
                                fixture.clock.now_ms())}));
  QueryFilter filter;
  filter.zone = Maybe<ZoneId>::of(ZoneId(StrongId::from_validated("zone.a")));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Divergence, filter);
  const DivergenceFinding& finding = cotest::divergence_for(report.divergence, "dp.zone.a");
  CO_CHECK(finding.state == DivergenceState::Contradictory);
  CO_CHECK(has_contradiction(finding, ContradictionKind::FlowWithoutPressure));
}

CO_TEST("differential pressure with no flow is reported as a contradiction") {
  cotest::Fixture fixture("adversarial-dp-no-flow", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::zone_measurement(1, epoch, "zone.a", "diff_pressure", "s.dp",
                                Quantity::pressure(120'000), fixture.clock.now_ms())}));
  QueryFilter filter;
  filter.zone = Maybe<ZoneId>::of(ZoneId(StrongId::from_validated("zone.a")));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Divergence, filter);
  const DivergenceFinding& finding = cotest::divergence_for(report.divergence, "dp.zone.a");
  CO_CHECK(has_contradiction(finding, ContradictionKind::PressureWithoutFlow));
}

CO_TEST("two current sensors that disagree are reported as conflicting, not averaged") {
  cotest::Fixture fixture("adversarial-conflict", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::zone_measurement(1, epoch, "zone.a", "flow", "s.flow.one", Quantity::flow(30'000'000),
                                fixture.clock.now_ms()),
       cotest::zone_measurement(2, epoch, "zone.a", "flow", "s.flow.two", Quantity::flow(5'000'000),
                                fixture.clock.now_ms())}));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Delivery);
  const DeliveryObservation& zone_a = cotest::delivery_for(report.delivery, "zone.a");
  CO_CHECK(zone_a.flow.freshness == Freshness::Conflicting);
  CO_CHECK(!zone_a.flow.value.has_value());

  const ObservationReport coverage = cotest::query(fixture.engine, QueryKind::Coverage);
  std::string gaps;
  for (const CoverageGap& gap : coverage.coverage.gaps) {
    gaps += render_subject(gap.subject);
    gaps += '=';
    gaps += gap.reason;
    gaps += ' ';
  }
  if (gaps.find("conflicting") == std::string::npos) {
    cotest::record_failure("expected a conflicting coverage gap, saw: " +
                               (gaps.empty() ? std::string("(no gaps)") : gaps),
                           __FILE__, __LINE__);
  }
}

CO_TEST("a failure with residual delivery is reported as residual, not confirmed") {
  cotest::Fixture fixture("adversarial-residual", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::capability(1, epoch, SubjectKind::Pump, "pump.p1", Quantity::flow(40'000'000), 0,
                          fixture.clock.now_ms()),
       cotest::equipment_state(2, epoch, SubjectKind::Pump, "pump.p1", LifecycleState::Faulted,
                               fixture.clock.now_ms()),
       cotest::zone_measurement(3, epoch, "zone.a", "flow", "s.flow", Quantity::flow(12'000'000),
                                fixture.clock.now_ms()),
       cotest::failure(4, epoch, SubjectKind::Pump, "pump.p1", FailureKind::PumpStopped,
                       FailureImpact::NoDelivery, fixture.clock.now_ms())}));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Failures);
  CO_REQUIRE_EQ(report.failures.failures.size(), 1u);
  CO_CHECK(report.failures.failures[0].effect == EffectState::Residual);
  CO_CHECK(report.failures.failures[0].declared_by_authority);
  CO_CHECK(report.failures.failures[0].element_resolved);
}

CO_TEST("a failure on unknown equipment is reported unresolved rather than attached elsewhere") {
  cotest::Fixture fixture("adversarial-failure-unknown", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::failure(1, epoch, SubjectKind::Pump, "pump.ghost", FailureKind::PumpStopped,
                       FailureImpact::NoDelivery, fixture.clock.now_ms())}));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Failures);
  CO_REQUIRE_EQ(report.failures.failures.size(), 1u);
  CO_CHECK(!report.failures.failures[0].element_resolved);
  CO_CHECK(!report.failures.failures[0].element.has_value());
  CO_CHECK(report.failures.failures[0].effect == EffectState::Indeterminate);
  bool explains = false;
  for (const Indeterminacy& indeterminacy : report.failures.failures[0].indeterminacies) {
    if (indeterminacy.reason == "failure_element_absent") {
      explains = true;
    }
  }
  CO_CHECK(explains);
}

CO_TEST("a constraint whose element is absent is unattributed, never guessed") {
  cotest::Fixture fixture("adversarial-constraint-unattributed", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::constraint(1, epoch, SubjectKind::Pump, "pump.p1", ConstraintKind::FlowLimit,
                          ConstraintDirection::Maximum, Quantity::flow(5'000'000), "pump.ghost",
                          fixture.clock.now_ms()),
       cotest::zone_measurement(2, epoch, "zone.a", "flow", "s.flow", Quantity::flow(30'000'000),
                                fixture.clock.now_ms())}));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Constraints);
  CO_REQUIRE_EQ(report.constraints.constraints.size(), 1u);
  const ConstraintAttribution& attribution = report.constraints.constraints[0];
  CO_CHECK(attribution.attribution != AttributionState::Direct);
  bool explains = false;
  for (const Indeterminacy& indeterminacy : attribution.indeterminacies) {
    if (indeterminacy.reason == "constraint_element_absent") {
      explains = true;
    }
  }
  CO_CHECK(explains);
  CO_CHECK_EQ(report.constraints.unattributed_count, 1u);
}

CO_TEST("a constraint stated in the wrong dimension is refused, never coerced") {
  cotest::Fixture fixture("adversarial-constraint-dimension", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::constraint(1, epoch, SubjectKind::Pump, "pump.p1", ConstraintKind::FlowLimit,
                          ConstraintDirection::Maximum, Quantity::pressure(5000), "pump.p1",
                          fixture.clock.now_ms())}));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Constraints);
  CO_REQUIRE_EQ(report.constraints.constraints.size(), 1u);
  CO_CHECK(report.constraints.constraints[0].attribution == AttributionState::Unknown);
  CO_CHECK(report.constraints.constraints[0].indeterminacies[0].reason ==
           std::string("constraint_dimension_mismatch"));
}

CO_TEST("a reserve claim that assumes absent equipment keeps its assumption visible") {
  cotest::Fixture fixture("adversarial-reserve-assumption", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::reserve_claim(1, epoch, SubjectKind::Loop, "loop.primary", Quantity::flow(20'000'000),
                             {"pump.ghost"}, fixture.clock.now_ms())}));
  QueryFilter filter;
  filter.loop = Maybe<LoopId>::of(LoopId(StrongId::from_validated("loop.primary")));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Reserve, filter);
  CO_REQUIRE(!report.reserve.scopes.empty());
  bool found_unverified = false;
  for (const ReserveObservation& scope : report.reserve.scopes) {
    for (const ReserveContribution& contribution : scope.contributions) {
      if (!contribution.unverified_assumptions.empty()) {
        found_unverified = true;
      }
    }
  }
  CO_CHECK(found_unverified);
}

CO_TEST("duplicate records in one batch are applied once and reported") {
  cotest::Fixture fixture("adversarial-duplicates", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  const IngestRecord record = cotest::zone_measurement(7, epoch, "zone.a", "flow", "s.flow",
                                                       Quantity::flow(30'000'000),
                                                       fixture.clock.now_ms());
  const auto report = fixture.engine.ingest({record, record, record});
  CO_REQUIRE_OK(report);
  CO_CHECK_EQ(report.value().patch.records_applied, 1u);
  CO_CHECK_EQ(report.value().duplicates.size(), 2u);
  CO_CHECK_EQ(fixture.engine.image().value().evidence.size(), 1u);
}

CO_TEST("numeric extremes do not produce a wrapped or invented answer") {
  cotest::Fixture fixture("adversarial-extremes", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;

  // A flow at the representable maximum with a maximum temperature difference:
  // the heat removal is not representable, and the answer says so.
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::zone_measurement(1, epoch, "zone.a", "flow", "s.flow",
                                Quantity::flow(std::numeric_limits<std::int64_t>::max()),
                                fixture.clock.now_ms()),
       cotest::zone_measurement(2, epoch, "zone.a", "supply_temp", "s.supply",
                                Quantity::temperature(0), fixture.clock.now_ms()),
       cotest::zone_measurement(3, epoch, "zone.a", "return_temp", "s.return",
                                Quantity::temperature(std::numeric_limits<std::int64_t>::max()),
                                fixture.clock.now_ms())}));
  const ObservationReport extreme = cotest::query(fixture.engine, QueryKind::Delivery);
  const DeliveryObservation& zone_a = cotest::delivery_for(extreme.delivery, "zone.a");
  if (zone_a.heat_removal.value.has_value() || zone_a.heat_removal.freshness != Freshness::Unknown) {
    cotest::record_failure(
        std::string("expected no heat removal, saw ") +
            (zone_a.heat_removal.value.has_value()
                 ? render_quantity(zone_a.heat_removal.value.value())
                 : std::string("no value")) +
            " with freshness " + std::string(to_token(zone_a.heat_removal.freshness)),
        __FILE__, __LINE__);
  }

  // A negative flow is a physical impossibility and is reported as one.
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::zone_measurement(4, epoch, "zone.b", "flow", "s.flow.b",
                                Quantity::flow(-1'000'000), fixture.clock.now_ms())}));
  QueryFilter filter;
  filter.zone = Maybe<ZoneId>::of(ZoneId(StrongId::from_validated("zone.b")));
  const ObservationReport negative = cotest::query(fixture.engine, QueryKind::Divergence, filter);
  const DivergenceFinding& finding = cotest::divergence_for(negative.divergence, "dp.zone.b");
  CO_CHECK(has_contradiction(finding, ContradictionKind::NegativeFlow));
}

CO_TEST("a temperature pair with the wrong sign is contradicted, not treated as cooling") {
  cotest::Fixture fixture("adversarial-inverted", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::zone_measurement(1, epoch, "zone.a", "flow", "s.flow", Quantity::flow(30'000'000),
                                fixture.clock.now_ms()),
       cotest::zone_measurement(2, epoch, "zone.a", "supply_temp", "s.supply",
                                Quantity::temperature(30'000), fixture.clock.now_ms()),
       cotest::zone_measurement(3, epoch, "zone.a", "return_temp", "s.return",
                                Quantity::temperature(18'000), fixture.clock.now_ms()),
       cotest::zone_measurement(4, epoch, "zone.a", "diff_pressure", "s.dp",
                                Quantity::pressure(90'000), fixture.clock.now_ms())}));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Delivery);
  const DeliveryObservation& zone_a = cotest::delivery_for(report.delivery, "zone.a");
  CO_REQUIRE(zone_a.heat_removal.value.has_value());
  CO_CHECK(zone_a.heat_removal.value.value().value < 0);
  CO_CHECK(zone_a.temperature_difference.value.has_value());
  CO_CHECK(zone_a.temperature_difference.value.value().value < 0);

  QueryFilter filter;
  filter.zone = Maybe<ZoneId>::of(ZoneId(StrongId::from_validated("zone.a")));
  const ObservationReport divergence = cotest::query(fixture.engine, QueryKind::Divergence, filter);
  const DivergenceFinding& finding = cotest::divergence_for(divergence.divergence, "dp.zone.a");
  CO_CHECK(has_contradiction(finding, ContradictionKind::SupplyWarmerThanReturn));
  CO_CHECK(has_contradiction(finding, ContradictionKind::NotCooling));
}

CO_TEST("malformed input is refused with a reason and leaves the state untouched") {
  cotest::Fixture fixture("adversarial-malformed", false);
  cotest::open_fixture(fixture);
  const Revision before = fixture.engine.image().value().revision;

  CO_CHECK_ERR(parse_records("record add_observation\nobservation measurement\n"), Code::MalformedInput);
  CO_CHECK_ERR(parse_records("record adopt_structure\ngeneration notanumber\n"), Code::MalformedInput);
  CO_CHECK_ERR(parse_records("record add_observation\nobservation measurement\nsubject loop:x\n"
                             "sensor s\nvalue 1cubits\n"),
               Code::MalformedInput);
  // A filter with no value is refused: an empty identity is not a zone.
  CO_CHECK_ERR(parse_request("query divergence\nzone\n"), Code::InvalidArgument);
  CO_CHECK_ERR(parse_request("query divergence\nnonsense 1\n"), Code::UnknownToken);

  CO_CHECK(fixture.engine.image().value().revision == before);
}

CO_TEST("a measurement reported good with no dimension is refused") {
  cotest::Fixture fixture("adversarial-no-dimension", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  IngestRecord record = cotest::zone_measurement(1, epoch, "zone.a", "flow", "s.flow",
                                                 Quantity::flow(1000), fixture.clock.now_ms());
  record.observation.measured = Quantity{};
  const auto report = fixture.engine.ingest({record});
  CO_REQUIRE_OK(report);
  CO_CHECK_EQ(report.value().rejections.size(), 1u);
  CO_CHECK_EQ(report.value().rejections[0].reason, std::string("measurement_without_dimension"));
}

CO_TEST("a constraint against a measured value reports the margin with its sign") {
  cotest::Fixture fixture("adversarial-margin", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::constraint(1, epoch, SubjectKind::Pump, "pump.p1", ConstraintKind::FlowLimit,
                          ConstraintDirection::Maximum, Quantity::flow(20'000'000), "pump.p1",
                          fixture.clock.now_ms()),
       cotest::equipment_state(2, epoch, SubjectKind::Pump, "pump.p1", LifecycleState::Running,
                               fixture.clock.now_ms()),
       cotest::zone_measurement(3, epoch, "zone.a", "flow", "s.flow", Quantity::flow(30'000'000),
                                fixture.clock.now_ms())}));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Constraints);
  CO_REQUIRE_EQ(report.constraints.constraints.size(), 1u);
  const ConstraintAttribution& attribution = report.constraints.constraints[0];
  CO_CHECK(attribution.attribution == AttributionState::Direct);
  CO_REQUIRE(attribution.observed_at_point.value.has_value());
  CO_CHECK_EQ(attribution.observed_at_point.value.value().value, 30'000'000);
  CO_REQUIRE(attribution.margin.value.has_value());
  // A maximum of 20 L/s against a measured 30 L/s is a violation of 10 L/s.
  CO_CHECK_EQ(attribution.margin.value.value().value, 10'000'000);
}

CO_TEST("a minimum constraint reports a violation with the opposite sign") {
  cotest::Fixture fixture("adversarial-margin-min", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::constraint(1, epoch, SubjectKind::Pump, "pump.p1", ConstraintKind::FlowLimit,
                          ConstraintDirection::Minimum, Quantity::flow(20'000'000), "pump.p1",
                          fixture.clock.now_ms()),
       cotest::equipment_state(2, epoch, SubjectKind::Pump, "pump.p1", LifecycleState::Running,
                               fixture.clock.now_ms()),
       cotest::zone_measurement(3, epoch, "zone.a", "flow", "s.flow", Quantity::flow(5'000'000),
                                fixture.clock.now_ms())}));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Constraints);
  CO_REQUIRE_EQ(report.constraints.constraints.size(), 1u);
  CO_REQUIRE(report.constraints.constraints[0].margin.value.has_value());
  // A minimum of 20 L/s against a measured 5 L/s is a violation of 15 L/s.
  CO_CHECK_EQ(report.constraints.constraints[0].margin.value.value().value, 15'000'000);
}

CO_TEST("equipment reported stopped while flow is measured is a contradiction") {
  cotest::Fixture fixture("adversarial-delivery-while-stopped", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::equipment_state(1, epoch, SubjectKind::Crah, "crah.h1", LifecycleState::Off,
                               fixture.clock.now_ms()),
       cotest::zone_measurement(2, epoch, "zone.a", "flow", "s.flow", Quantity::flow(30'000'000),
                                fixture.clock.now_ms()),
       cotest::zone_measurement(3, epoch, "zone.a", "diff_pressure", "s.dp",
                                Quantity::pressure(90'000), fixture.clock.now_ms())}));
  QueryFilter filter;
  filter.zone = Maybe<ZoneId>::of(ZoneId(StrongId::from_validated("zone.a")));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Divergence, filter);
  const DivergenceFinding& finding = cotest::divergence_for(report.divergence, "dp.zone.a");
  std::string found;
  for (const Contradiction& contradiction : finding.contradictions) {
    found += to_token(contradiction.kind);
    found += ' ';
  }
  if (!has_contradiction(finding, ContradictionKind::DeliveryWhileStopped)) {
    cotest::record_failure("expected delivery_while_stopped, saw: " +
                               (found.empty() ? std::string("(none)") : found),
                           __FILE__, __LINE__);
  }
}

CO_TEST("a stale failure declaration is excluded by default and shown on request") {
  cotest::Fixture fixture("adversarial-stale-failure", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::failure(1, epoch, SubjectKind::Pump, "pump.p1", FailureKind::PumpStopped,
                       FailureImpact::NoDelivery, fixture.clock.now_ms())}));
  fixture.clock.advance(30 * 60 * 1000);

  const ObservationReport excluded = cotest::query(fixture.engine, QueryKind::Failures);
  CO_CHECK(excluded.failures.failures.empty());
  CO_CHECK_EQ(excluded.failures.excluded_count, 1u);

  QueryFilter filter;
  filter.include_stale = true;
  const ObservationReport included = cotest::query(fixture.engine, QueryKind::Failures, filter);
  CO_REQUIRE_EQ(included.failures.failures.size(), 1u);
  CO_CHECK(included.failures.failures[0].declared_freshness == Freshness::Stale);
  CO_CHECK(included.failures.failures[0].effect == EffectState::Indeterminate);
}