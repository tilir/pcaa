// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Keeps the native benchmark input and capacities identical to the host runner.
#pragma once
#include "pbqp/pbqp.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>
namespace pcaa::tools {

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

bool load_problem(const std::filesystem::path &path, InputProblem &problem);
enum class SolverMode { kBareMetal, kLocal };
pbqp_status_t build_problem(const InputProblem &input, SolverMode mode, pbqp_problem_t &problem);

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

}  // namespace pcaa::tools
