// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The engine as a whole: lifecycle, commit points, idempotency, refusals that
// do not advance the revision, and the determinism of an answer.

#include <string>
#include <vector>

#include "dccp/cooling_observatory/engine.hpp"
#include "dccp/cooling_observatory/textproto.hpp"
#include "support.hpp"
#include "test_harness.hpp"

using namespace dccp::cooling_observatory;

namespace {

}  // namespace

CO_TEST("an in-memory engine opens, answers and reports stats without a directory") {
  cotest::Fixture fixture("engine-memory", false);
  cotest::open_fixture(fixture);
  const Stats stats = fixture.engine.stats();
  CO_CHECK_EQ(stats.commits, 1u);
  CO_CHECK_EQ(stats.structure_generations, 1u);
  CO_CHECK_EQ(stats.revision.value(), 1u);
  CO_CHECK_EQ(stats.generation.value(), 1u);
  CO_CHECK(!fixture.engine.manifest().ok());  // no directory, so no manifest
}

CO_TEST("a durable engine writes its state and reports a manifest") {
  cotest::Fixture fixture("engine-durable", true);
  cotest::open_fixture(fixture);
  const auto manifest = fixture.engine.manifest();
  CO_REQUIRE_OK(manifest);
  CO_CHECK_EQ(manifest.value().revision.value(), 1u);
  CO_CHECK_EQ(manifest.value().generation.value(), 1u);
  CO_CHECK(!manifest.value().digest.empty());
  CO_CHECK(cotest::file_size(fixture.directory.file("cooling-observatory.state")) > 0);
}

CO_TEST("a second engine on the same directory is refused while the first is open") {
  cotest::Fixture first("engine-lock", true);
  cotest::open_fixture(first);
  Engine second;
  const Status status = second.open(EngineOptions{first.directory.path(), {}, {}, {}, {}}, first.clock);
  CO_CHECK(!status.ok());
  CO_CHECK(status.code() == Code::StoreBusy);
}

CO_TEST("the record sequence advances only on a commit that applied something") {
  cotest::Fixture fixture("engine-sequence", false);
  cotest::open_fixture(fixture);
  const auto before = fixture.engine.image();
  CO_REQUIRE_OK(before);
  const Revision before_revision = before.value().revision;

  // An empty batch is not a commit.
  const auto empty = fixture.engine.ingest({});
  CO_REQUIRE_OK(empty);
  CO_CHECK_EQ(empty.value().patch.records_applied, 0u);
  CO_CHECK_EQ(empty.value().patch.revision.value(), before_revision.value());

  // A batch that is entirely rejected is not a commit either.
  IngestRecord invalid;
  invalid.kind = RecordKind::AddObservation;
  invalid.observation.kind = ObservationKind::Measurement;
  invalid.observation.subject = SubjectRef(SubjectKind::Measurement, StrongId::from_validated("m"));
  invalid.observation.quality = MeasurementQuality::Good;
  // No record sequence: an observation without one cannot be made idempotent.
  const auto rejected = fixture.engine.ingest({invalid});
  CO_REQUIRE_OK(rejected);
  CO_CHECK_EQ(rejected.value().patch.records_applied, 0u);
  CO_CHECK_EQ(rejected.value().rejections.size(), 1u);
  CO_CHECK_EQ(rejected.value().rejections[0].reason, std::string("record_seq_required"));

  const auto after = fixture.engine.image();
  CO_REQUIRE_OK(after);
  CO_CHECK(after.value().revision == before_revision);
}

