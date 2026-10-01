// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/cooling_observatory/dependency.hpp"

#include "analysis.hpp"

#include <algorithm>
#include <deque>
#include <map>

namespace dccp::cooling_observatory {
namespace {

/// A load is a heat source: it consumes cooling and produces none, so a
/// traversal that walks through it would attribute a constraint to the thing
/// being cooled rather than to the thing cooling it.
bool subject_is_load(const PlantModel& model, const SubjectRef& subject) {
  if (subject.kind == SubjectKind::RackGroup || subject.kind == SubjectKind::Zone) {
    return true;
  }
  const std::optional<PlantComponent> component = model.component(subject.kind, subject.id);
  if (!component.has_value()) {
    return false;
  }
  return component.value().kind == ComponentKind::RackGroup ||
         component.value().kind == ComponentKind::Reservoir;
}

std::vector<PlantLink> links_from(const PlantModel& model, const SubjectRef& subject,
                                 const TraversalOptions& options) {
  if (options.direction == TraversalOptions::Direction::Downstream) {
    return model.outgoing(subject, options.delivery_only);
  }
  return model.incoming(subject, options.delivery_only);
}

}  // namespace

bool is_load(const PlantModel& model, const SubjectRef& subject) { return subject_is_load(model, subject); }

Result<DependencyTraversal> traverse(const PlantModel& model, const SubjectRef& root,
                                     const TraversalOptions& options, GenerationId generation,
                                     Revision revision) {
  DependencyTraversal out;
  out.root = root;
  out.generation = generation;
  out.revision = revision;

  if (root.id.empty()) {
    return fail(Code::InvalidArgument, "traversal_root_required",
                "a traversal must name the subject it starts from");
  }
  if (options.max_depth == 0 || options.max_nodes == 0) {
    return fail(Code::InvalidArgument, "traversal_limit_zero",
                "a traversal needs a non-zero depth and node budget");
  }
  if (!model.knows(root)) {
    // The root itself is not in the adopted structure. That is an answer, not
    // an error: the caller asked about something this generation does not name.
    return fail(Code::MissingEvidence, "traversal_root_unknown",
                "the subject " + root.id.str() + " is not present in the adopted structure");
  }

  std::map<SubjectRef, bool> visited;
  std::deque<DependencyNode> frontier;

  if (options.include_root) {
    DependencyNode node;
    node.subject = root;
    node.depth = 0;
    out.nodes.push_back(node);
    frontier.push_back(node);
    visited[root] = true;
  } else {
    // Without the root in the result, its direct neighbours are the depth-zero
    // seeds. They are inserted explicitly so that a caller who does not want
    // the root still gets a traversal that starts at the right place.
    for (const PlantLink& link : links_from(model, root, options)) {
      DependencyNode node;
      node.subject = options.direction == TraversalOptions::Direction::Downstream ? link.to : link.from;
      node.depth = 1;
      node.via = Maybe<LinkRelation>::of(link.relation);
      node.parent = Maybe<SubjectRef>::of(root);
      if (visited.find(node.subject) == visited.end()) {
        visited[node.subject] = true;
        out.nodes.push_back(node);
        frontier.push_back(node);
      }
    }
  }

  while (!frontier.empty()) {
    const DependencyNode current = frontier.front();
    frontier.pop_front();

    if (current.depth >= options.max_depth) {
      // The node is at the depth ceiling. If it has any edge at all, the
      // traversal is incomplete and says so rather than presenting a partial
      // answer as a whole one.
      if (!links_from(model, current.subject, options).empty()) {
        out.truncated = true;
        out.truncated_by = "max_traversal_depth";
      }
      continue;
    }

    for (const PlantLink& link : links_from(model, current.subject, options)) {
      const SubjectRef& next =
          options.direction == TraversalOptions::Direction::Downstream ? link.to : link.from;
      if (options.stop_at_loads && next != root && subject_is_load(model, next)) {
        // The load is reached, recorded as a leaf, and not descended into. It is
        // marked visited here rather than merely checked, so that a second route
        // arriving at the same load does not walk past it: the load is the end
        // of the delivery path whichever route reached it first.
        if (visited.find(next) == visited.end()) {
          // A load is a node like any other: it counts against the budget, and a
          // traversal that cannot record it says it was truncated.
          if (out.nodes.size() >= options.max_nodes) {
            out.truncated = true;
            out.truncated_by = "max_traversal_nodes";
            break;
          }
          visited[next] = true;
          DependencyNode leaf;
          leaf.subject = next;
          leaf.depth = current.depth + 1;
          leaf.via = Maybe<LinkRelation>::of(link.relation);
          leaf.parent = Maybe<SubjectRef>::of(current.subject);
          out.nodes.push_back(leaf);
          out.traversed.push_back(link);
        }
        continue;
      }
      if (visited.find(next) != visited.end()) {
        continue;
      }
      if (out.nodes.size() >= options.max_nodes) {
        out.truncated = true;
        out.truncated_by = "max_traversal_nodes";
        break;
      }
      visited[next] = true;
      DependencyNode node;
      node.subject = next;
      node.depth = current.depth + 1;
      node.via = Maybe<LinkRelation>::of(link.relation);
      node.parent = Maybe<SubjectRef>::of(current.subject);
      out.nodes.push_back(node);
      out.traversed.push_back(link);
      frontier.push_back(node);
    }
    if (out.truncated) {
      break;
    }
  }

  // Dangling edges are reported, not skipped. An edge into an element the
  // adopted generation does not contain is a real gap in the structure, and a
  // consumer that never sees it will assume the plant is fully described.
  for (const PlantLink& link : model.links()) {
    if (!options.delivery_only || carries_delivery(link.relation)) {
      const bool from_known = model.knows(link.from);
      const bool to_known = model.knows(link.to);
      if (!from_known || !to_known) {
        out.dangling.push_back(link);
      }
    }
  }
  std::sort(out.dangling.begin(), out.dangling.end());
  out.dangling.erase(std::unique(out.dangling.begin(), out.dangling.end()), out.dangling.end());

  // Canonical order: by depth, then by identity. Breadth-first discovery order
  // is already a function of the canonical adjacency order, so this sort does
  // not change which nodes are present, only the order they are reported in.
  std::sort(out.nodes.begin(), out.nodes.end());
  std::sort(out.traversed.begin(), out.traversed.end());
  out.traversed.erase(std::unique(out.traversed.begin(), out.traversed.end()), out.traversed.end());
  return out;
}

Result<DependencyTraversal> analyse_dependencies(const AnalysisContext& context, const SubjectRef& root,
                                                const TraversalOptions& options,
                                                GenerationId generation, Revision revision) {
  // The traversal is defined entirely by the adopted structure, so this is the
  // same function the public API exposes with the context supplying the
  // generation and revision it was computed against.
  return traverse(context.structure(), root, options, generation, revision);
}

}  // namespace dccp::cooling_observatory