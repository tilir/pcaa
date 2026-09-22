// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Checks PBQP reductions against exhaustive enumeration without accelerator dependencies.

#include "accel_protocol.h"
#include "pbqp/pbqp.h"

#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#define CHECK(expression) EXPECT_TRUE(expression)

static void init_problem(pbqp_problem_t* problem) {
  CHECK(pbqp_init(problem, pbqp_heap_allocator(), PBQP_MAX_NODES, PBQP_MAX_EDGES,
                  PBQP_MAX_DOMAIN) == PBQP_OK);
}

static void clone_problem(pbqp_problem_t* destination, const pbqp_problem_t* source) {
  CHECK(pbqp_problem_clone(destination, source, pbqp_heap_allocator()) == PBQP_OK);
}

static void init_solution(pbqp_solution_t* solution) {
  const pbqp_allocator_t allocator = pbqp_heap_allocator();
  unsigned* assignment =
      static_cast<unsigned*>(allocator.allocate(allocator.context, PBQP_MAX_NODES * sizeof(int)));
  ASSERT_NE(assignment, nullptr);
  pbqp_solution_init(solution, assignment, PBQP_MAX_NODES);
}

static void build_triangle(pbqp_problem_t* problem) {
  const int32_t unary0[] = {2, -1};
  const int32_t unary1[] = {0, 3};
  const int32_t unary2[] = {1, -2};
  const int32_t edge01[] = {0, 4, -3, 2};
  const int32_t edge02[] = {2, -1, 5, 0};
  const int32_t edge12[] = {1, 3, -2, 4};
  init_problem(problem);
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
  init_problem(problem);
  CHECK(pbqp_add_node(problem, 2, unary) == PBQP_OK);
  CHECK(pbqp_add_node(problem, 2, unary) == PBQP_OK);
  CHECK(pbqp_add_node(problem, 2, unary) == PBQP_OK);
  CHECK(pbqp_add_edge(problem, 0, 1, edge) == PBQP_OK);
  CHECK(pbqp_add_edge(problem, 0, 2, edge) == PBQP_OK);
}

static void build_irreducible_core(pbqp_problem_t* problem) {
  const int32_t unary[] = {0, 1};
  const int32_t edge[] = {0, 1, 2, -1};
  init_problem(problem);
  for (unsigned node = 0; node < 4; ++node) {
    CHECK(pbqp_add_node(problem, 2, unary) == PBQP_OK);
  }
  for (unsigned first = 0; first < 4; ++first) {
    for (unsigned second = first + 1; second < 4; ++second) {
      CHECK(pbqp_add_edge(problem, first, second, edge) == PBQP_OK);
    }
  }
}

static void build_rn_orientation_problem(pbqp_problem_t* problem) {
  const int32_t unary[] = {0, 0};
  const int32_t constrained[] = {5, 0, ACCEL_INF, ACCEL_INF};
  const int32_t zero[] = {0, 0, 0, 0};
  init_problem(problem);
  for (unsigned node = 0; node < 4; ++node) {
    CHECK(pbqp_add_node(problem, 2, unary) == PBQP_OK);
  }
  CHECK(pbqp_add_edge(problem, 0, 1, constrained) == PBQP_OK);
  CHECK(pbqp_add_edge(problem, 0, 2, constrained) == PBQP_OK);
  CHECK(pbqp_add_edge(problem, 0, 3, constrained) == PBQP_OK);
  CHECK(pbqp_add_edge(problem, 1, 2, zero) == PBQP_OK);
  CHECK(pbqp_add_edge(problem, 1, 3, zero) == PBQP_OK);
  CHECK(pbqp_add_edge(problem, 2, 3, zero) == PBQP_OK);
}