CO_TEST("replaying the same record sequence is reported as a duplicate, not applied twice") {
  cotest::Fixture fixture("engine-idempotent", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  const std::vector<IngestRecord> records{
      cotest::zone_flow_measurement(1, epoch, "zone.a", "sensor.a.flow", Quantity::flow(1000),
                                    fixture.clock.now_ms())};
  const auto first = fixture.engine.ingest(records);
  CO_REQUIRE_OK(first);
  CO_CHECK_EQ(first.value().patch.records_applied, 1u);
  CO_CHECK_EQ(first.value().duplicates.size(), 0u);

  const auto second = fixture.engine.ingest(records);
  CO_REQUIRE_OK(second);
  CO_CHECK_EQ(second.value().patch.records_applied, 0u);
  CO_CHECK_EQ(second.value().duplicates.size(), 1u);
  CO_CHECK_EQ(second.value().duplicates[0].value(), 1u);

  const auto image = fixture.engine.image();
  CO_REQUIRE_OK(image);
  CO_CHECK_EQ(image.value().evidence.size(), 1u);
}

CO_TEST("a later record from one sensor supersedes the earlier one") {
  cotest::Fixture fixture("engine-supersede", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest({cotest::zone_flow_measurement(
      1, epoch, "zone.a", "sensor.a.flow", Quantity::flow(1000), fixture.clock.now_ms())}));
  CO_REQUIRE_OK(fixture.engine.ingest({cotest::zone_flow_measurement(
      2, epoch, "zone.a", "sensor.a.flow", Quantity::flow(2000), fixture.clock.now_ms() + 1)}));

  const auto image = fixture.engine.image();
  CO_REQUIRE_OK(image);
  CO_CHECK_EQ(image.value().evidence.size(), 1u);
  CO_CHECK_EQ(image.value().evidence[0].measured.value, 2000);
}

CO_TEST("two sensors for one measurement coexist and are reduced together") {
  cotest::Fixture fixture("engine-two-sensors", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::zone_flow_measurement(1, epoch, "zone.a", "sensor.a.flow.1", Quantity::flow(1000),
                                     fixture.clock.now_ms()),
       cotest::zone_flow_measurement(2, epoch, "zone.a", "sensor.a.flow.2", Quantity::flow(1000),
                                     fixture.clock.now_ms())}));
  const auto image = fixture.engine.image();
  CO_REQUIRE_OK(image);
  CO_CHECK_EQ(image.value().evidence.size(), 2u);
}

CO_TEST("an operation on a closed engine is refused rather than performed") {
  cotest::Fixture fixture("engine-closed", true);
  cotest::open_fixture(fixture);
  CO_REQUIRE_OK(fixture.engine.close());
  CO_CHECK(!fixture.engine.is_open());
  const auto after = fixture.engine.ingest({});
  CO_CHECK_ERR(after, Code::StoreClosed);
  const auto query_result = fixture.engine.observe(ObserveRequest{});
  CO_CHECK_ERR(query_result, Code::StoreClosed);
  CO_CHECK(!fixture.engine.image().ok());
  CO_CHECK(!fixture.engine.close().ok());
}

CO_TEST("reopening an engine that is already open is refused") {
  cotest::Fixture fixture("engine-double-open", true);
  cotest::open_fixture(fixture);
  const Status again =
      fixture.engine.open(EngineOptions{fixture.directory.path(), {}, {}, {}, {}}, fixture.clock);
  CO_CHECK(!again.ok());
  CO_CHECK(again.code() == Code::LifecycleRefused);
}

CO_TEST("a structure generation can be replaced and the revision advances once") {
  cotest::Fixture fixture("engine-generation", false);
  cotest::open_fixture(fixture);
  const Revision before = fixture.engine.image().value().revision;
  PlantModel replacement = cotest::synthetic_plant();
  replacement.add_zone(ThermalZone{ZoneId(StrongId::from_validated("zone.c")),
                                   FacilityId(StrongId::from_validated("dc1")), "extra", {}, {}});
  replacement.reindex();
  const auto report = fixture.engine.ingest({make_structure(GenerationId(2), replacement, "rev_2")});
  CO_REQUIRE_OK(report);
  CO_CHECK(report.value().patch.structure_replaced);
  CO_CHECK_EQ(report.value().patch.generation.value(), 2u);
  CO_CHECK_EQ(report.value().patch.revision.value(), before.value() + 1);
  CO_CHECK_EQ(fixture.engine.image().value().structure.zones().size(), 3u);
}

