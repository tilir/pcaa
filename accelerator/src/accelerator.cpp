// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements the SystemC/TLM functional model of the cost-algebra accelerator.

#include "accelerator.h"
#include "accel_protocol.h"
#include "cost_math.h"
#include "memory_interface.h"

#include <cstring>
#include <iostream>

#include <tlm_core/tlm_2/tlm_generic_payload/tlm_gp.h>
namespace {
constexpr uint64_t kLow32Mask = 0x00000000ffffffffULL;
constexpr uint64_t kHigh32Mask = 0xffffffff00000000ULL;
constexpr int kPhysicalAddressLowBits = 32;
}  // namespace

bool Accelerator::is_valid_mmio_transaction(const tlm::tlm_generic_payload &transaction) const {
  return transaction.get_data_ptr() != nullptr &&
         transaction.get_data_length() == sizeof(uint32_t) &&
         transaction.get_streaming_width() == sizeof(uint32_t) &&
         transaction.get_byte_enable_ptr() == nullptr &&
         transaction.get_address() <= ACCEL_MMIO_SIZE - sizeof(uint32_t);
}

void Accelerator::b_transport(tlm::tlm_generic_payload &transaction, sc_core::sc_time &delay) {
  if (!is_valid_mmio_transaction(transaction)) {
    transaction.set_response_status(tlm::TLM_BURST_ERROR_RESPONSE);
    return;
  }

  uint32_t value = 0;
  if (transaction.is_read()) {
    if (!read_register(transaction.get_address(), &value)) {
      transaction.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
      return;
    }
    std::memcpy(transaction.get_data_ptr(), &value, sizeof(value));
  } else if (transaction.is_write()) {
    std::memcpy(&value, transaction.get_data_ptr(), sizeof(value));
    if (!write_register(transaction.get_address(), value, &delay)) {
      transaction.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
      return;
    }
  } else {
    transaction.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
    return;
  }

  transaction.set_response_status(tlm::TLM_OK_RESPONSE);
}

bool Accelerator::read_register(uint64_t address, uint32_t *value) const {
  switch (address) {
    case ACCEL_MMIO_DESC_ADDR_LO:
      *value = static_cast<uint32_t>(descriptor_address_);
      return true;
    case ACCEL_MMIO_DESC_ADDR_HI:
      *value = static_cast<uint32_t>(descriptor_address_ >> kPhysicalAddressLowBits);
      return true;
    case ACCEL_MMIO_STATUS:
      *value = status_;
      return true;
    default:
      return false;
  }
}

bool Accelerator::write_register(uint64_t address, uint32_t value, sc_core::sc_time *delay) {
  switch (address) {
    case ACCEL_MMIO_DESC_ADDR_LO:
      descriptor_address_ = (descriptor_address_ & kHigh32Mask) | value;
      return true;
    case ACCEL_MMIO_DESC_ADDR_HI:
      descriptor_address_ = (descriptor_address_ & kLow32Mask) |
                            (static_cast<uint64_t>(value) << kPhysicalAddressLowBits);
      return true;
    case ACCEL_MMIO_DOORBELL:
      if (status_ == ACCEL_STATUS_BUSY) {
        return false;
      }
      status_ = ACCEL_STATUS_BUSY;
      if (verbose_) {
        std::cerr << "pcaa: doorbell descriptor=0x" << std::hex << descriptor_address_ << std::dec
                  << '\n';
      }
      status_ = execute(delay) ? ACCEL_STATUS_DONE : ACCEL_STATUS_ERROR;
      return true;
    default:
      return false;
  }
}

bool Accelerator::read_i32(uint64_t address, int32_t *value) {
  return memory_.read(address, value, sizeof(*value));
}

bool Accelerator::write_i32(uint64_t address, int32_t value) {
  return memory_.write(address, &value, sizeof(value));
}

bool Accelerator::is_valid_command(const accel_command_t &command) const {
  if (command.opcode == ACCEL_OPCODE_EXECUTE_BATCH) {
    return command.n != 0 && command.src0 != 0 && command.dst != 0;
  }

  if (command.n == 0 || command.src0 == 0 || command.src1 == 0 || command.dst == 0) {
    return false;
  }

  switch (command.opcode) {
    case ACCEL_OPCODE_MAP_ADD_REDUCE_MIN:
    case ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN:
      return true;
    case ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN:
    case ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN:
      return command.src2 != 0;
    default:
      return false;
  }
}

