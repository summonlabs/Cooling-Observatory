// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/version.hpp"

namespace dccp::cooling_observatory {

Version library_version() noexcept { return Version{1, 0, 0}; }

std::string version_string() {
  const Version v = library_version();
  std::string out;
  out.reserve(16);
  out += std::to_string(v.major);
  out += '.';
  out += std::to_string(v.minor);
  out += '.';
  out += std::to_string(v.patch);
  return out;
}

std::string_view producer_identity() noexcept { return "dccp-cooling-observatory/1.0.0"; }

}  // namespace dccp::cooling_observatory
