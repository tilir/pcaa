// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Loads a small PBQP text graph and solves it through the SystemC PCAA model.

#include "accelerator.h"
#include "accel_protocol.h"
#include "cost_math.h"
#include "memory_interface.h"
#include "pbqp/pbqp.h"
#include "timing_model.h"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <sysc/communication/sc_port.h>
#include <sysc/kernel/sc_dynamic_processes.h>
#include <sysc/kernel/sc_externs.h>
#include <sysc/kernel/sc_module.h>
#include <sysc/kernel/sc_module_name.h>
#include <sysc/kernel/sc_simcontext.h>
#include <sysc/kernel/sc_spawn.h>
#include <sysc/kernel/sc_time.h>
#include <sysc/kernel/sc_wait.h>
#include <tlm_core/tlm_2/tlm_2_interfaces/tlm_fw_bw_ifs.h>
#include <tlm_core/tlm_2/tlm_generic_payload/tlm_gp.h>
#include <tlm_core/tlm_2/tlm_generic_payload/tlm_phase.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>

namespace {

constexpr size_t kGuestMemoryBytes = 1024 * 1024;
constexpr size_t kMaximumHostDomain = 64 * 1024;
constexpr unsigned kTimedRunnerLanes = 4;
constexpr int kTimedRunnerCyclePeriodNanoseconds = 1;
constexpr uint64_t kFirstAllocationAddress = 0x100;
constexpr size_t kAllocationAlignment = 8;
constexpr uint32_t kDoorbellSubmit = 1;
constexpr int kAddressLowBits = 32;

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

enum class SolverMode { kBareMetal, kLocal };

void print_usage(std::ostream &output) {
  output << "Usage: pcaa_graph_run [OPTIONS] GRAPH.pbqp\n\n"
            "Run a PBQP graph through the PCAA SystemC model.\n\n"
            "Options:\n"
            "  --solver bare-metal|local  Select bounded shared or host-local execution.\n"
            "  --strategy reduce-only|heuristic-rn|exact-branch-reduce|local-search\n"
            "                              Select the solving algorithm (default: heuristic-rn).\n"
            "  --rn-policy min-degree|max-degree|min-work\n"
            "                              Select RN node choice (default: min-degree).\n"
            "  --verbose                   Trace model activity to standard error.\n"
            "  --help                      Show this help text.\n"
            "  --version                   Show the runner version.\n";
}

const char *strategy_name(pbqp_solver_strategy_t strategy) {
  switch (strategy) {
    case PBQP_STRATEGY_REDUCE_ONLY:
      return "REDUCE_ONLY";
    case PBQP_STRATEGY_HEURISTIC_RN:
      return "HEURISTIC_RN";
    case PBQP_STRATEGY_EXACT_BRANCH_REDUCE:
      return "EXACT_BRANCH_REDUCE";
    case PBQP_STRATEGY_LOCAL_SEARCH:
      return "LOCAL_SEARCH";
  }
  return "UNKNOWN";
}

class GuestMemory final : public MemoryInterface {
 public:
  GuestMemory() : bytes_(kGuestMemoryBytes) {}

  bool read(uint64_t address, void *destination, size_t size) override {
    if (!contains(address, size)) {
      return false;
    }
    std::memcpy(destination, bytes_.data() + address, size);
    return true;
  }

  bool write(uint64_t address, const void *source, size_t size) override {
    if (!contains(address, size)) {
      return false;
    }
    std::memcpy(bytes_.data() + address, source, size);
    return true;
  }

  uint64_t allocate(size_t size, size_t alignment = kAllocationAlignment) {
    const uint64_t aligned = (next_address_ + alignment - 1) / alignment * alignment;
    if (!contains(aligned, size)) {
      return 0;
    }
    next_address_ = aligned + size;
    return aligned;
  }

  void reset() {
    next_address_ = kFirstAllocationAddress;
  }

 private:
  bool contains(uint64_t address, size_t size) const {
    return address <= bytes_.size() && size <= bytes_.size() - address;
  }

