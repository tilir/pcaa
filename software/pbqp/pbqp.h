// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Defines the allocator-backed PBQP solver and its cost-kernel boundary.

#pragma once

#include "accel_protocol.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  /* Default RV64 arena capacities; the allocator-backed solver itself is not limited to these. */
  PBQP_MAX_NODES = 64,
  PBQP_MAX_DOMAIN = 6,
  PBQP_MAX_EDGES = PBQP_MAX_NODES * (PBQP_MAX_NODES - 1) / 2,
  /* Safe finite-cost range for the default RV64 capacities. */
  PBQP_MAX_FINITE_COST = (ACCEL_INF - 1) / (PBQP_MAX_NODES + PBQP_MAX_EDGES),
  PBQP_MIN_FINITE_COST = -PBQP_MAX_FINITE_COST,
};

typedef enum {
  PBQP_OK = 0,
  PBQP_CAPACITY_ERROR = -1,
  PBQP_ARGUMENT_ERROR = -2,
  PBQP_COST_RANGE_ERROR = -3,
  PBQP_IRREDUCIBLE = -4,
  PBQP_SEARCH_LIMIT = -5,
  /* Internal to exact-branch-reduce: this branch's lower bound already meets
     or exceeds the caller's incumbent, so it was not fully explored. Never
     returned by pbqp_solver_solve(); consumed by the caller that issued the
     branch. */
  PBQP_PRUNED = -6,
} pbqp_status_t;

/*
 * Allocation boundary shared by hosted and freestanding callers. Allocations
 * must be suitably aligned for any C type. The allocator remains caller-owned
 * and must outlive every problem and solver workspace that refers to it.
 */
typedef struct {
  void *context;
  void *(*allocate)(void *context, size_t size);
  void (*deallocate)(void *context, void *allocation, size_t size);
} pbqp_allocator_t;

/* LIFO arena over caller-owned bytes; nested allocations must be released in reverse order. */
typedef struct {
  unsigned char *storage;
  size_t capacity;
  size_t used;
} pbqp_arena_t;

typedef enum {
  PBQP_MODE_SOFTWARE,
  PBQP_MODE_ACCELERATOR,
} pbqp_mode_t;

/* Named graph-solving algorithms sharing the same cost-kernel interface. */
typedef enum {
  PBQP_STRATEGY_REDUCE_ONLY,
  PBQP_STRATEGY_HEURISTIC_RN,
  PBQP_STRATEGY_EXACT_CORE_ENUMERATION,
  PBQP_STRATEGY_EXACT_BRANCH_REDUCE,
  PBQP_STRATEGY_LOCAL_SEARCH,
  PBQP_STRATEGY_HEURISTIC_RN_LOCAL_SEARCH,
} pbqp_solver_strategy_t;

/* Deterministic selection rules for an irreducible node handled by RN. */
typedef enum {
  PBQP_RN_MIN_DEGREE,
  PBQP_RN_MAX_DEGREE,
  PBQP_RN_MIN_WORK,
} pbqp_rn_policy_t;

/* Vector RN/R2 path versus retained scalar jobs; per-edge controls RN batch scope. */
typedef enum {
  PBQP_RN_BATCH_PER_NODE,
  PBQP_RN_BATCH_PER_EDGE,
} pbqp_rn_batching_t;

/* Stable solver-event categories for external workload characterization. */
typedef enum {
  PBQP_TRACE_R0,
  PBQP_TRACE_R1,
  PBQP_TRACE_R2,
  PBQP_TRACE_RN_SELECT,
  PBQP_TRACE_RN_SCORE,
  PBQP_TRACE_RN_COMMIT,
  PBQP_TRACE_LOCAL_SCORE,
  PBQP_TRACE_LOCAL_MOVE,
  PBQP_TRACE_BRANCH_SELECT,
  PBQP_TRACE_BRANCH_CONDITION,
} pbqp_trace_event_type_t;

typedef enum {
  PBQP_TRACE_REDUCTION,
  PBQP_TRACE_HEURISTIC,
  PBQP_TRACE_LOCAL_SEARCH,
  PBQP_TRACE_EXACT_SEARCH,
} pbqp_trace_phase_t;

