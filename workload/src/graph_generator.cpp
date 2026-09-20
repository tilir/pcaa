// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements deterministic host-only PBQP graph and cost generation.

#include "pbqp_graph_generator/graph_generator.h"

#include <algorithm>
#include <array>
#include <random>
#include <stdexcept>

namespace pcaa::graph_generator {
namespace {

constexpr int kSparseEdgePercent = 20;
constexpr int kCostRange = 5;

int DomainFor(DomainProfile profile, std::mt19937 *random) {
  constexpr std::array<int, 3> kSmallDomains = {2, 3, 4};
  constexpr std::array<int, 3> kRegisterDomains = {4, 5, 6};
  constexpr std::array<int, 4> kLargeDomains = {8, 12, 16, 24};
  if (profile == DomainProfile::kBinary) {
    return 2;
  }
  if (profile == DomainProfile::kSmall) {
    return kSmallDomains[(*random)() % kSmallDomains.size()];
  }
  if (profile == DomainProfile::kRegisterLike) {
    return kRegisterDomains[(*random)() % kRegisterDomains.size()];
  }
  return kLargeDomains[(*random)() % kLargeDomains.size()];
}

void AddEdge(Graph *graph, int first, int second) {
  if (first == second) {
    return;
  }
  if (first > second) {
    std::swap(first, second);
  }
  const auto edge = std::make_pair(first, second);
  if (std::find(graph->edges.begin(), graph->edges.end(), edge) == graph->edges.end()) {
    graph->edges.push_back(edge);
  }
}

int CostFor(std::mt19937 *random) {
  return static_cast<int>((*random)() % (2 * kCostRange + 1)) - kCostRange;
}

}  // namespace

Graph GenerateGraph(const GeneratorConfig &config) {
  if (config.nodes < 1) {
    throw std::invalid_argument("node count must be positive");
  }
  if ((config.family == GraphFamily::kDegree3 || config.family == GraphFamily::kDegree4) &&
      (config.nodes < 6 || config.nodes % 2 != 0)) {
    throw std::invalid_argument(
        "degree-3 and degree-4 families require an even node count of at least 6");
  }

  std::mt19937 random(config.seed);
  Graph graph;
  graph.domains.reserve(config.nodes);
  for (int node = 0; node < config.nodes; ++node) {
    graph.domains.push_back(DomainFor(config.profile, &random));
  }

  switch (config.family) {
    case GraphFamily::kRandomSparse:
    case GraphFamily::kMixedDegree:
      for (int first = 0; first < config.nodes; ++first) {
        for (int second = first + 1; second < config.nodes; ++second) {
          const int probability =
              config.family == GraphFamily::kRandomSparse ? kSparseEdgePercent : 30;
          if (static_cast<int>(random() % 100) < probability) {
            AddEdge(&graph, first, second);
          }
        }
      }
      break;
    case GraphFamily::kDegree3:
      for (int node = 0; node < config.nodes; ++node) {
        AddEdge(&graph, node, (node + 1) % config.nodes);
        AddEdge(&graph, node, (node + config.nodes / 2) % config.nodes);
      }
      break;
    case GraphFamily::kDegree4:
      for (int node = 0; node < config.nodes; ++node) {
        AddEdge(&graph, node, (node + 1) % config.nodes);
        AddEdge(&graph, node, (node + 2) % config.nodes);
      }
      break;
    case GraphFamily::kTree:
      for (int node = 1; node < config.nodes; ++node) {
        AddEdge(&graph, node, static_cast<int>(random() % node));
      }
      break;
    case GraphFamily::kCycle:
      for (int node = 1; node < config.nodes; ++node) {
        AddEdge(&graph, node - 1, node);
      }
      if (config.nodes > 2) {
        AddEdge(&graph, 0, config.nodes - 1);
      }
      break;
    case GraphFamily::kTwoTree:
      if (config.nodes > 1) {
        AddEdge(&graph, 0, 1);
      }
      for (int node = 2; node < config.nodes; ++node) {
        const auto parent = graph.edges[random() % graph.edges.size()];
        AddEdge(&graph, node, parent.first);
        AddEdge(&graph, node, parent.second);
      }
      break;
  }

  graph.unary_costs.reserve(graph.domains.size());
  for (int domain : graph.domains) {
    std::vector<int> costs(domain);
    std::generate(costs.begin(), costs.end(), [&random] { return CostFor(&random); });
    graph.unary_costs.push_back(std::move(costs));
  }
  graph.edge_costs.reserve(graph.edges.size());
  for (const auto &[first, second] : graph.edges) {
    std::vector<int> costs(graph.domains[first] * graph.domains[second]);
    std::generate(costs.begin(), costs.end(), [&random] { return CostFor(&random); });
    graph.edge_costs.push_back(std::move(costs));
  }
  return graph;
}

std::string ToString(GraphFamily family) {
  switch (family) {
    case GraphFamily::kRandomSparse:
      return "random-sparse";
    case GraphFamily::kDegree3:
      return "degree-3";
    case GraphFamily::kDegree4:
      return "degree-4";
    case GraphFamily::kMixedDegree:
      return "mixed-degree";
    case GraphFamily::kTree:
      return "tree";
    case GraphFamily::kCycle:
      return "cycle";
    case GraphFamily::kTwoTree:
      return "two-tree";
  }
  return "unknown";
}

std::string ToString(DomainProfile profile) {
  switch (profile) {
    case DomainProfile::kBinary:
      return "binary";
    case DomainProfile::kSmall:
      return "small";
    case DomainProfile::kRegisterLike:
      return "register-like";
    case DomainProfile::kLarge:
      return "large";
  }
  return "unknown";
}

}  // namespace pcaa::graph_generator
