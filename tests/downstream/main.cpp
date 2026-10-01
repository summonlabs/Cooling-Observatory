// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// An independent consumer of the installed package.
//
// This program is deliberately written the way a downstream runtime would be
// written: it includes the public headers, opens a durable engine in its own
// state directory, feeds it a synthetic facility, asks the observatory
// questions, and checks the answers. It links only the installed library and
// includes only installed headers, so a missing installation step is a build
// failure here rather than a surprise for a user.

#include <cstdio>
#include <string>
#include <vector>

#include "dccp/cooling_observatory/engine.hpp"
#include "dccp/cooling_observatory/textproto.hpp"
#include "dccp/cooling_observatory/version.hpp"

namespace {

using namespace dccp::cooling_observatory;

int failures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "downstream: FAILED %s\n", what);
    ++failures;
  }
}

/// The synthetic facility this consumer observes. It is built here rather than
/// imported from the library's own tests, because a consumer has no access to
/// them: everything it uses is a public header.
PlantModel synthetic_facility() {
  PlantModel model;
  model.add_facility(FacilityId(StrongId::from_validated("dc1")), "dc1");

  CoolingPlant plant;
  plant.id = PlantId(StrongId::from_validated("plant.a"));
  plant.facility = FacilityId(StrongId::from_validated("dc1"));
  model.add_plant(plant);

  Loop loop;
  loop.id = LoopId(StrongId::from_validated("loop.primary"));
  loop.plant = plant.id;
  model.add_loop(loop);

  PlantComponent pump;
  pump.kind = ComponentKind::Pump;
  pump.id = StrongId::from_validated("pump.p1");
  pump.loop = loop.id;
  model.add_component(pump);

  ThermalZone zone;
  zone.id = ZoneId(StrongId::from_validated("zone.a"));
  zone.facility = FacilityId(StrongId::from_validated("dc1"));
  zone.declared_load = Maybe<Quantity>::of(Quantity::power(180'000));
  model.add_zone(zone);

  model.add_link(PlantLink{SubjectRef(SubjectKind::Plant, StrongId::from_validated("plant.a")),
                           SubjectRef(SubjectKind::Loop, StrongId::from_validated("loop.primary")),
                           LinkRelation::Supply});
  model.add_link(PlantLink{SubjectRef(SubjectKind::Loop, StrongId::from_validated("loop.primary")),
                           pump.subject(), LinkRelation::Supply});
  model.add_link(PlantLink{pump.subject(),
                           SubjectRef(SubjectKind::Zone, StrongId::from_validated("zone.a")),
                           LinkRelation::Supply});
  model.reindex();
  return model;
}

/// A measurement record for one of the delivery point's roles.
IngestRecord zone_flow(std::uint64_t sequence, EpochId epoch, Quantity value, TimestampMs now) {
  const ZoneId zone(StrongId::from_validated("zone.a"));
  IngestRecord record = make_zone_measurement(RecordSeq(sequence), epoch, zone, zone_flow(zone),
                                              "sensor.a.flow", value, now);
  record.observation.generation = GenerationId(1);
  return record;
}

IngestRecord zone_temperature(std::uint64_t sequence, EpochId epoch, bool supply, Quantity value,
                              TimestampMs now) {
  const ZoneId zone(StrongId::from_validated("zone.a"));
  const MeasurementId measurement = supply ? zone_supply_temperature(zone) : zone_return_temperature(zone);
  IngestRecord record = make_zone_measurement(RecordSeq(sequence), epoch, zone, measurement,
                                              supply ? "sensor.a.supply" : "sensor.a.return", value, now);
  record.observation.generation = GenerationId(1);
  return record;
}

}  // namespace

int main(int argc, char** argv) {
  // The version the package reported at configure time must be the version the
  // headers and library report at run time.
  const std::string configured(COOLING_OBSERVATORY_PACKAGE_VERSION);
  if (configured != version_string()) {
    std::fprintf(stderr, "downstream: package version %s but library reports %s\n",
                 configured.c_str(), version_string().c_str());
    return 1;
  }
  std::printf("downstream: package %s, library %s, producer %s\n", configured.c_str(),
              version_string().c_str(), std::string(producer_identity()).c_str());

  const std::string directory = argc > 1 ? std::string(argv[1]) : std::string("downstream-state");
  FixedClock clock(1'800'000'000'000);

  {
    Engine engine;
    EngineOptions options;
    options.state_directory = directory;
    const Status opened = engine.open(options, clock);
    if (!opened.ok()) {
      std::fprintf(stderr, "downstream: open failed: %s\n", opened.reason().c_str());
      return 1;
    }

    const auto image = engine.image();
    const EpochId epoch = image.value().epoch;

    std::vector<IngestRecord> records;
    records.push_back(make_structure(GenerationId(1), synthetic_facility(), "downstream_synthetic"));
    records.push_back(zone_flow(1, epoch, Quantity::flow(30'000'000), clock.now_ms()));
    records.push_back(zone_temperature(2, epoch, true, Quantity::temperature(18'000), clock.now_ms()));
    records.push_back(zone_temperature(3, epoch, false, Quantity::temperature(24'000), clock.now_ms()));

    const auto ingested = engine.ingest(records);
    check(ingested.ok(), "ingest succeeds");
    if (ingested.ok()) {
      check(ingested.value().patch.records_applied == records.size(), "every record is applied");
      check(ingested.value().rejections.empty(), "no record is rejected");
    }

    // The observatory answers the core question: what cooling is arriving?
    ObserveRequest request;
    request.kind = QueryKind::Delivery;
    const auto delivery = engine.observe(request);
    check(delivery.ok(), "delivery query succeeds");
    if (delivery.ok()) {
      check(delivery.value().delivery.points.size() == 1, "one delivery point is reported");
      if (!delivery.value().delivery.points.empty()) {
        const DeliveryObservation& observation = delivery.value().delivery.points[0];
        check(observation.flow.value.has_value(), "the measured flow is reported");
        check(observation.heat_removal.value.has_value(), "the heat removal is derived");
        if (observation.heat_removal.value.has_value()) {
          // 30 L/s across 6 K at 4.18 J/(L*K) is 752 W at the nearest watt.
          check(observation.heat_removal.value.value().value == 752,
                "the heat removal is 752 W for 30 L/s across 6 K");
        }
      }
    }

    // And it says what it cannot speak about.
    request.kind = QueryKind::Coverage;
    const auto coverage = engine.observe(request);
    check(coverage.ok(), "coverage query succeeds");
    if (coverage.ok()) {
      check(!coverage.value().coverage.axes.empty(), "coverage reports its axes");
    }

    // The boundaries this runtime does not own are visible in the answer: the
    // structure generation and the evidence revisions are reported, and no
    // section claims authority over the plant.
    request.kind = QueryKind::Image;
    const auto full = engine.observe(request);
    check(full.ok(), "image query succeeds");
    if (full.ok()) {
      check(full.value().generation.value() == 1, "the adopted generation is reported");
      check(full.value().image.structure.zones().size() == 1, "the structure is held");
    }

    // An interchange request written by hand, the way a script would send one.
    const auto parsed_request = parse_request("query reserve\nloop loop.primary\n");
    check(parsed_request.ok(), "a text-protocol request parses");
    if (parsed_request.ok()) {
      const auto reserve = engine.observe(parsed_request.value());
      check(reserve.ok(), "the parsed request is answered");
    }

    // Rendering is deterministic, which is what makes an answer comparable.
    const auto first = engine.observe(ObserveRequest{QueryKind::Delivery, {}});
    const auto second = engine.observe(ObserveRequest{QueryKind::Delivery, {}});
    if (first.ok() && second.ok()) {
      check(render_text(first.value()) == render_text(second.value()),
            "two renders of one answer are identical");
    }

    const Status closed = engine.close();
    check(closed.ok(), "close succeeds");
  }

  // A second process would see what this one committed.
  {
    Engine reopened;
    EngineOptions options;
    options.state_directory = directory;
    FixedClock later(1'800'000'000'000 + 3'600'000);
    const Status opened = reopened.open(options, later);
    check(opened.ok(), "the state reopens");
    if (opened.ok()) {
      const auto image = reopened.image();
      check(image.ok() && image.value().evidence.size() == 3, "the committed evidence is restored");
      check(image.ok() && image.value().recovered, "restored evidence is marked recovered");
      const Status closed = reopened.close();
      check(closed.ok(), "the reopened engine closes");
    }
  }

  if (failures != 0) {
    std::fprintf(stderr, "downstream: %d check(s) failed\n", failures);
    return 1;
  }
  std::printf("downstream: all checks passed\n");
  return 0;
}
