// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements the C PBQP API using exact R0/R1/R2 reductions and a small-core oracle.

#include "pbqp.h"
#include "cost_math.h"

#if defined(__riscv)
/* The freestanding RV64 toolchain has no <assert.h>; preserve assertion semantics. */
#define assert(expression) ((expression) ? static_cast<void>(0) : __builtin_trap())
#else
#include <assert.h>
#endif

namespace {

enum ReductionKind { kReductionR0, kReductionR1, kReductionR2 };

bool IsValidCost(int32_t cost) {
  return cost == ACCEL_INF || (cost >= PBQP_MIN_FINITE_COST && cost <= PBQP_MAX_FINITE_COST);
}

class Graph {
 public:
  explicit Graph(pbqp_problem_t &state) : state_(state) {}

  pbqp_problem_t &State() {
    return state_;
  }
  const pbqp_problem_t &State() const {
    return state_;
  }

  int FindEdge(unsigned first, unsigned second) const {
    for (unsigned index = 0; index < PBQP_MAX_EDGES; ++index) {
      const pbqp_edge_t &edge = state_.edges[index];
      if (edge.active && ((edge.first == first && edge.second == second) ||
                          (edge.first == second && edge.second == first))) {
        return static_cast<int>(index);
      }
    }
    return -1;
  }

  unsigned NeighborCount(unsigned node, int *edge_indices) const {
    unsigned count = 0;
    for (unsigned index = 0; index < PBQP_MAX_EDGES; ++index) {
      const pbqp_edge_t &edge = state_.edges[index];
      if (!edge.active || (edge.first != node && edge.second != node)) {
        continue;
      }
      if (count < 2) {
        edge_indices[count] = static_cast<int>(index);
      }
      ++count;
    }
    return count;
  }

  int FindReducibleNode(int *edge_indices) const {
    for (unsigned node = 0; node < state_.node_count; ++node) {
      if (state_.nodes[node].active && NeighborCount(node, edge_indices) <= 2) {
        return static_cast<int>(node);
      }
    }
    return -1;
  }

  unsigned OtherNode(const pbqp_edge_t &edge, unsigned node) const {
    assert(edge.first == node || edge.second == node);
    return edge.first == node ? edge.second : edge.first;
  }

  int AddFillEdge(unsigned first, unsigned second) {
    assert(first < state_.node_count);
    assert(second < state_.node_count);
    assert(first != second);
    for (unsigned index = 0; index < PBQP_MAX_EDGES; ++index) {
      pbqp_edge_t &edge = state_.edges[index];
      if (edge.active) {
        continue;
      }
      edge = {};
      edge.active = 1;
      edge.first = first;
      edge.second = second;
      ++state_.edge_count;
      return static_cast<int>(index);
    }
    return -1;
  }

 private:
  pbqp_problem_t &state_;
};

class CostKernel {
 public:
  explicit CostKernel(const pbqp_cost_kernel_t &api) : api_(api) {
    assert(api_.min2_argmin != nullptr);
    assert(api_.min3_argmin != nullptr);
  }

  int Min2(pbqp_vector_view_t first, pbqp_vector_view_t second,
           accel_min_argmin_result_t *result) const {
    return api_.min2_argmin(api_.context, first, second, result);
  }

  int Min3(pbqp_vector_view_t first, pbqp_vector_view_t second, pbqp_vector_view_t third,
           accel_min_argmin_result_t *result) const {
    return api_.min3_argmin(api_.context, first, second, third, result);
  }

 private:
  const pbqp_cost_kernel_t &api_;
};

class Solver {
 public:
  explicit Solver(const pbqp_cost_kernel_t &kernel) : kernel_(kernel) {}

