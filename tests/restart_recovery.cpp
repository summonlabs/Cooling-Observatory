// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Restart and recovery, in process and across processes. The questions are
// exact: what does a reopened store contain, what is the commit point, what
// does a process that dies mid-commit leave behind, and can restored evidence
// ever be mistaken for current evidence.

#include <cstdint>
#include <string>
#include <vector>

#include "child_process.hpp"
#include "support.hpp"
#include "test_harness.hpp"

using namespace dccp::cooling_observatory;

namespace {

}  // namespace

CO_TEST("a reopened store returns the state the previous incarnation committed") {
  cotest::Fixture fixture("restart-reopen", true);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  const std::string digest = fixture.engine.manifest().value().digest;
  const std::uint64_t revision = fixture.engine.image().value().revision.value();
  CO_REQUIRE_OK(fixture.engine.close());

  Engine restarted;
  cotest::FixedClock later(cotest::kStart + 3'600'000);
  CO_REQUIRE_OK(restarted.open(EngineOptions{fixture.directory.path(), {}, {}, {}, {}}, later));

  const auto image = restarted.image();
  CO_REQUIRE_OK(image);
  CO_CHECK_EQ(image.value().revision.value(), revision);
  CO_CHECK_EQ(image.value().evidence.size(), 12u);
  CO_CHECK_EQ(image.value().structure.components().size(), 6u);
  CO_CHECK_EQ(image.value().epoch.value(), epoch.value());
  CO_CHECK(restarted.manifest().value().digest == digest);
  CO_REQUIRE_OK(restarted.close());
}

CO_TEST("evidence restored from disk is recovered and never current") {
  cotest::Fixture fixture("restart-recovered", true);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  CO_REQUIRE_OK(fixture.engine.close());

  Engine restarted;
  cotest::FixedClock same_instant(cotest::kStart);
  CO_REQUIRE_OK(restarted.open(EngineOptions{fixture.directory.path(), {}, {}, {}, {}}, same_instant));

  const ObservationReport report = cotest::query(restarted, QueryKind::Delivery);
  CO_CHECK(report.recovered);
  for (const DeliveryObservation& observation : report.delivery.points) {
    CO_CHECK(observation.flow.freshness != Freshness::Fresh);
    CO_CHECK(!observation.flow.value.has_value());
  }
  // The measured values are still readable in history and in the image: being
  // recovered costs a value its currency, not its existence.
  const auto image = restarted.image();
  CO_REQUIRE_OK(image);
  CO_CHECK_EQ(image.value().evidence.size(), 12u);
  CO_CHECK(image.value().evidence[0].origin == EvidenceOrigin::Recovered);
  CO_REQUIRE_OK(restarted.close());
}

CO_TEST("new evidence after a restart is current again and the image stops being recovered") {
  cotest::Fixture fixture("restart-refresh", true);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  CO_REQUIRE_OK(fixture.engine.close());

  Engine restarted;
  cotest::FixedClock same_instant(cotest::kStart + 1000);
  CO_REQUIRE_OK(restarted.open(EngineOptions{fixture.directory.path(), {}, {}, {}, {}}, same_instant));
  CO_CHECK(restarted.image().value().recovered);

  // The same sensor that reported before the restart reports again, so this is a
  // new reading of one instrument rather than a second instrument appearing.
  const auto refreshed = restarted.ingest({cotest::zone_measurement(
      500, epoch, "zone.a", "flow", "sensor.a.flow", Quantity::flow(31'000'000),
      same_instant.now_ms())});
  CO_REQUIRE_OK(refreshed);
  CO_CHECK(refreshed.value().refreshed_after_recovery);
  CO_CHECK(!restarted.image().value().recovered);

  const ObservationReport report = cotest::query(restarted, QueryKind::Delivery);
  const DeliveryObservation& zone_a = cotest::delivery_for(report.delivery, "zone.a");
  CO_CHECK(zone_a.flow.freshness == Freshness::Fresh);
  CO_REQUIRE(zone_a.flow.value.has_value());
  CO_CHECK_EQ(zone_a.flow.value.value().value, 31'000'000);
  CO_REQUIRE_OK(restarted.close());
}

CO_TEST("a record sequence already committed before a restart is still a duplicate") {
  cotest::Fixture fixture("restart-idempotent", true);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  const std::vector<IngestRecord> records{cotest::zone_measurement(
      1, epoch, "zone.a", "zone.a.flow", "s.flow", Quantity::flow(30'000'000),
      fixture.clock.now_ms())};
  CO_REQUIRE_OK(fixture.engine.ingest(records));
  CO_REQUIRE_OK(fixture.engine.close());

  Engine restarted;
  CO_REQUIRE_OK(restarted.open(EngineOptions{fixture.directory.path(), {}, {}, {}, {}}, fixture.clock));
  const auto replay = restarted.ingest(records);
  CO_REQUIRE_OK(replay);
  CO_CHECK_EQ(replay.value().patch.records_applied, 0u);
  CO_CHECK_EQ(replay.value().duplicates.size(), 1u);
  CO_CHECK_EQ(restarted.image().value().evidence.size(), 1u);
  CO_REQUIRE_OK(restarted.close());
}