  std::vector<unsigned char> bytes_;
  uint64_t next_address_ = kFirstAllocationAddress;
};

class Initiator final : public sc_core::sc_module {
 public:
  tlm_utils::simple_initiator_socket<Initiator> socket;

  explicit Initiator(sc_core::sc_module_name name) : sc_core::sc_module(name), socket("socket") {}
};

class ModelKernel {
 public:
  ModelKernel(bool verbose, AccelTimingConfig timing)
      : accelerator_("accelerator", memory_, timing, verbose), initiator_("initiator") {
    initiator_.socket.bind(accelerator_.target_socket);
    sc_core::sc_start(sc_core::SC_ZERO_TIME);
  }

  void make_kernel(pbqp_cost_kernel_t *kernel) {
    kernel->context = this;
    kernel->min2_argmin = min2;
    kernel->min3_argmin = min3;
    kernel->min2_argmin_batch = min2_batch;
    kernel->min3_argmin_batch = min3_batch;
  }

  const AccelTimingStatistics &timing_statistics() const {
    return accelerator_.timing_statistics();
  }

 private:
  static int min2(void *opaque, pbqp_vector_view_t first, pbqp_vector_view_t second,
                  accel_min_argmin_result_t *result) {
    const pbqp_min2_job_t job = {first, second, result};
    return min2_batch(opaque, &job, 1);
  }

  static int min3(void *opaque, pbqp_vector_view_t first, pbqp_vector_view_t second,
                  pbqp_vector_view_t third, accel_min_argmin_result_t *result) {
    const pbqp_min3_job_t job = {first, second, third, result};
    return min3_batch(opaque, &job, 1);
  }

  static int min2_batch(void *opaque, const pbqp_min2_job_t *jobs, size_t count) {
    ModelKernel *kernel = static_cast<ModelKernel *>(opaque);
    kernel->memory_.reset();
    std::vector<accel_command_t> commands;
    std::vector<uint64_t> result_addresses;
    commands.reserve(count);
    result_addresses.reserve(count);
    for (size_t index = 0; index < count; ++index) {
      const uint64_t first = kernel->copy_view(jobs[index].a);
      const uint64_t second = kernel->copy_view(jobs[index].b);
      const uint64_t result = kernel->allocate_result();
      if (first == 0 || second == 0 || result == 0) {
        return -1;
      }
      commands.push_back({ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN, 0,
                          static_cast<uint32_t>(jobs[index].a.length), 0, 0, 0, first, second, 0,
                          result});
      result_addresses.push_back(result);
    }
    if (!kernel->submit_batch(commands)) {
      return -1;
    }
    for (size_t index = 0; index < count; ++index) {
      if (!kernel->memory_.read(result_addresses[index], jobs[index].result,
                                sizeof(*jobs[index].result))) {
        return -1;
      }
    }
    return 0;
  }

  static int min3_batch(void *opaque, const pbqp_min3_job_t *jobs, size_t count) {
    ModelKernel *kernel = static_cast<ModelKernel *>(opaque);
    kernel->memory_.reset();
    std::vector<accel_command_t> commands;
    std::vector<uint64_t> result_addresses;
    commands.reserve(count);
    result_addresses.reserve(count);
    for (size_t index = 0; index < count; ++index) {
      const uint64_t first = kernel->copy_view(jobs[index].a);
      const uint64_t second = kernel->copy_view(jobs[index].b);
      const uint64_t third = kernel->copy_view(jobs[index].c);
      const uint64_t result = kernel->allocate_result();
      if (first == 0 || second == 0 || third == 0 || result == 0) {
        return -1;
      }
      commands.push_back({ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN, 0,
                          static_cast<uint32_t>(jobs[index].a.length), 0, 0, 0, first, second,
                          third, result});
      result_addresses.push_back(result);
    }
    if (!kernel->submit_batch(commands)) {
      return -1;
    }
    for (size_t index = 0; index < count; ++index) {
      if (!kernel->memory_.read(result_addresses[index], jobs[index].result,
                                sizeof(*jobs[index].result))) {
        return -1;
      }
    }
    return 0;
  }

