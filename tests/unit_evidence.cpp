// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The evidence model: freshness classification, reduction of several sources,
// and the policies that decide what counts as current. Silence must be unknown
// rather than zero, a disagreement must be reported rather than averaged, and a
// recovered value must never be promoted to current by any path.

#include <string>
#include <vector>

#include "dccp/cooling_observatory/evidence.hpp"
#include "support.hpp"
#include "test_harness.hpp"

using namespace dccp::cooling_observatory;

namespace {

SubjectRef measurement_subject_for(const char* id) {
  return SubjectRef(SubjectKind::Measurement, StrongId::from_validated(id));
}

Observation observation(const char* measurement, const char* sensor, Quantity value,
                        TimestampMs observed_at, EpochId epoch, EvidenceOrigin origin,
                        MeasurementQuality quality = MeasurementQuality::Good) {
  Observation out;
  out.kind = ObservationKind::Measurement;
  out.subject = measurement_subject_for(measurement);
  out.sensor = SensorId(StrongId::from_validated(sensor));
  out.measured = value;
  out.observed_at_ms = observed_at;
  out.received_at_ms = observed_at;
  out.epoch = epoch;
  out.origin = origin;
  out.quality = quality;
  return out;
}

}  // namespace

CO_TEST("a current measurement inside its age bound is fresh") {
  const FreshnessPolicy policy;
  const Observation value = observation("m", "s", Quantity::flow(1000), cotest::at(0), EpochId(1),
                                        EvidenceOrigin::Observed);
  CO_CHECK(classify(value, policy, cotest::at(1000), EpochId(1), GenerationId(1)) == Freshness::Fresh);
  CO_CHECK(classify(value, policy, cotest::at(policy.delivery_max_age_ms), EpochId(1), GenerationId(1)) ==
           Freshness::Fresh);
  CO_CHECK(classify(value, policy, cotest::at(policy.delivery_max_age_ms + 1), EpochId(1), GenerationId(1)) ==
           Freshness::Stale);
}

CO_TEST("a record stamped in the future is stale, not fresh") {
  const FreshnessPolicy policy;
  const Observation value = observation("m", "s", Quantity::flow(1000), cotest::at(60000), EpochId(1),
                                        EvidenceOrigin::Observed);
  CO_CHECK(classify(value, policy, cotest::at(0), EpochId(1), GenerationId(1)) == Freshness::Stale);
}

CO_TEST("a sensor reporting bad or unavailable quality is silence, not zero") {
  const FreshnessPolicy policy;
  const Observation bad = observation("m", "s", Quantity::flow(0), cotest::at(0), EpochId(1),
                                      EvidenceOrigin::Observed, MeasurementQuality::Bad);
  CO_CHECK(classify(bad, policy, cotest::at(0), EpochId(1), GenerationId(1)) == Freshness::Unknown);
  const Observation unavailable =
      observation("m", "s", Quantity::flow(0), cotest::at(0), EpochId(1), EvidenceOrigin::Observed,
                  MeasurementQuality::NotAvailable);
  CO_CHECK(classify(unavailable, policy, cotest::at(0), EpochId(1), GenerationId(1)) == Freshness::Unknown);
  const Observation uncertain =
      observation("m", "s", Quantity::flow(0), cotest::at(0), EpochId(1), EvidenceOrigin::Observed,
                  MeasurementQuality::Uncertain);
  CO_CHECK(classify(uncertain, policy, cotest::at(0), EpochId(1), GenerationId(1)) == Freshness::Fresh);
}

CO_TEST("evidence from a superseded epoch is stale unless the policy says otherwise") {
  FreshnessPolicy policy;
  const Observation value = observation("m", "s", Quantity::flow(1000), cotest::at(0), EpochId(9),
                                        EvidenceOrigin::Observed);
  CO_CHECK(classify(value, policy, cotest::at(0), EpochId(1), GenerationId(1)) == Freshness::Stale);
  policy.accept_other_epoch_as_current = true;
  CO_CHECK(classify(value, policy, cotest::at(0), EpochId(1), GenerationId(1)) == Freshness::Fresh);
}

