// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The structural model: canonical ordering, digest stability, validation, the
// adjacency index and the dependency traversal, including the cases where a
// traversal is legitimately incomplete.

#include <algorithm>
#include <string>
#include <vector>

#include "dccp/cooling_observatory/dependency.hpp"
#include "support.hpp"
#include "test_harness.hpp"

using namespace dccp::cooling_observatory;

namespace {

SubjectRef loop_ref(const char* id) {
  return SubjectRef(SubjectKind::Loop, StrongId::from_validated(id));
}

SubjectRef pump_ref(const char* id) {
  return SubjectRef(SubjectKind::Pump, StrongId::from_validated(id));
}

SubjectRef zone_ref(const char* id) {
  return SubjectRef(SubjectKind::Zone, StrongId::from_validated(id));
}

bool contains(const std::vector<DependencyNode>& nodes, const SubjectRef& subject) {
  return std::any_of(nodes.begin(), nodes.end(),
                     [&subject](const DependencyNode& node) { return node.subject == subject; });
}

std::size_t depth_of(const std::vector<DependencyNode>& nodes, const SubjectRef& subject) {
  for (const DependencyNode& node : nodes) {
    if (node.subject == subject) {
      return node.depth;
    }
  }
  return 999;
}

}  // namespace

CO_TEST("the synthetic fixture is valid and its identities are unique") {
  const PlantModel model = cotest::synthetic_plant();
  CO_CHECK_OK(model.validate(default_limits()));
  CO_CHECK_EQ(model.plants().size(), 1u);
  CO_CHECK_EQ(model.loops().size(), 2u);
  CO_CHECK_EQ(model.components().size(), 6u);
  CO_CHECK_EQ(model.zones().size(), 2u);
  CO_CHECK_EQ(model.links().size(), 10u);
  CO_CHECK(model.knows(loop_ref("loop.primary")));
  CO_CHECK(model.knows(zone_ref("zone.a")));
  CO_CHECK(model.knows(pump_ref("pump.p1")));
  CO_CHECK(!model.knows(pump_ref("pump.nope")));
  CO_CHECK(!model.knows(SubjectRef()));
}