  pbqp_status_t Solve(pbqp_problem_t *problem, pbqp_solution_t *solution) const {
    if (problem == nullptr || solution == nullptr) {
      return PBQP_ARGUMENT_ERROR;
    }

    Graph graph(*problem);
    pbqp_problem_t &state = graph.State();
    state.statistics.nodes = state.node_count;
    state.statistics.initial_edges = state.edge_count;

    while (true) {
      int edge_indices[2];
      const int node = graph.FindReducibleNode(edge_indices);
      if (node < 0) {
        break;
      }

      const unsigned degree = graph.NeighborCount(static_cast<unsigned>(node), edge_indices);
      pbqp_status_t status = PBQP_OK;
      if (degree == 0) {
        status = ReduceR0(graph, static_cast<unsigned>(node));
      } else if (degree == 1) {
        status = ReduceR1(graph, static_cast<unsigned>(node), edge_indices[0]);
      } else {
        status = ReduceR2(graph, static_cast<unsigned>(node), edge_indices[0], edge_indices[1]);
      }
      if (status != PBQP_OK) {
        return status;
      }
    }

    unsigned core_assignment[PBQP_MAX_NODES] = {};
    bool has_assignment = false;
    solution->optimum = ACCEL_INF;
    EnumerateActiveCore(state, 0, core_assignment, solution, &has_assignment);
    solution->optimum = accel_cost_add(state.objective_offset, solution->optimum);
    ReconstructSolution(state, solution);
    return PBQP_OK;
  }

 private:
  static pbqp_vector_view_t EdgeView(const pbqp_edge_t &edge, unsigned node, unsigned other_value,
                                     unsigned length) {
    assert(edge.first == node || edge.second == node);
    assert(other_value < PBQP_MAX_DOMAIN);
    assert(length <= PBQP_MAX_DOMAIN);
    if (node == edge.first) {
      return {edge.cost + other_value, length, PBQP_MAX_DOMAIN};
    }
    return {edge.cost + other_value * PBQP_MAX_DOMAIN, length, 1};
  }

  static void RecordView(pbqp_statistics_t *statistics, pbqp_vector_view_t view) {
    if (view.stride == 1) {
      ++statistics->contiguous_views;
    } else {
      ++statistics->strided_views;
    }
  }

  static void RecordOperation(pbqp_statistics_t *statistics, unsigned opcode, unsigned length,
                              unsigned input_count) {
    ++statistics->primitive_submissions[opcode];
    ++statistics->vector_length_histogram[length];
    statistics->logical_map_elements += length;
    statistics->logical_bytes_read += static_cast<uint64_t>(length) * input_count * sizeof(int32_t);
    statistics->logical_bytes_written += sizeof(accel_min_argmin_result_t);
  }

  pbqp_status_t ReduceR0(Graph &graph, unsigned node_index) const {
    pbqp_problem_t &problem = graph.State();
    pbqp_node_t &node = problem.nodes[node_index];
    int32_t best = ACCEL_INF;
    unsigned choice = 0;
    for (unsigned value = 0; value < node.domain; ++value) {
      if (value == 0 || node.unary[value] < best) {
        best = node.unary[value];
        choice = value;
      }
    }
    problem.objective_offset = accel_cost_add(problem.objective_offset, best);
    node.choice[0] = choice;
    node.reduction_kind = kReductionR0;
    node.active = 0;
    ++problem.statistics.r0_count;
    problem.elimination_order[problem.elimination_count++] = node_index;
    return PBQP_OK;
  }

  pbqp_status_t ReduceR1(Graph &graph, unsigned node_index, int edge_index) const {
    pbqp_problem_t &problem = graph.State();
    pbqp_edge_t &edge = problem.edges[edge_index];
    const unsigned neighbor_index = graph.OtherNode(edge, node_index);
    pbqp_node_t &node = problem.nodes[node_index];
    pbqp_node_t &neighbor = problem.nodes[neighbor_index];

    for (unsigned neighbor_value = 0; neighbor_value < neighbor.domain; ++neighbor_value) {
      const pbqp_vector_view_t unary = {node.unary, node.domain, 1};
      const pbqp_vector_view_t edge_cost = EdgeView(edge, node_index, neighbor_value, node.domain);
      accel_min_argmin_result_t result;
      RecordView(&problem.statistics, unary);
      RecordView(&problem.statistics, edge_cost);
      RecordOperation(&problem.statistics, ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN, node.domain, 2);
      if (kernel_.Min2(unary, edge_cost, &result) != 0) {
        return PBQP_ARGUMENT_ERROR;
      }
      if (result.index >= node.domain) {
        return PBQP_ARGUMENT_ERROR;
      }
      assert(result.index < node.domain);
      neighbor.unary[neighbor_value] = accel_cost_add(neighbor.unary[neighbor_value], result.value);
      node.choice[neighbor_value] = result.index;
    }

    edge.active = 0;
    --problem.edge_count;
    node.first_neighbor = static_cast<int>(neighbor_index);
    node.reduction_kind = kReductionR1;
    node.active = 0;
    ++problem.statistics.r1_count;
    problem.elimination_order[problem.elimination_count++] = node_index;
    return PBQP_OK;
  }