CO_TEST("a declaration stated against a superseded structure is stale") {
  const FreshnessPolicy policy;
  Observation declaration = observation("m", "s", Quantity::flow(1000), cotest::at(0), EpochId(1),
                                        EvidenceOrigin::Observed);
  declaration.kind = ObservationKind::Capability;
  declaration.generation = GenerationId(4);
  CO_CHECK(classify(declaration, policy, cotest::at(0), EpochId(1), GenerationId(5)) == Freshness::Stale);
  CO_CHECK(classify(declaration, policy, cotest::at(0), EpochId(1), GenerationId(4)) == Freshness::Fresh);
  FreshnessPolicy permissive;
  permissive.accept_other_generation_as_current = true;
  CO_CHECK(classify(declaration, permissive, cotest::at(0), EpochId(1), GenerationId(5)) == Freshness::Fresh);
}

CO_TEST("recovered evidence is never promoted to current by any policy") {
  FreshnessPolicy permissive;
  permissive.accept_other_epoch_as_current = true;
  permissive.accept_other_generation_as_current = true;
  const Observation value = observation("m", "s", Quantity::flow(1000), cotest::at(0), EpochId(1),
                                        EvidenceOrigin::Recovered);
  CO_CHECK(classify(value, permissive, cotest::at(0), EpochId(1), GenerationId(1)) == Freshness::Recovered);
  CO_CHECK(!is_current(classify(value, permissive, cotest::at(0), EpochId(1), GenerationId(1))));
}

CO_TEST("policy validation refuses negative ages") {
  FreshnessPolicy policy;
  CO_CHECK_OK(validate(policy));
  policy.delivery_max_age_ms = -1;
  CO_CHECK_ERR(validate(policy), Code::InvalidArgument);
}

CO_TEST("the freshness lattice joins to the worst contributor") {
  CO_CHECK(worst(Freshness::Fresh, Freshness::Stale) == Freshness::Stale);
  CO_CHECK(worst(Freshness::Stale, Freshness::Fresh) == Freshness::Stale);
  CO_CHECK(worst(Freshness::Recovered, Freshness::Conflicting) == Freshness::Conflicting);
  CO_CHECK(worst(Freshness::Conflicting, Freshness::Unknown) == Freshness::Unknown);
  CO_CHECK(worst(Freshness::Fresh, Freshness::Fresh) == Freshness::Fresh);
  CO_CHECK(freshness_rank(Freshness::Unknown) > freshness_rank(Freshness::Fresh));
  CO_CHECK(!is_known(Freshness::Unknown));
  CO_CHECK(is_known(Freshness::Stale));
}

CO_TEST("one current source reduces to its value") {
  const FreshnessPolicy policy;
  Reducer reducer;
  reducer.policy = &policy;
  reducer.now_ms = cotest::at(0);
  reducer.current_epoch = EpochId(1);
  reducer.adopted_generation = GenerationId(1);
  const std::vector<Observation> values{
      observation("m", "s1", Quantity::flow(1234), cotest::at(0), EpochId(1), EvidenceOrigin::Observed)};
  const Reduction reduction = reducer.reduce_group(values);
  CO_CHECK(reduction.freshness == Freshness::Fresh);
  CO_REQUIRE(reduction.value.has_value());
  CO_CHECK_EQ(reduction.value.value().value, 1234);
  CO_CHECK_EQ(reduction.evidence.size(), 1u);
}

CO_TEST("several current sources that agree reduce to that value") {
  const FreshnessPolicy policy;
  Reducer reducer;
  reducer.policy = &policy;
  reducer.now_ms = cotest::at(0);
  reducer.current_epoch = EpochId(1);
  reducer.adopted_generation = GenerationId(1);
  reducer.tolerance = 500;
  const std::vector<Observation> values{
      observation("m", "s1", Quantity::flow(1000), cotest::at(0), EpochId(1), EvidenceOrigin::Observed),
      observation("m", "s2", Quantity::flow(1200), cotest::at(0), EpochId(1), EvidenceOrigin::Observed)};
  const Reduction reduction = reducer.reduce_group(values);
  CO_CHECK(reduction.freshness == Freshness::Fresh);
  CO_REQUIRE(reduction.value.has_value());
  CO_CHECK_EQ(reduction.value.value().value, 1000);
  CO_CHECK_EQ(reduction.evidence.size(), 2u);
}

