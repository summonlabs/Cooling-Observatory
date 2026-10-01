// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_DELIVERY_HPP
#define DCCP_COOLING_OBSERVATORY_DELIVERY_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/cooling_observatory/evidence.hpp"
#include "dccp/cooling_observatory/plant.hpp"

namespace dccp::cooling_observatory {

/// Physical constants used to turn observed flow and temperature into removed
/// heat, and to compare air and liquid paths.
///
/// These are properties of the coolant, not of the facility, so they are policy
/// with an explicit default. Every computed heat value carries the policy that
/// produced it, and a point served by air uses the air constants rather than
/// the coolant ones.
struct ThermalPolicy {
  /// Volumetric heat capacity of the coolant, in micro-joules per litre per
  /// kelvin, so that 4.18 J/(L*K) is 4180000 and the value stays an exact
  /// integer. Water is about 4.18 J/(L*K); a 25% propylene glycol mixture is
  /// about 3.9 J/(L*K).
  std::int64_t coolant_heat_capacity_uj_per_l_k = 4'180'000;
  /// The same, for a point served by air. Air is about 1.2 J/(L*K) at roughly
  /// 35 degrees Celsius and sea level, which is why an air path needs its own
  /// constant rather than the coolant default.
  std::int64_t air_heat_capacity_uj_per_l_k = 1'200;
  /// Whether a computed negative heat removal is clamped to zero in the value.
  /// The contradiction is reported either way; the default keeps the
  /// measurement honest instead of clamping it away.
  bool clamp_negative_removal = false;

  friend bool operator==(const ThermalPolicy& a, const ThermalPolicy& b) noexcept {
    return a.coolant_heat_capacity_uj_per_l_k == b.coolant_heat_capacity_uj_per_l_k &&
           a.air_heat_capacity_uj_per_l_k == b.air_heat_capacity_uj_per_l_k &&
           a.clamp_negative_removal == b.clamp_negative_removal;
  }
  friend bool operator!=(const ThermalPolicy& a, const ThermalPolicy& b) noexcept { return !(a == b); }
};

[[nodiscard]] const ThermalPolicy& default_thermal_policy() noexcept;
[[nodiscard]] Status validate(const ThermalPolicy& policy);

/// What carries heat away from a delivery point.
enum class CoolantMedium : std::uint8_t {
  Liquid = 0,
  Air = 1,
  /// The medium is not declared. The coolant constants are used and the
  /// assumption is recorded, so a consumer can see that the figure rests on a
  /// default rather than on a declaration.
  Undeclared = 2,
};

[[nodiscard]] std::string_view to_token(CoolantMedium medium) noexcept;
[[nodiscard]] std::optional<CoolantMedium> parse_coolant_medium(std::string_view token) noexcept;

/// A delivery point: the place where the observatory asks how much cooling is
/// actually arriving.
///
/// The point is defined by structure (which zone or loop) and by the
/// measurement identities that speak for it. It is a declaration about
/// instrumentation, not about capacity.
struct DeliveryPoint {
  StrongId id{};
  /// The zone being cooled. Optional: a delivery point may be a loop header
  /// observed before any zone is attributed to it.
  ZoneId zone{};
  LoopId loop{};
  CoolantMedium medium = CoolantMedium::Undeclared;
  /// Measurement that reports flow into this point.
  MeasurementId flow_measurement{};
  /// Supply and return temperature measurements, when the point is thermally
  /// instrumented. Without them, heat removal is unknown, not zero.
  MeasurementId supply_temperature{};
  MeasurementId return_temperature{};
  /// Differential pressure across the point, when instrumented.
  MeasurementId differential_pressure{};
  /// Differential pressure across the whole loop the point belongs to. A flow
  /// with a loop differential and no point differential is not a contradiction.
  MeasurementId loop_differential_pressure{};
  /// Airflow measurement, when the point is air cooled. Treated as a flow: the
  /// difference between moving air and moving coolant is the constant used to
  /// turn it into heat, not the arithmetic.
  MeasurementId airflow{};
  /// Declared heat load for this point, when a placement authority states one.
  /// Recorded as declared and never mixed with measured removal.
  Maybe<Quantity> declared_load{};
  /// When this declaration was committed. Set by the engine.
  Revision committed_revision{};

  /// True when the point declares no instrumentation at all.
  [[nodiscard]] bool uninstrumented() const noexcept {
    return flow_measurement.empty() && airflow.empty() && supply_temperature.empty() &&
           return_temperature.empty() && differential_pressure.empty();
  }

