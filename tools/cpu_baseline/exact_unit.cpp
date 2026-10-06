// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Checks exact execution policies against exhaustive search and snapshot invariants.
#include "kernels.h"
#include "profile.h"
#include "pbqp_input.h"
#include "pbqp/pbqp_storage.h"
#include <array>
#include <random>
#include <vector>
#include <gtest/gtest.h>
using namespace cpu_baseline;
using namespace pcaa::tools;
namespace {
constexpr int kCases = 40;
constexpr unsigned kNodes = 6, kDomain = 3, kEdges = kNodes * (kNodes - 1) / 2;
struct ProfileOwner {
  Profile value;
  ProfileOwner() {
    profile = &value;
  }
  ~ProfileOwner() {
    profile = nullptr;
    active_kernels = nullptr;
  }
};
TEST(ExactPolicies, RandomNegativeRectangularGraphsMatchExhaustiveOracle) {
  std::mt19937 random(6007);
  for (int test = 0; test < kCases; ++test) {
    ProblemOwner original;
    ASSERT_EQ(pbqp_init(&original.value, pbqp_heap_allocator(), kNodes, kEdges + 1, kDomain),
              PBQP_OK);
    std::array<int32_t, kDomain> unary{};
    std::array<unsigned, kNodes> domains{};
    for (unsigned n = 0; n < kNodes; ++n) {
      domains[n] = 2 + random() % 2;
      for (auto &v : unary) v = int(random() % 11) - 5;
      ASSERT_EQ(pbqp_add_node(&original.value, domains[n], unary.data()), PBQP_OK);
    }
    std::vector<int32_t> matrix;
    for (unsigned a = 0; a < kNodes; ++a)
      for (unsigned b = a + 1; b < kNodes; ++b) {
        if (random() % 4 == 0)
          continue;
        matrix.resize(domains[a] * domains[b]);
        for (auto &v : matrix) v = random() % 9 == 0 ? ACCEL_INF : int(random() % 13) - 6;
        ASSERT_EQ(pbqp_add_edge(&original.value, a, b, matrix.data()), PBQP_OK);
      }
    std::array<unsigned, kNodes> oracle_assignment{};
    pbqp_solution_t oracle;
    pbqp_solution_init(&oracle, oracle_assignment.data(), oracle_assignment.size());
    ASSERT_EQ(pbqp_bruteforce(&original.value, &oracle), PBQP_OK);
    for (Level level : {Level::Scalar, Level::Dense, Level::Structured})
      for (bool fast : {false, true})
        for (bool seeded : {false, true}) {
          ProfileOwner profile_owner;
          auto &p = profile_owner.value;
          p.degrees = fast;
          p.fast_clone = fast;
          p.vector_conditioning = fast;
          p.counters = true;
          p.solve_start = Now();
          if (seeded) {
            p.seed_assignment = oracle_assignment.data();
            p.seed_length = kNodes;
            p.seed_objective = oracle.optimum;
          }
          Kernels kernels(level);
          active_kernels = &kernels;
          pbqp_cost_kernel_t api{};
          kernels.Make(api);
          pbqp_solver_t solver;
          auto config = pbqp_solver_default_config();
          config.strategy = PBQP_STRATEGY_EXACT_BRANCH_REDUCE;
          config.maximum_search_nodes = 10000;
          ASSERT_EQ(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &api, &config),
                    PBQP_OK);
          ProblemOwner problem;
          ASSERT_EQ(pbqp_problem_clone(&problem.value, &original.value, pbqp_heap_allocator()),
                    PBQP_OK);
          std::array<unsigned, kNodes> assignment{};
          pbqp_solution_t solution;
          pbqp_solution_init(&solution, assignment.data(), assignment.size());
          const auto execution = MakeExecution(p);
          ASSERT_EQ(pcaa::pbqp::SolveWithExecution(solver, problem.value, solution, execution),
                    PBQP_OK);
          EXPECT_EQ(solution.optimum, oracle.optimum);
          EXPECT_EQ(pbqp_evaluate(&original.value, assignment.data()), oracle.optimum);
          EXPECT_TRUE(p.frames.empty());
        }
  }
}
TEST(ExactPolicies, FastCloneRebindsOwnedStorageAndStatistics) {
  ProblemOwner source, copy;
  ASSERT_EQ(pbqp_init(&source.value, pbqp_heap_allocator(), kNodes, kEdges + 1, kDomain), PBQP_OK);
  std::array<int32_t, kDomain> unary{-2, 0, 1};
  ASSERT_EQ(pbqp_add_node(&source.value, kDomain, unary.data()), PBQP_OK);
  source.value.statistics.search_nodes_pruned = 7;
  ASSERT_EQ(pcaa::pbqp_storage::Clone(copy.value, source.value, pbqp_heap_allocator(), true),
            PBQP_OK);
  EXPECT_NE(copy.value.nodes[0].unary, source.value.nodes[0].unary);
  EXPECT_NE(copy.value.statistics.rn_nodes, source.value.statistics.rn_nodes);
  EXPECT_EQ(copy.value.statistics.search_nodes_pruned, 7u);
  copy.value.nodes[0].unary[0] = 22;
  EXPECT_EQ(source.value.nodes[0].unary[0], -2);
}
TEST(ExactPolicies, RejectsInvalidIncumbentAndPropagatesConditioningFailure) {
  InputProblem input;
  ASSERT_TRUE(load_problem("examples/petersen.pbqp", input));
  ProblemOwner original;
  ASSERT_EQ(build_problem(input, SolverMode::kLocal, original.value), PBQP_OK);
  ProfileOwner profile_owner;
  auto &p = profile_owner.value;
  Kernels kernels(Level::Dense);
  active_kernels = &kernels;
  pbqp_cost_kernel_t api{};
  kernels.Make(api);
  api.cost_add_vector = [](void *, pbqp_vector_view_t, pbqp_vector_view_t, int32_t *) {
    return -777;
  };
  pbqp_solver_t solver;
  auto config = pbqp_solver_default_config();
  config.strategy = PBQP_STRATEGY_EXACT_BRANCH_REDUCE;
  ASSERT_EQ(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &api, &config), PBQP_OK);
  std::vector<unsigned> assignment(input.nodes.size());
  pbqp_solution_t solution;
  pbqp_solution_init(&solution, assignment.data(), assignment.size());
  auto execution = MakeExecution(p);
  execution.seed_assignment = assignment.data();
  execution.seed_length = 0;
  EXPECT_EQ(pcaa::pbqp::SolveWithExecution(solver, original.value, solution, execution),
            PBQP_ARGUMENT_ERROR);
  execution.seed_assignment = nullptr;
  execution.vector_conditioning = true;
  EXPECT_EQ(pcaa::pbqp::SolveWithExecution(solver, original.value, solution, execution),
            PBQP_KERNEL_ERROR);
  EXPECT_EQ(solver.last_kernel_status, -777);
}
}  // namespace
