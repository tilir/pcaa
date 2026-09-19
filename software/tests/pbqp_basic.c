// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Runs a reducible PBQP instance through software and accelerator cost kernels.

#include "accel_driver.h"
#include "common.h"
#include "pbqp/pbqp.h"
#include "pbqp/pbqp_accelerator.h"

enum {
  kExpectedR2Reductions = 1,
  kExpectedAdd3ArgminSubmissions = 4,
  kExpectedAddArgminSubmissions = 2,
  kExpectedContiguousViews = 6,
  kExpectedStridedViews = 10,
  kExpectedScratchPacks = 10,
  kExpectedScratchBytes = 80,
};

static pbqp_problem_t software_problem;
static pbqp_problem_t accelerator_problem;
static pbqp_problem_t original_problem;

static int build_problem(pbqp_problem_t *problem) {
  static const int32_t unary0[] = {2, -1};
  static const int32_t unary1[] = {0, 3};
  static const int32_t unary2[] = {1, -2};
  static const int32_t edge01[] = {0, 4, -3, 2};
  static const int32_t edge02[] = {2, -1, 5, 0};
  static const int32_t edge12[] = {1, 3, -2, 4};
  pbqp_init(problem);
  return pbqp_add_node(problem, 2, unary0) || pbqp_add_node(problem, 2, unary1) ||
         pbqp_add_node(problem, 2, unary2) || pbqp_add_edge(problem, 0, 1, edge01) ||
         pbqp_add_edge(problem, 0, 2, edge02) || pbqp_add_edge(problem, 1, 2, edge12);
}

int main(void) {
  pbqp_solution_t oracle;
  pbqp_solution_t software_solution;
  pbqp_solution_t accelerator_solution;
  pbqp_cost_kernel_t software_kernel;
  pbqp_cost_kernel_t accelerator_kernel;
  pbqp_solver_t software_solver;
  pbqp_solver_t accelerator_solver;
  pbqp_accelerator_kernel_context_t accelerator_context;

  if (build_problem(&original_problem) != PBQP_OK ||
      pbqp_bruteforce(&original_problem, &oracle) != PBQP_OK)
    finish(1);
  software_problem = original_problem;
  pbqp_make_software_kernel(&software_kernel, &software_problem.statistics);
  if (pbqp_solver_create(&software_solver, PBQP_MODE_SOFTWARE, &software_kernel) != PBQP_OK ||
      pbqp_solver_solve(&software_solver, &software_problem, &software_solution) != PBQP_OK)
    finish(2);

  accelerator_problem = original_problem;
  accel_init();
  pbqp_make_accelerator_kernel(&accelerator_kernel, &accelerator_context,
                               &accelerator_problem.statistics);
  if (pbqp_solver_create(&accelerator_solver, PBQP_MODE_ACCELERATOR, &accelerator_kernel) !=
          PBQP_OK ||
      pbqp_solver_solve(&accelerator_solver, &accelerator_problem, &accelerator_solution) !=
          PBQP_OK)
    finish(3);

  if (software_solution.optimum != oracle.optimum || accelerator_solution.optimum != oracle.optimum)
    finish(4);
  if (pbqp_evaluate(&original_problem, software_solution.assignment) != oracle.optimum ||
      pbqp_evaluate(&original_problem, accelerator_solution.assignment) != oracle.optimum)
    finish(5);
  const pbqp_statistics_t *statistics = &accelerator_problem.statistics;
  if (statistics->r2_count != kExpectedR2Reductions ||
      statistics->primitive_submissions[ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN] !=
          kExpectedAdd3ArgminSubmissions ||
      statistics->primitive_submissions[ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN] !=
          kExpectedAddArgminSubmissions ||
      statistics->contiguous_views != kExpectedContiguousViews ||
      statistics->strided_views != kExpectedStridedViews ||
      statistics->scratch_packs != kExpectedScratchPacks ||
      statistics->scratch_bytes != kExpectedScratchBytes)
    finish(6);
  finish(0);
}
