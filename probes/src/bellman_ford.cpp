// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements the Bellman-Ford generality probe declared in bellman_ford.h.

#include "bellman_ford.h"

#include "accel_protocol.h"
#include "cost_math.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace pcaa::probes {
namespace {

// Conceptually PCAA's MAP_ADD_REDUCE_MIN_ARGMIN (opcode 4, restricted to
// same-length inputs so it matches opcode 3's two-input shape): for
// 0 <= i < n, value[i] = cost_add(a[i], b[i]); returns the minimum value and
// the index of its first occurrence, matching PCAA's tie-break rule.
struct MinArgmin {
  int32_t value;
  int index;
};

MinArgmin AddReduceMinArgmin(const std::vector<int32_t> &a, const std::vector<int32_t> &b) {
  MinArgmin result{ACCEL_INF, -1};
  for (size_t i = 0; i < a.size(); ++i) {
    const int32_t value = accel_cost_add(a[i], b[i]);
    if (result.index < 0 || value < result.value) {
      result.value = value;
      result.index = static_cast<int>(i);
    }
  }
  return result;
}

}  // namespace

ShortestPathResult BellmanFord(int vertex_count, const std::vector<Edge> &edges, int source) {
  ShortestPathResult result;
  result.distance.assign(vertex_count > 0 ? static_cast<size_t>(vertex_count) : 0, ACCEL_INF);
  result.predecessor.assign(vertex_count > 0 ? static_cast<size_t>(vertex_count) : 0, -1);
  result.has_negative_cycle = false;
  result.distance_saturated = false;
  if (vertex_count <= 0 || source < 0 || source >= vertex_count) {
    return result;
  }
  result.distance[static_cast<size_t>(source)] = 0;

  // Group edges by destination once, so each relaxation round below is
  // literally one MAP_ADD_REDUCE_MIN_ARGMIN-shaped call per vertex over its
  // incoming edges, not a per-edge scalar update.
  std::vector<std::vector<int>> incoming(static_cast<size_t>(vertex_count));
  for (size_t index = 0; index < edges.size(); ++index) {
    incoming[static_cast<size_t>(edges[index].to)].push_back(static_cast<int>(index));
  }

  for (int round = 0; round < vertex_count - 1; ++round) {
    bool changed = false;
    for (int v = 0; v < vertex_count; ++v) {
      const std::vector<int> &in_edges = incoming[static_cast<size_t>(v)];
      if (in_edges.empty()) {
        continue;
      }
      std::vector<int32_t> predecessor_distance;
      std::vector<int32_t> edge_weight;
      predecessor_distance.reserve(in_edges.size());
      edge_weight.reserve(in_edges.size());
      for (int edge_index : in_edges) {
        const Edge &edge = edges[static_cast<size_t>(edge_index)];
        predecessor_distance.push_back(result.distance[static_cast<size_t>(edge.from)]);
        edge_weight.push_back(edge.weight);
      }
      const MinArgmin relaxed = AddReduceMinArgmin(predecessor_distance, edge_weight);
      if (relaxed.value < result.distance[static_cast<size_t>(v)]) {
        result.distance[static_cast<size_t>(v)] = relaxed.value;
        result.predecessor[static_cast<size_t>(v)] =
            edges[static_cast<size_t>(in_edges[static_cast<size_t>(relaxed.index)])].from;
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
  }

  // Exact shadow of the same relaxation in 64 bits. The int32 check above
  // cannot see a reachable negative cycle whose vertices already sit at the
  // saturated INT32_MIN floor: cost_add keeps them there, so nothing appears
  // to relax further. Edges with ACCEL_INF weight never relax, as in cost_add.
  constexpr int64_t kUnreached = std::numeric_limits<int64_t>::max();
  std::vector<int64_t> exact(static_cast<size_t>(vertex_count), kUnreached);
  exact[static_cast<size_t>(source)] = 0;
  const auto relax_exact = [&]() {
    bool relaxed = false;
    for (const Edge &edge : edges) {
      const int64_t from = exact[static_cast<size_t>(edge.from)];
      if (from == kUnreached || edge.weight == ACCEL_INF) {
        continue;
      }
      const int64_t candidate = from + edge.weight;
      if (candidate < exact[static_cast<size_t>(edge.to)]) {
        exact[static_cast<size_t>(edge.to)] = candidate;
        relaxed = true;
      }
    }
    return relaxed;
  };
  for (int round = 0; round < vertex_count - 1; ++round) {
    if (!relax_exact()) {
      break;
    }
  }
  result.has_negative_cycle = relax_exact();
  result.distance_saturated = false;
  if (!result.has_negative_cycle) {
    for (int64_t value : exact) {
      if (value != kUnreached && value < std::numeric_limits<int32_t>::min()) {
        result.distance_saturated = true;
      }
    }
  }
  return result;
}

}  // namespace pcaa::probes
