// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Exercises the accelerator through TLM MMIO using an in-memory guest RAM.

#include "accelerator.h"
#include "accel_protocol.h"
#include "cost_math.h"
#include "memory_interface.h"
#include "pcaa.h"
#include "pcaa_codec.h"
#include "timing_model.h"

#include <array>
#include <cstring>
#include <initializer_list>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <sysc/communication/sc_port.h>
#include <sysc/kernel/sc_dynamic_processes.h>
#include <sysc/kernel/sc_externs.h>
#include <sysc/kernel/sc_module.h>
#include <sysc/kernel/sc_module_name.h>
#include <sysc/kernel/sc_simcontext.h>
#include <sysc/kernel/sc_spawn.h>
#include <sysc/kernel/sc_time.h>
#include <sysc/kernel/sc_wait.h>
#include <tlm_core/tlm_2/tlm_2_interfaces/tlm_fw_bw_ifs.h>
#include <tlm_core/tlm_2/tlm_generic_payload/tlm_gp.h>
#include <tlm_core/tlm_2/tlm_generic_payload/tlm_phase.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>

#include <gtest/gtest.h>

#define CHECK(expression) EXPECT_TRUE(expression)

class TestMemory final : public MemoryInterface {
 public:
  explicit TestMemory(size_t size) : data(size) {}

  bool read(uint64_t address, void *destination, size_t size) override {
    if (!contains(address, size)) {
      return false;
    }
    std::memcpy(destination, data.data() + address, size);
    return true;
  }

  bool write(uint64_t address, const void *source, size_t size) override {
    if (!contains(address, size)) {
      return false;
    }
    std::memcpy(data.data() + address, source, size);
    return true;
  }

 private:
  bool contains(uint64_t address, size_t size) const {
    return address <= data.size() && size <= data.size() - address;
  }

 public:
  std::vector<unsigned char> data;
};

class TestInitiator final : public sc_core::sc_module {
 public:
  tlm_utils::simple_initiator_socket<TestInitiator> socket;

  explicit TestInitiator(sc_core::sc_module_name name)
      : sc_core::sc_module(name), socket("socket") {}
};

