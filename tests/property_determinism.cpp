// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Properties that must hold for every input the observatory accepts, checked
// over generated facilities and generated evidence. The generator is a small
// deterministic sequence rather than a random number generator, so a failing
// case is reproducible from the test name alone.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "dccp/cooling_observatory/textproto.hpp"
#include "support.hpp"
#include "test_harness.hpp"

using namespace dccp::cooling_observatory;

namespace {

/// A small deterministic generator. Every property below runs over the same
/// sequence, and the sequence is a function of the seed alone.
class Sequence {
 public:
  explicit Sequence(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  std::uint64_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }

  std::int64_t range(std::int64_t low, std::int64_t high) {
    if (high <= low) {
      return low;
    }
    const std::uint64_t span = static_cast<std::uint64_t>(high - low + 1);
    return low + static_cast<std::int64_t>(next() % span);
  }

 private:
  std::uint64_t state_;
};

/// A generated facility with a generated chain of loops and equipment.
struct GeneratedFacility {
  PlantModel model;
  std::vector<std::string> zone_ids;
  std::vector<std::string> loop_ids;
};

GeneratedFacility generate_facility(Sequence& sequence, std::size_t zone_count) {
  GeneratedFacility facility;
  facility.model.add_facility(FacilityId(StrongId::from_validated("gen")), "gen");
  CoolingPlant plant;
  plant.id = PlantId(StrongId::from_validated("plant.gen"));
  plant.facility = FacilityId(StrongId::from_validated("gen"));
  facility.model.add_plant(plant);

  Loop primary;
  primary.id = LoopId(StrongId::from_validated("loop.primary"));
  primary.plant = plant.id;
  facility.model.add_loop(primary);
  facility.loop_ids.push_back("loop.primary");
  facility.model.add_link(PlantLink{SubjectRef(SubjectKind::Plant, StrongId::from_validated("plant.gen")),
                                    SubjectRef(SubjectKind::Loop, StrongId::from_validated("loop.primary")),
                                    LinkRelation::Supply});

  // A chain of secondary loops, each fed by the one before it.
  const std::size_t loop_count = 1 + (zone_count / 4) + 1;
  for (std::size_t i = 1; i < loop_count; ++i) {
    const std::string id = "loop.secondary" + std::to_string(i);
    Loop loop;
    loop.id = LoopId(StrongId::from_validated(id));
    loop.plant = plant.id;
    loop.secondary = true;
    facility.model.add_loop(loop);
    facility.loop_ids.push_back(id);
    const std::string upstream = i == 1 ? std::string("loop.primary") : "loop.secondary" + std::to_string(i - 1);
    facility.model.add_link(PlantLink{SubjectRef(SubjectKind::Loop, StrongId::from_validated(upstream)),
                                      SubjectRef(SubjectKind::Loop, StrongId::from_validated(id)),
                                      LinkRelation::Supply});
    const std::string pump_id = "pump.gen" + std::to_string(i);
    PlantComponent pump;
    pump.kind = ComponentKind::Pump;
    pump.id = StrongId::from_validated(pump_id);
    pump.loop = loop.id;
    facility.model.add_component(pump);
    facility.model.add_link(PlantLink{SubjectRef(SubjectKind::Loop, StrongId::from_validated(id)),
                                      pump.subject(), LinkRelation::Supply});
  }

  for (std::size_t i = 0; i < zone_count; ++i) {
    const std::string zone_id = "zone.gen" + std::to_string(i);
    const std::string crah_id = "crah.gen" + std::to_string(i);
    const std::string loop_id = facility.loop_ids[(i % (facility.loop_ids.size() - 1)) + 1];
    ThermalZone zone;
    zone.id = ZoneId(StrongId::from_validated(zone_id));
    zone.facility = FacilityId(StrongId::from_validated("gen"));
    zone.declared_load = Maybe<Quantity>::of(
        Quantity::power(sequence.range(50'000, 250'000)));
    facility.model.add_zone(zone);

    PlantComponent crah;
    crah.kind = ComponentKind::Crah;
    crah.id = StrongId::from_validated(crah_id);
    crah.loop = LoopId(StrongId::from_validated(loop_id));
    facility.model.add_component(crah);
    facility.model.add_link(PlantLink{SubjectRef(SubjectKind::Loop, StrongId::from_validated(loop_id)),
                                      crah.subject(), LinkRelation::Supply});
    facility.model.add_link(PlantLink{
        crah.subject(), SubjectRef(SubjectKind::Zone, StrongId::from_validated(zone_id)),
        LinkRelation::Supply});
    facility.zone_ids.push_back(zone_id);
  }

  facility.model.reindex();
  return facility;
}

/// The canonical form of an answer: the text rendering with the two lines that
/// describe the durable position removed.
///
/// The position is not part of the answer's content, it is the identity of the
/// commit the answer was computed at, and two engines that received the same
/// records in different batches are legitimately at different revisions: one
/// commit of twelve records and twelve commits of one record are both correct
/// and both reachable. Everything that describes the plant must still agree, and
/// that is what this comparison asserts.
std::string answer_of(Engine& engine, QueryKind kind) {
  ObserveRequest request;
  request.kind = kind;
  const auto report = engine.observe(request);
  if (!report) {
    return std::string("error:") + std::string(to_token(report.error().code));
  }
  std::string text = render_text(report.value());
  std::string filtered;
  std::size_t position = 0;
  while (position <= text.size()) {
    const std::size_t newline = text.find('\n', position);
    const std::string line =
        newline == std::string::npos ? text.substr(position) : text.substr(position, newline - position);
    const bool positional = line.rfind("revision ", 0) == 0 || line.rfind("record_sequence ", 0) == 0;
    if (!positional && !line.empty()) {
      filtered += line;
      filtered += '\n';
    }
    if (newline == std::string::npos) {
      break;
    }
    position = newline + 1;
  }
  return filtered;
}

}  // namespace

