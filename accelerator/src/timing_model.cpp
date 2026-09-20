// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements simple parameterized L1 service-cycle estimates for PCAA commands.

#include "timing_model.h"

#include <algorithm>

namespace {

uint64_t divide_round_up(uint64_t value, unsigned divisor) {
  return (value + divisor - 1) / divisor;
}

bool is_add3(unsigned opcode) {
  return opcode == ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN ||
         opcode == ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN;
}

bool returns_argmin(unsigned opcode) {
  return opcode == ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN ||
         opcode == ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN;
}

unsigned operand_count(unsigned opcode) {
  return is_add3(opcode) ? 3 : 2;
}

}  // namespace

AccelCommandTiming accel_estimate_command_cycles(const accel_command_t &command,
                                                 const AccelTimingConfig &config) {
  if (config.mode == AccelTimingMode::kUntimed || config.lanes == 0 ||
      config.descriptor_bytes_per_cycle == 0 || config.memory_read_bytes_per_cycle == 0 ||
      config.memory_write_bytes_per_cycle == 0) {
    return {};
  }

  const uint64_t chunks = divide_round_up(command.n, config.lanes);
  const uint64_t operand_bytes =
      static_cast<uint64_t>(command.n) * operand_count(command.opcode) * sizeof(int32_t);
  const uint64_t result_bytes =
      returns_argmin(command.opcode) ? sizeof(accel_min_argmin_result_t) : sizeof(int32_t);
  AccelCommandTiming timing;
  timing.descriptor_cycles = divide_round_up(sizeof(command), config.descriptor_bytes_per_cycle);
  timing.operand_read_cycles = divide_round_up(operand_bytes, config.memory_read_bytes_per_cycle);
  timing.compute_cycles = config.primitive_start_cycles + config.map_pipeline_latency + chunks +
                          config.reduction_tree_latency + config.result_latency;
  if (is_add3(command.opcode)) {
    timing.compute_cycles += config.add3_map_extra_latency;
  }
  timing.result_write_cycles = divide_round_up(result_bytes, config.memory_write_bytes_per_cycle);
  if (config.mode == AccelTimingMode::kL1Sequential) {
    timing.total_cycles = timing.descriptor_cycles + timing.operand_read_cycles +
                          timing.compute_cycles + timing.result_write_cycles;
  } else {
    timing.total_cycles = timing.descriptor_cycles +
                          std::max(timing.operand_read_cycles, timing.compute_cycles) +
                          timing.result_write_cycles;
  }
  return timing;
}

void accel_accumulate_command_timing(const accel_command_t &command,
                                     const AccelTimingConfig &config,
                                     AccelTimingStatistics *statistics) {
  const AccelCommandTiming timing = accel_estimate_command_cycles(command, config);
  statistics->descriptor_cycles += timing.descriptor_cycles;
  statistics->operand_read_cycles += timing.operand_read_cycles;
  statistics->compute_cycles += timing.compute_cycles;
  statistics->result_write_cycles += timing.result_write_cycles;
  statistics->total_service_cycles += timing.total_cycles;
  ++statistics->primitive_count;
  const uint64_t chunks = config.lanes == 0 ? 0 : divide_round_up(command.n, config.lanes);
  statistics->lane_slots += chunks * config.lanes;
  statistics->active_lane_elements += command.n;
  if (timing.operand_read_cycles > timing.compute_cycles) {
    ++statistics->memory_dominant_primitives;
  } else {
    ++statistics->compute_dominant_primitives;
  }
}
