// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_RESERVE_HPP
#define DCCP_COOLING_OBSERVATORY_RESERVE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/cooling_observatory/delivery.hpp"
#include "dccp/cooling_observatory/evidence.hpp"
#include "dccp/cooling_observatory/plant.hpp"

namespace dccp::cooling_observatory {

/// Which evidence a reserve figure rests on.
///
/// The two are never combined into one number. A declared reserve with no
/// measured delivery behind it is a plan; a measured headroom with no declared
/// capability behind it is an observation that cannot be extrapolated. Merging
/// them produces a figure that looks authoritative and is neither.
enum class ReserveBasis : std::uint8_t {
  Unknown = 0,     ///< nothing current supports a reserve figure
  Claimed = 1,     ///< declared reserve claims only
  Evidenced = 2,   ///< measured delivery against measured capacity
  Mixed = 3,       ///< some contributors evidenced, some merely claimed
};

[[nodiscard]] std::string_view to_token(ReserveBasis basis) noexcept;
[[nodiscard]] std::optional<ReserveBasis> parse_reserve_basis(std::string_view token) noexcept;

/// How one contribution to reserve was obtained.
enum class ReserveContributionKind : std::uint8_t {
  DeclaredClaim = 0,      ///< from a reserve claim record
  MeasuredHeadroom = 1,   ///< measured capability minus measured delivery
  StandbyElement = 2,     ///< an element reported available and not delivering
  RecoveredEstimate = 3,  ///< from evidence restored after a restart
};

[[nodiscard]] std::string_view to_token(ReserveContributionKind kind) noexcept;
[[nodiscard]] std::optional<ReserveContributionKind> parse_reserve_contribution_kind(std::string_view token) noexcept;

/// One element of a reserve figure, with everything needed to reject it.
struct ReserveContribution {
  ReserveContributionKind kind = ReserveContributionKind::DeclaredClaim;
  /// The element this contribution is about. Absent for a claim stated for a
  /// loop or a zone as a whole.
  Maybe<SubjectRef> element{};
  StrongId claim_id{};   ///< the claim record, when the contribution came from one
  Quantity amount{};     ///< non-negative, in the report's dimension
  Freshness freshness = Freshness::Unknown;

  /// Assumptions the contribution rests on that this runtime did not verify.
  std::vector<StrongId> unverified_assumptions{};
  /// Assumptions that were checked and found false. Any non-empty entry makes
  /// the whole contribution unusable.
  std::vector<StrongId> refuted_assumptions{};
  std::vector<EvidenceRef> evidence{};
};

/// The reserve evidenced at one scope.
struct ReserveObservation {
  /// The scope: a loop, a zone or a plant.
  SubjectRef scope{};
  Dimension dimension = Dimension::Flow;

  /// Reserve claimed by declarations whose evidence is current.
  Judged<Maybe<Quantity>> claimed{};
  /// Reserve supported by measured delivery and measured capability.
  Judged<Maybe<Quantity>> evidenced{};
  /// The smaller of the two when both exist. This is the number a capacity
  /// decision may use, and it is absent when either input is absent.
  Judged<Maybe<Quantity>> binding{};
  /// Which basis the binding figure rests on.
  ReserveBasis basis = ReserveBasis::Unknown;

  /// Measured delivery at this scope.
  Judged<Maybe<Quantity>> delivered{};
  /// Measured and declared capability available at this scope.
  Judged<Maybe<Quantity>> capability{};

  std::vector<ReserveContribution> contributions{};
  std::vector<Indeterminacy> indeterminacies{};
  Freshness freshness = Freshness::Unknown;
};

struct ReserveReport {
  GenerationId generation{};
  EpochId epoch{};
  Revision revision{};
  TimestampMs as_of_ms = 0;
  Dimension dimension = Dimension::Flow;
  std::vector<ReserveObservation> scopes{};
  Freshness freshness = Freshness::Unknown;
};

struct ReserveQuery {
  Maybe<LoopId> loop{};
  Maybe<ZoneId> zone{};
  Maybe<PlantId> plant{};
  /// Require reserve to be backed by measurement rather than by declaration.
  bool require_evidenced = false;
};

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_RESERVE_HPP
