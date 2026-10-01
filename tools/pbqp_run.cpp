// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Loads a small PBQP text graph and solves it through the SystemC PCAA model.

#include "pbqp_model_kernel.h"
#include "accel_protocol.h"
#include "pbqp/pbqp.h"
#include "pcaa.h"
#include "pcaa_host_error.h"
#include "timing_model.h"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <sysc/kernel/sc_externs.h>
#include <sysc/kernel/sc_simcontext.h>
#include <sysc/kernel/sc_time.h>

namespace {

constexpr size_t kMaximumHostDomain = 64 * 1024;
constexpr unsigned kTimedRunnerLanes = 4;
constexpr unsigned kTimedRunnerBytesPerCycle = 16;
constexpr int kTimedRunnerCyclePeriodNanoseconds = 1;
constexpr size_t kLegacyDescriptorBytes = 80;

struct InputNode {
  std::vector<int32_t> unary;
};

struct InputEdge {
  size_t first;
  size_t second;
  std::vector<int32_t> costs;
};

struct InputProblem {
  std::vector<InputNode> nodes;
  std::vector<InputEdge> edges;
};

struct RunnerSolution {
  int32_t optimum = ACCEL_INF;
  std::vector<unsigned> assignment;
  bool exact = false;
};

const char *trace_type_name(pbqp_trace_event_type_t type) {
  static const char *const names[] = {
      "R0",        "R1",          "R2",         "RN_SELECT",     "RN_SCORE",
      "RN_COMMIT", "LOCAL_SCORE", "LOCAL_MOVE", "BRANCH_SELECT", "BRANCH_CONDITION"};
  return names[type];
}

const char *trace_phase_name(pbqp_trace_phase_t phase) {
  static const char *const names[] = {"reduction", "heuristic", "local-search", "exact-search"};
  return names[phase];
}

const char *trace_policy_name(pbqp_rn_policy_t policy) {
  static const char *const names[] = {"min-degree", "max-degree", "min-work"};
  return names[policy];
}

struct TraceWriter {
  std::ofstream output;
  unsigned sequence = 0;