CO_TEST("the digest is independent of the order the model was built in") {
  const PlantModel first = cotest::synthetic_plant();

  // The same plant, built in the opposite order everywhere: links first and
  // last-to-first, components and loops reversed, the facility added last.
  PlantModel second;
  const auto link = [&second](SubjectKind from_kind, const char* from, SubjectKind to_kind,
                              const char* to, LinkRelation relation) {
    PlantLink entry;
    entry.from = SubjectRef(from_kind, StrongId::from_validated(from));
    entry.to = SubjectRef(to_kind, StrongId::from_validated(to));
    entry.relation = relation;
    second.add_link(entry);
  };
  link(SubjectKind::Crah, "crah.h2", SubjectKind::Zone, "zone.b", LinkRelation::Supply);
  link(SubjectKind::Crah, "crah.h1", SubjectKind::Zone, "zone.a", LinkRelation::Supply);
  link(SubjectKind::Cdu, "cdu.d1", SubjectKind::Crah, "crah.h2", LinkRelation::Supply);
  link(SubjectKind::Cdu, "cdu.d1", SubjectKind::Crah, "crah.h1", LinkRelation::Supply);
  link(SubjectKind::Loop, "loop.secondary", SubjectKind::Cdu, "cdu.d1", LinkRelation::Supply);
  link(SubjectKind::Loop, "loop.primary", SubjectKind::Pump, "pump.p2", LinkRelation::Supply);
  link(SubjectKind::Loop, "loop.primary", SubjectKind::Pump, "pump.p1", LinkRelation::Supply);
  link(SubjectKind::Loop, "loop.primary", SubjectKind::Chiller, "chiller.c1", LinkRelation::Supply);
  link(SubjectKind::Loop, "loop.primary", SubjectKind::Loop, "loop.secondary", LinkRelation::Supply);
  link(SubjectKind::Plant, "plant.a", SubjectKind::Loop, "loop.primary", LinkRelation::Supply);

  const struct {
    ComponentKind kind;
    const char* id;
    const char* loop;
  } components[] = {
      {ComponentKind::Crah, "crah.h2", "loop.secondary"},
      {ComponentKind::Crah, "crah.h1", "loop.secondary"},
      {ComponentKind::Cdu, "cdu.d1", "loop.secondary"},
      {ComponentKind::Chiller, "chiller.c1", "loop.primary"},
      {ComponentKind::Pump, "pump.p2", "loop.primary"},
      {ComponentKind::Pump, "pump.p1", "loop.primary"},
  };
  for (const auto& entry : components) {
    PlantComponent component;
    component.kind = entry.kind;
    component.id = StrongId::from_validated(entry.id);
    component.loop = LoopId(StrongId::from_validated(entry.loop));
    second.add_component(std::move(component));
  }

  const struct {
    const char* id;
    std::int64_t load;
  } zones[] = {{"zone.b", 120'000}, {"zone.a", 180'000}};
  for (const auto& entry : zones) {
    ThermalZone zone;
    zone.id = ZoneId(StrongId::from_validated(entry.id));
    zone.facility = FacilityId(StrongId::from_validated("dc1"));
    zone.declared_load = Maybe<Quantity>::of(Quantity::power(entry.load));
    second.add_zone(std::move(zone));
  }

  Loop secondary;
  secondary.id = LoopId(StrongId::from_validated("loop.secondary"));
  secondary.plant = PlantId(StrongId::from_validated("plant.a"));
  secondary.secondary = true;
  secondary.label = "secondary";
  second.add_loop(secondary);
  Loop primary;
  primary.id = LoopId(StrongId::from_validated("loop.primary"));
  primary.plant = PlantId(StrongId::from_validated("plant.a"));
  primary.label = "primary";
  second.add_loop(primary);

  CoolingPlant plant;
  plant.id = PlantId(StrongId::from_validated("plant.a"));
  plant.facility = FacilityId(StrongId::from_validated("dc1"));
  plant.label = "plant_a";
  second.add_plant(plant);
  second.add_facility(FacilityId(StrongId::from_validated("dc1")), "dc1");
  second.reindex();

  CO_CHECK_EQ(second.digest(), first.digest());

  PlantModel third = first;
  third.add_zone(ThermalZone{ZoneId(StrongId::from_validated("zone.c")),
                             FacilityId(StrongId::from_validated("dc1")), "extra", {}, {}});
  third.reindex();
  CO_CHECK_NE(third.digest(), first.digest());
}

CO_TEST("the digest is unambiguous about where one identity ends") {
  // A model whose identities are "ab" + "c" must not digest the same as one
  // whose identities are "a" + "bc". Length prefixes are what prevent that.
  PlantModel first;
  first.add_facility(FacilityId(StrongId::from_validated("ab")), "");
  first.add_facility(FacilityId(StrongId::from_validated("c")), "");
  first.reindex();
  PlantModel second;
  second.add_facility(FacilityId(StrongId::from_validated("a")), "");
  second.add_facility(FacilityId(StrongId::from_validated("bc")), "");
  second.reindex();
  CO_CHECK_NE(first.digest(), second.digest());
}

CO_TEST("duplicate additions collapse rather than duplicating an element") {
  PlantModel model = cotest::synthetic_plant();
  const std::string before = model.digest();
  PlantComponent pump;
  pump.kind = ComponentKind::Pump;
  pump.id = StrongId::from_validated("pump.p1");
  pump.loop = LoopId(StrongId::from_validated("loop.primary"));
  model.add_component(pump);
  model.add_link(PlantLink{loop_ref("loop.primary"), pump_ref("pump.p1"), LinkRelation::Supply});
  model.reindex();
  CO_CHECK_EQ(model.components().size(), 6u);
  CO_CHECK_EQ(model.links().size(), 10u);
  CO_CHECK_EQ(model.digest(), before);
}