namespace {
constexpr size_t kTestMemorySize = 4096;
constexpr int kAddressLowBits = 32;
constexpr uint32_t kDoorbellSubmit = 1;
constexpr uint64_t kInvalidMmioOffset = 2;
constexpr uint64_t kDescriptorAddress = 0x100;
constexpr uint64_t kFirstInputAddress = 0x200;
constexpr uint64_t kSecondInputAddress = 0x240;
constexpr uint64_t kThirdInputAddress = 0x280;
constexpr uint64_t kResultAddress = 0x300;
constexpr uint64_t kBatchDescriptorAddress = 0x40;
constexpr uint64_t kBatchResultAddress = 0x340;
constexpr uint64_t kBatchSecondResultAddress = 0x360;
constexpr uint64_t kTieFirstInputAddress = 0x380;
constexpr uint64_t kTieSecondInputAddress = 0x3a0;
constexpr uint64_t kTieResultAddress = 0x3c0;
constexpr uint64_t kLargeFirstInputAddress = 0x500;
constexpr uint64_t kLargeSecondInputAddress = 0x900;
constexpr uint64_t kLargeResultAddress = 0xd00;
constexpr size_t kLargeVectorLength = 256;
constexpr uint64_t kTruncatedDescriptorAddress = kTestMemorySize - 6;
constexpr uint64_t kInvalidElementAddress = kTestMemorySize - 1;
constexpr uint32_t kUnsupportedOpcode = 99;

/* Test-only editable fixture, deliberately not a production wire descriptor. */
struct CommandFixture {
  uint32_t opcode = 0;
  uint32_t flags = 0;
  uint32_t n = 0;
  uint32_t m = 0;
  uint32_t k = 0;  // Explicit child_bytes for a batch; zero defaults to n scalar children.
  uint32_t reserved = 0;
  uint64_t src0 = 0;
  uint64_t src1 = 0;
  uint64_t src2 = 0;
  uint64_t dst = 0;
  uint32_t src0_stride = 0;
  uint32_t src1_stride = 0;
  uint32_t src2_stride = 0;
  uint32_t dst_stride = 0;
  uint32_t src0_outer_stride = 0;
  uint32_t src2_outer_stride = 0;
};

void fixture16(unsigned char *bytes, size_t offset, uint16_t value) {
  bytes[offset] = static_cast<unsigned char>(value);
  bytes[offset + 1] = static_cast<unsigned char>(value >> 8);
}

void fixture32(unsigned char *bytes, size_t offset, uint32_t value) {
  for (size_t index = 0; index < 4; ++index)
    bytes[offset + index] = static_cast<unsigned char>(value >> (8 * index));
}

void fixture64(unsigned char *bytes, size_t offset, uint64_t value) {
  for (size_t index = 0; index < 8; ++index)
    bytes[offset + index] = static_cast<unsigned char>(value >> (8 * index));
}

size_t encode_fixture(const CommandFixture &command, unsigned char *bytes) {
  std::memset(bytes, 0, ACCEL_COMMAND_MAX_BYTES);
  uint8_t format = 0;
  switch (command.opcode) {
    case ACCEL_OPCODE_MAP_ADD_REDUCE_MIN:
      format = ACCEL_FORMAT_REDUCE2;
      break;
    case ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN:
      format = ACCEL_FORMAT_REDUCE2_ARGMIN;
      break;
    case ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN:
      format = ACCEL_FORMAT_REDUCE3;
      break;
    case ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN:
      format = ACCEL_FORMAT_REDUCE3_ARGMIN;
      break;
    case ACCEL_OPCODE_EXECUTE_BATCH:
      format = ACCEL_FORMAT_EXECUTE_BATCH;
      break;
    case ACCEL_OPCODE_COST_ADD_VECTOR:
      format = command.dst == command.src0 && command.dst_stride == command.src0_stride
                   ? ACCEL_FORMAT_COST_ADD_VECTOR_INPLACE
               : command.dst == command.src1 && command.dst_stride == command.src1_stride
                   ? ACCEL_FORMAT_COST_ADD_VECTOR_INPLACE
                   : ACCEL_FORMAT_COST_ADD_VECTOR_GENERAL;
      break;
    case ACCEL_OPCODE_MINPLUS_PROJECT:
      format = ACCEL_FORMAT_MINPLUS_PROJECT;
      break;
    case ACCEL_OPCODE_MINPLUS_MAP3_PROJECT:
      format = ACCEL_FORMAT_MINPLUS_MAP3_PROJECT;
      break;
    default:
      break;
  }
  // Unsupported-opcode fixtures still need a complete header for ingress tests.
  size_t width = ACCEL_COMMAND_MIN_BYTES;
  if (format != 0)
    CHECK(pcaa_wire_format_size(static_cast<uint8_t>(command.opcode), format, &width) ==
          PCAA_STATUS_OK);
  bytes[0] = static_cast<unsigned char>(command.opcode);
  bytes[1] = format;
  fixture16(bytes, 2, static_cast<uint16_t>(command.flags));
  fixture16(bytes, 4,
            command.opcode == ACCEL_OPCODE_EXECUTE_BATCH ? 0 : static_cast<uint16_t>(command.n));
  fixture16(bytes, 6,
            command.opcode == ACCEL_OPCODE_EXECUTE_BATCH ? 0 : static_cast<uint16_t>(command.m));
  if (command.opcode == ACCEL_OPCODE_EXECUTE_BATCH) {
    fixture64(bytes, 8, command.src0);
    fixture64(bytes, 16, command.dst);
    fixture32(bytes, 24, command.n);
    fixture32(bytes, 28, command.k != 0 ? command.k : command.n * ACCEL_COMMAND_MIN_BYTES);
    return width;
  }
  if (format == ACCEL_FORMAT_COST_ADD_VECTOR_INPLACE) {
    const bool swap = command.dst != command.src0;
    fixture64(bytes, 8, swap ? command.src1 : command.src0);
    fixture64(bytes, 16, swap ? command.src0 : command.src1);
    fixture16(bytes, 24, swap ? command.src1_stride : command.src0_stride);
    fixture16(bytes, 26, swap ? command.src0_stride : command.src1_stride);
    return width;
  }
  fixture64(bytes, 8, command.src0);
  fixture64(bytes, 16, command.src1);
  if (width == ACCEL_COMMAND_MIN_BYTES) {
    fixture64(bytes, 24, command.dst);
    return width;
  }
  fixture64(bytes, 24,
            command.opcode == ACCEL_OPCODE_MINPLUS_MAP3_PROJECT ||
                    command.opcode == ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN ||
                    command.opcode == ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN
                ? command.src2
                : command.dst);
  if (command.opcode == ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN ||
      command.opcode == ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN) {
    fixture64(bytes, 32, command.dst);
  } else if (command.opcode == ACCEL_OPCODE_MINPLUS_MAP3_PROJECT) {
    fixture64(bytes, 32, command.dst);
    fixture16(bytes, 40, command.src0_stride);
    fixture16(bytes, 42, command.src1_stride);
    fixture16(bytes, 44, command.src2_stride);
    fixture16(bytes, 46, command.src2_outer_stride);
    fixture16(bytes, 48, command.dst_stride);
  } else if (command.opcode == ACCEL_OPCODE_MINPLUS_PROJECT) {
    fixture16(bytes, 32, command.src0_stride);
    fixture16(bytes, 34, command.src0_outer_stride);
    fixture16(bytes, 36, command.src1_stride);
    fixture16(bytes, 38, command.dst_stride);
  } else if (command.opcode == ACCEL_OPCODE_COST_ADD_VECTOR) {
    fixture16(bytes, 32, command.src0_stride);
    fixture16(bytes, 34, command.src1_stride);
    fixture16(bytes, 36, command.dst_stride);
  }
  if (command.reserved != 0)
    bytes[width - 1] = static_cast<unsigned char>(command.reserved);
  return width;
}

size_t stage_fixtures(TestMemory &memory, uint64_t address, const CommandFixture *commands,
                      size_t count) {
  size_t cursor = 0;
  for (size_t index = 0; index < count; ++index) {
    unsigned char bytes[ACCEL_COMMAND_MAX_BYTES];
    const size_t width = encode_fixture(commands[index], bytes);
    CHECK(memory.write(address + cursor, bytes, width));
    cursor += width;
  }
  return cursor;
}

void mmio(TestInitiator &initiator, uint64_t address, uint32_t *value, tlm::tlm_command command) {
  tlm::tlm_generic_payload transaction;
  sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
  transaction.set_command(command);
  transaction.set_address(address);
  transaction.set_data_ptr(reinterpret_cast<unsigned char *>(value));
  transaction.set_data_length(sizeof(*value));
  transaction.set_streaming_width(sizeof(*value));
  initiator.socket->b_transport(transaction, delay);
  CHECK(transaction.get_response_status() == tlm::TLM_OK_RESPONSE);
}

uint32_t submit_encoded(TestInitiator &initiator, TestMemory &memory, const void *bytes,
                        size_t width, uint64_t descriptor_address = kDescriptorAddress) {
  CHECK(memory.write(descriptor_address, bytes, width));

  uint32_t register_value = static_cast<uint32_t>(descriptor_address);
  mmio(initiator, ACCEL_MMIO_DESC_ADDR_LO, &register_value, tlm::TLM_WRITE_COMMAND);
  register_value = static_cast<uint32_t>(descriptor_address >> kAddressLowBits);
  mmio(initiator, ACCEL_MMIO_DESC_ADDR_HI, &register_value, tlm::TLM_WRITE_COMMAND);
  register_value = kDoorbellSubmit;
  mmio(initiator, ACCEL_MMIO_DOORBELL, &register_value, tlm::TLM_WRITE_COMMAND);
  register_value = 0;
  mmio(initiator, ACCEL_MMIO_STATUS, &register_value, tlm::TLM_READ_COMMAND);
  return register_value;
}

uint32_t submit(TestInitiator &initiator, TestMemory &memory, const CommandFixture &command,
                uint64_t descriptor_address = kDescriptorAddress) {
  unsigned char bytes[ACCEL_COMMAND_MAX_BYTES];
  const size_t width = encode_fixture(command, bytes);
  return submit_encoded(initiator, memory, bytes, width, descriptor_address);
}

pcaa_status_t make_semantic_command(const CommandFixture &fixture, pcaa_command_t *command) {
  const auto first = pcaa_cost_vector(fixture.src0, fixture.n, fixture.src0_stride);
  const auto second = pcaa_cost_vector(fixture.src1, fixture.n, fixture.src1_stride);
  switch (fixture.opcode) {
    case ACCEL_OPCODE_MAP_ADD_REDUCE_MIN:
    case ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN:
      return pcaa_make_reduce2(pcaa_cost_vector(fixture.src0, fixture.n, 1),
                               pcaa_cost_vector(fixture.src1, fixture.n, 1), fixture.dst,
                               fixture.opcode == ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN, command);
    case ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN:
    case ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN:
      return pcaa_make_reduce3(pcaa_cost_vector(fixture.src0, fixture.n, 1),
                               pcaa_cost_vector(fixture.src1, fixture.n, 1),
                               pcaa_cost_vector(fixture.src2, fixture.n, 1), fixture.dst,
                               fixture.opcode == ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN, command);
    case ACCEL_OPCODE_EXECUTE_BATCH:
      return pcaa_make_ordered_batch(
          fixture.src0, fixture.n, fixture.k != 0 ? fixture.k : fixture.n * ACCEL_COMMAND_MIN_BYTES,
          fixture.dst, command);
    case ACCEL_OPCODE_COST_ADD_VECTOR:
      return pcaa_make_cost_add_vector(
          first, second, pcaa_cost_output(fixture.dst, fixture.n, fixture.dst_stride), command);
    case ACCEL_OPCODE_MINPLUS_PROJECT:
      return pcaa_make_minplus_project(
          pcaa_cost_matrix(fixture.src0, fixture.m, fixture.n, fixture.src0_outer_stride,
                           fixture.src0_stride),
          second, pcaa_cost_output(fixture.dst, fixture.m, fixture.dst_stride), command);
    case ACCEL_OPCODE_MINPLUS_MAP3_PROJECT:
      return pcaa_make_minplus_map3_project(
          first, second,
          pcaa_cost_matrix(fixture.src2, fixture.m, fixture.n, fixture.src2_outer_stride,
                           fixture.src2_stride),
          pcaa_argmin_output(fixture.dst, fixture.m, fixture.dst_stride), command);
    default:
      return PCAA_STATUS_INVALID_COMMAND;
  }
}

void expect_done(TestInitiator &initiator, TestMemory &memory, const pcaa_command_t &command) {
  unsigned char bytes[ACCEL_COMMAND_MAX_BYTES];
  size_t width = 0;
  ASSERT_EQ(pcaa_encode_one(&command, bytes, sizeof(bytes), &width), PCAA_STATUS_OK);
  EXPECT_EQ(submit_encoded(initiator, memory, bytes, width), ACCEL_STATUS_DONE);
}

void expect_done(TestInitiator &initiator, TestMemory &memory, const CommandFixture &command) {
  pcaa_command_t semantic{};
  ASSERT_EQ(make_semantic_command(command, &semantic), PCAA_STATUS_OK);
  expect_done(initiator, memory, semantic);
}

void expect_error(TestInitiator &initiator, TestMemory &memory, const CommandFixture &command) {
  EXPECT_EQ(submit(initiator, memory, command), ACCEL_STATUS_ERROR)
      << "opcode=" << command.opcode << " n=" << command.n << " m=" << command.m
      << " dst=" << command.dst;
}

void test_invalid_mmio(TestInitiator &initiator) {
  uint16_t value = 0;
  tlm::tlm_generic_payload transaction;
  sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
  transaction.set_command(tlm::TLM_READ_COMMAND);
  transaction.set_address(ACCEL_MMIO_STATUS);
  transaction.set_data_ptr(reinterpret_cast<unsigned char *>(&value));
  transaction.set_data_length(sizeof(value));
  transaction.set_streaming_width(sizeof(value));
  initiator.socket->b_transport(transaction, delay);
  CHECK(transaction.get_response_status() == tlm::TLM_BURST_ERROR_RESPONSE);

  uint32_t aligned_value = 0;
  transaction.set_address(ACCEL_MMIO_STATUS + kInvalidMmioOffset);
  transaction.set_data_ptr(reinterpret_cast<unsigned char *>(&aligned_value));
  transaction.set_data_length(sizeof(aligned_value));
  transaction.set_streaming_width(sizeof(aligned_value));
  initiator.socket->b_transport(transaction, delay);
  CHECK(transaction.get_response_status() == tlm::TLM_ADDRESS_ERROR_RESPONSE);
}
}  // namespace

