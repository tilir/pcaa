// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Emits deterministic PBQP workload traces and aggregate interface statistics.

#include "accel_protocol.h"
#include "pbqp_workload/analyzer.h"
#include "pbqp_workload/graph_generator.h"
#include "timing_model.h"

#include <cstdint>
#include <algorithm>
#include <cassert>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

#include <sysc/kernel/sc_time.h>

namespace {

using pcaa::workload::DomainProfile;
using pcaa::workload::GeneratorConfig;
using pcaa::workload::GraphFamily;
using pcaa::workload::ReductionPolicy;

struct CorpusEntry {
  GraphFamily family;
  DomainProfile profile;
  int nodes;
  unsigned seed;
};

struct TimingBreakdown {
  uint64_t descriptor_control_cycles = 0;
  uint64_t operand_read_cycles = 0;
  uint64_t compute_cycles = 0;
  uint64_t result_write_cycles = 0;
  uint64_t exposed_service_cycles = 0;
  uint64_t active_lane_elements = 0;
  uint64_t lane_slots = 0;
  uint64_t primitive_count = 0;
  uint64_t compute_dominated_primitives = 0;
  uint64_t memory_dominated_primitives = 0;
  uint64_t balanced_primitives = 0;
  uint64_t compute_dominated_max_cycles = 0;
  uint64_t memory_dominated_max_cycles = 0;
  uint64_t balanced_max_cycles = 0;
};

void AddTiming(TimingBreakdown *total, const TimingBreakdown &part) {
  total->descriptor_control_cycles += part.descriptor_control_cycles;
  total->operand_read_cycles += part.operand_read_cycles;
  total->compute_cycles += part.compute_cycles;
  total->result_write_cycles += part.result_write_cycles;
  total->exposed_service_cycles += part.exposed_service_cycles;
  total->active_lane_elements += part.active_lane_elements;
  total->lane_slots += part.lane_slots;
  total->primitive_count += part.primitive_count;
  total->compute_dominated_primitives += part.compute_dominated_primitives;
  total->memory_dominated_primitives += part.memory_dominated_primitives;
  total->balanced_primitives += part.balanced_primitives;
  total->compute_dominated_max_cycles += part.compute_dominated_max_cycles;
  total->memory_dominated_max_cycles += part.memory_dominated_max_cycles;
  total->balanced_max_cycles += part.balanced_max_cycles;
}

TimingBreakdown EstimateTiming(const pcaa::workload::InstanceResult &result,
                               const AccelTimingConfig &config, bool structural) {
  TimingBreakdown breakdown;
  for (const auto &record : result.trace) {
    accel_command_t command{};
    command.opcode = record.kind == pcaa::workload::ReductionKind::kR1
                         ? ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN
                         : ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN;
    command.n = static_cast<uint32_t>(record.x_domain);
    const AccelCommandTiming timing = accel_estimate_command_cycles(command, config);
    const uint64_t compute_cycles =
        structural ? timing.compute_cycles - config.primitive_start_cycles : timing.compute_cycles;
    const uint64_t max_demand_cycles = std::max(timing.operand_read_cycles, compute_cycles);
    breakdown.operand_read_cycles += timing.operand_read_cycles;
    breakdown.compute_cycles += compute_cycles;
    breakdown.result_write_cycles += timing.result_write_cycles;
    const uint64_t chunks = (command.n + config.lanes - 1) / config.lanes;
    breakdown.active_lane_elements += command.n;
    breakdown.lane_slots += chunks * config.lanes;
    ++breakdown.primitive_count;
    if (compute_cycles > timing.operand_read_cycles) {
      ++breakdown.compute_dominated_primitives;
      breakdown.compute_dominated_max_cycles += max_demand_cycles;
    } else if (timing.operand_read_cycles > compute_cycles) {
      ++breakdown.memory_dominated_primitives;
      breakdown.memory_dominated_max_cycles += max_demand_cycles;
    } else {
      ++breakdown.balanced_primitives;
      breakdown.balanced_max_cycles += max_demand_cycles;
    }
    if (structural) {
      const uint64_t primitive_exposed =
          config.mode == AccelTimingMode::kL1Sequential
              ? timing.operand_read_cycles + compute_cycles + timing.result_write_cycles
              : max_demand_cycles + timing.result_write_cycles;
      breakdown.exposed_service_cycles += primitive_exposed;
    } else {
      breakdown.descriptor_control_cycles +=
          timing.descriptor_cycles + config.primitive_start_cycles;
      const uint64_t expected_cycles =
          config.mode == AccelTimingMode::kL1Sequential
              ? timing.descriptor_cycles + timing.operand_read_cycles + compute_cycles +
                    timing.result_write_cycles
              : timing.descriptor_cycles + max_demand_cycles + timing.result_write_cycles;
      assert(timing.total_cycles == expected_cycles);
      breakdown.exposed_service_cycles += timing.total_cycles;
    }
    if (record.batch_start) {
      const uint64_t outer_descriptor_cycles =
          (sizeof(accel_command_t) + config.descriptor_bytes_per_cycle - 1) /
          config.descriptor_bytes_per_cycle;
      const uint64_t batch_result_cycles =
          (sizeof(accel_batch_result_t) + config.memory_write_bytes_per_cycle - 1) /
          config.memory_write_bytes_per_cycle;
      breakdown.descriptor_control_cycles += outer_descriptor_cycles + config.batch_start_cycles;
      breakdown.result_write_cycles += batch_result_cycles;
      breakdown.exposed_service_cycles +=
          outer_descriptor_cycles + config.batch_start_cycles + batch_result_cycles;
    }
  }
  return breakdown;
}

std::vector<CorpusEntry> RepresentativeCorpus() {
  return {
      {GraphFamily::kTree, DomainProfile::kUniformSmall, 10, 101},
      {GraphFamily::kTree, DomainProfile::kRegisterLike, 100, 102},
      {GraphFamily::kTree, DomainProfile::kLargeStress, 1000, 103},
      {GraphFamily::kPath, DomainProfile::kRegisterLike, 100, 201},
      {GraphFamily::kStar, DomainProfile::kRegisterLike, 100, 202},
      {GraphFamily::kCycle, DomainProfile::kUniformSmall, 10, 301},
      {GraphFamily::kCycle, DomainProfile::kRegisterLike, 100, 302},
      {GraphFamily::kTwoTree, DomainProfile::kUniformSmall, 100, 401},
      {GraphFamily::kTwoTree, DomainProfile::kRegisterLike, 250, 402},
      {GraphFamily::kTwoTree, DomainProfile::kLargeStress, 100, 403},
      {GraphFamily::kIrreducibleCore, DomainProfile::kRegisterLike, 8, 501},
  };
}

TimingBreakdown EstimateCorpus(const std::vector<CorpusEntry> &corpus, ReductionPolicy policy,
                               const AccelTimingConfig &timing, bool structural) {
  TimingBreakdown total;
  for (const CorpusEntry &entry : corpus) {
    const GeneratorConfig config = {entry.family, entry.profile, entry.nodes, entry.seed};
    AddTiming(&total, EstimateTiming(pcaa::workload::Analyze(config, policy), timing, structural));
  }
  return total;
}

}  // namespace