  friend bool operator==(const DeliveryPoint& a, const DeliveryPoint& b) noexcept {
    return a.id == b.id && a.zone == b.zone && a.loop == b.loop && a.medium == b.medium &&
           a.flow_measurement == b.flow_measurement && a.supply_temperature == b.supply_temperature &&
           a.return_temperature == b.return_temperature &&
           a.differential_pressure == b.differential_pressure &&
           a.loop_differential_pressure == b.loop_differential_pressure && a.airflow == b.airflow &&
           a.declared_load == b.declared_load;
  }
  friend bool operator<(const DeliveryPoint& a, const DeliveryPoint& b) noexcept { return a.id < b.id; }
};

/// What is evidenced as arriving at one delivery point.
struct DeliveryObservation {
  DeliveryPoint point{};

  /// Volumetric flow into the point. Absent when the flow sensor is silent, and
  /// absent is not zero.
  Judged<Maybe<Quantity>> flow{};
  /// Differential pressure across the point.
  Judged<Maybe<Quantity>> differential_pressure{};
  /// The drop from return to supply, in millikelvin. Preserved separately from
  /// heat removal because it is what the observer measured, while heat removal
  /// is what this runtime computed.
  Judged<Maybe<Quantity>> temperature_difference{};
  /// Heat removal evidenced at the point, in watts. Derived from flow and
  /// temperature difference using the thermal policy. Unknown when either input
  /// is unknown, and never inferred from declared load.
  Judged<Maybe<Quantity>> heat_removal{};
  /// Declared heat load attributed to the point, when one is stated.
  Judged<Maybe<Quantity>> declared_load{};
  /// Absolute difference between measured removal and declared load, when both
  /// are known.
  Judged<Maybe<Quantity>> load_mismatch{};

  /// The freshness of the whole observation: the worst of its parts.
  Freshness freshness = Freshness::Unknown;
  /// Elements in the delivery path, from the plant towards the point.
  std::vector<SubjectRef> path{};
  /// True when the medium was not declared and the coolant constants were used.
  bool medium_assumed = false;
  /// Non-empty when part of the observation could not be made.
  std::vector<Indeterminacy> indeterminacies{};
};

/// The observatory's view of every delivery point it can compute.
struct DeliveryReport {
  GenerationId generation{};
  EpochId epoch{};
  Revision revision{};
  TimestampMs as_of_ms = 0;
  std::vector<DeliveryObservation> points{};
  /// Delivery points that exist for which no evidence at all is current.
  /// Reported explicitly so that silence is visible rather than looking like a
  /// healthy point with no findings.
  std::vector<DeliveryPoint> unevidenced_points{};
  /// Total heat removal evidenced across the points in this report, when at
  /// least one point has a current figure. Absent when none does, which is not
  /// zero.
  Judged<Maybe<Quantity>> total_heat_removal{};
  Freshness freshness = Freshness::Unknown;
};

/// Derive a measurement identity from a zone and a role. The convention is
/// published rather than invented per call site, so a producer can construct an
/// identity this runtime will resolve.
#define DCCP_COOLING_OBSERVATORY_DECLARE_MEASUREMENT_ROLE(Role, Suffix)                  \
  [[nodiscard]] inline MeasurementId zone_##Role(const ZoneId& zone) {                   \
    return MeasurementId(StrongId::from_validated(zone.str() + Suffix));                 \
  }

DCCP_COOLING_OBSERVATORY_DECLARE_MEASUREMENT_ROLE(flow, ".flow")
DCCP_COOLING_OBSERVATORY_DECLARE_MEASUREMENT_ROLE(supply_temperature, ".supply_temp")
DCCP_COOLING_OBSERVATORY_DECLARE_MEASUREMENT_ROLE(return_temperature, ".return_temp")
DCCP_COOLING_OBSERVATORY_DECLARE_MEASUREMENT_ROLE(differential_pressure, ".diff_pressure")
DCCP_COOLING_OBSERVATORY_DECLARE_MEASUREMENT_ROLE(airflow, ".airflow")

#undef DCCP_COOLING_OBSERVATORY_DECLARE_MEASUREMENT_ROLE

/// The delivery point implied by a zone and this library's measurement naming
/// convention. Used when a structure arrives without delivery-point
/// declarations.
[[nodiscard]] DeliveryPoint implied_delivery_point(const ThermalZone& zone);

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_DELIVERY_HPP