  static void emit(void *opaque, const pbqp_solver_event_t *event) {
    TraceWriter *writer = static_cast<TraceWriter *>(opaque);
    writer->output << "{\"sequence\":" << writer->sequence++ << ",\"phase\":\""
                   << trace_phase_name(event->phase) << "\",\"type\":\""
                   << trace_type_name(event->type) << "\",\"policy\":\""
                   << trace_policy_name(event->policy) << "\",\"node\":" << event->node
                   << ",\"choice\":" << event->choice << ",\"active_nodes\":" << event->active_nodes
                   << ",\"active_edges\":" << event->active_edges
                   << ",\"maximum_degree\":" << event->maximum_degree
                   << ",\"unary_elements\":" << event->unary_elements
                   << ",\"matrix_elements\":" << event->matrix_elements
                   << ",\"r0\":" << event->r0_count << ",\"r1\":" << event->r1_count
                   << ",\"r2\":" << event->r2_count << ",\"rn\":" << event->rn_count
                   << ",\"project\":" << event->minplus_project_elements
                   << ",\"project_accumulate\":" << event->project_accumulate_elements
                   << ",\"slice\":" << event->slice_accumulate_elements
                   << ",\"map3\":" << event->map3_reduce_elements
                   << ",\"argmin\":" << event->argmin_vector_elements
                   << ",\"primitive_descriptors\":" << event->primitive_descriptors
                   << ",\"structural_operations\":" << event->structural_operations
                   << ",\"branch_domain\":" << event->branch_domain
                   << ",\"operand_bytes\":" << event->operand_bytes
                   << ",\"result_bytes\":" << event->result_bytes << "}\n";
  }
};

enum class SolverMode { kBareMetal, kLocal };

void print_usage(std::ostream &output) {
  output << "Usage: pcaa_graph_run [OPTIONS] GRAPH.pbqp\n\n"
            "Run a PBQP graph through the PCAA SystemC model.\n\n"
            "Options:\n"
            "  --solver bare-metal|local  Select bounded shared or host-local execution.\n"
            "  --strategy reduce-only|heuristic-rn|exact-core-enumeration|exact-branch-reduce|\n"
            "             local-search|heuristic-rn-local-search\n"
            "                              Select the solving algorithm (default: heuristic-rn).\n"
            "  --rn-policy min-degree|max-degree|min-work\n"
            "                              Select RN node choice (default: min-degree).\n"
            "  --rn-batching per-node|per-edge\n"
            "                              Use vector RN/R2 jobs or retain scalar reference jobs.\n"
            "  --maximum-search-nodes N   Bound an exact search; zero leaves the limit unset.\n"
            "  --verbose                   Trace model activity to standard error.\n"
            "  --trace FILE                Write stable JSONL solver events to FILE.\n"
            "  --help                      Show this help text.\n"
            "  --version                   Show the runner version.\n";
}

const char *strategy_name(pbqp_solver_strategy_t strategy) {
  switch (strategy) {
    case PBQP_STRATEGY_REDUCE_ONLY:
      return "REDUCE_ONLY";
    case PBQP_STRATEGY_HEURISTIC_RN:
      return "HEURISTIC_RN";
    case PBQP_STRATEGY_EXACT_CORE_ENUMERATION:
      return "EXACT_CORE_ENUMERATION";
    case PBQP_STRATEGY_EXACT_BRANCH_REDUCE:
      return "EXACT_BRANCH_REDUCE";
    case PBQP_STRATEGY_LOCAL_SEARCH:
      return "LOCAL_SEARCH";
    case PBQP_STRATEGY_HEURISTIC_RN_LOCAL_SEARCH:
      return "HEURISTIC_RN_LOCAL_SEARCH";
  }
  return "UNKNOWN";
}

bool parse_costs(std::istringstream *line, size_t count, std::vector<int32_t> *costs) {
  costs->clear();
  costs->reserve(count);
  for (size_t index = 0; index < count; ++index) {
    std::string token;
    if (!(*line >> token)) {
      return false;
    }
    try {
      if (token == "INF") {
        costs->push_back(ACCEL_INF);
        continue;
      }
      size_t parsed = 0;
      const long long value = std::stoll(token, &parsed, 10);
      if (parsed != token.size() || value < std::numeric_limits<int32_t>::min() ||
          value > std::numeric_limits<int32_t>::max()) {
        return false;
      }
      costs->push_back(static_cast<int32_t>(value));
    } catch (const std::exception &) {
      return false;
    }
  }
  std::string extra;
  return !(*line >> extra);
}

bool is_valid_input_cost(int32_t cost) {
  return cost <= ACCEL_INF;
}

bool load_problem(const std::string &path, InputProblem *problem) {
  std::ifstream input(path);
  if (!input) {
    return false;
  }
  bool saw_nodes = false;
  size_t declared_nodes = 0;
  std::string text;
  while (std::getline(input, text)) {
    const size_t comment = text.find('#');
    std::istringstream line(text.substr(0, comment));
    std::string kind;
    if (!(line >> kind)) {
      continue;
    }
    if (kind == "nodes") {
      size_t count = 0;
      std::string extra;
      if (saw_nodes || !(line >> count) || line >> extra || count == 0 || !problem->nodes.empty()) {
        return false;
      }
      saw_nodes = true;
      declared_nodes = count;
    } else if (kind == "node") {
      size_t domain = 0;
      if (!saw_nodes || problem->nodes.size() == declared_nodes || !(line >> domain) ||
          domain == 0 || domain > kMaximumHostDomain) {
        return false;
      }
      std::vector<int32_t> costs;
      if (!parse_costs(&line, domain, &costs) ||
          !std::all_of(costs.begin(), costs.end(), is_valid_input_cost)) {
        return false;
      }
      problem->nodes.push_back({std::move(costs)});
    } else if (kind == "edge") {
      size_t first = 0;
      size_t second = 0;
      if (!saw_nodes || !(line >> first >> second) || first >= problem->nodes.size() ||
          second >= problem->nodes.size() || first == second) {
        return false;
      }
      const size_t count = problem->nodes[first].unary.size() * problem->nodes[second].unary.size();
      std::vector<int32_t> costs;
      if (!parse_costs(&line, count, &costs) ||
          !std::all_of(costs.begin(), costs.end(), is_valid_input_cost)) {
        return false;
      }
      problem->edges.push_back({first, second, std::move(costs)});
    } else {
      return false;
    }
  }
  return saw_nodes && problem->nodes.size() == declared_nodes;
}

bool build_problem(const InputProblem &input, bool fixed_capacity, pbqp_problem_t *problem) {
  if (fixed_capacity &&
      (input.nodes.size() > PBQP_MAX_NODES || input.edges.size() > PBQP_MAX_EDGES)) {
    return false;
  }
  if (input.nodes.size() > std::numeric_limits<unsigned>::max() ||
      input.edges.size() >= std::numeric_limits<unsigned>::max()) {
    return false;
  }
  // Bare-metal mode must accept exactly what the RV64 configuration accepts:
  // pbqp_max_finite_cost depends on the capacities, so use the fixed ones.
  const unsigned node_capacity =
      fixed_capacity ? PBQP_MAX_NODES : static_cast<unsigned>(input.nodes.size());
  size_t maximum_domain = 0;
  for (const InputNode &node : input.nodes)
    maximum_domain = std::max(maximum_domain, node.unary.size());
  if (maximum_domain > std::numeric_limits<unsigned>::max())
    return false;
  const unsigned domain_capacity =
      fixed_capacity ? PBQP_MAX_DOMAIN : static_cast<unsigned>(maximum_domain);
  // R2 may need one fill slot before it retires its two incident edges; the
  // fixed configuration already reserves every simple edge.
  const unsigned edge_capacity =
      fixed_capacity ? PBQP_MAX_EDGES : static_cast<unsigned>(input.edges.size()) + 1;
  if (pbqp_init(problem, pbqp_heap_allocator(), node_capacity, edge_capacity, domain_capacity) !=
      PBQP_OK) {
    return false;
  }
  for (const InputNode &node : input.nodes) {
    if (pbqp_add_node(problem, static_cast<unsigned>(node.unary.size()), node.unary.data()) !=
        PBQP_OK) {
      pbqp_destroy(problem);
      return false;
    }
  }
  for (const InputEdge &edge : input.edges) {
    if (pbqp_add_edge(problem, static_cast<unsigned>(edge.first),
                      static_cast<unsigned>(edge.second), edge.costs.data()) != PBQP_OK) {
      pbqp_destroy(problem);
      return false;
    }
  }
  return true;
}

struct ProblemOwner {
  pbqp_problem_t value{};

