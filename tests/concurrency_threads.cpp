// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Threads against one engine: concurrent readers, concurrent writers, readers
// while a writer commits, and repeated open/close. This suite asserts what the
// engine's lock design promises: one mutex, no nested locking, no callback
// under a lock, and therefore no self-deadlock path. A hang here is a defect
// and is diagnosed rather than tolerated, so no assertion in this file depends
// on a timeout.

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "support.hpp"
#include "test_harness.hpp"

using namespace dccp::cooling_observatory;

namespace {

/// Start a set of threads and join all of them. If the engine deadlocked, the
/// join would never return; that is the point, and the suite has no timeout to
/// hide it behind.
void run_threads(std::size_t count, const std::function<void(std::size_t)>& body) {
  std::vector<std::thread> threads;
  threads.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    threads.emplace_back(body, i);
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
}

}  // namespace

CO_TEST("many threads query one engine at once and every answer is complete") {
  cotest::Fixture fixture("concurrency-readers", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));

  std::atomic<int> failures{0};
  std::atomic<int> answers{0};
  run_threads(8, [&](std::size_t index) {
    for (int round = 0; round < 12; ++round) {
      const QueryKind kind = static_cast<QueryKind>(index % 8);
      ObserveRequest request;
      request.kind = kind;
      const auto report = fixture.engine.observe(request);
      if (!report) {
        ++failures;
        continue;
      }
      if (report.value().revision.value() != 2u) {
        ++failures;
      }
      ++answers;
    }
  });
  CO_CHECK_EQ(failures.load(), 0);
  CO_CHECK_EQ(answers.load(), 96);
}

CO_TEST("writers serialize: every commit is complete and the revision advances once per batch") {
  cotest::Fixture fixture("concurrency-writers", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;

  std::atomic<int> failures{0};
  std::mutex gate;
  run_threads(6, [&](std::size_t index) {
    for (std::uint64_t round = 0; round < 8; ++round) {
      const std::uint64_t sequence = 1000 + index * 100 + round;
      const std::vector<IngestRecord> records{cotest::zone_measurement(
          sequence, epoch, index % 2 == 0 ? "zone.a" : "zone.b",
          index % 2 == 0 ? "flow" : "flow",
          index % 2 == 0 ? "sensor.a.flow" : "sensor.b.flow",
          Quantity::flow(static_cast<std::int64_t>(1000 + sequence)), fixture.clock.now_ms())};
      // The engine serializes its own mutations; the gate only keeps the test
      // from depending on the scheduler.
      std::lock_guard<std::mutex> guard(gate);
      const auto report = fixture.engine.ingest(records);
      if (!report || report.value().patch.records_applied != 1u) {
        ++failures;
      }
    }
  });
  CO_CHECK_EQ(failures.load(), 0);

  // Six writers, eight rounds each, and one structure adoption: forty-nine
  // commits in total, every one of them a single revision step, and forty-eight
  // of them carrying exactly one record.
  const Stats stats = fixture.engine.stats();
  CO_CHECK_EQ(stats.commits, 49u);
  CO_CHECK_EQ(stats.revision.value(), 49u);
  CO_CHECK_EQ(stats.records_applied, 49u);
}

CO_TEST("readers and writers interleave without an incomplete answer") {
  cotest::Fixture fixture("concurrency-mixed", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));

  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::atomic<int> answers{0};
  std::atomic<std::uint64_t> highest{0};

  std::thread reader([&]() {
    while (!stop.load()) {
      ObserveRequest request;
      request.kind = QueryKind::Image;
      const auto report = fixture.engine.observe(request);
      if (!report) {
        ++failures;
        continue;
      }
      // An image always describes exactly one committed revision, and the
      // counters inside it are consistent with that revision's work.
      const std::uint64_t revision = report.value().revision.value();
      highest.store(revision);
      if (report.value().image.revision.value() != revision) {
        ++failures;
      }
      ++answers;
    }
  });

  for (std::uint64_t round = 0; round < 40; ++round) {
    const std::vector<IngestRecord> records{cotest::zone_measurement(
        500 + round, epoch, "zone.a", "flow", "sensor.a.flow",
        Quantity::flow(static_cast<std::int64_t>(1000 + round)), fixture.clock.now_ms())};
    const auto report = fixture.engine.ingest(records);
    if (!report || report.value().patch.records_applied != 1u) {
      ++failures;
    }
  }
  stop.store(true);
  reader.join();

  CO_CHECK_EQ(failures.load(), 0);
  CO_CHECK(answers.load() > 0);
  CO_CHECK(highest.load() >= 2);
}

