// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Checks PBQP reductions against exhaustive enumeration without accelerator dependencies.

#include "accel_protocol.h"
#include "pbqp/pbqp.h"

#include <stdint.h>
#include <string>

#include <gtest/gtest.h>

#define CHECK(expression) EXPECT_TRUE(expression)

static void build_triangle(pbqp_problem_t* problem) {
  const int32_t unary0[] = {2, -1};
  const int32_t unary1[] = {0, 3};
  const int32_t unary2[] = {1, -2};
  const int32_t edge01[] = {0, 4, -3, 2};
  const int32_t edge02[] = {2, -1, 5, 0};
  const int32_t edge12[] = {1, 3, -2, 4};
  pbqp_init(problem);
  CHECK(pbqp_add_node(problem, 2, unary0) == PBQP_OK);
  CHECK(pbqp_add_node(problem, 2, unary1) == PBQP_OK);
  CHECK(pbqp_add_node(problem, 2, unary2) == PBQP_OK);
  CHECK(pbqp_add_edge(problem, 0, 1, edge01) == PBQP_OK);
  CHECK(pbqp_add_edge(problem, 0, 2, edge02) == PBQP_OK);
  CHECK(pbqp_add_edge(problem, 1, 2, edge12) == PBQP_OK);
}

static void build_open_wedge(pbqp_problem_t* problem) {
  const int32_t unary[] = {0, 1};
  const int32_t edge[] = {0, 2, -1, 3};
  pbqp_init(problem);
  CHECK(pbqp_add_node(problem, 2, unary) == PBQP_OK);
  CHECK(pbqp_add_node(problem, 2, unary) == PBQP_OK);
  CHECK(pbqp_add_node(problem, 2, unary) == PBQP_OK);
  CHECK(pbqp_add_edge(problem, 0, 1, edge) == PBQP_OK);
  CHECK(pbqp_add_edge(problem, 0, 2, edge) == PBQP_OK);
}

static void build_irreducible_core(pbqp_problem_t* problem) {
  const int32_t unary[] = {0, 1};
  const int32_t edge[] = {0, 1, 2, -1};
  pbqp_init(problem);
  for (unsigned node = 0; node < 4; ++node) {
    CHECK(pbqp_add_node(problem, 2, unary) == PBQP_OK);
  }
  for (unsigned first = 0; first < 4; ++first) {
    for (unsigned second = first + 1; second < 4; ++second) {
      CHECK(pbqp_add_edge(problem, first, second, edge) == PBQP_OK);
    }
  }
}

static void check_cost_range(void) {
  pbqp_problem_t problem;
  const int32_t out_of_range[] = {ACCEL_INF - 1};
  const int32_t valid[] = {0};
  pbqp_init(&problem);
  CHECK(pbqp_add_node(&problem, 1, out_of_range) == PBQP_COST_RANGE_ERROR);
  CHECK(pbqp_add_node(&problem, 1, valid) == PBQP_OK);
  CHECK(pbqp_add_node(&problem, 1, valid) == PBQP_OK);
  CHECK(pbqp_add_edge(&problem, 0, 1, out_of_range) == PBQP_COST_RANGE_ERROR);
}

static void check_all_infinite_core(void) {
  const int32_t infinite[] = {ACCEL_INF};
  const int32_t zero[] = {0};
  pbqp_problem_t original;
  pbqp_problem_t reduced;
  pbqp_solution_t oracle = {0, {1000000}};
  pbqp_solution_t solution = {0, {1000000}};
  pbqp_cost_kernel_t kernel;
  pbqp_solver_t solver;

  pbqp_init(&original);
  for (unsigned node = 0; node < 4; ++node) {
    CHECK(pbqp_add_node(&original, 1, infinite) == PBQP_OK);
  }
  for (unsigned first = 0; first < 4; ++first) {
    for (unsigned second = first + 1; second < 4; ++second) {
      CHECK(pbqp_add_edge(&original, first, second, zero) == PBQP_OK);
    }
  }
  CHECK(pbqp_bruteforce(&original, &oracle) == PBQP_OK);
  reduced = original;
  pbqp_make_software_kernel(&kernel, &reduced.statistics);
  CHECK(pbqp_solver_create(&solver, PBQP_MODE_SOFTWARE, &kernel) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &reduced, &solution) == PBQP_OK);
  CHECK(oracle.optimum == ACCEL_INF && solution.optimum == ACCEL_INF);
  for (unsigned node = 0; node < 4; ++node) {
    CHECK(oracle.assignment[node] == 0 && solution.assignment[node] == 0);
  }
}

