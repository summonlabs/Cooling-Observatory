// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/semantics.hpp"

#include <array>
#include <utility>

namespace dccp::cooling_observatory {
namespace {

template <typename Enum, std::size_t N>
std::string_view token_of(const std::array<std::pair<Enum, std::string_view>, N>& table, Enum value,
                          std::string_view fallback) noexcept {
  for (const auto& entry : table) {
    if (entry.first == value) {
      return entry.second;
    }
  }
  return fallback;
}

template <typename Enum, std::size_t N>
std::optional<Enum> parse_of(const std::array<std::pair<Enum, std::string_view>, N>& table,
                             std::string_view token) noexcept {
  for (const auto& entry : table) {
    if (entry.second == token) {
      return entry.first;
    }
  }
  return std::nullopt;
}

constexpr std::array<std::pair<AuthorityDomain, std::string_view>, 9> kAuthorityDomains{{
    {AuthorityDomain::CoolingTopology, "cooling_topology"},
    {AuthorityDomain::CoolingCapacity, "cooling_capacity"},
    {AuthorityDomain::CoolingFailure, "cooling_failure"},
    {AuthorityDomain::CoolingControl, "cooling_control"},
    {AuthorityDomain::FacilityTelemetry, "facility_telemetry"},
    {AuthorityDomain::WorkloadPlacement, "workload_placement"},
    {AuthorityDomain::ThermalObservation, "thermal_observation"},
    {AuthorityDomain::PowerObservation, "power_observation"},
    {AuthorityDomain::LocalObservation, "local_observation"},
}};

constexpr std::array<std::pair<EvidenceAxis, std::string_view>, 6> kEvidenceAxes{{
    {EvidenceAxis::Structure, "structure"},
    {EvidenceAxis::Delivery, "delivery"},
    {EvidenceAxis::Condition, "condition"},
    {EvidenceAxis::Capability, "capability"},
    {EvidenceAxis::Reserve, "reserve"},
    {EvidenceAxis::Fault, "fault"},
}};

constexpr std::array<std::pair<EvidenceOrigin, std::string_view>, 5> kEvidenceOrigins{{
    {EvidenceOrigin::Observed, "observed"},
    {EvidenceOrigin::Configured, "configured"},
    {EvidenceOrigin::Declared, "declared"},
    {EvidenceOrigin::Derived, "derived"},
    {EvidenceOrigin::Recovered, "recovered"},
}};

constexpr std::array<std::pair<Freshness, std::string_view>, 5> kFreshness{{
    {Freshness::Fresh, "fresh"},
    {Freshness::Stale, "stale"},
    {Freshness::Recovered, "recovered"},
    {Freshness::Conflicting, "conflicting"},
    {Freshness::Unknown, "unknown"},
}};

constexpr std::array<std::pair<LifecycleState, std::string_view>, 8> kLifecycleStates{{
    {LifecycleState::Unknown, "unknown"},
    {LifecycleState::Off, "off"},
    {LifecycleState::Starting, "starting"},
    {LifecycleState::Running, "running"},
    {LifecycleState::Degraded, "degraded"},
    {LifecycleState::Stopping, "stopping"},
    {LifecycleState::Faulted, "faulted"},
    {LifecycleState::Maintenance, "maintenance"},
}};

}  // namespace

std::string_view to_token(AuthorityDomain domain) noexcept {
  return token_of<AuthorityDomain>(kAuthorityDomains, domain, "unknown");
}

std::optional<AuthorityDomain> parse_authority_domain(std::string_view token) noexcept {
  return parse_of<AuthorityDomain>(kAuthorityDomains, token);
}

std::string_view to_token(EvidenceAxis axis) noexcept {
  return token_of<EvidenceAxis>(kEvidenceAxes, axis, "unknown");
}

std::optional<EvidenceAxis> parse_evidence_axis(std::string_view token) noexcept {
  return parse_of<EvidenceAxis>(kEvidenceAxes, token);
}

std::string_view to_token(EvidenceOrigin origin) noexcept {
  return token_of<EvidenceOrigin>(kEvidenceOrigins, origin, "unknown");
}

std::optional<EvidenceOrigin> parse_evidence_origin(std::string_view token) noexcept {
  return parse_of<EvidenceOrigin>(kEvidenceOrigins, token);
}

std::string_view to_token(Freshness freshness) noexcept {
  return token_of<Freshness>(kFreshness, freshness, "unknown");
}

std::optional<Freshness> parse_freshness(std::string_view token) noexcept {
  return parse_of<Freshness>(kFreshness, token);
}

std::string_view to_token(LifecycleState state) noexcept {
  return token_of<LifecycleState>(kLifecycleStates, state, "unknown");
}

std::optional<LifecycleState> parse_lifecycle_state(std::string_view token) noexcept {
  return parse_of<LifecycleState>(kLifecycleStates, token);
}

bool operator<(const AuthorityRef& a, const AuthorityRef& b) noexcept {
  if (a.domain != b.domain) {
    return static_cast<std::uint8_t>(a.domain) < static_cast<std::uint8_t>(b.domain);
  }
  if (a.authority != b.authority) {
    return a.authority < b.authority;
  }
  if (a.generation != b.generation) {
    return a.generation < b.generation;
  }
  return a.digest < b.digest;
}

}  // namespace dccp::cooling_observatory
