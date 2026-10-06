// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Measures native solves without SystemC, parsing, or serialization in timed regions.
#include "pbqp_input.h"
#include "kernels.h"
#include "profile.h"
#include "pbqp/pbqp.h"
#include <exception>
#include <cstdint>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cpu_baseline;
using pcaa::tools::build_problem;
using pcaa::tools::InputProblem;
using pcaa::tools::load_problem;
using pcaa::tools::ProblemOwner;
using pcaa::tools::SolverMode;
namespace {
constexpr int kDefaultSamples = 11;
constexpr int kWarmups = 2;
constexpr int kProfileSamples = 5;
void EmitArray(const std::vector<unsigned> &array) {
  std::cout << '[';
  for (size_t i = 0; i < array.size(); ++i) {
    if (i)
      std::cout << ',';
    std::cout << array[i];
  }
  std::cout << ']';
}
void Benchmark(const std::filesystem::path &path, Level level, bool degrees, int repetitions) {
  std::vector<uint64_t> floors;
  for (int i = 0; i < 1001; ++i) {
    const auto first = Now();
    floors.push_back(Now() - first);
  }
  std::sort(floors.begin(), floors.end());
  std::cout << "{\"type\":\"calibration\",\"clock_pair_ns\":" << floors[floors.size() / 2] << "}\n";
  InputProblem input;
  if (!load_problem(path, input))
    throw std::runtime_error("invalid input");
  ProblemOwner original;
  if (build_problem(input, SolverMode::kLocal, original.value) != PBQP_OK)
    throw std::runtime_error("cannot build graph");
  Kernels kernels(level);
  Profile profiler;
  profiler.degrees = degrees;
  profile = &profiler;
  active_kernels = &kernels;
  std::vector<unsigned> expected;
  int32_t objective = 0;
  auto solve = [&](bool instrument, bool detail, bool structure, int sample) {
    ProblemOwner problem;
    if (pbqp_problem_clone(&problem.value, &original.value, pbqp_heap_allocator()) != PBQP_OK)
      throw std::runtime_error("cannot clone graph");
    std::vector<unsigned> assignment(input.nodes.size());
    pbqp_solution_t solution;
    pbqp_solution_init(&solution, assignment.data(), assignment.size());
    pbqp_cost_kernel_t kernel;
    kernels.Reset();
    kernels.record = instrument;
    kernels.class_record = structure;
    kernels.Make(kernel);
    pbqp_solver_t solver;
    auto config = pbqp_solver_default_config();
    config.strategy = PBQP_STRATEGY_HEURISTIC_RN;
    if (pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &kernel, &config) != PBQP_OK)
      throw std::runtime_error("cannot create solver");
    profiler.ns = {};
    profiler.current = Other;
    profiler.enabled = detail;
    profiler.stamp = std::chrono::steady_clock::now();
    const uint64_t start = Now();
    const auto execution = MakeExecution(profiler, instrument || detail || structure);
    const auto status = pcaa::pbqp::SolveWithExecution(solver, problem.value, solution, execution);
    const uint64_t elapsed = Now() - start;
    if (detail)
      profiler.Charge();
    profiler.enabled = false;
    if (status != PBQP_OK || pbqp_evaluate(&original.value, assignment.data()) != solution.optimum)
      throw std::runtime_error("invalid solution/status " + std::to_string(status));
    if (expected.empty()) {
      expected = assignment;
      objective = solution.optimum;
    }
    if (expected != assignment || objective != solution.optimum)
      throw std::runtime_error("unstable solution");
    if (sample < 0)
      return;
    std::cout << "{\"type\":\""
              << (detail ? "profile"
                         : (structure ? "structure" : (instrument ? "events" : "sample")))
              << "\",\"sample\":" << sample << ",\"ns\":" << elapsed;
    if (detail) {
      std::cout << ",\"components\":[";
      for (int c = 0; c < Components; ++c) {
        if (c)
          std::cout << ',';
        std::cout << profiler.ns[c];
      }
      std::cout << "]";
    }
    if (instrument) {
      std::cout << ",\"batches\":[";
      for (size_t i = 0; i < kernels.events.size(); ++i) {
        const auto &event = kernels.events[i];
        if (i)
          std::cout << ',';
        std::cout << "{\"kind\":\"" << event.kind << "\",\"count\":" << event.count
                  << ",\"ns\":" << event.ns << ",\"elements\":" << event.elements << '}';
      }
      std::cout << "],\"classes\":[";
      for (int c = 0; c < MatrixClasses; ++c) {
        const auto &s = kernels.classes[c];
        if (c)
          std::cout << ',';
        std::cout << "{\"calls\":" << s.calls << ",\"elements\":" << s.elements
                  << ",\"bytes\":" << s.bytes << ",\"ns\":" << s.ns
                  << ",\"evaluated\":" << s.evaluated << ",\"changed_calls\":" << s.changed_calls
                  << ",\"changed_elements\":" << s.changed_elements
                  << ",\"changed_ns\":" << s.changed_ns << ",\"changed_bytes\":" << s.changed_bytes
                  << ",\"preadd_elements\":" << s.preadd_elements
                  << ",\"predicate_elements\":" << s.predicate_elements << '}';
      }
      std::cout << ']';
      if (structure) {
        std::cout << ",\"projections\":[";
        for (size_t i = 0; i < kernels.projections.size(); ++i) {
          const auto &p = kernels.projections[i];
          if (i)
            std::cout << ',';
          std::cout << "{\"m\":" << p.rows << ",\"n\":" << p.columns
                    << ",\"column_stride\":" << p.column_stride << ",\"class\":" << p.kind
                    << ",\"changed\":" << (p.changed ? "true" : "false")
                    << ",\"add3\":" << (p.add3 ? "true" : "false") << ",\"ns\":" << p.ns << '}';
        }
        std::cout << ']';
      }
    }
    std::cout << ",\"r0\":" << problem.value.statistics.r0_count
              << ",\"r1\":" << problem.value.statistics.r1_count
              << ",\"r2\":" << problem.value.statistics.r2_count
              << ",\"rn\":" << problem.value.statistics.rn_count << "}\n";
  };
  for (int i = 0; i < kWarmups; ++i) solve(false, false, false, -1);
  for (int i = 0; i < repetitions; ++i) solve(false, false, false, i);
  for (int i = 0; i < kProfileSamples; ++i) solve(true, false, false, i);
  for (int i = 0; i < kProfileSamples; ++i) solve(false, true, false, i);
  if (level == Level::Structured)
    for (int i = 0; i < kProfileSamples; ++i) solve(true, false, true, i);
  std::cout << "{\"type\":\"answer\",\"objective\":" << objective << ",\"assignment\":";
  EmitArray(expected);
  std::cout << ",\"nodes\":" << input.nodes.size() << ",\"edges\":" << input.edges.size() << "}\n";
  profile = nullptr;
  active_kernels = nullptr;
}
}  // namespace
int main(int argc, char **argv) {
  try {
    if (argc < 3 || argc > 4) {
      std::cerr << "Usage: pcaa_cpu_bench current|scalar|dense|structured INPUT [SAMPLES]\n";
      return 2;
    }
    const std::string variant = argv[1];
    const Level level = variant == "structured" ? Level::Structured
                        : variant == "dense"    ? Level::Dense
                                                : Level::Scalar;
    if (variant != "current" && variant != "scalar" && variant != "dense" &&
        variant != "structured")
      throw std::runtime_error("unknown variant");
    const int samples = argc == 4 ? std::stoi(argv[3]) : kDefaultSamples;
    if (samples < 3)
      throw std::runtime_error("at least three samples required");
    Benchmark(argv[2], level, variant != "current", samples);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
