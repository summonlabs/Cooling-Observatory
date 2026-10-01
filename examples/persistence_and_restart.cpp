// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Durable state, atomic commit, and what a restart does to evidence: the state
// comes back, and the evidence that came back is labelled recovered rather than
// current.

#include <cstdio>
#include <iostream>
#include <string>

#include "example_support.hpp"

int main(int argc, char** argv) {
  using namespace dccp::cooling_observatory;
  using namespace example;

  const std::string directory =
      argc > 1 ? std::string(argv[1])
               : std::string("cooling-observatory-example-restart");
  ExampleClock clock(1'800'000'000'000);

  {
    Engine engine;
    EngineOptions options;
    options.state_directory = directory;
    const Status opened = engine.open(options, clock);
    report(opened, "open");
    if (!opened.ok()) {
      return 1;
    }
    const EpochId epoch = engine.image().value().epoch;

    const ZoneId zone_a(StrongId::from_validated("zone.a"));
    std::vector<IngestRecord> records;
    records.push_back(make_structure(GenerationId(1), synthetic_plant(), "synthetic_drawing_rev_1"));
    records.push_back(zone_measurement(1, epoch, "zone.a", zone_flow(zone_a), "sensor.a.flow",
                                       Quantity::flow(30'000'000), clock.now_ms()));
    records.push_back(zone_measurement(2, epoch, "zone.a", zone_supply_temperature(zone_a),
                                       "sensor.a.supply", Quantity::temperature(18'000),
                                       clock.now_ms()));
    records.push_back(zone_measurement(3, epoch, "zone.a", zone_return_temperature(zone_a),
                                       "sensor.a.return", Quantity::temperature(24'000),
                                       clock.now_ms()));
    const auto ingested = engine.ingest(records);
    report(ingested, "ingest");
    if (!ingested) {
      return 1;
    }
    std::cout << "-- before the restart --\n";
    std::cout << render_text(engine.observe(ObserveRequest{QueryKind::Delivery, {}}).value());
    const Status closed = engine.close();
    report(closed, "close");
  }

  // The clock has moved an hour forward: the evidence restored from disk is far
  // outside the delivery age bound, and it is still not promoted to current.
  ExampleClock later(1'800'000'000'000 + 3'600'000);
  Engine restarted;
  EngineOptions options;
  options.state_directory = directory;
  const Status reopened = restarted.open(options, later);
  report(reopened, "reopen");
  if (!reopened.ok()) {
    return 1;
  }
  const auto manifest = restarted.manifest();
  if (manifest) {
    std::cout << "\n-- manifest after the restart --\n";
    std::cout << "revision " << manifest.value().revision.value() << "\n";
    std::cout << "generation " << manifest.value().generation.value() << "\n";
    std::cout << "evidence " << manifest.value().evidence_count << "\n";
    std::cout << "digest " << manifest.value().digest << "\n";
  }

  std::cout << "\n-- after the restart --\n";
  const auto delivery = restarted.observe(ObserveRequest{QueryKind::Delivery, {}});
  report(delivery, "delivery");
  if (!delivery) {
    return 1;
  }
  std::cout << render_text(delivery.value());

  // New evidence for the same sensors replaces the recovered state and is
  // current again.
  IngestRecord fresh =
      zone_measurement(1, restarted.image().value().epoch, "zone.a",
                       zone_flow(ZoneId(StrongId::from_validated("zone.a"))), "sensor.a.flow",
                       Quantity::flow(31'000'000), later.now_ms());
  const auto refreshed = restarted.ingest({fresh});
  report(refreshed, "ingest after restart");
  if (refreshed) {
    std::cout << "\nrefreshed_after_recovery "
              << (refreshed.value().refreshed_after_recovery ? "true" : "false") << "\n";
    std::cout << "duplicates " << refreshed.value().duplicates.size()
              << "  (the same record sequence from the same epoch is applied once)\n";
  }

  const Status closed = restarted.close();
  report(closed, "close");
  std::cout << "\nstate directory: " << directory << "\n";
  return 0;
}