// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Runs a reducible PBQP instance through software and accelerator cost kernels.

#include "accel_driver.h"
#include "common.h"
#include "pbqp/pbqp.h"
#include "pbqp/pbqp_accelerator.h"
#include "pbqp_reference.h"

enum {
  kExitSuccess = 0,
  kExitGraphOrOracleFailure = 1,
  kExitSoftwareSolveFailure = 2,
  kExitAcceleratorCallbackErrorNotReported = 3,
  kExitAcceleratorSolveFailure = 4,
  kExitOptimumMismatch = 5,
  kExitAssignmentMismatch = 6,
  kExitWorkloadStatisticsMismatch = 7,
  kExpectedR2Reductions = 1,
  kExpectedAdd3ArgminSubmissions = 4,
  kExpectedAddArgminSubmissions = 2,
  kExpectedContiguousViews = 6,
  kExpectedStridedViews = 10,
  kExpectedScratchPacks = 6,
  kExpectedScratchBytes = 48,
  kExpectedTopLevelSubmissions = 2,
  kExpectedBatchSubmissions = 2,
  kExpectedBatchPrimitiveDescriptors = 6,
  kExpectedMaximumBatchSize = 4,
  kExpectedBatchDescriptorBytes = 448,
  kExpectedBatchChildDescriptorBytes = 336,
  kExpectedUniquePackedViews = 6,
  kInvalidGuestAddress = 0x40000000UL,
  kProblemStorageBytes = 512 * 1024,
};

static pbqp_problem_t software_problem;
static pbqp_problem_t accelerator_problem;
static pbqp_problem_t original_problem;
static uint64_t software_storage[kProblemStorageBytes / sizeof(uint64_t)];
static uint64_t accelerator_storage[kProblemStorageBytes / sizeof(uint64_t)];
static uint64_t original_storage[kProblemStorageBytes / sizeof(uint64_t)];
static pbqp_arena_t software_arena;
static pbqp_arena_t accelerator_arena;
static pbqp_arena_t original_arena;

static pbqp_allocator_t reset_allocator(pbqp_arena_t *arena, uint64_t *storage) {
  pbqp_arena_init(arena, storage, kProblemStorageBytes);
  return pbqp_arena_allocator(arena);
}

static int build_problem(pbqp_problem_t *problem) {
  static const int32_t unary0[] = {2, -1};
  static const int32_t unary1[] = {0, 3};
  static const int32_t unary2[] = {1, -2};
  static const int32_t edge01[] = {0, 4, -3, 2};
  static const int32_t edge02[] = {2, -1, 5, 0};
  static const int32_t edge12[] = {1, 3, -2, 4};
  if (pbqp_init(problem, reset_allocator(&original_arena, original_storage), PBQP_MAX_NODES,
                PBQP_MAX_EDGES, PBQP_MAX_DOMAIN) != PBQP_OK) {
    return PBQP_CAPACITY_ERROR;
  }
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
  unsigned oracle_assignment[PBQP_MAX_NODES];
  unsigned software_assignment[PBQP_MAX_NODES];
  unsigned accelerator_assignment[PBQP_MAX_NODES];

  pbqp_solution_init(&oracle, oracle_assignment, PBQP_MAX_NODES);
  pbqp_solution_init(&software_solution, software_assignment, PBQP_MAX_NODES);
  pbqp_solution_init(&accelerator_solution, accelerator_assignment, PBQP_MAX_NODES);

  if (build_problem(&original_problem) != PBQP_OK) {
    finish(kExitGraphOrOracleFailure);
  }
  pbqp_reference_bruteforce(&original_problem, &oracle);
  if (pbqp_problem_clone(&software_problem, &original_problem,
                         reset_allocator(&software_arena, software_storage)) != PBQP_OK)
    finish(kExitGraphOrOracleFailure);
  pbqp_make_software_kernel(&software_kernel, &software_problem.statistics);
  if (pbqp_solver_create(&software_solver, PBQP_MODE_SOFTWARE, &software_kernel) != PBQP_OK ||
      pbqp_solver_solve(&software_solver, &software_problem, &software_solution) != PBQP_OK)
    finish(kExitSoftwareSolveFailure);

  if (pbqp_problem_clone(&accelerator_problem, &original_problem,
                         reset_allocator(&accelerator_arena, accelerator_storage)) != PBQP_OK)
    finish(kExitGraphOrOracleFailure);
  accel_init();
  pbqp_make_accelerator_kernel(&accelerator_kernel, &accelerator_context,
                               &accelerator_problem.statistics);
  const pbqp_vector_view_t invalid_view = {(const int32_t *)kInvalidGuestAddress, 1, 1};
  const pbqp_vector_view_t valid_view = {original_problem.nodes[0].unary, 1, 1};
  accel_min_argmin_result_t failed_result;
  if (accelerator_kernel.min2_argmin(accelerator_kernel.context, invalid_view, valid_view,
                                     &failed_result) == 0 ||
      accelerator_kernel.min3_argmin(accelerator_kernel.context, valid_view, invalid_view,
                                     valid_view, &failed_result) == 0) {
    finish(kExitAcceleratorCallbackErrorNotReported);
  }
  if (pbqp_solver_create(&accelerator_solver, PBQP_MODE_ACCELERATOR, &accelerator_kernel) !=
          PBQP_OK ||
      pbqp_solver_solve(&accelerator_solver, &accelerator_problem, &accelerator_solution) !=
          PBQP_OK)
    finish(kExitAcceleratorSolveFailure);

  if (software_solution.optimum != oracle.optimum || accelerator_solution.optimum != oracle.optimum)
    finish(kExitOptimumMismatch);
  if (pbqp_reference_evaluate(&original_problem, software_solution.assignment) != oracle.optimum ||
      pbqp_reference_evaluate(&original_problem, accelerator_solution.assignment) != oracle.optimum)
    finish(kExitAssignmentMismatch);
  const pbqp_statistics_t *statistics = &accelerator_problem.statistics;
  if (statistics->r2_count != kExpectedR2Reductions ||
      statistics->primitive_submissions[ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN] !=
          kExpectedAdd3ArgminSubmissions ||
      statistics->primitive_submissions[ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN] !=
          kExpectedAddArgminSubmissions ||
      statistics->contiguous_views != kExpectedContiguousViews ||
      statistics->strided_views != kExpectedStridedViews ||
      statistics->scratch_packs != kExpectedScratchPacks ||
      statistics->scratch_bytes != kExpectedScratchBytes ||
      statistics->top_level_submissions != kExpectedTopLevelSubmissions ||
      statistics->batch_submissions != kExpectedBatchSubmissions ||
      statistics->batch_primitive_descriptors != kExpectedBatchPrimitiveDescriptors ||
      statistics->maximum_batch_size != kExpectedMaximumBatchSize ||
      statistics->batch_descriptor_bytes != kExpectedBatchDescriptorBytes ||
      statistics->batch_child_descriptor_bytes != kExpectedBatchChildDescriptorBytes ||
      statistics->unique_packed_views != kExpectedUniquePackedViews)
    finish(kExitWorkloadStatisticsMismatch);
  finish(kExitSuccess);
}
