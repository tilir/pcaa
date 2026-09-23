// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Tests compact command layouts, canonicalization, and typed pcaalib failures.

#include "accel_protocol.h"
#include "pcaa.h"
#include "pcaa_codec.h"
#include "pcaa_device.h"
#include "pcaa_host_error.h"
#include "pcaa_submission.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

namespace {
constexpr pcaa_guest_address_t kFirst = 0x1100;
constexpr pcaa_guest_address_t kSecond = 0x2200;
constexpr pcaa_guest_address_t kThird = 0x3300;
constexpr pcaa_guest_address_t kResult = 0x4400;
constexpr size_t kLength = 3;

pcaa_command_t reduce2(bool argmin = false, size_t length = kLength) {
  pcaa_command_t command{};
  EXPECT_EQ(pcaa_make_reduce2(pcaa_cost_vector(kFirst, length, 1),
                              pcaa_cost_vector(kSecond, length, 1), kResult, argmin, &command),
            PCAA_STATUS_OK);
  return command;
}

pcaa_command_t vector_add(pcaa_guest_address_t dst, size_t dst_stride = 1) {
  pcaa_command_t command{};
  EXPECT_EQ(pcaa_make_cost_add_vector(pcaa_cost_vector(kFirst, kLength, 1),
                                      pcaa_cost_vector(kSecond, kLength, 1),
                                      pcaa_cost_output(dst, kLength, dst_stride), &command),
            PCAA_STATUS_OK);
  return command;
}

struct DeviceProbe {
  int submissions = 0;
};

pcaa_status_t probe_submit(void *opaque, const pcaa_command_t *) {
  ++static_cast<DeviceProbe *>(opaque)->submissions;
  return PCAA_STATUS_OK;
}

pcaa_status_t probe_batch(void *, const pcaa_command_t *, size_t) {
  return PCAA_STATUS_OK;
}

pcaa_status_t probe_wait(void *, pcaa_completion_t *completion) {
  if (completion != nullptr) {
    completion->has_batch_result = 1;
    completion->completed = 1;
    completion->failed_index = UINT32_MAX;
  }
  return PCAA_STATUS_OK;
}

TEST(PcaaDevice, DispatchesAndPrintsTypedStatuses) {
  DeviceProbe probe;
  pcaa_device_t device{&probe, probe_submit, probe_batch, probe_wait};
  pcaa_command_t command{};
  EXPECT_EQ(pcaa_device_submit(nullptr, &command), PCAA_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(pcaa_device_submit(&device, nullptr), PCAA_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(pcaa_device_submit_batch(&device, &command, 0), PCAA_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(pcaa_device_submit(&device, &command), PCAA_STATUS_OK);
  pcaa_completion_t completion{};
  EXPECT_EQ(pcaa_device_wait(&device, &completion), PCAA_STATUS_OK);
  EXPECT_EQ(completion.failed_index, UINT32_MAX);
  EXPECT_STREQ(pcaa_status_string(PCAA_STATUS_DEVICE_ERROR), "device reported error");
  testing::internal::CaptureStderr();
  pcaa_perror("pcaa", PCAA_STATUS_DEVICE_ERROR);
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "pcaa: device reported error\n");
  EXPECT_EQ(probe.submissions, 1);
}

TEST(PcaaCodec, ExactSizesAndRoundTripsEveryFamily) {
  EXPECT_STREQ(pcaa_version(), "2.0.0");
  EXPECT_STREQ(pcaa_isa_version(), "1.0.0");
  const auto first = pcaa_cost_vector(kFirst, kLength, 1);
  const auto second = pcaa_cost_vector(kSecond, kLength, 1);
  const auto third = pcaa_cost_vector(kThird, kLength, 1);
  const auto matrix = pcaa_cost_matrix(kThird, 2, kLength, 4, 1);
  std::array<pcaa_command_t, 9> commands{};
  commands[0] = reduce2();
  commands[2] = reduce2(true);
  commands[5] = vector_add(kResult);
  commands[6] = vector_add(kFirst);
  ASSERT_EQ(pcaa_make_reduce3(first, second, third, kResult, 0, &commands[1]), PCAA_STATUS_OK);
  ASSERT_EQ(pcaa_make_reduce3(first, second, third, kResult, 1, &commands[3]), PCAA_STATUS_OK);
  ASSERT_EQ(
      pcaa_make_minplus_project(matrix, second, pcaa_cost_output(kResult, 2, 1), &commands[4]),
      PCAA_STATUS_OK);
  ASSERT_EQ(pcaa_make_minplus_map3_project(first, second, matrix, pcaa_argmin_output(kResult, 2, 1),
                                           &commands[7]),
            PCAA_STATUS_OK);
  ASSERT_EQ(pcaa_make_ordered_batch(kFirst, 2, 80, kResult, &commands[8]), PCAA_STATUS_OK);
  const std::array<size_t, 9> sizes{32, 48, 32, 48, 48, 48, 32, 64, 32};
  for (size_t index = 0; index < commands.size(); ++index) {
    size_t measured = 0;
    ASSERT_EQ(pcaa_encoded_size(&commands[index], &measured), PCAA_STATUS_OK) << index;
    EXPECT_EQ(measured, sizes[index]);
    pcaa_encoded_slot_t encoded{};
    size_t written = 0;
    ASSERT_EQ(pcaa_encode_one(&commands[index], encoded.bytes, sizeof(encoded.bytes), &written),
              PCAA_STATUS_OK)
        << index;
    EXPECT_EQ(written, sizes[index]);
    pcaa_command_t decoded{};
    size_t consumed = 0;
    ASSERT_EQ(pcaa_decode_one(encoded.bytes, written, &decoded, &consumed), PCAA_STATUS_OK)
        << index;
    EXPECT_EQ(consumed, written);
    EXPECT_EQ(decoded.kind, commands[index].kind);
    pcaa_encoded_slot_t repeated{};
    size_t repeated_bytes = 0;
    ASSERT_EQ(pcaa_encode_one(&decoded, repeated.bytes, sizeof(repeated.bytes), &repeated_bytes),
              PCAA_STATUS_OK);
    EXPECT_EQ(std::memcmp(repeated.bytes, encoded.bytes, written), 0);
  }
}

TEST(PcaaCodec, HeaderOffsetsLittleEndianAndStrictReservedBytes) {
  pcaa_command_t command{};
  ASSERT_EQ(pcaa_make_minplus_project(pcaa_cost_matrix(kFirst, 2, 3, 1, 4),
                                      pcaa_cost_vector(kSecond, 3, 2),
                                      pcaa_cost_output(kResult, 2, 2), &command),
            PCAA_STATUS_OK);
  pcaa_encoded_slot_t encoded{};
  size_t width = 0;
  ASSERT_EQ(pcaa_encode_one(&command, encoded.bytes, sizeof(encoded.bytes), &width),
            PCAA_STATUS_OK);
  EXPECT_EQ(width, 48u);
  EXPECT_EQ(encoded.bytes[0], ACCEL_OPCODE_MINPLUS_PROJECT);
  EXPECT_EQ(encoded.bytes[1], ACCEL_FORMAT_MINPLUS_PROJECT);
  EXPECT_EQ(encoded.bytes[4], 3u);
  EXPECT_EQ(encoded.bytes[6], 2u);
  EXPECT_EQ(encoded.bytes[8], static_cast<unsigned char>(kFirst));
  EXPECT_EQ(encoded.bytes[9], static_cast<unsigned char>(kFirst >> 8));
  EXPECT_EQ(encoded.bytes[32], 4u);
  EXPECT_EQ(encoded.bytes[34], 1u);
  EXPECT_EQ(encoded.bytes[36], 2u);
  EXPECT_EQ(encoded.bytes[38], 2u);
  pcaa_command_t decoded{};
  size_t consumed = 999;
  encoded.bytes[2] = 1;
  EXPECT_EQ(pcaa_decode_one(encoded.bytes, width, &decoded, &consumed),
            PCAA_STATUS_INVALID_COMMAND);
  EXPECT_EQ(consumed, 999u);
  encoded.bytes[2] = 0;
  encoded.bytes[40] = 1;
  EXPECT_EQ(pcaa_decode_one(encoded.bytes, width, &decoded, &consumed),
            PCAA_STATUS_INVALID_COMMAND);
  encoded.bytes[40] = 0;
  encoded.bytes[1] = ACCEL_FORMAT_REDUCE2;
  EXPECT_EQ(pcaa_decode_one(encoded.bytes, width, &decoded, &consumed),
            PCAA_STATUS_INVALID_COMMAND);
  encoded.bytes[1] = ACCEL_FORMAT_MINPLUS_PROJECT;
  EXPECT_EQ(pcaa_decode_one(encoded.bytes, width - 1, &decoded, &consumed),
            PCAA_STATUS_INVALID_COMMAND);
}

TEST(PcaaCodec, RangeBoundariesAndUnchangedFailureOutputs) {
  pcaa_command_t command = reduce2();
  EXPECT_EQ(pcaa_make_reduce2(pcaa_cost_vector(kFirst, 65535, 1),
                              pcaa_cost_vector(kSecond, 65535, 1), kResult, 0, &command),
            PCAA_STATUS_OK);
  EXPECT_EQ(pcaa_make_reduce2(pcaa_cost_vector(kFirst, 65536, 1),
                              pcaa_cost_vector(kSecond, 65536, 1), kResult, 0, &command),
            PCAA_STATUS_RANGE);
  EXPECT_EQ(command.operation.reduce2.first.length, 65535u);
  pcaa_encoded_slot_t encoded{};
  size_t written = 0;
  ASSERT_EQ(pcaa_encode_one(&command, encoded.bytes, sizeof(encoded.bytes), &written),
            PCAA_STATUS_OK);
  EXPECT_EQ(encoded.bytes[4], 0xffu);
  EXPECT_EQ(encoded.bytes[5], 0xffu);
  EXPECT_EQ(
      pcaa_make_cost_add_vector(pcaa_cost_vector(kFirst, 1, 65535), pcaa_cost_vector(kSecond, 1, 1),
                                pcaa_cost_output(kResult, 1, 1), &command),
      PCAA_STATUS_OK);
  ASSERT_EQ(pcaa_encode_one(&command, encoded.bytes, sizeof(encoded.bytes), &written),
            PCAA_STATUS_OK);
  EXPECT_EQ(encoded.bytes[32], 0xffu);
  EXPECT_EQ(encoded.bytes[33], 0xffu);
  EXPECT_EQ(
      pcaa_make_cost_add_vector(pcaa_cost_vector(kFirst, 1, 65536), pcaa_cost_vector(kSecond, 1, 1),
                                pcaa_cost_output(kResult, 1, 1), &command),
      PCAA_STATUS_RANGE);
  EXPECT_EQ(
      pcaa_make_ordered_batch(kFirst, 1, static_cast<size_t>(UINT32_MAX) + 1, kResult, &command),
      PCAA_STATUS_RANGE);
  encoded.bytes[0] = 0xaa;
  written = 123;
  EXPECT_EQ(pcaa_encode_one(&command, encoded.bytes, 1, &written), PCAA_STATUS_NO_SPACE);
  EXPECT_EQ(encoded.bytes[0], 0xaa);
  EXPECT_EQ(written, 123u);
}

TEST(PcaaCodec, RejectsAddressSpanOverflowAndEveryReservedTail) {
  pcaa_command_t command = vector_add(kResult);
  command.operation.vector_add.first.base = UINT64_MAX - 3;
  size_t bytes = 777;
  EXPECT_EQ(pcaa_encoded_size(&command, &bytes), PCAA_STATUS_INVALID_COMMAND);
  EXPECT_EQ(bytes, 777u);

  command = reduce2();
  std::array<pcaa_command_t, 4> commands{command, vector_add(kResult), vector_add(kFirst), {}};
  ASSERT_EQ(
      pcaa_make_reduce3(pcaa_cost_vector(kFirst, kLength, 1), pcaa_cost_vector(kSecond, kLength, 1),
                        pcaa_cost_vector(kThird, kLength, 1), kResult, 0, &commands[3]),
      PCAA_STATUS_OK);
  const std::array<size_t, 4> reserved_offsets{2, 38, 28, 40};
  for (size_t index = 0; index < commands.size(); ++index) {
    pcaa_encoded_slot_t encoded{};
    size_t written = 0;
    ASSERT_EQ(pcaa_encode_one(&commands[index], encoded.bytes, sizeof(encoded.bytes), &written),
              PCAA_STATUS_OK);
    pcaa_command_t decoded{};
    size_t consumed = 999;
    encoded.bytes[reserved_offsets[index]] = 1;
    EXPECT_EQ(pcaa_decode_one(encoded.bytes, written, &decoded, &consumed),
              PCAA_STATUS_INVALID_COMMAND);
    EXPECT_EQ(consumed, 999u);
  }
}

TEST(PcaaCodec, VectorAddCanonicalizesBothExactAliases) {
  const pcaa_command_t general = vector_add(kResult);
  const pcaa_command_t alias0 = vector_add(kFirst);
  const pcaa_command_t alias1 = vector_add(kSecond);
  std::array<pcaa_command_t, 3> commands{general, alias0, alias1};
  const std::array<size_t, 3> expected{48, 32, 32};
  for (size_t index = 0; index < commands.size(); ++index) {
    pcaa_encoded_slot_t encoded{};
    size_t bytes = 0;
    ASSERT_EQ(pcaa_encode_one(&commands[index], encoded.bytes, sizeof(encoded.bytes), &bytes),
              PCAA_STATUS_OK);
    EXPECT_EQ(bytes, expected[index]);
    EXPECT_EQ(encoded.bytes[1], index == 0 ? ACCEL_FORMAT_COST_ADD_VECTOR_GENERAL
                                           : ACCEL_FORMAT_COST_ADD_VECTOR_INPLACE);
    if (index == 2) {
      EXPECT_EQ(encoded.bytes[8], static_cast<unsigned char>(kSecond));
      EXPECT_EQ(encoded.bytes[16], static_cast<unsigned char>(kFirst));
      EXPECT_EQ(commands[index].operation.vector_add.first.base, kFirst);
    }
  }
  pcaa_command_t rejected{};
  EXPECT_EQ(
      pcaa_make_cost_add_vector(pcaa_cost_vector(kFirst, 3, 1), pcaa_cost_vector(kSecond, 3, 1),
                                pcaa_cost_output(kFirst, 3, 2), &rejected),
      PCAA_STATUS_INVALID_COMMAND);
  EXPECT_EQ(
      pcaa_make_cost_add_vector(pcaa_cost_vector(kFirst, 3, 1), pcaa_cost_vector(kSecond, 3, 1),
                                pcaa_cost_output(kFirst + 4, 3, 1), &rejected),
      PCAA_STATUS_INVALID_COMMAND);
  EXPECT_EQ(
      pcaa_make_cost_add_vector(pcaa_cost_vector(kFirst, 3, 1), pcaa_cost_vector(kFirst, 3, 1),
                                pcaa_cost_output(kFirst, 3, 1), &rejected),
      PCAA_STATUS_OK);
  size_t bytes = 0;
  EXPECT_EQ(pcaa_encoded_size(&rejected, &bytes), PCAA_STATUS_OK);
  EXPECT_EQ(bytes, 32u);
}

TEST(PcaaCodec, WalksMixedStreamAndRejectsNestedBatch) {
  std::array<pcaa_command_t, 3> commands{reduce2(), vector_add(kResult), vector_add(kSecond)};
  size_t measured = 0;
  ASSERT_EQ(pcaa_encoded_stream_size(commands.data(), commands.size(), &measured), PCAA_STATUS_OK);
  EXPECT_EQ(measured, 112u);
  std::array<unsigned char, 128> stream{};
  size_t written = 0;
  ASSERT_EQ(
      pcaa_encode_stream(commands.data(), commands.size(), stream.data(), stream.size(), &written),
      PCAA_STATUS_OK);
  EXPECT_EQ(written, measured);
  size_t cursor = 0;
  for (const auto &expected : commands) {
    pcaa_command_t decoded{};
    size_t consumed = 0;
    ASSERT_EQ(pcaa_decode_one(stream.data() + cursor, written - cursor, &decoded, &consumed),
              PCAA_STATUS_OK);
    EXPECT_EQ(decoded.kind, expected.kind);
    cursor += consumed;
  }
  EXPECT_EQ(cursor, written);
  pcaa_command_t parent{};
  ASSERT_EQ(pcaa_make_ordered_batch(kFirst, commands.size(), written, kResult, &parent),
            PCAA_STATUS_OK);
  commands[1] = parent;
  EXPECT_EQ(pcaa_encoded_stream_size(commands.data(), commands.size(), &measured),
            PCAA_STATUS_INVALID_COMMAND);
}
}  // namespace
