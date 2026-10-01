// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_LIMITS_HPP
#define DCCP_COOLING_OBSERVATORY_LIMITS_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "dccp/cooling_observatory/error.hpp"

namespace dccp::cooling_observatory {

/// Bounded resource behaviour.
///
/// Every collection this library builds is bounded by a number that appears
/// here. A cooling facility can produce unbounded evidence, and an observatory
/// that accepts unbounded input is an observatory that a single misbehaving
/// collector can stop. A limit breach is a reported outcome, never a truncation
/// that silently changes an answer.
struct Limits {
  /// Structure
  std::size_t max_facilities = 16;
  std::size_t max_plants = 256;
  std::size_t max_loops = 4096;
  std::size_t max_elements = 65536;      ///< pumps, valves, chillers, CDUs, CRAH/CRAC, manifolds
  std::size_t max_zones = 16384;
  std::size_t max_links = 262144;

  /// Evidence
  std::size_t max_measurements = 200000;
  std::size_t max_states = 65536;
  std::size_t max_capabilities = 65536;
  std::size_t max_reserve_claims = 65536;
  std::size_t max_constraints = 16384;
  std::size_t max_failures = 16384;
  std::size_t max_records = 4096;        ///< records accepted in one ingest call

  /// Answers
  std::size_t max_report_rows = 65536;
  std::size_t max_traversal_nodes = 65536;
  std::size_t max_traversal_depth = 64;
  std::size_t max_explanation_lines = 512;
  std::size_t max_evidence_refs = 64;

  /// Persistence
  std::size_t max_snapshot_bytes = 512ull * 1024ull * 1024ull;
  std::size_t max_journal_bytes = 64ull * 1024ull * 1024ull;
  std::size_t max_path_bytes = 32767;    ///< the Windows long-path ceiling

  /// Quantities
  /// Widest accepted tolerance for equality of two measurements, in the
  /// quantity's own base unit. Beyond this a "tolerance" is a request to
  /// declare everything equal.
  std::int64_t max_tolerance = 1'000'000'000;
};

/// The limits this build uses unless the caller supplies others.
[[nodiscard]] const Limits& default_limits() noexcept;

/// Validate a limit set. Rejects zero-valued structural limits (which would
/// make the runtime useless) and out-of-range byte ceilings.
[[nodiscard]] Status validate(const Limits& limits);

/// The largest snapshot this process will read or write.
[[nodiscard]] std::size_t effective_max_snapshot_bytes(const Limits& limits) noexcept;

/// Human-readable rendering of the limit that a breach hit.
[[nodiscard]] Error limit_error(std::string_view limit_name, std::size_t limit, std::size_t observed);

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_LIMITS_HPP