  pbqp_status_t ReduceR2(Graph &graph, unsigned node_index, int first_edge_index,
                         int second_edge_index) const {
    pbqp_problem_t &problem = graph.State();
    pbqp_edge_t &first_edge = problem.edges[first_edge_index];
    pbqp_edge_t &second_edge = problem.edges[second_edge_index];
    const unsigned first_neighbor = graph.OtherNode(first_edge, node_index);
    const unsigned second_neighbor = graph.OtherNode(second_edge, node_index);
    pbqp_node_t &node = problem.nodes[node_index];
    pbqp_node_t &first = problem.nodes[first_neighbor];
    pbqp_node_t &second = problem.nodes[second_neighbor];

    int fill_edge_index = graph.FindEdge(first_neighbor, second_neighbor);
    if (fill_edge_index < 0) {
      fill_edge_index = graph.AddFillEdge(first_neighbor, second_neighbor);
      if (fill_edge_index < 0) {
        return PBQP_CAPACITY_ERROR;
      }
    }
    pbqp_edge_t &fill_edge = problem.edges[fill_edge_index];

    for (unsigned first_value = 0; first_value < first.domain; ++first_value) {
      for (unsigned second_value = 0; second_value < second.domain; ++second_value) {
        const pbqp_vector_view_t unary = {node.unary, node.domain, 1};
        const pbqp_vector_view_t first_cost =
            EdgeView(first_edge, node_index, first_value, node.domain);
        const pbqp_vector_view_t second_cost =
            EdgeView(second_edge, node_index, second_value, node.domain);
        accel_min_argmin_result_t result;
        RecordView(&problem.statistics, unary);
        RecordView(&problem.statistics, first_cost);
        RecordView(&problem.statistics, second_cost);
        RecordOperation(&problem.statistics, ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN, node.domain,
                        3);
        if (kernel_.Min3(unary, first_cost, second_cost, &result) != 0) {
          return PBQP_ARGUMENT_ERROR;
        }
        if (result.index >= node.domain) {
          return PBQP_ARGUMENT_ERROR;
        }
        assert(result.index < node.domain);

        node.choice[first_value * second.domain + second_value] = result.index;
        const unsigned fill_first_value =
            fill_edge.first == first_neighbor ? first_value : second_value;
        const unsigned fill_second_value =
            fill_edge.second == second_neighbor ? second_value : first_value;
        const unsigned fill_index = fill_first_value * PBQP_MAX_DOMAIN + fill_second_value;
        fill_edge.cost[fill_index] = accel_cost_add(fill_edge.cost[fill_index], result.value);
      }
    }

    first_edge.active = 0;
    second_edge.active = 0;
    problem.edge_count -= 2;
    node.first_neighbor = static_cast<int>(first_neighbor);
    node.second_neighbor = static_cast<int>(second_neighbor);
    node.reduction_kind = kReductionR2;
    node.active = 0;
    ++problem.statistics.r2_count;
    problem.elimination_order[problem.elimination_count++] = node_index;
    return PBQP_OK;
  }

  static int32_t EvaluateActiveCore(const pbqp_problem_t &problem, const unsigned *assignment) {
    int32_t total = 0;
    for (unsigned node = 0; node < problem.node_count; ++node) {
      if (problem.nodes[node].active) {
        total = accel_cost_add(total, problem.nodes[node].unary[assignment[node]]);
      }
    }
    for (unsigned index = 0; index < PBQP_MAX_EDGES; ++index) {
      const pbqp_edge_t &edge = problem.edges[index];
      if (edge.active) {
        total = accel_cost_add(
            total, edge.cost[assignment[edge.first] * PBQP_MAX_DOMAIN + assignment[edge.second]]);
      }
    }
    return total;
  }

