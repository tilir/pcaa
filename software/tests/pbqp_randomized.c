// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Differentially checks seeded small PBQP instances through software and PCAA cost kernels.

#include "accel_driver.h"
#include "common.h"
#include "pbqp/pbqp.h"
#include "pbqp/pbqp_accelerator.h"
#include "pbqp_reference.h"

enum {
  kExitSuccess = 0,
  kExitGraphBuildFailure = 1,
  kExitSoftwareSolveFailure = 2,
  kExitAcceleratorSolveFailure = 3,
  kExitDifferentialMismatch = 4,
  kRounds = 24,
  kMinimumDomain = 2,
  kDomainSpan = 2,
  kRandomCostSpan = 31,
  kRandomCostBias = 15,
  kInfinityPeriodMask = 15,
  kInitialSeed = 0x42b0a55u,
  kProblemStorageBytes = 512 * 1024,
};

static pbqp_problem_t original_problem;
static pbqp_problem_t software_problem;
static pbqp_problem_t accelerator_problem;
static uint64_t original_storage[kProblemStorageBytes / sizeof(uint64_t)];
static uint64_t software_storage[kProblemStorageBytes / sizeof(uint64_t)];
static uint64_t accelerator_storage[kProblemStorageBytes / sizeof(uint64_t)];
static pbqp_arena_t original_arena;
static pbqp_arena_t software_arena;
static pbqp_arena_t accelerator_arena;

static pbqp_allocator_t reset_allocator(pbqp_arena_t *arena, uint64_t *storage) {
  pbqp_arena_init(arena, storage, kProblemStorageBytes);
  return pbqp_arena_allocator(arena);
}

static uint32_t random_state = kInitialSeed;

static uint32_t next_random(void) {
  random_state = random_state * 1664525u + 1013904223u;
  return random_state;
}

static int32_t next_cost(void) {
  if ((next_random() & kInfinityPeriodMask) == 0) {
    return ACCEL_INF;
  }
  return (int32_t)(next_random() % kRandomCostSpan) - kRandomCostBias;
}

static int build_problem(pbqp_problem_t *problem) {
  int32_t unary[3][PBQP_MAX_DOMAIN];
  int32_t edge01[PBQP_MAX_DOMAIN * PBQP_MAX_DOMAIN];
  int32_t edge02[PBQP_MAX_DOMAIN * PBQP_MAX_DOMAIN];
  int32_t edge12[PBQP_MAX_DOMAIN * PBQP_MAX_DOMAIN];
  unsigned domain[3];

  for (unsigned node = 0; node < 3; ++node) {
    domain[node] = kMinimumDomain + next_random() % kDomainSpan;
    for (unsigned value = 0; value < domain[node]; ++value) {
      unary[node][value] = next_cost();
    }
  }
  for (unsigned first = 0; first < domain[0]; ++first) {
    for (unsigned second = 0; second < domain[1]; ++second) {
      edge01[first * domain[1] + second] = next_cost();
    }
  }
  for (unsigned first = 0; first < domain[0]; ++first) {
    for (unsigned second = 0; second < domain[2]; ++second) {
      edge02[first * domain[2] + second] = next_cost();
    }
  }
  for (unsigned first = 0; first < domain[1]; ++first) {
    for (unsigned second = 0; second < domain[2]; ++second) {
      edge12[first * domain[2] + second] = next_cost();
    }
  }

  if (pbqp_init(problem, reset_allocator(&original_arena, original_storage), PBQP_MAX_NODES,
                PBQP_MAX_EDGES, PBQP_MAX_DOMAIN) != PBQP_OK) {
    return PBQP_CAPACITY_ERROR;
  }
  return pbqp_add_node(problem, domain[0], unary[0]) ||
         pbqp_add_node(problem, domain[1], unary[1]) ||
         pbqp_add_node(problem, domain[2], unary[2]) || pbqp_add_edge(problem, 0, 1, edge01) ||
         pbqp_add_edge(problem, 0, 2, edge02) || pbqp_add_edge(problem, 1, 2, edge12);
}

int main(void) {
  accel_init();
  for (int round = 0; round < kRounds; ++round) {
    pbqp_solution_t oracle;
    pbqp_solution_t software_solution;
    pbqp_solution_t accelerator_solution;
    pbqp_cost_kernel_t software_kernel;
    pbqp_cost_kernel_t accelerator_kernel;
    pbqp_solver_t software_solver;
    pbqp_solver_t accelerator_solver;
    pbqp_accelerator_kernel_context_t accelerator_context;
    unsigned oracle_assignment[PBQP_MAX_NODES];
    unsigned software_assignment[PBQP_MAX_NODES];
    unsigned accelerator_assignment[PBQP_MAX_NODES];

    pbqp_solution_init(&oracle, oracle_assignment, PBQP_MAX_NODES);
    pbqp_solution_init(&software_solution, software_assignment, PBQP_MAX_NODES);
    pbqp_solution_init(&accelerator_solution, accelerator_assignment, PBQP_MAX_NODES);

    if (build_problem(&original_problem) != PBQP_OK) {
      finish(kExitGraphBuildFailure);
    }
    pbqp_reference_bruteforce(&original_problem, &oracle);
    if (pbqp_problem_clone(&software_problem, &original_problem,
                           reset_allocator(&software_arena, software_storage)) != PBQP_OK) {
      finish(kExitGraphBuildFailure);
    }
    pbqp_make_software_kernel(&software_kernel, &software_problem.statistics);
    if (pbqp_solver_create(&software_solver, PBQP_MODE_SOFTWARE, &software_kernel) != PBQP_OK ||
        pbqp_solver_solve(&software_solver, &software_problem, &software_solution) != PBQP_OK) {
      finish(kExitSoftwareSolveFailure);
    }
    if (pbqp_problem_clone(&accelerator_problem, &original_problem,
                           reset_allocator(&accelerator_arena, accelerator_storage)) != PBQP_OK) {
      finish(kExitGraphBuildFailure);
    }
    pbqp_make_accelerator_kernel(&accelerator_kernel, &accelerator_context,
                                 &accelerator_problem.statistics);
    if (pbqp_solver_create(&accelerator_solver, PBQP_MODE_ACCELERATOR, &accelerator_kernel) !=
            PBQP_OK ||
        pbqp_solver_solve(&accelerator_solver, &accelerator_problem, &accelerator_solution) !=
            PBQP_OK) {
      finish(kExitAcceleratorSolveFailure);
    }
    if (software_solution.optimum != oracle.optimum ||
        accelerator_solution.optimum != oracle.optimum ||
        pbqp_reference_evaluate(&original_problem, software_solution.assignment) !=
            oracle.optimum ||
        pbqp_reference_evaluate(&original_problem, accelerator_solution.assignment) !=
            oracle.optimum) {
      finish(kExitDifferentialMismatch);
    }
  }
  finish(kExitSuccess);
}
