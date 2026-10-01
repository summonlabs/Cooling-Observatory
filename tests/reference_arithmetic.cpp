// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The arithmetic the observatory publishes, checked against independent
// references: heat removal from flow and temperature, derates, dependency
// traversal and freshness by age. The reference implementations are written
// from the specification and share no code with the library.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "dccp/cooling_observatory/checked.hpp"
#include "reference_model.hpp"
#include "support.hpp"
#include "test_harness.hpp"

using namespace dccp::cooling_observatory;

namespace {

SubjectRef loop_ref(const char* id) {
  return SubjectRef(SubjectKind::Loop, StrongId::from_validated(id));
}

}  // namespace

CO_TEST("the reference heat-removal derivation agrees with the library") {
  // A range of flows and temperature differences covering the fixture values,
  // small values, values beside a rounding edge, and values whose exact product
  // leaves the representable range. The last group is the interesting one: the
  // library must report no figure at all rather than a wrapped one, and the
  // reference must agree about where that boundary is.
  const std::int64_t flows[] = {0,          1,          999,        1'000'000,  30'000'000,
                                123'456'789, 47'123'456, 1'000'000'000, 999'999'999};
  const std::int64_t differences[] = {0, 1, 999, 1000, 6000, 25'000, 123'456};

  cotest::Fixture fixture("reference-heat", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;

  std::uint64_t sequence = 1;
  for (const std::int64_t flow : flows) {
    for (const std::int64_t difference : differences) {
      CO_REQUIRE_OK(fixture.engine.ingest(
          {cotest::zone_measurement(sequence++, epoch, "zone.a", "flow", "s.flow",
                                    Quantity::flow(flow), fixture.clock.now_ms()),
           cotest::zone_measurement(sequence++, epoch, "zone.a", "supply_temp", "s.supply",
                                    Quantity::temperature(0), fixture.clock.now_ms()),
           cotest::zone_measurement(sequence++, epoch, "zone.a", "return_temp", "s.return",
                                    Quantity::temperature(difference), fixture.clock.now_ms())}));
      const ObservationReport report = cotest::query(fixture.engine, QueryKind::Delivery);
      const DeliveryObservation& observation = cotest::delivery_for(report.delivery, "zone.a");
      const bool representable = reference::heat_removal_representable(flow, difference, 4'180'000);
      if (!representable) {
        if (observation.heat_removal.value.has_value() ||
            observation.heat_removal.freshness != Freshness::Unknown) {
          cotest::record_failure(
              "flow " + std::to_string(flow) + " mK " + std::to_string(difference) +
                  ": the exact product leaves the representable range, so no figure may be "
                  "reported, but the answer carried " +
                  (observation.heat_removal.value.has_value()
                       ? render_quantity(observation.heat_removal.value.value())
                       : std::string("no value")) +
                  " with freshness " + std::string(to_token(observation.heat_removal.freshness)),
              __FILE__, __LINE__);
        }
        continue;
      }
      const std::int64_t expected = reference::heat_removal_watts(flow, difference, 4'180'000);
      if (!observation.heat_removal.value.has_value() ||
          observation.heat_removal.value.value().value != expected) {
        cotest::record_failure(
            "flow " + std::to_string(flow) + " mK " + std::to_string(difference) +
                ": expected " + std::to_string(expected) + ", saw " +
                (observation.heat_removal.value.has_value()
                     ? std::to_string(observation.heat_removal.value.value().value)
                     : std::string("no value")),
            __FILE__, __LINE__);
      }
    }
  }
}

CO_TEST("the library's heat removal matches the reference for every policy capacity") {
  const std::int64_t capacities[] = {4'180'000, 3'900'000, 1'200, 1, 8'000'000};
  std::uint64_t index = 0;
  for (const std::int64_t capacity : capacities) {
    cotest::Fixture fixture("reference-capacity-" + std::to_string(index++), false);
    cotest::open_fixture(fixture);
    const EpochId epoch = fixture.engine.image().value().epoch;
    IngestRecord policy;
    policy.kind = RecordKind::ThermalPolicyUpdate;
    policy.thermal_policy.coolant_heat_capacity_uj_per_l_k = capacity;
    CO_REQUIRE_OK(fixture.engine.ingest({policy}));
    CO_REQUIRE_OK(fixture.engine.ingest(
        {cotest::zone_measurement(1, epoch, "zone.a", "flow", "s.flow", Quantity::flow(12'345'678),
                                  fixture.clock.now_ms()),
         cotest::zone_measurement(2, epoch, "zone.a", "supply_temp", "s.supply",
                                  Quantity::temperature(17'500), fixture.clock.now_ms()),
         cotest::zone_measurement(3, epoch, "zone.a", "return_temp", "s.return",
                                  Quantity::temperature(23'750), fixture.clock.now_ms())}));
    const ObservationReport report = cotest::query(fixture.engine, QueryKind::Delivery);
    const DeliveryObservation& observation = cotest::delivery_for(report.delivery, "zone.a");
    CO_REQUIRE(observation.heat_removal.value.has_value());
    CO_CHECK_EQ(observation.heat_removal.value.value().value,
                reference::heat_removal_watts(12'345'678, 6250, capacity));
  }
}

CO_TEST("a derate reduces a declared capability by exactly its ratio") {
  const std::int64_t capacities[] = {40'000'000, 1, 123'456'789, 1'000'000'000};
  const std::int64_t derates[] = {0, 1, 100'000, 333'333, 500'000, 999'999, 1'000'000};
  std::uint64_t sequence = 1;
  for (const std::int64_t capacity : capacities) {
    for (const std::int64_t derate : derates) {
      cotest::Fixture fixture("reference-derate", false);
      cotest::open_fixture(fixture);
      const EpochId epoch = fixture.engine.image().value().epoch;
      CO_REQUIRE_OK(fixture.engine.ingest(
          {cotest::capability(sequence++, epoch, SubjectKind::Pump, "pump.p1",
                              Quantity::flow(capacity), derate, fixture.clock.now_ms()),
           cotest::equipment_state(sequence++, epoch, SubjectKind::Pump, "pump.p1",
                                   LifecycleState::Running, fixture.clock.now_ms())}));
      QueryFilter filter;
      filter.loop = Maybe<LoopId>::of(LoopId(StrongId::from_validated("loop.primary")));
      const ObservationReport report = cotest::query(fixture.engine, QueryKind::Reserve, filter);
      bool found = false;
      for (const ReserveObservation& scope : report.reserve.scopes) {
        if (scope.scope == loop_ref("loop.primary")) {
          CO_REQUIRE(scope.capability.value.has_value());
          CO_CHECK_EQ(scope.capability.value.value().value, reference::derated(capacity, derate));
          found = true;
        }
      }
      CO_CHECK(found);
    }
  }
}

CO_TEST("reserve headroom is capability minus measured delivery") {
  cotest::Fixture fixture("reference-reserve", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(cotest::baseline_records(epoch, fixture.clock.now_ms())));

  QueryFilter filter;
  filter.loop = Maybe<LoopId>::of(LoopId(StrongId::from_validated("loop.primary")));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Reserve, filter);
  CO_REQUIRE(!report.reserve.scopes.empty());
  for (const ReserveObservation& scope : report.reserve.scopes) {
    CO_REQUIRE(scope.capability.value.has_value());
    CO_REQUIRE(scope.delivered.value.has_value());
    CO_REQUIRE(scope.evidenced.value.has_value());
    CO_CHECK_EQ(scope.evidenced.value.value().value,
                scope.capability.value.value().value - scope.delivered.value.value().value);
    // Two pumps at 40 L/s each is 80 L/s of capability; the two zones measure
    // 30 and 15 L/s, so 35 L/s remains.
    CO_CHECK_EQ(scope.capability.value.value().value, 80'000'000);
    CO_CHECK_EQ(scope.delivered.value.value().value, 45'000'000);
    CO_CHECK_EQ(scope.evidenced.value.value().value, 35'000'000);
  }
}

CO_TEST("the traversal agrees with an independently written breadth-first walk") {
  const PlantModel model = cotest::synthetic_plant();
  const SubjectRef roots[] = {loop_ref("loop.primary"), loop_ref("loop.secondary"),
                              SubjectRef(SubjectKind::Plant, StrongId::from_validated("plant.a"))};
  for (const SubjectRef& root : roots) {
    for (const bool downstream : {true, false}) {
      TraversalOptions options;
      options.direction = downstream ? TraversalOptions::Direction::Downstream
                                     : TraversalOptions::Direction::Upstream;
      const auto traversal = traverse(model, root, options);
      CO_REQUIRE_OK(traversal);
      const reference::ReferenceTraversal walk =
          reference::reference_traverse(model, root, downstream,
                                        static_cast<std::uint32_t>(options.max_depth));
      std::vector<SubjectRef> actual;
      for (const DependencyNode& node : traversal.value().nodes) {
        actual.push_back(node.subject);
      }
      std::sort(actual.begin(), actual.end());
      const bool same = actual.size() == walk.nodes.size() &&
                        std::equal(actual.begin(), actual.end(), walk.nodes.begin());
      CO_CHECK(same);
    }
  }
}

CO_TEST("the traversal depth for every node agrees with the reference walk") {
  const PlantModel model = cotest::synthetic_plant();
  const auto traversal = traverse(model, loop_ref("loop.secondary"), TraversalOptions{});
  CO_REQUIRE_OK(traversal);
  const reference::ReferenceTraversal walk =
      reference::reference_traverse(model, loop_ref("loop.secondary"), true, 64);
  for (const auto& entry : walk.with_depth) {
    bool found = false;
    for (const DependencyNode& node : traversal.value().nodes) {
      if (node.subject == entry.first) {
        CO_CHECK_EQ(node.depth, entry.second);
        found = true;
      }
    }
    CO_CHECK(found);
  }
  CO_CHECK_EQ(traversal.value().nodes.size(), walk.nodes.size());
}

CO_TEST("flow summation agrees with an independent checked sum") {
  const std::vector<std::int64_t> cases[] = {
      {0}, {1, 2, 3}, {kMaxInt64, 0}, {kMaxInt64, -1}, {1'000'000, 2'000'000, -500'000},
  };
  for (const std::vector<std::int64_t>& values : cases) {
    const std::optional<std::int64_t> expected = reference::sum_flows(values);
    std::int64_t total = 0;
    bool overflowed = false;
    for (const std::int64_t value : values) {
      const std::optional<std::int64_t> next = checked_add(total, value);
      if (!next.has_value()) {
        overflowed = true;
        break;
      }
      total = next.value();
    }
    CO_CHECK_EQ(overflowed, !expected.has_value());
    if (expected.has_value()) {
      CO_CHECK_EQ(total, expected.value());
    }
  }
}

CO_TEST("the freshness by age rule agrees with the reference across the boundary") {
  const FreshnessPolicy policy;
  const std::int64_t bounds[] = {policy.delivery_max_age_ms, policy.condition_max_age_ms,
                                 policy.capability_max_age_ms, 0};
  for (const std::int64_t bound : bounds) {
    for (const std::int64_t age : {bound - 1, bound, bound + 1}) {
      const bool expected = reference::fresh_by_age(cotest::at(0), cotest::at(age), bound);
      Observation observation;
      observation.kind = ObservationKind::Measurement;
      observation.observed_at_ms = cotest::at(0);
      observation.epoch = EpochId(1);
      FreshnessPolicy local;
      local.delivery_max_age_ms = bound;
      local.condition_max_age_ms = bound;
      local.capability_max_age_ms = bound;
      local.structure_max_age_ms = bound;
      local.reserve_max_age_ms = bound;
      local.fault_max_age_ms = bound;
      const Freshness classified = classify(observation, local, cotest::at(age), EpochId(1), GenerationId(1));
      CO_CHECK_EQ(classified == Freshness::Fresh, expected);
    }
  }
}

CO_TEST("a measured flow above the declared capability is reported as over delivery") {
  cotest::Fixture fixture("reference-overdelivery", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::capability(1, epoch, SubjectKind::Pump, "pump.p1", Quantity::flow(10'000'000), 0,
                          fixture.clock.now_ms()),
       cotest::equipment_state(2, epoch, SubjectKind::Pump, "pump.p1", LifecycleState::Running,
                               fixture.clock.now_ms()),
       cotest::zone_measurement(3, epoch, "zone.a", "flow", "s.flow", Quantity::flow(40'000'000),
                                fixture.clock.now_ms()),
       cotest::zone_measurement(4, epoch, "zone.a", "diff_pressure", "s.dp",
                                Quantity::pressure(90'000), fixture.clock.now_ms())}));
  QueryFilter filter;
  filter.zone = Maybe<ZoneId>::of(ZoneId(StrongId::from_validated("zone.a")));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Divergence, filter);
  const DivergenceFinding& finding = cotest::divergence_for(report.divergence, "dp.zone.a");
  CO_CHECK(finding.state == DivergenceState::OverDelivery);
  CO_REQUIRE(finding.delta.value.has_value());
  CO_CHECK_EQ(finding.delta.value.value().value, 30'000'000);
}

CO_TEST("a measured flow below the declared capability is reported as under delivery") {
  cotest::Fixture fixture("reference-underdelivery", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::capability(1, epoch, SubjectKind::Pump, "pump.p1", Quantity::flow(40'000'000), 0,
                          fixture.clock.now_ms()),
       cotest::equipment_state(2, epoch, SubjectKind::Pump, "pump.p1", LifecycleState::Running,
                               fixture.clock.now_ms()),
       cotest::zone_measurement(3, epoch, "zone.a", "flow", "s.flow", Quantity::flow(20'000'000),
                                fixture.clock.now_ms()),
       cotest::zone_measurement(4, epoch, "zone.a", "diff_pressure", "s.dp",
                                Quantity::pressure(90'000), fixture.clock.now_ms())}));
  QueryFilter filter;
  filter.zone = Maybe<ZoneId>::of(ZoneId(StrongId::from_validated("zone.a")));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Divergence, filter);
  const DivergenceFinding& finding = cotest::divergence_for(report.divergence, "dp.zone.a");
  CO_CHECK(finding.state == DivergenceState::UnderDelivery);
  CO_REQUIRE(finding.delta.value.has_value());
  CO_CHECK_EQ(finding.delta.value.value().value, -20'000'000);
}

CO_TEST("zero measured flow against a declared capability is reported as dry") {
  cotest::Fixture fixture("reference-dry", false);
  cotest::open_fixture(fixture);
  const EpochId epoch = fixture.engine.image().value().epoch;
  CO_REQUIRE_OK(fixture.engine.ingest(
      {cotest::capability(1, epoch, SubjectKind::Pump, "pump.p1", Quantity::flow(40'000'000), 0,
                          fixture.clock.now_ms()),
       cotest::equipment_state(2, epoch, SubjectKind::Pump, "pump.p1", LifecycleState::Running,
                               fixture.clock.now_ms()),
       cotest::zone_measurement(3, epoch, "zone.a", "flow", "s.flow", Quantity::flow(0),
                                fixture.clock.now_ms())}));
  QueryFilter filter;
  filter.zone = Maybe<ZoneId>::of(ZoneId(StrongId::from_validated("zone.a")));
  const ObservationReport report = cotest::query(fixture.engine, QueryKind::Divergence, filter);
  const DivergenceFinding& finding = cotest::divergence_for(report.divergence, "dp.zone.a");
  CO_CHECK(finding.state == DivergenceState::Dry);
}