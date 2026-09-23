// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements the SystemC/TLM functional model of the cost-algebra accelerator.

#include "accelerator.h"
#include "accel_protocol.h"
#include "cost_math.h"
#include "memory_interface.h"

#include <cstring>
#include <iostream>
#include <limits>

#include <tlm_core/tlm_2/tlm_generic_payload/tlm_gp.h>
namespace {
constexpr uint64_t kLow32Mask = 0x00000000ffffffffULL;
constexpr uint64_t kHigh32Mask = 0xffffffff00000000ULL;
constexpr int kPhysicalAddressLowBits = 32;

bool span(uint64_t base, uint32_t rows, uint32_t columns, uint32_t outer_stride,
          uint32_t inner_stride, uint64_t element_bytes, uint64_t *end) {
  if (base == 0 || rows == 0 || columns == 0 || inner_stride == 0 ||
      (rows > 1 && outer_stride == 0))
    return false;
  const uint64_t outer = uint64_t(rows - 1) * outer_stride;
  const uint64_t inner = uint64_t(columns - 1) * inner_stride;
  if (outer > std::numeric_limits<uint64_t>::max() - inner ||
      base > std::numeric_limits<uint64_t>::max() - element_bytes ||
      outer + inner > (std::numeric_limits<uint64_t>::max() - base - element_bytes) / element_bytes)
    return false;
  const uint64_t last = outer + inner;
  *end = base + (last + 1) * element_bytes;
  return true;
}

bool overlaps(uint64_t first, uint64_t first_end, uint64_t second, uint64_t second_end) {
  return first < second_end && second < first_end;
}

bool output_overlaps(const accel_command_t &command, uint64_t protected_begin,
                     uint64_t protected_end) {
  const bool vector_add = command.opcode == ACCEL_OPCODE_COST_ADD_VECTOR;
  const bool project = command.opcode == ACCEL_OPCODE_MINPLUS_PROJECT;
  const bool map3 = command.opcode == ACCEL_OPCODE_MINPLUS_MAP3_PROJECT;
  const uint32_t count = vector_add ? command.n : (project || map3 ? command.m : 1);
  const uint32_t stride = vector_add || project || map3 ? command.dst_stride : 1;
  const uint64_t width = map3 || command.opcode == ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN ||
                                 command.opcode == ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN
                             ? sizeof(accel_min_argmin_result_t)
                             : sizeof(int32_t);
  uint64_t end = 0;
  if (!span(command.dst, 1, count, 0, stride, width, &end))
    return true;
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t address = command.dst + uint64_t(i) * stride * width;
    if (overlaps(address, address + width, protected_begin, protected_end))
      return true;
  }
  return false;
}
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

  if (command.opcode >= ACCEL_OPCODE_COST_ADD_VECTOR &&
      command.opcode <= ACCEL_OPCODE_MINPLUS_MAP3_PROJECT) {
    if (command.flags != 0 || command.k != 0 || command.reserved != 0)
      return false;
    const bool add = command.opcode == ACCEL_OPCODE_COST_ADD_VECTOR;
    const bool map3 = command.opcode == ACCEL_OPCODE_MINPLUS_MAP3_PROJECT;
    if (add && command.m != 0)
      return false;
    const uint32_t rows = add ? 1 : command.m;
    const uint32_t columns = command.n;
    if (rows == 0 || (map3 && command.src2 == 0))
      return false;
    uint64_t first_end = 0;
    uint64_t second_end = 0;
    uint64_t third_end = 0;
    uint64_t destination_end = 0;
    const uint64_t result_bytes = map3 ? sizeof(accel_min_argmin_result_t) : sizeof(int32_t);
    if (!span(command.src0, map3 ? 1 : rows, columns, add || map3 ? 0 : command.src0_outer_stride,
              command.src0_stride, sizeof(int32_t), &first_end) ||
        !span(command.src1, 1, columns, 0, command.src1_stride, sizeof(int32_t), &second_end) ||
        !span(command.dst, 1, add ? columns : rows, 0, command.dst_stride, result_bytes,
              &destination_end))
      return false;
    if (map3 && !span(command.src2, rows, columns, command.src2_outer_stride, command.src2_stride,
                      sizeof(int32_t), &third_end))
      return false;
    if (add) {
      const bool first_alias =
          command.dst == command.src0 && command.dst_stride == command.src0_stride;
      const bool second_alias =
          command.dst == command.src1 && command.dst_stride == command.src1_stride;
      if ((overlaps(command.dst, destination_end, command.src0, first_end) && !first_alias) ||
          (overlaps(command.dst, destination_end, command.src1, second_end) && !second_alias))
        return false;
    } else if (overlaps(command.dst, destination_end, command.src0, first_end) ||
               overlaps(command.dst, destination_end, command.src1, second_end) ||
               (map3 && overlaps(command.dst, destination_end, command.src2, third_end))) {
      return false;
    }
    return true;
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
  uint64_t children_end = 0;
  uint64_t descriptor_end = 0;
  uint64_t result_end = 0;
  if (!span(command.src0, 1, command.n, 0, 1, sizeof(accel_command_t), &children_end) ||
      !span(descriptor_address_, 1, 1, 0, 1, sizeof(accel_command_t), &descriptor_end) ||
      !span(command.dst, 1, 1, 0, 1, sizeof(accel_batch_result_t), &result_end) ||
      overlaps(command.dst, result_end, command.src0, children_end) ||
      overlaps(command.dst, result_end, descriptor_address_, descriptor_end))
    return false;
  if (timing_.mode != AccelTimingMode::kUntimed && timing_.descriptor_bytes_per_cycle != 0 &&
      timing_.memory_write_bytes_per_cycle != 0) {
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
        output_overlaps(child, command.src0, children_end) ||
        output_overlaps(child, descriptor_address_, descriptor_end) ||
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
  if (command.opcode >= ACCEL_OPCODE_COST_ADD_VECTOR &&
      command.opcode <= ACCEL_OPCODE_MINPLUS_MAP3_PROJECT) {
    const bool written = execute_vector_command(command);
    if (written && timing_.mode != AccelTimingMode::kUntimed)
      accel_accumulate_command_timing(command, timing_, &timing_statistics_);
    return written;
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

    int32_t value = 0;
    if (accel_cost_add_checked(left, right, &value) != 0) {
      return false;
    }
    if (command.opcode == ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN ||
        command.opcode == ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN) {
      if (accel_cost_add_checked(value, third, &value) != 0) {
        return false;
      }
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

bool Accelerator::vector_output(const accel_command_t &command, uint32_t output,
                                accel_min_argmin_result_t *result) {
  const bool add = command.opcode == ACCEL_OPCODE_COST_ADD_VECTOR;
  const bool map3 = command.opcode == ACCEL_OPCODE_MINPLUS_MAP3_PROJECT;
  result->value = ACCEL_INF;
  result->index = 0;
  const uint32_t count = add ? 1 : command.n;
  for (uint32_t inner = 0; inner < count; ++inner) {
    const uint32_t vector_index = add ? output : inner;
    const uint64_t first_index = add ? uint64_t(output) * command.src0_stride
                                     : uint64_t(map3 ? 0 : output) * command.src0_outer_stride +
                                           uint64_t(inner) * command.src0_stride;
    int32_t first = 0;
    int32_t second = 0;
    if (!read_i32(command.src0 + first_index * sizeof(int32_t), &first) ||
        !read_i32(command.src1 +
                      uint64_t(map3 ? inner : vector_index) * command.src1_stride * sizeof(int32_t),
                  &second))
      return false;
    int32_t value = 0;
    if (accel_cost_add_checked(first, second, &value) != 0)
      return false;
    if (map3) {
      int32_t third = 0;
      const uint64_t third_index =
          uint64_t(output) * command.src2_outer_stride + uint64_t(inner) * command.src2_stride;
      if (!read_i32(command.src2 + third_index * sizeof(int32_t), &third) ||
          accel_cost_add_checked(value, third, &value) != 0)
        return false;
    }
    if (inner == 0 || value < result->value) {
      result->value = value;
      result->index = inner;
    }
  }
  return true;
}

bool Accelerator::execute_vector_command(const accel_command_t &command) {
  const bool add = command.opcode == ACCEL_OPCODE_COST_ADD_VECTOR;
  const bool map3 = command.opcode == ACCEL_OPCODE_MINPLUS_MAP3_PROJECT;
  const uint32_t outputs = add ? command.n : command.m;
  for (uint32_t output = 0; output < outputs; ++output) {
    accel_min_argmin_result_t result{};
    if (!vector_output(command, output, &result))
      return false;
    const uint64_t address = command.dst + uint64_t(output) * command.dst_stride *
                                               (map3 ? sizeof(result) : sizeof(int32_t));
    if (map3) {
      if (!memory_.write(address, &result, sizeof(result)))
        return false;
    } else if (!write_i32(address, result.value)) {
      return false;
    }
  }
  return true;
}