bool Accelerator::execute(sc_core::sc_time *delay) {
  const uint64_t initial_cycles = timing_statistics_.total_service_cycles;
  accel_command_t command{};
  if (descriptor_address_ == 0 || !memory_.read(descriptor_address_, &command, sizeof(command)) ||
      !is_valid_command(command)) {
    return false;
  }

  const bool completed = command.opcode == ACCEL_OPCODE_EXECUTE_BATCH
                             ? execute_batch(command, delay)
                             : execute_command(command, delay);
  if (delay != nullptr && timing_.cycle_period != sc_core::SC_ZERO_TIME) {
    *delay += timing_.cycle_period * (timing_statistics_.total_service_cycles - initial_cycles);
  }
  return completed;
}

bool Accelerator::execute_batch(const accel_command_t &command, sc_core::sc_time *delay) {
  (void)delay;
  if (timing_.mode != AccelTimingMode::kUntimed && timing_.descriptor_bytes_per_cycle != 0) {
    const uint64_t descriptor_cycles = (sizeof(command) + timing_.descriptor_bytes_per_cycle - 1) /
                                       timing_.descriptor_bytes_per_cycle;
    timing_statistics_.descriptor_cycles += descriptor_cycles;
    timing_statistics_.total_service_cycles += descriptor_cycles + timing_.batch_start_cycles;
    const uint64_t result_cycles =
        (sizeof(accel_batch_result_t) + timing_.memory_write_bytes_per_cycle - 1) /
        timing_.memory_write_bytes_per_cycle;
    timing_statistics_.result_write_cycles += result_cycles;
    timing_statistics_.total_service_cycles += result_cycles;
    ++timing_statistics_.batch_count;
  }
  accel_batch_result_t result{0, UINT32_MAX};
  if (verbose_) {
    std::cerr << "pcaa: batch children=" << command.n << '\n';
  }
  for (uint32_t index = 0; index < command.n; ++index) {
    accel_command_t child{};
    const uint64_t address = command.src0 + uint64_t(index) * sizeof(child);
    if (verbose_) {
      std::cerr << "pcaa: child=" << index << " descriptor=0x" << std::hex << address << std::dec
                << '\n';
    }
    if (!memory_.read(address, &child, sizeof(child)) ||
        child.opcode == ACCEL_OPCODE_EXECUTE_BATCH || !is_valid_command(child) ||
        !execute_command(child, delay)) {
      result.completed = index;
      result.failed_index = index;
      memory_.write(command.dst, &result, sizeof(result));
      return false;
    }
  }
  result.completed = command.n;
  return memory_.write(command.dst, &result, sizeof(result));
}

bool Accelerator::execute_command(const accel_command_t &command, sc_core::sc_time *delay) {
  (void)delay;
  if (!is_valid_command(command) || command.opcode == ACCEL_OPCODE_EXECUTE_BATCH) {
    return false;
  }

  int32_t minimum = ACCEL_INF;
  uint32_t argmin = 0;
  for (uint32_t i = 0; i < command.n; ++i) {
    const uint64_t offset = uint64_t(i) * sizeof(int32_t);
    int32_t left = 0;
    int32_t right = 0;
    int32_t third = 0;
    if (!read_i32(command.src0 + offset, &left) || !read_i32(command.src1 + offset, &right) ||
        ((command.opcode == ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN ||
          command.opcode == ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN) &&
         !read_i32(command.src2 + offset, &third))) {
      return false;
    }

    int32_t value = accel_cost_add(left, right);
    if (command.opcode == ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN ||
        command.opcode == ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN) {
      value = accel_cost_add(value, third);
    }
    if (i == 0 || value < minimum) {
      minimum = value;
      argmin = i;
    }
  }
  if (command.opcode == ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN ||
      command.opcode == ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN) {
    accel_min_argmin_result_t result{minimum, argmin};
    const bool written = memory_.write(command.dst, &result, sizeof(result));
    if (written && timing_.mode != AccelTimingMode::kUntimed) {
      accel_accumulate_command_timing(command, timing_, &timing_statistics_);
    }
    if (verbose_) {
      std::cerr << "pcaa: primitive opcode=" << command.opcode << " n=" << command.n
                << " value=" << minimum << " argmin=" << argmin << '\n';
    }
    return written;
  }
  const bool written = write_i32(command.dst, minimum);
  if (written && timing_.mode != AccelTimingMode::kUntimed) {
    accel_accumulate_command_timing(command, timing_, &timing_statistics_);
  }
  if (verbose_) {
    std::cerr << "pcaa: primitive opcode=" << command.opcode << " n=" << command.n
              << " value=" << minimum << '\n';
  }
  return written;
}
