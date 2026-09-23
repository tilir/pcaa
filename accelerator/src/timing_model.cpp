// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements simple parameterized L1 service-cycle estimates for PCAA commands.

#include "timing_model.h"
#include "pcaa_codec.h"

#include <stddef.h>
#include <algorithm>

namespace {

uint64_t divide_round_up(uint64_t value, unsigned divisor) {
  return (value + divisor - 1) / divisor;
}

}  // namespace
AccelCommandTiming accel_estimate_command_cycles(const pcaa_command_t &command,
                                                 const AccelTimingConfig &config) {
  if (config.mode == AccelTimingMode::kUntimed || config.lanes == 0 ||
      config.descriptor_bytes_per_cycle == 0 || config.memory_read_bytes_per_cycle == 0 ||
      config.memory_write_bytes_per_cycle == 0)
    return {};
  const bool add3 =
      command.kind == PCAA_MAP_ADD3_REDUCE_MIN || command.kind == PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN;
  const bool vector_add = command.kind == PCAA_COST_ADD_VECTOR;
  const bool project = command.kind == PCAA_MINPLUS_PROJECT;
  const bool map3 = command.kind == PCAA_MINPLUS_MAP3_PROJECT;
  const bool argmin = command.kind == PCAA_MAP_ADD_REDUCE_MIN_ARGMIN ||
                      command.kind == PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN;
  const uint64_t n = add3         ? command.operation.reduce3.first.length
                     : vector_add ? command.operation.vector_add.first.length
                     : project    ? command.operation.project.matrix.columns
                     : map3       ? command.operation.map3_project.first.length
                                  : command.operation.reduce2.first.length;
  const uint64_t outputs = project ? command.operation.project.result.length
                           : map3  ? command.operation.map3_project.result.length
                                   : 1;
  const uint64_t operand_elements = project ? n * (outputs + 1)
                                    : map3  ? n * (outputs + 2)
                                            : n * (add3 ? 3 : 2);
  const uint64_t result_bytes = map3         ? outputs * sizeof(accel_min_argmin_result_t)
                                : vector_add ? n * sizeof(int32_t)
                                : project    ? outputs * sizeof(int32_t)
                                : argmin     ? sizeof(accel_min_argmin_result_t)
                                             : sizeof(int32_t);
  const uint64_t chunks = divide_round_up(n, config.lanes) * outputs;
  AccelCommandTiming timing;
  size_t descriptor_bytes = 0;
  if (pcaa_encoded_size(&command, &descriptor_bytes) != PCAA_STATUS_OK)
    return {};
  timing.descriptor_cycles = divide_round_up(descriptor_bytes, config.descriptor_bytes_per_cycle);
  timing.operand_read_cycles =
      divide_round_up(operand_elements * sizeof(int32_t), config.memory_read_bytes_per_cycle);
  timing.compute_cycles = config.primitive_start_cycles + config.map_pipeline_latency + chunks +
                          config.reduction_tree_latency + config.result_latency;
  if (add3 || map3)
    timing.compute_cycles += config.add3_map_extra_latency;
  timing.result_write_cycles = divide_round_up(result_bytes, config.memory_write_bytes_per_cycle);
  timing.total_cycles = config.mode == AccelTimingMode::kL1Sequential
                            ? timing.descriptor_cycles + timing.operand_read_cycles +
                                  timing.compute_cycles + timing.result_write_cycles
                            : timing.descriptor_cycles +
                                  std::max(timing.operand_read_cycles, timing.compute_cycles) +
                                  timing.result_write_cycles;
  return timing;
}

void accel_accumulate_semantic_command_timing(const pcaa_command_t &command,
                                              const AccelTimingConfig &config,
                                              AccelTimingStatistics *statistics) {
  const AccelCommandTiming timing = accel_estimate_command_cycles(command, config);
  if (timing.total_cycles == 0)
    return;
  const bool add3 =
      command.kind == PCAA_MAP_ADD3_REDUCE_MIN || command.kind == PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN;
  const bool vector_add = command.kind == PCAA_COST_ADD_VECTOR;
  const bool project = command.kind == PCAA_MINPLUS_PROJECT;
  const bool map3 = command.kind == PCAA_MINPLUS_MAP3_PROJECT;
  const uint64_t n = add3         ? command.operation.reduce3.first.length
                     : vector_add ? command.operation.vector_add.first.length
                     : project    ? command.operation.project.matrix.columns
                     : map3       ? command.operation.map3_project.first.length
                                  : command.operation.reduce2.first.length;
  const uint64_t outputs = project ? command.operation.project.result.length
                           : map3  ? command.operation.map3_project.result.length
                                   : 1;
  const uint64_t chunks = divide_round_up(n, config.lanes) * outputs;
  statistics->descriptor_cycles += timing.descriptor_cycles;
  statistics->operand_read_cycles += timing.operand_read_cycles;
  statistics->compute_cycles += timing.compute_cycles;
  statistics->result_write_cycles += timing.result_write_cycles;
  statistics->total_service_cycles += timing.total_cycles;
  ++statistics->primitive_count;
  statistics->lane_slots += chunks * config.lanes;
  statistics->active_lane_elements += n * outputs;
  if (timing.operand_read_cycles > timing.compute_cycles)
    ++statistics->memory_dominant_primitives;
  else
    ++statistics->compute_dominant_primitives;
}