  static void EnumerateActiveCore(const pbqp_problem_t &problem, unsigned node,
                                  unsigned *assignment, pbqp_solution_t *solution,
                                  bool *has_assignment) {
    if (node == problem.node_count) {
      const int32_t value = EvaluateActiveCore(problem, assignment);
      if (!*has_assignment || value < solution->optimum) {
        solution->optimum = value;
        *has_assignment = true;
        for (unsigned index = 0; index < problem.node_count; ++index) {
          solution->assignment[index] = assignment[index];
        }
      }
      return;
    }
    if (!problem.nodes[node].active) {
      EnumerateActiveCore(problem, node + 1, assignment, solution, has_assignment);
      return;
    }
    for (unsigned value = 0; value < problem.nodes[node].domain; ++value) {
      assignment[node] = value;
      EnumerateActiveCore(problem, node + 1, assignment, solution, has_assignment);
    }
  }

  static void ReconstructSolution(const pbqp_problem_t &problem, pbqp_solution_t *solution) {
    for (unsigned position = problem.elimination_count; position > 0; --position) {
      const unsigned node_index = problem.elimination_order[position - 1];
      const pbqp_node_t &node = problem.nodes[node_index];
      if (node.reduction_kind == kReductionR0) {
        solution->assignment[node_index] = node.choice[0];
      } else if (node.reduction_kind == kReductionR1) {
        assert(node.first_neighbor >= 0);
        assert(solution->assignment[node.first_neighbor] <
               problem.nodes[node.first_neighbor].domain);
        solution->assignment[node_index] = node.choice[solution->assignment[node.first_neighbor]];
      } else {
        assert(node.first_neighbor >= 0);
        assert(node.second_neighbor >= 0);
        const unsigned first_value = solution->assignment[node.first_neighbor];
        const unsigned second_value = solution->assignment[node.second_neighbor];
        assert(first_value < problem.nodes[node.first_neighbor].domain);
        assert(second_value < problem.nodes[node.second_neighbor].domain);
        const unsigned choice_index =
            first_value * problem.nodes[node.second_neighbor].domain + second_value;
        solution->assignment[node_index] = node.choice[choice_index];
      }
    }
  }

  const CostKernel kernel_;
};

void Enumerate(const pbqp_problem_t &problem, unsigned node, unsigned *assignment,
               pbqp_solution_t *solution, bool *has_assignment) {
  if (node == problem.node_count) {
    const int32_t value = pbqp_evaluate(&problem, assignment);
    if (!*has_assignment || value < solution->optimum) {
      solution->optimum = value;
      *has_assignment = true;
      for (unsigned index = 0; index < problem.node_count; ++index) {
        solution->assignment[index] = assignment[index];
      }
    }
    return;
  }
  for (unsigned value = 0; value < problem.nodes[node].domain; ++value) {
    assignment[node] = value;
    Enumerate(problem, node + 1, assignment, solution, has_assignment);
  }
}

int SoftwareMin2(void *, pbqp_vector_view_t first, pbqp_vector_view_t second,
                 accel_min_argmin_result_t *result) {
  result->value = ACCEL_INF;
  result->index = 0;
  for (size_t index = 0; index < first.length; ++index) {
    const int32_t value =
        accel_cost_add(first.base[index * first.stride], second.base[index * second.stride]);
    if (index == 0 || value < result->value) {
      result->value = value;
      result->index = static_cast<uint32_t>(index);
    }
  }
  return 0;
}

int SoftwareMin3(void *, pbqp_vector_view_t first, pbqp_vector_view_t second,
                 pbqp_vector_view_t third, accel_min_argmin_result_t *result) {
  result->value = ACCEL_INF;
  result->index = 0;
  for (size_t index = 0; index < first.length; ++index) {
    const int32_t sum =
        accel_cost_add(first.base[index * first.stride], second.base[index * second.stride]);
    const int32_t value = accel_cost_add(sum, third.base[index * third.stride]);
    if (index == 0 || value < result->value) {
      result->value = value;
      result->index = static_cast<uint32_t>(index);
    }
  }
  return 0;
}

}  // namespace

