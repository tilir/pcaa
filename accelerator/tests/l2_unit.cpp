// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Verifies MAS schedules, errors, ordered streams and asynchronous MMIO against L0.
#include "l2_support.h"
#include "l2_accelerator.h"
#include "accel_protocol.h"
#include "pcaa.h"
#include "pcaa_codec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <memory>
#include <numeric>
#include <vector>

#include <sysc/kernel/sc_externs.h>
#include <sysc/kernel/sc_simcontext.h>
#include <sysc/kernel/sc_time.h>
#include <gtest/gtest.h>

namespace {
using namespace l2_test;
std::vector<std::unique_ptr<Rig>> rigs;
constexpr std::array<size_t, 15> kLengths = {1, 2, 3, 4, 7, 8, 15, 16, 17, 31, 32, 63, 64, 65, 129};
constexpr std::array<size_t, 11> kRows = {1, 2, 4, 7, 8, 9, 15, 16, 17, 31, 32};
void batch(Ram &ram, const std::vector<pcaa_command_t> &children, size_t extra = 0,
           size_t count = 0) {
  size_t bytes = 0;
  ASSERT_EQ(pcaa_encode_stream(children.data(), children.size(), ram.data.data() + kChildren,
                               kBatchResult - kChildren, &bytes),
            PCAA_STATUS_OK);
  pcaa_command_t parent{};
  ASSERT_EQ(pcaa_make_ordered_batch(kChildren, count ? count : children.size(), bytes + extra,
                                    kBatchResult, &parent),
            PCAA_STATUS_OK);
  ram.encode(parent);
}
TEST(L2, ShapesFormatsAndAffineSchedules) {
  for (auto &rig : rigs) {
    for (int op : {1, 2, 3, 4, 6, 7, 8}) {
      for (size_t n : kLengths) {
        for (size_t m :
             (op >= 7 ? std::vector<size_t>(kRows.begin(), kRows.end()) : std::vector<size_t>{1})) {
          for (int layout = 0; layout < (op >= 6 ? 3 : 1); ++layout) {
            const size_t inner = layout == 1 ? 3 : 1;
            const size_t outer = layout == 2 ? n + 5 : n * inner;
            const size_t stride = layout == 1 ? 2 : 1;
            const uint64_t skew = layout == 2 ? 3 : 0;
            fill(rig->memory, n, m, inner, outer, stride, skew);
            rig->memory.encode(command(op, n, m, inner, outer, stride, 2, skew));
            ASSERT_TRUE(rig->differential()) << op << ' ' << n << ' ' << m << ' ' << layout;
          }
        }
      }
    }
    const auto &s = rig->l2.statistics();
    EXPECT_EQ(s.cycles, std::accumulate(s.phase_cycles.begin(), s.phase_cycles.end(), uint64_t{0}));
    EXPECT_EQ(s.max_outstanding, 1u);
    EXPECT_EQ(s.max_writebacks, 1u);
    EXPECT_EQ(s.active_elements + s.tail_slots, s.lane_slots);
  }
}
TEST(L2, AliasesAndCrossBoundaryTies) {
  auto &r = *rigs[0];
  for (size_t n : kLengths) {
    fill(r.memory, n, 1);
    r.memory.encode(command(6, n, 1, 1, 0, 1, 1, 0, true));
    ASSERT_TRUE(r.differential());
    for (size_t i = 0; i < n; ++i) {
      r.memory.cost(kA + i * 4, ACCEL_INF);
      r.memory.cost(kB + i * 4, 0);
    }
    r.memory.encode(command(3, n));
    ASSERT_TRUE(r.differential());
    EXPECT_EQ(r.memory.cost(kOut), ACCEL_INF);
    EXPECT_EQ(r.memory.cost(kOut + 4), 0);
    if (n > 17) {
      r.memory.cost(kA + 7 * 4, -10);
      r.memory.cost(kA + 17 * 4, -10);
      ASSERT_TRUE(r.differential());
      EXPECT_EQ(r.memory.cost(kOut + 4), 7);
    }
  }
}
TEST(L2, ArithmeticFailureAndRecovery) {
  auto &r = *rigs[0];
  for (const auto &values : std::array<std::array<int32_t, 3>, 7>{{{ACCEL_INF, ACCEL_INF + 1, 0},
                                                                   {ACCEL_INF, 0, ACCEL_INF + 1},
                                                                   {INT32_MIN, -1, ACCEL_INF},
                                                                   {INT32_MIN, 0, -1},
                                                                   {ACCEL_INF - 1, 1, -1},
                                                                   {INT32_MIN, 0, 0},
                                                                   {-7, 4, -3}}}) {
    r.memory.cost(kA, values[0]);
    r.memory.cost(kB, values[1]);
    r.memory.cost(kC, values[2]);
    for (int op : {1, 2, 3, 4, 6, 7, 8}) {
      r.memory.encode(command(op, 1));
      ASSERT_TRUE(r.differential());
    }
    fill(r.memory, 17, 9);
    r.memory.encode(command(8, 17, 9));
    ASSERT_TRUE(r.differential());
    EXPECT_EQ(r.p2.status(), ACCEL_STATUS_DONE);
  }
}
TEST(L2, BusySnapshotAndTerminalVisibility) {
  auto &r = *rigs[0];
  fill(r.memory, 64, 17);
  r.memory.encode(command(8, 64, 17));
  ASSERT_TRUE(r.p2.start());
  EXPECT_EQ(r.p2.status(), ACCEL_STATUS_BUSY);
  sc_core::sc_start(r.l2.config().cycle_period * 2);
  EXPECT_EQ(r.p2.status(), ACCEL_STATUS_BUSY);
  uint32_t value = 1;
  EXPECT_FALSE(r.p2.mmio(ACCEL_MMIO_DOORBELL, &value, true));
  value = 0;
  EXPECT_TRUE(r.p2.mmio(ACCEL_MMIO_DESC_ADDR_LO, &value, true));
  EXPECT_EQ(r.p2.finish(r.l2.config()), ACCEL_STATUS_DONE);
  sc_core::sc_start(r.l2.config().cycle_period * 10);
  EXPECT_EQ(r.p2.status(), ACCEL_STATUS_DONE);
  EXPECT_FALSE(r.p2.mmio(ACCEL_MMIO_STATUS, &value, true));
  EXPECT_FALSE(r.p2.mmio(ACCEL_MMIO_DOORBELL, &value, false));
}
TEST(L2, MixedStreamsProducerConsumerAndFailStop) {
  auto &r = *rigs[0];
  for (int mode = 0; mode < 6; ++mode) {
    fill(r.memory, 17, 9);
    auto project = command(7, 17, 9);
    auto add = command(6, 9);
    add.operation.vector_add.first = pcaa_cost_vector(kOut, 9, 1);
    add.operation.vector_add.result = pcaa_cost_output(kA, 9, 1);
    auto map3 = command(8, 17, 9);
    map3.operation.map3_project.result.base = kOut + 0x1000;
    batch(r.memory, {project, add, map3}, mode == 1 ? 16 : 0, mode == 2 ? 4 : 0);
    if (mode == 3)
      r.memory.data[kChildren + 48] = 99;
    if (mode == 4)
      r.memory.data[kChildren + 48 + 2] = 1;
    if (mode == 5) {  // Nested batch is rejected after the successful first child.
      pcaa_command_t nested{};
      ASSERT_EQ(pcaa_make_ordered_batch(kChildren, 1, 32, kBatchResult, &nested), PCAA_STATUS_OK);
      r.memory.encode(nested, kChildren + 48);
    }
    ASSERT_TRUE(r.differential()) << mode;
    EXPECT_EQ(r.memory.cost(kBatchResult), mode >= 3 ? 1 : 3);
    EXPECT_EQ(r.memory.cost(kBatchResult + 4), mode == 0 ? -1 : mode >= 3 ? 1 : 3);
  }
}
TEST(L2, ActualOutputProtectionAllowsDescriptorPadding) {
  auto &r = *rigs[0];
  fill(r.memory, 2, 1);
  auto add = command(6, 2);
  add.operation.vector_add.result = pcaa_cost_output(kChildren - 4, 2, 32);
  batch(r.memory, {add});
  ASSERT_TRUE(r.differential());
  EXPECT_EQ(r.p2.status(), ACCEL_STATUS_DONE);
  add.operation.vector_add.result = pcaa_cost_output(kChildren, 2, 32);
  batch(r.memory, {add});
  ASSERT_TRUE(r.differential());
  EXPECT_EQ(r.p2.status(), ACCEL_STATUS_ERROR);
}
TEST(L2, MemoryFaultsSplitWriteRecordAndRecovery) {
  auto &r = *rigs[0];
  for (uint64_t address : {kDescriptor, kDescriptor + 8, kChildren, kChildren + 8, kA, kB, kC,
                           kOut + 16, kBatchResult}) {
    for (bool write : {false, true}) {
      fill(r.memory, 17, 9, 1, 0, 1, 3);
      batch(r.memory, {command(8, 17, 9, 1, 0, 1, 1, 3)});
      r.memory.fail_begin = address;
      r.memory.fail_end = address + 4;
      r.memory.fail_read = !write;
      r.memory.fail_write = write;
      ASSERT_TRUE(r.differential()) << address << ' ' << write;
      const auto writes = r.memory.writes;
      sc_core::sc_start(r.l2.config().cycle_period * 100);
      EXPECT_EQ(r.memory.writes, writes);
      r.memory.fail_read = r.memory.fail_write = false;
      fill(r.memory, 3, 1);
      r.memory.encode(command(3, 3));
      ASSERT_TRUE(r.differential());
      EXPECT_EQ(r.p2.status(), ACCEL_STATUS_DONE);
    }
  }
}
TEST(L2, MalformedWireAndSpanOverflow) {
  auto &r = *rigs[0];
  for (size_t offset : {size_t{0}, size_t{1}, size_t{2}, size_t{3}, size_t{6}}) {
    fill(r.memory, 3, 1);
    r.memory.encode(command(1, 3));
    r.memory.data[kDescriptor + offset] = 255;
    ASSERT_TRUE(r.differential());
    EXPECT_EQ(r.p2.status(), ACCEL_STATUS_ERROR);
  }
  r.memory.encode(command(1, 3));
  for (size_t i = 0; i < 8; ++i) r.memory.data[kDescriptor + 8 + i] = 255;
  ASSERT_TRUE(r.differential());
  EXPECT_EQ(r.p2.status(), ACCEL_STATUS_ERROR);
  ASSERT_TRUE(r.differential(UINT64_MAX - 4));
  EXPECT_EQ(r.p2.status(), ACCEL_STATUS_ERROR);
  r.memory.encode(command(1, 3));
  ASSERT_TRUE(r.differential(kMemorySize - 4));
  EXPECT_EQ(r.p2.status(), ACCEL_STATUS_ERROR);
}
TEST(L2, ArchitecturalMaximumLengthAndStride) {
  auto &r = *rigs[0];
  fill(r.memory, UINT16_MAX, 1);
  r.memory.encode(command(3, UINT16_MAX));
  ASSERT_TRUE(r.differential());
  fill(r.memory, 2, 1, UINT16_MAX, 0, UINT16_MAX);
  r.memory.encode(command(8, 2, 1, UINT16_MAX, 1, UINT16_MAX));
  ASSERT_TRUE(r.differential());
}

TEST(L2, AliasValidationAndWireCanonicality) {
  auto &r = *rigs[0];
  for (int mode = 0; mode < 3; ++mode) {
    fill(r.memory, 17, 1);
    auto c = command(6, 17);
    c.operation.vector_add.result = pcaa_cost_output(mode == 0 ? kB : kA, 17, 1);
    if (mode == 2)
      c.operation.vector_add.second = c.operation.vector_add.first;
    r.memory.encode(c);
    ASSERT_TRUE(r.differential());
  }
  fill(r.memory, 17, 1);
  r.memory.encode(command(6, 17));
  const uint64_t partial = kA + sizeof(int32_t);
  for (size_t i = 0; i < sizeof(partial); ++i)
    r.memory.data[kDescriptor + 24 + i] = static_cast<unsigned char>(partial >> (8 * i));
  ASSERT_TRUE(r.differential());
  EXPECT_EQ(r.p2.status(), ACCEL_STATUS_ERROR);
  r.memory.encode(command(6, 17));
  for (size_t i = 0; i < sizeof(kA); ++i)
    r.memory.data[kDescriptor + 24 + i] = static_cast<unsigned char>(kA >> (8 * i));
  ASSERT_TRUE(r.differential());
  EXPECT_EQ(r.p2.status(), ACCEL_STATUS_ERROR);
}
TEST(L2, TruncatedMixedChildAndMalformedParent) {
  auto &r = *rigs[0];
  fill(r.memory, 17, 9);
  for (int mode = 0; mode < 3; ++mode) {
    batch(r.memory, {command(3, 17), command(8, 17, 9)});
    if (mode == 0) {
      // 32-byte first child plus only 32 of the required 64-byte second child.
      r.memory.cost(kDescriptor + 28, 64);
    } else if (mode == 1) {
      for (size_t i = 0; i < sizeof(kDescriptor); ++i)
        r.memory.data[kDescriptor + 16 + i] = static_cast<unsigned char>(kDescriptor >> (8 * i));
    } else
      r.memory.cost(kDescriptor + 24, 0);
    ASSERT_TRUE(r.differential());
    EXPECT_EQ(r.p2.status(), ACCEL_STATUS_ERROR);
    if (mode == 0) {
      EXPECT_EQ(r.memory.cost(kBatchResult), 1);
      EXPECT_EQ(r.memory.cost(kBatchResult + 4), 1);
    }
  }
}
TEST(L2, MaximumOutputDimensionAndOwnAperture) {
  auto &r = *rigs[0];
  fill(r.memory, 1, UINT16_MAX, 1, 1);
  r.memory.encode(command(8, 1, UINT16_MAX, 1, 1));
  ASSERT_TRUE(r.differential());
  auto c = command(1, 1);
  c.operation.reduce2.first.base = r.l2.config().mmio_base;
  r.memory.encode(c);
  EXPECT_EQ(r.run(), ACCEL_STATUS_ERROR);
  EXPECT_EQ(r.l2.diagnostic().cause, PCAA_STATUS_RANGE);
  fill(r.memory, 3, 1);
  r.memory.encode(command(3, 3));
  ASSERT_TRUE(r.differential());
}
TEST(L2, ProjectionReuseAndExactPayloadAccounting) {
  auto &r = *rigs[0];
  for (int op : {7, 8}) {
    fill(r.memory, 31, 17);
    const auto c = command(op, 31, 17);
    r.memory.encode(c);
    r.l2.reset_statistics();
    ASSERT_TRUE(r.differential());
    const auto &s = r.l2.statistics();
    const size_t shared = op == 7 ? 1 : 2;
    EXPECT_EQ(s.tiles, 3u);
    EXPECT_EQ(s.chunks, 6u);
    EXPECT_EQ(s.shared_loads, shared * 6);
    EXPECT_EQ(s.shared_reread_bytes, shared * 2 * 31 * sizeof(int32_t));
    EXPECT_EQ(s.operand_bytes, (17 * 31 + shared * 3 * 31) * sizeof(int32_t));
    EXPECT_EQ(s.result_bytes, 17 * (op == 7 ? sizeof(int32_t) : sizeof(accel_min_argmin_result_t)));
    EXPECT_EQ(s.descriptor_bytes, op == 7 ? 48u : 64u);
  }
}
}  // namespace
int sc_main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  rigs.push_back(std::make_unique<l2_test::Rig>());
  for (size_t lanes : {size_t{1}, size_t{3}, size_t{8}}) {
    L2Config config;
    config.lanes = lanes;
    config.tn = 7;
    config.tm = 3;
    config.mem_bytes = 7;
    config.memory_latency = 4;
    config.acceptance_delay = 2;
    rigs.push_back(std::make_unique<l2_test::Rig>(config));
  }
  sc_core::sc_start(sc_core::SC_ZERO_TIME);
  return RUN_ALL_TESTS();
}