static void build_rn_transposed_orientation_problem(pbqp_problem_t* problem) {
  const int32_t unary3[] = {0, 0, 0};
  const int32_t unary2[] = {0, 0};
  const int32_t asymmetric[] = {0, -5, 0, 0, 0, 0};
  const int32_t zero32[] = {0, 0, 0, 0, 0, 0};
  const int32_t zero22[] = {0, 0, 0, 0};
  init_problem(problem);
  CHECK(pbqp_add_node(problem, 3, unary3) == PBQP_OK);
  for (unsigned node = 1; node < 5; ++node) {
    CHECK(pbqp_add_node(problem, 2, unary2) == PBQP_OK);
  }
  CHECK(pbqp_add_edge(problem, 0, 1, asymmetric) == PBQP_OK);
  for (unsigned first = 0; first < 5; ++first) {
    for (unsigned second = first + 1; second < 5; ++second) {
      if ((first != 0 || second != 1) && (first != 0 || second != 4)) {
        const int32_t* costs = first == 0 ? zero32 : zero22;
        CHECK(pbqp_add_edge(problem, first, second, costs) == PBQP_OK);
      }
    }
  }
}

struct BatchRecordingKernel {
  pbqp_cost_kernel_t inner;
  unsigned min2_value_batch_calls = 0;
  std::vector<int32_t> rn_projected_terms;

  static int min2_value_batch(void* opaque, const pbqp_min2_value_job_t* jobs, size_t count) {
    BatchRecordingKernel* recording = static_cast<BatchRecordingKernel*>(opaque);
    ++recording->min2_value_batch_calls;
    const int status = recording->inner.min2_value_batch(recording->inner.context, jobs, count);
    if (status == 0) {
      for (size_t index = 0; index < count; ++index)
        recording->rn_projected_terms.push_back(*jobs[index].result);
    }
    return status;
  }
};

static void make_batch_recording_kernel(pbqp_cost_kernel_t* kernel,
                                        BatchRecordingKernel* recording,
                                        pbqp_statistics_t* statistics) {
  pbqp_make_software_kernel(&recording->inner, statistics);
  *kernel = recording->inner;
  kernel->context = recording;
  kernel->min2_value_batch = BatchRecordingKernel::min2_value_batch;
}

struct TraceCapture {
  std::vector<pbqp_solver_event_t> events;

  static void emit(void* opaque, const pbqp_solver_event_t* event) {
    static_cast<TraceCapture*>(opaque)->events.push_back(*event);
  }
};

static void check_cost_range(void) {
  pbqp_problem_t problem;
  const int32_t out_of_range[] = {ACCEL_INF - 1};
  const int32_t valid[] = {0};
  init_problem(&problem);
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
  unsigned oracle_assignment[PBQP_MAX_NODES] = {1000000};
  unsigned solution_assignment[PBQP_MAX_NODES] = {1000000};
  pbqp_solution_t oracle;
  pbqp_solution_t solution;
  pbqp_cost_kernel_t kernel;
  pbqp_solver_t solver;

  pbqp_solution_init(&oracle, oracle_assignment, PBQP_MAX_NODES);
  pbqp_solution_init(&solution, solution_assignment, PBQP_MAX_NODES);
  init_problem(&original);
  for (unsigned node = 0; node < 4; ++node) {
    CHECK(pbqp_add_node(&original, 1, infinite) == PBQP_OK);
  }
  for (unsigned first = 0; first < 4; ++first) {
    for (unsigned second = first + 1; second < 4; ++second) {
      CHECK(pbqp_add_edge(&original, first, second, zero) == PBQP_OK);
    }
  }
  CHECK(pbqp_bruteforce(&original, &oracle) == PBQP_OK);
  clone_problem(&reduced, &original);
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
  init_solution(&oracle);
  init_solution(&solution);
  CHECK(pbqp_bruteforce(&original, &oracle) == PBQP_OK);
  clone_problem(&reduced, &original);
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
  init_solution(&oracle);
  init_solution(&solution);
  CHECK(pbqp_bruteforce(&original, &oracle) == PBQP_OK);
  clone_problem(&reduced, &original);
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
  init_solution(&solution);
  clone_problem(&reduced, &original);
  pbqp_make_software_kernel(&kernel, &reduced.statistics);
  config.strategy = PBQP_STRATEGY_REDUCE_ONLY;
  CHECK(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &kernel, &config) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &reduced, &solution) == PBQP_IRREDUCIBLE);

  for (pbqp_rn_policy_t policy = PBQP_RN_MIN_DEGREE; policy <= PBQP_RN_MIN_WORK;
       policy = static_cast<pbqp_rn_policy_t>(policy + 1)) {
    clone_problem(&reduced, &original);
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
    CHECK(reduced.statistics.rn_episodes == reduced.statistics.rn_count);
    CHECK(reduced.statistics.rn_cascade_r0[0] + reduced.statistics.rn_cascade_r1[0] +
              reduced.statistics.rn_cascade_r2[0] ==
          reduced.statistics.rn_cascade_total_length);
    CHECK(reduced.statistics.rn_cascade_length_histogram[
              reduced.statistics.rn_cascade_total_length] == 1);
    CHECK(reduced.statistics.rn_cascade_maximum_length ==
          reduced.statistics.rn_cascade_total_length);
  }
}