extern "C" {

void pbqp_init(pbqp_problem_t *problem) {
  if (problem != nullptr) {
    *problem = {};
  }
}

pbqp_status_t pbqp_add_node(pbqp_problem_t *problem, unsigned domain, const int32_t *unary) {
  if (problem == nullptr || unary == nullptr || domain == 0 || domain > PBQP_MAX_DOMAIN) {
    return PBQP_ARGUMENT_ERROR;
  }
  if (problem->node_count == PBQP_MAX_NODES) {
    return PBQP_CAPACITY_ERROR;
  }

  for (unsigned index = 0; index < domain; ++index) {
    if (!IsValidCost(unary[index])) {
      return PBQP_COST_RANGE_ERROR;
    }
  }

  pbqp_node_t &node = problem->nodes[problem->node_count++];
  node.active = 1;
  node.domain = domain;
  for (unsigned index = 0; index < domain; ++index) {
    node.unary[index] = unary[index];
  }
  return PBQP_OK;
}

pbqp_status_t pbqp_add_edge(pbqp_problem_t *problem, unsigned first, unsigned second,
                            const int32_t *costs) {
  if (problem == nullptr || costs == nullptr || first >= problem->node_count ||
      second >= problem->node_count || first == second) {
    return PBQP_ARGUMENT_ERROR;
  }
  for (unsigned index = 0; index < PBQP_MAX_EDGES; ++index) {
    const pbqp_edge_t &edge = problem->edges[index];
    if (edge.active && ((edge.first == first && edge.second == second) ||
                        (edge.first == second && edge.second == first))) {
      return PBQP_ARGUMENT_ERROR;
    }
  }

  const unsigned first_domain = problem->nodes[first].domain;
  const unsigned second_domain = problem->nodes[second].domain;
  for (unsigned first_value = 0; first_value < first_domain; ++first_value) {
    for (unsigned second_value = 0; second_value < second_domain; ++second_value) {
      if (!IsValidCost(costs[first_value * second_domain + second_value])) {
        return PBQP_COST_RANGE_ERROR;
      }
    }
  }

  for (unsigned index = 0; index < PBQP_MAX_EDGES; ++index) {
    pbqp_edge_t &edge = problem->edges[index];
    if (edge.active) {
      continue;
    }
    edge.active = 1;
    edge.first = first;
    edge.second = second;
    for (unsigned first_value = 0; first_value < first_domain; ++first_value) {
      for (unsigned second_value = 0; second_value < second_domain; ++second_value) {
        edge.cost[first_value * PBQP_MAX_DOMAIN + second_value] =
            costs[first_value * second_domain + second_value];
      }
    }
    ++problem->edge_count;
    return PBQP_OK;
  }
  return PBQP_CAPACITY_ERROR;
}

int32_t pbqp_evaluate(const pbqp_problem_t *problem, const unsigned *assignment) {
  int32_t total = 0;
  for (unsigned node = 0; node < problem->node_count; ++node) {
    total = accel_cost_add(total, problem->nodes[node].unary[assignment[node]]);
  }
  for (unsigned index = 0; index < PBQP_MAX_EDGES; ++index) {
    const pbqp_edge_t &edge = problem->edges[index];
    if (edge.active) {
      total = accel_cost_add(
          total, edge.cost[assignment[edge.first] * PBQP_MAX_DOMAIN + assignment[edge.second]]);
    }
  }
  return total;
}

pbqp_status_t pbqp_bruteforce(const pbqp_problem_t *problem, pbqp_solution_t *solution) {
  if (problem == nullptr || solution == nullptr) {
    return PBQP_ARGUMENT_ERROR;
  }
  unsigned assignment[PBQP_MAX_NODES] = {};
  bool has_assignment = false;
  solution->optimum = ACCEL_INF;
  Enumerate(*problem, 0, assignment, solution, &has_assignment);
  return PBQP_OK;
}

void pbqp_make_software_kernel(pbqp_cost_kernel_t *kernel, pbqp_statistics_t *statistics) {
  kernel->context = statistics;
  kernel->min2_argmin = SoftwareMin2;
  kernel->min3_argmin = SoftwareMin3;
}

pbqp_status_t pbqp_solver_create(pbqp_solver_t *solver, pbqp_mode_t mode,
                                 const pbqp_cost_kernel_t *kernel) {
  if (solver == nullptr || kernel == nullptr || kernel->min2_argmin == nullptr ||
      kernel->min3_argmin == nullptr) {
    return PBQP_ARGUMENT_ERROR;
  }
  solver->mode = mode;
  solver->kernel = *kernel;
  return PBQP_OK;
}

pbqp_status_t pbqp_solver_solve(pbqp_solver_t *solver, pbqp_problem_t *problem,
                                pbqp_solution_t *solution) {
  if (solver == nullptr) {
    return PBQP_ARGUMENT_ERROR;
  }
  return Solver(solver->kernel).Solve(problem, solution);
}

}  // extern "C"