  uint64_t copy_view(pbqp_vector_view_t view) {
    const uint64_t address = memory_.allocate(view.length * sizeof(int32_t));
    if (address == 0) {
      return 0;
    }
    for (size_t index = 0; index < view.length; ++index) {
      if (!memory_.write(address + index * sizeof(int32_t), &view.base[index * view.stride],
                         sizeof(int32_t))) {
        return 0;
      }
    }
    return address;
  }

  uint64_t allocate_result() {
    return memory_.allocate(sizeof(accel_min_argmin_result_t));
  }

  bool submit_batch(const std::vector<accel_command_t> &commands) {
    if (commands.empty()) {
      return false;
    }
    const uint64_t child_address = memory_.allocate(commands.size() * sizeof(commands.front()));
    const uint64_t batch_result_address = memory_.allocate(sizeof(accel_batch_result_t));
    const uint64_t descriptor_address = memory_.allocate(sizeof(accel_command_t));
    if (child_address == 0 || batch_result_address == 0 || descriptor_address == 0 ||
        !memory_.write(child_address, commands.data(),
                       commands.size() * sizeof(commands.front()))) {
      return false;
    }
    const accel_command_t batch = {ACCEL_OPCODE_EXECUTE_BATCH,
                                   0,
                                   static_cast<uint32_t>(commands.size()),
                                   0,
                                   0,
                                   0,
                                   child_address,
                                   0,
                                   0,
                                   batch_result_address};
    if (!memory_.write(descriptor_address, &batch, sizeof(batch)) ||
        !mmio(ACCEL_MMIO_DESC_ADDR_LO, static_cast<uint32_t>(descriptor_address)) ||
        !mmio(ACCEL_MMIO_DESC_ADDR_HI,
              static_cast<uint32_t>(descriptor_address >> kAddressLowBits)) ||
        !mmio(ACCEL_MMIO_DOORBELL, kDoorbellSubmit)) {
      return false;
    }
    uint32_t status = 0;
    if (!mmio_read(ACCEL_MMIO_STATUS, &status) || status != ACCEL_STATUS_DONE) {
      return false;
    }
    accel_batch_result_t result{};
    return memory_.read(batch_result_address, &result, sizeof(result)) &&
           result.completed == commands.size() && result.failed_index == UINT32_MAX;
  }

  bool mmio(uint64_t address, uint32_t value) {
    return transport(address, &value, tlm::TLM_WRITE_COMMAND);
  }

  bool mmio_read(uint64_t address, uint32_t *value) {
    return transport(address, value, tlm::TLM_READ_COMMAND);
  }

  bool transport(uint64_t address, uint32_t *value, tlm::tlm_command command) {
    tlm::tlm_generic_payload transaction;
    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    transaction.set_command(command);
    transaction.set_address(address);
    transaction.set_data_ptr(reinterpret_cast<unsigned char *>(value));
    transaction.set_data_length(sizeof(*value));
    transaction.set_streaming_width(sizeof(*value));
    initiator_.socket->b_transport(transaction, delay);
    return transaction.get_response_status() == tlm::TLM_OK_RESPONSE;
  }

