// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Provides optional exclusive host profiling for the isolated CPU experiment.
#pragma once
#include "pbqp/pbqp.h"
#include "pbqp/pbqp_execution.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace cpu_baseline {
enum Component {
  Other,
  Topology,
  Selection,
  Reduction,
  Reconstruction,
  Kernel,
  Metadata,
  Search,
  Clone,
  Allocation,
  Conditioning,
  R0,
  R1,
  R2,
  LowerBound,
  Incumbent,
  Seed,
  Components
};
struct ExactCounters {
  uint64_t visited = 0, branches = 0, pruned = 0, clone_bytes = 0;
  uint64_t lower_unary = 0, lower_matrix = 0, conditioning = 0;
  uint64_t r0 = 0, r1 = 0, r2 = 0;
  unsigned depth = 0;
  std::map<unsigned, uint64_t> domains, cores;
};
struct Profile : pcaa::pbqp::ExecutionObserver, pcaa::pbqp::KernelObserver {
  bool enabled = false;
  bool degrees = false;
  bool fast_clone = false;
  bool vector_conditioning = false;
  bool counters = false;
  uint64_t mask = ~uint64_t{0};
  int phase = Other;
  const unsigned *seed_assignment = nullptr;
  size_t seed_length = 0;
  int32_t seed_objective = ACCEL_INF;
  uint64_t solve_start = 0, optimum_found_ns = 0, first_solution_ns = 0;
  int32_t observed_best = ACCEL_INF;
  ExactCounters exact;
  struct Frame {
    int phase, current;
    bool active;
  };
  std::vector<Frame> frames;
  int current = Other;
  std::chrono::steady_clock::time_point stamp;
  std::array<uint64_t, Components> ns{};
  void Charge();
  void Enter(pcaa::pbqp::Phase phase) override;
  void Leave(pcaa::pbqp::Phase phase) override;
  void Record(pcaa::pbqp::Work kind, uint64_t count, uint64_t detail) override;
  void Observe(int32_t objective) override;
  void MatrixChanged(const int32_t *base) override;
  void ForgetMatrices() override;
  void R1Projection(pbqp_matrix_view_t matrix, pbqp_vector_view_t unary,
                    accel_min_argmin_result_t *results) override;
};
pcaa::pbqp::Execution MakeExecution(Profile &profile, bool observe = true);
extern thread_local Profile *profile;
// Invalidates all oriented representations of an R2-modified/reused edge slot.
void MatrixChanged(const int32_t *base);
void ForgetMatrices();
void ObserveSolution(int32_t objective);
void R1Projection(pbqp_matrix_view_t matrix, pbqp_vector_view_t unary,
                  accel_min_argmin_result_t *results);
class Scope {
 public:
  explicit Scope(int component)
      : active_(profile && profile->enabled && (profile->mask & (uint64_t{1} << component)) &&
                profile->current != component) {
    if (active_) {
      profile->Charge();
      previous_ = profile->current;
      profile->current = component;
    }
  }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
  ~Scope() {
    if (active_) {
      profile->Charge();
      profile->current = previous_;
    }
  }

 private:
  bool active_;
  int previous_ = Other;
};
// Phase identity remains available to callback accounting without clock reads.
class PhaseScope {
 public:
  explicit PhaseScope(int phase) : scope_(phase), previous_(profile ? profile->phase : Other) {
    if (profile)
      profile->phase = phase;
  }
  ~PhaseScope() {
    if (profile)
      profile->phase = previous_;
  }
  PhaseScope(const PhaseScope &) = delete;
  PhaseScope &operator=(const PhaseScope &) = delete;

 private:
  Scope scope_;
  int previous_;
};
}  // namespace cpu_baseline
