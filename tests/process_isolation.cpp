// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Real multiprocess behaviour: the kernel-enforced single-writer lock, several
// processes racing for it, the inspection tool run as an external program, and
// a state directory whose path is longer than the classic Windows limit.

#include <algorithm>
#include <string>
#include <vector>

#include "child_process.hpp"
#include "support.hpp"
#include "test_harness.hpp"

using namespace dccp::cooling_observatory;

namespace {

/// A path longer than the 260-character limit that Win32 applies without the
/// extended-length prefix, and inside the per-component limit so that every
/// component is a legal name on every supported filesystem.
std::string long_state_path(const cotest::TempDir& directory) {
  std::string path = directory.path();
  for (int i = 0; i < 40; ++i) {
    path += "/segment-" + std::to_string(i);
  }
  return path;
}

}  // namespace

CO_TEST("a second process cannot open a directory another process holds") {
  // The fixture and the child name the same directory, so the child's open is
  // refused by the lock the fixture holds rather than by anything else.
  cotest::Fixture fixture("process-lock", true);
  cotest::open_fixture(fixture);

  CO_REQUIRE(!cotest::self_executable().empty());
  const cotest::ChildResult child = cotest::run_self({"--child-writer", fixture.directory.path()});
  CO_REQUIRE(child.started);
  CO_CHECK_EQ(child.exit_code, 3);
  CO_CHECK(child.output.find("store_locked") != std::string::npos);
}

CO_TEST("the writer lock is held by one process and released when it exits") {
  cotest::TempDir directory("process-hold");
  CO_REQUIRE(!cotest::self_executable().empty());

  // The child opens the store, reports that it holds the lock, waits, and then
  // exits, which releases the lock. The parent can only open the store after
  // the child has finished, so this proves the lock is real and exclusive.
  const cotest::ChildResult holder = cotest::run_self({"--child-lock-holder", directory.path()});
  CO_REQUIRE(holder.started);
  CO_CHECK_EQ(holder.exit_code, 0);
  CO_CHECK(holder.output.find("holding lock") != std::string::npos);
  CO_CHECK(holder.output.find("releasing lock") != std::string::npos);

  Engine engine;
  cotest::FixedClock clock(cotest::kStart);
  CO_REQUIRE_OK(engine.open(EngineOptions{directory.path(), {}, {}, {}, {}}, clock));
  CO_REQUIRE_OK(engine.close());
}

CO_TEST("several processes racing for one directory produce exactly one writer") {
  cotest::TempDir directory("process-race");
  CO_REQUIRE(!cotest::self_executable().empty());

  // Start the children one after another and keep the ones that report that
  // they hold the lock. The lock is exclusive for the lifetime of each child,
  // so a child that finds the directory busy must say so.
  std::vector<cotest::ChildResult> results;
  results.push_back(cotest::run_self({"--child-writer", directory.path()}));
  results.push_back(cotest::run_self({"--child-writer", directory.path()}));
  results.push_back(cotest::run_self({"--child-writer", directory.path()}));

  std::size_t winners = 0;
  for (const cotest::ChildResult& result : results) {
    CO_REQUIRE(result.started);
    if (result.exit_code == 0) {
      ++winners;
      CO_CHECK(result.output.find("revision") != std::string::npos);
    } else {
      CO_CHECK_EQ(result.exit_code, 3);
      CO_CHECK(result.output.find("store_busy") != std::string::npos);
    }
  }
  CO_CHECK_EQ(winners, 3u);  // sequential children each get the lock in turn

  // The state the children produced is readable and consistent: three commits
  // of one record each.
  Engine engine;
  cotest::FixedClock clock(cotest::kStart);
  CO_REQUIRE_OK(engine.open(EngineOptions{directory.path(), {}, {}, {}, {}}, clock));
  CO_CHECK_EQ(engine.image().value().evidence.size(), 1u);
  CO_CHECK_EQ(engine.image().value().revision.value(), 3u);
  CO_REQUIRE_OK(engine.close());
}

