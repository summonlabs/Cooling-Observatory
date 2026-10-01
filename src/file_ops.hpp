// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Durable file primitives, isolated in one translation unit so that the
// platform-specific parts of durability are reviewable in one place.

#ifndef DCCP_COOLING_OBSERVATORY_FILE_OPS_HPP
#define DCCP_COOLING_OBSERVATORY_FILE_OPS_HPP

#include <cstddef>
#include <string>
#include <string_view>

#include "dccp/cooling_observatory/error.hpp"

namespace dccp::cooling_observatory {

/// An open writer lock. The platform handle is stored as a void pointer so that
/// no platform header reaches a caller.
struct LockHandle {
  void* handle = nullptr;
  bool valid() const noexcept { return handle != nullptr; }
};

/// Join two path components with the platform separator.
[[nodiscard]] std::string join_path(std::string_view directory, std::string_view name);

/// Create a directory and every missing parent.
[[nodiscard]] Status ensure_directory(const std::string& path);

/// Read a whole file. A missing file reports NotFound, and is not an error the
/// caller has to distinguish from a failure by inspecting a platform code.
[[nodiscard]] Result<std::string> read_file(const std::string& path, std::size_t max_bytes);

/// Write bytes to a temporary name in the same directory and replace the live
/// name with it.
///
/// The commit point is the replace. Before it, the live name refers to the
/// previous content in full; after it, to the new content in full; there is no
/// interval in which it refers to a mixture. When sync is true the temporary
/// file is flushed to stable storage before the replace, so the window in which
/// a power loss could lose the new state is closed rather than merely narrowed.
[[nodiscard]] Status write_file_atomic(const std::string& temporary_path, const std::string& live_path,
                                       std::string_view bytes, bool sync);

/// Remove a file, treating absence as success. Used to clean up a temporary
/// file after a failed commit.
[[nodiscard]] Status remove_file(const std::string& path);

/// Take a kernel-enforced exclusive lock. Returns StoreBusy when another
/// process already holds it.
[[nodiscard]] Result<LockHandle> acquire_writer_lock(const std::string& path);

/// Release a lock taken by acquire_writer_lock. Safe on an invalid handle.
void release_writer_lock(LockHandle& handle) noexcept;

/// True when a path exists and is a regular file.
[[nodiscard]] bool file_exists(const std::string& path);

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_FILE_OPS_HPP