CO_TEST("delivery points are registered and forgotten without touching the structure") {
  cotest::Fixture fixture("engine-points", false);
  cotest::open_fixture(fixture);
  IngestRecord registration;
  registration.kind = RecordKind::RegisterDeliveryPoint;
  registration.delivery_point.id = StrongId::from_validated("dp.zone.a");
  registration.delivery_point.zone = ZoneId(StrongId::from_validated("zone.a"));
  registration.delivery_point.loop = LoopId(StrongId::from_validated("loop.secondary"));
  registration.delivery_point.medium = CoolantMedium::Liquid;
  registration.delivery_point.flow_measurement = zone_flow(ZoneId(StrongId::from_validated("zone.a")));
  CO_REQUIRE_OK(fixture.engine.ingest({registration}));
  CO_CHECK_EQ(fixture.engine.image().value().delivery_points.size(), 1u);

  IngestRecord forget;
  forget.kind = RecordKind::ForgetDeliveryPoint;
  forget.forget_point = StrongId::from_validated("dp.zone.a");
  CO_REQUIRE_OK(fixture.engine.ingest({forget}));
  CO_CHECK_EQ(fixture.engine.image().value().delivery_points.size(), 0u);
  // The zone still implies a delivery point, so the delivery report is not empty.
  const ObservationReport delivery = cotest::query(fixture.engine, QueryKind::Delivery);
  CO_CHECK(!delivery.delivery.points.empty() || !delivery.delivery.unevidenced_points.empty());
}

CO_TEST("retiring a sensor withdraws its evidence without deleting the record") {
  cotest::Fixture fixture("engine-retire", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::zone_measurement(1, epoch, "zone.a", "flow", "sensor.a.flow", Quantity::flow(1000),
                                fixture.clock.now_ms()),
       cotest::zone_measurement(2, epoch, "zone.b", "flow", "sensor.b.flow", Quantity::flow(2000),
                                fixture.clock.now_ms())}));

  IngestRecord retire;
  retire.kind = RecordKind::RetireEvidence;
  retire.retire_sensor = SensorId(StrongId::from_validated("sensor.a.flow"));
  retire.retire_axis = EvidenceAxis::Delivery;
  const auto report = fixture.engine.ingest({retire});
  CO_REQUIRE_OK(report);
  CO_CHECK_EQ(report.value().patch.evidence_retired, 1u);

  const ObservationReport delivery = cotest::query(fixture.engine, QueryKind::Delivery);
  const DeliveryObservation& zone_a = cotest::delivery_for(delivery.delivery, "zone.a");
  CO_CHECK(!zone_a.flow.value.has_value());
  CO_CHECK(zone_a.flow.freshness == Freshness::Unknown);
}

CO_TEST("a policy record changes what counts as current") {
  cotest::Fixture fixture("engine-policy", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::zone_measurement(1, epoch, "zone.a", "flow", "sensor.a.flow", Quantity::flow(1000),
                                fixture.clock.now_ms())}));

  IngestRecord policy;
  policy.kind = RecordKind::ParsePolicy;
  policy.freshness_policy.delivery_max_age_ms = 1;
  CO_REQUIRE_OK(fixture.engine.ingest({policy}));

  fixture.clock.advance(10);
  const ObservationReport delivery = cotest::query(fixture.engine, QueryKind::Delivery);
  const DeliveryObservation& zone_a = cotest::delivery_for(delivery.delivery, "zone.a");
  CO_CHECK(zone_a.flow.freshness == Freshness::Stale);
}

CO_TEST("a thermal policy record changes the computed heat removal") {
  cotest::Fixture fixture("engine-thermal", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));

  const ObservationReport before = cotest::query(fixture.engine, QueryKind::Delivery);
  const DeliveryObservation& before_zone = cotest::delivery_for(before.delivery, "zone.a");
  CO_REQUIRE(before_zone.heat_removal.value.has_value());
  // 30 L/s across a 6 K drop with a volumetric heat capacity of 4.18 J/(L*K) is
  // 30 * 6 * 4.18 = 752.4 W, reported at the nearest watt.
  CO_CHECK_EQ(before_zone.heat_removal.value.value().value, 752);

  IngestRecord thermal;
  thermal.kind = RecordKind::ThermalPolicyUpdate;
  thermal.thermal_policy.coolant_heat_capacity_uj_per_l_k = 2'090'000;
  CO_REQUIRE_OK(fixture.engine.ingest({thermal}));

  const ObservationReport after = cotest::query(fixture.engine, QueryKind::Delivery);
  const DeliveryObservation& after_zone = cotest::delivery_for(after.delivery, "zone.a");
  CO_REQUIRE(after_zone.heat_removal.value.has_value());
  // The computed removal is exactly proportional to the declared heat capacity,
  // which is what makes the figure auditable rather than mysterious.
  const std::int64_t expected = (before_zone.heat_removal.value.value().value * 2'090'000) / 4'180'000;
  CO_CHECK_EQ(after_zone.heat_removal.value.value().value, expected);
}

