// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Declares the parameterized L1 architectural cycle model for PCAA commands.

#pragma once

#include "accel_protocol.h"

#include <cstdint>

#include <sysc/kernel/sc_time.h>

enum class AccelTimingMode { kUntimed, kL1Sequential, kL1Streaming };

struct AccelTimingConfig {
  AccelTimingMode mode = AccelTimingMode::kUntimed;
  unsigned lanes = 1;
  unsigned descriptor_bytes_per_cycle = sizeof(accel_command_t);
  unsigned memory_read_bytes_per_cycle = sizeof(int32_t);
  unsigned memory_write_bytes_per_cycle = sizeof(int32_t);
  unsigned batch_start_cycles = 0;
  unsigned primitive_start_cycles = 0;
  unsigned map_pipeline_latency = 0;
  unsigned add3_map_extra_latency = 0;
  unsigned reduction_tree_latency = 0;
  unsigned result_latency = 0;
  sc_core::sc_time cycle_period = sc_core::SC_ZERO_TIME;
};

struct AccelTimingStatistics {
  uint64_t descriptor_cycles = 0;
  uint64_t operand_read_cycles = 0;
  uint64_t compute_cycles = 0;
  uint64_t result_write_cycles = 0;
  uint64_t total_service_cycles = 0;
  uint64_t primitive_count = 0;
  uint64_t batch_count = 0;
  uint64_t memory_dominant_primitives = 0;
  uint64_t compute_dominant_primitives = 0;
  uint64_t lane_slots = 0;
  uint64_t active_lane_elements = 0;
};

struct AccelCommandTiming {
  uint64_t descriptor_cycles = 0;
  uint64_t operand_read_cycles = 0;
  uint64_t compute_cycles = 0;
  uint64_t result_write_cycles = 0;
  uint64_t total_cycles = 0;
};

AccelCommandTiming accel_estimate_command_cycles(const accel_command_t &command,
                                                 const AccelTimingConfig &config);
void accel_accumulate_command_timing(const accel_command_t &command,
                                     const AccelTimingConfig &config,
                                     AccelTimingStatistics *statistics);
