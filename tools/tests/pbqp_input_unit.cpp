// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Tests the shared graph reader, capacity policy, ownership and typed failures.
#include "pbqp_input.h"
#include "accel_protocol.h"
#include "pbqp/pbqp.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <gtest/gtest.h>

namespace pcaa::tools {
namespace {
class InputFile {
 public:
  explicit InputFile(std::string_view text) {
    static int sequence = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ = std::filesystem::temp_directory_path() /
                 ("pcaa-input-unit-" + std::to_string(stamp) + "-" + std::to_string(sequence++));
    if (!std::filesystem::create_directory(directory_))
      throw std::runtime_error("temporary directory");
    path_ = directory_ / "graph with spaces.pbqp";
    std::ofstream output(path_);
    output << text;
    if (!output)
      throw std::runtime_error("temporary input");
  }
  ~InputFile() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }
  const std::filesystem::path &Path() const {
    return path_;
  }

 private:
  std::filesystem::path directory_, path_;
};
TEST(GraphInput, ReadsCommentsInfRectanglesAndReversedOrientation) {
  const InputFile file(
      "# comment\nnodes 2\n\nnode 2 -7 0 # unary\nnode 3 INF 2 4\nedge 1 0 1 2 3 4 5 INF\n");
  InputProblem input;
  ASSERT_TRUE(load_problem(file.Path(), input));
  ASSERT_EQ(input.nodes.size(), 2u);
  EXPECT_EQ(input.nodes[0].unary, (std::vector<int32_t>{-7, 0}));
  EXPECT_EQ(input.nodes[1].unary[0], ACCEL_INF);
  ASSERT_EQ(input.edges.size(), 1u);
  EXPECT_EQ(input.edges[0].first, 1u);
  ProblemOwner graph;
  ASSERT_EQ(build_problem(input, SolverMode::kLocal, graph.value), PBQP_OK);
  EXPECT_EQ(graph.value.node_capacity, 2u);
  EXPECT_EQ(graph.value.edge_capacity, 2u);
  EXPECT_EQ(graph.value.domain_capacity, 3u);
  EXPECT_EQ(graph.value.edges[0].cost[2 * graph.value.edges[0].stride + 1], ACCEL_INF);
}
TEST(GraphInput, RejectsMalformedSyntaxAndOutOfRangeNumbers) {
  const std::array<std::string_view, 18> invalid{"",
                                                 "nodes 0\n",
                                                 "nodes -1\n",
                                                 "nodes 1 extra\n",
                                                 "node 1 0\n",
                                                 "nodes 2\nnode 1 0\n",
                                                 "nodes 1\nnodes 1\nnode 1 0\n",
                                                 "nodes 1\nnode 0\n",
                                                 "nodes 1\nnode 65537 0\n",
                                                 "nodes 1\nnode 1 1.5\n",
                                                 "nodes 1\nnode 1 2147483648\n",
                                                 "nodes 1\nnode 1 -2147483649\n",
                                                 "nodes 1\nnode 1 536870912\n",
                                                 "nodes 1\nnode 1 INF extra\n",
                                                 "nodes 1\nnode 1 NaN\n",
                                                 "nodes 1\nnode 1 0\nedge 0 0 0\n",
                                                 "nodes 2\nedge 0 1 0\n",
                                                 "nodes 1\nnode 1 0\nunknown\n"};
  for (auto text : invalid) {
    const InputFile file(text);
    InputProblem input;
    EXPECT_FALSE(load_problem(file.Path(), input)) << text;
  }
}
TEST(GraphInput, ReportsMissingFileAndTruncatedOrExtraMatrix) {
  InputProblem missing;
  EXPECT_FALSE(load_problem(std::filesystem::path("/nonexistent/pcaa/input.pbqp"), missing));
  for (auto text : {"nodes 2\nnode 1 0\nnode 2 0 1\nedge 0 1 5\n",
                    "nodes 2\nnode 1 0\nnode 2 0 1\nedge 0 1 5 6 7\n"}) {
    const InputFile file(text);
    InputProblem input;
    EXPECT_FALSE(load_problem(file.Path(), input));
  }
}
TEST(GraphInput, FixedCapacitiesAndTypedCostRangeFailure) {
  InputProblem input{{{{0, 1}}, {{0, 2}}}, {{0, 1, {0, 0, 0, 0}}}};
  ProblemOwner local, fixed;
  ASSERT_EQ(build_problem(input, SolverMode::kLocal, local.value), PBQP_OK);
  ASSERT_EQ(build_problem(input, SolverMode::kBareMetal, fixed.value), PBQP_OK);
  EXPECT_EQ(fixed.value.node_capacity, static_cast<unsigned>(PBQP_MAX_NODES));
  EXPECT_EQ(fixed.value.edge_capacity, static_cast<unsigned>(PBQP_MAX_EDGES));
  EXPECT_EQ(fixed.value.domain_capacity, static_cast<unsigned>(PBQP_MAX_DOMAIN));
  EXPECT_EQ(pbqp_max_finite_cost(&fixed.value), PBQP_MAX_FINITE_COST);
  input.nodes[0].unary[0] = PBQP_MAX_FINITE_COST + 1;
  ProblemOwner rejected, accepted;
  EXPECT_EQ(build_problem(input, SolverMode::kBareMetal, rejected.value), PBQP_COST_RANGE_ERROR);
  EXPECT_EQ(rejected.value.storage, nullptr);
  EXPECT_EQ(build_problem(input, SolverMode::kLocal, accepted.value), PBQP_OK);
}
TEST(GraphInput, DomainAndDuplicateEdgeFailuresRemainTyped) {
  InputProblem wide;
  wide.nodes.push_back({std::vector<int32_t>(PBQP_MAX_DOMAIN + 1, 0)});
  ProblemOwner fixed, local;
  EXPECT_EQ(build_problem(wide, SolverMode::kBareMetal, fixed.value), PBQP_ARGUMENT_ERROR);
  ASSERT_EQ(build_problem(wide, SolverMode::kLocal, local.value), PBQP_OK);
  InputProblem duplicate{{{{0}}, {{0}}}, {{0, 1, {0}}, {1, 0, {0}}}};
  ProblemOwner rejected;
  EXPECT_EQ(build_problem(duplicate, SolverMode::kLocal, rejected.value), PBQP_ARGUMENT_ERROR);
  EXPECT_EQ(rejected.value.storage, nullptr);
}
TEST(GraphInput, MoveTransfersSingleOwnedBlock) {
  InputProblem input{{{{1, 2}}}, {}};
  ProblemOwner first;
  ASSERT_EQ(build_problem(input, SolverMode::kLocal, first.value), PBQP_OK);
  void *storage = first.value.storage;
  ProblemOwner second(std::move(first));
  EXPECT_EQ(first.value.storage, nullptr);
  EXPECT_EQ(second.value.storage, storage);
  ProblemOwner third;
  third = std::move(second);
  EXPECT_EQ(second.value.storage, nullptr);
  EXPECT_EQ(third.value.nodes[0].unary[1], 2);
}
}  // namespace
}  // namespace pcaa::tools
