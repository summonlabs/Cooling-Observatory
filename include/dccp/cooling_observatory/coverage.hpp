// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_COVERAGE_HPP
#define DCCP_COOLING_OBSERVATORY_COVERAGE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/cooling_observatory/delivery.hpp"
#include "dccp/cooling_observatory/evidence.hpp"
#include "dccp/cooling_observatory/plant.hpp"

namespace dccp::cooling_observatory {

/// What is missing for a subject, on one evidence axis.
struct CoverageGap {
  SubjectRef subject{};
  EvidenceAxis axis = EvidenceAxis::Delivery;
  /// The best freshness found on that axis, or Unknown when nothing was found.
  Freshness best = Freshness::Unknown;
  /// How many records were considered on that axis.
  std::size_t record_count = 0;
  /// The age of the freshest record, when any exists.
  Maybe<DurationMs> age_ms{};
  /// A stable reason token: "no_evidence", "stale", "recovered", "conflicting",
  /// "unknown_value".
  std::string reason{};

  friend bool operator<(const CoverageGap& a, const CoverageGap& b) noexcept {
    if (a.subject != b.subject) {
      return a.subject < b.subject;
    }
    return static_cast<std::uint8_t>(a.axis) < static_cast<std::uint8_t>(b.axis);
  }
};

/// Coverage of one evidence axis.
struct AxisCoverage {
  EvidenceAxis axis = EvidenceAxis::Delivery;
  std::size_t subjects_total = 0;
  std::size_t subjects_fresh = 0;
  std::size_t subjects_stale = 0;
  std::size_t subjects_conflicting = 0;
  std::size_t subjects_unknown = 0;
  /// subjects_fresh in parts per million of subjects_total, or absent when
  /// there are no subjects. A ratio over an empty population is not zero; it is
  /// undefined, and reporting it as zero would read as total failure.
  Maybe<std::int64_t> fresh_ratio_ppm{};
};

/// The coverage picture: what the observatory can and cannot speak about.
struct CoverageReport {
  GenerationId generation{};
  EpochId epoch{};
  Revision revision{};
  TimestampMs as_of_ms = 0;
  std::vector<AxisCoverage> axes{};
  /// Gaps in canonical order, bounded by the configured row limit.
  std::vector<CoverageGap> gaps{};
  bool gaps_truncated = false;
  /// Zones with no fresh delivery evidence at all. The single most important
  /// line an observatory can print.
  std::vector<ZoneId> unobserved_zones{};
  /// Zones whose declared load has no corresponding measured removal.
  std::vector<ZoneId> unverified_load_zones{};
  Freshness freshness = Freshness::Unknown;
};

struct CoverageQuery {
  Maybe<LoopId> loop{};
  Maybe<PlantId> plant{};
  /// Include subjects that are fully covered.
  bool include_covered = false;
};

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_COVERAGE_HPP