void test_batches(TestInitiator &initiator, TestMemory &memory);
void test_vector_isa(TestInitiator &initiator, TestMemory &memory);
void test_invalid_costs(TestInitiator &initiator, TestMemory &memory);
void test_semantic_differential(TestInitiator &initiator, TestMemory &memory);
void test_vector_add_swap_symmetry(TestInitiator &initiator, TestMemory &memory);

TEST(SystemcAccelerator, ExecutesCommandsAndReportsErrors) {
  CHECK(accel_cost_add(2, 3) == 5);
  CHECK(accel_cost_add(ACCEL_INF, -100) == ACCEL_INF);
  CHECK(accel_cost_add(ACCEL_INF - 1, 2) == ACCEL_INF);
  CHECK(accel_cost_add(INT32_MIN, -1) == INT32_MIN);
  int32_t checked_sum = 0;
  CHECK(accel_cost_add_checked(INT32_MIN, -1, &checked_sum) != 0);
  CHECK(accel_cost_add_checked(ACCEL_INF - 1, 0, &checked_sum) == 0);
  CHECK(checked_sum == ACCEL_INF - 1);
  CHECK(accel_cost_add_checked(ACCEL_INF, -1, &checked_sum) == 0);
  CHECK(checked_sum == ACCEL_INF);
  CHECK(accel_cost_add_checked(ACCEL_INF, ACCEL_INF + 1, &checked_sum) != 0);
  CHECK(accel_cost_add_checked(INT32_MAX, -INT32_MAX, &checked_sum) != 0);

  TestMemory memory(kTestMemorySize);
  AccelTimingConfig timing;
  timing.mode = AccelTimingMode::kL1Sequential;
  timing.lanes = 4;
  timing.descriptor_bytes_per_cycle = ACCEL_COMMAND_SLOT_BYTES;
  timing.memory_read_bytes_per_cycle = 16;
  timing.memory_write_bytes_per_cycle = 8;
  timing.batch_start_cycles = 2;
  timing.primitive_start_cycles = 1;
  timing.map_pipeline_latency = 1;
  timing.add3_map_extra_latency = 1;
  timing.reduction_tree_latency = 1;
  timing.result_latency = 1;
  Accelerator accelerator("accelerator", memory, timing);
  TestInitiator initiator("initiator");
  initiator.socket.bind(accelerator.target_socket);
  TestMemory zero_bandwidth_memory(kTestMemorySize);
  AccelTimingConfig zero_bandwidth_timing = timing;
  zero_bandwidth_timing.mode = AccelTimingMode::kL1Streaming;
  zero_bandwidth_timing.memory_write_bytes_per_cycle = 0;
  Accelerator zero_bandwidth_accelerator("zero_bandwidth_accelerator", zero_bandwidth_memory,
                                         zero_bandwidth_timing);
  TestInitiator zero_bandwidth_initiator("zero_bandwidth_initiator");
  zero_bandwidth_initiator.socket.bind(zero_bandwidth_accelerator.target_socket);
  sc_core::sc_start(sc_core::SC_ZERO_TIME);
  test_invalid_mmio(initiator);

  const std::array<int32_t, 5> first = {ACCEL_INF, -4, 7, -4, 9};
  const std::array<int32_t, 5> second = {1, 2, -10, 2, ACCEL_INF};
  const std::array<int32_t, 5> third = {3, 4, 5, -1, 7};
  CHECK(memory.write(kFirstInputAddress, first.data(), first.size() * sizeof(first.front())));
  CHECK(memory.write(kSecondInputAddress, second.data(), second.size() * sizeof(second.front())));
  CHECK(memory.write(kThirdInputAddress, third.data(), third.size() * sizeof(third.front())));

  pcaa_command_t initial{};
  ASSERT_EQ(
      pcaa_make_reduce2(pcaa_cost_vector(kFirstInputAddress, 1, 1),
                        pcaa_cost_vector(kSecondInputAddress, 1, 1), kResultAddress, 0, &initial),
      PCAA_STATUS_OK);
  expect_done(initiator, memory, initial);
  int32_t result = 0;
  CHECK(memory.read(kResultAddress, &result, sizeof(result)) && result == ACCEL_INF);
  CommandFixture command{ACCEL_OPCODE_MAP_ADD_REDUCE_MIN,
                         0,
                         1,
                         0,
                         0,
                         0,
                         kFirstInputAddress,
                         kSecondInputAddress,
                         0,
                         kResultAddress};
  command.flags = 1;
  command.m = 1;
  command.k = 1;
  command.reserved = 1;
  command.src2 = kThirdInputAddress;
  command.src0_stride = 17;
  expect_error(initiator, memory, command);  // Compact encoding requires zero flags and m.
  command = {ACCEL_OPCODE_MAP_ADD_REDUCE_MIN,
             0,
             1,
             0,
             0,
             0,
             kFirstInputAddress,
             kSecondInputAddress,
             0,
             kResultAddress};

  command.opcode = ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN;
  command.n = 5;
  command.src2 = kThirdInputAddress;
  expect_done(initiator, memory, command);
  CHECK(memory.read(kResultAddress, &result, sizeof(result)) && result == -3);

  command.opcode = ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN;
  expect_done(initiator, memory, command);
  accel_min_argmin_result_t argmin_result{};
  CHECK(memory.read(kResultAddress, &argmin_result, sizeof(argmin_result)));
  CHECK(argmin_result.value == -3 && argmin_result.index == 3);

  command.opcode = ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN;
  command.src2 = 0;
  expect_done(initiator, memory, command);
  CHECK(memory.read(kResultAddress, &argmin_result, sizeof(argmin_result)));
  CHECK(argmin_result.value == -3 && argmin_result.index == 2);

  const std::array<int32_t, 3> tie_a = {-4, -4, 9};
  const std::array<int32_t, 3> tie_b = {0, 0, 0};
  CHECK(memory.write(kTieFirstInputAddress, tie_a.data(), tie_a.size() * sizeof(tie_a.front())));
  CHECK(memory.write(kTieSecondInputAddress, tie_b.data(), tie_b.size() * sizeof(tie_b.front())));
  command = {ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN,
             0,
             3,
             0,
             0,
             0,
             kTieFirstInputAddress,
             kTieSecondInputAddress,
             0,
             kTieResultAddress};
  expect_done(initiator, memory, command);
  CHECK(memory.read(kTieResultAddress, &argmin_result, sizeof(argmin_result)));
  CHECK(argmin_result.value == -4 && argmin_result.index == 0);

  // Without a reported underflow these two different exact sums both clamp
  // to INT32_MIN and falsely tie, making first-index argmin select index 0
  // even though index 1 is strictly smaller.
  const std::array<int32_t, 2> underflow_a = {INT32_MIN, INT32_MIN + 10};
  const std::array<int32_t, 2> underflow_b = {-1, -20};
  CHECK(memory.write(kTieFirstInputAddress, underflow_a.data(), sizeof(underflow_a)));
  CHECK(memory.write(kTieSecondInputAddress, underflow_b.data(), sizeof(underflow_b)));
  command.n = 2;
  expect_error(initiator, memory, command);

  std::vector<int32_t> large_a(kLargeVectorLength, 10);
  std::vector<int32_t> large_b(kLargeVectorLength, 1);
  large_a.back() = -100;
  CHECK(memory.write(kLargeFirstInputAddress, large_a.data(), large_a.size() * sizeof(int32_t)));
  CHECK(memory.write(kLargeSecondInputAddress, large_b.data(), large_b.size() * sizeof(int32_t)));
  command = {ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN,
             0,
             static_cast<uint32_t>(kLargeVectorLength),
             0,
             0,
             0,
             kLargeFirstInputAddress,
             kLargeSecondInputAddress,
             0,
             kLargeResultAddress};
  expect_done(initiator, memory, command);
  CHECK(memory.read(kLargeResultAddress, &argmin_result, sizeof(argmin_result)));
  CHECK(argmin_result.value == -99 && argmin_result.index == 255);

  command.opcode = kUnsupportedOpcode;
  expect_error(initiator, memory, command);
  command.opcode = ACCEL_OPCODE_MAP_ADD_REDUCE_MIN;
  command.n = 0;
  expect_error(initiator, memory, command);
  command.opcode = ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN;
  command.n = 1;
  command.src2 = 0;
  expect_error(initiator, memory, command);
  command.opcode = ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN;
  expect_error(initiator, memory, command);

  command = {ACCEL_OPCODE_MAP_ADD_REDUCE_MIN,
             0,
             1,
             0,
             0,
             0,
             kFirstInputAddress,
             kSecondInputAddress,
             0,
             kResultAddress};
  unsigned char truncated_wire[ACCEL_COMMAND_MAX_BYTES];
  const size_t truncated_width = encode_fixture(command, truncated_wire);
  CHECK(!memory.write(kTruncatedDescriptorAddress, truncated_wire, truncated_width));
  uint32_t register_value = kTruncatedDescriptorAddress;
  mmio(initiator, ACCEL_MMIO_DESC_ADDR_LO, &register_value, tlm::TLM_WRITE_COMMAND);
  register_value = 0;
  mmio(initiator, ACCEL_MMIO_DESC_ADDR_HI, &register_value, tlm::TLM_WRITE_COMMAND);
  register_value = kDoorbellSubmit;
  mmio(initiator, ACCEL_MMIO_DOORBELL, &register_value, tlm::TLM_WRITE_COMMAND);
  register_value = 0;
  mmio(initiator, ACCEL_MMIO_STATUS, &register_value, tlm::TLM_READ_COMMAND);
  CHECK(register_value == ACCEL_STATUS_ERROR);

  command.dst = kInvalidElementAddress;
  expect_error(initiator, memory, command);
  command.dst = kResultAddress;
  command.src0 = kInvalidElementAddress;
  expect_error(initiator, memory, command);
  command.src0 = kFirstInputAddress;
  expect_done(initiator, memory, command);
  test_batches(initiator, memory);
  test_vector_isa(initiator, memory);
  test_invalid_costs(initiator, memory);
  test_semantic_differential(initiator, memory);
  test_vector_add_swap_symmetry(initiator, memory);
  EXPECT_GT(accelerator.timing_statistics().primitive_count, 0);
  EXPECT_GE(accelerator.timing_statistics().batch_count, 7);
  EXPECT_GT(accelerator.timing_statistics().descriptor_cycles, 0);
  EXPECT_GT(accelerator.timing_statistics().total_service_cycles, 0);
  const int32_t zero_bandwidth_first = 1;
  const int32_t zero_bandwidth_second = 2;
  const CommandFixture child = {ACCEL_OPCODE_MAP_ADD_REDUCE_MIN,
                                0,
                                1,
                                0,
                                0,
                                0,
                                kFirstInputAddress,
                                kSecondInputAddress,
                                0,
                                kResultAddress};
  const CommandFixture batch = {
      ACCEL_OPCODE_EXECUTE_BATCH, 0, 1, 0, 0, 0, kBatchDescriptorAddress, 0, 0,
      kBatchResultAddress};
  CHECK(zero_bandwidth_memory.write(kFirstInputAddress, &zero_bandwidth_first,
                                    sizeof(zero_bandwidth_first)));
  CHECK(zero_bandwidth_memory.write(kSecondInputAddress, &zero_bandwidth_second,
                                    sizeof(zero_bandwidth_second)));
  stage_fixtures(zero_bandwidth_memory, kBatchDescriptorAddress, &child, 1);
  expect_done(zero_bandwidth_initiator, zero_bandwidth_memory, batch);
  EXPECT_EQ(zero_bandwidth_accelerator.timing_statistics().total_service_cycles, 0);
}