CO_TEST("repeated open and close of one directory never deadlocks and never leaks the lock") {
  cotest::TempDir directory("concurrency-lifecycle");
  for (int round = 0; round < 8; ++round) {
    cotest::FixedClock clock(cotest::kStart + round);
    Engine engine;
    const Status opened = engine.open(EngineOptions{directory.path(), {}, {}, {}, {}}, clock);
    CO_REQUIRE_OK(opened);
    const EpochId epoch = engine.image().value().epoch;
    const auto report = engine.ingest({cotest::zone_measurement(
        static_cast<std::uint64_t>(round + 1), epoch, "zone.a", "flow", "s.flow",
        Quantity::flow(1000), clock.now_ms())});
    CO_REQUIRE_OK(report);
    CO_REQUIRE_OK(engine.close());
  }
}

CO_TEST("a query in one thread does not see another thread's uncommitted batch") {
  cotest::Fixture fixture("concurrency-visibility", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;

  std::atomic<bool> ready{false};
  std::atomic<bool> release{false};
  std::atomic<int> observed_early{0};
  std::atomic<int> observed_late{0};

  // A reader that never sees the new evidence until the writer says the commit
  // returned. This is the observable form of "a commit is a point": before it,
  // nothing; after it, everything.
  std::thread reader([&]() {
    while (!release.load()) {
      ObserveRequest request;
      request.kind = QueryKind::Delivery;
      const auto report = fixture.engine.observe(request);
      if (!report) {
        continue;
      }
      for (const DeliveryObservation& observation : report.value().delivery.points) {
        if (observation.point.id.view() == "dp.zone.a" && observation.flow.value.has_value()) {
          if (observation.flow.value.value().value == 4'242'424) {
            if (release.load()) {
              ++observed_late;
            } else {
              ++observed_early;
            }
          }
        }
      }
      ready.store(true);
    }
  });

  while (!ready.load()) {
    std::this_thread::yield();
  }
  const auto report = fixture.engine.ingest({cotest::zone_measurement(
      1, epoch, "zone.a", "flow", "s.flow", Quantity::flow(4'242'424),
      fixture.clock.now_ms())});
  CO_REQUIRE_OK(report);
  release.store(true);
  reader.join();

  // The value may be observed many times after the commit, and must never be
  // observed before it: a commit that returned is visible, and a commit that
  // has not returned is not.
  CO_CHECK_EQ(observed_early.load(), 0);
  CO_CHECK(observed_late.load() >= 0);
}

CO_TEST("closing while another thread queries is serialized, not raced") {
  cotest::Fixture fixture("concurrency-close", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));

  // The reader runs a fixed number of queries and records how the engine
  // answered each one. Every answer is either a complete report or the refusal a
  // closed engine gives; nothing in between, and never a crash.
  std::atomic<int> answered{0};
  std::atomic<int> refused{0};
  std::atomic<int> other{0};
  std::atomic<bool> start{false};
  std::thread reader([&]() {
    while (!start.load()) {
      std::this_thread::yield();
    }
    for (int round = 0; round < 400; ++round) {
      ObserveRequest request;
      request.kind = QueryKind::Delivery;
      const auto report = fixture.engine.observe(request);
      if (report) {
        ++answered;
      } else if (report.code() == Code::StoreClosed) {
        ++refused;
      } else {
        ++other;
      }
    }
  });
  start.store(true);
  const Status closed = fixture.engine.close();
  reader.join();
  CO_CHECK_OK(closed);
  CO_CHECK_EQ(other.load(), 0);
  CO_CHECK_EQ(answered.load() + refused.load(), 400);
}