CO_TEST("validation reports a dangling loop reference and a duplicate subject") {
  PlantModel dangling;
  Loop loop;
  loop.id = LoopId(StrongId::from_validated("loop.x"));
  loop.plant = PlantId(StrongId::from_validated("plant.missing"));
  dangling.add_loop(loop);
  dangling.reindex();
  const Status status = dangling.validate(default_limits());
  CO_CHECK(!status.ok());
  CO_CHECK_EQ(status.reason(), std::string("loop_plant_missing"));

  // A component added twice is the same element stated twice, and reindexing
  // collapses it: two elements of one kind may not share an identity, and the
  // canonical model keeps one of them rather than reporting an ambiguity that
  // the producer did not create.
  PlantModel duplicate;
  PlantComponent pump;
  pump.kind = ComponentKind::Pump;
  pump.id = StrongId::from_validated("pump.dup");
  duplicate.add_component(pump);
  duplicate.add_component(pump);
  duplicate.reindex();
  CO_CHECK_EQ(duplicate.components().size(), 1u);
  CO_CHECK_OK(duplicate.validate(default_limits()));

  // Validation does report a duplicate that survived into the model, which is
  // what a hand-assembled model without a reindex produces.
  PlantModel unindexed;
  unindexed.add_component(pump);
  unindexed.add_component(pump);
  const Status unindexed_status = unindexed.validate(default_limits());
  CO_CHECK(!unindexed_status.ok());
  CO_CHECK_EQ(unindexed_status.reason(), std::string("duplicate_subject"));
}

CO_TEST("validation enforces the configured collection limits") {
  Limits limits = default_limits();
  limits.max_zones = 1;
  const PlantModel model = cotest::synthetic_plant();
  const Status status = model.validate(limits);
  CO_CHECK(!status.ok());
  CO_CHECK(status.reason().find("max_zones") != std::string::npos);
}

CO_TEST("traversal reaches every downstream element exactly once, breadth first") {
  const PlantModel model = cotest::synthetic_plant();
  const auto traversal = traverse(model, loop_ref("loop.secondary"), TraversalOptions{}, GenerationId(3),
                                  Revision(4));
  CO_REQUIRE_OK(traversal);
  CO_CHECK(!traversal.value().truncated);
  CO_CHECK_EQ(traversal.value().generation.value(), 3u);
  CO_CHECK_EQ(traversal.value().revision.value(), 4u);
  // loop.secondary, cdu.d1, crah.h1, crah.h2, zone.a and zone.b: six nodes, each
  // reached once.
  CO_CHECK_EQ(traversal.value().nodes.size(), 6u);
  CO_CHECK(contains(traversal.value().nodes, zone_ref("zone.a")));
  CO_CHECK(contains(traversal.value().nodes, zone_ref("zone.b")));
  CO_CHECK_EQ(depth_of(traversal.value().nodes, loop_ref("loop.secondary")), 0u);
  CO_CHECK_EQ(depth_of(traversal.value().nodes, zone_ref("zone.a")), 3u);

  // The nodes are ordered by depth and then by identity, so two runs over the
  // same structure produce the same sequence.
  std::vector<DependencyNode> sorted = traversal.value().nodes;
  std::sort(sorted.begin(), sorted.end());
  CO_CHECK(sorted == traversal.value().nodes);
}

CO_TEST("traversal upstream reaches the plant and stops there") {
  const PlantModel model = cotest::synthetic_plant();
  TraversalOptions options;
  options.direction = TraversalOptions::Direction::Upstream;
  const auto traversal = traverse(model, zone_ref("zone.a"), options);
  CO_REQUIRE_OK(traversal);
  CO_CHECK(contains(traversal.value().nodes, loop_ref("loop.secondary")));
  CO_CHECK(contains(traversal.value().nodes, loop_ref("loop.primary")));
  CO_CHECK(contains(traversal.value().nodes, SubjectRef(SubjectKind::Plant, StrongId::from_validated("plant.a"))));
  CO_CHECK(!contains(traversal.value().nodes, zone_ref("zone.b")));
}

CO_TEST("a traversal that runs out of depth says so instead of pretending to finish") {
  const PlantModel model = cotest::synthetic_plant();
  TraversalOptions options;
  options.max_depth = 1;
  const auto traversal = traverse(model, loop_ref("loop.secondary"), options);
  CO_REQUIRE_OK(traversal);
  CO_CHECK(traversal.value().truncated);
  CO_CHECK_EQ(traversal.value().truncated_by, std::string("max_traversal_depth"));
  CO_CHECK(!contains(traversal.value().nodes, zone_ref("zone.a")));
}