TEST(TimingModel, CalculatesSequentialAndStreamingCommandCycles) {
  AccelTimingConfig config;
  config.mode = AccelTimingMode::kL1Sequential;
  config.lanes = 4;
  config.descriptor_bytes_per_cycle = ACCEL_COMMAND_SLOT_BYTES;
  config.memory_read_bytes_per_cycle = 16;
  config.memory_write_bytes_per_cycle = 8;
  config.primitive_start_cycles = 1;
  config.map_pipeline_latency = 2;
  config.add3_map_extra_latency = 5;
  config.reduction_tree_latency = 3;
  config.result_latency = 4;

  pcaa_command_t command{};
  ASSERT_EQ(
      pcaa_make_reduce2(pcaa_cost_vector(kFirstInputAddress, 3, 1),
                        pcaa_cost_vector(kSecondInputAddress, 3, 1), kResultAddress, 0, &command),
      PCAA_STATUS_OK);
  AccelCommandTiming timing = accel_estimate_command_cycles(command, config);
  EXPECT_EQ(timing.descriptor_cycles, 2);
  EXPECT_EQ(timing.operand_read_cycles, 2);
  EXPECT_EQ(timing.compute_cycles, 11);
  EXPECT_EQ(timing.result_write_cycles, 1);
  EXPECT_EQ(timing.total_cycles, 16);

  command.operation.reduce2.first.length = command.operation.reduce2.second.length = 4;
  EXPECT_EQ(accel_estimate_command_cycles(command, config).total_cycles, 16);
  command.operation.reduce2.first.length = command.operation.reduce2.second.length = 5;
  EXPECT_EQ(accel_estimate_command_cycles(command, config).total_cycles, 18);

  ASSERT_EQ(
      pcaa_make_reduce3(pcaa_cost_vector(kFirstInputAddress, 4, 1),
                        pcaa_cost_vector(kSecondInputAddress, 4, 1),
                        pcaa_cost_vector(kThirdInputAddress, 4, 1), kResultAddress, 1, &command),
      PCAA_STATUS_OK);
  timing = accel_estimate_command_cycles(command, config);
  EXPECT_EQ(timing.descriptor_cycles, 3);
  EXPECT_EQ(timing.operand_read_cycles, 3);
  EXPECT_EQ(timing.compute_cycles, 16);
  EXPECT_EQ(timing.total_cycles, 23);

  config.mode = AccelTimingMode::kL1Streaming;
  EXPECT_EQ(accel_estimate_command_cycles(command, config).total_cycles, 20);

  ASSERT_EQ(pcaa_make_cost_add_vector(pcaa_cost_vector(kFirstInputAddress, 5, 1),
                                      pcaa_cost_vector(kSecondInputAddress, 5, 1),
                                      pcaa_cost_output(kResultAddress, 5, 1), &command),
            PCAA_STATUS_OK);
  timing = accel_estimate_command_cycles(command, config);
  EXPECT_EQ(timing.descriptor_cycles, 3);
  EXPECT_EQ(timing.operand_read_cycles, 3);
  EXPECT_EQ(timing.result_write_cycles, 3);
  ASSERT_EQ(pcaa_make_minplus_project(pcaa_cost_matrix(kFirstInputAddress, 2, 3, 3, 1),
                                      pcaa_cost_vector(kSecondInputAddress, 3, 1),
                                      pcaa_cost_output(kResultAddress, 2, 1), &command),
            PCAA_STATUS_OK);
  timing = accel_estimate_command_cycles(command, config);
  EXPECT_EQ(timing.operand_read_cycles, 3);
  EXPECT_EQ(timing.result_write_cycles, 1);
  ASSERT_EQ(pcaa_make_minplus_map3_project(pcaa_cost_vector(kFirstInputAddress, 3, 1),
                                           pcaa_cost_vector(kSecondInputAddress, 3, 1),
                                           pcaa_cost_matrix(kThirdInputAddress, 2, 3, 3, 1),
                                           pcaa_argmin_output(kResultAddress, 2, 1), &command),
            PCAA_STATUS_OK);
  timing = accel_estimate_command_cycles(command, config);
  EXPECT_EQ(timing.descriptor_cycles, 4);
  EXPECT_EQ(timing.operand_read_cycles, 3);
  EXPECT_EQ(timing.result_write_cycles, 2);
}