  ProblemOwner() = default;

  ~ProblemOwner() {
    pbqp_destroy(&value);
  }

  ProblemOwner(const ProblemOwner &) = delete;
  ProblemOwner &operator=(const ProblemOwner &) = delete;

  ProblemOwner(ProblemOwner &&other) noexcept : value(other.value) {
    other.value = {};
  }

  ProblemOwner &operator=(ProblemOwner &&other) noexcept {
    if (this != &other) {
      pbqp_destroy(&value);
      value = other.value;
      other.value = {};
    }
    return *this;
  }
};

}  // namespace

AccelTimingConfig runner_timing_config() {
#if defined(PCAA_GRAPH_RUN_TIMED)
  AccelTimingConfig timing;
  timing.mode = AccelTimingMode::kL1Streaming;
  timing.lanes = kTimedRunnerLanes;
  timing.descriptor_bytes_per_cycle = kTimedRunnerBytesPerCycle;
  timing.memory_read_bytes_per_cycle = kTimedRunnerBytesPerCycle;
  timing.memory_write_bytes_per_cycle = kTimedRunnerBytesPerCycle;
  timing.cycle_period = sc_core::sc_time(kTimedRunnerCyclePeriodNanoseconds, sc_core::SC_NS);
  return timing;
#else
  return {};
#endif
}

