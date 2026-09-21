// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Differentially checks every shared PBQP solver strategy on RV64 bare metal.

#include "accel_driver.h"
#include "common.h"
#include "pbqp/pbqp.h"
#include "pbqp/pbqp_accelerator.h"
#include "pbqp_reference.h"

enum {
  kExitSuccess = 0,
  kExitBuildFailure = 1,
  kExitSolverFailure = 2,
  kExitMismatch = 3,
  kProblemStorageBytes = 512 * 1024,
  kWorkspaceStorageBytes = 4 * 1024 * 1024,
};

static pbqp_problem_t original_problem;
static pbqp_problem_t software_problem;
static pbqp_problem_t accelerator_problem;
static uint64_t original_storage[kProblemStorageBytes / sizeof(uint64_t)];
static uint64_t software_storage[kProblemStorageBytes / sizeof(uint64_t)];
static uint64_t accelerator_storage[kProblemStorageBytes / sizeof(uint64_t)];
static uint64_t solver_workspace[kWorkspaceStorageBytes / sizeof(uint64_t)];
static pbqp_arena_t original_arena;
static pbqp_arena_t software_arena;
static pbqp_arena_t accelerator_arena;
static pbqp_arena_t solver_workspace_arena;

static pbqp_allocator_t reset_allocator(pbqp_arena_t *arena, uint64_t *storage, size_t size) {
  pbqp_arena_init(arena, storage, size);
  return pbqp_arena_allocator(arena);
}

enum { kInvalidPhysicalAddress = 0x40000000 };

static int build_problem(pbqp_problem_t *problem) {
  static const int32_t unary[] = {0, 1};
  static const int32_t edge[] = {0, 1, 2, -1};
  unsigned first;
  unsigned second;

  if (pbqp_init(problem, reset_allocator(&original_arena, original_storage, kProblemStorageBytes),
                PBQP_MAX_NODES, PBQP_MAX_EDGES, PBQP_MAX_DOMAIN) != PBQP_OK) {
    return -1;
  }
  for (first = 0; first < 4; ++first) {
    if (pbqp_add_node(problem, 2, unary) != PBQP_OK) {
      return -1;
    }
  }
  for (first = 0; first < 4; ++first) {
    for (second = first + 1; second < 4; ++second) {
      if (pbqp_add_edge(problem, first, second, edge) != PBQP_OK) {
        return -1;
      }
    }
  }
  return 0;
}

static int equal_trace(const pbqp_problem_t *software, const pbqp_problem_t *accelerator) {
  unsigned index;
  if (software->statistics.r0_count != accelerator->statistics.r0_count ||
      software->statistics.r1_count != accelerator->statistics.r1_count ||
      software->statistics.r2_count != accelerator->statistics.r2_count ||
      software->statistics.rn_count != accelerator->statistics.rn_count ||
      software->statistics.condition_elements != accelerator->statistics.condition_elements ||
      software->statistics.local_search_node_evaluations !=
          accelerator->statistics.local_search_node_evaluations ||
      software->statistics.search_nodes_visited != accelerator->statistics.search_nodes_visited ||
      software->statistics.search_branches_created !=
          accelerator->statistics.search_branches_created ||
      software->statistics.rn_projection_primitives !=
          accelerator->statistics.rn_projection_primitives) {
    return 0;
  }
  for (index = 0; index < software->statistics.rn_count; ++index) {
    if (software->statistics.rn_nodes[index] != accelerator->statistics.rn_nodes[index] ||
        software->statistics.rn_choices[index] != accelerator->statistics.rn_choices[index]) {
      return 0;
    }
  }
  return 1;
}

