// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Measures bounded exact search with identical native/model policy and archived phase work.
#include "exact.h"
#include "kernels.h"
#include "profile.h"
#include "pbqp_input.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cpu_baseline;
using namespace pcaa::tools;
namespace {
constexpr size_t kWorkspaceLimit = 512 * 1024 * 1024;
constexpr int kDefaultSamples = 7;
constexpr int kDiagnosticSamples = 3;
constexpr unsigned kDefaultSearchLimit = 128;
constexpr std::array<const char *, Components> kNames = {
    "other",        "topology", "selection", "reduction", "reconstruction",
    "kernel",       "metadata", "search",    "clone",     "allocation",
    "conditioning", "r0",       "r1",        "r2",        "lower_bound",
    "incumbent",    "seed"};
template <typename T>
void ArrayJson(const T &values) {
  std::cout << '[';
  bool comma = false;
  for (const auto &value : values) {
    if (comma)
      std::cout << ',';
    std::cout << value;
    comma = true;
  }
  std::cout << ']';
}
void Histogram(const std::map<unsigned, uint64_t> &values) {
  std::cout << '{';
  bool comma = false;
  for (const auto &[key, value] : values) {
    if (comma)
      std::cout << ',';
    std::cout << '"' << key << "\":" << value;
    comma = true;
  }
  std::cout << '}';
}
class Workspace {
 public:
  explicit Workspace(bool reuse) : reuse_(reuse) {}
  ~Workspace() {
    for (auto &[size, blocks] : free_)
      for (void *block : blocks) std::free(block);
  }
  Workspace(const Workspace &) = delete;
  Workspace &operator=(const Workspace &) = delete;
  pbqp_allocator_t Allocator() {
    return {this, Allocate, Deallocate};
  }
  uint64_t allocations = 0, bytes = 0, peak = 0, failures = 0;

 private:
  static void *Allocate(void *context, size_t size) {
    auto &self = *static_cast<Workspace *>(context);
    PhaseScope scope(Allocation);
    if (size > kWorkspaceLimit - self.live_) {
      ++self.failures;
      return nullptr;
    }
    void *block = nullptr;
    if (self.reuse_ && !self.free_[size].empty()) {
      block = self.free_[size].back();
      self.free_[size].pop_back();
    } else
      block = std::malloc(size);
    if (block) {
      self.live_ += size;
      ++self.allocations;
      self.bytes += size;
      self.peak = std::max<uint64_t>(self.peak, self.live_);
    } else
      ++self.failures;
    return block;
  }
  static void Deallocate(void *context, void *block, size_t size) {
    auto &self = *static_cast<Workspace *>(context);
    PhaseScope scope(Allocation);
    self.live_ -= size;
    if (self.reuse_)
      self.free_[size].push_back(block);
    else
      std::free(block);
  }
  bool reuse_;
  size_t live_ = 0;
  std::map<size_t, std::vector<void *>> free_;
};
struct Event {
  int phase;
  const char *kind;
  size_t count;
  uint64_t elements, ns;
  uint64_t operand_bytes = 0, result_bytes = 0;
  DeviceEvent device;
};
class Callbacks {
 public:
  explicit Callbacks(pbqp_cost_kernel_t kernel) : inner_(kernel) {}
  bool recording = false;
  std::vector<Event> events;
  DeviceRecorder *device = nullptr;
  pbqp_cost_kernel_t Api() {
    pbqp_cost_kernel_t result{};
    result.context = this;
    result.min2_argmin = Min2;
    result.min3_argmin = Min3;
    result.min2_argmin_batch = Min2Batch;
    result.min3_argmin_batch = Min3Batch;
    result.project_add_batch = ProjectBatch;
    result.map3_project_batch = Map3Batch;
    result.cost_add_vector = Add;
    result.set_statistics = Statistics;
    return result;
  }

