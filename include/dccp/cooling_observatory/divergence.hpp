// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_DIVERGENCE_HPP
#define DCCP_COOLING_OBSERVATORY_DIVERGENCE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/cooling_observatory/delivery.hpp"
#include "dccp/cooling_observatory/evidence.hpp"

namespace dccp::cooling_observatory {

/// The relationship between what the structure implies and what the
/// measurements show.
enum class DivergenceState : std::uint8_t {
  Unknown = 0,        ///< not enough current evidence to compare
  Consistent = 1,     ///< measured delivery matches the expectation within tolerance
  UnderDelivery = 2,  ///< measured below expectation beyond tolerance
  OverDelivery = 3,   ///< measured above expectation beyond tolerance
  Undeclared = 4,     ///< measured delivery with no declared expectation to compare against
  Dry = 5,            ///< an expectation exists and the measured flow is zero
  Contradictory = 6,  ///< observations that cannot all be true at once
};

[[nodiscard]] std::string_view to_token(DivergenceState state) noexcept;
[[nodiscard]] std::optional<DivergenceState> parse_divergence_state(std::string_view token) noexcept;

/// A single contradiction, named so that it can be asserted on and reported
/// without parsing prose.
enum class ContradictionKind : std::uint8_t {
  FlowWithoutPressure = 0,      ///< flow through a point with no differential pressure at all
  PressureWithoutFlow = 1,      ///< differential pressure with no flow at all
  DeliveryWhileStopped = 2,     ///< flow through equipment reported stopped or faulted
  NoDeliveryWhileRunning = 3,   ///< equipment reported running with no flow below it
  SupplyWarmerThanReturn = 4,   ///< a temperature pair with the wrong sign
  RemovalAboveCapability = 5,   ///< measured heat removal above declared capability
  NegativeFlow = 6,             ///< a physically non-negative quantity reported negative
  NotCooling = 7,               ///< a computed heat removal that is negative
};

[[nodiscard]] std::string_view to_token(ContradictionKind kind) noexcept;
[[nodiscard]] std::optional<ContradictionKind> parse_contradiction_kind(std::string_view token) noexcept;

/// One contradiction found while comparing evidence.
struct Contradiction {
  ContradictionKind kind = ContradictionKind::PressureWithoutFlow;
  SubjectRef subject{};
  std::string detail{};
  Maybe<Quantity> first{};
  Maybe<Quantity> second{};
  std::vector<EvidenceRef> evidence{};

  friend bool operator<(const Contradiction& a, const Contradiction& b) noexcept {
    if (a.kind != b.kind) {
      return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
    }
    return a.subject < b.subject;
  }
};

/// The divergence of one delivery point.
struct DivergenceFinding {
  StrongId point{};
  DivergenceState state = DivergenceState::Unknown;

  /// What the structure and declarations expect. A capability declared for the
  /// equipment that feeds the point is the only expectation this runtime uses;
  /// it never invents one.
  Judged<Maybe<Quantity>> expected{};
  /// What was measured.
  Judged<Maybe<Quantity>> observed{};
  /// Observed minus expected. Positive means more delivered than declared.
  Judged<Maybe<Quantity>> delta{};
  /// The tolerance applied, from policy and from the query.
  Quantity tolerance{};
  std::vector<Contradiction> contradictions{};
  std::vector<Indeterminacy> indeterminacies{};
  Freshness freshness = Freshness::Unknown;
};

struct DivergenceReport {
  GenerationId generation{};
  EpochId epoch{};
  Revision revision{};
  TimestampMs as_of_ms = 0;
  std::vector<DivergenceFinding> findings{};
  /// Findings that are not Consistent, in canonical order. The part a consumer
  /// normally acts on, provided so that it does not have to filter.
  std::vector<StrongId> divergent_points{};
  std::size_t contradiction_count = 0;
  Freshness freshness = Freshness::Unknown;
};

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_DIVERGENCE_HPP
