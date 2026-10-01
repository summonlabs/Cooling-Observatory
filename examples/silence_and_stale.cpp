// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Silence is unknown, never zero and never healthy; evidence older than its
// axis bound stops being current without being deleted.

#include <iostream>

#include "example_support.hpp"

int main() {
  using namespace dccp::cooling_observatory;
  using namespace example;

  ExampleClock clock(1'800'000'000'000);
  Engine engine;
  const Status opened = engine.open(EngineOptions{}, clock);
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
                                     "sensor.a.supply", Quantity::temperature(18'000), clock.now_ms()));
  records.push_back(zone_measurement(3, epoch, "zone.a", zone_return_temperature(zone_a),
                                     "sensor.a.return", Quantity::temperature(24'000), clock.now_ms()));
  const auto ingested = engine.ingest(records);
  report(ingested, "ingest");
  if (!ingested) {
    return 1;
  }

  ObserveRequest request;
  request.kind = QueryKind::Coverage;
  std::cout << "-- immediately after measurement --\n";
  std::cout << render_text(engine.observe(request).value());

  // A delivery record that reports its own sensor as unavailable is silence,
  // not a zero measurement.
  std::vector<IngestRecord> silent;
  IngestRecord unavailable =
      zone_measurement(4, epoch, "zone.b", zone_flow(ZoneId(StrongId::from_validated("zone.b"))),
                       "sensor.b.flow", Quantity::flow(0), clock.now_ms());
  unavailable.observation.quality = MeasurementQuality::NotAvailable;
  silent.push_back(unavailable);
  const auto second = engine.ingest(silent);
  report(second, "ingest silent");
  if (!second) {
    return 1;
  }

  std::cout << "\n-- after a sensor reports itself unavailable --\n";
  std::cout << render_text(engine.observe(request).value());

  // Time passes beyond the delivery age bound. The measurement is still held,
  // and it is no longer current.
  clock.advance(120'000);
  std::cout << "\n-- two minutes later, with no new delivery evidence --\n";
  std::cout << render_text(engine.observe(request).value());

  request.kind = QueryKind::Delivery;
  const auto delivery = engine.observe(request);
  report(delivery, "delivery");
  std::cout << render_text(delivery.value());
  std::cout << "\nthe flow figure is still reported, and it is labelled stale rather than\n"
               "silently reused as a current measurement.\n";
  return 0;
}