/* One solver action and its directly associated logical cost-algebra work. */
typedef struct {
  pbqp_trace_event_type_t type;
  pbqp_trace_phase_t phase;
  pbqp_rn_policy_t policy;
  int node;
  int choice;
  unsigned active_nodes;
  unsigned active_edges;
  unsigned maximum_degree;
  unsigned unary_elements;
  unsigned matrix_elements;
  unsigned r0_count;
  unsigned r1_count;
  unsigned r2_count;
  unsigned rn_count;
  uint64_t minplus_project_elements;
  uint64_t project_accumulate_elements;
  uint64_t slice_accumulate_elements;
  uint64_t map3_reduce_elements;
  uint64_t argmin_vector_elements;
  unsigned primitive_descriptors;
  unsigned structural_operations;
  /* Non-zero only for BRANCH_SELECT; absent meaning in older JSONL consumers is zero. */
  unsigned branch_domain;
  uint64_t operand_bytes;
  uint64_t result_bytes;
} pbqp_solver_event_t;

typedef struct {
  void *context;
  void (*emit)(void *context, const pbqp_solver_event_t *event);
} pbqp_trace_sink_t;

typedef struct {
  pbqp_solver_strategy_t strategy;
  pbqp_rn_policy_t rn_policy;
  pbqp_rn_batching_t rn_batching;
  unsigned maximum_search_nodes;
  const pbqp_trace_sink_t *trace_sink;
  /* Optional snapshot/scratch allocator; an empty value uses the problem allocator. */
  pbqp_allocator_t workspace_allocator;
} pbqp_solver_config_t;

/* A logical cost vector; stride is measured in int32_t elements. */
typedef struct {
  const int32_t *base;
  size_t length;
  size_t stride;
} pbqp_vector_view_t;

/* Matrix element (row, column) is base[row * row_stride + column * column_stride]. */
typedef struct {
  const int32_t *base;
  size_t rows;
  size_t columns;
  size_t row_stride;
  size_t column_stride;
} pbqp_matrix_view_t;

/* One ordered MINPLUS_PROJECT -> COST_ADD_VECTOR chain. */
typedef struct {
  pbqp_matrix_view_t matrix;
  pbqp_vector_view_t unary;
  int32_t *temporary;
  int32_t *scores;
} pbqp_project_add_job_t;

/* One fixed-first-neighbor R2 slice, producing one vector of value/argmin pairs. */
typedef struct {
  pbqp_vector_view_t unary;
  pbqp_vector_view_t fixed_edge;
  pbqp_matrix_view_t varying_edge;
  accel_min_argmin_result_t *results;
} pbqp_map3_project_job_t;

/* One already-scheduled two-input primitive reduction and its caller-owned output. */
typedef struct {
  pbqp_vector_view_t a;
  pbqp_vector_view_t b;
  accel_min_argmin_result_t *result;
} pbqp_min2_job_t;

/* One value-only two-input primitive reduction for callers that do not reconstruct argmin. */
typedef struct {
  pbqp_vector_view_t a;
  pbqp_vector_view_t b;
  int32_t *result;
} pbqp_min2_value_job_t;

/* One already-scheduled three-input primitive reduction and its caller-owned output. */
typedef struct {
  pbqp_vector_view_t a;
  pbqp_vector_view_t b;
  pbqp_vector_view_t c;
  accel_min_argmin_result_t *result;
} pbqp_min3_job_t;