 private:
  template <typename F>
  int Call(const char *kind, size_t count, uint64_t elements, F execute, uint64_t operand_bytes = 0,
           uint64_t result_bytes = 0) {
    if (!recording)
      return execute();
    Event event{};
    event.phase = profile->phase;
    event.kind = kind;
    event.count = count;
    event.elements = elements;
    event.operand_bytes = operand_bytes;
    event.result_bytes = result_bytes;
    if (device)
      device->Before();
    const auto start = Now();
    const int status = execute();
    event.ns = Now() - start;
    if (device)
      event.device = device->After();
    events.push_back(event);
    return status;
  }
  static int Min2(void *p, pbqp_vector_view_t a, pbqp_vector_view_t b,
                  accel_min_argmin_result_t *r) {
    auto &s = *static_cast<Callbacks *>(p);
    return s.Call(
        "min2", 1, a.length, [&] { return s.inner_.min2_argmin(s.inner_.context, a, b, r); },
        2 * a.length * sizeof(int32_t), sizeof(accel_min_argmin_result_t));
  }
  static int Min3(void *p, pbqp_vector_view_t a, pbqp_vector_view_t b, pbqp_vector_view_t c,
                  accel_min_argmin_result_t *r) {
    auto &s = *static_cast<Callbacks *>(p);
    return s.Call(
        "min3", 1, a.length, [&] { return s.inner_.min3_argmin(s.inner_.context, a, b, c, r); },
        3 * a.length * sizeof(int32_t), sizeof(accel_min_argmin_result_t));
  }
  static int Min2Batch(void *p, const pbqp_min2_job_t *jobs, size_t count) {
    auto &s = *static_cast<Callbacks *>(p);
    if (!s.recording)
      return s.inner_.min2_argmin_batch(s.inner_.context, jobs, count);
    uint64_t elements = 0;
    for (size_t i = 0; i < count; ++i) elements += jobs[i].a.length;
    return s.Call(
        "min2_batch", count, elements,
        [&] { return s.inner_.min2_argmin_batch(s.inner_.context, jobs, count); },
        2 * elements * sizeof(int32_t), count * sizeof(accel_min_argmin_result_t));
  }
  static int Min3Batch(void *p, const pbqp_min3_job_t *jobs, size_t count) {
    auto &s = *static_cast<Callbacks *>(p);
    if (!s.recording)
      return s.inner_.min3_argmin_batch(s.inner_.context, jobs, count);
    uint64_t elements = 0;
    for (size_t i = 0; i < count; ++i) elements += jobs[i].a.length;
    return s.Call(
        "min3_batch", count, elements,
        [&] { return s.inner_.min3_argmin_batch(s.inner_.context, jobs, count); },
        3 * elements * sizeof(int32_t), count * sizeof(accel_min_argmin_result_t));
  }
  static int ProjectBatch(void *p, const pbqp_project_add_job_t *jobs, size_t count) {
    auto &s = *static_cast<Callbacks *>(p);
    if (!s.recording)
      return s.inner_.project_add_batch(s.inner_.context, jobs, count);
    uint64_t elements = 0;
    uint64_t words = 0, results = 0;
    for (size_t i = 0; i < count; ++i) {
      elements += jobs[i].matrix.rows * jobs[i].matrix.columns + jobs[i].matrix.rows;
      words += jobs[i].matrix.rows * jobs[i].matrix.columns + jobs[i].matrix.columns +
               2 * jobs[i].matrix.rows;
      results += 2 * jobs[i].matrix.rows;
    }
    return s.Call(
        "project_add_batch", count, elements,
        [&] { return s.inner_.project_add_batch(s.inner_.context, jobs, count); },
        words * sizeof(int32_t), results * sizeof(int32_t));
  }
  static int Map3Batch(void *p, const pbqp_map3_project_job_t *jobs, size_t count) {
    auto &s = *static_cast<Callbacks *>(p);
    if (!s.recording)
      return s.inner_.map3_project_batch(s.inner_.context, jobs, count);
    uint64_t elements = 0;
    uint64_t words = 0, results = 0;
    for (size_t i = 0; i < count; ++i) {
      elements += jobs[i].varying_edge.rows * jobs[i].varying_edge.columns;
      words += jobs[i].varying_edge.rows * jobs[i].varying_edge.columns +
               2 * jobs[i].varying_edge.columns;
      results += jobs[i].varying_edge.rows;
    }
    return s.Call(
        "map3_project_batch", count, elements,
        [&] { return s.inner_.map3_project_batch(s.inner_.context, jobs, count); },
        words * sizeof(int32_t), results * sizeof(accel_min_argmin_result_t));
  }
  static int Add(void *p, pbqp_vector_view_t a, pbqp_vector_view_t b, int32_t *r) {
    auto &s = *static_cast<Callbacks *>(p);
    return s.Call(
        "add", 1, a.length, [&] { return s.inner_.cost_add_vector(s.inner_.context, a, b, r); },
        2 * a.length * sizeof(int32_t), a.length * sizeof(int32_t));
  }
  static void Statistics(void *p, pbqp_statistics_t *statistics) {
    auto &s = *static_cast<Callbacks *>(p);
    if (s.inner_.set_statistics)
      s.inner_.set_statistics(s.inner_.context, statistics);
  }
  pbqp_cost_kernel_t inner_;
};
void Run(const std::filesystem::path &path, const std::string &variant, bool seeded, unsigned limit,
         int samples, bool vector_conditioning, pbqp_solver_strategy_t strategy,
         DeviceRecorder *device) {
  InputProblem input;
  if (!load_problem(path, input))
    throw std::runtime_error("invalid graph");
  ProblemOwner original;
  const auto built = build_problem(input, SolverMode::kLocal, original.value);
  if (built != PBQP_OK)
    throw std::runtime_error("graph status " + std::to_string(built));
  Profile profiler;
  profiler.degrees = variant != "current";
  profiler.fast_clone = variant != "current" && variant != "degree";
  profiler.vector_conditioning = vector_conditioning;
  profile = &profiler;
  const Level level = variant == "structured"                       ? Level::Structured
                      : variant == "current" || variant == "degree" ? Level::Scalar
                                                                    : Level::Dense;
  Kernels native(level);
  active_kernels = &native;
  pbqp_cost_kernel_t backend{};
  native.Make(backend);
  if (device)
    device->MakeKernel(backend);
  Callbacks callbacks(backend);
  callbacks.device = device;
  Workspace workspace(profiler.fast_clone);
  auto api = callbacks.Api();
  std::vector<unsigned> expected;
  int expected_status = -1;
  int32_t expected_objective = 0;
  std::vector<uint64_t> floors;
  for (int i = 0; i < 1001; ++i) {
    auto start = Now();
    floors.push_back(Now() - start);
  }
  std::sort(floors.begin(), floors.end());
  std::cout << "{\"type\":\"calibration\",\"ns\":" << floors[floors.size() / 2] << "}\n";
  auto solve = [&](const char *type, bool detail, bool counters, bool record) {
    ProblemOwner problem;
    if (pbqp_problem_clone(&problem.value, &original.value, pbqp_heap_allocator()) != PBQP_OK)
      throw std::runtime_error("root clone failed");
    native.Reset();
    callbacks.events.clear();
    callbacks.recording = record;
    std::vector<unsigned> assignment(input.nodes.size()), seed(input.nodes.size());
    profiler.seed_assignment = nullptr;
    profiler.counters = counters;
    profiler.exact = {};
    profiler.ns = {};
    profiler.current = Other;
    profiler.phase = Other;
    profiler.enabled = detail;
    profiler.stamp = std::chrono::steady_clock::now();
    profiler.solve_start = Now();
    profiler.observed_best = ACCEL_INF;
    const auto previous_allocations = workspace.allocations;
    const auto previous_bytes = workspace.bytes;
    const auto previous_failures = workspace.failures;
    profiler.first_solution_ns = profiler.optimum_found_ns = 0;
    uint64_t seed_ns = 0;
    if (seeded) {
      PhaseScope seed_scope(Seed);
      ProblemOwner seed_problem;
      auto start = Now();
      if (pbqp_problem_clone(&seed_problem.value, &original.value, pbqp_heap_allocator()) !=
          PBQP_OK)
        throw std::runtime_error("seed clone failed");
      Kernels seed_kernel(Level::Dense);
      pbqp_cost_kernel_t seed_api{};
      seed_kernel.Make(seed_api);
      auto *saved = active_kernels;
      active_kernels = &seed_kernel;
      pbqp_solver_t solver;
      auto conf = pbqp_solver_default_config();
      conf.strategy = PBQP_STRATEGY_HEURISTIC_RN;
      conf.workspace_allocator = workspace.Allocator();
      pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &seed_api, &conf);
      pbqp_solution_t solution;
      pbqp_solution_init(&solution, seed.data(), seed.size());
      const auto seed_execution = MakeExecution(profiler, detail);
      const auto status =
          pcaa::pbqp::SolveWithExecution(solver, seed_problem.value, solution, seed_execution);
      active_kernels = saved;
      if (status != PBQP_OK)
        throw std::runtime_error("seed failed");
      profiler.seed_assignment = seed.data();
      profiler.seed_objective = solution.optimum;
      profiler.seed_length = seed.size();
      seed_ns = Now() - start;
      ObserveSolution(solution.optimum);
      profiler.exact = {};
      native.ForgetMatrices();
    }
    pbqp_solver_t solver;
    auto conf = pbqp_solver_default_config();
    conf.strategy = strategy;
    conf.maximum_search_nodes = limit;
    conf.workspace_allocator = workspace.Allocator();
    pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &api, &conf);
    pbqp_solution_t solution;
    pbqp_solution_init(&solution, assignment.data(), assignment.size());
    const auto execution = MakeExecution(profiler, detail || counters || record);
    const auto status = pcaa::pbqp::SolveWithExecution(solver, problem.value, solution, execution);
    const auto elapsed = Now() - profiler.solve_start;
    if (detail)
      profiler.Charge();
    profiler.enabled = false;
    profiler.counters = false;
    if (status == PBQP_OK && pbqp_evaluate(&original.value, assignment.data()) != solution.optimum)
      throw std::runtime_error("invalid assignment");
    if (status == PBQP_OK)
      for (size_t i = 0; i < assignment.size(); ++i)
        if (assignment[i] >= input.nodes[i].unary.size())
          throw std::runtime_error("assignment outside domain");
    if (expected_status == -1) {
      expected_status = status;
      expected = assignment;
      expected_objective = solution.optimum;
    }
    if (expected_status != status ||
        (status == PBQP_OK && (expected != assignment || expected_objective != solution.optimum)))
      throw std::runtime_error("unstable exact result");
    if (std::string(type) == "warmup")
      return;
    std::cout << "{\"type\":\"" << type << "\",\"status\":" << status << ",\"ns\":" << elapsed
              << ",\"seed_ns\":" << seed_ns << ",\"objective\":" << solution.optimum
              << ",\"assignment\":";
    if (status == PBQP_OK)
      ArrayJson(assignment);
    else
      std::cout << "null";
    std::cout << ",\"components\":{";
    for (int c = 0; c < Components; ++c) {
      if (c)
        std::cout << ',';
      std::cout << '"' << kNames[c] << "\":" << profiler.ns[c];
    }
    const auto &x = profiler.exact;
    std::cout << "},\"work\":{\"visited\":" << x.visited << ",\"branches\":" << x.branches
              << ",\"pruned\":" << x.pruned << ",\"depth\":" << x.depth
              << ",\"clone_bytes\":" << x.clone_bytes << ",\"lower_unary\":" << x.lower_unary
              << ",\"lower_matrix\":" << x.lower_matrix << ",\"conditioning\":" << x.conditioning
              << ",\"r0\":" << x.r0 << ",\"r1\":" << x.r1 << ",\"r2\":" << x.r2 << ",\"domains\":";
    Histogram(x.domains);
    std::cout << ",\"cores\":";
    Histogram(x.cores);
    std::cout << "},\"first_solution_ns\":" << profiler.first_solution_ns
              << ",\"optimum_found_ns\":" << profiler.optimum_found_ns
              << ",\"allocations\":" << workspace.allocations - previous_allocations
              << ",\"allocated_bytes\":" << workspace.bytes - previous_bytes
              << ",\"workspace_failures\":" << workspace.failures - previous_failures
              << ",\"peak_workspace\":" << workspace.peak << ",\"events\":[";
    for (size_t i = 0; i < callbacks.events.size(); ++i) {
      if (i)
        std::cout << ',';
      const auto &e = callbacks.events[i];
      std::cout << "{\"phase\":\"" << kNames[e.phase] << "\",\"kind\":\"" << e.kind
                << "\",\"count\":" << e.count << ",\"elements\":" << e.elements
                << ",\"ns\":" << e.ns << ",\"operand_bytes\":" << e.operand_bytes
                << ",\"result_bytes\":" << e.result_bytes;
      if (device) {
        const auto &d = e.device;
        std::cout << ",\"cycles\":" << d.cycles << ",\"phases\":";
        ArrayJson(d.phases);
        std::cout << ",\"opcodes\":";
        ArrayJson(d.opcodes);
        std::cout << ",\"host_ns\":" << d.host_ns << ",\"staging_ns\":" << d.staging_ns
                  << ",\"build_ns\":" << d.build_ns << ",\"readback_ns\":" << d.readback_ns;
      }
      std::cout << '}';
    }
    std::cout << ']';
    if (device) {
      std::cout << ",\"l2\":";
      device->WriteSummary(std::cout);
    }
    std::cout << "}\n" << std::flush;
  };
  solve("warmup", false, false, false);
  if (device) {
    std::cout << "{\"type\":\"baseline\",\"l2\":";
    device->WriteSummary(std::cout);
    std::cout << "}\n";
    solve("model", false, true, true);
  } else {
    for (int i = 0; i < samples; ++i) solve("sample", false, false, false);
    for (int i = 0; i < kDiagnosticSamples; ++i) solve("events", false, true, true);
    for (int i = 0; i < kDiagnosticSamples; ++i) solve("profile", true, true, false);
  }
  profile = nullptr;
  active_kernels = nullptr;
}
int Main(int argc, char **argv, DeviceRecorder *device) {
  try {
    if (argc < 5 || argc > 8)
      throw std::runtime_error(
          "Usage: EXACT current|degree|dense|structured INPUT none|heuristic LIMIT [SAMPLES] "
          "[vector|scalar] [branch|enumeration|heuristic]");
    const std::string variant = argv[1];
    if (variant != "current" && variant != "degree" && variant != "dense" &&
        variant != "structured")
      throw std::runtime_error("bad variant");
    const std::string seed = argv[3];
    if (seed != "none" && seed != "heuristic")
      throw std::runtime_error("bad incumbent");
    const unsigned long parsed_limit = std::stoul(argv[4]);
    if (argv[4][0] == '-' || !parsed_limit || parsed_limit > std::numeric_limits<unsigned>::max())
      throw std::runtime_error("finite search limit required");
    const unsigned limit = static_cast<unsigned>(parsed_limit);
    const int samples = argc > 5 ? std::stoi(argv[5]) : kDefaultSamples;
    if (samples < 3)
      throw std::runtime_error("at least three samples");
    const std::string conditioning = argc > 6 ? argv[6] : "vector";
    const std::string strategy = argc > 7 ? argv[7] : "branch";
    if (conditioning != "vector" && conditioning != "scalar")
      throw std::runtime_error("bad conditioning mode");
    if (strategy != "branch" && strategy != "enumeration" && strategy != "heuristic")
      throw std::runtime_error("bad strategy");
    Run(argv[2], variant, seed == "heuristic", limit, samples, conditioning == "vector",
        strategy == "heuristic"     ? PBQP_STRATEGY_HEURISTIC_RN
        : strategy == "enumeration" ? PBQP_STRATEGY_EXACT_CORE_ENUMERATION
                                    : PBQP_STRATEGY_EXACT_BRANCH_REDUCE,
        device);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
}  // namespace
namespace cpu_baseline {
int RunExact(int argc, char **argv, DeviceRecorder *device) {
  return Main(argc, argv, device);
}
}  // namespace cpu_baseline