CO_TEST("a record that fills fields of another kind is refused") {
  cotest::Fixture fixture("engine-shape", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;

  IngestRecord measurement_with_state =
      cotest::zone_flow_measurement(1, epoch, "zone.a", "sensor.a.flow", Quantity::flow(1000),
                                    fixture.clock.now_ms());
  measurement_with_state.observation.state = LifecycleState::Running;
  const auto report = fixture.engine.ingest({measurement_with_state});
  CO_REQUIRE_OK(report);
  CO_CHECK_EQ(report.value().rejections.size(), 1u);
  CO_CHECK_EQ(report.value().rejections[0].reason, std::string("measurement_carries_state"));

  IngestRecord negative_capability = cotest::capability(2, epoch, SubjectKind::Pump, "pump.p1",
                                                        Quantity::flow(-1), 0, fixture.clock.now_ms());
  const auto second = fixture.engine.ingest({negative_capability});
  CO_REQUIRE_OK(second);
  CO_CHECK_EQ(second.value().rejections.size(), 1u);
  CO_CHECK_EQ(second.value().rejections[0].reason, std::string("negative_capability"));

  IngestRecord excessive_derate = cotest::capability(3, epoch, SubjectKind::Pump, "pump.p1",
                                                     Quantity::flow(1000), 2'000'000,
                                                     fixture.clock.now_ms());
  const auto third = fixture.engine.ingest({excessive_derate});
  CO_REQUIRE_OK(third);
  CO_CHECK_EQ(third.value().rejections.size(), 1u);
  CO_CHECK_EQ(third.value().rejections[0].reason, std::string("derate_above_full"));
}

CO_TEST("a batch that exceeds the record limit is refused whole") {
  cotest::Fixture fixture("engine-batch-limit", false);
  EngineOptions options;
  options.store.limits.max_records = 3;
  Engine engine;
  CO_REQUIRE_OK(engine.open(options, fixture.clock));
  const EpochId epoch = engine.image().value().epoch;
  std::vector<IngestRecord> records;
  for (std::uint64_t i = 1; i <= 4; ++i) {
    records.push_back(cotest::zone_flow_measurement(i, epoch, "zone.a", "sensor.a.flow",
                                                    Quantity::flow(1000), fixture.clock.now_ms()));
  }
  const auto report = engine.ingest(records);
  CO_CHECK_ERR(report, Code::LimitExceeded);
  CO_CHECK_EQ(engine.image().value().revision.value(), 0u);
  CO_REQUIRE_OK(engine.close());
}

CO_TEST("an answer cites the revision, epoch and generation it was computed against") {
  cotest::Fixture fixture("engine-provenance", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Delivery);
  CO_CHECK_EQ(report.revision.value(), fixture.engine.image().value().revision.value());
  CO_CHECK(report.epoch == epoch);
  CO_CHECK_EQ(report.generation.value(), 1u);
  CO_CHECK_EQ(report.as_of_ms, fixture.clock.now_ms());

  const DeliveryObservation& zone_a = cotest::delivery_for(report.delivery, "zone.a");
  CO_REQUIRE(!zone_a.flow.evidence.empty());
  CO_CHECK_EQ(zone_a.flow.evidence[0].epoch.value(), epoch.value());
  CO_CHECK_EQ(zone_a.flow.evidence[0].revision.value(), report.revision.value());
  CO_CHECK(!zone_a.flow.evidence[0].authority.authority.empty());
}

CO_TEST("the history query reports what was committed, in order") {
  cotest::Fixture fixture("engine-history", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  const ObservationReport history = cotest::query(fixture.engine, QueryKind::History);
  CO_CHECK(history.sections.history);
  CO_CHECK_EQ(history.history.size(), 12u);
  for (std::size_t i = 1; i < history.history.size(); ++i) {
    CO_CHECK(!(history.history[i].revision < history.history[i - 1].revision));
  }
}

CO_TEST("every query kind answers against one committed revision and sets its section flag") {
  cotest::Fixture fixture("engine-queries", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));

  const QueryKind kinds[] = {QueryKind::Image,  QueryKind::Delivery,     QueryKind::Constraints,
                             QueryKind::Failures, QueryKind::Reserve,   QueryKind::Divergence,
                             QueryKind::Coverage, QueryKind::History};
  for (const QueryKind kind : kinds) {
    const ObservationReport report = cotest::query(fixture.engine, kind);
    CO_CHECK(report.kind == kind);
    CO_CHECK_EQ(report.revision.value(), fixture.engine.image().value().revision.value());
    const int set_sections = static_cast<int>(report.sections.image) +
                             static_cast<int>(report.sections.delivery) +
                             static_cast<int>(report.sections.constraints) +
                             static_cast<int>(report.sections.failures) +
                             static_cast<int>(report.sections.reserve) +
                             static_cast<int>(report.sections.divergence) +
                             static_cast<int>(report.sections.coverage) +
                             static_cast<int>(report.sections.dependencies) +
                             static_cast<int>(report.sections.history);
    CO_CHECK_EQ(set_sections, 1);
  }
}

CO_TEST("a dependency query without a root reports why instead of guessing one") {
  cotest::Fixture fixture("engine-deps-root", false);
  cotest::open_fixture(fixture);
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Dependencies);
  CO_CHECK(!report.indeterminacies.empty());
  CO_CHECK_EQ(report.indeterminacies[0].reason, std::string("traversal_root_required"));
}