/* Workload counters collected by a solve, including explicit scratch packing. */
typedef struct {
  unsigned nodes;
  unsigned initial_edges;
  unsigned r0_count;
  unsigned r1_count;
  unsigned r2_count;
  unsigned rn_count;
  unsigned rn_degree_min;
  unsigned rn_degree_max;
  uint64_t rn_degree_total;
  unsigned *rn_degree_histogram;
  unsigned rn_projection_count;
  unsigned rn_projection_primitives;
  uint64_t rn_projection_map_elements;
  uint64_t rn_projection_operand_bytes;
  uint64_t rn_projection_result_bytes;
  uint64_t rn_score_accumulation_elements;
  uint64_t rn_commit_elements;
  uint64_t rn_commit_bytes;
  unsigned condition_count;
  uint64_t condition_elements;
  uint64_t condition_matrix_read_bytes;
  uint64_t condition_unary_read_bytes;
  uint64_t condition_unary_write_bytes;
  uint64_t commit_matrix_read_bytes;
  uint64_t commit_unary_read_bytes;
  uint64_t commit_unary_write_bytes;
  unsigned r0_after_rn;
  unsigned r1_after_rn;
  unsigned r2_after_rn;
  unsigned first_rn_active_nodes;
  unsigned first_rn_active_edges;
  unsigned maximum_irreducible_core_nodes;
  unsigned maximum_irreducible_core_edges;
  unsigned rn_episodes;
  unsigned *rn_nodes;
  unsigned *rn_choices;
  unsigned *rn_cascade_r0;
  unsigned *rn_cascade_r1;
  unsigned *rn_cascade_r2;
  unsigned *rn_cascade_length_histogram;
  unsigned rn_cascade_total_length;
  unsigned rn_cascade_maximum_length;
  unsigned local_search_node_evaluations;
  unsigned local_search_sweeps;
  unsigned local_search_accepted_moves;
  uint64_t local_search_slice_accumulations;
  uint64_t local_search_slice_elements;
  uint64_t local_search_matrix_read_bytes;
  uint64_t local_search_score_read_bytes;
  uint64_t local_search_score_write_bytes;
  unsigned local_search_argmin_reductions;
  uint64_t local_search_argmin_elements;
  /* Generic cost-algebra operation mix; bytes are logical operand/result traffic. */
  uint64_t minplus_project_elements;
  unsigned minplus_project_descriptors;
  uint64_t minplus_project_bytes;
  uint64_t project_accumulate_elements;
  uint64_t project_accumulate_bytes;
  uint64_t slice_accumulate_elements;
  unsigned slice_accumulate_operations;
  uint64_t slice_accumulate_bytes;
  uint64_t map3_reduce_elements;
  unsigned map3_reduce_descriptors;
  unsigned scalar_project_descriptors;
  unsigned vector_project_descriptors;
  unsigned vector_add_descriptors;
  unsigned scalar_map3_descriptors;
  unsigned partial_map3_descriptors;
  uint64_t map3_reduce_bytes;
  uint64_t argmin_vector_elements;
  unsigned argmin_vector_descriptors;
  uint64_t argmin_vector_bytes;
  /* Operand and result traffic across the five generic operation categories. */
  uint64_t operation_mix_operand_bytes;
  uint64_t operation_mix_result_bytes;
  uint64_t search_nodes_visited;
  uint64_t search_branches_created;
  unsigned search_maximum_depth;
  unsigned search_limit_hits;
  /* exact-branch-reduce only: branches whose lower bound already met or
     exceeded the incumbent when reached, so they were not explored further. */
  unsigned search_nodes_pruned;
  unsigned primitive_submissions[9];
  unsigned *vector_length_histogram;
  uint64_t logical_map_elements;
  uint64_t logical_bytes_read;
  uint64_t logical_bytes_written;
  unsigned contiguous_views;
  unsigned strided_views;
  unsigned scratch_packs;
  uint64_t scratch_bytes;
  unsigned top_level_submissions;
  unsigned batch_submissions;
  unsigned batch_primitive_descriptors;
  unsigned maximum_batch_size;
  uint64_t batch_descriptor_bytes;
  uint64_t batch_child_descriptor_bytes;
  unsigned unique_packed_views;
} pbqp_statistics_t;

/* Pluggable execution of already-scheduled two- and three-input cost primitives. */
typedef struct {
  void *context;
  int (*min2_argmin)(void *context, pbqp_vector_view_t a, pbqp_vector_view_t b,
                     accel_min_argmin_result_t *result);
  int (*min3_argmin)(void *context, pbqp_vector_view_t a, pbqp_vector_view_t b,
                     pbqp_vector_view_t c, accel_min_argmin_result_t *result);
  int (*min2_argmin_batch)(void *context, const pbqp_min2_job_t *jobs, size_t count);
  int (*min3_argmin_batch)(void *context, const pbqp_min3_job_t *jobs, size_t count);
  int (*min2_value)(void *context, pbqp_vector_view_t a, pbqp_vector_view_t b, int32_t *result);
  int (*min2_value_batch)(void *context, const pbqp_min2_value_job_t *jobs, size_t count);
  int (*cost_add_vector)(void *context, pbqp_vector_view_t a, pbqp_vector_view_t b,
                         int32_t *result);
  int (*minplus_project)(void *context, pbqp_matrix_view_t matrix, pbqp_vector_view_t vector,
                         int32_t *result);
  int (*minplus_map3_project)(void *context, pbqp_vector_view_t unary,
                              pbqp_vector_view_t fixed_edge, pbqp_matrix_view_t varying_edge,
                              accel_min_argmin_result_t *result);
  int (*project_add_batch)(void *context, const pbqp_project_add_job_t *jobs, size_t count);
  int (*map3_project_batch)(void *context, const pbqp_map3_project_job_t *jobs, size_t count);
  /* Redirects backend-only accounting while the solver evaluates a graph snapshot. */
  void (*set_statistics)(void *context, pbqp_statistics_t *statistics);
} pbqp_cost_kernel_t;