void test_batches(TestInitiator &initiator, TestMemory &memory) {
  const std::array<int32_t, 3> first = {4, -3, 8};
  const std::array<int32_t, 3> second = {-1, 2, -9};
  const std::array<int32_t, 3> third = {5, 1, 2};
  CHECK(memory.write(kFirstInputAddress, first.data(), first.size() * sizeof(first.front())));
  CHECK(memory.write(kSecondInputAddress, second.data(), second.size() * sizeof(second.front())));
  CHECK(memory.write(kThirdInputAddress, third.data(), third.size() * sizeof(third.front())));

  int32_t minimum = 0;
  accel_min_argmin_result_t argmin{};
  std::array<CommandFixture, 2> children = {{
      {ACCEL_OPCODE_MAP_ADD_REDUCE_MIN, 0, 3, 0, 0, 0, kFirstInputAddress, kSecondInputAddress, 0,
       kBatchSecondResultAddress},
      {ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN, 0, 3, 0, 0, 0, kFirstInputAddress,
       kSecondInputAddress, kThirdInputAddress, kResultAddress},
  }};
  const size_t first_child_bytes =
      stage_fixtures(memory, kBatchDescriptorAddress, children.data(), children.size());
  CommandFixture batch = {ACCEL_OPCODE_EXECUTE_BATCH,
                          0,
                          static_cast<uint32_t>(children.size()),
                          0,
                          static_cast<uint32_t>(first_child_bytes),
                          0,
                          kBatchDescriptorAddress,
                          0,
                          0,
                          kBatchResultAddress};
  expect_done(initiator, memory, batch);
  accel_batch_result_t batch_result{};
  CHECK(memory.read(kBatchResultAddress, &batch_result, sizeof(batch_result)));
  CHECK(batch_result.completed == children.size() && batch_result.failed_index == UINT32_MAX);
  CHECK(memory.read(kBatchSecondResultAddress, &minimum, sizeof(minimum)) && minimum == -1);
  CHECK(memory.read(kResultAddress, &argmin, sizeof(argmin)) && argmin.value == 0 &&
        argmin.index == 1);

  children[1].opcode = kUnsupportedOpcode;
  minimum = 0;
  argmin = {123, 123};
  CHECK(memory.write(kResultAddress, &argmin, sizeof(argmin)));
  stage_fixtures(memory, kBatchDescriptorAddress, children.data(), children.size());
  expect_error(initiator, memory, batch);
  CHECK(memory.read(kBatchResultAddress, &batch_result, sizeof(batch_result)));
  CHECK(batch_result.completed == 1 && batch_result.failed_index == 1);
  CHECK(memory.read(kBatchSecondResultAddress, &minimum, sizeof(minimum)) && minimum == -1);
  CHECK(memory.read(kResultAddress, &argmin, sizeof(argmin)) && argmin.value == 123);

  children[1] = {ACCEL_OPCODE_MAP_ADD_REDUCE_MIN,
                 0,
                 3,
                 0,
                 0,
                 0,
                 kInvalidElementAddress,
                 kSecondInputAddress,
                 0,
                 kResultAddress};
  stage_fixtures(memory, kBatchDescriptorAddress, children.data(), children.size());
  batch.k = 64;
  expect_error(initiator, memory, batch);
  CHECK(memory.read(kBatchResultAddress, &batch_result, sizeof(batch_result)));
  CHECK(batch_result.completed == 1 && batch_result.failed_index == 1);

  children[0].dst = kInvalidElementAddress;
  children[1].opcode = ACCEL_OPCODE_MAP_ADD_REDUCE_MIN;
  stage_fixtures(memory, kBatchDescriptorAddress, children.data(), children.size());
  expect_error(initiator, memory, batch);
  CHECK(memory.read(kBatchResultAddress, &batch_result, sizeof(batch_result)));
  CHECK(batch_result.completed == 0 && batch_result.failed_index == 0);

  children[0] = batch;
  stage_fixtures(memory, kBatchDescriptorAddress, children.data(), children.size());
  expect_error(initiator, memory, batch);
  CHECK(memory.read(kBatchResultAddress, &batch_result, sizeof(batch_result)));
  CHECK(batch_result.completed == 0 && batch_result.failed_index == 0);

  CommandFixture invalid_batch = batch;
  invalid_batch.n = 0;
  expect_error(initiator, memory, invalid_batch);
  invalid_batch.n = 1;
  invalid_batch.src0 = kTruncatedDescriptorAddress;
  expect_error(initiator, memory, invalid_batch);
  CHECK(memory.read(kBatchResultAddress, &batch_result, sizeof(batch_result)));
  CHECK(batch_result.completed == 0 && batch_result.failed_index == 0);

  children[0] = {
      ACCEL_OPCODE_MAP_ADD_REDUCE_MIN, 0, 3, 0, 0, 0, kFirstInputAddress, kSecondInputAddress, 0,
      kBatchSecondResultAddress};
  stage_fixtures(memory, kBatchDescriptorAddress, children.data(), 1);
  invalid_batch = batch;
  invalid_batch.n = 1;
  invalid_batch.k = ACCEL_COMMAND_MIN_BYTES;
  expect_done(initiator, memory, invalid_batch);

  // A child may not rewrite a later child or the top-level descriptor.
  children[0].dst = kBatchDescriptorAddress + ACCEL_COMMAND_MIN_BYTES;
  children[1] = children[0];
  children[1].dst = kResultAddress;
  stage_fixtures(memory, kBatchDescriptorAddress, children.data(), children.size());
  expect_error(initiator, memory, batch);
  CHECK(memory.read(kBatchResultAddress, &batch_result, sizeof(batch_result)));
  EXPECT_EQ(batch_result.completed, 0u);
  EXPECT_EQ(batch_result.failed_index, 0u);
  children[0].dst = kDescriptorAddress;
  stage_fixtures(memory, kBatchDescriptorAddress, children.data(), children.size());
  expect_error(initiator, memory, batch);
  CHECK(memory.read(kBatchResultAddress, &batch_result, sizeof(batch_result)));
  EXPECT_EQ(batch_result.failed_index, 0u);

  // Bounds are checked against child_bytes, not a fixed descriptor stride.
  children[0].dst = kBatchSecondResultAddress;
  stage_fixtures(memory, kBatchDescriptorAddress, children.data(), children.size());
  invalid_batch = batch;
  invalid_batch.k = ACCEL_COMMAND_SLOT_BYTES;
  expect_error(initiator, memory, invalid_batch);  // First child extends past the stream.
  CHECK(memory.read(kBatchResultAddress, &batch_result, sizeof(batch_result)));
  EXPECT_EQ(batch_result.failed_index, 0u);
  invalid_batch = batch;
  invalid_batch.n = 3;
  expect_error(initiator, memory, invalid_batch);  // Too few children in child_bytes.
  CHECK(memory.read(kBatchResultAddress, &batch_result, sizeof(batch_result)));
  EXPECT_EQ(batch_result.failed_index, 2u);
  invalid_batch = batch;
  invalid_batch.k += ACCEL_COMMAND_MIN_BYTES;
  expect_error(initiator, memory, invalid_batch);  // Trailing bytes after two children.
  CHECK(memory.read(kBatchResultAddress, &batch_result, sizeof(batch_result)));
  EXPECT_EQ(batch_result.failed_index, 2u);

  // The batch's own result may not corrupt its descriptor array.
  invalid_batch = batch;
  invalid_batch.dst = kBatchDescriptorAddress;
  expect_error(initiator, memory, invalid_batch);
  invalid_batch.dst = kDescriptorAddress;
  expect_error(initiator, memory, invalid_batch);
}

