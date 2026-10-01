// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_COOLING_OBSERVATORY_DEPENDENCY_HPP
#define DCCP_COOLING_OBSERVATORY_DEPENDENCY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dccp/cooling_observatory/error.hpp"
#include "dccp/cooling_observatory/plant.hpp"

namespace dccp::cooling_observatory {

/// One step of a dependency traversal.
struct DependencyNode {
  SubjectRef subject{};
  /// Distance in delivery edges from the traversal root. The root is 0.
  std::uint32_t depth = 0;
  /// The edge that reached this node. Absent at the root.
  Maybe<LinkRelation> via{};
  /// The node this one was reached from. Absent at the root.
  Maybe<SubjectRef> parent{};

  friend bool operator==(const DependencyNode& a, const DependencyNode& b) noexcept {
    return a.subject == b.subject && a.depth == b.depth;
  }
  friend bool operator<(const DependencyNode& a, const DependencyNode& b) noexcept {
    if (a.depth != b.depth) {
      return a.depth < b.depth;
    }
    return a.subject < b.subject;
  }
};

/// A traversal result. Ordering is canonical: breadth first, then by identity,
/// so two runs over the same structure produce byte-identical output.
struct DependencyTraversal {
  SubjectRef root{};
  std::vector<DependencyNode> nodes{};
  /// Links that were followed. Reported so that an explanation can show the
  /// path, not only the destination.
  std::vector<PlantLink> traversed{};
  /// True when the traversal stopped because it hit a limit rather than
  /// exhausting the structure. A truncated traversal is never presented as a
  /// complete one.
  bool truncated = false;
  /// The limit that stopped it, when truncated.
  std::string truncated_by{};
  /// Links whose endpoints are not present in the model. Reported rather than
  /// skipped: a dangling edge in an adopted generation is a real finding.
  std::vector<PlantLink> dangling{};
  GenerationId generation{};
  /// The exact structure revision the traversal used.
  Revision revision{};
};

/// How a traversal walks the structure.
struct TraversalOptions {
  /// Follow edges away from the root (what the root feeds) or towards it (what
  /// feeds the root).
  enum class Direction : std::uint8_t { Downstream = 0, Upstream = 1 };
  Direction direction = Direction::Downstream;
  /// Follow only relations that carry delivered coolant or air.
  bool delivery_only = true;
  /// When true, edges that lead into a zone or rack group are not descenced
  /// further. Loads are heat sources, not cooling equipment.
  bool stop_at_loads = true;
  /// Include the root itself in nodes. The default keeps it, because a caller
  /// asking "what depends on this" usually wants the answer to include it.
  bool include_root = true;
  std::uint32_t max_depth = 64;
  std::size_t max_nodes = 65536;
};

/// Traverse the structure from a root. Deterministic, bounded, and explicit
/// about truncation and about dangling edges.
[[nodiscard]] Result<DependencyTraversal> traverse(const PlantModel& model, const SubjectRef& root,
                                                   const TraversalOptions& options = {},
                                                   GenerationId generation = {},
                                                   Revision revision = {});

/// True when the subject is a heat load rather than cooling equipment.
[[nodiscard]] bool is_load(const PlantModel& model, const SubjectRef& subject);

}  // namespace dccp::cooling_observatory

#endif  // DCCP_COOLING_OBSERVATORY_DEPENDENCY_HPP
