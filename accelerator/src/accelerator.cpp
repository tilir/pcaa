// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements the SystemC/TLM functional model of the cost-algebra accelerator.

#include "accelerator.h"
#include "accel_protocol.h"
#include "cost_math.h"
#include "memory_interface.h"
#include "pcaa_codec.h"

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

bool Accelerator::execute(sc_core::sc_time *delay) {
  const uint64_t initial_cycles = timing_statistics_.total_service_cycles;
  accel_command_t wire{};
  pcaa_command_t command{};
  if (descriptor_address_ == 0 || !memory_.read(descriptor_address_, &wire, sizeof(wire)) ||
      pcaa_decode_descriptor(&wire, &command) != 0) {
    return false;
  }

  const bool completed = command.kind == PCAA_ORDERED_BATCH ? execute_batch(command, delay)
                                                            : execute_command(command, delay);
  if (delay != nullptr && timing_.cycle_period != sc_core::SC_ZERO_TIME) {
    *delay += timing_.cycle_period * (timing_statistics_.total_service_cycles - initial_cycles);
  }
  return completed;
}

bool Accelerator::execute_batch(const pcaa_command_t &command, sc_core::sc_time *delay) {
  (void)delay;
  const pcaa_batch_reference_t &batch = command.operation.batch;
  uint64_t children_end = 0;
  uint64_t descriptor_end = 0;
  uint64_t result_end = 0;
  if (!span(batch.child_descriptors, 1, batch.count, 0, 1, sizeof(accel_command_t),
            &children_end) ||
      !span(descriptor_address_, 1, 1, 0, 1, sizeof(accel_command_t), &descriptor_end) ||
      !span(batch.result, 1, 1, 0, 1, sizeof(accel_batch_result_t), &result_end) ||
      overlaps(batch.result, result_end, batch.child_descriptors, children_end) ||
      overlaps(batch.result, result_end, descriptor_address_, descriptor_end))
    return false;
  if (timing_.mode != AccelTimingMode::kUntimed && timing_.descriptor_bytes_per_cycle != 0 &&
      timing_.memory_write_bytes_per_cycle != 0) {
    const uint64_t descriptor_cycles =
        (sizeof(accel_command_t) + timing_.descriptor_bytes_per_cycle - 1) /
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
    std::cerr << "pcaa: batch children=" << batch.count << '\n';
  }
  for (uint32_t index = 0; index < batch.count; ++index) {
    accel_command_t child_wire{};
    pcaa_command_t child{};
    const uint64_t address = batch.child_descriptors + uint64_t(index) * sizeof(child_wire);
    if (verbose_) {
      std::cerr << "pcaa: child=" << index << " descriptor=0x" << std::hex << address << std::dec
                << '\n';
    }
    if (!memory_.read(address, &child_wire, sizeof(child_wire)) ||
        pcaa_decode_descriptor(&child_wire, &child) != 0 || child.kind == PCAA_ORDERED_BATCH ||
        pcaa_output_overlaps(&child, batch.child_descriptors, children_end) ||
        pcaa_output_overlaps(&child, descriptor_address_, descriptor_end) ||
        !execute_command(child, delay)) {
      result.completed = index;
      result.failed_index = index;
      memory_.write(batch.result, &result, sizeof(result));
      return false;
    }
  }
  result.completed = batch.count;
  return memory_.write(batch.result, &result, sizeof(result));
}

