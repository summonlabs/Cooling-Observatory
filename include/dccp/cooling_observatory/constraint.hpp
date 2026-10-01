// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_CONSTRAINT_HPP
#define DCCP_COOLING_OBSERVATORY_CONSTRAINT_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/cooling_observatory/delivery.hpp"
#include "dccp/cooling_observatory/evidence.hpp"
#include "dccp/cooling_observatory/plant.hpp"

namespace dccp::cooling_observatory {

/// Why a constraint is attributed where it is.
///
/// Attribution is the answer to "where is the cooling constrained", and it is
/// only useful when it also says how confident the attribution is and what it
/// rests on. A constraint whose element is unknown is reported as
/// Unattributed; it is never attached to a plausible-looking neighbour.
enum class AttributionState : std::uint8_t {
  Direct = 0,        ///< the constraint names an element present in the adopted structure
  Inherited = 1,     ///< the element is upstream of the point, reached by traversal
  Ambiguous = 2,     ///< several equally plausible elements upstream; all are named
  Unattributed = 3,  ///< no element could be attributed
  Unknown = 4,       ///< the constraint itself is not current evidence
};

[[nodiscard]] std::string_view to_token(AttributionState state) noexcept;
[[nodiscard]] std::optional<AttributionState> parse_attribution_state(std::string_view token) noexcept;

[[nodiscard]] std::string_view to_token(AttributionState state) noexcept;
[[nodiscard]] std::optional<AttributionState> parse_attribution_state(std::string_view token) noexcept;

/// One constraint, localised to the delivery it affects.
struct ConstraintAttribution {
  /// Identity of the constraint as the producing authority named it.
  StrongId constraint_id{};
  ConstraintKind kind = ConstraintKind::FlowLimit;
  ConstraintDirection direction = ConstraintDirection::Maximum;
  ConstraintState state = ConstraintState::Unknown;

  /// The declared limit. Absent when the producer declared a constraint with no
  /// value, which is a real and common report and is not the same as zero.
  Judged<Maybe<Quantity>> limit{};

  /// The element the constraint is attributed to. Absent when unattributed.
  Maybe<SubjectRef> element{};

  /// Every element that could equally explain the constraint, ordered
  /// canonically. Non-empty exactly when state is Ambiguous.
  std::vector<SubjectRef> candidates{};

  AttributionState attribution = AttributionState::Unknown;

  /// The observed value that makes this constraint binding at the point, when
  /// one was measured. A flow limit is only binding against a measured flow.
  Judged<Maybe<Quantity>> observed_at_point{};
  /// How far the observed value is from the limit, in the limit's dimension.
  /// Positive means the observed value is on the violating side.
  Judged<Maybe<Quantity>> margin{};

  /// Zones whose delivery is affected, in canonical order. Empty when the
  /// constraint is real but nothing downstream consumes it.
  std::vector<ZoneId> affected_zones{};

  /// Delivery points whose evidence was used to make the attribution.
  std::vector<StrongId> affected_points{};

  /// Why the attribution is not Direct, when it is not.
  std::vector<Indeterminacy> indeterminacies{};

  Freshness freshness = Freshness::Unknown;
};

/// The full constraint picture.
struct ConstraintReport {
  GenerationId generation{};
  EpochId epoch{};
  Revision revision{};
  TimestampMs as_of_ms = 0;
  std::vector<ConstraintAttribution> constraints{};
  /// Constraints declared by an authority whose declaring reference is stale,
  /// listed so that a consumer can see the claim without relying on it.
  std::size_t stale_constraint_count = 0;
  /// Constraints whose element could not be located in the adopted structure.
  std::size_t unattributed_count = 0;
  Freshness freshness = Freshness::Unknown;
};

/// Which delivery point to analyse, or all of them.
struct ConstraintQuery {
  Maybe<StrongId> point{};
  Maybe<LoopId> loop{};
  Maybe<ZoneId> zone{};
  /// Include constraints whose evidence is stale. Off by default: a stale
  /// constraint is reported as a stale claim, not as a live restriction.
  bool include_stale = false;
};

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_CONSTRAINT_HPP
