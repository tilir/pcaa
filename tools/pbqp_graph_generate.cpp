// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Emits a deterministic synthetic PBQP graph in the graph runner's text format.

#include "pbqp_graph_generator/graph_generator.h"

#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using pcaa::graph_generator::DomainProfile;
using pcaa::graph_generator::GeneratorConfig;
using pcaa::graph_generator::GraphFamily;

void print_usage(std::ostream &output) {
  output << "Usage: pbqp_graph_generate --family FAMILY --profile PROFILE --nodes N --seed N "
            "[--domain-size D]\n"
            "Families: random-sparse, degree-3, degree-4, mixed-degree, tree, cycle, two-tree\n"
            "Profiles: binary, small, register-like, large\n";
}

bool parse_family(const std::string &text, GraphFamily *family) {
  if (text == "random-sparse")
    *family = GraphFamily::kRandomSparse;
  else if (text == "degree-3")
    *family = GraphFamily::kDegree3;
  else if (text == "degree-4")
    *family = GraphFamily::kDegree4;
  else if (text == "mixed-degree")
    *family = GraphFamily::kMixedDegree;
  else if (text == "tree")
    *family = GraphFamily::kTree;
  else if (text == "cycle")
    *family = GraphFamily::kCycle;
  else if (text == "two-tree")
    *family = GraphFamily::kTwoTree;
  else
    return false;
  return true;
}

bool parse_profile(const std::string &text, DomainProfile *profile) {
  if (text == "binary")
    *profile = DomainProfile::kBinary;
  else if (text == "small")
    *profile = DomainProfile::kSmall;
  else if (text == "register-like")
    *profile = DomainProfile::kRegisterLike;
  else if (text == "large")
    *profile = DomainProfile::kLarge;
  else
    return false;
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  GeneratorConfig config{GraphFamily::kRandomSparse, DomainProfile::kBinary, 0, 0, 0};
  bool saw_family = false;
  bool saw_profile = false;
  bool saw_nodes = false;
  bool saw_seed = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help") {
      print_usage(std::cout);
      return 0;
    }
    if (index + 1 == argc) {
      print_usage(std::cerr);
      return 2;
    }
    const std::string value = argv[++index];
    try {
      if (argument == "--family") {
        saw_family = parse_family(value, &config.family);
      } else if (argument == "--profile") {
        saw_profile = parse_profile(value, &config.profile);
      } else if (argument == "--nodes") {
        config.nodes = std::stoi(value);
        saw_nodes = true;
      } else if (argument == "--seed") {
        config.seed = static_cast<unsigned>(std::stoul(value));
        saw_seed = true;
      } else if (argument == "--domain-size") {
        config.uniform_domain = std::stoi(value);
      } else {
        print_usage(std::cerr);
        return 2;
      }
    } catch (const std::exception &) {
      print_usage(std::cerr);
      return 2;
    }
  }
  if (!saw_family || !saw_profile || !saw_nodes || !saw_seed) {
    print_usage(std::cerr);
    return 2;
  }
  try {
    const auto graph = pcaa::graph_generator::GenerateGraph(config);
    std::cout << "# family=" << pcaa::graph_generator::ToString(config.family)
              << " profile=" << pcaa::graph_generator::ToString(config.profile)
              << " nodes=" << config.nodes << " seed=" << config.seed
              << " domain-size=" << config.uniform_domain << "\n"
              << "nodes " << graph.domains.size() << '\n';
    for (size_t node = 0; node < graph.domains.size(); ++node) {
      std::cout << "node " << graph.domains[node];
      for (int cost : graph.unary_costs[node]) std::cout << ' ' << cost;
      std::cout << '\n';
    }
    for (size_t edge = 0; edge < graph.edges.size(); ++edge) {
      std::cout << "edge " << graph.edges[edge].first << ' ' << graph.edges[edge].second;
      for (int cost : graph.edge_costs[edge]) std::cout << ' ' << cost;
      std::cout << '\n';
    }
  } catch (const std::exception &error) {
    std::cerr << "cannot generate graph: " << error.what() << '\n';
    return 2;
  }
  return 0;
}
