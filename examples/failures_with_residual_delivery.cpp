// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// A declared failure is a claim. This example shows the four things the
// observatory can say about one: confirmed, contradicted, residual and
// indeterminate.

#include <iostream>

#include "example_support.hpp"

namespace {

dccp::cooling_observatory::IngestRecord failure_record(
    std::uint64_t sequence, dccp::cooling_observatory::EpochId epoch, const char* element,
    dccp::cooling_observatory::FailureKind kind, dccp::cooling_observatory::FailureImpact impact,
    dccp::cooling_observatory::TimestampMs now) {
  using namespace dccp::cooling_observatory;
  IngestRecord record;
  record.kind = RecordKind::AddObservation;
  record.record_seq = RecordSeq(sequence);
  record.epoch = epoch;
  record.received_at_ms = now;
  record.observation.kind = ObservationKind::Failure;
  record.observation.subject = SubjectRef(SubjectKind::Pump, StrongId::from_validated(element));
  record.observation.failure_kind = kind;
  record.observation.severity = FailureSeverity::Major;
  record.observation.impact = impact;
  record.observation.observed_at_ms = now;
  record.observation.received_at_ms = now;
  record.observation.record_seq = RecordSeq(sequence);
  record.observation.epoch = epoch;
  // The failure is declared by the failure authority, not concluded here.
  record.observation.authority.domain = AuthorityDomain::CoolingFailure;
  record.observation.authority.authority = "dccp-cooling-failure-manager/1.0.0";
  return record;
}

}  // namespace

int main() {
  using namespace dccp::cooling_observatory;
  using namespace example;

  ExampleClock clock(1'800'000'000'000);
  Engine engine;
  report(engine.open(EngineOptions{}, clock), "open");
  const EpochId epoch = engine.image().value().epoch;
  const ZoneId zone_a(StrongId::from_validated("zone.a"));

  std::vector<IngestRecord> records;
  records.push_back(make_structure(GenerationId(1), synthetic_plant(), "synthetic_drawing_rev_1"));
  records.push_back(capability(1, epoch, SubjectKind::Pump, "pump.p1", Quantity::flow(40'000'000), 0,
                               clock.now_ms()));
  records.push_back(equipment_state(2, epoch, SubjectKind::Pump, "pump.p1", LifecycleState::Faulted,
                                    clock.now_ms()));
  // Flow is still measured through the zone the failed pump feeds.
  records.push_back(zone_measurement(3, epoch, "zone.a", zone_flow(zone_a), "sensor.a.flow",
                                     Quantity::flow(12'000'000), clock.now_ms()));
  records.push_back(failure_record(4, epoch, "pump.p1", FailureKind::PumpStopped,
                                   FailureImpact::NoDelivery, clock.now_ms()));
  // A second failure is declared for equipment the adopted structure does not
  // contain at all.
  records.push_back(failure_record(5, epoch, "pump.p9", FailureKind::PumpStopped,
                                   FailureImpact::NoDelivery, clock.now_ms()));
  const auto ingested = engine.ingest(records);
  report(ingested, "ingest");
  if (!ingested) {
    return 1;
  }

  ObserveRequest request;
  request.kind = QueryKind::Failures;
  const auto failures = engine.observe(request);
  report(failures, "failures");
  if (!failures) {
    return 1;
  }
  std::cout << render_text(failures.value());
  std::cout << "\npump.p1 is declared stopped with no delivery; twelve litres per second are\n"
               "still measured downstream, so the effect is residual rather than confirmed.\n"
               "pump.p9 is not in the adopted structure, and that is reported rather than\n"
               "quietly attached to a plausible neighbour.\n";
  return 0;
}
