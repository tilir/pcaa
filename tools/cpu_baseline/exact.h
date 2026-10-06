// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Separates the native exact driver from optional hosted device recording.
#pragma once
#include "pbqp/pbqp.h"
#include <cstdint>
#include <ostream>
#include <vector>
namespace cpu_baseline {
struct DeviceEvent {
  uint64_t cycles = 0, host_ns = 0, staging_ns = 0, build_ns = 0, readback_ns = 0;
  std::vector<uint64_t> phases, opcodes;
};
class DeviceRecorder {
 public:
  virtual ~DeviceRecorder() = default;
  virtual void MakeKernel(pbqp_cost_kernel_t &kernel) = 0;
  virtual void Before() = 0;
  virtual DeviceEvent After() = 0;
  virtual void WriteSummary(std::ostream &out) const = 0;
};
int RunExact(int argc, char **argv, DeviceRecorder *device = nullptr);
}  // namespace cpu_baseline
