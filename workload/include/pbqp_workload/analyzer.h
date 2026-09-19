// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Declares logical PBQP reduction traces and aggregate interface analysis.

#pragma once

#include "pbqp_workload/graph_generator.h"

#include <cstddef>
#include <string>
#include <vector>

namespace pcaa::workload {

enum class ReductionKind { kR1, kR2 };

struct TraceRecord {
  ReductionKind kind;
  int x_domain;
  int y_domain;
  int z_domain;
  int operand_count;
  int contiguous_operands;
  int strided_operands;
  size_t logical_elements;
  size_t logical_read_bytes;
  size_t logical_write_bytes;
  size_t minimum_only_write_bytes;
  size_t scratch_packs;
  size_t scratch_bytes;
  bool batch_start;
  size_t batch_size;
  size_t batch_scratch_bytes;
};

struct InstanceResult {
  GeneratorConfig config;
  int initial_edges;
  int final_edges;
  int r0_count;
  int r1_count;
  int r2_count;
  int irreducible_nodes;
  std::vector<TraceRecord> trace;
};

struct Aggregate {
  int instances = 0;
  size_t nodes = 0;
  size_t r0 = 0;
  size_t r1 = 0;
  size_t r2 = 0;
  size_t baseline_commands = 0;
  size_t strided_views = 0;
  size_t operand_views = 0;
  size_t scratch_bytes = 0;
  size_t logical_read_bytes = 0;
  size_t logical_write_bytes = 0;
  size_t minimum_only_write_bytes = 0;
  size_t batched_r1_commands = 0;
  size_t batched_r2_commands = 0;
  size_t software_batch_scratch_bytes = 0;
  std::vector<int> lengths;
};

InstanceResult Analyze(const GeneratorConfig &config);
void AddToAggregate(const InstanceResult &result, Aggregate *aggregate);
std::string CsvHeader();
std::string ToCsv(const GeneratorConfig &config, const TraceRecord &record);
std::string FormatAggregate(const Aggregate &aggregate);

}  // namespace pcaa::workload
