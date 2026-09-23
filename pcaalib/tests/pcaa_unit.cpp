// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Checks semantic builders and exact descriptor codec behavior without SystemC.

#include "pcaa.h"
#include "pcaa_device.h"
#include "pcaa_host_error.h"
#include "pcaa_submission.h"
#include "accel_protocol.h"
#include "pcaa_codec.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

namespace {
constexpr uint64_t kFirst = 0x100;
constexpr uint64_t kSecond = 0x200;
constexpr uint64_t kThird = 0x300;
constexpr uint64_t kResult = 0x400;
constexpr uint32_t kLength = 3;
constexpr size_t kWireOpcodeOffset = 0x00;
constexpr size_t kWireFirstSourceOffset = 0x18;
constexpr size_t kWireFirstStrideOffset = 0x38;
constexpr size_t kWireFirstOuterStrideOffset = 0x48;

struct DeviceProbe {
  int submissions = 0;
  size_t batch_size = 0;
  int waits = 0;
};

pcaa_status_t probe_submit(void *opaque, const pcaa_command_t *) {
  ++static_cast<DeviceProbe *>(opaque)->submissions;
  return PCAA_STATUS_OK;
}

pcaa_status_t probe_batch(void *opaque, const pcaa_command_t *, size_t count) {
  static_cast<DeviceProbe *>(opaque)->batch_size = count;
  return PCAA_STATUS_OK;
}

pcaa_status_t probe_wait(void *opaque, pcaa_completion_t *completion) {
  ++static_cast<DeviceProbe *>(opaque)->waits;
  if (completion != nullptr) {
    completion->has_batch_result = 1;
    completion->completed = 1;
    completion->failed_index = UINT32_MAX;
  }
  return PCAA_STATUS_OK;
}

TEST(PcaaDevice, DispatchesWithoutExposingTransport) {
  DeviceProbe probe;
  pcaa_device_t device{&probe, probe_submit, probe_batch, probe_wait};
  pcaa_command_t command{};
  EXPECT_EQ(pcaa_device_submit(nullptr, &command), PCAA_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(pcaa_device_submit(&device, nullptr), PCAA_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(pcaa_device_submit_batch(&device, &command, 0), PCAA_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(pcaa_device_wait(nullptr, nullptr), PCAA_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(pcaa_device_submit(&device, &command), PCAA_STATUS_OK);
  EXPECT_EQ(pcaa_device_submit_batch(&device, &command, 1), PCAA_STATUS_OK);
  pcaa_completion_t completion{};
  EXPECT_EQ(pcaa_device_wait(&device, &completion), PCAA_STATUS_OK);
  EXPECT_EQ(completion.has_batch_result, 1);
  EXPECT_EQ(completion.completed, 1u);
  EXPECT_EQ(completion.failed_index, UINT32_MAX);
  EXPECT_STREQ(pcaa_status_string(PCAA_STATUS_DEVICE_ERROR), "device reported error");
  testing::internal::CaptureStderr();
  pcaa_perror("pcaa", PCAA_STATUS_DEVICE_ERROR);
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "pcaa: device reported error\n");
  device.submit_command = [](void *, const pcaa_command_t *) { return PCAA_STATUS_NO_SPACE; };
  EXPECT_EQ(pcaa_device_submit(&device, &command), PCAA_STATUS_NO_SPACE);
  EXPECT_EQ(probe.submissions, 1);
  EXPECT_EQ(probe.batch_size, 1u);
  EXPECT_EQ(probe.waits, 1);
}

TEST(PcaaCodec, RoundTripsEverySemanticFamily) {
  EXPECT_STREQ(pcaa_version(), "1.0.0");
  EXPECT_STREQ(pcaa_isa_version(), "1.0.0");
  const auto first = pcaa_cost_vector(kFirst, kLength, 1);
  const auto second = pcaa_cost_vector(kSecond, kLength, 1);
  const auto third = pcaa_cost_vector(kThird, kLength, 1);
  const auto matrix = pcaa_cost_matrix(kThird, 2, kLength, 4, 1);
  std::array<pcaa_command_t, 8> commands{};
  ASSERT_EQ(pcaa_make_reduce2(first, second, kResult, 0, &commands[0]), 0);
  ASSERT_EQ(pcaa_make_reduce3(first, second, third, kResult, 0, &commands[1]), 0);
  ASSERT_EQ(pcaa_make_reduce2(first, second, kResult, 1, &commands[2]), 0);
  ASSERT_EQ(pcaa_make_reduce3(first, second, third, kResult, 1, &commands[3]), 0);
  ASSERT_EQ(
      pcaa_make_cost_add_vector(first, second, pcaa_cost_output(kResult, kLength, 1), &commands[4]),
      0);
  ASSERT_EQ(
      pcaa_make_minplus_project(matrix, second, pcaa_cost_output(kResult, 2, 1), &commands[5]), 0);
  ASSERT_EQ(pcaa_make_minplus_map3_project(first, second, matrix, pcaa_argmin_output(kResult, 2, 1),
                                           &commands[6]),
            0);
  ASSERT_EQ(pcaa_make_ordered_batch(kFirst, 2, kResult, &commands[7]), 0);
  for (size_t index = 0; index < commands.size(); ++index) {
    accel_command_t wire{};
    pcaa_command_t decoded{};
    ASSERT_EQ(pcaa_encode_descriptor(&commands[index], &wire), 0) << index;
    ASSERT_EQ(pcaa_decode_descriptor(&wire, &decoded), 0) << index;
    EXPECT_EQ(decoded.kind, commands[index].kind);
    accel_command_t repeated{};
    ASSERT_EQ(pcaa_encode_descriptor(&decoded, &repeated), 0);
    EXPECT_EQ(std::memcmp(&repeated, &wire, sizeof(wire)), 0);
  }
  EXPECT_EQ(pcaa_descriptor_bytes(), sizeof(accel_command_t));
  EXPECT_EQ(pcaa_batch_descriptor_bytes(2), 3 * sizeof(accel_command_t));
}

TEST(PcaaCodec, PreservesOrientationAndRejectsMalformedWire) {
  pcaa_command_t semantic{};
  ASSERT_EQ(pcaa_make_minplus_project(pcaa_cost_matrix(kFirst, 2, 3, 1, 4),
                                      pcaa_cost_vector(kSecond, 3, 2),
                                      pcaa_cost_output(kResult, 2, 2), &semantic),
            0);
  accel_command_t wire{};
  ASSERT_EQ(pcaa_encode_descriptor(&semantic, &wire), 0);
  EXPECT_EQ(wire.opcode, ACCEL_OPCODE_MINPLUS_PROJECT);
  EXPECT_EQ(wire.src0_outer_stride, 1u);
  EXPECT_EQ(wire.src0_stride, 4u);
  EXPECT_EQ(wire.src1_stride, 2u);
  EXPECT_EQ(wire.dst_stride, 2u);
  const auto *bytes = reinterpret_cast<const unsigned char *>(&wire);
  EXPECT_EQ(bytes[kWireOpcodeOffset], ACCEL_OPCODE_MINPLUS_PROJECT);
  EXPECT_EQ(bytes[kWireFirstSourceOffset], static_cast<unsigned char>(kFirst));
  EXPECT_EQ(bytes[kWireFirstSourceOffset + 1], static_cast<unsigned char>(kFirst >> 8));
  EXPECT_EQ(bytes[kWireFirstStrideOffset], 4u);
  EXPECT_EQ(bytes[kWireFirstOuterStrideOffset], 1u);
  wire.flags = 1;
  EXPECT_NE(pcaa_decode_descriptor(&wire, &semantic), 0);
  wire.flags = 0;
  wire.src0_stride = 0;
  EXPECT_NE(pcaa_decode_descriptor(&wire, &semantic), 0);
  wire.src0_stride = 4;
  wire.dst = kFirst;
  EXPECT_NE(pcaa_decode_descriptor(&wire, &semantic), 0);
}

TEST(PcaaCodec, RejectsDimensionsTooWideForCurrentDescriptor) {
  if (sizeof(size_t) <= sizeof(uint32_t))
    GTEST_SKIP() << "size_t cannot exceed a descriptor dimension";
  pcaa_command_t command{};
  const size_t oversized = static_cast<size_t>(UINT32_MAX) + 1;
  EXPECT_EQ(pcaa_make_reduce2(pcaa_cost_vector(kFirst, oversized, 1),
                              pcaa_cost_vector(kSecond, oversized, 1), kResult, 0, &command),
            PCAA_STATUS_RANGE);
  EXPECT_EQ(pcaa_make_ordered_batch(kFirst, oversized, kResult, &command), PCAA_STATUS_RANGE);
}

TEST(PcaaSubmission, PacksOrderedCommandsWithoutExposingWireToCaller) {
  const auto first = pcaa_cost_vector(kFirst, kLength, 1);
  const auto second = pcaa_cost_vector(kSecond, kLength, 1);
  std::array<pcaa_command_t, 2> commands{};
  ASSERT_EQ(pcaa_make_reduce2(first, second, kResult, 1, &commands[0]), 0);
  ASSERT_EQ(
      pcaa_make_cost_add_vector(first, second, pcaa_cost_output(kResult, kLength, 1), &commands[1]),
      0);
  std::array<pcaa_encoded_slot_t, 2> packed{};
  ASSERT_EQ(pcaa_encode_commands(commands.data(), commands.size(), packed.data(), sizeof(packed)),
            0);
  EXPECT_EQ(pcaa_encoded_command_bytes(), sizeof(accel_command_t));
  EXPECT_EQ(pcaa_encoded_batch_bytes(commands.size()), 3 * sizeof(accel_command_t));
  for (size_t index = 0; index < commands.size(); ++index) {
    accel_command_t wire{};
    std::memcpy(&wire,
                reinterpret_cast<const unsigned char *>(packed.data()) +
                    index * pcaa_encoded_command_bytes(),
                sizeof(wire));
    pcaa_command_t decoded{};
    ASSERT_EQ(pcaa_decode_descriptor(&wire, &decoded), 0);
    EXPECT_EQ(decoded.kind, commands[index].kind);
  }
  pcaa_encoded_slot_t batch{};
  ASSERT_EQ(pcaa_encode_batch(kFirst, 2, kResult, &batch), 0);
  accel_command_t wire_batch{};
  std::memcpy(&wire_batch, batch.bytes, sizeof(wire_batch));
  EXPECT_EQ(wire_batch.opcode, ACCEL_OPCODE_EXECUTE_BATCH);
  EXPECT_EQ(wire_batch.n, 2u);
}
}  // namespace
