// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Parses and constructs benchmark graphs with the host runner's capacity policy.
#include "pbqp_input.h"
#include "accel_protocol.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <exception>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
namespace pcaa::tools {
namespace {
constexpr size_t kMaximumHostDomain = 64 * 1024;
bool parse_costs(std::istringstream &line, size_t count, std::vector<int32_t> &costs) {
  costs.clear();
  costs.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    std::string token;
    if (!(line >> token)) {
      return false;
    }
    try {
      if (token == "INF") {
        costs.push_back(ACCEL_INF);
        continue;
      }
      size_t parsed = 0;
      const long long value = std::stoll(token, &parsed, 10);
      if (parsed != token.size() || value < std::numeric_limits<int32_t>::min() ||
          value > std::numeric_limits<int32_t>::max()) {
        return false;
      }
      costs.push_back(static_cast<int32_t>(value));
    } catch (const std::exception &) {
      return false;
    }
  }
  std::string extra;
  return !(line >> extra);
}

bool is_valid_input_cost(int32_t cost) {
  return cost <= ACCEL_INF;
}

}  // namespace

bool load_problem(const std::filesystem::path &path, InputProblem &problem) {
  std::ifstream input(path);
  if (!input) {
    return false;
  }
  bool saw_nodes = false;
  size_t declared_nodes = 0;
  std::string text;
  while (std::getline(input, text)) {
    const size_t comment = text.find('#');
    std::istringstream line(text.substr(0, comment));
    std::string kind;
    if (!(line >> kind)) {
      continue;
    }
    if (kind == "nodes") {
      size_t count = 0;
      std::string extra;
      if (saw_nodes || !(line >> count) || line >> extra || count == 0 || !problem.nodes.empty()) {
        return false;
      }
      saw_nodes = true;
      declared_nodes = count;
    } else if (kind == "node") {
      size_t domain = 0;
      if (!saw_nodes || problem.nodes.size() == declared_nodes || !(line >> domain) ||
          domain == 0 || domain > kMaximumHostDomain) {
        return false;
      }
      std::vector<int32_t> costs;
      if (!parse_costs(line, domain, costs) ||
          !std::all_of(costs.begin(), costs.end(), is_valid_input_cost)) {
        return false;
      }
      problem.nodes.push_back({std::move(costs)});
    } else if (kind == "edge") {
      size_t first = 0;
      size_t second = 0;
      if (!saw_nodes || !(line >> first >> second) || first >= problem.nodes.size() ||
          second >= problem.nodes.size() || first == second) {
        return false;
      }
      const size_t count = problem.nodes[first].unary.size() * problem.nodes[second].unary.size();
      std::vector<int32_t> costs;
      if (!parse_costs(line, count, costs) ||
          !std::all_of(costs.begin(), costs.end(), is_valid_input_cost)) {
        return false;
      }
      problem.edges.push_back({first, second, std::move(costs)});
    } else {
      return false;
    }
  }
  return saw_nodes && problem.nodes.size() == declared_nodes;
}

pbqp_status_t build_problem(const InputProblem &input, SolverMode mode, pbqp_problem_t &problem) {
  const bool fixed_capacity = mode == SolverMode::kBareMetal;
  if (fixed_capacity &&
      (input.nodes.size() > PBQP_MAX_NODES || input.edges.size() > PBQP_MAX_EDGES)) {
    return PBQP_CAPACITY_ERROR;
  }
  if (input.nodes.size() > std::numeric_limits<unsigned>::max() ||
      input.edges.size() >= std::numeric_limits<unsigned>::max()) {
    return PBQP_CAPACITY_ERROR;
  }
  // Bare-metal mode must accept exactly what the RV64 configuration accepts:
  // pbqp_max_finite_cost depends on the capacities, so use the fixed ones.
  const unsigned node_capacity =
      fixed_capacity ? PBQP_MAX_NODES : static_cast<unsigned>(input.nodes.size());
  size_t maximum_domain = 0;
  for (const InputNode &node : input.nodes)
    maximum_domain = std::max(maximum_domain, node.unary.size());
  if (maximum_domain > std::numeric_limits<unsigned>::max())
    return PBQP_CAPACITY_ERROR;
  const unsigned domain_capacity =
      fixed_capacity ? PBQP_MAX_DOMAIN : static_cast<unsigned>(maximum_domain);
  // R2 may need one fill slot before it retires its two incident edges; the
  // fixed configuration already reserves every simple edge.
  const unsigned edge_capacity =
      fixed_capacity ? PBQP_MAX_EDGES : static_cast<unsigned>(input.edges.size()) + 1;
  const auto initialized =
      pbqp_init(&problem, pbqp_heap_allocator(), node_capacity, edge_capacity, domain_capacity);
  if (initialized != PBQP_OK)
    return initialized;
  for (const InputNode &node : input.nodes) {
    const auto added =
        pbqp_add_node(&problem, static_cast<unsigned>(node.unary.size()), node.unary.data());
    if (added != PBQP_OK) {
      pbqp_destroy(&problem);
      return added;
    }
  }
  for (const InputEdge &edge : input.edges) {
    if (edge.first >= input.nodes.size() || edge.second >= input.nodes.size() ||
        edge.costs.size() !=
            input.nodes[edge.first].unary.size() * input.nodes[edge.second].unary.size()) {
      pbqp_destroy(&problem);
      return PBQP_ARGUMENT_ERROR;
    }
    const auto added = pbqp_add_edge(&problem, static_cast<unsigned>(edge.first),
                                     static_cast<unsigned>(edge.second), edge.costs.data());
    if (added != PBQP_OK) {
      pbqp_destroy(&problem);
      return added;
    }
  }
  return PBQP_OK;
}

}  // namespace pcaa::tools
