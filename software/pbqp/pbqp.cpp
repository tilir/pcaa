// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements the C PBQP API using exact R0/R1/R2 reductions and a small-core oracle.

#include "pbqp.h"
#include "cost_math.h"
#include "pbqp_storage.h"

#if defined(__riscv)
/* The freestanding RV64 toolchain has no <assert.h>; preserve assertion semantics. */
#define assert(expression) ((expression) ? static_cast<void>(0) : __builtin_trap())
extern "C" void *memcpy(void *destination, const void *source, size_t size);
extern "C" void *memset(void *destination, int value, size_t size);
#else
#include <assert.h>
#include <string.h>
#endif

namespace {

enum ReductionKind { kReductionR0, kReductionR1, kReductionR2, kReductionRN, kReductionBranch };

using pcaa::pbqp_storage::Array;
using pcaa::pbqp_storage::CopyStatistics;
using pcaa::pbqp_storage::Problem;
using pcaa::pbqp_storage::Reconstruction;

pbqp_allocator_t WorkspaceAllocator(const pbqp_problem_t &problem,
                                    const pbqp_solver_config_t &config) {
  return config.workspace_allocator.allocate != nullptr ? config.workspace_allocator
                                                        : problem.allocator;
}

bool IsValidCost(const pbqp_problem_t &problem, int32_t cost) {
  const int32_t maximum = pbqp_max_finite_cost(&problem);
  return cost == ACCEL_INF || (cost >= -maximum && cost <= maximum);
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
    for (unsigned index = 0; index < state_.edge_capacity; ++index) {
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
    for (unsigned index = 0; index < state_.edge_capacity; ++index) {
      const pbqp_edge_t &edge = state_.edges[index];
      if (!edge.active || (edge.first != node && edge.second != node)) {
        continue;
      }
      if (edge_indices != nullptr && count < 2) {
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

  unsigned ActiveNodeCount() const {
    unsigned count = 0;
    for (unsigned node = 0; node < state_.node_count; ++node) {
      count += state_.nodes[node].active != 0;
    }
    return count;
  }

  unsigned ActiveEdgeCount() const {
    unsigned count = 0;
    for (unsigned index = 0; index < state_.edge_capacity; ++index) {
      count += state_.edges[index].active != 0;
    }
    return count;
  }

  unsigned MaximumDegree() const {
    unsigned maximum = 0;
    for (unsigned node = 0; node < state_.node_count; ++node) {
      if (state_.nodes[node].active) {
        const unsigned degree = NeighborCount(node, nullptr);
        if (degree > maximum) {
          maximum = degree;
        }
      }
    }
    return maximum;
  }

  unsigned UnaryElements() const {
    unsigned elements = 0;
    for (unsigned node = 0; node < state_.node_count; ++node) {
      if (state_.nodes[node].active) {
        elements += state_.nodes[node].domain;
      }
    }
    return elements;
  }

  unsigned MatrixElements() const {
    unsigned elements = 0;
    for (unsigned index = 0; index < state_.edge_capacity; ++index) {
      const pbqp_edge_t &edge = state_.edges[index];
      if (edge.active) {
        elements += state_.nodes[edge.first].domain * state_.nodes[edge.second].domain;
      }
    }
    return elements;
  }

  int SelectRnNode(pbqp_rn_policy_t policy) const {
    int selected = -1;
    unsigned selected_degree = 0;
    uint64_t selected_work = 0;
    for (unsigned node = 0; node < state_.node_count; ++node) {
      if (!state_.nodes[node].active) {
        continue;
      }
      const unsigned degree = NeighborCount(node, nullptr);
      uint64_t work = 0;
      if (policy == PBQP_RN_MIN_WORK) {
        for (unsigned index = 0; index < state_.edge_capacity; ++index) {
          const pbqp_edge_t &edge = state_.edges[index];
          if (edge.active && (edge.first == node || edge.second == node)) {
            work += state_.nodes[OtherNode(edge, node)].domain;
          }
        }
        work *= state_.nodes[node].domain;
      }
      if (selected < 0 ||
          (policy == PBQP_RN_MIN_DEGREE &&
           (degree < selected_degree ||
            (degree == selected_degree && node < unsigned(selected)))) ||
          (policy == PBQP_RN_MAX_DEGREE &&
           (degree > selected_degree ||
            (degree == selected_degree && node < unsigned(selected)))) ||
          (policy == PBQP_RN_MIN_WORK &&
           (work < selected_work || (work == selected_work &&
                                     (degree < selected_degree || (degree == selected_degree &&
                                                                   node < unsigned(selected))))))) {
        selected = static_cast<int>(node);
        selected_degree = degree;
        selected_work = work;
      }
    }
    return selected;
  }

  unsigned OtherNode(const pbqp_edge_t &edge, unsigned node) const {
    assert(edge.first == node || edge.second == node);
    return edge.first == node ? edge.second : edge.first;
  }

  int AddFillEdge(unsigned first, unsigned second) {
    assert(first < state_.node_count);
    assert(second < state_.node_count);
    assert(first != second);
    for (unsigned index = 0; index < state_.edge_capacity; ++index) {
      pbqp_edge_t &edge = state_.edges[index];
      if (edge.active) {
        continue;
      }
      int32_t *cost = edge.cost;
      const size_t stride = edge.stride;
      edge = {};
      edge.cost = cost;
      edge.stride = stride;
      memset(edge.cost, 0, stride * stride * sizeof(int32_t));
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

  int Min2Batch(const pbqp_min2_job_t *jobs, size_t count) const {
    if (api_.min2_argmin_batch != nullptr)
      return api_.min2_argmin_batch(api_.context, jobs, count);
    for (size_t index = 0; index < count; ++index) {
      if (Min2(jobs[index].a, jobs[index].b, jobs[index].result) != 0)
        return -1;
    }
    return 0;
  }

  int Min3Batch(const pbqp_min3_job_t *jobs, size_t count) const {
    if (api_.min3_argmin_batch != nullptr)
      return api_.min3_argmin_batch(api_.context, jobs, count);
    for (size_t index = 0; index < count; ++index) {
      if (Min3(jobs[index].a, jobs[index].b, jobs[index].c, jobs[index].result) != 0)
        return -1;
    }
    return 0;
  }

  int Min2Value(pbqp_vector_view_t first, pbqp_vector_view_t second, int32_t *result) const {
    if (api_.min2_value != nullptr)
      return api_.min2_value(api_.context, first, second, result);
    accel_min_argmin_result_t argmin{};
    const int status = Min2(first, second, &argmin);
    *result = argmin.value;
    return status;
  }

  int Min2ValueBatch(const pbqp_min2_value_job_t *jobs, size_t count) const {
    if (api_.min2_value_batch != nullptr)
      return api_.min2_value_batch(api_.context, jobs, count);
    for (size_t index = 0; index < count; ++index) {
      if (Min2Value(jobs[index].a, jobs[index].b, jobs[index].result) != 0)
        return -1;
    }
    return 0;
  }

  const pbqp_cost_kernel_t &Api() const {
    return api_;
  }

  void SetStatistics(pbqp_statistics_t *statistics) const {
    if (api_.set_statistics != nullptr)
      api_.set_statistics(api_.context, statistics);
  }

 private:
  const pbqp_cost_kernel_t &api_;
};

class Solver {
 public:
  Solver(const pbqp_cost_kernel_t &kernel, const pbqp_solver_config_t &config)
      : kernel_(kernel), config_(config) {}

  pbqp_status_t Solve(pbqp_problem_t *problem, pbqp_solution_t *solution) const {
    if (problem == nullptr || solution == nullptr || solution->assignment == nullptr ||
        solution->assignment_capacity < problem->node_count) {
      return PBQP_ARGUMENT_ERROR;
    }

    if (config_.strategy == PBQP_STRATEGY_HEURISTIC_RN_LOCAL_SEARCH) {
      const pbqp_allocator_t allocator = WorkspaceAllocator(*problem, config_);
      Problem original(*problem, allocator);
      Array<unsigned> rn_assignment(allocator, problem->node_count);
      if (original.Status() != PBQP_OK || rn_assignment.Get() == nullptr)
        return PBQP_CAPACITY_ERROR;
      pbqp_solver_config_t rn_config = config_;
      rn_config.strategy = PBQP_STRATEGY_HEURISTIC_RN;
      pbqp_solution_t rn_solution;
      pbqp_solution_init(&rn_solution, rn_assignment.Get(), problem->node_count);
      const pbqp_status_t rn_status = Solver(kernel_.Api(), rn_config).Solve(problem, &rn_solution);
      if (rn_status != PBQP_OK) {
        return rn_status;
      }
      CopyStatistics(&original.State().statistics, problem->statistics, problem->node_capacity,
                     problem->domain_capacity);
      kernel_.SetStatistics(&original.State().statistics);
      const pbqp_status_t local_status =
          RunLocalDescent(original.State(), rn_solution.assignment, solution);
      if (local_status == PBQP_OK) {
        assert(solution->optimum <= rn_solution.optimum);
        CopyStatistics(&problem->statistics, original.State().statistics, problem->node_capacity,
                       problem->domain_capacity);
      }
      return local_status;
    }

    Graph graph(*problem);
    pbqp_problem_t &state = graph.State();
    kernel_.SetStatistics(&state.statistics);
    state.statistics.nodes = state.node_count;
    state.statistics.initial_edges = state.edge_count;

    if (config_.strategy == PBQP_STRATEGY_LOCAL_SEARCH) {
      return SolveLocalSearch(state, solution);
    }

    int rn_episode = -1;
    const auto record_rn_cascade = [&state, &rn_episode] {
      if (rn_episode < 0)
        return;
      const unsigned length = state.statistics.rn_cascade_r0[rn_episode] +
                              state.statistics.rn_cascade_r1[rn_episode] +
                              state.statistics.rn_cascade_r2[rn_episode];
      ++state.statistics.rn_cascade_length_histogram[length];
      state.statistics.rn_cascade_total_length += length;
      if (length > state.statistics.rn_cascade_maximum_length)
        state.statistics.rn_cascade_maximum_length = length;
    };
    while (true) {
      int edge_indices[2];
      const int node = graph.FindReducibleNode(edge_indices);
      if (node < 0) {
        if (graph.ActiveNodeCount() == 0) {
          record_rn_cascade();
          solution->optimum = state.objective_offset;
          ReconstructSolution(state, solution);
          return PBQP_OK;
        }
        if (config_.strategy == PBQP_STRATEGY_REDUCE_ONLY) {
          return PBQP_IRREDUCIBLE;
        }
        if (config_.strategy == PBQP_STRATEGY_HEURISTIC_RN) {
          record_rn_cascade();
          RecordIrreducibleCore(graph);
          const int rn_node = graph.SelectRnNode(config_.rn_policy);
          if (rn_node < 0) {
            return PBQP_IRREDUCIBLE;
          }
          Emit(graph, PBQP_TRACE_RN_SELECT, PBQP_TRACE_HEURISTIC, rn_node, -1, 0, 0, 0, 0, 0, 0, 0,
               0, 0);
          const pbqp_status_t status = ReduceRN(graph, static_cast<unsigned>(rn_node));
          if (status != PBQP_OK) {
            return status;
          }
          rn_episode = static_cast<int>(state.statistics.rn_count - 1);
          continue;
        }
        break;
      }

      const unsigned degree = graph.NeighborCount(static_cast<unsigned>(node), edge_indices);
      pbqp_status_t status = PBQP_OK;
      if (degree == 0) {
        status = ReduceR0(graph, static_cast<unsigned>(node));
        if (rn_episode >= 0) {
          ++state.statistics.r0_after_rn;
          ++state.statistics.rn_cascade_r0[rn_episode];
        }
      } else if (degree == 1) {
        status = ReduceR1(graph, static_cast<unsigned>(node), edge_indices[0]);
        if (rn_episode >= 0) {
          ++state.statistics.r1_after_rn;
          ++state.statistics.rn_cascade_r1[rn_episode];
        }
      } else {
        status = ReduceR2(graph, static_cast<unsigned>(node), edge_indices[0], edge_indices[1]);
        if (rn_episode >= 0) {
          ++state.statistics.r2_after_rn;
          ++state.statistics.rn_cascade_r2[rn_episode];
        }
      }
      if (status != PBQP_OK) {
        return status;
      }
    }

    if (config_.strategy != PBQP_STRATEGY_EXACT_CORE_ENUMERATION &&
        config_.strategy != PBQP_STRATEGY_EXACT_BRANCH_REDUCE) {
      return PBQP_ARGUMENT_ERROR;
    }
    if (config_.strategy == PBQP_STRATEGY_EXACT_BRANCH_REDUCE) {
      return SolveBranchAndReduce(&state, solution, 0);
    }
    Array<unsigned> core_assignment(WorkspaceAllocator(state, config_), state.node_count);
    if (core_assignment.Get() == nullptr)
      return PBQP_CAPACITY_ERROR;
    bool has_assignment = false;
    bool search_limit_hit = false;
    solution->optimum = ACCEL_INF;
    EnumerateActiveCore(state, 0, core_assignment.Get(), solution, &has_assignment,
                        &search_limit_hit);
    if (search_limit_hit) {
      ++state.statistics.search_limit_hits;
      return PBQP_SEARCH_LIMIT;
    }
    solution->optimum = accel_cost_add(state.objective_offset, solution->optimum);
    ReconstructSolution(state, solution);
    return PBQP_OK;
  }

 private:
  void Emit(Graph &graph, pbqp_trace_event_type_t type, pbqp_trace_phase_t phase, int node,
            int choice, uint64_t project, uint64_t project_accumulate, uint64_t slice,
            uint64_t map3, uint64_t argmin, unsigned descriptors, unsigned structural,
            uint64_t operand_bytes, uint64_t result_bytes) const {
    if (config_.trace_sink == nullptr || config_.trace_sink->emit == nullptr)
      return;
    pbqp_solver_event_t event{};
    event.type = type;
    event.phase = phase;
    event.policy = config_.rn_policy;
    event.node = node;
    event.choice = choice;
    event.active_nodes = graph.ActiveNodeCount();
    event.active_edges = graph.ActiveEdgeCount();
    event.maximum_degree = graph.MaximumDegree();
    event.unary_elements = graph.UnaryElements();
    event.matrix_elements = graph.MatrixElements();
    event.r0_count = type == PBQP_TRACE_R0;
    event.r1_count = type == PBQP_TRACE_R1;
    event.r2_count = type == PBQP_TRACE_R2;
    event.rn_count = type == PBQP_TRACE_RN_COMMIT;
    event.minplus_project_elements = project;
    event.project_accumulate_elements = project_accumulate;
    event.slice_accumulate_elements = slice;
    event.map3_reduce_elements = map3;
    event.argmin_vector_elements = argmin;
    event.primitive_descriptors = descriptors;
    event.structural_operations = structural;
    event.operand_bytes = operand_bytes;
    event.result_bytes = result_bytes;
    config_.trace_sink->emit(config_.trace_sink->context, &event);
  }

  pbqp_trace_phase_t ReductionPhase() const {
    return config_.strategy == PBQP_STRATEGY_HEURISTIC_RN ? PBQP_TRACE_HEURISTIC
                                                          : PBQP_TRACE_REDUCTION;
  }

  static pbqp_vector_view_t EdgeView(const pbqp_edge_t &edge, unsigned node, unsigned other_value,
                                     unsigned length) {
    assert(edge.first == node || edge.second == node);
    assert(other_value < edge.stride);
    assert(length <= edge.stride);
    if (node == edge.first) {
      return {edge.cost + other_value, length, edge.stride};
    }
    return {edge.cost + other_value * edge.stride, length, 1};
  }

  static pbqp_vector_view_t ConditionedEdgeView(const pbqp_edge_t &edge, unsigned node,
                                                unsigned node_value, unsigned neighbor_length) {
    assert(edge.first == node || edge.second == node);
    assert(node_value < edge.stride);
    assert(neighbor_length <= edge.stride);
    if (node == edge.first) {
      return {edge.cost + node_value * edge.stride, neighbor_length, 1};
    }
    return {edge.cost + node_value, neighbor_length, edge.stride};
  }

  static void RecordView(pbqp_statistics_t *statistics, pbqp_vector_view_t view) {
    if (view.stride == 1) {
      ++statistics->contiguous_views;
    } else {
      ++statistics->strided_views;
    }
  }

  static void RecordOperation(pbqp_statistics_t *statistics, unsigned opcode, unsigned length,
                              unsigned input_count,
                              size_t result_bytes = sizeof(accel_min_argmin_result_t)) {
    ++statistics->primitive_submissions[opcode];
    ++statistics->vector_length_histogram[length];
    statistics->logical_map_elements += length;
    statistics->logical_bytes_read += static_cast<uint64_t>(length) * input_count * sizeof(int32_t);
    statistics->logical_bytes_written += result_bytes;
  }

  static void RecordMinplusProject(pbqp_statistics_t *statistics, unsigned length,
                                   unsigned result_bytes) {
    statistics->minplus_project_elements += length;
    ++statistics->minplus_project_descriptors;
    statistics->minplus_project_bytes += 2 * length * sizeof(int32_t) + result_bytes;
    statistics->operation_mix_operand_bytes += 2 * length * sizeof(int32_t);
    statistics->operation_mix_result_bytes += result_bytes;
  }

  static void RecordMap3Reduce(pbqp_statistics_t *statistics, unsigned length) {
    statistics->map3_reduce_elements += length;
    ++statistics->map3_reduce_descriptors;
    statistics->map3_reduce_bytes +=
        3 * length * sizeof(int32_t) + sizeof(accel_min_argmin_result_t);
    statistics->operation_mix_operand_bytes += 3 * length * sizeof(int32_t);
    statistics->operation_mix_result_bytes += sizeof(accel_min_argmin_result_t);
  }

  static void RecordArgminVector(pbqp_statistics_t *statistics, unsigned length) {
    statistics->argmin_vector_elements += length;
    ++statistics->argmin_vector_descriptors;
    statistics->argmin_vector_bytes +=
        2 * length * sizeof(int32_t) + sizeof(accel_min_argmin_result_t);
    statistics->operation_mix_operand_bytes += 2 * length * sizeof(int32_t);
    statistics->operation_mix_result_bytes += sizeof(accel_min_argmin_result_t);
  }

  pbqp_status_t RunLocalDescent(pbqp_problem_t &problem, unsigned *assignment,
                                pbqp_solution_t *solution) const {
    Graph graph(problem);
    const pbqp_allocator_t allocator = WorkspaceAllocator(problem, config_);
    Array<int32_t> scores_storage(allocator, problem.domain_capacity);
    Array<int32_t> zeroes_storage(allocator, problem.domain_capacity);
    if (scores_storage.Get() == nullptr || zeroes_storage.Get() == nullptr)
      return PBQP_CAPACITY_ERROR;
    int32_t *scores = scores_storage.Get();
    int32_t *zeroes = zeroes_storage.Get();
    bool changed = true;
    while (changed) {
      changed = false;
      ++problem.statistics.local_search_sweeps;
      for (unsigned node_index = 0; node_index < problem.node_count; ++node_index) {
        const pbqp_node_t &node = problem.nodes[node_index];
        for (unsigned value = 0; value < node.domain; ++value) {
          scores[value] = node.unary[value];
        }
        for (unsigned edge_index = 0; edge_index < problem.edge_capacity; ++edge_index) {
          const pbqp_edge_t &edge = problem.edges[edge_index];
          if (!edge.active || (edge.first != node_index && edge.second != node_index)) {
            continue;
          }
          const unsigned neighbor = edge.first == node_index ? edge.second : edge.first;
          const pbqp_vector_view_t slice =
              ConditionedEdgeView(edge, neighbor, assignment[neighbor], node.domain);
          for (unsigned value = 0; value < node.domain; ++value) {
            scores[value] = accel_cost_add(scores[value], slice.base[value * slice.stride]);
          }
          ++problem.statistics.local_search_slice_accumulations;
          problem.statistics.local_search_slice_elements += node.domain;
          problem.statistics.local_search_matrix_read_bytes += node.domain * sizeof(int32_t);
          problem.statistics.local_search_score_read_bytes += node.domain * sizeof(int32_t);
          problem.statistics.local_search_score_write_bytes += node.domain * sizeof(int32_t);
          problem.statistics.slice_accumulate_elements += node.domain;
          ++problem.statistics.slice_accumulate_operations;
          problem.statistics.slice_accumulate_bytes += 3 * node.domain * sizeof(int32_t);
          problem.statistics.operation_mix_operand_bytes += 2 * node.domain * sizeof(int32_t);
          problem.statistics.operation_mix_result_bytes += node.domain * sizeof(int32_t);
        }
        accel_min_argmin_result_t best{};
        const pbqp_vector_view_t score_view = {scores, node.domain, 1};
        const pbqp_vector_view_t zero_view = {zeroes, node.domain, 1};
        RecordView(&problem.statistics, score_view);
        RecordView(&problem.statistics, zero_view);
        RecordOperation(&problem.statistics, ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN, node.domain,
                        2);
        RecordArgminVector(&problem.statistics, node.domain);
        if (kernel_.Min2(score_view, zero_view, &best) != 0 || best.index >= node.domain) {
          return PBQP_ARGUMENT_ERROR;
        }
        const unsigned degree = graph.NeighborCount(node_index, nullptr);
        const uint64_t slice_elements = static_cast<uint64_t>(degree) * node.domain;
        Emit(graph, PBQP_TRACE_LOCAL_SCORE, PBQP_TRACE_LOCAL_SEARCH, static_cast<int>(node_index),
             static_cast<int>(best.index), 0, 0, slice_elements, 0, node.domain, 1, 2,
             2 * (slice_elements + node.domain) * sizeof(int32_t),
             slice_elements * sizeof(int32_t) + sizeof(accel_min_argmin_result_t));
        ++problem.statistics.local_search_node_evaluations;
        ++problem.statistics.local_search_argmin_reductions;
        problem.statistics.local_search_argmin_elements += node.domain;
        if (best.value < scores[assignment[node_index]]) {
          assignment[node_index] = best.index;
          ++problem.statistics.local_search_accepted_moves;
          changed = true;
          Emit(graph, PBQP_TRACE_LOCAL_MOVE, PBQP_TRACE_LOCAL_SEARCH, static_cast<int>(node_index),
               static_cast<int>(best.index), 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
      }
    }
    solution->optimum = pbqp_evaluate(&problem, assignment);
    for (unsigned node = 0; node < problem.node_count; ++node) {
      solution->assignment[node] = assignment[node];
    }
    return PBQP_OK;
  }

  pbqp_status_t SolveLocalSearch(pbqp_problem_t &problem, pbqp_solution_t *solution) const {
    const pbqp_allocator_t allocator = WorkspaceAllocator(problem, config_);
    Array<unsigned> initial(allocator, problem.node_count);
    Array<unsigned> best_assignment(allocator, problem.node_count);
    Array<unsigned> candidate_assignment(allocator, problem.node_count);
    if (initial.Get() == nullptr || best_assignment.Get() == nullptr ||
        candidate_assignment.Get() == nullptr)
      return PBQP_CAPACITY_ERROR;
    pbqp_solution_t best;
    pbqp_solution_init(&best, best_assignment.Get(), problem.node_count);
    if (RunLocalDescent(problem, initial.Get(), &best) != PBQP_OK) {
      return PBQP_ARGUMENT_ERROR;
    }
    for (unsigned node = 0; node < problem.node_count; ++node) {
      for (unsigned value = 1; value < problem.nodes[node].domain; ++value) {
        initial.Get()[node] = value;
        pbqp_solution_t candidate;
        pbqp_solution_init(&candidate, candidate_assignment.Get(), problem.node_count);
        const pbqp_status_t status = RunLocalDescent(problem, initial.Get(), &candidate);
        if (status != PBQP_OK) {
          return status;
        }
        if (candidate.optimum < best.optimum) {
          best.optimum = candidate.optimum;
          memcpy(best.assignment, candidate.assignment,
                 static_cast<size_t>(problem.node_count) * sizeof(unsigned));
        }
        initial.Get()[node] = 0;
      }
    }
    solution->optimum = best.optimum;
    memcpy(solution->assignment, best.assignment,
           static_cast<size_t>(problem.node_count) * sizeof(unsigned));
    return PBQP_OK;
  }

  void RecordIrreducibleCore(Graph &graph) const {
    pbqp_statistics_t &statistics = graph.State().statistics;
    const unsigned nodes = graph.ActiveNodeCount();
    const unsigned edges = graph.ActiveEdgeCount();
    if (statistics.rn_count == 0) {
      statistics.first_rn_active_nodes = nodes;
      statistics.first_rn_active_edges = edges;
    }
    if (nodes > statistics.maximum_irreducible_core_nodes) {
      statistics.maximum_irreducible_core_nodes = nodes;
    }
    if (edges > statistics.maximum_irreducible_core_edges) {
      statistics.maximum_irreducible_core_edges = edges;
    }
    ++statistics.rn_episodes;
  }

  pbqp_status_t ConditionNode(Graph &graph, unsigned node_index, unsigned choice,
                              ReductionKind kind) const {
    pbqp_problem_t &problem = graph.State();
    pbqp_node_t &node = problem.nodes[node_index];
    if (!node.active || choice >= node.domain) {
      return PBQP_ARGUMENT_ERROR;
    }
    ++problem.statistics.condition_count;
    problem.objective_offset = accel_cost_add(problem.objective_offset, node.unary[choice]);
    for (unsigned edge_index = 0; edge_index < problem.edge_capacity; ++edge_index) {
      pbqp_edge_t &edge = problem.edges[edge_index];
      if (!edge.active || (edge.first != node_index && edge.second != node_index)) {
        continue;
      }
      const unsigned neighbor_index = graph.OtherNode(edge, node_index);
      pbqp_node_t &neighbor = problem.nodes[neighbor_index];
      const pbqp_vector_view_t slice =
          ConditionedEdgeView(edge, node_index, choice, neighbor.domain);
      for (unsigned value = 0; value < neighbor.domain; ++value) {
        neighbor.unary[value] =
            accel_cost_add(neighbor.unary[value], slice.base[value * slice.stride]);
      }
      problem.statistics.condition_elements += neighbor.domain;
      problem.statistics.condition_matrix_read_bytes += neighbor.domain * sizeof(int32_t);
      problem.statistics.condition_unary_read_bytes += neighbor.domain * sizeof(int32_t);
      problem.statistics.condition_unary_write_bytes += neighbor.domain * sizeof(int32_t);
      problem.statistics.slice_accumulate_elements += neighbor.domain;
      ++problem.statistics.slice_accumulate_operations;
      problem.statistics.slice_accumulate_bytes += 3 * neighbor.domain * sizeof(int32_t);
      problem.statistics.operation_mix_operand_bytes += 2 * neighbor.domain * sizeof(int32_t);
      problem.statistics.operation_mix_result_bytes += neighbor.domain * sizeof(int32_t);
      if (kind == kReductionRN) {
        problem.statistics.rn_commit_elements += neighbor.domain;
        problem.statistics.commit_matrix_read_bytes += neighbor.domain * sizeof(int32_t);
        problem.statistics.commit_unary_read_bytes += neighbor.domain * sizeof(int32_t);
        problem.statistics.commit_unary_write_bytes += neighbor.domain * sizeof(int32_t);
        problem.statistics.rn_commit_bytes += 3 * neighbor.domain * sizeof(int32_t);
      }
      edge.active = 0;
      --problem.edge_count;
    }
    Reconstruction(problem, node_index)[0] = choice;
    node.reduction_kind = kind;
    node.active = 0;
    problem.elimination_order[problem.elimination_count++] = node_index;
    return PBQP_OK;
  }

  pbqp_status_t ReduceRN(Graph &graph, unsigned node_index) const {
    pbqp_problem_t &problem = graph.State();
    pbqp_node_t &node = problem.nodes[node_index];
    const pbqp_allocator_t allocator = WorkspaceAllocator(problem, config_);
    Array<int32_t> scores_storage(allocator, node.domain);
    Array<pbqp_min2_value_job_t> jobs_storage(allocator, node.domain);
    Array<int32_t> results_storage(allocator, node.domain);
    if (scores_storage.Get() == nullptr || jobs_storage.Get() == nullptr ||
        results_storage.Get() == nullptr)
      return PBQP_CAPACITY_ERROR;
    int32_t *scores = scores_storage.Get();
    pbqp_min2_value_job_t *jobs = jobs_storage.Get();
    int32_t *results = results_storage.Get();
    const uint64_t project_before = problem.statistics.rn_projection_map_elements;
    const uint64_t accumulate_before = problem.statistics.rn_score_accumulation_elements;
    const unsigned descriptors_before = problem.statistics.rn_projection_primitives;
    const uint64_t operand_before = problem.statistics.rn_projection_operand_bytes;
    const uint64_t result_before = problem.statistics.rn_projection_result_bytes;
    for (unsigned value = 0; value < node.domain; ++value) {
      scores[value] = node.unary[value];
    }

    const unsigned degree = graph.NeighborCount(node_index, nullptr);
    ++problem.statistics.rn_count;
    problem.statistics.rn_degree_total += degree;
    if (problem.statistics.rn_count == 1 || degree < problem.statistics.rn_degree_min) {
      problem.statistics.rn_degree_min = degree;
    }
    if (degree > problem.statistics.rn_degree_max) {
      problem.statistics.rn_degree_max = degree;
    }
    ++problem.statistics.rn_degree_histogram[degree];

    for (unsigned edge_index = 0; edge_index < problem.edge_capacity; ++edge_index) {
      const pbqp_edge_t &edge = problem.edges[edge_index];
      if (!edge.active || (edge.first != node_index && edge.second != node_index)) {
        continue;
      }
      const unsigned neighbor_index = graph.OtherNode(edge, node_index);
      const pbqp_node_t &neighbor = problem.nodes[neighbor_index];
      for (unsigned value = 0; value < node.domain; ++value) {
        const pbqp_vector_view_t matrix_slice =
            ConditionedEdgeView(edge, node_index, value, neighbor.domain);
        const pbqp_vector_view_t unary = {neighbor.unary, neighbor.domain, 1};
        jobs[value] = {matrix_slice, unary, &results[value]};
        RecordView(&problem.statistics, matrix_slice);
        RecordView(&problem.statistics, unary);
        RecordOperation(&problem.statistics, ACCEL_OPCODE_MAP_ADD_REDUCE_MIN, neighbor.domain, 2,
                        sizeof(int32_t));
        RecordMinplusProject(&problem.statistics, neighbor.domain, sizeof(int32_t));
        ++problem.statistics.rn_projection_primitives;
        problem.statistics.rn_projection_map_elements += neighbor.domain;
        problem.statistics.rn_projection_operand_bytes += 2 * neighbor.domain * sizeof(int32_t);
        problem.statistics.rn_projection_result_bytes += sizeof(int32_t);
      }
      ++problem.statistics.rn_projection_count;
      if (kernel_.Min2ValueBatch(jobs, node.domain) != 0) {
        return PBQP_ARGUMENT_ERROR;
      }
      for (unsigned value = 0; value < node.domain; ++value) {
        scores[value] = accel_cost_add(scores[value], results[value]);
      }
      problem.statistics.rn_score_accumulation_elements += node.domain;
      problem.statistics.project_accumulate_elements += node.domain;
      problem.statistics.project_accumulate_bytes += 3 * node.domain * sizeof(int32_t);
      problem.statistics.operation_mix_operand_bytes += 2 * node.domain * sizeof(int32_t);
      problem.statistics.operation_mix_result_bytes += node.domain * sizeof(int32_t);
    }

    unsigned choice = 0;
    for (unsigned value = 1; value < node.domain; ++value) {
      if (scores[value] < scores[choice]) {
        choice = value;
      }
    }
    const uint64_t project_elements =
        problem.statistics.rn_projection_map_elements - project_before;
    const uint64_t project_accumulate =
        problem.statistics.rn_score_accumulation_elements - accumulate_before;
    const unsigned descriptors = problem.statistics.rn_projection_primitives - descriptors_before;
    const uint64_t operand_bytes = problem.statistics.rn_projection_operand_bytes - operand_before +
                                   2 * project_accumulate * sizeof(int32_t);
    const uint64_t result_bytes = problem.statistics.rn_projection_result_bytes - result_before +
                                  project_accumulate * sizeof(int32_t);
    Emit(graph, PBQP_TRACE_RN_SCORE, PBQP_TRACE_HEURISTIC, static_cast<int>(node_index),
         static_cast<int>(choice), project_elements, project_accumulate, 0, 0, 0, descriptors, 2,
         operand_bytes, result_bytes);
    const unsigned rn_index = problem.statistics.rn_count - 1;
    problem.statistics.rn_nodes[rn_index] = node_index;
    problem.statistics.rn_choices[rn_index] = choice;
    const unsigned before_elements = problem.statistics.rn_commit_elements;
    const pbqp_status_t status = ConditionNode(graph, node_index, choice, kReductionRN);
    if (status == PBQP_OK) {
      const uint64_t elements = problem.statistics.rn_commit_elements - before_elements;
      Emit(graph, PBQP_TRACE_RN_COMMIT, PBQP_TRACE_HEURISTIC, static_cast<int>(node_index),
           static_cast<int>(choice), 0, 0, elements, 0, 0, 0, 1, 2 * elements * sizeof(int32_t),
           elements * sizeof(int32_t));
    }
    return status;
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
    Reconstruction(problem, node_index)[0] = choice;
    node.reduction_kind = kReductionR0;
    node.active = 0;
    ++problem.statistics.r0_count;
    problem.elimination_order[problem.elimination_count++] = node_index;
    Emit(graph, PBQP_TRACE_R0, ReductionPhase(), static_cast<int>(node_index), choice, 0, 0, 0, 0,
         0, 0, 0, 0, 0);
    return PBQP_OK;
  }

  pbqp_status_t ReduceR1(Graph &graph, unsigned node_index, int edge_index) const {
    pbqp_problem_t &problem = graph.State();
    pbqp_edge_t &edge = problem.edges[edge_index];
    const unsigned neighbor_index = graph.OtherNode(edge, node_index);
    pbqp_node_t &node = problem.nodes[node_index];
    pbqp_node_t &neighbor = problem.nodes[neighbor_index];
    const pbqp_allocator_t allocator = WorkspaceAllocator(problem, config_);
    Array<pbqp_min2_job_t> jobs_storage(allocator, neighbor.domain);
    Array<accel_min_argmin_result_t> results_storage(allocator, neighbor.domain);
    if (jobs_storage.Get() == nullptr || results_storage.Get() == nullptr)
      return PBQP_CAPACITY_ERROR;
    pbqp_min2_job_t *jobs = jobs_storage.Get();
    accel_min_argmin_result_t *results = results_storage.Get();

    for (unsigned neighbor_value = 0; neighbor_value < neighbor.domain; ++neighbor_value) {
      const pbqp_vector_view_t unary = {node.unary, node.domain, 1};
      const pbqp_vector_view_t edge_cost = EdgeView(edge, node_index, neighbor_value, node.domain);
      RecordView(&problem.statistics, unary);
      RecordView(&problem.statistics, edge_cost);
      RecordOperation(&problem.statistics, ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN, node.domain, 2);
      RecordMinplusProject(&problem.statistics, node.domain, sizeof(accel_min_argmin_result_t));
      jobs[neighbor_value] = {unary, edge_cost, &results[neighbor_value]};
    }
    if (kernel_.Min2Batch(jobs, neighbor.domain) != 0)
      return PBQP_ARGUMENT_ERROR;
    for (unsigned neighbor_value = 0; neighbor_value < neighbor.domain; ++neighbor_value) {
      const accel_min_argmin_result_t result = results[neighbor_value];
      if (result.index >= node.domain) {
        return PBQP_ARGUMENT_ERROR;
      }
      assert(result.index < node.domain);
      neighbor.unary[neighbor_value] = accel_cost_add(neighbor.unary[neighbor_value], result.value);
      Reconstruction(problem, node_index)[neighbor_value] = result.index;
    }

    edge.active = 0;
    --problem.edge_count;
    node.first_neighbor = static_cast<int>(neighbor_index);
    node.reduction_kind = kReductionR1;
    node.active = 0;
    ++problem.statistics.r1_count;
    problem.elimination_order[problem.elimination_count++] = node_index;
    const uint64_t elements = static_cast<uint64_t>(node.domain) * neighbor.domain;
    Emit(graph, PBQP_TRACE_R1, ReductionPhase(), static_cast<int>(node_index), -1, elements, 0, 0,
         0, 0, neighbor.domain, 1, 2 * elements * sizeof(int32_t),
         neighbor.domain * sizeof(accel_min_argmin_result_t));
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
    const size_t maximum_jobs = static_cast<size_t>(first.domain) * second.domain;
    const pbqp_allocator_t allocator = WorkspaceAllocator(problem, config_);
    Array<pbqp_min3_job_t> jobs_storage(allocator, maximum_jobs);
    Array<accel_min_argmin_result_t> results_storage(allocator, maximum_jobs);
    if (jobs_storage.Get() == nullptr || results_storage.Get() == nullptr)
      return PBQP_CAPACITY_ERROR;
    pbqp_min3_job_t *jobs = jobs_storage.Get();
    accel_min_argmin_result_t *results = results_storage.Get();

    unsigned job_count = 0;
    for (unsigned first_value = 0; first_value < first.domain; ++first_value) {
      for (unsigned second_value = 0; second_value < second.domain; ++second_value) {
        const pbqp_vector_view_t unary = {node.unary, node.domain, 1};
        const pbqp_vector_view_t first_cost =
            EdgeView(first_edge, node_index, first_value, node.domain);
        const pbqp_vector_view_t second_cost =
            EdgeView(second_edge, node_index, second_value, node.domain);
        RecordView(&problem.statistics, unary);
        RecordView(&problem.statistics, first_cost);
        RecordView(&problem.statistics, second_cost);
        RecordOperation(&problem.statistics, ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN, node.domain,
                        3);
        RecordMap3Reduce(&problem.statistics, node.domain);
        jobs[job_count] = {unary, first_cost, second_cost, &results[job_count]};
        ++job_count;
      }
    }
    if (kernel_.Min3Batch(jobs, job_count) != 0)
      return PBQP_ARGUMENT_ERROR;
    job_count = 0;
    for (unsigned first_value = 0; first_value < first.domain; ++first_value) {
      for (unsigned second_value = 0; second_value < second.domain; ++second_value) {
        const accel_min_argmin_result_t result = results[job_count++];
        if (result.index >= node.domain) {
          return PBQP_ARGUMENT_ERROR;
        }
        assert(result.index < node.domain);

        Reconstruction(problem, node_index)[first_value * second.domain + second_value] =
            result.index;
        const unsigned fill_first_value =
            fill_edge.first == first_neighbor ? first_value : second_value;
        const unsigned fill_second_value =
            fill_edge.second == second_neighbor ? second_value : first_value;
        const size_t fill_index = fill_first_value * fill_edge.stride + fill_second_value;
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
    const unsigned descriptors = first.domain * second.domain;
    const uint64_t elements = static_cast<uint64_t>(descriptors) * node.domain;
    Emit(graph, PBQP_TRACE_R2, ReductionPhase(), static_cast<int>(node_index), -1, 0, 0, 0,
         elements, 0, descriptors, 1, 3 * elements * sizeof(int32_t),
         descriptors * sizeof(accel_min_argmin_result_t));
    return PBQP_OK;
  }

  static int32_t EvaluateActiveCore(const pbqp_problem_t &problem, const unsigned *assignment) {
    int32_t total = 0;
    for (unsigned node = 0; node < problem.node_count; ++node) {
      if (problem.nodes[node].active) {
        total = accel_cost_add(total, problem.nodes[node].unary[assignment[node]]);
      }
    }
    for (unsigned index = 0; index < problem.edge_capacity; ++index) {
      const pbqp_edge_t &edge = problem.edges[index];
      if (edge.active) {
        total = accel_cost_add(
            total, edge.cost[assignment[edge.first] * edge.stride + assignment[edge.second]]);
      }
    }
    return total;
  }

  void EnumerateActiveCore(pbqp_problem_t &problem, unsigned node, unsigned *assignment,
                           pbqp_solution_t *solution, bool *has_assignment,
                           bool *search_limit_hit) const {
    if (config_.maximum_search_nodes != 0 &&
        problem.statistics.search_nodes_visited == config_.maximum_search_nodes) {
      *search_limit_hit = true;
      return;
    }
    ++problem.statistics.search_nodes_visited;
    if (node > problem.statistics.search_maximum_depth) {
      problem.statistics.search_maximum_depth = node;
    }
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
      EnumerateActiveCore(problem, node + 1, assignment, solution, has_assignment,
                          search_limit_hit);
      return;
    }
    for (unsigned value = 0; value < problem.nodes[node].domain; ++value) {
      ++problem.statistics.search_branches_created;
      assignment[node] = value;
      EnumerateActiveCore(problem, node + 1, assignment, solution, has_assignment,
                          search_limit_hit);
      if (*search_limit_hit) {
        return;
      }
    }
  }

  static void AddBranchStatistics(pbqp_statistics_t *total, const pbqp_statistics_t &base,
                                  const pbqp_statistics_t &branch, unsigned domain_capacity) {
    total->r0_count += branch.r0_count - base.r0_count;
    total->r1_count += branch.r1_count - base.r1_count;
    total->r2_count += branch.r2_count - base.r2_count;
    total->condition_count += branch.condition_count - base.condition_count;
    total->condition_elements += branch.condition_elements - base.condition_elements;
    total->condition_matrix_read_bytes +=
        branch.condition_matrix_read_bytes - base.condition_matrix_read_bytes;
    total->condition_unary_read_bytes +=
        branch.condition_unary_read_bytes - base.condition_unary_read_bytes;
    total->condition_unary_write_bytes +=
        branch.condition_unary_write_bytes - base.condition_unary_write_bytes;
    total->rn_commit_elements += branch.rn_commit_elements - base.rn_commit_elements;
    total->rn_commit_bytes += branch.rn_commit_bytes - base.rn_commit_bytes;
    total->commit_matrix_read_bytes +=
        branch.commit_matrix_read_bytes - base.commit_matrix_read_bytes;
    total->commit_unary_read_bytes += branch.commit_unary_read_bytes - base.commit_unary_read_bytes;
    total->commit_unary_write_bytes +=
        branch.commit_unary_write_bytes - base.commit_unary_write_bytes;
    total->logical_map_elements += branch.logical_map_elements - base.logical_map_elements;
    total->logical_bytes_read += branch.logical_bytes_read - base.logical_bytes_read;
    total->logical_bytes_written += branch.logical_bytes_written - base.logical_bytes_written;
    total->minplus_project_elements +=
        branch.minplus_project_elements - base.minplus_project_elements;
    total->minplus_project_descriptors +=
        branch.minplus_project_descriptors - base.minplus_project_descriptors;
    total->minplus_project_bytes += branch.minplus_project_bytes - base.minplus_project_bytes;
    total->project_accumulate_elements +=
        branch.project_accumulate_elements - base.project_accumulate_elements;
    total->project_accumulate_bytes +=
        branch.project_accumulate_bytes - base.project_accumulate_bytes;
    total->slice_accumulate_elements +=
        branch.slice_accumulate_elements - base.slice_accumulate_elements;
    total->slice_accumulate_operations +=
        branch.slice_accumulate_operations - base.slice_accumulate_operations;
    total->slice_accumulate_bytes += branch.slice_accumulate_bytes - base.slice_accumulate_bytes;
    total->map3_reduce_elements += branch.map3_reduce_elements - base.map3_reduce_elements;
    total->map3_reduce_descriptors += branch.map3_reduce_descriptors - base.map3_reduce_descriptors;
    total->map3_reduce_bytes += branch.map3_reduce_bytes - base.map3_reduce_bytes;
    total->argmin_vector_elements += branch.argmin_vector_elements - base.argmin_vector_elements;
    total->argmin_vector_descriptors +=
        branch.argmin_vector_descriptors - base.argmin_vector_descriptors;
    total->argmin_vector_bytes += branch.argmin_vector_bytes - base.argmin_vector_bytes;
    total->operation_mix_operand_bytes +=
        branch.operation_mix_operand_bytes - base.operation_mix_operand_bytes;
    total->operation_mix_result_bytes +=
        branch.operation_mix_result_bytes - base.operation_mix_result_bytes;
    total->contiguous_views += branch.contiguous_views - base.contiguous_views;
    total->strided_views += branch.strided_views - base.strided_views;
    total->scratch_packs += branch.scratch_packs - base.scratch_packs;
    total->scratch_bytes += branch.scratch_bytes - base.scratch_bytes;
    total->top_level_submissions += branch.top_level_submissions - base.top_level_submissions;
    total->batch_submissions += branch.batch_submissions - base.batch_submissions;
    total->batch_primitive_descriptors +=
        branch.batch_primitive_descriptors - base.batch_primitive_descriptors;
    if (branch.maximum_batch_size > total->maximum_batch_size)
      total->maximum_batch_size = branch.maximum_batch_size;
    total->batch_descriptor_bytes += branch.batch_descriptor_bytes - base.batch_descriptor_bytes;
    total->batch_child_descriptor_bytes +=
        branch.batch_child_descriptor_bytes - base.batch_child_descriptor_bytes;
    total->unique_packed_views += branch.unique_packed_views - base.unique_packed_views;
    for (unsigned opcode = 0; opcode < 5; ++opcode)
      total->primitive_submissions[opcode] +=
          branch.primitive_submissions[opcode] - base.primitive_submissions[opcode];
    for (unsigned length = 0; length <= domain_capacity; ++length)
      total->vector_length_histogram[length] +=
          branch.vector_length_histogram[length] - base.vector_length_histogram[length];
  }

  struct SearchControl {
    uint64_t nodes_visited = 0;
    uint64_t branches_created = 0;
    unsigned maximum_depth = 0;
    unsigned limit_hits = 0;
    unsigned nodes_pruned = 0;
  };

  static void RecordSearchStatistics(pbqp_statistics_t *statistics, const SearchControl &search) {
    statistics->search_nodes_visited = search.nodes_visited;
    statistics->search_branches_created = search.branches_created;
    statistics->search_maximum_depth = search.maximum_depth;
    statistics->search_limit_hits = search.limit_hits;
    statistics->search_nodes_pruned = search.nodes_pruned;
  }

  // A cheap, sign-agnostic relaxation lower bound for the still-active part
  // of `state`: the sum of each active node's own cheapest unary choice plus
  // each active edge's cheapest matrix entry. For any real assignment, each
  // node/edge contributes at least its own minimum, so this never
  // overestimates the true remaining cost -- unlike a bound that assumed
  // non-negative costs, which PBQP does not guarantee (see pbqp_max_finite_cost).
  static int32_t RemainingCoreLowerBound(const pbqp_problem_t &state) {
    int32_t bound = 0;
    for (unsigned index = 0; index < state.node_capacity; ++index) {
      const pbqp_node_t &node = state.nodes[index];
      if (!node.active)
        continue;
      int32_t minimum = node.unary[0];
      for (unsigned choice = 1; choice < node.domain; ++choice) {
        if (node.unary[choice] < minimum)
          minimum = node.unary[choice];
      }
      bound = accel_cost_add(bound, minimum);
    }
    for (unsigned index = 0; index < state.edge_capacity; ++index) {
      const pbqp_edge_t &edge = state.edges[index];
      if (!edge.active)
        continue;
      // Row-major with a fixed problem-wide stride (pbqp_storage.cpp sets
      // stride to domain_capacity, not this edge's own second-node domain),
      // so only the first `second.domain` columns of each row are this
      // edge's actual cost entries; the rest is unrelated padding.
      const unsigned rows = state.nodes[edge.first].domain;
      const unsigned cols = state.nodes[edge.second].domain;
      int32_t minimum = edge.cost[0];
      for (unsigned row = 0; row < rows; ++row) {
        for (unsigned col = 0; col < cols; ++col) {
          const int32_t value = edge.cost[static_cast<size_t>(row) * edge.stride + col];
          if (value < minimum)
            minimum = value;
        }
      }
      bound = accel_cost_add(bound, minimum);
    }
    return bound;
  }

  pbqp_status_t SolveBranchAndReduce(pbqp_problem_t *state, pbqp_solution_t *solution,
                                     unsigned depth, SearchControl *search = nullptr,
                                     int32_t best_known = ACCEL_INF) const {
    kernel_.SetStatistics(&state->statistics);
    SearchControl root_search;
    if (search == nullptr) {
      search = &root_search;
    }
    if (config_.maximum_search_nodes != 0 &&
        search->nodes_visited >= config_.maximum_search_nodes) {
      ++search->limit_hits;
      RecordSearchStatistics(&state->statistics, *search);
      return PBQP_SEARCH_LIMIT;
    }
    ++search->nodes_visited;
    if (depth > search->maximum_depth)
      search->maximum_depth = depth;
    Graph graph(*state);
    while (true) {
      int edges[2];
      const int node = graph.FindReducibleNode(edges);
      if (node < 0)
        break;
      const unsigned degree = graph.NeighborCount(static_cast<unsigned>(node), edges);
      const pbqp_status_t status =
          degree == 0   ? ReduceR0(graph, static_cast<unsigned>(node))
          : degree == 1 ? ReduceR1(graph, static_cast<unsigned>(node), edges[0])
                        : ReduceR2(graph, static_cast<unsigned>(node), edges[0], edges[1]);
      if (status != PBQP_OK)
        return status;
    }
    // Bound-and-prune: state->objective_offset is the cost already locked in
    // by reductions on this path (root down to here), and
    // RemainingCoreLowerBound is a valid lower bound on whatever the
    // still-active graph can contribute (see that function's comment on why
    // it stays valid despite PBQP allowing negative costs). If even that
    // optimistic total cannot beat the caller's incumbent, there is nothing
    // left worth exploring on this path, including the graph.ActiveNodeCount()
    // == 0 case just below (its bound is exactly objective_offset).
    if (best_known != ACCEL_INF &&
        accel_cost_add(state->objective_offset, RemainingCoreLowerBound(*state)) >= best_known) {
      ++search->nodes_pruned;
      RecordSearchStatistics(&state->statistics, *search);
      return PBQP_PRUNED;
    }
    if (graph.ActiveNodeCount() == 0) {
      solution->optimum = state->objective_offset;
      ReconstructSolution(*state, solution);
      RecordSearchStatistics(&state->statistics, *search);
      return PBQP_OK;
    }
    const int branch_node = graph.SelectRnNode(config_.rn_policy);
    if (branch_node < 0)
      return PBQP_IRREDUCIBLE;
    Emit(graph, PBQP_TRACE_BRANCH_SELECT, PBQP_TRACE_EXACT_SEARCH, branch_node, -1, 0, 0, 0, 0, 0,
         0, 0, 0, 0);
    const pbqp_allocator_t allocator = WorkspaceAllocator(*state, config_);
    Array<unsigned> best_assignment(allocator, state->node_count);
    if (best_assignment.Get() == nullptr) {
      ++search->limit_hits;
      RecordSearchStatistics(&state->statistics, *search);
      return PBQP_SEARCH_LIMIT;
    }
    pbqp_solution_t best;
    pbqp_solution_init(&best, best_assignment.Get(), state->node_count);
    bool has_best = false;
    const unsigned domain = state->nodes[branch_node].domain;
    const pbqp_statistics_t base_statistics = state->statistics;
    pbqp_statistics_t aggregate_statistics = base_statistics;
    for (unsigned value = 0; value < domain; ++value) {
      Problem child_storage(*state, allocator);
      if (child_storage.Status() != PBQP_OK) {
        ++search->limit_hits;
        RecordSearchStatistics(&state->statistics, *search);
        return PBQP_SEARCH_LIMIT;
      }
      pbqp_problem_t &child = child_storage.State();
      CopyStatistics(&child.statistics, base_statistics, child.node_capacity,
                     child.domain_capacity);
      Graph child_graph(child);
      unsigned condition_elements = 0;
      for (unsigned edge = 0; edge < child.edge_capacity; ++edge) {
        const pbqp_edge_t &candidate = child.edges[edge];
        if (candidate.active && (candidate.first == static_cast<unsigned>(branch_node) ||
                                 candidate.second == static_cast<unsigned>(branch_node))) {
          condition_elements +=
              child.nodes[child_graph.OtherNode(candidate, static_cast<unsigned>(branch_node))]
                  .domain;
        }
      }
      const pbqp_status_t condition =
          ConditionNode(child_graph, static_cast<unsigned>(branch_node), value, kReductionBranch);
      if (condition != PBQP_OK)
        return condition;
      Emit(child_graph, PBQP_TRACE_BRANCH_CONDITION, PBQP_TRACE_EXACT_SEARCH, branch_node,
           static_cast<int>(value), 0, 0, condition_elements, 0, 0, 0, 1,
           2 * condition_elements * sizeof(int32_t), condition_elements * sizeof(int32_t));
      Array<unsigned> candidate_assignment(allocator, state->node_count);
      if (candidate_assignment.Get() == nullptr) {
        ++search->limit_hits;
        RecordSearchStatistics(&state->statistics, *search);
        return PBQP_SEARCH_LIMIT;
      }
      pbqp_solution_t candidate;
      pbqp_solution_init(&candidate, candidate_assignment.Get(), state->node_count);
      ++search->branches_created;
      // Use whichever incumbent is tighter: one found among this node's own
      // earlier children, or one this whole call was already given by its
      // caller (found in an unrelated part of the tree). Both are valid
      // global upper bounds, so the smaller one prunes at least as much.
      int32_t child_best_known = has_best ? best.optimum : ACCEL_INF;
      if (best_known < child_best_known)
        child_best_known = best_known;
      const pbqp_status_t status =
          SolveBranchAndReduce(&child, &candidate, depth + 1, search, child_best_known);
      if (status != PBQP_OK && status != PBQP_PRUNED) {
        RecordSearchStatistics(&state->statistics, *search);
        return status;
      }
      AddBranchStatistics(&aggregate_statistics, base_statistics, child.statistics,
                          child.domain_capacity);
      if (status == PBQP_OK && (!has_best || candidate.optimum < best.optimum)) {
        best.optimum = candidate.optimum;
        memcpy(best.assignment, candidate.assignment,
               static_cast<size_t>(state->node_count) * sizeof(unsigned));
        has_best = true;
      }
    }
    state->statistics = aggregate_statistics;
    if (!has_best) {
      // Every branch value was pruned against the caller's incumbent: this
      // whole subtree cannot improve on it either, so propagate the same
      // signal up rather than reporting a solution that was never computed.
      RecordSearchStatistics(&state->statistics, *search);
      return PBQP_PRUNED;
    }
    RecordSearchStatistics(&state->statistics, *search);
    solution->optimum = best.optimum;
    memcpy(solution->assignment, best.assignment,
           static_cast<size_t>(state->node_count) * sizeof(unsigned));
    return PBQP_OK;
  }

  static void ReconstructSolution(const pbqp_problem_t &problem, pbqp_solution_t *solution) {
    for (unsigned position = problem.elimination_count; position > 0; --position) {
      const unsigned node_index = problem.elimination_order[position - 1];
      const pbqp_node_t &node = problem.nodes[node_index];
      const unsigned *choice = Reconstruction(problem, node_index);
      if (node.reduction_kind == kReductionR0 || node.reduction_kind == kReductionRN ||
          node.reduction_kind == kReductionBranch) {
        solution->assignment[node_index] = choice[0];
      } else if (node.reduction_kind == kReductionR1) {
        assert(node.first_neighbor >= 0);
        assert(solution->assignment[node.first_neighbor] <
               problem.nodes[node.first_neighbor].domain);
        solution->assignment[node_index] = choice[solution->assignment[node.first_neighbor]];
      } else {
        assert(node.first_neighbor >= 0);
        assert(node.second_neighbor >= 0);
        const unsigned first_value = solution->assignment[node.first_neighbor];
        const unsigned second_value = solution->assignment[node.second_neighbor];
        assert(first_value < problem.nodes[node.first_neighbor].domain);
        assert(second_value < problem.nodes[node.second_neighbor].domain);
        const unsigned choice_index =
            first_value * problem.nodes[node.second_neighbor].domain + second_value;
        solution->assignment[node_index] = choice[choice_index];
      }
    }
  }

  const CostKernel kernel_;
  const pbqp_solver_config_t config_;
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

int SoftwareMin2Batch(void *context, const pbqp_min2_job_t *jobs, size_t count) {
  for (size_t index = 0; index < count; ++index) {
    if (SoftwareMin2(context, jobs[index].a, jobs[index].b, jobs[index].result) != 0)
      return -1;
  }
  return 0;
}

int SoftwareMin2Value(void *, pbqp_vector_view_t first, pbqp_vector_view_t second,
                      int32_t *result) {
  *result = ACCEL_INF;
  for (size_t index = 0; index < first.length; ++index) {
    const int32_t value =
        accel_cost_add(first.base[index * first.stride], second.base[index * second.stride]);
    if (index == 0 || value < *result)
      *result = value;
  }
  return 0;
}

int SoftwareMin2ValueBatch(void *context, const pbqp_min2_value_job_t *jobs, size_t count) {
  for (size_t index = 0; index < count; ++index) {
    if (SoftwareMin2Value(context, jobs[index].a, jobs[index].b, jobs[index].result) != 0)
      return -1;
  }
  return 0;
}

int SoftwareMin3Batch(void *context, const pbqp_min3_job_t *jobs, size_t count) {
  for (size_t index = 0; index < count; ++index) {
    if (SoftwareMin3(context, jobs[index].a, jobs[index].b, jobs[index].c, jobs[index].result) != 0)
      return -1;
  }
  return 0;
}

}  // namespace

extern "C" {

pbqp_status_t pbqp_add_node(pbqp_problem_t *problem, unsigned domain, const int32_t *unary) {
  if (problem == nullptr || unary == nullptr || domain == 0 || domain > problem->domain_capacity) {
    return PBQP_ARGUMENT_ERROR;
  }
  if (problem->node_count == problem->node_capacity) {
    return PBQP_CAPACITY_ERROR;
  }

  for (unsigned index = 0; index < domain; ++index) {
    if (!IsValidCost(*problem, unary[index])) {
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
  for (unsigned index = 0; index < problem->edge_capacity; ++index) {
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
      if (!IsValidCost(*problem, costs[first_value * second_domain + second_value])) {
        return PBQP_COST_RANGE_ERROR;
      }
    }
  }

  for (unsigned index = 0; index < problem->edge_capacity; ++index) {
    pbqp_edge_t &edge = problem->edges[index];
    if (edge.active) {
      continue;
    }
    edge.active = 1;
    edge.first = first;
    edge.second = second;
    for (unsigned first_value = 0; first_value < first_domain; ++first_value) {
      for (unsigned second_value = 0; second_value < second_domain; ++second_value) {
        edge.cost[first_value * edge.stride + second_value] =
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
  for (unsigned index = 0; index < problem->edge_capacity; ++index) {
    const pbqp_edge_t &edge = problem->edges[index];
    if (edge.active) {
      total = accel_cost_add(
          total, edge.cost[assignment[edge.first] * edge.stride + assignment[edge.second]]);
    }
  }
  return total;
}

int32_t pbqp_max_finite_cost(const pbqp_problem_t *problem) {
  if (problem == nullptr)
    return 0;
  const uint64_t terms = static_cast<uint64_t>(problem->node_capacity) + problem->edge_capacity;
  if (terms == 0)
    return 0;
  return static_cast<int32_t>((ACCEL_INF - 1) / terms);
}

pbqp_status_t pbqp_bruteforce(const pbqp_problem_t *problem, pbqp_solution_t *solution) {
  if (problem == nullptr || solution == nullptr || solution->assignment == nullptr ||
      solution->assignment_capacity < problem->node_count) {
    return PBQP_ARGUMENT_ERROR;
  }
  Array<unsigned> assignment(problem->allocator, problem->node_count);
  if (assignment.Get() == nullptr)
    return PBQP_CAPACITY_ERROR;
  bool has_assignment = false;
  solution->optimum = ACCEL_INF;
  Enumerate(*problem, 0, assignment.Get(), solution, &has_assignment);
  return PBQP_OK;
}

void pbqp_make_software_kernel(pbqp_cost_kernel_t *kernel, pbqp_statistics_t *statistics) {
  kernel->context = statistics;
  kernel->min2_argmin = SoftwareMin2;
  kernel->min3_argmin = SoftwareMin3;
  kernel->min2_argmin_batch = SoftwareMin2Batch;
  kernel->min3_argmin_batch = SoftwareMin3Batch;
  kernel->min2_value = SoftwareMin2Value;
  kernel->min2_value_batch = SoftwareMin2ValueBatch;
  kernel->set_statistics = nullptr;
}

pbqp_solver_config_t pbqp_solver_default_config(void) {
  return {PBQP_STRATEGY_EXACT_CORE_ENUMERATION, PBQP_RN_MIN_DEGREE, 0, nullptr, {}};
}

pbqp_status_t pbqp_solver_create(pbqp_solver_t *solver, pbqp_mode_t mode,
                                 const pbqp_cost_kernel_t *kernel) {
  const pbqp_solver_config_t config = pbqp_solver_default_config();
  return pbqp_solver_create_with_config(solver, mode, kernel, &config);
}

pbqp_status_t pbqp_solver_create_with_config(pbqp_solver_t *solver, pbqp_mode_t mode,
                                             const pbqp_cost_kernel_t *kernel,
                                             const pbqp_solver_config_t *config) {
  if (solver == nullptr || kernel == nullptr || kernel->min2_argmin == nullptr ||
      kernel->min3_argmin == nullptr || config == nullptr ||
      ((config->workspace_allocator.allocate == nullptr) !=
       (config->workspace_allocator.deallocate == nullptr))) {
    return PBQP_ARGUMENT_ERROR;
  }
  solver->mode = mode;
  solver->kernel = *kernel;
  solver->config = *config;
  return PBQP_OK;
}

pbqp_status_t pbqp_solver_solve(pbqp_solver_t *solver, pbqp_problem_t *problem,
                                pbqp_solution_t *solution) {
  if (solver == nullptr) {
    return PBQP_ARGUMENT_ERROR;
  }
  return Solver(solver->kernel, solver->config).Solve(problem, solution);
}

}  // extern "C"