int main(void) {
  pbqp_cost_kernel_t software_kernel;
  pbqp_cost_kernel_t accelerator_kernel;
  pbqp_accelerator_kernel_context_t accelerator_context;
  pbqp_solver_t software_solver;
  pbqp_solver_t accelerator_solver;
  pbqp_solution_t software_solution;
  pbqp_solution_t accelerator_solution;
  pbqp_solver_config_t config;
  pbqp_rn_policy_t policy;
  int32_t checked_result = 0;
  const int32_t zero = 0;
  static const pbqp_solver_strategy_t strategies[] = {
      PBQP_STRATEGY_HEURISTIC_RN,
      PBQP_STRATEGY_EXACT_CORE_ENUMERATION,
      PBQP_STRATEGY_EXACT_BRANCH_REDUCE,
      PBQP_STRATEGY_LOCAL_SEARCH,
      PBQP_STRATEGY_HEURISTIC_RN_LOCAL_SEARCH,
  };
  unsigned strategy_index;
  unsigned software_assignment[PBQP_MAX_NODES];
  unsigned accelerator_assignment[PBQP_MAX_NODES];

  pbqp_solution_init(&software_solution, software_assignment, PBQP_MAX_NODES);
  pbqp_solution_init(&accelerator_solution, accelerator_assignment, PBQP_MAX_NODES);

  if (build_problem(&original_problem) != 0) {
    finish(kExitBuildFailure);
  }
  accel_init();
  if (accel_min_add_checked((const int32_t *)(uintptr_t)kInvalidPhysicalAddress, &zero, 1,
                            &checked_result) == 0) {
    finish(kExitMismatch);
  }
  for (strategy_index = 0; strategy_index < sizeof(strategies) / sizeof(strategies[0]);
       ++strategy_index) {
    for (policy = PBQP_RN_MIN_DEGREE; policy <= PBQP_RN_MIN_WORK; ++policy) {
      config = pbqp_solver_default_config();
      config.strategy = strategies[strategy_index];
      config.rn_policy = policy;
      if (pbqp_problem_clone(&software_problem, &original_problem,
                             reset_allocator(&software_arena, software_storage,
                                             kProblemStorageBytes)) != PBQP_OK ||
          pbqp_problem_clone(&accelerator_problem, &original_problem,
                             reset_allocator(&accelerator_arena, accelerator_storage,
                                             kProblemStorageBytes)) != PBQP_OK) {
        finish(kExitBuildFailure);
      }
      pbqp_make_software_kernel(&software_kernel, &software_problem.statistics);
      pbqp_make_accelerator_kernel(&accelerator_kernel, &accelerator_context,
                                   &accelerator_problem.statistics);
      config.workspace_allocator =
          reset_allocator(&solver_workspace_arena, solver_workspace, kWorkspaceStorageBytes);
      if (pbqp_solver_create_with_config(&software_solver, PBQP_MODE_SOFTWARE, &software_kernel,
                                         &config) != PBQP_OK)
        finish(kExitSolverFailure);
      if (pbqp_solver_create_with_config(&accelerator_solver, PBQP_MODE_ACCELERATOR,
                                         &accelerator_kernel, &config) != PBQP_OK ||
          pbqp_solver_solve(&software_solver, &software_problem, &software_solution) != PBQP_OK ||
          pbqp_solver_solve(&accelerator_solver, &accelerator_problem, &accelerator_solution) !=
              PBQP_OK) {
        finish(kExitSolverFailure);
      }
      if (software_solution.optimum != accelerator_solution.optimum ||
          pbqp_reference_evaluate(&original_problem, software_solution.assignment) !=
              software_solution.optimum ||
          pbqp_reference_evaluate(&original_problem, accelerator_solution.assignment) !=
              accelerator_solution.optimum ||
          !equal_trace(&software_problem, &accelerator_problem)) {
        finish(kExitMismatch);
      }
      if (config.strategy == PBQP_STRATEGY_EXACT_BRANCH_REDUCE &&
          (accelerator_problem.statistics.batch_submissions == 0 ||
           accelerator_problem.statistics.top_level_submissions == 0)) {
        finish(kExitMismatch);
      }
    }
  }
  finish(kExitSuccess);
}
