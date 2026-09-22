// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Declares a host-only, non-PBQP min-plus generality probe: single-source
// shortest paths (Bellman-Ford) expressed in terms of PCAA's existing
// conceptual primitives. See doc/generality-probe.md for what it exercises
// and what it would need beyond opcodes 1-4. No RTL/ABI/SystemC involvement.

#pragma once

#include <cstdint>
#include <vector>

namespace pcaa::probes {

// A directed, weighted edge. `weight` is a PCAA cost: a finite int32_t or
// ACCEL_INF for a structurally absent edge (this probe never constructs one
// with that value; ACCEL_INF only ever appears as an unreached distance).
struct Edge {
  int from;
  int to;
  int32_t weight;
};

struct ShortestPathResult {
  // distance[v] is the shortest-path cost from the source to v, or
  // ACCEL_INF if v is unreached.
  std::vector<int32_t> distance;
  // predecessor[v] is the vertex before v on a shortest path, or -1 for the
  // source or an unreached vertex.
  std::vector<int> predecessor;
  // True if a negative-weight cycle is reachable from the source, in which
  // case distance/predecessor are not shortest-path values.
  bool has_negative_cycle;
};

// Single-source shortest paths via Bellman-Ford. Each relaxation round does
// one conceptual MAP_ADD_REDUCE_MIN_ARGMIN pass per vertex over its
// incoming edges: cost-add each predecessor's current distance with the
// edge weight, take the minimum and the edge that achieved it. Ties resolve
// to the first minimal incoming edge, matching PCAA's own argmin tie-break.
ShortestPathResult BellmanFord(int vertex_count, const std::vector<Edge> &edges, int source);

}  // namespace pcaa::probes
