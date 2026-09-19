// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Exercises the accelerator through TLM MMIO using an in-memory guest RAM.

#include "accelerator.h"
#include "accel_protocol.h"
#include "cost_math.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#include <tlm_utils/simple_initiator_socket.h>

class TestMemory final : public MemoryInterface {
 public:
  explicit TestMemory(size_t size) : data(size) {}

  bool read(uint64_t address, void* destination, size_t size) override {
    if (!contains(address, size)) {
      return false;
    }
    std::memcpy(destination, data.data() + address, size);
    return true;
  }

  bool write(uint64_t address, const void* source, size_t size) override {
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

  explicit TestInitiator(sc_core::sc_module_name name) : sc_core::sc_module(name), socket("socket") {}
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

void mmio(TestInitiator& initiator, uint64_t address, uint32_t* value,
          tlm::tlm_command command) {
  tlm::tlm_generic_payload transaction;
  sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
  transaction.set_command(command);
  transaction.set_address(address);
  transaction.set_data_ptr(reinterpret_cast<unsigned char*>(value));
  transaction.set_data_length(sizeof(*value));
  transaction.set_streaming_width(sizeof(*value));
  initiator.socket->b_transport(transaction, delay);
  assert(transaction.get_response_status() == tlm::TLM_OK_RESPONSE);
}

uint32_t submit(TestInitiator& initiator, TestMemory& memory, const accel_command_t& command,
                uint64_t descriptor_address = kDescriptorAddress) {
  assert(memory.write(descriptor_address, &command, sizeof(command)));

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

void expect_done(TestInitiator& initiator, TestMemory& memory, const accel_command_t& command) {
  assert(submit(initiator, memory, command) == ACCEL_STATUS_DONE);
}

void expect_error(TestInitiator& initiator, TestMemory& memory, const accel_command_t& command) {
  assert(submit(initiator, memory, command) == ACCEL_STATUS_ERROR);
}

void test_invalid_mmio(TestInitiator& initiator) {
  uint16_t value = 0;
  tlm::tlm_generic_payload transaction;
  sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
  transaction.set_command(tlm::TLM_READ_COMMAND);
  transaction.set_address(ACCEL_MMIO_STATUS);
  transaction.set_data_ptr(reinterpret_cast<unsigned char*>(&value));
  transaction.set_data_length(sizeof(value));
  transaction.set_streaming_width(sizeof(value));
  initiator.socket->b_transport(transaction, delay);
  assert(transaction.get_response_status() == tlm::TLM_BURST_ERROR_RESPONSE);

  uint32_t aligned_value = 0;
  transaction.set_address(ACCEL_MMIO_STATUS + kInvalidMmioOffset);
  transaction.set_data_ptr(reinterpret_cast<unsigned char*>(&aligned_value));
  transaction.set_data_length(sizeof(aligned_value));
  transaction.set_streaming_width(sizeof(aligned_value));
  initiator.socket->b_transport(transaction, delay);
  assert(transaction.get_response_status() == tlm::TLM_ADDRESS_ERROR_RESPONSE);
}
}  // namespace

int sc_main(int, char**) {
  assert(accel_cost_add(2, 3) == 5);
  assert(accel_cost_add(ACCEL_INF, -100) == ACCEL_INF);
  assert(accel_cost_add(ACCEL_INF - 1, 2) == ACCEL_INF);
  assert(accel_cost_add(INT32_MIN, -1) == INT32_MIN);

  TestMemory memory(kTestMemorySize);
  Accelerator accelerator("accelerator", memory);
  TestInitiator initiator("initiator");
  initiator.socket.bind(accelerator.target_socket);
  sc_core::sc_start(sc_core::SC_ZERO_TIME);
  test_invalid_mmio(initiator);

  const int32_t first[] = {ACCEL_INF, -4, 7, -4, 9};
  const int32_t second[] = {1, 2, -10, 2, ACCEL_INF};
  const int32_t third[] = {3, 4, 5, -1, 7};
  assert(memory.write(kFirstInputAddress, first, sizeof(first)));
  assert(memory.write(kSecondInputAddress, second, sizeof(second)));
  assert(memory.write(kThirdInputAddress, third, sizeof(third)));

  accel_command_t command{ACCEL_OPCODE_MAP_ADD_REDUCE_MIN, 0, 1, 0, 0, 0,
                          kFirstInputAddress, kSecondInputAddress, 0, kResultAddress};
  expect_done(initiator, memory, command);
  int32_t result = 0;
  assert(memory.read(kResultAddress, &result, sizeof(result)) && result == ACCEL_INF);

  command.opcode = ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN;
  command.n = 5;
  command.src2 = kThirdInputAddress;
  expect_done(initiator, memory, command);
  assert(memory.read(kResultAddress, &result, sizeof(result)) && result == -3);

  command.opcode = ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN;
  command.src2 = 0;
  expect_done(initiator, memory, command);
  accel_min_argmin_result_t argmin_result{};
  assert(memory.read(kResultAddress, &argmin_result, sizeof(argmin_result)));
  assert(argmin_result.value == -3 && argmin_result.index == 2);

  const int32_t tie_a[] = {-4, -4, 9}; const int32_t tie_b[] = {0, 0, 0};
  assert(memory.write(kTieFirstInputAddress, tie_a, sizeof(tie_a)));
  assert(memory.write(kTieSecondInputAddress, tie_b, sizeof(tie_b)));
  command = {ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN, 0, 3, 0, 0, 0,
             kTieFirstInputAddress, kTieSecondInputAddress, 0, kTieResultAddress};
  expect_done(initiator, memory, command);
  assert(memory.read(kTieResultAddress, &argmin_result, sizeof(argmin_result)));
  assert(argmin_result.value == -4 && argmin_result.index == 0);

  std::vector<int32_t> large_a(kLargeVectorLength, 10);
  std::vector<int32_t> large_b(kLargeVectorLength, 1);
  large_a.back() = -100;
  assert(memory.write(kLargeFirstInputAddress, large_a.data(), large_a.size() * sizeof(int32_t)));
  assert(memory.write(kLargeSecondInputAddress, large_b.data(), large_b.size() * sizeof(int32_t)));
  command = {ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN, 0,
             static_cast<uint32_t>(kLargeVectorLength), 0, 0, 0,
             kLargeFirstInputAddress, kLargeSecondInputAddress, 0, kLargeResultAddress};
  expect_done(initiator, memory, command);
  assert(memory.read(kLargeResultAddress, &argmin_result, sizeof(argmin_result)));
  assert(argmin_result.value == -99 && argmin_result.index == 255);

  command.opcode = kUnsupportedOpcode;
  expect_error(initiator, memory, command);
  command.opcode = ACCEL_OPCODE_MAP_ADD_REDUCE_MIN;
  command.n = 0;
  expect_error(initiator, memory, command);
  command.opcode = ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN;
  command.n = 1;
  command.src2 = 0;
  expect_error(initiator, memory, command);

  command = {ACCEL_OPCODE_MAP_ADD_REDUCE_MIN, 0, 1, 0, 0, 0,
             kFirstInputAddress, kSecondInputAddress, 0, kResultAddress};
  assert(!memory.write(kTruncatedDescriptorAddress, &command, sizeof(command)));
  uint32_t register_value = kTruncatedDescriptorAddress;
  mmio(initiator, ACCEL_MMIO_DESC_ADDR_LO, &register_value, tlm::TLM_WRITE_COMMAND);
  register_value = 0;
  mmio(initiator, ACCEL_MMIO_DESC_ADDR_HI, &register_value, tlm::TLM_WRITE_COMMAND);
  register_value = kDoorbellSubmit;
  mmio(initiator, ACCEL_MMIO_DOORBELL, &register_value, tlm::TLM_WRITE_COMMAND);
  register_value = 0;
  mmio(initiator, ACCEL_MMIO_STATUS, &register_value, tlm::TLM_READ_COMMAND);
  assert(register_value == ACCEL_STATUS_ERROR);

  command.dst = kInvalidElementAddress;
  expect_error(initiator, memory, command);
  command.dst = kResultAddress;
  command.src0 = kInvalidElementAddress;
  expect_error(initiator, memory, command);
  command.src0 = kFirstInputAddress;
  expect_done(initiator, memory, command);

  std::cout << "SystemC accelerator unit tests passed\n";
  return 0;
}
