// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Defines a small fixed-capacity PBQP solver and its cost-kernel boundary.

#pragma once

#include "accel_protocol.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  PBQP_MAX_NODES = 64,
  PBQP_MAX_DOMAIN = 6,
  PBQP_MAX_EDGES = PBQP_MAX_NODES * (PBQP_MAX_NODES - 1) / 2,
  PBQP_VECTOR_HISTOGRAM_BINS = PBQP_MAX_DOMAIN + 1,
  /* Keeps every finite sum of one complete fixed-capacity graph below ACCEL_INF. */
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
} pbqp_status_t;

typedef enum {
  PBQP_MODE_SOFTWARE,
  PBQP_MODE_ACCELERATOR,
} pbqp_mode_t;

/* Named graph-solving algorithms sharing the same cost-kernel interface. */
typedef enum {
  PBQP_STRATEGY_REDUCE_ONLY,
  PBQP_STRATEGY_HEURISTIC_RN,
  PBQP_STRATEGY_EXACT_BRANCH_REDUCE,
  PBQP_STRATEGY_LOCAL_SEARCH,
} pbqp_solver_strategy_t;

/* Deterministic selection rules for an irreducible node handled by RN. */
typedef enum {
  PBQP_RN_MIN_DEGREE,
  PBQP_RN_MAX_DEGREE,
  PBQP_RN_MIN_WORK,
} pbqp_rn_policy_t;

typedef struct {
  pbqp_solver_strategy_t strategy;
  pbqp_rn_policy_t rn_policy;
  unsigned maximum_search_nodes;
} pbqp_solver_config_t;

/* A logical cost vector; stride is measured in int32_t elements. */
typedef struct {
  const int32_t *base;
  size_t length;
  size_t stride;
} pbqp_vector_view_t;

/* One already-scheduled two-input primitive reduction and its caller-owned output. */
typedef struct {
  pbqp_vector_view_t a;
  pbqp_vector_view_t b;
  accel_min_argmin_result_t *result;
} pbqp_min2_job_t;

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
  unsigned rn_degree_histogram[PBQP_MAX_NODES + 1];
  unsigned rn_projection_count;
  unsigned rn_projection_primitives;
  uint64_t rn_projection_map_elements;
  uint64_t rn_projection_operand_bytes;
  uint64_t rn_projection_result_bytes;
  uint64_t rn_score_accumulation_elements;
  uint64_t rn_commit_elements;
  uint64_t rn_commit_bytes;
  unsigned r0_after_rn;
  unsigned r1_after_rn;
  unsigned r2_after_rn;
  unsigned first_rn_active_nodes;
  unsigned first_rn_active_edges;
  unsigned maximum_irreducible_core_nodes;
  unsigned maximum_irreducible_core_edges;
  unsigned rn_episodes;
  unsigned rn_nodes[PBQP_MAX_NODES];
  unsigned rn_choices[PBQP_MAX_NODES];
  uint64_t search_nodes_visited;
  uint64_t search_branches_created;
  unsigned search_maximum_depth;
  unsigned search_limit_hits;
  unsigned primitive_submissions[5];
  unsigned vector_length_histogram[PBQP_VECTOR_HISTOGRAM_BINS];
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
} pbqp_cost_kernel_t;

/* A configured solver: the mode is descriptive, while kernel supplies its cost primitive. */
typedef struct {
  pbqp_mode_t mode;
  pbqp_cost_kernel_t kernel;
  pbqp_solver_config_t config;
} pbqp_solver_t;

/* Fixed-capacity node state, including unary costs and elimination reconstruction data. */
typedef struct {
  int active;
  unsigned domain;
  int32_t unary[PBQP_MAX_DOMAIN];
  unsigned choice[PBQP_MAX_DOMAIN * PBQP_MAX_DOMAIN];
  int first_neighbor;
  int second_neighbor;
  int reduction_kind;
} pbqp_node_t;

/* Fixed-stride row-major binary cost matrix for an unordered pair of node indices. */
typedef struct {
  int active;
  unsigned first;
  unsigned second;
  int32_t cost[PBQP_MAX_DOMAIN * PBQP_MAX_DOMAIN];
} pbqp_edge_t;

/*
 * Caller-owned fixed-capacity PBQP graph and mutable reduction workspace.
 * Finite unary and edge costs must be in [PBQP_MIN_FINITE_COST,
 * PBQP_MAX_FINITE_COST]; ACCEL_INF is also allowed. This preserves exact PBQP
 * reductions despite the accelerator's saturating arithmetic.
 */
typedef struct {
  unsigned node_count;
  unsigned edge_count;
  pbqp_node_t nodes[PBQP_MAX_NODES];
  pbqp_edge_t edges[PBQP_MAX_EDGES];
  int32_t objective_offset;
  unsigned elimination_order[PBQP_MAX_NODES];
  unsigned elimination_count;
  pbqp_statistics_t statistics;
} pbqp_problem_t;

/* Minimum objective and one corresponding assignment returned by an oracle or solver. */
typedef struct {
  int32_t optimum;
  unsigned assignment[PBQP_MAX_NODES];
} pbqp_solution_t;

void pbqp_init(pbqp_problem_t *problem);
pbqp_status_t pbqp_add_node(pbqp_problem_t *problem, unsigned domain, const int32_t *unary);
pbqp_status_t pbqp_add_edge(pbqp_problem_t *problem, unsigned first, unsigned second,
                            const int32_t *costs);
int32_t pbqp_evaluate(const pbqp_problem_t *problem, const unsigned *assignment);
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