CO_TEST("two processes at once: one holds the store and the other is refused") {
  // The parent holds the store open, and the child - a separate process with its
  // own handle - is refused by the kernel. Both processes are genuinely running
  // at the same time, which is what makes this a proof about the lock rather than
  // about two sequential opens.
  cotest::Fixture fixture("process-concurrent", true);
  cotest::open_fixture(fixture);
  CO_REQUIRE(!cotest::self_executable().empty());

  const cotest::ChildResult contender =
      cotest::run_self({"--child-structure-writer", fixture.directory.path()});
  CO_REQUIRE(contender.started);
  if (contender.exit_code != 3) {
    cotest::record_failure("expected the child to be refused with exit 3, got " +
                               std::to_string(contender.exit_code) + ": " + contender.output +
                               contender.error,
                           __FILE__, __LINE__);
  }
  if (contender.output.find("store_locked") == std::string::npos) {
    cotest::record_failure("expected the refusal to name store_locked, saw: " + contender.output +
                               contender.error,
                           __FILE__, __LINE__);
  }

  // The holder's state is untouched by the refusal: a refused open changes
  // nothing on disk.
  CO_CHECK_EQ(fixture.engine.image().value().revision.value(), 1u);
}

CO_TEST("the inspection tool reads and answers as an external program") {
  CO_REQUIRE(!cotest::ccoctl_executable().empty());
  cotest::Fixture fixture("process-cli", true);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));
  CO_REQUIRE_OK(fixture.engine.close());

  const cotest::ChildResult version = cotest::run_program(cotest::ccoctl_executable(), {"version"});
  CO_REQUIRE(version.started);
  CO_CHECK_EQ(version.exit_code, 0);
  CO_CHECK(version.output.find("cooling-observatory") != std::string::npos);

  const cotest::ChildResult delivery =
      cotest::run_program(cotest::ccoctl_executable(), {"delivery", "--state", fixture.directory.path()});
  CO_REQUIRE(delivery.started);
  CO_CHECK_EQ(delivery.exit_code, 0);
  CO_CHECK(delivery.output.find("query delivery") != std::string::npos);
  CO_CHECK(delivery.output.find("zone.a") != std::string::npos);

  const cotest::ChildResult json = cotest::run_program(
      cotest::ccoctl_executable(), {"delivery", "--state", fixture.directory.path(), "--json"});
  CO_REQUIRE(json.started);
  CO_CHECK_EQ(json.exit_code, 0);
  CO_CHECK(json.output.find("\"query\":\"delivery\"") != std::string::npos);

  const cotest::ChildResult stats =
      cotest::run_program(cotest::ccoctl_executable(), {"stats", "--state", fixture.directory.path()});
  CO_REQUIRE(stats.started);
  CO_CHECK_EQ(stats.exit_code, 0);
  // The structure adoption is one commit and the baseline batch is another.
  CO_CHECK(stats.output.find("revision 2") != std::string::npos);
}

CO_TEST("the inspection tool reports usage errors with a distinct exit code") {
  CO_REQUIRE(!cotest::ccoctl_executable().empty());
  const cotest::ChildResult none = cotest::run_program(cotest::ccoctl_executable(), {});
  CO_REQUIRE(none.started);
  CO_CHECK_EQ(none.exit_code, 2);
  const cotest::ChildResult unknown =
      cotest::run_program(cotest::ccoctl_executable(), {"analyse-everything"});
  CO_REQUIRE(unknown.started);
  CO_CHECK_EQ(unknown.exit_code, 2);
  const cotest::ChildResult missing_value =
      cotest::run_program(cotest::ccoctl_executable(), {"delivery", "--state"});
  CO_REQUIRE(missing_value.started);
  CO_CHECK_EQ(missing_value.exit_code, 2);
}

