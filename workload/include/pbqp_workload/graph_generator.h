// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Declares host-only PBQP graph families for workload characterization.

#pragma once

#include <string>
#include <vector>

namespace pcaa::workload {

enum class GraphFamily { kTree, kPath, kStar, kCycle, kTwoTree, kIrreducibleCore };
enum class DomainProfile { kUniformSmall, kRegisterLike, kLargeStress };

struct GeneratorConfig {
  GraphFamily family;
  DomainProfile profile;
  int nodes;
  unsigned seed;
};

struct EdgeSlot {
  int first = -1;
  int second = -1;
  bool active = false;
};

struct Graph {
  std::vector<int> domains;
  std::vector<std::vector<bool>> edges;
  std::vector<std::vector<int>> edge_first;
  std::vector<EdgeSlot> edge_slots;
};

Graph GenerateGraph(const GeneratorConfig &config);
std::string ToString(GraphFamily family);
std::string ToString(DomainProfile profile);

}  // namespace pcaa::workload
