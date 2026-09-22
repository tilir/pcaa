// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Exercises the accelerator through TLM MMIO using an in-memory guest RAM.

#include "accelerator.h"
#include "accel_protocol.h"
#include "cost_math.h"
#include "memory_interface.h"
#include "timing_model.h"

#include <array>
#include <cstring>
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

uint32_t submit(TestInitiator &initiator, TestMemory &memory, const accel_command_t &command,
                uint64_t descriptor_address = kDescriptorAddress) {
  CHECK(memory.write(descriptor_address, &command, sizeof(command)));

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

void expect_done(TestInitiator &initiator, TestMemory &memory, const accel_command_t &command) {
  CHECK(submit(initiator, memory, command) == ACCEL_STATUS_DONE);
}

void expect_error(TestInitiator &initiator, TestMemory &memory, const accel_command_t &command) {
  CHECK(submit(initiator, memory, command) == ACCEL_STATUS_ERROR);
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

TEST(SystemcAccelerator, ExecutesCommandsAndReportsErrors) {
  CHECK(accel_cost_add(2, 3) == 5);
  CHECK(accel_cost_add(ACCEL_INF, -100) == ACCEL_INF);
  CHECK(accel_cost_add(ACCEL_INF - 1, 2) == ACCEL_INF);
  CHECK(accel_cost_add(INT32_MIN, -1) == INT32_MIN);
  int32_t checked_sum = 0;
  CHECK(accel_cost_add_checked(INT32_MIN, -1, &checked_sum) != 0);

  TestMemory memory(kTestMemorySize);
  AccelTimingConfig timing;
  timing.mode = AccelTimingMode::kL1Sequential;
  timing.lanes = 4;
  timing.descriptor_bytes_per_cycle = sizeof(accel_command_t);
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

  accel_command_t command{ACCEL_OPCODE_MAP_ADD_REDUCE_MIN,
                          0,
                          1,
                          0,
                          0,
                          0,
                          kFirstInputAddress,
                          kSecondInputAddress,
                          0,
                          kResultAddress};
  expect_done(initiator, memory, command);
  int32_t result = 0;
  CHECK(memory.read(kResultAddress, &result, sizeof(result)) && result == ACCEL_INF);

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
  CHECK(!memory.write(kTruncatedDescriptorAddress, &command, sizeof(command)));
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
  EXPECT_GT(accelerator.timing_statistics().primitive_count, 0);
  EXPECT_GE(accelerator.timing_statistics().batch_count, 7);
  EXPECT_GT(accelerator.timing_statistics().descriptor_cycles, 0);
  EXPECT_GT(accelerator.timing_statistics().total_service_cycles, 0);
  const int32_t zero_bandwidth_first = 1;
  const int32_t zero_bandwidth_second = 2;
  const accel_command_t child = {ACCEL_OPCODE_MAP_ADD_REDUCE_MIN,
                                 0,
                                 1,
                                 0,
                                 0,
                                 0,
                                 kFirstInputAddress,
                                 kSecondInputAddress,
                                 0,
                                 kResultAddress};
  const accel_command_t batch = {
      ACCEL_OPCODE_EXECUTE_BATCH, 0, 1, 0, 0, 0, kBatchDescriptorAddress, 0, 0,
      kBatchResultAddress};
  CHECK(zero_bandwidth_memory.write(kFirstInputAddress, &zero_bandwidth_first,
                                    sizeof(zero_bandwidth_first)));
  CHECK(zero_bandwidth_memory.write(kSecondInputAddress, &zero_bandwidth_second,
                                    sizeof(zero_bandwidth_second)));
  CHECK(zero_bandwidth_memory.write(kBatchDescriptorAddress, &child, sizeof(child)));
  expect_done(zero_bandwidth_initiator, zero_bandwidth_memory, batch);
  EXPECT_EQ(zero_bandwidth_accelerator.timing_statistics().total_service_cycles, 0);
}

TEST(TimingModel, CalculatesSequentialAndStreamingCommandCycles) {
  AccelTimingConfig config;
  config.mode = AccelTimingMode::kL1Sequential;
  config.lanes = 4;
  config.descriptor_bytes_per_cycle = sizeof(accel_command_t);
  config.memory_read_bytes_per_cycle = 16;
  config.memory_write_bytes_per_cycle = 8;
  config.primitive_start_cycles = 1;
  config.map_pipeline_latency = 2;
  config.add3_map_extra_latency = 5;
  config.reduction_tree_latency = 3;
  config.result_latency = 4;

  accel_command_t command{};
  command.opcode = ACCEL_OPCODE_MAP_ADD_REDUCE_MIN;
  command.n = 3;
  AccelCommandTiming timing = accel_estimate_command_cycles(command, config);
  EXPECT_EQ(timing.descriptor_cycles, 1);
  EXPECT_EQ(timing.operand_read_cycles, 2);
  EXPECT_EQ(timing.compute_cycles, 11);
  EXPECT_EQ(timing.result_write_cycles, 1);
  EXPECT_EQ(timing.total_cycles, 15);

  command.n = 4;
  EXPECT_EQ(accel_estimate_command_cycles(command, config).total_cycles, 15);
  command.n = 5;
  EXPECT_EQ(accel_estimate_command_cycles(command, config).total_cycles, 17);

  command.opcode = ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN;
  command.n = 4;
  timing = accel_estimate_command_cycles(command, config);
  EXPECT_EQ(timing.operand_read_cycles, 3);
  EXPECT_EQ(timing.compute_cycles, 16);
  EXPECT_EQ(timing.total_cycles, 21);

  config.mode = AccelTimingMode::kL1Streaming;
  EXPECT_EQ(accel_estimate_command_cycles(command, config).total_cycles, 18);

  command.opcode = ACCEL_OPCODE_COST_ADD_VECTOR;
  command.n = 5;
  timing = accel_estimate_command_cycles(command, config);
  EXPECT_EQ(timing.operand_read_cycles, 3);
  EXPECT_EQ(timing.result_write_cycles, 3);
  command.opcode = ACCEL_OPCODE_MINPLUS_PROJECT;
  command.n = 3;
  command.m = 2;
  timing = accel_estimate_command_cycles(command, config);
  EXPECT_EQ(timing.operand_read_cycles, 3);
  EXPECT_EQ(timing.result_write_cycles, 1);
  command.opcode = ACCEL_OPCODE_MINPLUS_MAP3_PROJECT;
  timing = accel_estimate_command_cycles(command, config);
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
  std::array<accel_command_t, 2> children = {{
      {ACCEL_OPCODE_MAP_ADD_REDUCE_MIN, 0, 3, 0, 0, 0, kFirstInputAddress, kSecondInputAddress, 0,
       kBatchSecondResultAddress},
      {ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN, 0, 3, 0, 0, 0, kFirstInputAddress,
       kSecondInputAddress, kThirdInputAddress, kResultAddress},
  }};
  CHECK(memory.write(kBatchDescriptorAddress, children.data(), sizeof(children)));
  const accel_command_t batch = {ACCEL_OPCODE_EXECUTE_BATCH,
                                 0,
                                 static_cast<uint32_t>(children.size()),
                                 0,
                                 0,
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
  CHECK(memory.write(kBatchDescriptorAddress, children.data(), sizeof(children)));
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
  CHECK(memory.write(kBatchDescriptorAddress, children.data(), sizeof(children)));
  expect_error(initiator, memory, batch);
  CHECK(memory.read(kBatchResultAddress, &batch_result, sizeof(batch_result)));
  CHECK(batch_result.completed == 1 && batch_result.failed_index == 1);

  children[0].dst = kInvalidElementAddress;
  children[1].opcode = ACCEL_OPCODE_MAP_ADD_REDUCE_MIN;
  CHECK(memory.write(kBatchDescriptorAddress, children.data(), sizeof(children)));
  expect_error(initiator, memory, batch);
  CHECK(memory.read(kBatchResultAddress, &batch_result, sizeof(batch_result)));
  CHECK(batch_result.completed == 0 && batch_result.failed_index == 0);

  children[0] = batch;
  CHECK(memory.write(kBatchDescriptorAddress, children.data(), sizeof(children)));
  expect_error(initiator, memory, batch);
  CHECK(memory.read(kBatchResultAddress, &batch_result, sizeof(batch_result)));
  CHECK(batch_result.completed == 0 && batch_result.failed_index == 0);

  accel_command_t invalid_batch = batch;
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
  CHECK(memory.write(kBatchDescriptorAddress, children.data(), sizeof(accel_command_t)));
  invalid_batch = batch;
  invalid_batch.n = 1;
  expect_done(initiator, memory, invalid_batch);
}

void test_vector_isa(TestInitiator &initiator, TestMemory &memory) {
  const std::array<int32_t, 5> first = {3, ACCEL_INF, -2, 5, ACCEL_INF - 1};
  const std::array<int32_t, 5> second = {4, -9, 1, -8, 2};
  CHECK(memory.write(kFirstInputAddress, first.data(), sizeof(first)));
  CHECK(memory.write(kSecondInputAddress, second.data(), sizeof(second)));
  accel_command_t add{};
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
  add.src0_stride = 0;
  expect_error(initiator, memory, add);
  add.src0_stride = 1;
  add.dst = kFirstInputAddress + sizeof(int32_t);
  expect_error(initiator, memory, add);
  add.dst = kResultAddress;
  add.m = 1;
  expect_error(initiator, memory, add);
  add.m = 0;
  const std::array<int32_t, 2> underflow = {1, INT32_MIN};
  const std::array<int32_t, 2> minus_one = {1, -1};
  const std::array<int32_t, 2> sentinel = {73, 74};
  CHECK(memory.write(kFirstInputAddress, underflow.data(), sizeof(underflow)));
  CHECK(memory.write(kSecondInputAddress, minus_one.data(), sizeof(minus_one)));
  CHECK(memory.write(kResultAddress, sentinel.data(), sizeof(sentinel)));
  add.n = 2;
  expect_error(initiator, memory, add);
  std::array<int32_t, 2> unchanged{};
  CHECK(memory.read(kResultAddress, unchanged.data(), sizeof(unchanged)));
  EXPECT_EQ(unchanged, sentinel);

  // Three padded rows, two reduced columns: row stride 4, inner stride 1.
  const std::array<int32_t, 12> matrix = {4, 1, 99, 99, -2, 6, 99, 99, ACCEL_INF, 0, 99, 99};
  const std::array<int32_t, 2> unary = {-1, 2};
  CHECK(memory.write(kFirstInputAddress, matrix.data(), sizeof(matrix)));
  CHECK(memory.write(kSecondInputAddress, unary.data(), sizeof(unary)));
  accel_command_t project{};
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
  project.src1 = kInvalidElementAddress;
  expect_error(initiator, memory, project);
  project.src1 = kSecondInputAddress;
  const std::array<int32_t, 2> underflow_unary = {INT32_MIN, 0};
  CHECK(memory.write(kSecondInputAddress, underflow_unary.data(), sizeof(underflow_unary)));
  CHECK(memory.write(kResultAddress, sentinel.data(), sizeof(sentinel)));
  expect_error(initiator, memory, project);
  CHECK(memory.read(kResultAddress, unchanged.data(), sizeof(unchanged)));
  EXPECT_EQ(unchanged, sentinel);

  const std::array<int32_t, 2> zero = {0, 0};
  const std::array<int32_t, 2> fixed = {1, 1};
  const std::array<int32_t, 12> varying = {2, 2, 99, 99, ACCEL_INF, -3, 99, 99, -4, 0, 99, 99};
  CHECK(memory.write(kFirstInputAddress, varying.data(), sizeof(varying)));
  CHECK(memory.write(kSecondInputAddress, zero.data(), sizeof(zero)));
  CHECK(memory.write(kThirdInputAddress, fixed.data(), sizeof(fixed)));
  accel_command_t map3{};
  map3.opcode = ACCEL_OPCODE_MINPLUS_MAP3_PROJECT;
  map3.n = 2;
  map3.m = 3;
  map3.src0 = kSecondInputAddress;
  map3.src1 = kThirdInputAddress;
  map3.src2 = kFirstInputAddress;
  map3.dst = kResultAddress;
  map3.src0_stride = map3.src1_stride = map3.src2_stride = map3.dst_stride = 1;
  map3.src2_outer_stride = 4;
  expect_done(initiator, memory, map3);
  std::array<accel_min_argmin_result_t, 3> results{};
  CHECK(memory.read(kResultAddress, results.data(), sizeof(results)));
  EXPECT_EQ(results[0].value, 3);
  EXPECT_EQ(results[0].index, 0u);
  EXPECT_EQ(results[1].value, -2);
  EXPECT_EQ(results[1].index, 1u);
  EXPECT_EQ(results[2].value, -3);
  EXPECT_EQ(results[2].index, 0u);
  accel_command_t scalar{};
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
  CHECK(memory.write(kResultAddress, sentinel.data(), sizeof(sentinel)));
  expect_error(initiator, memory, map3);
  CHECK(memory.read(kResultAddress, unchanged.data(), sizeof(unchanged)));
  EXPECT_EQ(unchanged, sentinel);

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
  std::array<accel_command_t, 2> children = {project, add};
  CHECK(memory.write(kBatchDescriptorAddress, children.data(), sizeof(children)));
  const accel_command_t batch = {
      ACCEL_OPCODE_EXECUTE_BATCH, 0, 2, 0, 0, 0, kBatchDescriptorAddress, 0, 0,
      kBatchResultAddress};
  expect_done(initiator, memory, batch);
  CHECK(memory.read(kResultAddress, unchanged.data(), sizeof(unchanged)));
  EXPECT_EQ(unchanged, (std::array<int32_t, 2>{3, -2}));
  children[1].src0_stride = 0;
  CHECK(memory.write(kBatchDescriptorAddress, children.data(), sizeof(children)));
  expect_error(initiator, memory, batch);
  accel_batch_result_t batch_result{};
  CHECK(memory.read(kBatchResultAddress, &batch_result, sizeof(batch_result)));
  EXPECT_EQ(batch_result.completed, 1u);
  EXPECT_EQ(batch_result.failed_index, 1u);
}

int sc_main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
