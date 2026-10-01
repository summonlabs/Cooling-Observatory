// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/limits.hpp"

namespace dccp::cooling_observatory {

const Limits& default_limits() noexcept {
  static const Limits limits;
  return limits;
}

Status validate(const Limits& limits) {
  struct Entry {
    const char* name;
    std::size_t value;
  };
  // Structural limits must be positive: a zero here would make the runtime
  // accept nothing at all, which is never what a caller means.
  const Entry structural[] = {
      {"max_facilities", limits.max_facilities},
      {"max_plants", limits.max_plants},
      {"max_loops", limits.max_loops},
      {"max_elements", limits.max_elements},
      {"max_zones", limits.max_zones},
      {"max_links", limits.max_links},
      {"max_measurements", limits.max_measurements},
      {"max_states", limits.max_states},
      {"max_capabilities", limits.max_capabilities},
      {"max_reserve_claims", limits.max_reserve_claims},
      {"max_constraints", limits.max_constraints},
      {"max_failures", limits.max_failures},
      {"max_records", limits.max_records},
      {"max_report_rows", limits.max_report_rows},
      {"max_traversal_nodes", limits.max_traversal_nodes},
      {"max_traversal_depth", limits.max_traversal_depth},
      {"max_explanation_lines", limits.max_explanation_lines},
      {"max_evidence_refs", limits.max_evidence_refs},
      {"max_snapshot_bytes", limits.max_snapshot_bytes},
      {"max_path_bytes", limits.max_path_bytes},
  };
  for (const Entry& entry : structural) {
    if (entry.value == 0) {
      return Status::failure(Code::InvalidArgument, "limit_must_be_positive",
                             std::string("limit '") + entry.name + "' must be greater than zero");
    }
  }
  if (limits.max_traversal_depth > 4096) {
    return Status::failure(Code::InvalidArgument, "limit_too_large",
                           "max_traversal_depth above 4096 is refused: a traversal that deep is a cycle");
  }
  if (limits.max_snapshot_bytes > (1ull << 40)) {
    return Status::failure(Code::InvalidArgument, "limit_too_large",
                           "max_snapshot_bytes above 1 TiB is refused");
  }
  if (limits.max_tolerance < 0) {
    return Status::failure(Code::InvalidArgument, "limit_negative",
                           "max_tolerance must not be negative");
  }
  if (limits.max_measurements > limits.max_snapshot_bytes) {
    // A snapshot cannot hold more measurements than it has bytes, so this
    // combination is unsatisfiable and is caught here rather than at commit.
    return Status::failure(Code::InvalidArgument, "limit_inconsistent",
                           "max_measurements exceeds max_snapshot_bytes; no snapshot could hold them");
  }
  return Status::success();
}

std::size_t effective_max_snapshot_bytes(const Limits& limits) noexcept { return limits.max_snapshot_bytes; }

Error limit_error(std::string_view limit_name, std::size_t limit, std::size_t observed) {
  return Error(Code::LimitExceeded, std::string(limit_name),
               "limit " + std::to_string(limit) + " exceeded by " + std::to_string(observed));
}

}  // namespace dccp::cooling_observatory