int sc_main(int argc, char **argv) {
  bool verbose = false;
  SolverMode solver_mode = SolverMode::kLocal;
  bool saw_solver_mode = false;
  bool saw_strategy = false;
  const char *trace_path = nullptr;
  pbqp_solver_config_t solver_config = pbqp_solver_default_config();
  const char *path = nullptr;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help") {
      print_usage(std::cout);
      return 0;
    } else if (argument == "--version") {
      std::cout << "pcaa_graph_run " << PCAA_VERSION << '\n';
      return 0;
    } else if (argument == "--verbose" && !verbose) {
      verbose = true;
    } else if (argument == "--solver" && !saw_solver_mode && index + 1 < argc) {
      const std::string mode = argv[++index];
      if (mode == "bare-metal") {
        solver_mode = SolverMode::kBareMetal;
      } else if (mode == "local") {
        solver_mode = SolverMode::kLocal;
      } else {
        std::cerr << "unknown solver mode: " << mode << '\n';
        return 2;
      }
      saw_solver_mode = true;
    } else if (argument == "--strategy" && index + 1 < argc) {
      const std::string strategy = argv[++index];
      if (strategy == "reduce-only") {
        solver_config.strategy = PBQP_STRATEGY_REDUCE_ONLY;
      } else if (strategy == "heuristic-rn") {
        solver_config.strategy = PBQP_STRATEGY_HEURISTIC_RN;
      } else if (strategy == "exact-branch-reduce") {
        solver_config.strategy = PBQP_STRATEGY_EXACT_BRANCH_REDUCE;
      } else if (strategy == "exact-core-enumeration") {
        solver_config.strategy = PBQP_STRATEGY_EXACT_CORE_ENUMERATION;
      } else if (strategy == "local-search") {
        solver_config.strategy = PBQP_STRATEGY_LOCAL_SEARCH;
      } else if (strategy == "heuristic-rn-local-search") {
        solver_config.strategy = PBQP_STRATEGY_HEURISTIC_RN_LOCAL_SEARCH;
      } else {
        std::cerr << "unknown solver strategy: " << strategy << '\n';
        return 2;
      }
      saw_strategy = true;
    } else if (argument == "--rn-policy" && index + 1 < argc) {
      const std::string policy = argv[++index];
      if (policy == "min-degree") {
        solver_config.rn_policy = PBQP_RN_MIN_DEGREE;
      } else if (policy == "max-degree") {
        solver_config.rn_policy = PBQP_RN_MAX_DEGREE;
      } else if (policy == "min-work") {
        solver_config.rn_policy = PBQP_RN_MIN_WORK;
      } else {
        std::cerr << "unknown RN policy: " << policy << '\n';
        return 2;
      }
    } else if (argument == "--rn-batching" && index + 1 < argc) {
      const std::string batching = argv[++index];
      if (batching == "per-node") {
        solver_config.rn_batching = PBQP_RN_BATCH_PER_NODE;
      } else if (batching == "per-edge") {
        solver_config.rn_batching = PBQP_RN_BATCH_PER_EDGE;
      } else {
        std::cerr << "unknown RN batching mode: " << batching << '\n';
        return 2;
      }
    } else if (argument == "--maximum-search-nodes" && index + 1 < argc) {
      try {
        size_t parsed = 0;
        const std::string value = argv[++index];
        const unsigned long parsed_value = std::stoul(value, &parsed, 10);
        if (parsed != value.size() || parsed_value > std::numeric_limits<unsigned>::max()) {
          throw std::out_of_range("maximum search-node count");
        }
        solver_config.maximum_search_nodes = static_cast<unsigned>(parsed_value);
      } catch (const std::exception &) {
        std::cerr << "invalid maximum search-node count\n";
        return 2;
      }
    } else if (argument == "--trace" && index + 1 < argc) {
      trace_path = argv[++index];
    } else if (path == nullptr) {
      path = argv[index];
    } else {
      std::cerr << "usage: pcaa_graph_run [--verbose] --solver bare-metal|local "
                   "[--strategy reduce-only|heuristic-rn|exact-core-enumeration|"
                   "exact-branch-reduce|local-search|heuristic-rn-local-search] "
                   "[--rn-policy min-degree|max-degree|min-work] "
                   "[--maximum-search-nodes N] [--trace FILE] GRAPH.pbqp\n";
      return 2;
    }
  }
  if (!saw_solver_mode || path == nullptr) {
    std::cerr << "usage: pcaa_graph_run [--verbose] --solver bare-metal|local "
                 "[--strategy reduce-only|heuristic-rn|exact-core-enumeration|"
                 "exact-branch-reduce|local-search|heuristic-rn-local-search] "
                 "[--rn-policy min-degree|max-degree|min-work] "
                 "[--maximum-search-nodes N] [--trace FILE] GRAPH.pbqp\n";
    return 2;
  }
  if (!saw_strategy) {
    solver_config.strategy = PBQP_STRATEGY_HEURISTIC_RN;
  }
  // SystemC 3 emits this banner on kernel startup unless explicitly disabled.
  setenv("SYSTEMC_DISABLE_COPYRIGHT_MESSAGE", "1", 0);
  InputProblem input;
  if (!load_problem(path, &input)) {
    std::cerr << "invalid PBQP input: " << path << '\n';
    return 2;
  }
  ModelKernel model(verbose, runner_timing_config());
  sc_core::sc_start(sc_core::SC_ZERO_TIME);
  pbqp_cost_kernel_t kernel;
  model.make_kernel(&kernel);
  TraceWriter trace_writer;
  pbqp_trace_sink_t trace_sink{};
  if (trace_path != nullptr) {
    trace_writer.output.open(trace_path);
    if (!trace_writer.output) {
      std::cerr << "cannot open trace output: " << trace_path << '\n';
      return 2;
    }
    trace_sink.context = &trace_writer;
    trace_sink.emit = TraceWriter::emit;
    solver_config.trace_sink = &trace_sink;
  }
  RunnerSolution solution;
  ProblemOwner problem;
  ProblemOwner original_problem;
  const pbqp_statistics_t *statistics = nullptr;
  const bool uses_shared_solver = true;
  if (uses_shared_solver) {
    if (!build_problem(input, solver_mode == SolverMode::kBareMetal, &problem.value)) {
      std::cerr << "graph does not fit the shared PBQP solver in the selected mode\n";
      return 2;
    }
    if (pbqp_problem_clone(&original_problem.value, &problem.value, pbqp_heap_allocator()) !=
        PBQP_OK) {
      std::cerr << "could not allocate the PBQP graph copy\n";
      return 1;
    }
    pbqp_solver_t solver;
    std::vector<unsigned> assignment(input.nodes.size());
    pbqp_solution_t fixed_solution;
    pbqp_solution_init(&fixed_solution, assignment.data(), assignment.size());
    if (pbqp_solver_create_with_config(&solver, PBQP_MODE_ACCELERATOR, &kernel, &solver_config) !=
        PBQP_OK) {
      std::cerr << "PCAA model could not solve the graph\n";
      return 1;
    }
    const pbqp_status_t status = pbqp_solver_solve(&solver, &problem.value, &fixed_solution);
    if (status == PBQP_IRREDUCIBLE) {
      std::cout << "status IRREDUCIBLE\nstrategy " << strategy_name(solver_config.strategy) << '\n';
      return 0;
    }
    if (status == PBQP_SEARCH_LIMIT) {
      std::cout << "status SEARCH_LIMIT\nstrategy " << strategy_name(solver_config.strategy)
                << '\n';
      return 0;
    }
    if (status != PBQP_OK) {
      if (status == PBQP_KERNEL_ERROR && solver.last_kernel_status > 0 &&
          solver.last_kernel_status <= PCAA_STATUS_DEVICE_ERROR)
        pcaa_perror("PCAA cost kernel", static_cast<pcaa_status_t>(solver.last_kernel_status));
      std::cerr << "PCAA model could not solve the graph\n";
      return 1;
    }
    solution.optimum = fixed_solution.optimum;
    solution.assignment = assignment;
    const int32_t evaluated = pbqp_evaluate(&original_problem.value, fixed_solution.assignment);
    if (evaluated != fixed_solution.optimum) {
      std::cerr << "PBQP solver returned an objective inconsistent with its assignment\n";
      return 1;
    }
    solution.exact = solver_config.strategy == PBQP_STRATEGY_EXACT_CORE_ENUMERATION ||
                     solver_config.strategy == PBQP_STRATEGY_EXACT_BRANCH_REDUCE;
    statistics = &problem.value.statistics;
  }
  std::cout << "optimum " << solution.optimum << "\nassignment";
  for (unsigned value : solution.assignment) {
    std::cout << ' ' << value;
  }
  const char *solution_kind = solution.exact ? "exact" : "local-optimum";
  if (solver_config.strategy != PBQP_STRATEGY_LOCAL_SEARCH && !solution.exact) {
    solution_kind = "heuristic";
  }
  std::cout << "\nsolution " << solution_kind << '\n';
  std::cout << "strategy " << strategy_name(solver_config.strategy) << '\n';
  if (verbose && statistics != nullptr) {
    std::cerr << "pcaa: reductions R0=" << statistics->r0_count << " R1=" << statistics->r1_count
              << " R2=" << statistics->r2_count << " RN=" << statistics->rn_count
              << " projections=" << statistics->rn_projection_count
              << " projection_primitives=" << statistics->rn_projection_primitives
              << " commits=" << statistics->rn_commit_elements << '\n';
    std::cerr << "pcaa: RN core first=" << statistics->first_rn_active_nodes << " nodes/"
              << statistics->first_rn_active_edges
              << " edges max=" << statistics->maximum_irreducible_core_nodes << " nodes/"
              << statistics->maximum_irreducible_core_edges
              << " edges episodes=" << statistics->rn_episodes
              << " degree=" << statistics->rn_degree_min << ".." << statistics->rn_degree_max
              << " after-RN R0=" << statistics->r0_after_rn << " R1=" << statistics->r1_after_rn
              << " R2=" << statistics->r2_after_rn
              << " cascade-total=" << statistics->rn_cascade_total_length
              << " cascade-max=" << statistics->rn_cascade_maximum_length << '\n';
    unsigned rn_cascade_r0_total = 0;
    unsigned rn_cascade_r1_total = 0;
    unsigned rn_cascade_r2_total = 0;
    for (unsigned episode = 0; episode < statistics->rn_episodes; ++episode) {
      rn_cascade_r0_total += statistics->rn_cascade_r0[episode];
      rn_cascade_r1_total += statistics->rn_cascade_r1[episode];
      rn_cascade_r2_total += statistics->rn_cascade_r2[episode];
    }
    const double rn_cascade_mean =
        statistics->rn_episodes == 0
            ? 0.0
            : static_cast<double>(statistics->rn_cascade_total_length) / statistics->rn_episodes;
    std::cerr << "pcaa: rn_cascades rn_episodes=" << statistics->rn_episodes
              << " rn_cascade_r0_total=" << rn_cascade_r0_total
              << " rn_cascade_r1_total=" << rn_cascade_r1_total
              << " rn_cascade_r2_total=" << rn_cascade_r2_total
              << " rn_cascade_exact_total=" << statistics->rn_cascade_total_length
              << " rn_cascade_mean=" << std::to_string(rn_cascade_mean)
              << " rn_cascade_max=" << statistics->rn_cascade_maximum_length << '\n';
    std::cerr << "pcaa: rn_cascade_histogram";
    for (size_t length = 0; length <= input.nodes.size(); ++length) {
      std::cerr << " cascade_len_" << length << '='
                << statistics->rn_cascade_length_histogram[length];
    }
    std::cerr << '\n';
    std::cerr << "pcaa: RN traffic projection-read=" << statistics->rn_projection_operand_bytes
              << " projection-write=" << statistics->rn_projection_result_bytes
              << " score-accumulation=" << statistics->rn_score_accumulation_elements
              << " commit-bytes=" << statistics->rn_commit_bytes << '\n';
    std::cerr << "pcaa: condition traffic operations=" << statistics->condition_count
              << " elements=" << statistics->condition_elements
              << " matrix-read=" << statistics->condition_matrix_read_bytes
              << " unary-read=" << statistics->condition_unary_read_bytes
              << " unary-write=" << statistics->condition_unary_write_bytes << '\n';
    std::cerr << "pcaa: local search evaluations=" << statistics->local_search_node_evaluations
              << " sweeps=" << statistics->local_search_sweeps
              << " accepted-moves=" << statistics->local_search_accepted_moves
              << " slices=" << statistics->local_search_slice_accumulations
              << " slice-elements=" << statistics->local_search_slice_elements
              << " argmin=" << statistics->local_search_argmin_reductions << '\n';
    const unsigned operation_descriptors =
        statistics->minplus_project_descriptors + statistics->vector_add_descriptors +
        statistics->map3_reduce_descriptors + statistics->argmin_vector_descriptors;
    const uint64_t operation_bytes =
        statistics->operation_mix_operand_bytes + statistics->operation_mix_result_bytes;
    std::cerr << "pcaa: operation mix project-elements=" << statistics->minplus_project_elements
              << " project-accumulate-elements=" << statistics->project_accumulate_elements
              << " slice-elements=" << statistics->slice_accumulate_elements
              << " map3-elements=" << statistics->map3_reduce_elements
              << " argmin-elements=" << statistics->argmin_vector_elements
              << " descriptors=" << operation_descriptors
              << " batches=" << statistics->batch_submissions
              << " operand-bytes=" << statistics->operation_mix_operand_bytes
              << " result-bytes=" << statistics->operation_mix_result_bytes
              << " bytes=" << operation_bytes
              << " scalar-project-descriptors=" << statistics->scalar_project_descriptors
              << " vector-project-descriptors=" << statistics->vector_project_descriptors
              << " vector-add-descriptors=" << statistics->vector_add_descriptors
              << " vector-add-dst-src0=" << statistics->vector_add_dst_src0
              << " vector-add-dst-src1=" << statistics->vector_add_dst_src1
              << " vector-add-inplace=" << statistics->vector_add_inplace_descriptors
              << " vector-add-general=" << statistics->vector_add_general_descriptors
              << " scalar-map3-descriptors=" << statistics->scalar_map3_descriptors
              << " partial-map3-descriptors=" << statistics->partial_map3_descriptors << '\n';
    std::cerr << "pcaa: views contiguous=" << statistics->contiguous_views
              << " strided=" << statistics->strided_views << '\n';
    std::cerr << "pcaa: encoding old-fixed-bytes="
              << kLegacyDescriptorBytes *
                     (statistics->batch_primitive_descriptors + statistics->batch_submissions)
              << " compact-bytes=" << statistics->batch_descriptor_bytes
              << " child-bytes=" << statistics->batch_child_descriptor_bytes << " vector-add-total="
              << statistics->vector_add_inplace_descriptors +
                     statistics->vector_add_general_descriptors
              << " dst-src0=" << statistics->vector_add_dst_src0
              << " dst-src1=" << statistics->vector_add_dst_src1
              << " inplace=" << statistics->vector_add_inplace_descriptors
              << " general=" << statistics->vector_add_general_descriptors << '\n';
    std::cerr << "pcaa: encoding primitive-counts";
    for (unsigned opcode = ACCEL_OPCODE_MAP_ADD_REDUCE_MIN;
         opcode <= ACCEL_OPCODE_MINPLUS_MAP3_PROJECT; ++opcode)
      std::cerr << " op" << opcode << '=' << statistics->primitive_submissions[opcode];
    std::cerr << '\n';
    std::cerr << "pcaa: exact search nodes=" << statistics->search_nodes_visited
              << " branches=" << statistics->search_branches_created
              << " max-depth=" << statistics->search_maximum_depth
              << " limit-hits=" << statistics->search_limit_hits
              << " pruned=" << statistics->search_nodes_pruned << '\n';
  }
