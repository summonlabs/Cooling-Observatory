// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_PERSISTENCE_HPP
#define DCCP_COOLING_OBSERVATORY_PERSISTENCE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "dccp/cooling_observatory/error.hpp"
#include "dccp/cooling_observatory/identity.hpp"
#include "dccp/cooling_observatory/ingest.hpp"
#include "dccp/cooling_observatory/limits.hpp"

namespace dccp::cooling_observatory {

/// How a store behaves. Every field is a decision the caller makes explicitly,
/// because durability, atomicity and single-writer enforcement are properties a
/// consumer needs to be able to state, not defaults it discovers later.
struct StoreOptions {
  /// Replace the live state file atomically on every commit. When false, a
  /// commit only updates memory and a caller must call Store::flush() itself;
  /// that mode is offered for throughput experiments and is never the default,
  /// because a state that is not on disk is not durable.
  bool atomic_commit = true;

  /// Take a kernel-enforced exclusive lock on the state directory. Only one
  /// process may hold the store open; a second open fails with StoreBusy rather
  /// than corrupting the state.
  bool exclusive_writer = true;

  /// Flush file buffers to the device before the atomic replace. Turning this
  /// off makes a crash window in which the replaced file's bytes are not on
  /// stable storage; the default keeps the window closed.
  bool sync_before_replace = true;

  Limits limits{};
};

/// What a store found when it opened its state directory.
struct StoreManifest {
  Revision revision{};
  RecordSeq sequence{};
  EpochId epoch{};
  GenerationId generation{};
  TimestampMs committed_at_ms = 0;
  std::size_t evidence_count = 0;
  std::string digest{};
  /// True when the live state was unusable and the store fell back to the
  /// previous generation. A recovered state is readable and is never promoted
  /// to current: the engine marks recovered evidence as recovered.
  bool recovered = false;
  /// Why the fallback happened, when it did.
  std::string recovery_reason{};
  /// The schema version of the file that was read.
  std::uint16_t schema_version = 0;
};

/// The durable home of one engine's state.
///
/// The store owns exactly one file per generation and commits by writing a new
/// generation and replacing the live name atomically. There is therefore never
/// a moment at which the live name refers to a partially written file: a
/// process that dies mid-commit leaves either the previous generation or the
/// new one, and the CRC on the file decides which.
class Store {
 public:
  Store();
  ~Store();

  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  /// Open a state directory, creating it when absent, and load the newest
  /// usable state.
  ///
  /// A missing state is not an error: it means this store has never committed,
  /// and the returned manifest is empty.
  Status open(const std::string& directory, const StoreOptions& options);

  /// Persist an image. The commit point is the atomic replace of the live file
  /// name: after this returns successfully, a process that dies and reopens
  /// observes the committed state, and before it returns, no observer can see a
  /// partial one.
  Status commit(const ObservationImage& image, StoreManifest& manifest_out);

  /// Re-read the newest state from disk. A store already holds its image, so
  /// this exists for the reopen proof rather than for normal operation.
  Status reload(ObservationImage& image_out, StoreManifest& manifest_out);

  /// The state as last loaded or committed.
  [[nodiscard]] const ObservationImage& image() const;
  [[nodiscard]] const StoreManifest& manifest() const;
  [[nodiscard]] bool open_state() const noexcept;
  [[nodiscard]] const std::string& directory() const noexcept;
  [[nodiscard]] std::size_t last_encode_bytes() const noexcept;

  /// Release the writer lock and forget the loaded state. Committing after
  /// close fails with StoreClosed rather than writing to a directory this
  /// process no longer owns.
  void close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Encode an image into the durable byte representation. Published so that the
/// tests and the benchmarks can measure encoding without a filesystem.
[[nodiscard]] Result<std::string> encode_image(const ObservationImage& image, const Limits& limits);

/// Decode a durable representation. Rejects a schema this build cannot read and
/// an interior corruption; reports a conservation failure as corrupt rather
/// than returning a state that is missing records.
[[nodiscard]] Result<ObservationImage> decode_image(std::string_view bytes, std::size_t max_bytes);

/// The digest of the canonical encoding of an image's content.
[[nodiscard]] std::string image_digest(const ObservationImage& image);

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_PERSISTENCE_HPP
