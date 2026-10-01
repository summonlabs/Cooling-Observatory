// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// What the structure and the declarations imply against what the measurements
// show, and what happens when two observations cannot both be true.

#include <iostream>

#include "example_support.hpp"

int main() {
  using namespace dccp::cooling_observatory;
  using namespace example;

  ExampleClock clock(1'800'000'000'000);
  Engine engine;
  report(engine.open(EngineOptions{}, clock), "open");
  const EpochId epoch = engine.image().value().epoch;
  const ZoneId zone_a(StrongId::from_validated("zone.a"));
  const ZoneId zone_b(StrongId::from_validated("zone.b"));

  std::vector<IngestRecord> records;
  records.push_back(make_structure(GenerationId(1), synthetic_plant(), "synthetic_drawing_rev_1"));
  // Two pumps declared at 40 L/s each: the expected flow for the primary loop is
  // 80 L/s, and the zones are measured well below it.
  records.push_back(capability(1, epoch, SubjectKind::Pump, "pump.p1", Quantity::flow(40'000'000), 0,
                               clock.now_ms()));
  records.push_back(capability(2, epoch, SubjectKind::Pump, "pump.p2", Quantity::flow(40'000'000), 0,
                               clock.now_ms()));
  records.push_back(equipment_state(3, epoch, SubjectKind::Pump, "pump.p1", LifecycleState::Running,
                                    clock.now_ms()));
  records.push_back(equipment_state(4, epoch, SubjectKind::Pump, "pump.p2", LifecycleState::Running,
                                    clock.now_ms()));
  records.push_back(zone_measurement(5, epoch, "zone.a", zone_flow(zone_a), "sensor.a.flow",
                                     Quantity::flow(20'000'000), clock.now_ms()));
  records.push_back(zone_measurement(6, epoch, "zone.a", zone_supply_temperature(zone_a),
                                     "sensor.a.supply", Quantity::temperature(24'000), clock.now_ms()));
  records.push_back(zone_measurement(7, epoch, "zone.a", zone_return_temperature(zone_a),
                                     "sensor.a.return", Quantity::temperature(18'000), clock.now_ms()));
  // zone.b reports a differential pressure and no flow sensor at all, and the
  // pump is reported running: two separate contradictions.
  records.push_back(zone_measurement(8, epoch, "zone.b", zone_differential_pressure(zone_b),
                                     "sensor.b.dp", Quantity::pressure(90'000), clock.now_ms()));

  const auto ingested = engine.ingest(records);
  report(ingested, "ingest");
  if (!ingested) {
    return 1;
  }

  ObserveRequest request;
  request.kind = QueryKind::Divergence;
  const auto divergence = engine.observe(request);
  report(divergence, "divergence");
  if (!divergence) {
    return 1;
  }
  std::cout << render_text(divergence.value());
  std::cout << "\nzone.a delivers 20 L/s against a declared 80 L/s: under delivery.\n"
               "zone.a also has a supply temperature above its return temperature: the\n"
               "temperature pair is inverted, and the computed removal is reported next to\n"
               "the contradiction rather than being treated as cooling.\n"
               "zone.b has pressure and no flow, and a running pump below it with no flow\n"
               "anywhere: both are reported as contradictions, not averaged away.\n";
  return 0;
}
