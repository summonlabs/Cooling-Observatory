// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// What cooling is actually arriving, and how much of it is evidenced rather
// than merely declared.

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

  std::vector<IngestRecord> records;
  records.push_back(make_structure(GenerationId(1), synthetic_plant(), "synthetic_drawing_rev_1"));

  // Declared capability of the two primary pumps, one of them derated by 10%.
  records.push_back(capability(1, epoch, SubjectKind::Pump, "pump.p1", Quantity::flow(40'000'000), 0,
                               clock.now_ms()));
  records.push_back(capability(2, epoch, SubjectKind::Pump, "pump.p2", Quantity::flow(40'000'000),
                               100'000, clock.now_ms()));
  records.push_back(equipment_state(3, epoch, SubjectKind::Pump, "pump.p1", LifecycleState::Running,
                                    clock.now_ms()));
  records.push_back(equipment_state(4, epoch, SubjectKind::Pump, "pump.p2", LifecycleState::Running,
                                    clock.now_ms()));

  // Measured delivery into zone.a: 30 L/s with a 6 K drop.
  records.push_back(zone_measurement(10, epoch, "zone.a", zone_flow(ZoneId(StrongId::from_validated("zone.a"))),
                                     "sensor.a.flow", Quantity::flow(30'000'000), clock.now_ms()));
  records.push_back(zone_measurement(11, epoch, "zone.a",
                                     zone_supply_temperature(ZoneId(StrongId::from_validated("zone.a"))),
                                     "sensor.a.supply", Quantity::temperature(18'000), clock.now_ms()));
  records.push_back(zone_measurement(12, epoch, "zone.a",
                                     zone_return_temperature(ZoneId(StrongId::from_validated("zone.a"))),
                                     "sensor.a.return", Quantity::temperature(24'000), clock.now_ms()));

  // Zone.b has a declared load and no instrumentation at all.
  const auto ingested = engine.ingest(records);
  report(ingested, "ingest");
  if (!ingested) {
    return 1;
  }

  ObserveRequest request;
  request.kind = QueryKind::Delivery;
  const auto delivery = engine.observe(request);
  report(delivery, "delivery");
  if (!delivery) {
    return 1;
  }
  std::cout << render_text(delivery.value());

  request.kind = QueryKind::Reserve;
  const auto reserve = engine.observe(request);
  report(reserve, "reserve");
  if (!reserve) {
    return 1;
  }
  std::cout << render_text(reserve.value());

  std::cout << "\nzone.b is declared and unevidenced: the reserve report names it, and does not\n"
               "report its headroom as zero.\n";
  return 0;
}