CO_TEST("a traversal with a node budget reports the budget as the reason") {
  const PlantModel model = cotest::synthetic_plant();
  TraversalOptions options;
  options.max_nodes = 2;
  const auto traversal = traverse(model, loop_ref("loop.primary"), options);
  CO_REQUIRE_OK(traversal);
  CO_CHECK(traversal.value().truncated);
  CO_CHECK_EQ(traversal.value().truncated_by, std::string("max_traversal_nodes"));
}

CO_TEST("traversal refuses an unknown root with a reason rather than an empty answer") {
  const PlantModel model = cotest::synthetic_plant();
  const auto traversal = traverse(model, pump_ref("pump.missing"));
  CO_CHECK_ERR(traversal, Code::MissingEvidence);
  CO_CHECK(traversal.error().reason == std::string("traversal_root_unknown"));
}

CO_TEST("a cycle in the structure does not hang or repeat a node") {
  PlantModel model = cotest::synthetic_plant();
  // Close a loop: the secondary loop feeds back into the primary loop.
  model.add_link(PlantLink{loop_ref("loop.secondary"), loop_ref("loop.primary"), LinkRelation::Return});
  model.reindex();
  const auto traversal = traverse(model, loop_ref("loop.primary"), TraversalOptions{});
  CO_REQUIRE_OK(traversal);
  std::vector<SubjectRef> seen;
  for (const DependencyNode& node : traversal.value().nodes) {
    CO_CHECK(std::find(seen.begin(), seen.end(), node.subject) == seen.end());
    seen.push_back(node.subject);
  }
}

CO_TEST("an edge whose endpoint is absent is reported as dangling") {
  PlantModel model = cotest::synthetic_plant();
  model.add_link(PlantLink{loop_ref("loop.primary"),
                           SubjectRef(SubjectKind::Pump, StrongId::from_validated("pump.ghost")),
                           LinkRelation::Supply});
  model.reindex();
  const auto traversal = traverse(model, loop_ref("loop.primary"), TraversalOptions{});
  CO_REQUIRE_OK(traversal);
  CO_CHECK_EQ(traversal.value().dangling.size(), 1u);
  CO_CHECK(traversal.value().dangling[0].to.id.view() == "pump.ghost");
}

CO_TEST("control bindings are not delivery edges and are excluded on request") {
  // Two elements connected only by a control binding: the observatory can see
  // the relationship and must not walk it as if coolant flowed along it.
  PlantModel model;
  model.add_facility(FacilityId(StrongId::from_validated("dc1")), "dc1");
  CoolingPlant plant;
  plant.id = PlantId(StrongId::from_validated("plant.a"));
  plant.facility = FacilityId(StrongId::from_validated("dc1"));
  model.add_plant(plant);
  Loop primary;
  primary.id = LoopId(StrongId::from_validated("loop.primary"));
  primary.plant = plant.id;
  model.add_loop(primary);
  PlantComponent valve;
  valve.kind = ComponentKind::Valve;
  valve.id = StrongId::from_validated("valve.v1");
  model.add_component(valve);
  model.add_link(PlantLink{loop_ref("loop.primary"), valve.subject(), LinkRelation::ControlBinding});
  model.reindex();

  const auto delivery_only = traverse(model, loop_ref("loop.primary"), TraversalOptions{});
  CO_REQUIRE_OK(delivery_only);
  CO_CHECK_EQ(delivery_only.value().nodes.size(), 1u);
  CO_CHECK(!contains(delivery_only.value().nodes, valve.subject()));

  TraversalOptions all;
  all.delivery_only = false;
  const auto everything = traverse(model, loop_ref("loop.primary"), all);
  CO_REQUIRE_OK(everything);
  CO_CHECK(contains(everything.value().nodes, valve.subject()));
}