#if defined(PCAA_GRAPH_RUN_TIMED)
  const AccelTimingStatistics &timing = model.timing_statistics();
  const VectorCycleProjection &projection = model.vector_cycle_projection();
  std::cout << "timing cycles=" << timing.total_service_cycles
            << " descriptor=" << timing.descriptor_cycles
            << " operands=" << timing.operand_read_cycles << " compute=" << timing.compute_cycles
            << " result=" << timing.result_write_cycles << " primitives=" << timing.primitive_count
            << " batches=" << timing.batch_count << '\n';
  const auto print_projection = [](const char *name, const CycleBreakdown &cycles) {
    std::cout << "projection " << name << " cycles=" << cycles.total
              << " descriptor=" << cycles.descriptor << " operands=" << cycles.operands
              << " compute=" << cycles.compute << " result=" << cycles.result
              << " descriptors=" << cycles.descriptors << '\n';
  };
  print_projection("project-scalar", projection.project_scalar);
  print_projection("project-vector", projection.project_vector);
  print_projection("map3-scalar", projection.map3_scalar);
  print_projection("map3-partial", projection.map3_partial);
  print_projection("map3-full", projection.map3_full);
  if (timing.primitive_count == 0) {
    std::cout << "timing note=no accelerator primitives were issued; the PBQP core was solved "
                 "in software\n";
  }
#endif
  return 0;
}