CO_TEST("a dependency query for an unknown root reports the reason") {
  cotest::Fixture fixture("engine-deps-unknown", false);
  cotest::open_fixture(fixture);
  QueryFilter filter;
  filter.root = Maybe<SubjectRef>::of(
      SubjectRef(SubjectKind::Pump, StrongId::from_validated("pump.missing")));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Dependencies, filter);
  CO_CHECK(!report.indeterminacies.empty());
  CO_CHECK_EQ(report.indeterminacies[0].reason, std::string("traversal_root_unknown"));
}

CO_TEST("stats account for commits, rejections, duplicates and queries") {
  cotest::Fixture fixture("engine-stats", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  (void)cotest::query(fixture.engine, QueryKind::Delivery);
  const Stats stats = fixture.engine.stats();
  // The second batch carried exactly the same record sequences, so it was
  // recognised as a replay and committed nothing: a commit is a revision
  // advance, and a replay is not one.
  CO_CHECK_EQ(stats.commits, 2u);  // the structure adoption and the first batch
  CO_CHECK_EQ(stats.queries, 1u);
  CO_CHECK_EQ(stats.duplicates_rejected, 12u);
  CO_CHECK(stats.render().find("commits 2") != std::string::npos);
}

CO_TEST("two engines fed the same records in one order produce the same digest") {
  cotest::Fixture first("engine-determinism-a", true);
  cotest::open_fixture(first);
  cotest::Fixture second("engine-determinism-b", true);
  cotest::open_fixture(second);
  const EpochId epoch = first.engine.image().value().epoch;
  CO_REQUIRE_OK(first.engine.ingest(cotest::baseline_records(epoch, first.clock.now_ms())));
  CO_REQUIRE_OK(second.engine.ingest(cotest::baseline_records(epoch, second.clock.now_ms())));

  const ObservationReport first_delivery = cotest::query(first.engine, QueryKind::Delivery);
  const ObservationReport second_delivery = cotest::query(second.engine, QueryKind::Delivery);
  CO_CHECK_EQ(render_text(first_delivery), render_text(second_delivery));
  CO_CHECK_EQ(render_json(first_delivery), render_json(second_delivery));
  CO_CHECK_EQ(first.engine.manifest().value().digest, second.engine.manifest().value().digest);
}

CO_TEST("rendering the same report twice produces the same bytes") {
  cotest::Fixture fixture("engine-render-stable", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Coverage);
  CO_CHECK_EQ(render_text(report), render_text(report));
  CO_CHECK_EQ(render_json(report), render_json(report));
  CO_CHECK(render_json(report).find("\"query\":\"coverage\"") != std::string::npos);
}

CO_TEST("a delivery point filter restricts the answer without changing the revision") {
  cotest::Fixture fixture("engine-filter", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  QueryFilter filter;
  filter.zone = Maybe<ZoneId>::of(ZoneId(StrongId::from_validated("zone.a")));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Delivery, filter);
  CO_CHECK_EQ(report.delivery.points.size(), 1u);
  CO_CHECK(report.delivery.points[0].point.zone.view() == "zone.a");
}