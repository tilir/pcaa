// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Declares deterministic host-only PBQP graph and cost generation.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace pcaa::graph_generator {

enum class GraphFamily { kRandomSparse, kDegree3, kDegree4, kMixedDegree, kTree, kCycle, kTwoTree };
enum class DomainProfile { kBinary, kSmall, kRegisterLike, kLarge };

struct GeneratorConfig {
  GraphFamily family;
  DomainProfile profile;
  int nodes;
  unsigned seed;
};

struct Graph {
  std::vector<int> domains;
  std::vector<std::pair<int, int>> edges;
  std::vector<std::vector<int>> unary_costs;
  std::vector<std::vector<int>> edge_costs;
};

Graph GenerateGraph(const GeneratorConfig &config);
std::string ToString(GraphFamily family);
std::string ToString(DomainProfile profile);

}  // namespace pcaa::graph_generator