TEST(PbqpSolver, RnProjectsTheConditionedNodeAxis) {
  pbqp_problem_t original;
  pbqp_problem_t reduced;
  pbqp_solution_t solution;
  pbqp_cost_kernel_t kernel;
  pbqp_solver_t solver;
  pbqp_solver_config_t config = pbqp_solver_default_config();

  build_rn_orientation_problem(&original);
  init_solution(&solution);
  clone_problem(&reduced, &original);
  config.strategy = PBQP_STRATEGY_HEURISTIC_RN;
  pbqp_make_software_kernel(&kernel, &reduced.statistics);
  CHECK(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &kernel, &config) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &reduced, &solution) == PBQP_OK);
  CHECK(solution.optimum == 0);
  CHECK(solution.assignment[0] == 0);
  CHECK(pbqp_evaluate(&original, solution.assignment) == 0);

  build_rn_transposed_orientation_problem(&original);
  clone_problem(&reduced, &original);
  config.rn_policy = PBQP_RN_MAX_DEGREE;
  pbqp_make_software_kernel(&kernel, &reduced.statistics);
  CHECK(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &kernel, &config) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &reduced, &solution) == PBQP_OK);
  CHECK(reduced.statistics.rn_nodes[0] == 1);
  CHECK(reduced.statistics.rn_choices[0] == 1);
  CHECK(pbqp_evaluate(&original, solution.assignment) == solution.optimum);
}