static void check_problem(void (*build)(pbqp_problem_t*), int expect_r2) {
  pbqp_problem_t original;
  pbqp_problem_t reduced;
  pbqp_solution_t oracle;
  pbqp_solution_t solution;
  pbqp_cost_kernel_t kernel;
  pbqp_solver_t solver;

  build(&original);
  CHECK(pbqp_bruteforce(&original, &oracle) == PBQP_OK);
  reduced = original;
  pbqp_make_software_kernel(&kernel, &reduced.statistics);
  CHECK(pbqp_solver_create(&solver, PBQP_MODE_SOFTWARE, &kernel) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &reduced, &solution) == PBQP_OK);
  CHECK(solution.optimum == oracle.optimum);
  CHECK(pbqp_evaluate(&original, solution.assignment) == oracle.optimum);
  CHECK(reduced.statistics.r2_count == (unsigned)expect_r2);
  if (expect_r2 != 0) {
    CHECK(reduced.statistics.r0_count != 0);
  }
}

TEST(PbqpSolver, ReductionsAndReconstruction) {
  pbqp_problem_t original;
  pbqp_problem_t reduced;
  pbqp_solution_t oracle;
  pbqp_solution_t solution;
  pbqp_cost_kernel_t kernel;
  pbqp_solver_t solver;

  build_triangle(&original);
  CHECK(pbqp_bruteforce(&original, &oracle) == PBQP_OK);
  reduced = original;
  pbqp_make_software_kernel(&kernel, &reduced.statistics);
  CHECK(pbqp_solver_create(&solver, PBQP_MODE_SOFTWARE, &kernel) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &reduced, &solution) == PBQP_OK);
  CHECK(solution.optimum == oracle.optimum);
  CHECK(pbqp_evaluate(&original, solution.assignment) == oracle.optimum);
  CHECK(reduced.statistics.r2_count == 1);
  CHECK(reduced.statistics.r1_count != 0);
  CHECK(reduced.statistics.r0_count != 0);
  CHECK(reduced.statistics.primitive_submissions[ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN] != 0);
  check_problem(build_open_wedge, 1);
  check_problem(build_irreducible_core, 0);
  check_cost_range();
  check_all_infinite_core();
}

TEST(PbqpSolver, ReduceOnlyAndHeuristicRn) {
  pbqp_problem_t original;
  pbqp_problem_t reduced;
  pbqp_solution_t solution;
  pbqp_cost_kernel_t kernel;
  pbqp_solver_t solver;
  pbqp_solver_config_t config = pbqp_solver_default_config();

  build_irreducible_core(&original);
  reduced = original;
  pbqp_make_software_kernel(&kernel, &reduced.statistics);
  config.strategy = PBQP_STRATEGY_REDUCE_ONLY;
  CHECK(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &kernel, &config) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &reduced, &solution) == PBQP_IRREDUCIBLE);

  for (pbqp_rn_policy_t policy = PBQP_RN_MIN_DEGREE; policy <= PBQP_RN_MIN_WORK;
       policy = static_cast<pbqp_rn_policy_t>(policy + 1)) {
    reduced = original;
    pbqp_make_software_kernel(&kernel, &reduced.statistics);
    config.strategy = PBQP_STRATEGY_HEURISTIC_RN;
    config.rn_policy = policy;
    CHECK(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &kernel, &config) == PBQP_OK);
    CHECK(pbqp_solver_solve(&solver, &reduced, &solution) == PBQP_OK);
    CHECK(pbqp_evaluate(&original, solution.assignment) == solution.optimum);
    CHECK(reduced.statistics.rn_count != 0);
    CHECK(reduced.statistics.rn_projection_count != 0);
    CHECK(reduced.statistics.rn_projection_primitives != 0);
    CHECK(reduced.statistics.rn_commit_elements != 0);
    CHECK(reduced.statistics.primitive_submissions[ACCEL_OPCODE_MAP_ADD_REDUCE_MIN] != 0);
    CHECK(reduced.statistics.rn_commit_bytes == 3 * reduced.statistics.rn_commit_elements *
                                                   sizeof(int32_t));
  }
}

