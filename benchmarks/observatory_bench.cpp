// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The benchmark measures completed work: a fixed number of records ingested,
// a fixed number of queries answered, and a fixed number of durable commits.
// It reports the elapsed time of each phase and the work each phase completed,
// so a throughput figure is a division the reader can check rather than a
// number this program asserts.
//
// Every number it prints is SYNTHETIC: the facility, the telemetry and the
// workloads are generated in this process. No hardware, no BMS/DCIM, no
// multi-node and no real telemetry is involved, and nothing here is evidence
// about any of them.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "dccp/cooling_observatory/engine.hpp"
#include "dccp/cooling_observatory/textproto.hpp"
#include "dccp/cooling_observatory/version.hpp"

namespace {

using namespace dccp::cooling_observatory;
using SteadyClock = std::chrono::steady_clock;

double millis_since(SteadyClock::time_point start) {
  return std::chrono::duration<double, std::milli>(SteadyClock::now() - start).count();
}

struct SyntheticFacility {
  PlantModel model;
  std::vector<std::string> zone_ids;
  std::vector<std::string> pump_ids;
};

/// Build a synthetic facility with the given number of zones, each fed by its
/// own CRAH on a shared secondary loop, with a shared primary loop of pumps.
SyntheticFacility build_facility(std::size_t zones, std::size_t pumps) {
  SyntheticFacility facility;
  facility.model.add_facility(FacilityId(StrongId::from_validated("bench")), "bench");

  CoolingPlant plant;
  plant.id = PlantId(StrongId::from_validated("plant.bench"));
  plant.facility = FacilityId(StrongId::from_validated("bench"));
  facility.model.add_plant(plant);

  Loop primary;
  primary.id = LoopId(StrongId::from_validated("loop.primary"));
  primary.plant = plant.id;
  facility.model.add_loop(primary);
  Loop secondary;
  secondary.id = LoopId(StrongId::from_validated("loop.secondary"));
  secondary.plant = plant.id;
  secondary.secondary = true;
  facility.model.add_loop(secondary);

  facility.model.add_link(PlantLink{SubjectRef(SubjectKind::Plant, StrongId::from_validated("plant.bench")),
                                    SubjectRef(SubjectKind::Loop, StrongId::from_validated("loop.primary")),
                                    LinkRelation::Supply});
  facility.model.add_link(PlantLink{SubjectRef(SubjectKind::Loop, StrongId::from_validated("loop.primary")),
                                    SubjectRef(SubjectKind::Loop, StrongId::from_validated("loop.secondary")),
                                    LinkRelation::Supply});

  for (std::size_t i = 0; i < pumps; ++i) {
    const std::string id = "pump.bench" + std::to_string(i);
    PlantComponent component;
    component.kind = ComponentKind::Pump;
    component.id = StrongId::from_validated(id);
    component.loop = primary.id;
    facility.model.add_component(component);
    facility.pump_ids.push_back(id);
    facility.model.add_link(
        PlantLink{SubjectRef(SubjectKind::Loop, StrongId::from_validated("loop.primary")),
                  SubjectRef(SubjectKind::Pump, StrongId::from_validated(id)), LinkRelation::Supply});
  }

  for (std::size_t i = 0; i < zones; ++i) {
    const std::string zone_id = "zone.bench" + std::to_string(i);
    const std::string crah_id = "crah.bench" + std::to_string(i);
    ThermalZone zone;
    zone.id = ZoneId(StrongId::from_validated(zone_id));
    zone.facility = FacilityId(StrongId::from_validated("bench"));
    zone.declared_load = Maybe<Quantity>::of(Quantity::power(100'000));
    facility.model.add_zone(zone);

    PlantComponent crah;
    crah.kind = ComponentKind::Crah;
    crah.id = StrongId::from_validated(crah_id);
    crah.loop = secondary.id;
    facility.model.add_component(crah);

    facility.model.add_link(
        PlantLink{SubjectRef(SubjectKind::Loop, StrongId::from_validated("loop.secondary")),
                  SubjectRef(SubjectKind::Crah, StrongId::from_validated(crah_id)), LinkRelation::Supply});
    facility.model.add_link(
        PlantLink{SubjectRef(SubjectKind::Crah, StrongId::from_validated(crah_id)),
                  SubjectRef(SubjectKind::Zone, StrongId::from_validated(zone_id)), LinkRelation::Supply});
  }

  facility.model.reindex();
  facility.zone_ids.reserve(zones);
  for (const ThermalZone& zone : facility.model.zones()) {
    facility.zone_ids.push_back(zone.id.str());
  }
  return facility;
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t zone_count = argc > 1 ? static_cast<std::size_t>(std::stoull(argv[1])) : 64;
  const std::size_t pump_count = argc > 2 ? static_cast<std::size_t>(std::stoull(argv[2])) : 4;
  const std::size_t rounds = argc > 3 ? static_cast<std::size_t>(std::stoull(argv[3])) : 20;
  const bool durable = argc > 4 && std::string(argv[4]) == "durable";
  const std::string directory = argc > 5 ? std::string(argv[5]) : std::string("bench-state");

  std::printf("cooling_observatory %s benchmark\n", version_string().c_str());
  std::printf("proof: SYNTHETIC (generated facility and telemetry, single process, no hardware)\n");
  std::printf("zones %zu pumps %zu rounds %zu mode %s\n", zone_count, pump_count, rounds,
              durable ? "durable" : "in-memory");

  FixedClock clock(1'800'000'000'000);
  EngineOptions options;
  if (durable) {
    options.state_directory = directory;
  }
  Engine engine;
  const Status opened = engine.open(options, clock);
  if (!opened.ok()) {
    std::fprintf(stderr, "open failed: %s\n", opened.reason().c_str());
    return 1;
  }
  const EpochId epoch = engine.image().value().epoch;

  const auto build_start = SteadyClock::now();
  const SyntheticFacility facility = build_facility(zone_count, pump_count);
  const double build_ms = millis_since(build_start);

  const auto structure_start = SteadyClock::now();
  {
    const auto report = engine.ingest({make_structure(GenerationId(1), facility.model, "bench")});
    if (!report) {
      std::fprintf(stderr, "structure ingest failed\n");
      return 1;
    }
  }
  const double structure_ms = millis_since(structure_start);

  // A fixed workload: every zone reports flow, supply temperature, return
  // temperature and differential pressure once per round.
  std::size_t records_ingested = 0;
  const auto ingest_start = SteadyClock::now();
  std::uint64_t sequence = 1;
  for (std::size_t round = 0; round < rounds; ++round) {
    std::vector<IngestRecord> batch;
    batch.reserve(facility.zone_ids.size() * 4 + facility.pump_ids.size() * 2);
    for (const std::string& zone_id : facility.zone_ids) {
      const ZoneId zone(StrongId::from_validated(zone_id));
      batch.push_back(make_zone_measurement(RecordSeq(sequence++), epoch, zone, zone_flow(zone),
                                            "sensor." + zone_id + ".flow",
                                            Quantity::flow(30'000'000 + static_cast<std::int64_t>(round)),
                                            clock.now_ms()));
      batch.push_back(make_zone_measurement(RecordSeq(sequence++), epoch, zone,
                                            zone_supply_temperature(zone),
                                            "sensor." + zone_id + ".supply",
                                            Quantity::temperature(18'000), clock.now_ms()));
      batch.push_back(make_zone_measurement(RecordSeq(sequence++), epoch, zone,
                                            zone_return_temperature(zone),
                                            "sensor." + zone_id + ".return",
                                            Quantity::temperature(24'000), clock.now_ms()));
      batch.push_back(make_zone_measurement(RecordSeq(sequence++), epoch, zone,
                                            zone_differential_pressure(zone),
                                            "sensor." + zone_id + ".dp", Quantity::pressure(120'000),
                                            clock.now_ms()));
    }
    for (const std::string& pump_id : facility.pump_ids) {
      IngestRecord record;
      record.kind = RecordKind::AddObservation;
      record.record_seq = RecordSeq(sequence++);
      record.epoch = epoch;
      record.received_at_ms = clock.now_ms();
      record.observation.kind = ObservationKind::Capability;
      record.observation.subject = SubjectRef(SubjectKind::Pump, StrongId::from_validated(pump_id));
      record.observation.capability_dimension = Dimension::Flow;
      record.observation.declared_capacity = 40'000'000;
      record.observation.observed_at_ms = clock.now_ms();
      record.observation.received_at_ms = clock.now_ms();
      record.observation.record_seq = record.record_seq;
      record.observation.epoch = epoch;
      record.observation.authority.domain = AuthorityDomain::CoolingCapacity;
      batch.push_back(std::move(record));

      IngestRecord state;
      state.kind = RecordKind::AddObservation;
      state.record_seq = RecordSeq(sequence++);
      state.epoch = epoch;
      state.received_at_ms = clock.now_ms();
      state.observation.kind = ObservationKind::EquipmentState;
      state.observation.subject = SubjectRef(SubjectKind::Pump, StrongId::from_validated(pump_id));
      state.observation.state = LifecycleState::Running;
      state.observation.observed_at_ms = clock.now_ms();
      state.observation.received_at_ms = clock.now_ms();
      state.observation.record_seq = state.record_seq;
      state.observation.epoch = epoch;
      state.observation.authority.domain = AuthorityDomain::CoolingControl;
      batch.push_back(std::move(state));
    }
    const auto report = engine.ingest(batch);
    if (!report) {
      std::fprintf(stderr, "ingest failed\n");
      return 1;
    }
    records_ingested += report.value().patch.records_applied;
    clock.advance(1'000);
  }
  const double ingest_ms = millis_since(ingest_start);

  struct Phase {
    const char* name;
    QueryKind kind;
  };
  const Phase phases[] = {
      {"delivery", QueryKind::Delivery},       {"divergence", QueryKind::Divergence},
      {"constraints", QueryKind::Constraints}, {"failures", QueryKind::Failures},
      {"reserve", QueryKind::Reserve},         {"coverage", QueryKind::Coverage},
  };

  std::printf("\nphase,work,elapsed_ms,per_query_us,per_row_us\n");
  std::printf("build_structure,%zu components,%.3f,,\n",
              facility.model.components().size() + facility.model.links().size(), build_ms);
  std::printf("adopt_structure,1 record,%.3f,,\n", structure_ms);
  std::printf("ingest,%zu records,%.3f,,%.3f\n", records_ingested, ingest_ms,
              records_ingested == 0 ? 0.0 : (ingest_ms * 1000.0) / static_cast<double>(records_ingested));

  for (const Phase& phase : phases) {
    ObserveRequest request;
    request.kind = phase.kind;
    const auto start = SteadyClock::now();
    std::size_t answered = 0;
    std::size_t rows = 0;
    for (std::size_t i = 0; i < rounds; ++i) {
      const auto report = engine.observe(request);
      if (!report) {
        std::fprintf(stderr, "%s query failed\n", phase.name);
        return 1;
      }
      ++answered;
      switch (phase.kind) {
        case QueryKind::Delivery:
          rows += report.value().delivery.points.size();
          break;
        case QueryKind::Divergence:
          rows += report.value().divergence.findings.size();
          break;
        case QueryKind::Constraints:
          rows += report.value().constraints.constraints.size();
          break;
        case QueryKind::Failures:
          rows += report.value().failures.failures.size();
          break;
        case QueryKind::Reserve:
          rows += report.value().reserve.scopes.size();
          break;
        case QueryKind::Coverage:
          rows += report.value().coverage.gaps.size();
          break;
        default:
          break;
      }
    }
    const double elapsed = millis_since(start);
    // Both unit costs are printed. Dividing the total by the number of queries
    // while the work column counts rows invites reading one as the other, and a
    // benchmark whose arithmetic does not agree with its own columns is not
    // evidence of anything.
    std::printf("%s,%zu queries %zu rows,%.3f,%.3f,%.3f\n", phase.name, answered, rows, elapsed,
                answered == 0 ? 0.0 : (elapsed * 1000.0) / static_cast<double>(answered),
                rows == 0 ? 0.0 : (elapsed * 1000.0) / static_cast<double>(rows));
  }

  // Encoding and durable commit, measured separately from the analysis.
  {
    const auto image = engine.image();
    if (image) {
      const auto start = SteadyClock::now();
      const auto encoded = encode_image(image.value(), default_limits());
      const double elapsed = millis_since(start);
      if (encoded) {
        std::printf("encode_snapshot,%zu bytes,%.3f,,\n", encoded.value().size(), elapsed);
      }
    }
  }

  const Stats stats = engine.stats();
  std::printf("\ncompleted work: commits %llu records %llu evidence_retired %llu queries %llu\n",
              static_cast<unsigned long long>(stats.commits),
              static_cast<unsigned long long>(stats.records_applied),
              static_cast<unsigned long long>(stats.evidence_retired),
              static_cast<unsigned long long>(stats.queries));
  std::printf("durable commits: %llu bytes %llu\n",
              static_cast<unsigned long long>(stats.snapshot_writes),
              static_cast<unsigned long long>(stats.snapshot_bytes_written));
  std::printf("final revision %llu generation %llu\n",
              static_cast<unsigned long long>(stats.revision.value()),
              static_cast<unsigned long long>(stats.generation.value()));

  const Status closed = engine.close();
  if (!closed.ok()) {
    std::fprintf(stderr, "close failed\n");
    return 1;
  }
  return 0;
}