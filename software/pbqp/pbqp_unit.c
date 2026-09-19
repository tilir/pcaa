// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Checks PBQP reductions against exhaustive enumeration without accelerator dependencies.

#include <assert.h>
#include <stdint.h>

#include "accel_protocol.h"
#include "pbqp/pbqp.h"

static void build_triangle(pbqp_problem_t* problem) {
  const int32_t unary0[] = {2, -1};
  const int32_t unary1[] = {0, 3};
  const int32_t unary2[] = {1, -2};
  const int32_t edge01[] = {0, 4, -3, 2};
  const int32_t edge02[] = {2, -1, 5, 0};
  const int32_t edge12[] = {1, 3, -2, 4};
  pbqp_init(problem);
  assert(pbqp_add_node(problem, 2, unary0) == PBQP_OK);
  assert(pbqp_add_node(problem, 2, unary1) == PBQP_OK);
  assert(pbqp_add_node(problem, 2, unary2) == PBQP_OK);
  assert(pbqp_add_edge(problem, 0, 1, edge01) == PBQP_OK);
  assert(pbqp_add_edge(problem, 0, 2, edge02) == PBQP_OK);
  assert(pbqp_add_edge(problem, 1, 2, edge12) == PBQP_OK);
}

static void build_open_wedge(pbqp_problem_t* problem) {
  const int32_t unary[] = {0, 1};
  const int32_t edge[] = {0, 2, -1, 3};
  pbqp_init(problem);
  assert(pbqp_add_node(problem, 2, unary) == PBQP_OK);
  assert(pbqp_add_node(problem, 2, unary) == PBQP_OK);
  assert(pbqp_add_node(problem, 2, unary) == PBQP_OK);
  assert(pbqp_add_edge(problem, 0, 1, edge) == PBQP_OK);
  assert(pbqp_add_edge(problem, 0, 2, edge) == PBQP_OK);
}

static void build_irreducible_core(pbqp_problem_t* problem) {
  const int32_t unary[] = {0, 1};
  const int32_t edge[] = {0, 1, 2, -1};
  pbqp_init(problem);
  for (unsigned node = 0; node < 4; ++node) {
    assert(pbqp_add_node(problem, 2, unary) == PBQP_OK);
  }
  for (unsigned first = 0; first < 4; ++first) {
    for (unsigned second = first + 1; second < 4; ++second) {
      assert(pbqp_add_edge(problem, first, second, edge) == PBQP_OK);
    }
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
  assert(pbqp_bruteforce(&original, &oracle) == PBQP_OK);
  reduced = original;
  pbqp_make_software_kernel(&kernel, &reduced.statistics);
  assert(pbqp_solver_create(&solver, PBQP_MODE_SOFTWARE, &kernel) == PBQP_OK);
  assert(pbqp_solver_solve(&solver, &reduced, &solution) == PBQP_OK);
  assert(solution.optimum == oracle.optimum);
  assert(pbqp_evaluate(&original, solution.assignment) == oracle.optimum);
  assert(reduced.statistics.r2_count == (unsigned)expect_r2);
  if (expect_r2 != 0) {
    assert(reduced.statistics.r0_count != 0);
  }
}

int main(void) {
  pbqp_problem_t original;
  pbqp_problem_t reduced;
  pbqp_solution_t oracle;
  pbqp_solution_t solution;
  pbqp_cost_kernel_t kernel;
  pbqp_solver_t solver;

  build_triangle(&original);
  assert(pbqp_bruteforce(&original, &oracle) == PBQP_OK);
  reduced = original;
  pbqp_make_software_kernel(&kernel, &reduced.statistics);
  assert(pbqp_solver_create(&solver, PBQP_MODE_SOFTWARE, &kernel) == PBQP_OK);
  assert(pbqp_solver_solve(&solver, &reduced, &solution) == PBQP_OK);
  assert(solution.optimum == oracle.optimum);
  assert(pbqp_evaluate(&original, solution.assignment) == oracle.optimum);
  assert(reduced.statistics.r2_count == 1);
  assert(reduced.statistics.r1_count != 0);
  assert(reduced.statistics.r0_count != 0);
  assert(reduced.statistics.primitive_submissions[ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN] != 0);
  check_problem(build_open_wedge, 1);
  check_problem(build_irreducible_core, 0);
  return 0;
}