CO_TEST("the reopen proof runs in a separate process against the same directory") {
  cotest::Fixture fixture("restart-child", true);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  const std::string digest = fixture.engine.manifest().value().digest;
  CO_REQUIRE_OK(fixture.engine.close());

  CO_REQUIRE(!cotest::self_executable().empty());
  const cotest::ChildResult result =
      cotest::run_self({"--child-reopen", fixture.directory.path()});
  CO_REQUIRE(result.started);
  if (result.exit_code != 0) {
    cotest::record_failure("child reopen failed: " + result.output + result.error, __FILE__, __LINE__);
  }
  CO_CHECK_EQ(result.exit_code, 0);
  CO_CHECK(result.output.find("revision 2") != std::string::npos);
  CO_CHECK(result.output.find("evidence 12") != std::string::npos);
  CO_CHECK(result.output.find("recovered true") != std::string::npos);

  // The child's read did not change what the state says: a reopen is a read.
  Engine after;
  CO_REQUIRE_OK(after.open(EngineOptions{fixture.directory.path(), {}, {}, {}, {}}, fixture.clock));
  CO_CHECK(after.manifest().value().digest == digest);
  CO_REQUIRE_OK(after.close());
}

CO_TEST("a process that dies without closing leaves a state a later process can read") {
  cotest::Fixture fixture("restart-kill", true);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  CO_REQUIRE_OK(fixture.engine.close());

  // The writer below commits nothing: it opens the store, is killed while it
  // holds the writer lock, and the lock is released by the kernel when the
  // process ends. The state file itself was never mid-write, which is the
  // property the atomic replace exists to provide.
  CO_REQUIRE(!cotest::self_executable().empty());
  const cotest::ChildResult holder = cotest::run_self({"--child-lock-holder", fixture.directory.path()});
  CO_REQUIRE(holder.started);
  CO_CHECK_EQ(holder.exit_code, 0);
  CO_CHECK(holder.output.find("holding lock") != std::string::npos);

  // Now that the child has exited, the lock is free and the state is intact.
  Engine after;
  CO_REQUIRE_OK(after.open(EngineOptions{fixture.directory.path(), {}, {}, {}, {}}, fixture.clock));
  CO_CHECK_EQ(after.image().value().evidence.size(), 12u);
  CO_REQUIRE_OK(after.close());
}

CO_TEST("a killed writer leaves either the previous state or the new one, never a mixture") {
  cotest::Fixture fixture("restart-torn-commit", true);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  const std::uint64_t committed_revision = fixture.engine.image().value().revision.value();
  CO_REQUIRE_OK(fixture.engine.close());

  // Simulate a crash between writing the temporary file and replacing the live
  // name: the temporary file is present and the live file is untouched.
  const std::string live = fixture.directory.file("cooling-observatory.state");
  const std::string temporary = live + ".tmp";
  const std::string original = cotest::read_bytes(live);
  std::string partial = original.substr(0, original.size() / 3);
  cotest::write_bytes(temporary, partial);

  Engine after;
  CO_REQUIRE_OK(after.open(EngineOptions{fixture.directory.path(), {}, {}, {}, {}}, fixture.clock));
  CO_CHECK_EQ(after.image().value().revision.value(), committed_revision);
  CO_CHECK_EQ(after.image().value().evidence.size(), 12u);
  CO_REQUIRE_OK(after.close());

  // A commit after that residue still replaces the live name atomically.
  Engine again;
  CO_REQUIRE_OK(again.open(EngineOptions{fixture.directory.path(), {}, {}, {}, {}}, fixture.clock));
  const auto report = again.ingest({cotest::zone_measurement(
      900, epoch, "zone.a", "zone.a.flow", "s.flow", Quantity::flow(1), fixture.clock.now_ms())});
  CO_REQUIRE_OK(report);
  CO_CHECK_EQ(report.value().patch.revision.value(), committed_revision + 1);
  CO_REQUIRE_OK(again.close());
}

CO_TEST("an explicit reopen re-reads the file and re-marks the evidence recovered") {
  cotest::Fixture fixture("restart-explicit", true);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  CO_CHECK(!fixture.engine.image().value().recovered);

  CO_REQUIRE_OK(fixture.engine.reopen());
  // The image is a value copied out of the engine's lock, so it is held in a
  // local: iterating the contents of a temporary would read freed memory.
  const ObservationImage image = fixture.engine.image().value();
  CO_CHECK(image.recovered);
  for (const Observation& observation : image.evidence) {
    CO_CHECK(observation.origin == EvidenceOrigin::Recovered);
  }
  CO_CHECK_EQ(fixture.engine.stats().reopen_count, 1u);
}

CO_TEST("reopening an in-memory engine is refused with a reason") {
  cotest::Fixture fixture("restart-memory", false);
  cotest::open_fixture(fixture);
  const Status reopened = fixture.engine.reopen();
  CO_CHECK(!reopened.ok());
  CO_CHECK(reopened.code() == Code::LifecycleRefused);
  CO_CHECK(reopened.reason() == std::string("engine_not_durable"));
}

CO_TEST("the retired-sensor decision survives a restart") {
  cotest::Fixture fixture("restart-retirement", true);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::zone_measurement(1, epoch, "zone.a", "zone.a.flow", "s.flow", Quantity::flow(1000),
                                fixture.clock.now_ms())}));
  IngestRecord retire;
  retire.kind = RecordKind::RetireEvidence;
  retire.retire_sensor = SensorId(StrongId::from_validated("s.flow"));
  retire.retire_axis = EvidenceAxis::Delivery;
  CO_REQUIRE_OK(fixture.engine.ingest({retire}));
  CO_REQUIRE_OK(fixture.engine.close());

  Engine restarted;
  CO_REQUIRE_OK(restarted.open(EngineOptions{fixture.directory.path(), {}, {}, {}, {}}, fixture.clock));
  CO_CHECK_EQ(restarted.image().value().retired_sensors.size(), 1u);
  const ObservationReport report = cotest::query(restarted, QueryKind::Delivery);
  const DeliveryObservation& zone_a = cotest::delivery_for(report.delivery, "zone.a");
  CO_CHECK(!zone_a.flow.value.has_value());
  CO_REQUIRE_OK(restarted.close());
}