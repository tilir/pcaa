// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Characterizes direct semantic commands with an independent mathematical oracle.
#include "l2_support.h"
#include "accel_protocol.h"
#include "l2_accelerator.h"
#include "pcaa.h"
#include "pcaa_codec.h"
#include "timing_model.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <sysc/kernel/sc_externs.h>
#include <sysc/kernel/sc_simcontext.h>
#include <sysc/kernel/sc_time.h>

namespace {
using namespace l2_test;
constexpr std::array<size_t, 16> kLengths = {1,  2,  3,  4,  7,  8,   15,  16,
                                             17, 31, 32, 63, 64, 129, 256, 1024};
constexpr std::array<size_t, 11> kRows = {1, 2, 4, 7, 8, 9, 15, 16, 17, 31, 32};
int32_t oracle_add(int32_t a, int32_t b) {
  if (a > ACCEL_INF || b > ACCEL_INF)
    throw std::runtime_error("oracle invalid input");
  if (a == ACCEL_INF || b == ACCEL_INF)
    return ACCEL_INF;
  const long long sum = static_cast<long long>(a) + b;
  if (sum < INT32_MIN)
    throw std::runtime_error("oracle underflow");
  return sum >= ACCEL_INF ? ACCEL_INF : static_cast<int32_t>(sum);
}
std::vector<std::pair<uint64_t, int32_t>> expected(const Ram &ram, const pcaa_command_t &c) {
  pcaa_cost_vector_view_t a{}, b{}, third{};
  pcaa_cost_matrix_view_t matrix{};
  pcaa_output_view_t out{};
  bool project = false, add3 = false, vector = false;
  switch (c.kind) {
    case PCAA_MINPLUS_PROJECT:
      project = true;
      a = c.operation.project.vector;
      matrix = c.operation.project.matrix;
      out = c.operation.project.result;
      break;
    case PCAA_MINPLUS_MAP3_PROJECT:
      project = add3 = true;
      a = c.operation.map3_project.first;
      b = c.operation.map3_project.second;
      matrix = c.operation.map3_project.third;
      out = c.operation.map3_project.result;
      break;
    case PCAA_COST_ADD_VECTOR:
      vector = true;
      a = c.operation.vector_add.first;
      b = c.operation.vector_add.second;
      out = c.operation.vector_add.result;
      break;
    case PCAA_MAP_ADD3_REDUCE_MIN:
    case PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN:
      add3 = true;
      a = c.operation.reduce3.first;
      b = c.operation.reduce3.second;
      third = c.operation.reduce3.third;
      out = {c.operation.reduce3.result, 1, 1,
             c.kind == PCAA_MAP_ADD3_REDUCE_MIN ? PCAA_OUTPUT_COST : PCAA_OUTPUT_MIN_ARGMIN};
      break;
    default:
      a = c.operation.reduce2.first;
      b = c.operation.reduce2.second;
      out = {c.operation.reduce2.result, 1, 1,
             c.kind == PCAA_MAP_ADD_REDUCE_MIN ? PCAA_OUTPUT_COST : PCAA_OUTPUT_MIN_ARGMIN};
  }
  std::vector<std::pair<uint64_t, int32_t>> result;
  const auto get = [&](pcaa_cost_vector_view_t view, size_t j) {
    return ram.cost(view.base + j * view.stride * 4);
  };
  for (size_t i = 0; i < (project ? matrix.rows : 1); ++i) {
    int32_t minimum = ACCEL_INF;
    size_t winner = 0;
    for (size_t j = 0; j < a.length; ++j) {
      const int32_t value =
          project
              ? oracle_add(
                    add3 ? oracle_add(get(a, j), get(b, j)) : get(a, j),
                    ram.cost(matrix.base + (i * matrix.row_stride + j * matrix.column_stride) * 4))
          : add3 ? oracle_add(oracle_add(get(a, j), get(b, j)), get(third, j))
                 : oracle_add(get(a, j), get(b, j));
      if (vector)
        result.emplace_back(out.base + j * out.stride * 4, value);
      if (value < minimum) {
        minimum = value;
        winner = j;
      }
    }
    if (!vector) {
      const size_t width = out.kind == PCAA_OUTPUT_COST ? 4 : 8;
      result.emplace_back(out.base + i * out.stride * width, minimum);
      if (width == 8)
        result.emplace_back(out.base + i * out.stride * width + 4, static_cast<int>(winner));
    }
  }
  return result;
}
void measure(Rig &r, int op, size_t n, size_t m, int layout, size_t batch_count) {
  const size_t inner = layout == 1 ? 3 : layout == 3 ? m : 1;
  const size_t outer = layout == 2 ? n + 5 : layout == 3 ? 1 : n * inner;
  const size_t stride = layout == 1 ? 2 : 1;
  const bool inplace = layout == 4;
  fill(r.memory, n, m, inner, outer, stride);
  const auto c = command(op, n, m, inner, outer, stride, 1, 0, inplace);
  const auto want = expected(r.memory, c);
  AccelTimingConfig l1;
  l1.mode = AccelTimingMode::kL1Streaming;
  l1.lanes = 4;
  l1.descriptor_bytes_per_cycle = l1.memory_read_bytes_per_cycle = l1.memory_write_bytes_per_cycle =
      16;
  uint64_t l1_cycles = accel_estimate_command_cycles(c, l1).total_cycles;
  if (batch_count) {
    std::vector<pcaa_command_t> children(batch_count, c);
    size_t bytes = 0;
    if (pcaa_encode_stream(children.data(), children.size(), r.memory.data.data() + kChildren,
                           kBatchResult - kChildren, &bytes) != PCAA_STATUS_OK)
      throw std::runtime_error("encode batch");
    pcaa_command_t parent{};
    if (pcaa_make_ordered_batch(kChildren, batch_count, bytes, kBatchResult, &parent) !=
        PCAA_STATUS_OK)
      throw std::runtime_error("build batch");
    r.memory.encode(parent);
    l1_cycles = l1_cycles * batch_count + 3;  // Current L1 parent fetch (2) and record (1).
  } else
    r.memory.encode(c);
  r.l2.reset_statistics();
  if (r.run() != ACCEL_STATUS_DONE)
    throw std::runtime_error("L2 command error");
  for (const auto &[address, value] : want)
    if (r.memory.cost(address) != value)
      throw std::runtime_error("independent oracle mismatch");
  if (batch_count && (r.memory.cost(kBatchResult) != static_cast<int>(batch_count) ||
                      r.memory.cost(kBatchResult + 4) != -1))
    throw std::runtime_error("batch record mismatch");
  std::cout << "{\"opcode\":" << op << ",\"n\":" << n << ",\"m\":" << m << ",\"layout\":" << layout
            << ",\"batch_count\":" << batch_count
            << ",\"verified\":true,\"l1_cycles\":" << l1_cycles << ",\"l2\":";
  l2_write_json(std::cout, r.l2.config(), r.l2.statistics());
  std::cout << "}\n";
}
}  // namespace
int sc_main(int argc, char **argv) {
  L2Config config;
  bool smoke = false;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--smoke") {
        smoke = true;
        continue;
      }
      if (i + 1 >= argc)
        throw std::invalid_argument("missing option value");
      const std::string value = argv[++i];
      size_t used = 0;
      const auto number = std::stoul(value, &used);
      if (used != value.size() || number == 0 || number > UINT16_MAX)
        throw std::invalid_argument("parameter");
      if (option == "--l2-lanes")
        config.lanes = number;
      else if (option == "--l2-mem-bytes")
        config.mem_bytes = number;
      else if (option == "--l2-tm")
        config.tm = number;
      else if (option == "--l2-tn")
        config.tn = number;
      else if (option == "--l2-memory-latency")
        config.memory_latency = static_cast<int>(number);
      else
        throw std::invalid_argument("unknown option");
    }
    Rig rig(config);
    sc_core::sc_start(sc_core::SC_ZERO_TIME);
    for (int op : {1, 2, 3, 4, 6, 7, 8})
      for (size_t n : kLengths) {
        if (smoke && n != 3 && n != 17 && n != 64)
          continue;
        if (op >= 7) {
          for (size_t m : kRows) {
            if (smoke && m != 1 && m != 9 && m != 17)
              continue;
            for (int layout = 0; layout < 4; ++layout) measure(rig, op, n, m, layout, 0);
          }
        } else {
          measure(rig, op, n, 1, 0, 0);
          if (op == 6)
            for (int layout : {1, 4}) measure(rig, op, n, 1, layout, 0);
        }
      }
    for (size_t count : {size_t{1}, size_t{4}, size_t{16}, size_t{128}})
      for (int op : {3, 6, 7, 8}) measure(rig, op, 17, op >= 7 ? 9 : 1, 0, count);
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
  return 0;
}
