// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Differentially checks shared heuristic-RN solver semantics on RV64 bare metal.

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
};

static pbqp_problem_t original_problem;
static pbqp_problem_t software_problem;
static pbqp_problem_t accelerator_problem;

static int build_problem(pbqp_problem_t *problem) {
  static const int32_t unary[] = {0, 1};
  static const int32_t edge[] = {0, 1, 2, -1};
  unsigned first;
  unsigned second;

  pbqp_init(problem);
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

  if (build_problem(&original_problem) != 0) {
    finish(kExitBuildFailure);
  }
  accel_init();
  for (policy = PBQP_RN_MIN_DEGREE; policy <= PBQP_RN_MIN_WORK; ++policy) {
    config = pbqp_solver_default_config();
    config.strategy = PBQP_STRATEGY_HEURISTIC_RN;
    config.rn_policy = policy;
    software_problem = original_problem;
    accelerator_problem = original_problem;
    pbqp_make_software_kernel(&software_kernel, &software_problem.statistics);
    pbqp_make_accelerator_kernel(&accelerator_kernel, &accelerator_context,
                                 &accelerator_problem.statistics);
    if (pbqp_solver_create_with_config(&software_solver, PBQP_MODE_SOFTWARE, &software_kernel,
                                       &config) != PBQP_OK ||
        pbqp_solver_create_with_config(&accelerator_solver, PBQP_MODE_ACCELERATOR,
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
  }
  finish(kExitSuccess);
}