void test_vector_isa(TestInitiator &initiator, TestMemory &memory) {
  const std::array<int32_t, 5> first = {3, ACCEL_INF, -2, 5, ACCEL_INF - 1};
  const std::array<int32_t, 5> second = {4, -9, 1, -8, 2};
  CHECK(memory.write(kFirstInputAddress, first.data(), sizeof(first)));
  CHECK(memory.write(kSecondInputAddress, second.data(), sizeof(second)));
  CommandFixture add{};
  add.opcode = ACCEL_OPCODE_COST_ADD_VECTOR;
  add.n = 5;
  add.src0 = kFirstInputAddress;
  add.src1 = kSecondInputAddress;
  add.dst = kResultAddress;
  add.src0_stride = add.src1_stride = add.dst_stride = 1;
  expect_done(initiator, memory, add);
  std::array<int32_t, 5> values{};
  CHECK(memory.read(kResultAddress, values.data(), sizeof(values)));
  EXPECT_EQ(values, (std::array<int32_t, 5>{7, ACCEL_INF, -1, -3, ACCEL_INF}));
  add.n = 1;
  add.dst = add.src1;
  expect_done(initiator, memory, add);
  int32_t one = 0;
  CHECK(memory.read(kSecondInputAddress, &one, sizeof(one)));
  EXPECT_EQ(one, 7);
  add.n = 5;
  add.dst = kResultAddress;
  add.src2 = UINT64_MAX;  // Unused addresses and strides are ignored.
  add.src2_stride = 0;
  add.src0_outer_stride = 19;
  add.src2_outer_stride = 23;
  expect_done(initiator, memory, add);
  add.reserved = 1;
  expect_error(initiator, memory, add);
  add.reserved = 0;
  add.src0_stride = 0;
  expect_error(initiator, memory, add);
  add.src0_stride = 1;
  add.dst = kFirstInputAddress + sizeof(int32_t);
  expect_error(initiator, memory, add);
  add.dst = kResultAddress;
  add.m = 1;
  expect_error(initiator, memory, add);
  add.m = 0;
  const std::array<int32_t, 6> padded_first = {1, 99, 2, 99, 3, 99};
  const std::array<int32_t, 6> padded_second = {4, 88, 5, 88, 6, 88};
  CHECK(memory.write(kFirstInputAddress, padded_first.data(), sizeof(padded_first)));
  CHECK(memory.write(kSecondInputAddress, padded_second.data(), sizeof(padded_second)));
  add.n = 3;
  add.src0_stride = add.src1_stride = add.dst_stride = 2;
  add.dst = add.src1;
  expect_done(initiator, memory, add);
  std::array<int32_t, 6> aliased{};
  CHECK(memory.read(kSecondInputAddress, aliased.data(), sizeof(aliased)));
  EXPECT_EQ(aliased, (std::array<int32_t, 6>{5, 88, 7, 88, 9, 88}));
  add.dst = add.src1 + sizeof(int32_t);
  expect_error(initiator, memory, add);
  add.dst = UINT64_MAX - sizeof(int32_t);
  expect_error(initiator, memory, add);
  add.src0_stride = add.src1_stride = add.dst_stride = 1;
  add.dst = kResultAddress;
  const std::array<int32_t, 2> underflow = {1, INT32_MIN};
  const std::array<int32_t, 2> minus_one = {1, -1};
  CHECK(memory.write(kFirstInputAddress, underflow.data(), sizeof(underflow)));
  CHECK(memory.write(kSecondInputAddress, minus_one.data(), sizeof(minus_one)));
  add.n = 2;
  expect_error(initiator, memory, add);
  std::array<int32_t, 2> unchanged{};

  // Three padded rows, two reduced columns: row stride 4, inner stride 1.
  const std::array<int32_t, 12> matrix = {4, 1, 99, 99, -2, 6, 99, 99, ACCEL_INF, 0, 99, 99};
  const std::array<int32_t, 2> unary = {-1, 2};
  CHECK(memory.write(kFirstInputAddress, matrix.data(), sizeof(matrix)));
  CHECK(memory.write(kSecondInputAddress, unary.data(), sizeof(unary)));
  CommandFixture project{};
  project.opcode = ACCEL_OPCODE_MINPLUS_PROJECT;
  project.n = 2;
  project.m = 3;
  project.src0 = kFirstInputAddress;
  project.src1 = kSecondInputAddress;
  project.dst = kResultAddress;
  project.src0_stride = project.src1_stride = project.dst_stride = 1;
  project.src0_outer_stride = 4;
  expect_done(initiator, memory, project);
  std::array<int32_t, 3> projection{};
  CHECK(memory.read(kResultAddress, projection.data(), sizeof(projection)));
  EXPECT_EQ(projection, (std::array<int32_t, 3>{3, -3, 2}));
  project.src0_stride = 4;
  project.src0_outer_stride = 1;
  project.m = 2;
  expect_done(initiator, memory, project);
  CHECK(memory.read(kResultAddress, projection.data(), 2 * sizeof(int32_t)));
  EXPECT_EQ(projection[0], 0);
  EXPECT_EQ(projection[1], 0);
  project.src0_stride = 1;
  project.src0_outer_stride = 4;
  project.m = 3;
  project.dst = project.src0;
  expect_error(initiator, memory, project);
  project.dst = kResultAddress;
  project.src1 = kInvalidElementAddress;
  expect_error(initiator, memory, project);
  project.src1 = kSecondInputAddress;
  project.src2 = UINT64_MAX;
  project.src2_stride = 0;
  project.src2_outer_stride = 13;
  expect_done(initiator, memory, project);
  project.reserved = 1;
  expect_error(initiator, memory, project);
  project.reserved = 0;
  const std::array<int32_t, 2> underflow_unary = {INT32_MIN, 0};
  CHECK(memory.write(kSecondInputAddress, underflow_unary.data(), sizeof(underflow_unary)));
  expect_error(initiator, memory, project);

  const std::array<int32_t, 2> zero = {0, 0};
  const std::array<int32_t, 2> fixed = {1, 1};
  const std::array<int32_t, 12> varying = {2, 2, 99, 99, ACCEL_INF, -3, 99, 99, -4, 0, 99, 99};
  CHECK(memory.write(kFirstInputAddress, varying.data(), sizeof(varying)));
  CHECK(memory.write(kSecondInputAddress, zero.data(), sizeof(zero)));
  CHECK(memory.write(kThirdInputAddress, fixed.data(), sizeof(fixed)));
  CommandFixture map3{};
  map3.opcode = ACCEL_OPCODE_MINPLUS_MAP3_PROJECT;
  map3.n = 2;
  map3.m = 3;
  map3.src0 = kSecondInputAddress;
  map3.src1 = kThirdInputAddress;
  map3.src2 = kFirstInputAddress;
  map3.dst = kResultAddress;
  map3.src0_stride = map3.src1_stride = map3.src2_stride = map3.dst_stride = 1;
  map3.src0_outer_stride = 7;  // Unused by opcode 8.
  map3.src2_outer_stride = 4;
  expect_done(initiator, memory, map3);
  map3.flags = 1;
  expect_error(initiator, memory, map3);
  map3.flags = 0;
  std::array<accel_min_argmin_result_t, 3> results{};
  CHECK(memory.read(kResultAddress, results.data(), sizeof(results)));
  EXPECT_EQ(results[0].value, 3);
  EXPECT_EQ(results[0].index, 0u);
  EXPECT_EQ(results[1].value, -2);
  EXPECT_EQ(results[1].index, 1u);
  EXPECT_EQ(results[2].value, -3);
  EXPECT_EQ(results[2].index, 0u);
  CommandFixture scalar{};
  scalar.opcode = ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN;
  scalar.n = 2;
  scalar.src0 = kSecondInputAddress;
  scalar.src1 = kThirdInputAddress;
  scalar.dst = kBatchSecondResultAddress;
  for (size_t row = 0; row < results.size(); ++row) {
    scalar.src2 = kFirstInputAddress + row * 4 * sizeof(int32_t);
    expect_done(initiator, memory, scalar);
    accel_min_argmin_result_t old{};
    CHECK(memory.read(kBatchSecondResultAddress, &old, sizeof(old)));
    EXPECT_EQ(old.value, results[row].value);
    EXPECT_EQ(old.index, results[row].index);
  }
  map3.src2_outer_stride = 0;
  expect_error(initiator, memory, map3);
  map3.src2_outer_stride = 4;
  map3.dst = kFirstInputAddress;
  expect_error(initiator, memory, map3);
  map3.dst = kResultAddress;
  const std::array<int32_t, 2> negative = {0, INT32_MIN};
  CHECK(memory.write(kSecondInputAddress, negative.data(), sizeof(negative)));
  expect_error(initiator, memory, map3);

  // Ordered producer-consumer chain and fail-stop visibility.
  CHECK(memory.write(kSecondInputAddress, zero.data(), sizeof(zero)));
  project.m = 2;
  project.src0 = kFirstInputAddress;
  project.src1 = kSecondInputAddress;
  project.dst = kBatchSecondResultAddress;
  project.src0_outer_stride = 4;
  add.n = 2;
  add.src0 = kBatchSecondResultAddress;
  add.src1 = kThirdInputAddress;
  add.dst = kResultAddress;
  std::array<CommandFixture, 2> children = {project, add};
  const size_t child_bytes =
      stage_fixtures(memory, kBatchDescriptorAddress, children.data(), children.size());
  const CommandFixture batch = {ACCEL_OPCODE_EXECUTE_BATCH,
                                0,
                                2,
                                0,
                                static_cast<uint32_t>(child_bytes),
                                0,
                                kBatchDescriptorAddress,
                                0,
                                0,
                                kBatchResultAddress};
  expect_done(initiator, memory, batch);
  CHECK(memory.read(kResultAddress, unchanged.data(), sizeof(unchanged)));
  EXPECT_EQ(unchanged, (std::array<int32_t, 2>{3, -2}));
  children[1].src0_stride = 0;
  stage_fixtures(memory, kBatchDescriptorAddress, children.data(), children.size());
  expect_error(initiator, memory, batch);
  accel_batch_result_t batch_result{};
  CHECK(memory.read(kBatchResultAddress, &batch_result, sizeof(batch_result)));
  EXPECT_EQ(batch_result.completed, 1u);
  EXPECT_EQ(batch_result.failed_index, 1u);
}