TEST(PbqpSolver, ExactBranchReduceReappliesReductions) {
  pbqp_problem_t original;
  pbqp_problem_t enumerated;
  pbqp_problem_t branched;
  pbqp_solution_t enumeration_solution;
  pbqp_solution_t branch_solution;
  pbqp_cost_kernel_t kernel;
  pbqp_solver_t solver;
  pbqp_solver_config_t config = pbqp_solver_default_config();

  build_irreducible_core(&original);
  enumerated = original;
  pbqp_make_software_kernel(&kernel, &enumerated.statistics);
  config.strategy = PBQP_STRATEGY_EXACT_CORE_ENUMERATION;
  CHECK(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &kernel, &config) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &enumerated, &enumeration_solution) == PBQP_OK);

  branched = original;
  pbqp_make_software_kernel(&kernel, &branched.statistics);
  config.strategy = PBQP_STRATEGY_EXACT_BRANCH_REDUCE;
  CHECK(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &kernel, &config) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &branched, &branch_solution) == PBQP_OK);
  CHECK(branch_solution.optimum == enumeration_solution.optimum);
  CHECK(pbqp_evaluate(&original, branch_solution.assignment) == branch_solution.optimum);
  CHECK(branched.statistics.search_nodes_visited > 1);
  CHECK(branched.statistics.search_branches_created != 0);
  CHECK(branched.statistics.condition_count != 0);
  CHECK(branched.statistics.condition_elements != 0);
  CHECK(branched.statistics.rn_count == 0);
}

TEST(PbqpSolver, LocalSearchHybridNeverWorsensRn) {
  pbqp_problem_t original;
  pbqp_problem_t rn_problem;
  pbqp_problem_t hybrid_problem;
  pbqp_solution_t rn_solution;
  pbqp_solution_t hybrid_solution;
  pbqp_cost_kernel_t kernel;
  pbqp_solver_t solver;
  pbqp_solver_config_t config = pbqp_solver_default_config();

  build_irreducible_core(&original);
  rn_problem = original;
  pbqp_make_software_kernel(&kernel, &rn_problem.statistics);
  config.strategy = PBQP_STRATEGY_HEURISTIC_RN;
  CHECK(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &kernel, &config) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &rn_problem, &rn_solution) == PBQP_OK);

  hybrid_problem = original;
  pbqp_make_software_kernel(&kernel, &hybrid_problem.statistics);
  config.strategy = PBQP_STRATEGY_HEURISTIC_RN_LOCAL_SEARCH;
  CHECK(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &kernel, &config) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &hybrid_problem, &hybrid_solution) == PBQP_OK);
  CHECK(hybrid_solution.optimum <= rn_solution.optimum);
  CHECK(pbqp_evaluate(&original, hybrid_solution.assignment) == hybrid_solution.optimum);
  CHECK(hybrid_problem.statistics.local_search_node_evaluations != 0);
}

TEST(PbqpSolver, ExactSearchLimit) {
  pbqp_problem_t problem;
  pbqp_solution_t solution;
  pbqp_cost_kernel_t kernel;
  pbqp_solver_t solver;
  pbqp_solver_config_t config = pbqp_solver_default_config();

  build_irreducible_core(&problem);
  config.maximum_search_nodes = 1;
  pbqp_make_software_kernel(&kernel, &problem.statistics);
  CHECK(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &kernel, &config) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &problem, &solution) == PBQP_SEARCH_LIMIT);
  CHECK(problem.statistics.search_limit_hits == 1);

  build_irreducible_core(&problem);
  config.strategy = PBQP_STRATEGY_EXACT_BRANCH_REDUCE;
  pbqp_make_software_kernel(&kernel, &problem.statistics);
  CHECK(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &kernel, &config) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &problem, &solution) == PBQP_SEARCH_LIMIT);
  CHECK(problem.statistics.search_limit_hits == 1);
  CHECK(problem.statistics.search_nodes_visited == 1);
}
