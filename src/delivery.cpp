// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/delivery.hpp"

#include <array>
#include <utility>

namespace dccp::cooling_observatory {
namespace {

constexpr std::array<std::pair<CoolantMedium, std::string_view>, 3> kCoolantMedia{{
    {CoolantMedium::Liquid, "liquid"},
    {CoolantMedium::Air, "air"},
    {CoolantMedium::Undeclared, "undeclared"},
}};

}  // namespace

std::string_view to_token(CoolantMedium medium) noexcept {
  for (const auto& entry : kCoolantMedia) {
    if (entry.first == medium) {
      return entry.second;
    }
  }
  return "undeclared";
}

std::optional<CoolantMedium> parse_coolant_medium(std::string_view token) noexcept {
  for (const auto& entry : kCoolantMedia) {
    if (entry.second == token) {
      return entry.first;
    }
  }
  return std::nullopt;
}

const ThermalPolicy& default_thermal_policy() noexcept {
  static const ThermalPolicy policy;
  return policy;
}

Status validate(const ThermalPolicy& policy) {
  if (policy.coolant_heat_capacity_uj_per_l_k <= 0) {
    return Status::failure(Code::InvalidArgument, "coolant_capacity_not_positive",
                           "the coolant volumetric heat capacity must be greater than zero");
  }
  if (policy.air_heat_capacity_uj_per_l_k <= 0) {
    return Status::failure(Code::InvalidArgument, "air_capacity_not_positive",
                           "the air volumetric heat capacity must be greater than zero");
  }
  return Status::success();
}

DeliveryPoint implied_delivery_point(const ThermalZone& zone) {
  DeliveryPoint point;
  point.id = StrongId::from_validated("dp." + zone.id.str());
  point.zone = zone.id;
  point.flow_measurement = zone_flow(zone.id);
  point.supply_temperature = zone_supply_temperature(zone.id);
  point.return_temperature = zone_return_temperature(zone.id);
  point.differential_pressure = zone_differential_pressure(zone.id);
  point.airflow = zone_airflow(zone.id);
  point.declared_load = zone.declared_load;
  return point;
}

}  // namespace dccp::cooling_observatory
