// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/stats.hpp"

namespace dccp::cooling_observatory {

std::string Stats::render() const {
  std::string out;
  out.reserve(512);
  auto line = [&out](const char* name, std::uint64_t value) {
    out += name;
    out += ' ';
    out += std::to_string(value);
    out += '\n';
  };
  line("commits", commits);
  line("records_applied", records_applied);
  line("records_rejected", records_rejected);
  line("duplicates_rejected", duplicates_rejected);
  line("evidence_retired", evidence_retired);
  line("structure_generations", structure_generations);
  line("snapshot_writes", snapshot_writes);
  line("snapshot_recoveries", snapshot_recoveries);
  line("reopen_count", reopen_count);
  line("snapshot_bytes_written", snapshot_bytes_written);
  line("snapshot_bytes_read", snapshot_bytes_read);
  line("lock_contention", lock_contention);
  line("queries", queries);
  line("revision", revision.value());
  line("sequence", sequence.value());
  line("epoch", epoch.value());
  line("generation", generation.value());
  return out;
}

void CounterSet::increment(std::atomic<std::uint64_t>& counter) noexcept {
  counter.fetch_add(1, std::memory_order_relaxed);
}

Stats CounterSet::snapshot(Revision revision_value, RecordSeq sequence_value, EpochId epoch_value,
                           GenerationId generation_value) const noexcept {
  Stats out;
  out.commits = commits.load(std::memory_order_relaxed);
  out.records_applied = records_applied.load(std::memory_order_relaxed);
  out.records_rejected = records_rejected.load(std::memory_order_relaxed);
  out.duplicates_rejected = duplicates_rejected.load(std::memory_order_relaxed);
  out.evidence_retired = evidence_retired.load(std::memory_order_relaxed);
  out.structure_generations = structure_generations.load(std::memory_order_relaxed);
  out.snapshot_writes = snapshot_writes.load(std::memory_order_relaxed);
  out.snapshot_recoveries = snapshot_recoveries.load(std::memory_order_relaxed);
  out.reopen_count = reopen_count.load(std::memory_order_relaxed);
  out.snapshot_bytes_written = snapshot_bytes_written.load(std::memory_order_relaxed);
  out.snapshot_bytes_read = snapshot_bytes_read.load(std::memory_order_relaxed);
  out.lock_contention = lock_contention.load(std::memory_order_relaxed);
  out.queries = queries.load(std::memory_order_relaxed);
  out.revision = revision_value;
  out.sequence = sequence_value;
  out.epoch = epoch_value;
  out.generation = generation_value;
  return out;
}

}  // namespace dccp::cooling_observatory
