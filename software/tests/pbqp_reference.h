// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Provides an independent exhaustive PBQP oracle for bare-metal differential tests.

#pragma once

#include "common.h"
#include "pbqp/pbqp.h"

static inline int32_t pbqp_reference_evaluate(const pbqp_problem_t *problem,
                                              const unsigned *assignment) {
  int32_t total = 0;
  for (unsigned node = 0; node < problem->node_count; ++node) {
    total = ref_add(total, problem->nodes[node].unary[assignment[node]]);
  }
  for (unsigned index = 0; index < PBQP_MAX_EDGES; ++index) {
    const pbqp_edge_t *edge = &problem->edges[index];
    if (edge->active) {
      total = ref_add(
          total, edge->cost[assignment[edge->first] * edge->stride + assignment[edge->second]]);
    }
  }
  return total;
}

static inline void pbqp_reference_enumerate(const pbqp_problem_t *problem, unsigned node,
                                            unsigned *assignment, pbqp_solution_t *solution,
                                            int *has_assignment) {
  if (node == problem->node_count) {
    const int32_t value = pbqp_reference_evaluate(problem, assignment);
    if (!*has_assignment || value < solution->optimum) {
      solution->optimum = value;
      *has_assignment = 1;
      for (unsigned index = 0; index < problem->node_count; ++index) {
        solution->assignment[index] = assignment[index];
      }
    }
    return;
  }
  for (unsigned value = 0; value < problem->nodes[node].domain; ++value) {
    assignment[node] = value;
    pbqp_reference_enumerate(problem, node + 1, assignment, solution, has_assignment);
  }
}

static inline void pbqp_reference_bruteforce(const pbqp_problem_t *problem,
                                             pbqp_solution_t *solution) {
  unsigned assignment[PBQP_MAX_NODES] = {0};
  int has_assignment = 0;
  solution->optimum = ACCEL_INF;
  pbqp_reference_enumerate(problem, 0, assignment, solution, &has_assignment);
}
