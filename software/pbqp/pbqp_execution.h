// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Provides optional C++ execution policy and observation without hosted dependencies.
#pragma once
#include "pbqp.h"

namespace pcaa::pbqp {
enum class Phase {
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
  Seed
};
enum class Work {
  Visit,
  Branch,
  Prune,
  CloneBytes,
  LowerUnary,
  LowerMatrix,
  Condition,
  R0,
  R1,
  R2,
  BranchPoint
};
class ExecutionObserver {
 public:
  virtual void Enter(Phase phase) = 0;
  virtual void Leave(Phase phase) = 0;
  virtual void Record(Work kind, uint64_t count, uint64_t detail) = 0;
  virtual void Observe(int32_t objective) = 0;

 protected:
  ~ExecutionObserver() = default;
};
class KernelObserver {
 public:
  virtual void MatrixChanged(const int32_t *base) = 0;
  virtual void ForgetMatrices() = 0;
  virtual void R1Projection(pbqp_matrix_view_t matrix, pbqp_vector_view_t unary,
                            accel_min_argmin_result_t *results) = 0;

 protected:
  ~KernelObserver() = default;
};
struct Execution {
  bool cache_degrees = false;
  bool fast_clone = false;
  bool vector_conditioning = false;
  const unsigned *seed_assignment = nullptr;
  size_t seed_length = 0;
  int32_t seed_objective = ACCEL_INF;
  ExecutionObserver *observer = nullptr;
  KernelObserver *kernel_observer = nullptr;
  void Record(Work kind, uint64_t count = 1, uint64_t detail = 0) const {
    if (observer)
      observer->Record(kind, count, detail);
  }
  void Observe(int32_t objective) const {
    if (observer)
      observer->Observe(objective);
  }
  void MatrixChanged(const int32_t *base) const {
    if (kernel_observer)
      kernel_observer->MatrixChanged(base);
  }
  void ForgetMatrices() const {
    if (kernel_observer)
      kernel_observer->ForgetMatrices();
  }
  void R1Projection(pbqp_matrix_view_t matrix, pbqp_vector_view_t unary,
                    accel_min_argmin_result_t *results) const {
    if (kernel_observer)
      kernel_observer->R1Projection(matrix, unary, results);
  }
};
class ExecutionScope {
 public:
  ExecutionScope(const Execution &execution, Phase phase) : execution_(execution), phase_(phase) {
    if (execution_.observer)
      execution_.observer->Enter(phase_);
  }
  ~ExecutionScope() {
    if (execution_.observer)
      execution_.observer->Leave(phase_);
  }
  ExecutionScope(const ExecutionScope &) = delete;
  ExecutionScope &operator=(const ExecutionScope &) = delete;

 private:
  const Execution &execution_;
  Phase phase_;
};
// The C entry point uses the default policy. Hosted experiments opt in here;
// allocator, arithmetic, search order and the solver implementation are shared.
pbqp_status_t SolveWithExecution(pbqp_solver_t &solver, pbqp_problem_t &problem,
                                 pbqp_solution_t &solution, const Execution &execution);
}  // namespace pcaa::pbqp