TEST(PbqpSolver, PerNodeRnBatchingMatchesLegacyPerEdgePath) {
  pbqp_problem_t original;
  pbqp_problem_t per_node;
  pbqp_problem_t per_edge;
  pbqp_solution_t per_node_solution;
  pbqp_solution_t per_edge_solution;
  pbqp_cost_kernel_t kernel;
  pbqp_solver_t solver;
  pbqp_solver_config_t config = pbqp_solver_default_config();
  BatchRecordingKernel recording;

  build_irreducible_core(&original);
  init_solution(&per_node_solution);
  init_solution(&per_edge_solution);
  config.strategy = PBQP_STRATEGY_HEURISTIC_RN;

  clone_problem(&per_node, &original);
  make_batch_recording_kernel(&kernel, &recording, &per_node.statistics);
  config.rn_batching = PBQP_RN_BATCH_PER_NODE;
  CHECK(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &kernel, &config) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &per_node, &per_node_solution) == PBQP_OK);
  CHECK(recording.min2_value_batch_calls == per_node.statistics.rn_count);
  const std::vector<int32_t> per_node_terms = recording.rn_projected_terms;

  recording = {};
  clone_problem(&per_edge, &original);
  make_batch_recording_kernel(&kernel, &recording, &per_edge.statistics);
  config.rn_batching = PBQP_RN_BATCH_PER_EDGE;
  CHECK(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &kernel, &config) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &per_edge, &per_edge_solution) == PBQP_OK);
  CHECK(recording.min2_value_batch_calls == per_edge.statistics.rn_projection_count);

  // Projected terms appear in the same edge/value order. Since both paths
  // fold them into the same unary vector in that order, every RN score bit
  // pattern (not merely the chosen minimum) is identical.
  CHECK(per_node_terms == recording.rn_projected_terms);
  CHECK(per_node_solution.optimum == per_edge_solution.optimum);
  CHECK(memcmp(per_node_solution.assignment, per_edge_solution.assignment,
               original.node_count * sizeof(unsigned)) == 0);
  CHECK(pbqp_evaluate(&original, per_node_solution.assignment) == per_node_solution.optimum);
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
  TraceCapture trace;
  const pbqp_trace_sink_t trace_sink = {&trace, TraceCapture::emit};

  build_irreducible_core(&original);
  init_solution(&enumeration_solution);
  init_solution(&branch_solution);
  clone_problem(&enumerated, &original);
  pbqp_make_software_kernel(&kernel, &enumerated.statistics);
  config.strategy = PBQP_STRATEGY_EXACT_CORE_ENUMERATION;
  CHECK(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &kernel, &config) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &enumerated, &enumeration_solution) == PBQP_OK);

  clone_problem(&branched, &original);
  pbqp_make_software_kernel(&kernel, &branched.statistics);
  config.strategy = PBQP_STRATEGY_EXACT_BRANCH_REDUCE;
  config.trace_sink = &trace_sink;
  CHECK(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &kernel, &config) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &branched, &branch_solution) == PBQP_OK);
  CHECK(branch_solution.optimum == enumeration_solution.optimum);
  CHECK(pbqp_evaluate(&original, branch_solution.assignment) == branch_solution.optimum);
  CHECK(branched.statistics.search_nodes_visited > 1);
  CHECK(branched.statistics.search_branches_created != 0);
  CHECK(branched.statistics.condition_count != 0);
  CHECK(branched.statistics.condition_elements != 0);
  CHECK(branched.statistics.rn_count == 0);
  bool saw_branch = false;
  for (const pbqp_solver_event_t& event : trace.events) {
    if (event.type == PBQP_TRACE_BRANCH_SELECT) {
      saw_branch = true;
      CHECK(event.branch_domain == 2);
    } else {
      CHECK(event.branch_domain == 0);
    }
  }
  CHECK(saw_branch);
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
  init_solution(&rn_solution);
  init_solution(&hybrid_solution);
  clone_problem(&rn_problem, &original);
  pbqp_make_software_kernel(&kernel, &rn_problem.statistics);
  config.strategy = PBQP_STRATEGY_HEURISTIC_RN;
  CHECK(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &kernel, &config) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &rn_problem, &rn_solution) == PBQP_OK);

  clone_problem(&hybrid_problem, &original);
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
  init_solution(&solution);
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

TEST(PbqpSolver, HostedStorageExceedsBareMetalCapacity) {
  constexpr unsigned kNodeCount = PBQP_MAX_NODES * 4;
  constexpr unsigned kDomain = PBQP_MAX_DOMAIN * 2;
  std::vector<int32_t> unary(kDomain, 1);
  std::vector<int32_t> edge(kDomain * kDomain);
  unary[0] = 0;
  pbqp_problem_t problem;
  CHECK(pbqp_init(&problem, pbqp_heap_allocator(), kNodeCount, kNodeCount, kDomain) == PBQP_OK);
  for (unsigned node = 0; node < kNodeCount; ++node)
    CHECK(pbqp_add_node(&problem, kDomain, unary.data()) == PBQP_OK);
  for (unsigned node = 1; node < kNodeCount; ++node)
    CHECK(pbqp_add_edge(&problem, node - 1, node, edge.data()) == PBQP_OK);

  std::vector<unsigned> assignment(kNodeCount);
  pbqp_solution_t solution;
  pbqp_solution_init(&solution, assignment.data(), assignment.size());
  pbqp_cost_kernel_t kernel;
  pbqp_make_software_kernel(&kernel, &problem.statistics);
  pbqp_solver_t solver;
  CHECK(pbqp_solver_create(&solver, PBQP_MODE_SOFTWARE, &kernel) == PBQP_OK);
  CHECK(pbqp_solver_solve(&solver, &problem, &solution) == PBQP_OK);
  CHECK(solution.optimum == 0);
  CHECK(problem.statistics.nodes == kNodeCount);
  for (unsigned value : assignment)
    CHECK(value == 0);
  pbqp_destroy(&problem);
}