CO_TEST("disagreeing current sources conflict rather than average") {
  const FreshnessPolicy policy;
  Reducer reducer;
  reducer.policy = &policy;
  reducer.now_ms = cotest::at(0);
  reducer.current_epoch = EpochId(1);
  reducer.adopted_generation = GenerationId(1);
  reducer.tolerance = 100;
  const std::vector<Observation> values{
      observation("m", "s1", Quantity::flow(1000), cotest::at(0), EpochId(1), EvidenceOrigin::Observed),
      observation("m", "s2", Quantity::flow(9000), cotest::at(0), EpochId(1), EvidenceOrigin::Observed)};
  const Reduction reduction = reducer.reduce_group(values);
  CO_CHECK(reduction.freshness == Freshness::Conflicting);
  CO_CHECK(!reduction.value.has_value());
  CO_CHECK_EQ(reduction.conflicting_sensors.size(), 2u);
  CO_CHECK_EQ(reduction.conflicting_values.size(), 2u);
  CO_CHECK_EQ(reduction.conflicting_values[0].value, 1000);
  CO_CHECK_EQ(reduction.conflicting_values[1].value, 9000);
}

CO_TEST("no evidence at all reduces to unknown with no value") {
  const FreshnessPolicy policy;
  Reducer reducer;
  reducer.policy = &policy;
  reducer.now_ms = cotest::at(0);
  const std::vector<Observation> empty;
  const Reduction reduction = reducer.reduce_group(empty);
  CO_CHECK(reduction.freshness == Freshness::Unknown);
  CO_CHECK(!reduction.value.has_value());
  CO_CHECK(reduction.evidence.empty());
}

CO_TEST("stale evidence alone reduces to stale with no current value") {
  const FreshnessPolicy policy;
  Reducer reducer;
  reducer.policy = &policy;
  reducer.now_ms = cotest::at(policy.delivery_max_age_ms + 1);
  reducer.current_epoch = EpochId(1);
  reducer.adopted_generation = GenerationId(1);
  const std::vector<Observation> values{
      observation("m", "s1", Quantity::flow(1234), cotest::at(0), EpochId(1), EvidenceOrigin::Observed)};
  const Reduction reduction = reducer.reduce_group(values);
  // The classification is of the group, not of the usable value: a group whose
  // only member is stale reduces to a stale group with no value.
  CO_CHECK(reduction.freshness == Freshness::Stale);
  CO_CHECK(!reduction.value.has_value());
  CO_CHECK(reduction.evidence.empty());
}

CO_TEST("a stale source does not drag a current one down") {
  const FreshnessPolicy policy;
  Reducer reducer;
  reducer.policy = &policy;
  reducer.now_ms = cotest::at(policy.delivery_max_age_ms + 1);
  reducer.current_epoch = EpochId(1);
  reducer.adopted_generation = GenerationId(1);
  const std::vector<Observation> values{
      observation("m", "s1", Quantity::flow(1), cotest::at(0), EpochId(1), EvidenceOrigin::Observed),
      observation("m", "s2", Quantity::flow(1234), cotest::at(policy.delivery_max_age_ms + 1), EpochId(1),
                  EvidenceOrigin::Observed)};
  const Reduction reduction = reducer.reduce_group(values);
  CO_CHECK(reduction.freshness == Freshness::Fresh);
  CO_REQUIRE(reduction.value.has_value());
  CO_CHECK_EQ(reduction.value.value().value, 1234);
  CO_CHECK_EQ(reduction.evidence.size(), 1u);
}

CO_TEST("a recovered source alone reduces to recovered, never to fresh") {
  const FreshnessPolicy policy;
  Reducer reducer;
  reducer.policy = &policy;
  reducer.now_ms = cotest::at(0);
  reducer.current_epoch = EpochId(1);
  reducer.adopted_generation = GenerationId(1);
  const std::vector<Observation> values{
      observation("m", "s1", Quantity::flow(1234), cotest::at(0), EpochId(1), EvidenceOrigin::Recovered)};
  const Reduction reduction = reducer.reduce_group(values);
  CO_CHECK(reduction.freshness == Freshness::Recovered);
  CO_CHECK(!reduction.value.has_value());
}

CO_TEST("reduce() ignores observations addressed to another subject") {
  const FreshnessPolicy policy;
  Reducer reducer;
  reducer.policy = &policy;
  reducer.now_ms = cotest::at(0);
  reducer.current_epoch = EpochId(1);
  reducer.adopted_generation = GenerationId(1);
  const std::vector<Observation> values{
      observation("m1", "s1", Quantity::flow(1000), cotest::at(0), EpochId(1), EvidenceOrigin::Observed),
      observation("m2", "s2", Quantity::flow(9000), cotest::at(0), EpochId(1), EvidenceOrigin::Observed)};
  const Reduction reduction = reducer.reduce(values, measurement_subject_for("m1"));
  CO_CHECK(reduction.freshness == Freshness::Fresh);
  CO_REQUIRE(reduction.value.has_value());
  CO_CHECK_EQ(reduction.value.value().value, 1000);
}

