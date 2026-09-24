// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Declares a host-only, non-PBQP min-plus generality probe: single-source
// shortest paths (Bellman-Ford) used as an independent software oracle for
// the pcaalib/SystemC device probe. See doc/generality-probe.md.

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
  // case distance/predecessor are not shortest-path values. Decided with exact
  // 64-bit arithmetic, independent of the accelerator's bounded cost domain.
  bool has_negative_cycle;
  // True if some reachable shortest-path cost is below INT32_MIN, so the
  // matching software-oracle distance[] entry is saturated, not exact.
  bool distance_saturated;
};

// Single-source shortest paths via Bellman-Ford. Each relaxation round does
// one conceptual MAP_ADD_REDUCE_MIN_ARGMIN pass per vertex over its
// incoming edges: cost-add each predecessor's current distance with the
// edge weight, take the minimum and the edge that achieved it. Ties resolve
// to the first minimal incoming edge, matching PCAA's own argmin tie-break.
// The software oracle saturates at INT32_MIN to study out-of-domain paths;
// the accelerator instead reports negative underflow as an error. Cycle
// detection and saturation reporting use a separate exact 64-bit relaxation. It cannot
// overflow while vertex_count * edges.size() < 2^32: every value it holds is
// the weight of a walk of at most that many 32-bit-weighted edges.
ShortestPathResult BellmanFord(int vertex_count, const std::vector<Edge> &edges, int source);

}  // namespace pcaa::probes