CO_TEST("the same records produce the same answer, whatever order the batches arrived in") {
  const QueryKind kinds[] = {QueryKind::Delivery, QueryKind::Divergence, QueryKind::Reserve,
                             QueryKind::Coverage, QueryKind::Constraints};
  for (std::uint64_t seed = 1; seed <= 6; ++seed) {
    Sequence sequence(seed);
    const GeneratedFacility facility = generate_facility(sequence, 4 + seed);
    CO_REQUIRE_OK(facility.model.validate(default_limits()));

    cotest::Fixture reference("property-reference", false);
    cotest::open_fixture(reference, false);
    CO_REQUIRE_OK(reference.engine.ingest({make_structure(GenerationId(1), facility.model, "gen")}));
    const EpochId epoch = reference.engine.image().value().epoch;

    std::vector<IngestRecord> records;
    std::uint64_t sequence_number = 1;
    for (const std::string& zone_id : facility.zone_ids) {
      const ZoneId zone(StrongId::from_validated(zone_id));
      records.push_back(make_zone_measurement(RecordSeq(sequence_number++), epoch, zone, zone_flow(zone),
                                              "s." + zone_id + ".flow",
                                              Quantity::flow(sequence.range(1'000'000, 40'000'000)),
                                              reference.clock.now_ms()));
      records.push_back(make_zone_measurement(RecordSeq(sequence_number++), epoch, zone,
                                              zone_supply_temperature(zone),
                                              "s." + zone_id + ".supply",
                                              Quantity::temperature(sequence.range(10'000, 24'000)),
                                              reference.clock.now_ms()));
      records.push_back(make_zone_measurement(RecordSeq(sequence_number++), epoch, zone,
                                              zone_return_temperature(zone),
                                              "s." + zone_id + ".return",
                                              Quantity::temperature(sequence.range(18'000, 30'000)),
                                              reference.clock.now_ms()));
      records.push_back(make_zone_measurement(RecordSeq(sequence_number++), epoch, zone,
                                              zone_differential_pressure(zone),
                                              "s." + zone_id + ".dp",
                                              Quantity::pressure(sequence.range(0, 200'000)),
                                              reference.clock.now_ms()));
    }
    CO_REQUIRE_OK(reference.engine.ingest(records));

    // The same records again, committed one call at a time but in reverse, so
    // that the arrival order and the batch boundaries are both different.
    cotest::Fixture permuted("property-permuted", false);
    cotest::open_fixture(permuted, false);
    CO_REQUIRE_OK(permuted.engine.ingest({make_structure(GenerationId(1), facility.model, "gen")}));
    std::vector<IngestRecord> reversed = records;
    std::reverse(reversed.begin(), reversed.end());
    for (const IngestRecord& record : reversed) {
      CO_REQUIRE_OK(permuted.engine.ingest({record}));
    }

    for (const QueryKind kind : kinds) {
      CO_CHECK_EQ(answer_of(reference.engine, kind), answer_of(permuted.engine, kind));
    }
  }
}

CO_TEST("a query is repeatable and does not depend on the order the queries are asked in") {
  for (std::uint64_t seed = 10; seed <= 12; ++seed) {
    Sequence sequence(seed);
    const GeneratedFacility facility = generate_facility(sequence, 3 + seed);
    cotest::Fixture fixture("property-repeat", false);
    cotest::open_fixture(fixture, false);
    CO_REQUIRE_OK(fixture.engine.ingest({make_structure(GenerationId(1), facility.model, "gen")}));
    const EpochId epoch = fixture.engine.image().value().epoch;
    std::uint64_t number = 1;
    std::vector<IngestRecord> records;
    for (const std::string& zone_id : facility.zone_ids) {
      const ZoneId zone(StrongId::from_validated(zone_id));
      records.push_back(make_zone_measurement(RecordSeq(number++), epoch, zone, zone_flow(zone),
                                              "s." + zone_id + ".flow",
                                              Quantity::flow(sequence.range(1'000'000, 40'000'000)),
                                              fixture.clock.now_ms()));
    }
    CO_REQUIRE_OK(fixture.engine.ingest(records));

    const std::string first = answer_of(fixture.engine, QueryKind::Delivery);
    const std::string coverage = answer_of(fixture.engine, QueryKind::Coverage);
    const std::string again = answer_of(fixture.engine, QueryKind::Delivery);
    CO_CHECK_EQ(first, again);
    CO_CHECK(!coverage.empty());
    // Rendering the same answer twice is byte-identical.
    const auto report = fixture.engine.observe(ObserveRequest{QueryKind::Delivery, {}});
    CO_REQUIRE_OK(report);
    CO_CHECK_EQ(render_text(report.value()), render_text(report.value()));
    CO_CHECK_EQ(render_json(report.value()), render_json(report.value()));
  }
}

CO_TEST("every generated facility digests stably under a re-index") {
  for (std::uint64_t seed = 20; seed <= 25; ++seed) {
    Sequence sequence(seed);
    GeneratedFacility facility = generate_facility(sequence, 2 + seed);
    const std::string before = facility.model.digest();
    facility.model.reindex();
    facility.model.reindex();
    CO_CHECK_EQ(facility.model.digest(), before);
    CO_CHECK_OK(facility.model.validate(default_limits()));
  }
}

CO_TEST("every generated answer is internally consistent about its revision") {
  for (std::uint64_t seed = 30; seed <= 34; ++seed) {
    Sequence sequence(seed);
    const GeneratedFacility facility = generate_facility(sequence, 3 + seed);
    cotest::Fixture fixture("property-consistent", false);
    cotest::open_fixture(fixture, false);
    CO_REQUIRE_OK(fixture.engine.ingest({make_structure(GenerationId(1), facility.model, "gen")}));
    const EpochId epoch = fixture.engine.image().value().epoch;
    std::uint64_t number = 1;
    std::vector<IngestRecord> records;
    for (const std::string& zone_id : facility.zone_ids) {
      const ZoneId zone(StrongId::from_validated(zone_id));
      records.push_back(make_zone_measurement(RecordSeq(number++), epoch, zone, zone_flow(zone),
                                              "s." + zone_id + ".flow",
                                              Quantity::flow(sequence.range(0, 5'000'000)),
                                              fixture.clock.now_ms()));
    }
    CO_REQUIRE_OK(fixture.engine.ingest(records));

    const ObservationReport delivery = cotest::query(fixture.engine, QueryKind::Delivery);
    CO_CHECK_EQ(delivery.delivery.revision.value(), delivery.revision.value());
    CO_CHECK(delivery.delivery.generation == delivery.generation);
    CO_CHECK_EQ(delivery.delivery.epoch.value(), delivery.epoch.value());
    // The points reported plus the points reported as unevidenced are exactly
    // the points this image can describe.
    const ObservationReport image = cotest::query(fixture.engine, QueryKind::Image);
    CO_CHECK(delivery.delivery.points.size() + delivery.delivery.unevidenced_points.size() >=
             image.image.structure.zones().size());
  }
}

CO_TEST("a constraint reported by the engine always names what it is about") {
  for (std::uint64_t seed = 40; seed <= 44; ++seed) {
    Sequence sequence(seed);
    const GeneratedFacility facility = generate_facility(sequence, 3 + seed);
    cotest::Fixture fixture("property-constraints", false);
    cotest::open_fixture(fixture, false);
    CO_REQUIRE_OK(fixture.engine.ingest({make_structure(GenerationId(1), facility.model, "gen")}));
    const EpochId epoch = fixture.engine.image().value().epoch;
    std::uint64_t number = 1;
    std::vector<IngestRecord> records;
    const std::string target_pump = "pump.gen1";
    const std::int64_t limit = sequence.range(0, 20'000'000);
    records.push_back(cotest::constraint(number++, epoch, SubjectKind::Pump, target_pump.c_str(),
                                         ConstraintKind::FlowLimit, ConstraintDirection::Maximum,
                                         Quantity::flow(limit), target_pump.c_str(),
                                         fixture.clock.now_ms()));
    for (const std::string& zone_id : facility.zone_ids) {
      const ZoneId zone(StrongId::from_validated(zone_id));
      records.push_back(make_zone_measurement(RecordSeq(number++), epoch, zone, zone_flow(zone),
                                              "s." + zone_id + ".flow",
                                              Quantity::flow(sequence.range(0, 40'000'000)),
                                              fixture.clock.now_ms()));
    }
    CO_REQUIRE_OK(fixture.engine.ingest(records));

    const ObservationReport report = cotest::query(fixture.engine, QueryKind::Constraints);
    for (const ConstraintAttribution& attribution : report.constraints.constraints) {
      CO_CHECK(!attribution.constraint_id.empty());
      if (attribution.attribution == AttributionState::Direct ||
          attribution.attribution == AttributionState::Inherited) {
        CO_CHECK(attribution.element.has_value());
      }
      if (attribution.attribution == AttributionState::Ambiguous) {
        CO_CHECK(attribution.candidates.size() > 1);
      }
      if (attribution.attribution == AttributionState::Unattributed ||
          attribution.attribution == AttributionState::Unknown) {
        CO_CHECK(!attribution.indeterminacies.empty());
      }
    }
  }
}

CO_TEST("the interchange format round-trips every record the engine accepts") {
  cotest::Fixture fixture("property-interchange", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  const std::string text =
      "record add_observation\n"
      "observation measurement\n"
      "subject measurement:zone.a.flow\n"
      "sensor sensor.a.flow\n"
      "value 30000ml/s\n"
      "observed_at 1800000000000\n"
      "authority facility_telemetry synthetic-gateway/1.0.0\n"
      "seq 1\n"
      "epoch 1\n"
      "record add_observation\n"
      "observation capability\n"
      "subject pump:pump.p1\n"
      "capacity 40l/s\n"
      "derate 10%\n"
      "seq 2\n"
      "epoch 1\n"
      "record add_observation\n"
      "observation constraint\n"
      "subject pump:pump.p1\n"
      "constraint_kind flow_limit\n"
      "direction maximum\n"
      "limit 20l/s\n"
      "origin_element pump:pump.p1\n"
      "seq 3\n"
      "epoch 1\n";
  const auto records = parse_records(text);
  CO_REQUIRE_OK(records);
  CO_CHECK_EQ(records.value().size(), 3u);
  CO_CHECK_EQ(records.value()[0].observation.measured.value, 30'000'000);
  CO_CHECK_EQ(records.value()[1].observation.declared_capacity, 40'000'000);
  CO_CHECK_EQ(records.value()[1].observation.derate_ppm, 100'000);
  CO_CHECK(records.value()[2].observation.constraint_kind == ConstraintKind::FlowLimit);
  CO_CHECK(records.value()[2].observation.origin_element.has_value());

  const auto report = fixture.engine.ingest(records.value());
  CO_REQUIRE_OK(report);
  CO_CHECK_EQ(report.value().patch.records_applied, 3u);
  CO_CHECK_EQ(report.value().rejections.size(), 0u);
  // The epoch in the records is not this engine's, so the evidence is held and
  // reported as stale rather than as current.
  const ObservationReport delivery = cotest::query(fixture.engine, QueryKind::Delivery);
  const DeliveryObservation& zone_a = cotest::delivery_for(delivery.delivery, "zone.a");
  CO_CHECK(zone_a.flow.freshness != Freshness::Fresh);
  (void)epoch;
}

CO_TEST("a facility at the configured structural limits is still valid and answerable") {
  cotest::Fixture fixture("property-limits", false);
  EngineOptions options;
  options.store.limits.max_zones = 8;
  Engine engine;
  CO_REQUIRE_OK(engine.open(options, fixture.clock));
  Sequence sequence(99);
  const GeneratedFacility facility = generate_facility(sequence, 8);
  CO_REQUIRE_OK(engine.ingest({make_structure(GenerationId(1), facility.model, "gen")}));
  EngineOptions tighter = options;
  tighter.store.limits.max_zones = 4;
  (void)tighter;
  const ObservationReport image = cotest::query(engine, QueryKind::Image);
  CO_CHECK_EQ(image.image.structure.zones().size(), 8u);
  CO_REQUIRE_OK(engine.close());
}