  GuestMemory memory_;
  Accelerator accelerator_;
  Initiator initiator_;
};

bool parse_costs(std::istringstream *line, size_t count, std::vector<int32_t> *costs) {
  costs->clear();
  costs->reserve(count);
  for (size_t index = 0; index < count; ++index) {
    std::string token;
    if (!(*line >> token)) {
      return false;
    }
    try {
      costs->push_back(token == "INF" ? ACCEL_INF : std::stoi(token));
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

bool build_fixed_problem(const InputProblem &input, pbqp_problem_t *problem) {
  if (input.nodes.size() > PBQP_MAX_NODES || input.edges.size() > PBQP_MAX_EDGES) {
    return false;
  }
  pbqp_init(problem);
  for (const InputNode &node : input.nodes) {
    if (pbqp_add_node(problem, static_cast<unsigned>(node.unary.size()), node.unary.data()) !=
        PBQP_OK) {
      return false;
    }
  }
  for (const InputEdge &edge : input.edges) {
    if (pbqp_add_edge(problem, static_cast<unsigned>(edge.first),
                      static_cast<unsigned>(edge.second), edge.costs.data()) != PBQP_OK) {
      return false;
    }
  }
  return true;
}

int32_t edge_cost(const InputProblem &problem, const InputEdge &edge, size_t node, unsigned value,
                  const std::vector<unsigned> &assignment) {
  const bool node_is_first = edge.first == node;
  const size_t other = node_is_first ? edge.second : edge.first;
  const size_t first_value = node_is_first ? value : assignment[other];
  const size_t second_value = node_is_first ? assignment[other] : value;
  return edge.costs[first_value * problem.nodes[edge.second].unary.size() + second_value];
}

RunnerSolution run_local_descent(const InputProblem &problem, const pbqp_cost_kernel_t &kernel,
                                 std::vector<unsigned> assignment) {
  RunnerSolution solution;
  solution.assignment = std::move(assignment);
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t node = 0; node < problem.nodes.size(); ++node) {
      const InputNode &current = problem.nodes[node];
      std::vector<int32_t> scores = current.unary;
      for (const InputEdge &edge : problem.edges) {
        if (edge.first != node && edge.second != node) {
          continue;
        }
        for (size_t value = 0; value < scores.size(); ++value) {
          scores[value] = accel_cost_add(
              scores[value],
              edge_cost(problem, edge, node, static_cast<unsigned>(value), solution.assignment));
        }
      }
      std::vector<int32_t> zeroes(scores.size());
      accel_min_argmin_result_t best{};
      const pbqp_vector_view_t score_view = {scores.data(), scores.size(), 1};
      const pbqp_vector_view_t zero_view = {zeroes.data(), zeroes.size(), 1};
      if (kernel.min2_argmin(kernel.context, score_view, zero_view, &best) != 0 ||
          best.index >= scores.size()) {
        return solution;
      }
      if (best.value < scores[solution.assignment[node]]) {
        solution.assignment[node] = best.index;
        changed = true;
      }
    }
  }
  int32_t total = 0;
  for (size_t node = 0; node < problem.nodes.size(); ++node) {
    total = accel_cost_add(total, problem.nodes[node].unary[solution.assignment[node]]);
  }
  for (const InputEdge &edge : problem.edges) {
    total = accel_cost_add(
        total,
        edge.costs[solution.assignment[edge.first] * problem.nodes[edge.second].unary.size() +
                   solution.assignment[edge.second]]);
  }
  solution.optimum = total;
  return solution;
}

RunnerSolution solve_large_problem(const InputProblem &problem, const pbqp_cost_kernel_t &kernel) {
  RunnerSolution best;
  std::vector<unsigned> initial(problem.nodes.size());
  best = run_local_descent(problem, kernel, initial);

  for (size_t node = 0; node < problem.nodes.size(); ++node) {
    for (unsigned value = 1; value < problem.nodes[node].unary.size(); ++value) {
      initial[node] = value;
      RunnerSolution candidate = run_local_descent(problem, kernel, initial);
      if (candidate.optimum < best.optimum) {
        best = std::move(candidate);
      }
      initial[node] = 0;
    }
  }
  return best;
}

}  // namespace

AccelTimingConfig runner_timing_config() {
#if defined(PCAA_GRAPH_RUN_TIMED)
  AccelTimingConfig timing;
  timing.mode = AccelTimingMode::kL1Streaming;
  timing.lanes = kTimedRunnerLanes;
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
      } else if (strategy == "local-search") {
        solver_config.strategy = PBQP_STRATEGY_LOCAL_SEARCH;
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
    } else if (path == nullptr) {
      path = argv[index];
    } else {
      std::cerr << "usage: pcaa_graph_run [--verbose] --solver bare-metal|local "
                   "[--strategy reduce-only|heuristic-rn|exact-branch-reduce|local-search] "
                   "[--rn-policy min-degree|max-degree|min-work] GRAPH.pbqp\n";
      return 2;
    }
  }
  if (!saw_solver_mode || path == nullptr) {
    std::cerr << "usage: pcaa_graph_run [--verbose] --solver bare-metal|local "
                 "[--strategy reduce-only|heuristic-rn|exact-branch-reduce|local-search] "
                 "[--rn-policy min-degree|max-degree|min-work] GRAPH.pbqp\n";
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
  pbqp_cost_kernel_t kernel;
  model.make_kernel(&kernel);
  RunnerSolution solution;
  pbqp_problem_t fixed_problem;
  const pbqp_statistics_t *statistics = nullptr;
  const bool uses_shared_solver = solver_config.strategy != PBQP_STRATEGY_LOCAL_SEARCH;
  if (uses_shared_solver) {
    if (!build_fixed_problem(input, &fixed_problem)) {
      std::cerr << "graph does not fit the shared PBQP solver (64 vertices, 6 choices per vertex, "
                   "2016 edges); use --solver local --strategy local-search\n";
      return 2;
    }
    const pbqp_problem_t original_problem = fixed_problem;
    pbqp_solver_t solver;
    pbqp_solution_t fixed_solution;
    if (pbqp_solver_create_with_config(&solver, PBQP_MODE_ACCELERATOR, &kernel, &solver_config) !=
        PBQP_OK) {
      std::cerr << "PCAA model could not solve the graph\n";
      return 1;
    }
    const pbqp_status_t status = pbqp_solver_solve(&solver, &fixed_problem, &fixed_solution);
    if (status == PBQP_IRREDUCIBLE) {
      std::cout << "status IRREDUCIBLE\nstrategy " << strategy_name(solver_config.strategy) << '\n';
      return 0;
    }
    if (status != PBQP_OK) {
      std::cerr << "PCAA model could not solve the graph\n";
      return 1;
    }
    solution.optimum = fixed_solution.optimum;
    solution.assignment.assign(fixed_solution.assignment,
                               fixed_solution.assignment + input.nodes.size());
    const int32_t evaluated = pbqp_evaluate(&original_problem, fixed_solution.assignment);
    if (evaluated != fixed_solution.optimum) {
      std::cerr << "PBQP solver returned an objective inconsistent with its assignment\n";
      return 1;
    }
    solution.exact = solver_config.strategy == PBQP_STRATEGY_EXACT_BRANCH_REDUCE;
    statistics = &fixed_problem.statistics;
  } else {
    if (solver_mode == SolverMode::kBareMetal) {
      std::cerr << "local-search is available only with --solver local\n";
      return 2;
    }
    solution = solve_large_problem(input, kernel);
  }
  std::cout << "optimum " << solution.optimum << "\nassignment";
  for (unsigned value : solution.assignment) {
    std::cout << ' ' << value;
  }
  const char *solution_kind = solution.exact ? "exact" : "local-optimum";
  if (uses_shared_solver && !solution.exact) {
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
              << " R2=" << statistics->r2_after_rn << '\n';
    std::cerr << "pcaa: RN traffic projection-read=" << statistics->rn_projection_operand_bytes
              << " projection-write=" << statistics->rn_projection_result_bytes
              << " score-accumulation=" << statistics->rn_score_accumulation_elements
              << " commit-bytes=" << statistics->rn_commit_bytes << '\n';
    std::cerr << "pcaa: exact search nodes=" << statistics->search_nodes_visited
              << " branches=" << statistics->search_branches_created
              << " max-depth=" << statistics->search_maximum_depth
              << " limit-hits=" << statistics->search_limit_hits << '\n';
  }
#if defined(PCAA_GRAPH_RUN_TIMED)
  const AccelTimingStatistics &timing = model.timing_statistics();
  std::cout << "timing cycles=" << timing.total_service_cycles
            << " descriptor=" << timing.descriptor_cycles
            << " operands=" << timing.operand_read_cycles << " compute=" << timing.compute_cycles
            << " result=" << timing.result_write_cycles << " primitives=" << timing.primitive_count
            << " batches=" << timing.batch_count << '\n';
  if (timing.primitive_count == 0) {
    std::cout << "timing note=no accelerator primitives were issued; the PBQP core was solved "
                 "in software\n";
  }
#endif
  return 0;
}