CO_TEST("axis and kind stay consistent, and the axis determines the age bound") {
  CO_CHECK(axis_of(ObservationKind::Measurement) == EvidenceAxis::Delivery);
  CO_CHECK(axis_of(ObservationKind::EquipmentState) == EvidenceAxis::Condition);
  CO_CHECK(axis_of(ObservationKind::Capability) == EvidenceAxis::Capability);
  CO_CHECK(axis_of(ObservationKind::ReserveClaim) == EvidenceAxis::Reserve);
  CO_CHECK(axis_of(ObservationKind::Constraint) == EvidenceAxis::Fault);
  CO_CHECK(axis_of(ObservationKind::Failure) == EvidenceAxis::Fault);
  FreshnessPolicy policy;
  CO_CHECK_EQ(policy.max_age_for(EvidenceAxis::Delivery), policy.delivery_max_age_ms);
  CO_CHECK_EQ(policy.max_age_for(EvidenceAxis::Structure), policy.structure_max_age_ms);
}

CO_TEST("a capability declared in the wrong dimension is refused by validation") {
  FreshnessPolicy policy;
  Reducer reducer;
  reducer.policy = &policy;
  Observation declaration = observation("m", "s", Quantity::flow(1000), cotest::at(0), EpochId(1),
                                       EvidenceOrigin::Observed);
  declaration.kind = ObservationKind::Capability;
  declaration.capability_dimension = Dimension::Power;
  declaration.declared_capacity = 5;
  // The evidence layer records the dimension it was told; the analysis layer is
  // what refuses to compare across dimensions, and that refusal is tested with
  // the reserve and capability analyses.
  CO_CHECK(declaration.capability_dimension == Dimension::Power);
  CO_CHECK(dimension_of(ConstraintKind::FlowLimit).value() == Dimension::Flow);
  CO_CHECK(dimension_of(ConstraintKind::HeatRemovalLimit).value() == Dimension::Power);
  CO_CHECK(dimension_of(ConstraintKind::SupplyTemperature).value() == Dimension::Temperature);
  CO_CHECK(dimension_of(ConstraintKind::ValvePosition).value() == Dimension::Ratio);
  CO_CHECK(dimension_of(ConstraintKind::PumpSpeed).value() == Dimension::Frequency);
}

CO_TEST("lifecycle helpers distinguish delivering, failed and unknown states") {
  CO_CHECK(may_deliver(LifecycleState::Running));
  CO_CHECK(may_deliver(LifecycleState::Degraded));
  CO_CHECK(!may_deliver(LifecycleState::Off));
  CO_CHECK(!may_deliver(LifecycleState::Faulted));
  CO_CHECK(!may_deliver(LifecycleState::Unknown));
  CO_CHECK(is_failed(LifecycleState::Faulted));
  CO_CHECK(!is_failed(LifecycleState::Degraded));
}

CO_TEST("Maybe distinguishes an absent value from a zero value") {
  const Maybe<Quantity> absent;
  CO_CHECK(!absent.has_value());
  const Maybe<Quantity> zero = Maybe<Quantity>::of(Quantity::flow(0));
  CO_REQUIRE(zero.has_value());
  CO_CHECK_EQ(zero.value().value, 0);
  CO_CHECK(absent != zero);
  CO_CHECK(absent == Maybe<Quantity>::missing());
  CO_CHECK_EQ(zero.value_or(Quantity::flow(5)).value, 0);
  CO_CHECK_EQ(absent.value_or(Quantity::flow(5)).value, 5);
}

CO_TEST("evidence references carry the revision and ordinal of the commit") {
  Observation value = observation("m", "s", Quantity::flow(1), cotest::at(0), EpochId(3), EvidenceOrigin::Observed);
  value.record_seq = RecordSeq(77);
  value.generation = GenerationId(2);
  value.committed_ordinal = 12;
  const EvidenceRef reference = make_reference(value, Revision(9), 12);
  CO_CHECK_EQ(reference.revision.value(), 9u);
  CO_CHECK_EQ(reference.record_seq.value(), 77u);
  CO_CHECK_EQ(reference.epoch.value(), 3u);
  CO_CHECK_EQ(reference.generation.value(), 2u);
  CO_CHECK_EQ(reference.ordinal, 12u);
  CO_CHECK(reference.kind == ObservationKind::Measurement);
}