int main(int argc, char **argv) {
  std::string trace_path = "pbqp-workload.csv";
  if (argc == 3 && std::string(argv[1]) == "--trace") {
    trace_path = argv[2];
  } else if (argc != 1) {
    std::cerr << "usage: pbqp_workload [--trace PATH]\n";
    return 2;
  }

  std::ofstream trace(trace_path);
  if (!trace) {
    std::cerr << "cannot write " << trace_path << '\n';
    return 1;
  }
  trace << pcaa::workload::CsvHeader();

  const std::vector<ReductionPolicy> policies = {
      ReductionPolicy::kProductionIndex,
      ReductionPolicy::kDegreePriority,
      ReductionPolicy::kMinimumKernelWork,
  };
  const std::vector<AccelTimingConfig> timing_configs = {
      {AccelTimingMode::kL1Sequential, 4, 16, 16, 16, 4, 2, 2, 1, 3, 1, sc_core::SC_ZERO_TIME},
      {AccelTimingMode::kL1Streaming, 8, 32, 32, 16, 4, 2, 2, 1, 3, 1, sc_core::SC_ZERO_TIME},
      {AccelTimingMode::kL1Streaming, 16, 32, 64, 16, 4, 2, 2, 1, 3, 1, sc_core::SC_ZERO_TIME},
  };
  for (ReductionPolicy policy : policies) {
    pcaa::workload::Aggregate aggregate;
    for (const CorpusEntry &entry : RepresentativeCorpus()) {
      const GeneratorConfig config = {entry.family, entry.profile, entry.nodes, entry.seed};
      const auto result = pcaa::workload::Analyze(config, policy);
      for (const auto &record : result.trace) {
        trace << pcaa::workload::ToCsv(config, policy, record);
      }
      pcaa::workload::AddToAggregate(result, &aggregate);
      pcaa::workload::Aggregate instance_aggregate;
      pcaa::workload::AddToAggregate(result, &instance_aggregate);
      std::cout << "policy=" << pcaa::workload::ToString(policy)
                << " family=" << pcaa::workload::ToString(entry.family)
                << " profile=" << pcaa::workload::ToString(entry.profile) << " seed=" << entry.seed
                << '\n'
                << pcaa::workload::FormatAggregate(instance_aggregate);
    }
    std::cout << "policy=" << pcaa::workload::ToString(policy) << '\n'
              << pcaa::workload::FormatAggregate(aggregate);
    for (const AccelTimingConfig &timing : timing_configs) {
      uint64_t software_batch_cycles = 0;
      uint64_t structural_cycles = 0;
      for (const CorpusEntry &entry : RepresentativeCorpus()) {
        const GeneratorConfig config = {entry.family, entry.profile, entry.nodes, entry.seed};
        const auto result = pcaa::workload::Analyze(config, policy);
        software_batch_cycles += EstimateTiming(result, timing, false).exposed_service_cycles;
        structural_cycles += EstimateTiming(result, timing, true).exposed_service_cycles;
      }
      std::cout << "timing mode="
                << (timing.mode == AccelTimingMode::kL1Sequential ? "sequential" : "streaming")
                << " lanes=" << timing.lanes << " software_batch_cycles=" << software_batch_cycles
                << " structural_cycles=" << structural_cycles << '\n';
    }
  }
  const std::vector<CorpusEntry> corpus = RepresentativeCorpus();
  const AccelTimingConfig t8 = timing_configs[1];
  const AccelTimingConfig t16 = timing_configs[2];
  for (const AccelTimingConfig *timing : {&t8, &t16}) {
    for (bool structural : {false, true}) {
      const TimingBreakdown breakdown =
          EstimateCorpus(corpus, ReductionPolicy::kDegreePriority, *timing, structural);
      std::cout << "breakdown policy=degree_priority lanes=" << timing->lanes
                << " interface=" << (structural ? "structural" : "software_batch")
                << " control=" << breakdown.descriptor_control_cycles
                << " read=" << breakdown.operand_read_cycles
                << " compute=" << breakdown.compute_cycles
                << " write=" << breakdown.result_write_cycles
                << " exposed=" << breakdown.exposed_service_cycles << '\n';
      const uint64_t max_demand_total = breakdown.compute_dominated_max_cycles +
                                        breakdown.memory_dominated_max_cycles +
                                        breakdown.balanced_max_cycles;
      std::cout << "dominance policy=degree_priority lanes=" << timing->lanes
                << " interface=" << (structural ? "structural" : "software_batch")
                << " primitives=" << breakdown.primitive_count
                << " compute_count=" << breakdown.compute_dominated_primitives
                << " memory_count=" << breakdown.memory_dominated_primitives
                << " balanced_count=" << breakdown.balanced_primitives
                << " compute_max=" << breakdown.compute_dominated_max_cycles
                << " memory_max=" << breakdown.memory_dominated_max_cycles
                << " balanced_max=" << breakdown.balanced_max_cycles
                << " max_total=" << max_demand_total << '\n';
    }
  }
  for (unsigned lanes : {1U, 2U, 4U, 8U, 16U, 32U}) {
    AccelTimingConfig sweep = t8;
    sweep.lanes = lanes;
    for (bool structural : {false, true}) {
      const TimingBreakdown breakdown =
          EstimateCorpus(corpus, ReductionPolicy::kDegreePriority, sweep, structural);
      const double utilization =
          breakdown.lane_slots == 0
              ? 0.0
              : static_cast<double>(breakdown.active_lane_elements) / breakdown.lane_slots;
      std::cout << "lane_sweep policy=degree_priority lanes=" << lanes
                << " interface=" << (structural ? "structural" : "software_batch")
                << " exposed=" << breakdown.exposed_service_cycles
                << " control=" << breakdown.descriptor_control_cycles
                << " read=" << breakdown.operand_read_cycles
                << " compute=" << breakdown.compute_cycles << " utilization=" << utilization
                << '\n';
    }
  }
  std::cout << "trace=" << trace_path << '\n';
  return 0;
}
