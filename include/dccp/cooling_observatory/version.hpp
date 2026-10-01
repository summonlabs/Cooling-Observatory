// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_VERSION_HPP
#define DCCP_COOLING_OBSERVATORY_VERSION_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace dccp::cooling_observatory {

/// Semantic version of this build, baked in at compile time.
struct Version {
  std::uint16_t major = 1;
  std::uint16_t minor = 0;
  std::uint16_t patch = 0;
};

/// The version of this library build.
[[nodiscard]] Version library_version() noexcept;

/// The version as "major.minor.patch".
[[nodiscard]] std::string version_string();

/// Producer identity stamped into provenance that this library originates,
/// for example "dccp-cooling-observatory/1.0.0".
[[nodiscard]] std::string_view producer_identity() noexcept;

/// Schema version of the durable snapshot format. A store refuses a snapshot
/// whose schema it cannot decode rather than guessing at its contents.
inline constexpr std::uint16_t kSnapshotSchemaVersion = 1;

/// Schema version of the observation interchange encoding.
inline constexpr std::uint16_t kInterchangeSchemaVersion = 1;

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_VERSION_HPP
