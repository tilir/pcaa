// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Correctness tests for the Bellman-Ford generality probe against
// hand-checked shortest-path instances.

#include "bellman_ford.h"

#include "accel_protocol.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace pcaa::probes {
namespace {

// 0 -> 1 -> 2 -> 3, plus a longer 0 -> 1 -> 3 direct edge and a negative
// edge on the shorter path, so the shortest 0->3 path is 0-1-2-3 (cost 2),
// not the direct 0-1-3 edge (cost 5), and exercises a negative weight
// without a negative cycle.
TEST(BellmanFordTest, ShortestPathPrefersNegativeWeightedDetour) {
  const std::vector<Edge> edges = {
      {0, 1, 1},
      {1, 2, -3},
      {2, 3, 4},
      {1, 3, 4},
  };
  const ShortestPathResult result = BellmanFord(4, edges, 0);
  EXPECT_FALSE(result.has_negative_cycle);
  EXPECT_EQ(result.distance[0], 0);
  EXPECT_EQ(result.distance[1], 1);
  EXPECT_EQ(result.distance[2], -2);
  EXPECT_EQ(result.distance[3], 2);
  EXPECT_EQ(result.predecessor[3], 2);
  EXPECT_EQ(result.predecessor[2], 1);
  EXPECT_EQ(result.predecessor[1], 0);
}

TEST(BellmanFordTest, UnreachableVertexStaysAtInf) {
  const std::vector<Edge> edges = {{0, 1, 5}};
  const ShortestPathResult result = BellmanFord(3, edges, 0);
  EXPECT_EQ(result.distance[0], 0);
  EXPECT_EQ(result.distance[1], 5);
  EXPECT_EQ(result.distance[2], ACCEL_INF);
  EXPECT_EQ(result.predecessor[2], -1);
}

TEST(BellmanFordTest, DetectsReachableNegativeCycle) {
  const std::vector<Edge> edges = {
      {0, 1, 1},
      {1, 2, -1},
      {2, 1, -1},
  };
  const ShortestPathResult result = BellmanFord(3, edges, 0);
  EXPECT_TRUE(result.has_negative_cycle);
}

// Oracle regression: once 1 and 2 saturate at INT32_MIN, its cost addition keeps
// them there, so an int32-only check sees no further relaxation even though
// the reachable cycle 1->2->1 has weight -2.
TEST(BellmanFordTest, DetectsNegativeCycleAfterSaturation) {
  const std::vector<Edge> edges = {
      {0, 1, std::numeric_limits<int32_t>::min()},
      {1, 2, -1},
      {2, 1, -1},
  };
  const ShortestPathResult result = BellmanFord(3, edges, 0);
  EXPECT_TRUE(result.has_negative_cycle);
}

TEST(BellmanFordTest, ReportsSaturatedDistanceWithoutCycle) {
  const std::vector<Edge> edges = {
      {0, 1, std::numeric_limits<int32_t>::min()},
      {1, 2, -1},
  };
  const ShortestPathResult result = BellmanFord(3, edges, 0);
  EXPECT_FALSE(result.has_negative_cycle);
  EXPECT_TRUE(result.distance_saturated);
  EXPECT_EQ(result.distance[2], std::numeric_limits<int32_t>::min());
}

TEST(BellmanFordTest, OrdinaryDistancesAreNotSaturated) {
  const std::vector<Edge> edges = {{0, 1, -5}, {1, 2, 3}};
  const ShortestPathResult result = BellmanFord(3, edges, 0);
  EXPECT_FALSE(result.has_negative_cycle);
  EXPECT_FALSE(result.distance_saturated);
  EXPECT_EQ(result.distance[2], -2);
}

TEST(BellmanFordTest, AcceptsFixedPointRoadDistancesFromPublishedCorpus) {
  // Millimetre-scaled great-circle distances derived from every 100,000th
  // arc of the DIMACS 9 USA-road-d.NY graph and its published coordinates.
  // The sampling rule and scale are documented in cost-representation-study.md.
  const std::vector<Edge> edges = {
      {0, 1, 80384},  {1, 2, 115347}, {2, 3, 19472}, {3, 4, 181062},
      {4, 5, 100910}, {5, 6, 65003},  {6, 7, 90256}, {7, 8, 87238},
  };
  const ShortestPathResult result = BellmanFord(9, edges, 0);
  EXPECT_FALSE(result.has_negative_cycle);
  EXPECT_FALSE(result.distance_saturated);
  EXPECT_EQ(result.distance[8], 739672);
}

TEST(BellmanFordTest, TieBreaksToFirstMinimalIncomingEdge) {
  // 0->1 (2) and 0->3 (1) both settle before vertex 3 is relaxed in the
  // same round (vertices 1 and 2 are visited first), so 1->3 (1) and
  // 2->3 (2) present a genuine tie (both cost 3) to the same argmin call.
  // PCAA's argmin selects the first equal minimum; 1->3 is listed first.
  const std::vector<Edge> edges = {
      {0, 1, 2},
      {0, 2, 1},
      {1, 3, 1},
      {2, 3, 2},
  };
  const ShortestPathResult result = BellmanFord(4, edges, 0);
  EXPECT_EQ(result.distance[3], 3);
  EXPECT_EQ(result.predecessor[3], 1);
}

TEST(BellmanFordTest, SourceOutOfRangeReturnsAllUnreached) {
  const std::vector<Edge> edges = {{0, 1, 1}};
  const ShortestPathResult result = BellmanFord(2, edges, 5);
  EXPECT_EQ(result.distance[0], ACCEL_INF);
  EXPECT_EQ(result.distance[1], ACCEL_INF);
}

}  // namespace
}  // namespace pcaa::probes