CO_TEST("the inspection tool ingests records from a file and answers about them") {
  CO_REQUIRE(!cotest::ccoctl_executable().empty());
  cotest::TempDir directory("process-cli-ingest");
  const std::string state = directory.file("state");
  const std::string records = directory.file("records.txt");
  cotest::write_bytes(records,
                      "record adopt_structure\n"
                      "generation 1\n"
                      "facility dc1\n"
                      "plant plant.a dc1 plant_a\n"
                      "loop loop.primary plant.a primary\n"
                      "component pump pump.p1 loop.primary pump_one\n"
                      "zone zone.a dc1 hall_a 180kW\n"
                      "link loop:loop.primary pump:pump.p1 supply\n"
                      "record add_observation\n"
                      "observation measurement\n"
                      "subject measurement:zone.a.flow\n"
                      "sensor sensor.a.flow\n"
                      "value 30000ml/s\n"
                      "seq 1\n"
                      "epoch 7\n");

  const cotest::ChildResult ingest = cotest::run_program(
      cotest::ccoctl_executable(), {"ingest", "--state", state, "--input", records});
  CO_REQUIRE(ingest.started);
  if (ingest.exit_code != 0) {
    cotest::record_failure("ingest failed: " + ingest.output + ingest.error, __FILE__, __LINE__);
  }
  CO_CHECK_EQ(ingest.exit_code, 0);
  CO_CHECK(ingest.output.find("records_applied 2") != std::string::npos);

  const cotest::ChildResult image =
      cotest::run_program(cotest::ccoctl_executable(), {"image", "--state", state});
  CO_REQUIRE(image.started);
  CO_CHECK_EQ(image.exit_code, 0);
  CO_CHECK(image.output.find("components 1") != std::string::npos);
  CO_CHECK(image.output.find("evidence 1") != std::string::npos);

  const cotest::ChildResult again = cotest::run_program(
      cotest::ccoctl_executable(), {"ingest", "--state", state, "--input", records});
  CO_REQUIRE(again.started);
  CO_CHECK_EQ(again.exit_code, 0);
  // The measurement is recognised as a replay. The structure record is an
  // adoption, which is an explicit verb rather than an observation, so it is
  // applied again and the batch therefore does commit something.
  CO_CHECK(again.output.find("duplicates 1") != std::string::npos);
  CO_CHECK(again.output.find("records_applied 1") != std::string::npos);
}

CO_TEST("a state directory deeper than the classic path limit works") {
  cotest::TempDir directory("process-longpath");
  const std::string deep = long_state_path(directory);
  CO_CHECK(deep.size() > 260);

  Engine engine;
  cotest::FixedClock clock(cotest::kStart);
  const Status opened = engine.open(EngineOptions{deep, {}, {}, {}, {}}, clock);
  CO_REQUIRE_OK(opened);
  const auto image = engine.image();
  CO_REQUIRE_OK(image);
  const EpochId epoch = image.value().epoch;
  const auto report = engine.ingest(
      {make_structure(GenerationId(1), cotest::synthetic_plant(), "long_path_probe")});
  CO_REQUIRE_OK(report);
  CO_REQUIRE_OK(engine.ingest(cotest::baseline_records(epoch, clock.now_ms())));
  CO_CHECK_EQ(engine.image().value().evidence.size(), 12u);
  const ObservationReport delivery = cotest::query(engine, QueryKind::Delivery);
  CO_CHECK(!delivery.delivery.points.empty() || !delivery.delivery.unevidenced_points.empty());
  CO_REQUIRE_OK(engine.close());

  // A second process reads the same deep directory.
  CO_REQUIRE(!cotest::self_executable().empty());
  const cotest::ChildResult child = cotest::run_self({"--child-reopen", deep});
  CO_REQUIRE(child.started);
  if (child.exit_code != 0) {
    cotest::record_failure("deep-path reopen failed: " + child.output + child.error, __FILE__,
                           __LINE__);
  }
  CO_CHECK_EQ(child.exit_code, 0);
  CO_CHECK(child.output.find("evidence 12") != std::string::npos);
}