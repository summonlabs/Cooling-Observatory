// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Independent reference computations.
//
// These are deliberately written from the specification rather than from the
// library: the reference implementations use different loops, different data
// layout and, where it matters, a different algorithm, so an agreement between
// them and the library is evidence about both. A reference that called the code
// under test would prove nothing.

#ifndef COOLING_OBSERVATORY_REFERENCE_MODEL_HPP
#define COOLING_OBSERVATORY_REFERENCE_MODEL_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dccp/cooling_observatory/identity.hpp"
#include "dccp/cooling_observatory/plant.hpp"
#include "dccp/cooling_observatory/semantics.hpp"
#include "dccp/cooling_observatory/units.hpp"

namespace reference {

/// The domain types these references describe. Named here so that the
/// reference implementations read as domain code rather than as library code.
using SubjectRef = dccp::cooling_observatory::SubjectRef;
using PlantModel = dccp::cooling_observatory::PlantModel;

/// A 128-bit product reduced by an exact divisor, implemented with long
/// multiplication over decimal digits so that it shares no code and no
/// assumption with the library's binary implementation.
[[nodiscard]] std::int64_t exact_mul_div(std::int64_t value, std::int64_t num, std::int64_t den);

/// Heat removal in watts from a flow in microlitres per second, a temperature
/// difference in millikelvin and a volumetric heat capacity in micro-joules per
/// litre per kelvin. Derived from the units rather than from the library.
[[nodiscard]] std::int64_t heat_removal_watts(std::int64_t flow_ul_per_s,
                                              std::int64_t difference_mk,
                                              std::int64_t capacity_uj_per_l_k);

/// True when the exact intermediate product of the heat-removal computation
/// fits a signed 64-bit value. The library refuses a figure it cannot compute
/// exactly, so the reference has to say independently where that boundary is.
[[nodiscard]] bool heat_removal_representable(std::int64_t flow_ul_per_s,
                                              std::int64_t difference_mk,
                                              std::int64_t capacity_uj_per_l_k);

/// A declared capacity reduced by a derate in parts per million.
[[nodiscard]] std::int64_t derated(std::int64_t capacity, std::int64_t derate_ppm);

/// Freshness by the specification's rules: recovered first, then age, then
/// quality, then epoch, then generation.
[[nodiscard]] bool fresh_by_age(std::int64_t observed_at_ms, std::int64_t now_ms, std::int64_t bound_ms);

/// Every subject reachable from a root through edges that carry delivery,
/// breadth first, using an explicitly sorted work list rather than the
/// library's adjacency index.
struct ReferenceTraversal {
  std::vector<SubjectRef> nodes{};
  std::vector<std::pair<SubjectRef, std::uint32_t>> with_depth{};
};

[[nodiscard]] ReferenceTraversal reference_traverse(const PlantModel& model, const SubjectRef& root,
                                                    bool downstream, std::uint32_t max_depth);

/// The sum of a list of flows, or nothing when the sum overflows.
[[nodiscard]] std::optional<std::int64_t> sum_flows(const std::vector<std::int64_t>& flows);

}  // namespace reference

#endif  // COOLING_OBSERVATORY_REFERENCE_MODEL_HPP