CO_TEST("a load is reached as a leaf and not descended into") {
  // A manifold feeding a zone and, below it, the rack group inside the zone.
  PlantModel model;
  model.add_facility(FacilityId(StrongId::from_validated("dc1")), "dc1");
  PlantComponent manifold;
  manifold.kind = ComponentKind::Manifold;
  manifold.id = StrongId::from_validated("manifold.m1");
  model.add_component(manifold);
  ThermalZone zone;
  zone.id = ZoneId(StrongId::from_validated("zone.a"));
  zone.facility = FacilityId(StrongId::from_validated("dc1"));
  model.add_zone(zone);
  PlantComponent racks;
  racks.kind = ComponentKind::RackGroup;
  racks.id = StrongId::from_validated("racks.r1");
  model.add_component(racks);
  PlantComponent inner;
  inner.kind = ComponentKind::Cdu;
  inner.id = StrongId::from_validated("cdu.inner");
  model.add_component(inner);

  // The manifold feeds the rack group directly and the zone feeds the element
  // behind it, so the traversal must reach the load and stop there even though
  // another route continues past it.
  model.add_link(PlantLink{manifold.subject(), racks.subject(), LinkRelation::Supply});
  model.add_link(PlantLink{racks.subject(), zone_ref("zone.a"), LinkRelation::Supply});
  model.add_link(PlantLink{zone_ref("zone.a"), inner.subject(), LinkRelation::Supply});
  model.reindex();

  TraversalOptions options;
  options.delivery_only = false;
  const auto traversal = traverse(model, manifold.subject(), options);
  CO_REQUIRE_OK(traversal);
  // The load is reported as a leaf, and the element behind it is not reached:
  // the path stops at the rack group even though the group also feeds a zone
  // that feeds a coolant distribution unit.
  CO_CHECK(contains(traversal.value().nodes, racks.subject()));
  CO_CHECK(!contains(traversal.value().nodes, zone_ref("zone.a")));
  CO_CHECK(!contains(traversal.value().nodes, inner.subject()));
  CO_CHECK(is_load(model, racks.subject()));
  CO_CHECK(is_load(model, zone_ref("zone.a")));
  CO_CHECK(!is_load(model, manifold.subject()));
}

CO_TEST("a model with a self loop terminates and reports the node once") {
  PlantModel model = cotest::synthetic_plant();
  model.add_link(PlantLink{loop_ref("loop.primary"), loop_ref("loop.primary"), LinkRelation::Return});
  model.reindex();
  const auto traversal = traverse(model, loop_ref("loop.primary"), TraversalOptions{});
  CO_REQUIRE_OK(traversal);
  std::size_t occurrences = 0;
  for (const DependencyNode& node : traversal.value().nodes) {
    if (node.subject == loop_ref("loop.primary")) {
      ++occurrences;
    }
  }
  CO_CHECK_EQ(occurrences, 1u);
}

CO_TEST("component and link lookups agree with the raw collections") {
  const PlantModel model = cotest::synthetic_plant();
  for (const PlantComponent& component : model.components()) {
    const auto found = model.component(subject_kind_of(component.kind), component.id);
    CO_REQUIRE(found.has_value());
    CO_CHECK(found.value().kind == component.kind);
  }
  CO_CHECK_EQ(model.components_of_kind(ComponentKind::Pump).size(), 2u);
  CO_CHECK_EQ(model.components_in_loop(LoopId(StrongId::from_validated("loop.secondary"))).size(), 3u);
  CO_CHECK_EQ(model.outgoing(loop_ref("loop.primary"), true).size(), 4u);
  CO_CHECK_EQ(model.incoming(zone_ref("zone.a"), true).size(), 1u);
}

CO_TEST("traversal refuses a zero budget rather than answering nothing") {
  const PlantModel model = cotest::synthetic_plant();
  TraversalOptions options;
  options.max_depth = 0;
  CO_CHECK_ERR(traverse(model, loop_ref("loop.primary"), options), Code::InvalidArgument);
  TraversalOptions no_nodes;
  no_nodes.max_nodes = 0;
  CO_CHECK_ERR(traverse(model, loop_ref("loop.primary"), no_nodes), Code::InvalidArgument);
  CO_CHECK_ERR(traverse(model, SubjectRef(), TraversalOptions{}), Code::InvalidArgument);
}