/* A configured solver: the mode is descriptive, while kernel supplies its cost primitive. */
typedef struct {
  pbqp_mode_t mode;
  pbqp_cost_kernel_t kernel;
  pbqp_solver_config_t config;
} pbqp_solver_t;

/* Node state; reconstruction choices live in the owning problem's allocated storage. */
typedef struct {
  int active;
  unsigned domain;
  int32_t *unary;
  int first_neighbor;
  int second_neighbor;
  int reduction_kind;
} pbqp_node_t;

/* Fixed-stride row-major binary cost matrix for an unordered pair of node indices. */
typedef struct {
  int active;
  unsigned first;
  unsigned second;
  size_t stride;
  int32_t *cost;
} pbqp_edge_t;

/*
 * Caller-owned PBQP graph handle and mutable reduction workspace. All arrays
 * are carved from one allocator-owned block, so a graph must be copied with
 * pbqp_problem_clone rather than by assigning this structure. This is a
 * process-local owning handle containing native pointers, not a serialized
 * binary format. pbqp_destroy releases its block but never the allocator.
 * Finite unary and edge costs must be within the capacity-dependent symmetric
 * bound returned by pbqp_max_finite_cost; ACCEL_INF is also allowed. This
 * preserves exact PBQP reductions despite the accelerator's saturating
 * arithmetic.
 */
typedef struct {
  pbqp_allocator_t allocator;
  void *storage;
  size_t storage_size;
  unsigned node_capacity;
  unsigned edge_capacity;
  unsigned domain_capacity;
  unsigned node_count;
  unsigned edge_count;
  pbqp_node_t *nodes;
  pbqp_edge_t *edges;
  unsigned *reconstruction;
  int32_t objective_offset;
  unsigned *elimination_order;
  unsigned elimination_count;
  pbqp_statistics_t statistics;
} pbqp_problem_t;

/* Caller-sized output for a minimum objective and one corresponding assignment. */
typedef struct {
  int32_t optimum;
  unsigned *assignment;
  size_t assignment_capacity;
} pbqp_solution_t;

/* Hosted malloc/free allocator; it returns an empty allocator in a freestanding build. */
pbqp_allocator_t pbqp_heap_allocator(void);
void pbqp_arena_init(pbqp_arena_t *arena, void *storage, size_t size);
pbqp_allocator_t pbqp_arena_allocator(pbqp_arena_t *arena);
/* Returns zero when the requested capacity cannot be represented in size_t. */
size_t pbqp_problem_storage_size(unsigned node_capacity, unsigned edge_capacity,
                                 unsigned domain_capacity);
/* Initializes a previously unowned handle and allocates its single state block. */
pbqp_status_t pbqp_init(pbqp_problem_t *problem, pbqp_allocator_t allocator, unsigned node_capacity,
                        unsigned edge_capacity, unsigned domain_capacity);
void pbqp_destroy(pbqp_problem_t *problem);
/* Deep-copies source into a previously unowned destination using allocator. */
pbqp_status_t pbqp_problem_clone(pbqp_problem_t *destination, const pbqp_problem_t *source,
                                 pbqp_allocator_t allocator);
void pbqp_solution_init(pbqp_solution_t *solution, unsigned *assignment,
                        size_t assignment_capacity);
pbqp_status_t pbqp_add_node(pbqp_problem_t *problem, unsigned domain, const int32_t *unary);
pbqp_status_t pbqp_add_edge(pbqp_problem_t *problem, unsigned first, unsigned second,
                            const int32_t *costs);
int32_t pbqp_evaluate(const pbqp_problem_t *problem, const unsigned *assignment);
/* Largest finite input magnitude safe for this problem's reserved capacities. */
int32_t pbqp_max_finite_cost(const pbqp_problem_t *problem);
pbqp_status_t pbqp_bruteforce(const pbqp_problem_t *problem, pbqp_solution_t *solution);
void pbqp_make_software_kernel(pbqp_cost_kernel_t *kernel, pbqp_statistics_t *statistics);
pbqp_solver_config_t pbqp_solver_default_config(void);
pbqp_status_t pbqp_solver_create(pbqp_solver_t *solver, pbqp_mode_t mode,
                                 const pbqp_cost_kernel_t *kernel);
pbqp_status_t pbqp_solver_create_with_config(pbqp_solver_t *solver, pbqp_mode_t mode,
                                             const pbqp_cost_kernel_t *kernel,
                                             const pbqp_solver_config_t *config);
pbqp_status_t pbqp_solver_solve(pbqp_solver_t *solver, pbqp_problem_t *problem,
                                pbqp_solution_t *solution);

#ifdef __cplusplus
}  // extern "C"
#endif