void test_invalid_costs(TestInitiator &initiator, TestMemory &memory) {
  const int32_t invalid = ACCEL_INF + 1;
  const int32_t infinity = ACCEL_INF;
  const int32_t negative = -100;
  CHECK(memory.write(kFirstInputAddress, &infinity, sizeof(infinity)));
  CHECK(memory.write(kSecondInputAddress, &invalid, sizeof(invalid)));
  CHECK(memory.write(kThirdInputAddress, &negative, sizeof(negative)));

  CommandFixture command{};
  command.n = 1;
  command.src0 = kFirstInputAddress;
  command.src1 = kSecondInputAddress;
  command.src2 = kThirdInputAddress;
  command.dst = kResultAddress;
  for (uint32_t opcode :
       {ACCEL_OPCODE_MAP_ADD_REDUCE_MIN, ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN,
        ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN, ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN}) {
    command.opcode = opcode;
    expect_error(initiator, memory, command);
  }
  command.opcode = ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN;
  CHECK(memory.write(kSecondInputAddress, &negative, sizeof(negative)));
  CHECK(memory.write(kThirdInputAddress, &invalid, sizeof(invalid)));
  expect_error(initiator, memory, command);  // Third operand is checked after INF.
  const int32_t maximum = INT32_MAX;
  CHECK(memory.write(kSecondInputAddress, &maximum, sizeof(maximum)));
  command.opcode = ACCEL_OPCODE_MAP_ADD_REDUCE_MIN;
  expect_error(initiator, memory, command);

  command.src0_stride = command.src1_stride = command.src2_stride = command.dst_stride = 1;
  command.src0_outer_stride = command.src2_outer_stride = 1;
  command.m = 1;
  command.opcode = ACCEL_OPCODE_MINPLUS_PROJECT;
  CHECK(memory.write(kSecondInputAddress, &invalid, sizeof(invalid)));
  expect_error(initiator, memory, command);
  command.opcode = ACCEL_OPCODE_MINPLUS_MAP3_PROJECT;
  expect_error(initiator, memory, command);
  command.opcode = ACCEL_OPCODE_COST_ADD_VECTOR;
  command.m = 0;
  expect_error(initiator, memory, command);
  CHECK(memory.write(kSecondInputAddress, &negative, sizeof(negative)));
  CHECK(memory.write(kThirdInputAddress, &invalid, sizeof(invalid)));
  command.opcode = ACCEL_OPCODE_MINPLUS_MAP3_PROJECT;
  command.m = 1;
  expect_error(initiator, memory, command);
}

namespace {
int32_t reference_add(int32_t first, int32_t second) {
  if (first == ACCEL_INF || second == ACCEL_INF)
    return ACCEL_INF;
  const int64_t sum = int64_t(first) + second;
  return sum >= ACCEL_INF ? ACCEL_INF : static_cast<int32_t>(sum);
}

int32_t reference_cost(TestMemory &memory, pcaa_cost_vector_view_t view, uint32_t index) {
  int32_t value = 0;
  EXPECT_TRUE(memory.read(view.base + uint64_t(index) * view.stride * sizeof(value), &value,
                          sizeof(value)));
  return value;
}

int32_t reference_cost(TestMemory &memory, pcaa_cost_matrix_view_t view, uint32_t row,
                       uint32_t column) {
  int32_t value = 0;
  const uint64_t address =
      view.base +
      (uint64_t(row) * view.row_stride + uint64_t(column) * view.column_stride) * sizeof(value);
  EXPECT_TRUE(memory.read(address, &value, sizeof(value)));
  return value;
}

void reference_execute(TestMemory &memory, const pcaa_command_t &command) {
  const bool reduce2 =
      command.kind == PCAA_MAP_ADD_REDUCE_MIN || command.kind == PCAA_MAP_ADD_REDUCE_MIN_ARGMIN;
  const bool reduce3 =
      command.kind == PCAA_MAP_ADD3_REDUCE_MIN || command.kind == PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN;
  const bool map3 = command.kind == PCAA_MINPLUS_MAP3_PROJECT;
  const bool add = command.kind == PCAA_COST_ADD_VECTOR;
  const bool project = command.kind == PCAA_MINPLUS_PROJECT;
  const uint32_t outputs = add       ? command.operation.vector_add.result.length
                           : project ? command.operation.project.result.length
                           : map3    ? command.operation.map3_project.result.length
                                     : 1;
  for (uint32_t output = 0; output < outputs; ++output) {
    const uint32_t count = reduce2   ? command.operation.reduce2.first.length
                           : reduce3 ? command.operation.reduce3.first.length
                           : add     ? 1
                           : project ? command.operation.project.vector.length
                                     : command.operation.map3_project.first.length;
    int32_t minimum = ACCEL_INF;
    uint32_t argmin = 0;
    for (uint32_t inner = 0; inner < count; ++inner) {
      const uint32_t vector_index = add ? output : inner;
      int32_t first = 0;
      int32_t second = 0;
      int32_t third = 0;
      if (reduce2) {
        first = reference_cost(memory, command.operation.reduce2.first, inner);
        second = reference_cost(memory, command.operation.reduce2.second, inner);
      } else if (reduce3) {
        first = reference_cost(memory, command.operation.reduce3.first, inner);
        second = reference_cost(memory, command.operation.reduce3.second, inner);
        third = reference_cost(memory, command.operation.reduce3.third, inner);
      } else if (add) {
        first = reference_cost(memory, command.operation.vector_add.first, vector_index);
        second = reference_cost(memory, command.operation.vector_add.second, vector_index);
      } else if (project) {
        first = reference_cost(memory, command.operation.project.matrix, output, inner);
        second = reference_cost(memory, command.operation.project.vector, inner);
      } else {
        first = reference_cost(memory, command.operation.map3_project.first, inner);
        second = reference_cost(memory, command.operation.map3_project.second, inner);
        third = reference_cost(memory, command.operation.map3_project.third, output, inner);
      }
      int32_t value = reference_add(first, second);
      if (reduce3 || map3)
        value = reference_add(value, third);
      if (inner == 0 || value < minimum) {
        minimum = value;
        argmin = inner;
      }
    }
    const bool argmin_result = map3 || command.kind == PCAA_MAP_ADD_REDUCE_MIN_ARGMIN ||
                               command.kind == PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN;
    const pcaa_output_view_t destination =
        add       ? command.operation.vector_add.result
        : project ? command.operation.project.result
        : map3    ? command.operation.map3_project.result
                  : pcaa_cost_output(
                     reduce2 ? command.operation.reduce2.result : command.operation.reduce3.result,
                     1, 1);
    const uint64_t address =
        destination.base +
        uint64_t(output) * destination.stride *
            (argmin_result ? sizeof(accel_min_argmin_result_t) : sizeof(int32_t));
    if (argmin_result) {
      const accel_min_argmin_result_t result{minimum, argmin};
      EXPECT_TRUE(memory.write(address, &result, sizeof(result)));
    } else {
      EXPECT_TRUE(memory.write(address, &minimum, sizeof(minimum)));
    }
  }
}
}  // namespace

