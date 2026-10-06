// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Hosts PBQP cost callbacks using guest staging and the SystemC device.

#pragma once

#include "pbqp/pbqp.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

struct L2Config;
struct L2Statistics;
struct AccelTimingConfig;
struct AccelTimingStatistics;

struct CycleBreakdown {
  uint64_t descriptor = 0;
  uint64_t operands = 0;
  uint64_t compute = 0;
  uint64_t result = 0;
  uint64_t total = 0;
  uint64_t descriptors = 0;
};

struct VectorCycleProjection {
  CycleBreakdown project_scalar;
  CycleBreakdown project_vector;
  CycleBreakdown map3_scalar;
  CycleBreakdown map3_partial;
  CycleBreakdown map3_full;
};

// Optional hosted measurements; simulated execution/wait time is excluded from
// host preparation. Counts/cycles retain the existing ordered batch boundary.
struct HostKernelMeasurement {
  const char *kind;
  size_t count;
  uint64_t cycles;
  uint64_t host_ns;
  uint64_t staging_ns;
  uint64_t submission_build_ns;
  uint64_t readback_ns;
};

class ModelKernel {
 public:
  ModelKernel(bool verbose, AccelTimingConfig timing,
              size_t staging_limit = std::numeric_limits<size_t>::max(),
              const L2Config *l2 = nullptr);
  ~ModelKernel();
  void make_kernel(pbqp_cost_kernel_t &kernel);
  const AccelTimingStatistics &timing_statistics() const;
  const VectorCycleProjection &vector_cycle_projection() const;
  const L2Statistics &l2_statistics() const;
  void enable_measurements();
  const std::vector<HostKernelMeasurement> &measurements() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
