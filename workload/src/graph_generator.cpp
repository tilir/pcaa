// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements deterministic host-only PBQP graph-family generation.

#include "pbqp_workload/graph_generator.h"

#include <random>
#include <stdexcept>
#include <utility>

namespace pcaa::workload {
namespace {

void AddEdge(Graph *graph, int first, int second) {
  if (first == second) {
    return;
  }
  graph->edges[first][second] = true;
  graph->edges[second][first] = true;
  graph->edge_first[first][second] = first;
  graph->edge_first[second][first] = first;
}

int DomainFor(DomainProfile profile, std::mt19937 *random) {
  static constexpr int kSmallDomains[] = {2, 3, 4, 5, 6};
  static constexpr int kRegisterDomains[] = {8, 11, 16, 17, 31, 32};
  static constexpr int kLargeDomains[] = {4, 6, 7, 8, 16, 31, 32, 33, 64};
  const int *choices = kSmallDomains;
  int choice_count = 5;
  if (profile == DomainProfile::kRegisterLike) {
    choices = kRegisterDomains;
    choice_count = 6;
  } else if (profile == DomainProfile::kLargeStress) {
    choices = kLargeDomains;
    choice_count = 9;
  }
  return choices[(*random)() % choice_count];
}

Graph MakeGraph(const GeneratorConfig &config, std::mt19937 *random) {
  if (config.nodes < 1) {
    throw std::invalid_argument("node count must be positive");
  }
  Graph graph;
  graph.domains.reserve(config.nodes);
  graph.edges.assign(config.nodes, std::vector<bool>(config.nodes, false));
  graph.edge_first.assign(config.nodes, std::vector<int>(config.nodes, -1));
  for (int node = 0; node < config.nodes; ++node) {
    graph.domains.push_back(DomainFor(config.profile, random));
  }
  return graph;
}

}  // namespace

Graph GenerateGraph(const GeneratorConfig &config) {
  std::mt19937 random(config.seed);
  Graph graph = MakeGraph(config, &random);

  switch (config.family) {
    case GraphFamily::kTree:
      for (int node = 1; node < config.nodes; ++node) {
        AddEdge(&graph, node, static_cast<int>(random() % node));
      }
      break;
    case GraphFamily::kPath:
      for (int node = 1; node < config.nodes; ++node) {
        AddEdge(&graph, node - 1, node);
      }
      break;
    case GraphFamily::kStar:
      for (int node = 1; node < config.nodes; ++node) {
        AddEdge(&graph, 0, node);
      }
      break;
    case GraphFamily::kCycle:
      if (config.nodes > 2) {
        for (int node = 1; node < config.nodes; ++node) {
          AddEdge(&graph, node - 1, node);
        }
        AddEdge(&graph, config.nodes - 1, 0);
      }
      break;
    case GraphFamily::kTwoTree:
      if (config.nodes > 1) {
        AddEdge(&graph, 0, 1);
      }
      for (int node = 2; node < config.nodes; ++node) {
        std::vector<std::pair<int, int>> edges;
        for (int first = 0; first < node; ++first) {
          for (int second = first + 1; second < node; ++second) {
            if (graph.edges[first][second]) {
              edges.emplace_back(first, second);
            }
          }
        }
        const auto [first, second] = edges[random() % edges.size()];
        AddEdge(&graph, node, first);
        AddEdge(&graph, node, second);
      }
      break;
    case GraphFamily::kIrreducibleCore:
      for (int first = 0; first < config.nodes; ++first) {
        for (int second = first + 1; second < config.nodes; ++second) {
          AddEdge(&graph, first, second);
        }
      }
      break;
  }
  return graph;
}

std::string ToString(GraphFamily family) {
  switch (family) {
    case GraphFamily::kTree:
      return "tree";
    case GraphFamily::kPath:
      return "path";
    case GraphFamily::kStar:
      return "star";
    case GraphFamily::kCycle:
      return "cycle";
    case GraphFamily::kTwoTree:
      return "two_tree";
    case GraphFamily::kIrreducibleCore:
      return "irreducible_core";
  }
  return "unknown";
}

std::string ToString(DomainProfile profile) {
  switch (profile) {
    case DomainProfile::kUniformSmall:
      return "uniform_small";
    case DomainProfile::kRegisterLike:
      return "register_allocation_like";
    case DomainProfile::kLargeStress:
      return "large_domain_stress";
  }
  return "unknown";
}

}  // namespace pcaa::workload
