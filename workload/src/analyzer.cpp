// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements logical PBQP reduction tracing and interface-alternative analysis.

#include "pbqp_workload/analyzer.h"

#include <algorithm>
#include <cstdint>
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
  for (const EdgeSlot &slot : graph.edge_slots) {
    if (!slot.active) {
      continue;
    }
    if (slot.first == node) {
      neighbors.push_back(slot.second);
    } else if (slot.second == node) {
      neighbors.push_back(slot.first);
    }
  }
  return neighbors;
}

int FindEdgeSlot(const Graph &graph, int first, int second) {
  for (size_t index = 0; index < graph.edge_slots.size(); ++index) {
    const EdgeSlot &slot = graph.edge_slots[index];
    if (slot.active && ((slot.first == first && slot.second == second) ||
                        (slot.first == second && slot.second == first))) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

void RemoveEdge(Graph *graph, int first, int second) {
  const int index = FindEdgeSlot(*graph, first, second);
  if (index >= 0) {
    graph->edge_slots[static_cast<size_t>(index)].active = false;
  }
  graph->edges[first][second] = false;
  graph->edges[second][first] = false;
  graph->edge_first[first][second] = -1;
  graph->edge_first[second][first] = -1;
}

void AddFillEdge(Graph *graph, int first, int second) {
  for (EdgeSlot &slot : graph->edge_slots) {
    if (!slot.active) {
      slot = {first, second, true};
      graph->edges[first][second] = true;
      graph->edges[second][first] = true;
      graph->edge_first[first][second] = first;
      graph->edge_first[second][first] = first;
      return;
    }
  }
  graph->edge_slots.push_back({first, second, true});
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

int SelectNode(const Graph &graph, const std::vector<bool> &active, const std::vector<int> &degree,
               ReductionPolicy policy) {
  int selected = -1;
  uint64_t best_work = 0;
  int best_degree = 0;
  for (int node = 0; node < static_cast<int>(graph.domains.size()); ++node) {
    if (!active[node] || degree[node] > 2) {
      continue;
    }
    if (policy == ReductionPolicy::kProductionIndex) {
      return node;
    }
    if (policy == ReductionPolicy::kDegreePriority) {
      if (selected < 0 || degree[node] < degree[selected]) {
        selected = node;
      }
      continue;
    }
    const std::vector<int> neighbors = Neighbors(graph, node);
    uint64_t work = graph.domains[node];
    if (degree[node] == 1) {
      work *= graph.domains[neighbors[0]];
    } else if (degree[node] == 2) {
      work *= static_cast<uint64_t>(graph.domains[neighbors[0]]) * graph.domains[neighbors[1]];
    }
    if (selected < 0 || work < best_work ||
        (work == best_work &&
         (degree[node] < best_degree || (degree[node] == best_degree && node < selected)))) {
      selected = node;
      best_work = work;
      best_degree = degree[node];
    }
  }
  return selected;
}

}  // namespace

InstanceResult Analyze(const GeneratorConfig &config, ReductionPolicy policy) {
  Graph graph = GenerateGraph(config);
  InstanceResult result{config, policy, CountEdges(graph), 0, 0, 0, 0, 0, {}};
  std::vector<bool> active(graph.domains.size(), true);
  std::vector<int> degree(graph.domains.size(), 0);
  for (int node = 0; node < static_cast<int>(graph.domains.size()); ++node) {
    degree[node] = static_cast<int>(Neighbors(graph, node).size());
  }

  while (true) {
    const int node = SelectNode(graph, active, degree, policy);
    if (node < 0)
      break;
    const std::vector<int> node_neighbors = Neighbors(graph, node);
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
      RemoveEdge(&graph, node, neighbor);
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
      RemoveEdge(&graph, node, first);
      RemoveEdge(&graph, node, second);
      --degree[first];
      --degree[second];
      if (!graph.edges[first][second]) {
        AddFillEdge(&graph, first, second);
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

std::string ToString(ReductionPolicy policy) {
  switch (policy) {
    case ReductionPolicy::kProductionIndex:
      return "production_index";
    case ReductionPolicy::kDegreePriority:
      return "degree_priority";
    case ReductionPolicy::kMinimumKernelWork:
      return "minimum_kernel_work";
  }
  return "unknown";
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
  return "family,profile,nodes,seed,policy,reduction,opcode,x_domain,y_domain,z_domain,operand_"
         "count,"
         "contiguous_operands,strided_operands,logical_elements,logical_read_bytes,"
         "argmin_write_bytes,minimum_only_write_bytes,scratch_packs,scratch_bytes,"
         "batch_start,batch_size,batch_scratch_bytes\n";
}

std::string ToCsv(const GeneratorConfig &config, ReductionPolicy policy,
                  const TraceRecord &record) {
  std::ostringstream output;
  output << ToString(config.family) << ',' << ToString(config.profile) << ',' << config.nodes << ','
         << config.seed << ',' << ToString(policy) << ','
         << (record.kind == ReductionKind::kR1 ? "R1" : "R2") << ','
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