bool Accelerator::execute_command(const pcaa_command_t &command, sc_core::sc_time *delay) {
  (void)delay;
  if (command.kind == PCAA_ORDERED_BATCH) {
    return false;
  }
  if (command.kind == PCAA_COST_ADD_VECTOR || command.kind == PCAA_MINPLUS_PROJECT ||
      command.kind == PCAA_MINPLUS_MAP3_PROJECT) {
    const bool written = execute_vector_command(command);
    if (written && timing_.mode != AccelTimingMode::kUntimed)
      accel_accumulate_semantic_command_timing(command, timing_, &timing_statistics_);
    return written;
  }

  const bool add3 =
      command.kind == PCAA_MAP_ADD3_REDUCE_MIN || command.kind == PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN;
  const bool argmin_output = command.kind == PCAA_MAP_ADD_REDUCE_MIN_ARGMIN ||
                             command.kind == PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN;
  const pcaa_cost_vector_view_t first =
      add3 ? command.operation.reduce3.first : command.operation.reduce2.first;
  const pcaa_cost_vector_view_t second =
      add3 ? command.operation.reduce3.second : command.operation.reduce2.second;
  const uint64_t destination =
      add3 ? command.operation.reduce3.result : command.operation.reduce2.result;
  int32_t minimum = ACCEL_INF;
  uint32_t argmin = 0;
  for (uint32_t i = 0; i < first.length; ++i) {
    const uint64_t offset = uint64_t(i) * sizeof(int32_t);
    int32_t left = 0;
    int32_t right = 0;
    int32_t third = 0;
    if (!read_i32(first.base + offset, &left) || !read_i32(second.base + offset, &right) ||
        (add3 && !read_i32(command.operation.reduce3.third.base + offset, &third))) {
      return false;
    }

    int32_t value = 0;
    if (accel_cost_add_checked(left, right, &value) != 0) {
      return false;
    }
    if (add3) {
      if (accel_cost_add_checked(value, third, &value) != 0) {
        return false;
      }
    }
    if (i == 0 || value < minimum) {
      minimum = value;
      argmin = i;
    }
  }
  if (argmin_output) {
    accel_min_argmin_result_t result{minimum, argmin};
    const bool written = memory_.write(destination, &result, sizeof(result));
    if (written && timing_.mode != AccelTimingMode::kUntimed) {
      accel_accumulate_semantic_command_timing(command, timing_, &timing_statistics_);
    }
    if (verbose_) {
      std::cerr << "pcaa: primitive kind=" << command.kind << " n=" << first.length
                << " value=" << minimum << " argmin=" << argmin << '\n';
    }
    return written;
  }
  const bool written = write_i32(destination, minimum);
  if (written && timing_.mode != AccelTimingMode::kUntimed) {
    accel_accumulate_semantic_command_timing(command, timing_, &timing_statistics_);
  }
  if (verbose_) {
    std::cerr << "pcaa: primitive kind=" << command.kind << " n=" << first.length
              << " value=" << minimum << '\n';
  }
  return written;
}

bool Accelerator::vector_output(const pcaa_command_t &command, uint32_t output,
                                accel_min_argmin_result_t *result) {
  const bool add = command.kind == PCAA_COST_ADD_VECTOR;
  const bool map3 = command.kind == PCAA_MINPLUS_MAP3_PROJECT;
  const pcaa_cost_vector_view_t first_view = add    ? command.operation.vector_add.first
                                             : map3 ? command.operation.map3_project.first
                                                    : command.operation.project.vector;
  const pcaa_cost_vector_view_t second_view = add    ? command.operation.vector_add.second
                                              : map3 ? command.operation.map3_project.second
                                                     : command.operation.project.vector;
  const pcaa_cost_matrix_view_t matrix =
      map3 ? command.operation.map3_project.third : command.operation.project.matrix;
  result->value = ACCEL_INF;
  result->index = 0;
  const uint32_t count = add ? 1 : first_view.length;
  for (uint32_t inner = 0; inner < count; ++inner) {
    const uint32_t vector_index = add ? output : inner;
    int32_t first = 0;
    int32_t second = 0;
    const uint64_t first_address =
        add || map3 ? first_view.base + uint64_t(vector_index) * first_view.stride * sizeof(int32_t)
                    : matrix.base + (uint64_t(output) * matrix.row_stride +
                                     uint64_t(inner) * matrix.column_stride) *
                                        sizeof(int32_t);
    const uint64_t second_address = second_view.base + uint64_t(map3 ? inner : vector_index) *
                                                           second_view.stride * sizeof(int32_t);
    if (!read_i32(first_address, &first) || !read_i32(second_address, &second))
      return false;
    int32_t value = 0;
    if (accel_cost_add_checked(first, second, &value) != 0)
      return false;
    if (map3) {
      int32_t third = 0;
      const uint64_t third_index =
          uint64_t(output) * matrix.row_stride + uint64_t(inner) * matrix.column_stride;
      if (!read_i32(matrix.base + third_index * sizeof(int32_t), &third) ||
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

bool Accelerator::execute_vector_command(const pcaa_command_t &command) {
  const bool add = command.kind == PCAA_COST_ADD_VECTOR;
  const bool map3 = command.kind == PCAA_MINPLUS_MAP3_PROJECT;
  const pcaa_output_view_t destination = add    ? command.operation.vector_add.result
                                         : map3 ? command.operation.map3_project.result
                                                : command.operation.project.result;
  const uint32_t outputs = destination.length;
  for (uint32_t output = 0; output < outputs; ++output) {
    accel_min_argmin_result_t result{};
    if (!vector_output(command, output, &result))
      return false;
    const uint64_t address = destination.base + uint64_t(output) * destination.stride *
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
