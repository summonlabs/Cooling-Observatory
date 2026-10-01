// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_FAILURE_HPP
#define DCCP_COOLING_OBSERVATORY_FAILURE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/cooling_observatory/delivery.hpp"
#include "dccp/cooling_observatory/evidence.hpp"
#include "dccp/cooling_observatory/plant.hpp"

namespace dccp::cooling_observatory {

/// How well the observed effect of a failure matches what the failure claims.
///
/// This is the observatory's central discipline: a declared failure is a claim,
/// and a claim whose effect is not observable is reported as unconfirmed rather
/// than propagated as fact.
enum class EffectState : std::uint8_t {
  Confirmed = 0,      ///< delivery through the affected element is gone or reduced as claimed
  Unconfirmed = 1,    ///< the claim is current, but no delivery evidence shows an effect
  Contradicted = 2,   ///< delivery is at or above declared while the failure claims otherwise
  Residual = 3,       ///< the failure is real and delivery continues, measurably
  Indeterminate = 4,  ///< no delivery evidence either way
};

[[nodiscard]] std::string_view to_token(EffectState state) noexcept;
[[nodiscard]] std::optional<EffectState> parse_effect_state(std::string_view token) noexcept;

[[nodiscard]] std::string_view to_token(EffectState state) noexcept;
[[nodiscard]] std::optional<EffectState> parse_effect_state(std::string_view token) noexcept;

/// One declared failure, with what the observatory could and could not confirm.
struct FailureAssessment {
  StrongId failure_id{};
  FailureKind kind = FailureKind::Unknown;
  FailureSeverity severity = FailureSeverity::Advisory;
  FailureImpact declared_impact = FailureImpact::UnknownImpact;

  SubjectRef subject{};
  /// The element the failure is attributed to after structure resolution.
  Maybe<SubjectRef> element{};
  /// Whether the element exists in the adopted generation.
  bool element_resolved = false;

  /// The declared failure record's freshness. A stale failure declaration is
  /// reported as a stale claim.
  Freshness declared_freshness = Freshness::Unknown;

  /// Measured delivery through the affected element, when a point exists that
  /// speaks for it.
  Judged<Maybe<Quantity>> observed_delivery{};
  /// Declared capability of that element, when a capability generation covers it.
  Judged<Maybe<Quantity>> declared_capability{};
  /// Residual delivery measured while the failure is in force.
  Judged<Maybe<Quantity>> observed_residual{};

  EffectState effect = EffectState::Indeterminate;

  /// Zones whose heat removal depends on the affected element.
  std::vector<ZoneId> affected_zones{};
  /// Delivery points in the affected subtree.
  std::vector<StrongId> affected_points{};
  /// Zones in the affected subtree for which no delivery evidence exists. These
  /// are the places where a failure is real and the observatory cannot say
  /// whether heat is still being removed.
  std::vector<ZoneId> unevidenced_zones{};

  /// Distinguishes "the producer declared this failure" from "this runtime
  /// concluded it". Always Declared: declaring failure state is not this
  /// runtime's authority.
  bool declared_by_authority = true;
  /// Reference to the authority that declared it, for the explanation.
  AuthorityRef declaring_authority{};

  std::vector<Indeterminacy> indeterminacies{};
  Freshness freshness = Freshness::Unknown;
};

/// The full failure picture.
struct FailureReport {
  GenerationId generation{};
  EpochId epoch{};
  Revision revision{};
  TimestampMs as_of_ms = 0;
  std::vector<FailureAssessment> failures{};
  /// Failures declared by an authority but excluded by the query, so that the
  /// count is visible even when the rows are not.
  std::size_t excluded_count = 0;
  Freshness freshness = Freshness::Unknown;
};

struct FailureQuery {
  Maybe<SubjectRef> subject{};
  Maybe<ZoneId> zone{};
  Maybe<LoopId> loop{};
  /// Include failures whose declaration is stale. Off by default.
  bool include_stale = false;
  /// Include failures whose declared impact is Advisory only.
  bool include_advisory = true;
  /// Minimum declared severity to report.
  Maybe<FailureSeverity> minimum_severity{};
};

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_FAILURE_HPP