void test_semantic_differential(TestInitiator &initiator, TestMemory &memory) {
  const std::array<int32_t, 3> first = {3, ACCEL_INF, -4};
  const std::array<int32_t, 3> second = {-6, 2, 1};  // Two-way argmin tie at -3.
  const std::array<int32_t, 8> matrix = {3, 1, ACCEL_INF, 99, -2, 4, 5, 99};
  CHECK(memory.write(kFirstInputAddress, first.data(), sizeof(first)));
  CHECK(memory.write(kSecondInputAddress, second.data(), sizeof(second)));
  CHECK(memory.write(kThirdInputAddress, matrix.data(), sizeof(matrix)));
  const auto a = pcaa_cost_vector(kFirstInputAddress, 3, 1);
  const auto b = pcaa_cost_vector(kSecondInputAddress, 3, 1);
  const auto c = pcaa_cost_vector(kThirdInputAddress, 3, 1);
  const auto table = pcaa_cost_matrix(kThirdInputAddress, 2, 3, 4, 1);
  std::array<pcaa_command_t, 7> commands{};
  CHECK(pcaa_make_reduce2(a, b, kResultAddress, 0, &commands[0]) == 0);
  CHECK(pcaa_make_reduce3(a, b, c, kResultAddress, 0, &commands[1]) == 0);
  CHECK(pcaa_make_reduce2(a, b, kResultAddress, 1, &commands[2]) == 0);
  CHECK(pcaa_make_reduce3(a, b, c, kResultAddress, 1, &commands[3]) == 0);
  CHECK(pcaa_make_cost_add_vector(a, b, pcaa_cost_output(kResultAddress, 3, 1), &commands[4]) == 0);
  CHECK(pcaa_make_minplus_project(table, b, pcaa_cost_output(kResultAddress, 2, 1), &commands[5]) ==
        0);
  CHECK(pcaa_make_minplus_map3_project(a, b, table, pcaa_argmin_output(kResultAddress, 2, 1),
                                       &commands[6]) == 0);
  for (const pcaa_command_t &command : commands) {
    TestMemory reference(kTestMemorySize);
    reference.data = memory.data;
    reference_execute(reference, command);
    unsigned char encoded[ACCEL_COMMAND_MAX_BYTES]{};
    size_t width = 0;
    CHECK(pcaa_encode_one(&command, encoded, sizeof(encoded), &width) == PCAA_STATUS_OK);
    CHECK(submit_encoded(initiator, memory, encoded, width) == ACCEL_STATUS_DONE);
    const size_t result_bytes = command.kind == PCAA_COST_ADD_VECTOR   ? 3 * sizeof(int32_t)
                                : command.kind == PCAA_MINPLUS_PROJECT ? 2 * sizeof(int32_t)
                                : command.kind == PCAA_MINPLUS_MAP3_PROJECT
                                    ? 2 * sizeof(accel_min_argmin_result_t)
                                : command.kind == PCAA_MAP_ADD_REDUCE_MIN_ARGMIN ||
                                        command.kind == PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN
                                    ? sizeof(accel_min_argmin_result_t)
                                    : sizeof(int32_t);
    std::array<unsigned char, 16> expected{};
    std::array<unsigned char, 16> actual{};
    CHECK(reference.read(kResultAddress, expected.data(), result_bytes));
    CHECK(memory.read(kResultAddress, actual.data(), result_bytes));
    EXPECT_EQ(actual, expected);
  }
}

void test_vector_add_swap_symmetry(TestInitiator &initiator, TestMemory &memory) {
  struct Operands {
    int32_t first;
    int32_t second;
  };
  const std::array<Operands, 5> cases{
      {{2, -3}, {ACCEL_INF, -7}, {ACCEL_INF - 2, 5}, {ACCEL_INF + 1, ACCEL_INF}, {INT32_MIN, -1}}};
  for (const Operands operands : cases) {
    pcaa_command_t general{};
    pcaa_command_t alias_second{};
    const auto first = pcaa_cost_vector(kFirstInputAddress, 1, 1);
    const auto second = pcaa_cost_vector(kSecondInputAddress, 1, 1);
    ASSERT_EQ(
        pcaa_make_cost_add_vector(first, second, pcaa_cost_output(kResultAddress, 1, 1), &general),
        PCAA_STATUS_OK);
    ASSERT_EQ(pcaa_make_cost_add_vector(first, second, pcaa_cost_output(kSecondInputAddress, 1, 1),
                                        &alias_second),
              PCAA_STATUS_OK);
    unsigned char bytes[ACCEL_COMMAND_MAX_BYTES]{};
    size_t width = 0;
    ASSERT_EQ(pcaa_encode_one(&general, bytes, sizeof(bytes), &width), PCAA_STATUS_OK);
    ASSERT_EQ(width, 48u);
    ASSERT_TRUE(memory.write(kFirstInputAddress, &operands.first, sizeof(int32_t)));
    ASSERT_TRUE(memory.write(kSecondInputAddress, &operands.second, sizeof(int32_t)));
    const uint32_t general_status = submit_encoded(initiator, memory, bytes, width);
    int32_t general_result = 0;
    if (general_status == ACCEL_STATUS_DONE)
      ASSERT_TRUE(memory.read(kResultAddress, &general_result, sizeof(general_result)));
    ASSERT_EQ(pcaa_encode_one(&alias_second, bytes, sizeof(bytes), &width), PCAA_STATUS_OK);
    ASSERT_EQ(width, 32u);
    ASSERT_TRUE(memory.write(kFirstInputAddress, &operands.first, sizeof(int32_t)));
    ASSERT_TRUE(memory.write(kSecondInputAddress, &operands.second, sizeof(int32_t)));
    const uint32_t alias_status = submit_encoded(initiator, memory, bytes, width);
    EXPECT_EQ(alias_status, general_status);
    if (alias_status == ACCEL_STATUS_DONE) {
      int32_t alias_result = 0;
      ASSERT_TRUE(memory.read(kSecondInputAddress, &alias_result, sizeof(alias_result)));
      EXPECT_EQ(alias_result, general_result);
    }
  }
}

int sc_main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
