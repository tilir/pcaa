// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements logical PBQP reduction tracing and interface-alternative analysis.

#include "pbqp_workload/analyzer.h"

#include <algorithm>
#include <numeric>
#include <sstream>
#include <utility>

namespace pcaa::workload {
namespace {

constexpr int kCostBytes = 4;
constexpr int kArgminResultBytes = 8;
constexpr size_t kPrimitiveDescriptorBytes = 56;

int CountEdges(const Graph &graph) {
  int count = 0;
  for (int first = 0; first < static_cast<int>(graph.domains.size()); ++first) {
    for (int second = first + 1; second < static_cast<int>(graph.domains.size()); ++second) {
      count += graph.edges[first][second];
    }
  }
  return count;
}

std::vector<int> Neighbors(const Graph &graph, int node) {
  std::vector<int> neighbors;
  for (int other = 0; other < static_cast<int>(graph.domains.size()); ++other) {
    if (graph.edges[node][other]) {
      neighbors.push_back(other);
    }
  }
  return neighbors;
}

TraceRecord MakeR1(int x, int y, bool edge_is_strided) {
  const size_t elements = x;
  return {ReductionKind::kR1,
          x,
          y,
          0,
          2,
          edge_is_strided ? 1 : 2,
          edge_is_strided ? 1 : 0,
          elements,
          elements * 2 * kCostBytes,
          kArgminResultBytes,
          kCostBytes,
          edge_is_strided ? 1U : 0U,
          edge_is_strided ? static_cast<size_t>(x) * kCostBytes : 0,
          false,
          0,
          0};
}

TraceRecord MakeR2(int x, int y, int z, bool first_edge_is_strided, bool second_edge_is_strided) {
  const size_t elements = x;
  const int strided_operands =
      static_cast<int>(first_edge_is_strided) + static_cast<int>(second_edge_is_strided);
  return {ReductionKind::kR2,
          x,
          y,
          z,
          3,
          3 - strided_operands,
          strided_operands,
          elements,
          elements * 3 * kCostBytes,
          kArgminResultBytes,
          kCostBytes,
          static_cast<size_t>(strided_operands),
          static_cast<size_t>(x) * strided_operands * kCostBytes,
          false,
          0,
          0};
}

double Median(std::vector<int> values) {
  if (values.empty())
    return 0.0;
  std::sort(values.begin(), values.end());
  const int middle = static_cast<int>(values.size() / 2);
  return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
}

}  // namespace

InstanceResult Analyze(const GeneratorConfig &config) {
  Graph graph = GenerateGraph(config);
  InstanceResult result{config, CountEdges(graph), 0, 0, 0, 0, 0, {}};
  std::vector<bool> active(graph.domains.size(), true);
  std::vector<int> degree(graph.domains.size(), 0);
  for (int node = 0; node < static_cast<int>(graph.domains.size()); ++node) {
    degree[node] = static_cast<int>(Neighbors(graph, node).size());
  }

  while (true) {
    int node = -1;
    std::vector<int> node_neighbors;
    for (int candidate = 0; candidate < static_cast<int>(graph.domains.size()); ++candidate) {
      if (!active[candidate] || degree[candidate] > 2)
        continue;
      node = candidate;
      node_neighbors = Neighbors(graph, candidate);
      break;
    }
    if (node < 0)
      break;
    if (node_neighbors.empty()) {
      ++result.r0_count;
    } else if (node_neighbors.size() == 1) {
      ++result.r1_count;
      const int neighbor = node_neighbors[0];
      const bool edge_is_strided = graph.edge_first[node][neighbor] == node;
      for (int value = 0; value < graph.domains[node_neighbors[0]]; ++value) {
        result.trace.push_back(
            MakeR1(graph.domains[node], graph.domains[node_neighbors[0]], edge_is_strided));
        if (value == 0) {
          TraceRecord &first_record = result.trace.back();
          first_record.batch_start = true;
          first_record.batch_size = graph.domains[neighbor];
          first_record.batch_scratch_bytes =
              edge_is_strided
                  ? static_cast<size_t>(graph.domains[node]) * graph.domains[neighbor] * kCostBytes
                  : 0;
        }
      }
      graph.edges[node][neighbor] = false;
      graph.edges[neighbor][node] = false;
      graph.edge_first[node][neighbor] = -1;
      graph.edge_first[neighbor][node] = -1;
      --degree[neighbor];
    } else {
      ++result.r2_count;
      const int first = node_neighbors[0];
      const int second = node_neighbors[1];
      const bool first_edge_is_strided = graph.edge_first[node][first] == node;
      const bool second_edge_is_strided = graph.edge_first[node][second] == node;
      for (int first_value = 0; first_value < graph.domains[first]; ++first_value) {
        for (int second_value = 0; second_value < graph.domains[second]; ++second_value) {
          result.trace.push_back(MakeR2(graph.domains[node], graph.domains[first],
                                        graph.domains[second], first_edge_is_strided,
                                        second_edge_is_strided));
          if (first_value == 0 && second_value == 0) {
            TraceRecord &first_record = result.trace.back();
            first_record.batch_start = true;
            first_record.batch_size =
                static_cast<size_t>(graph.domains[first]) * graph.domains[second];
            first_record.batch_scratch_bytes =
                static_cast<size_t>(graph.domains[node]) *
                ((first_edge_is_strided ? graph.domains[first] : 0) +
                 (second_edge_is_strided ? graph.domains[second] : 0)) *
                kCostBytes;
          }
        }
      }
      graph.edges[node][first] = false;
      graph.edges[first][node] = false;
      graph.edges[node][second] = false;
      graph.edges[second][node] = false;
      graph.edge_first[node][first] = -1;
      graph.edge_first[first][node] = -1;
      graph.edge_first[node][second] = -1;
      graph.edge_first[second][node] = -1;
      --degree[first];
      --degree[second];
      if (!graph.edges[first][second]) {
        graph.edges[first][second] = true;
        graph.edges[second][first] = true;
        graph.edge_first[first][second] = first;
        graph.edge_first[second][first] = first;
        ++degree[first];
        ++degree[second];
      }
    }
    active[node] = false;
  }

  result.final_edges = CountEdges(graph);
  for (bool is_active : active) result.irreducible_nodes += is_active;
  return result;
}

void AddToAggregate(const InstanceResult &result, Aggregate *aggregate) {
  ++aggregate->instances;
  aggregate->nodes += result.config.nodes;
  aggregate->r0 += result.r0_count;
  aggregate->r1 += result.r1_count;
  aggregate->r2 += result.r2_count;
  for (const TraceRecord &record : result.trace) {
    ++aggregate->baseline_commands;
    aggregate->operand_views += record.operand_count;
    aggregate->strided_views += record.strided_operands;
    aggregate->scratch_bytes += record.scratch_bytes;
    aggregate->logical_read_bytes += record.logical_read_bytes;
    aggregate->logical_write_bytes += record.logical_write_bytes;
    aggregate->minimum_only_write_bytes += record.minimum_only_write_bytes;
    if (record.batch_start)
      aggregate->software_batch_scratch_bytes += record.batch_scratch_bytes;
    aggregate->lengths.push_back(record.x_domain);
  }
  aggregate->batched_r1_commands += result.r1_count;
  aggregate->batched_r2_commands += result.r2_count;
}

std::string CsvHeader() {
  return "family,profile,nodes,seed,reduction,opcode,x_domain,y_domain,z_domain,operand_count,"
         "contiguous_operands,strided_operands,logical_elements,logical_read_bytes,"
         "argmin_write_bytes,minimum_only_write_bytes,scratch_packs,scratch_bytes,"
         "batch_start,batch_size,batch_scratch_bytes\n";
}

std::string ToCsv(const GeneratorConfig &config, const TraceRecord &record) {
  std::ostringstream output;
  output << ToString(config.family) << ',' << ToString(config.profile) << ',' << config.nodes << ','
         << config.seed << ',' << (record.kind == ReductionKind::kR1 ? "R1" : "R2") << ','
         << (record.kind == ReductionKind::kR1 ? "MAP_ADD_REDUCE_MIN_ARGMIN"
                                               : "MAP_ADD3_REDUCE_MIN_ARGMIN")
         << ',' << record.x_domain << ',' << record.y_domain << ',' << record.z_domain << ','
         << record.operand_count << ',' << record.contiguous_operands << ','
         << record.strided_operands << ',' << record.logical_elements << ','
         << record.logical_read_bytes << ',' << record.logical_write_bytes << ','
         << record.minimum_only_write_bytes << ',' << record.scratch_packs << ','
         << record.scratch_bytes << ',' << record.batch_start << ',' << record.batch_size << ','
         << record.batch_scratch_bytes << '\n';
  return output.str();
}

std::string FormatAggregate(const Aggregate &aggregate) {
  const size_t reductions = aggregate.r0 + aggregate.r1 + aggregate.r2;
  const double mean = aggregate.lengths.empty()
                          ? 0.0
                          : static_cast<double>(std::accumulate(
                                aggregate.lengths.begin(), aggregate.lengths.end(), size_t{0})) /
                                aggregate.lengths.size();
  int minimum = 0;
  int maximum = 0;
  if (!aggregate.lengths.empty()) {
    const auto bounds = std::minmax_element(aggregate.lengths.begin(), aggregate.lengths.end());
    minimum = *bounds.first;
    maximum = *bounds.second;
  }
  std::ostringstream output;
  output << "instances=" << aggregate.instances << " nodes=" << aggregate.nodes << "\n"
         << "reductions R0=" << aggregate.r0 << " R1=" << aggregate.r1 << " R2=" << aggregate.r2
         << "\nsubmissions original=" << aggregate.baseline_commands
         << " software_batch=" << aggregate.batched_r1_commands + aggregate.batched_r2_commands
         << " structural_batch=" << aggregate.batched_r1_commands + aggregate.batched_r2_commands
         << " primitive_descriptors=" << aggregate.baseline_commands << "\n"
         << "descriptor_bytes outer="
         << (aggregate.batched_r1_commands + aggregate.batched_r2_commands) *
                kPrimitiveDescriptorBytes
         << " child=" << aggregate.baseline_commands * kPrimitiveDescriptorBytes << "\n"
         << "length min=" << minimum << " max=" << maximum << " mean=" << mean
         << " median=" << Median(aggregate.lengths) << "\n"
         << "views strided=" << aggregate.strided_views << '/' << aggregate.operand_views
         << " packing_bytes original=" << aggregate.scratch_bytes
         << " software_batch=" << aggregate.software_batch_scratch_bytes
         << " logical_read_bytes=" << aggregate.logical_read_bytes
         << " argmin_write_bytes=" << aggregate.logical_write_bytes
         << " minimum_only_write_bytes=" << aggregate.minimum_only_write_bytes << "\n"
         << "ratios packing/read="
         << (aggregate.logical_read_bytes == 0
                 ? 0.0
                 : static_cast<double>(aggregate.scratch_bytes) / aggregate.logical_read_bytes)
         << " commands/reduction="
         << (reductions == 0 ? 0.0 : static_cast<double>(aggregate.baseline_commands) / reductions)
         << '\n';
  return output.str();
}

}  // namespace pcaa::workload
