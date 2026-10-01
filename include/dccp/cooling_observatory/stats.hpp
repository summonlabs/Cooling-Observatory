// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_STATS_HPP
#define DCCP_COOLING_OBSERVATORY_STATS_HPP

#include <atomic>
#include <cstdint>
#include <string>

#include "dccp/cooling_observatory/identity.hpp"

namespace dccp::cooling_observatory {

/// Counters describing what this engine instance has done.
struct Stats {
  std::uint64_t commits = 0;
  std::uint64_t records_applied = 0;
  std::uint64_t records_rejected = 0;
  std::uint64_t duplicates_rejected = 0;
  std::uint64_t evidence_retired = 0;
  std::uint64_t structure_generations = 0;
  std::uint64_t snapshot_writes = 0;
  std::uint64_t snapshot_recoveries = 0;
  std::uint64_t reopen_count = 0;
  std::uint64_t snapshot_bytes_written = 0;
  std::uint64_t snapshot_bytes_read = 0;
  std::uint64_t lock_contention = 0;
  std::uint64_t queries = 0;

  Revision revision{};
  RecordSeq sequence{};
  EpochId epoch{};
  GenerationId generation{};

  /// Deterministic, line-oriented rendering: one counter per line, in a fixed
  /// order, so two runs can be compared with a text diff.
  [[nodiscard]] std::string render() const;
};

/// Counters an engine instance owns.
///
/// Each counter is atomic on its own. The counters are deliberately not
/// published as one atomic unit: a reader that needs a consistent whole reads
/// the committed state under the engine lock instead, and this type exists so
/// that an operator can see activity without taking that lock at all.
class CounterSet {
 public:
  void increment(std::atomic<std::uint64_t>& counter) noexcept;
  [[nodiscard]] Stats snapshot(Revision revision, RecordSeq sequence, EpochId epoch,
                               GenerationId generation) const noexcept;

  std::atomic<std::uint64_t> commits{0};
  std::atomic<std::uint64_t> records_applied{0};
  std::atomic<std::uint64_t> records_rejected{0};
  std::atomic<std::uint64_t> duplicates_rejected{0};
  std::atomic<std::uint64_t> evidence_retired{0};
  std::atomic<std::uint64_t> structure_generations{0};
  std::atomic<std::uint64_t> snapshot_writes{0};
  std::atomic<std::uint64_t> snapshot_recoveries{0};
  std::atomic<std::uint64_t> reopen_count{0};
  std::atomic<std::uint64_t> snapshot_bytes_written{0};
  std::atomic<std::uint64_t> snapshot_bytes_read{0};
  std::atomic<std::uint64_t> lock_contention{0};
  std::atomic<std::uint64_t> queries{0};
};

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_STATS_HPP
