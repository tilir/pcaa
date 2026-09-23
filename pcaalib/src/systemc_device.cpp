// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Stages semantic commands and submits them through a SystemC target socket.

#include "pcaa_systemc_device.h"
#include "accel_protocol.h"
#include "memory_interface.h"
#include "pcaa.h"
#include "pcaa_device.h"
#include "pcaa_submission.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

#include <sysc/kernel/sc_time.h>
#include <tlm_core/tlm_2/tlm_2_interfaces/tlm_fw_bw_ifs.h>
#include <tlm_core/tlm_2/tlm_generic_payload/tlm_gp.h>

namespace {
constexpr int kAddressLowBits = 32;
constexpr uint32_t kDoorbellSubmit = 1;
}  // namespace

PcaaSystemCDevice::PcaaSystemCDevice(MemoryInterface &memory, void *allocation_context,
                                     Allocate allocate, tlm::tlm_fw_transport_if<> &transport)
    : memory_(memory),
      allocation_context_(allocation_context),
      allocate_(allocate),
      transport_(transport) {
  device_.context = this;
  device_.submit_command = submit_command_callback;
  device_.submit_batch = submit_batch_callback;
  device_.wait = wait_callback;
}

pcaa_status_t PcaaSystemCDevice::submit_command_callback(void *context,
                                                         const pcaa_command_t *command) {
  return static_cast<PcaaSystemCDevice *>(context)->submit_command(command);
}

pcaa_status_t PcaaSystemCDevice::submit_batch_callback(void *context,
                                                       const pcaa_command_t *commands,
                                                       size_t count) {
  return static_cast<PcaaSystemCDevice *>(context)->submit_batch(commands, count);
}

pcaa_status_t PcaaSystemCDevice::wait_callback(void *context, pcaa_completion_t *completion) {
  return static_cast<PcaaSystemCDevice *>(context)->wait(completion);
}

pcaa_status_t PcaaSystemCDevice::submit_command(const pcaa_command_t *command) {
  if (pending_)
    return PCAA_STATUS_BUSY;
  if (allocate_ == nullptr)
    return PCAA_STATUS_INVALID_ARGUMENT;
  pcaa_encoded_slot_t encoded{};
  const pcaa_status_t encoded_status = pcaa_encode_command(command, &encoded);
  if (encoded_status != PCAA_STATUS_OK)
    return encoded_status;
  const size_t width = pcaa_encoded_command_bytes();
  const pcaa_guest_address_t address = allocate_(allocation_context_, width);
  if (address == 0)
    return PCAA_STATUS_NO_SPACE;
  if (!memory_.write(address, encoded.bytes, width))
    return PCAA_STATUS_MEMORY_ERROR;
  if (!ring(address))
    return PCAA_STATUS_TRANSPORT_ERROR;
  pending_batch_count_ = 0;
  pending_ = true;
  return PCAA_STATUS_OK;
}

pcaa_status_t PcaaSystemCDevice::submit_batch(const pcaa_command_t *commands, size_t count) {
  if (pending_)
    return PCAA_STATUS_BUSY;
  if (allocate_ == nullptr)
    return PCAA_STATUS_INVALID_ARGUMENT;
  if (count > UINT32_MAX)
    return PCAA_STATUS_RANGE;
  if (pcaa_encoded_batch_bytes(count) == 0)
    return PCAA_STATUS_NO_SPACE;
  const size_t width = pcaa_encoded_command_bytes();
  std::vector<unsigned char> encoded;
  try {
    encoded.resize(count * width);
  } catch (const std::bad_alloc &) {
    return PCAA_STATUS_NO_SPACE;
  }
  const pcaa_status_t encoded_status =
      pcaa_encode_commands(commands, count, encoded.data(), encoded.size());
  if (encoded_status != PCAA_STATUS_OK)
    return encoded_status;
  const pcaa_guest_address_t child_address = allocate_(allocation_context_, encoded.size());
  const pcaa_guest_address_t result_address =
      allocate_(allocation_context_, sizeof(accel_batch_result_t));
  const pcaa_guest_address_t parent_address = allocate_(allocation_context_, width);
  if (child_address == 0 || result_address == 0 || parent_address == 0)
    return PCAA_STATUS_NO_SPACE;
  const accel_batch_result_t initial_result{0, UINT32_MAX};
  if (!memory_.write(child_address, encoded.data(), encoded.size()) ||
      !memory_.write(result_address, &initial_result, sizeof(initial_result)))
    return PCAA_STATUS_MEMORY_ERROR;
  pcaa_encoded_slot_t parent{};
  const pcaa_status_t parent_status =
      pcaa_encode_batch(child_address, count, result_address, &parent);
  if (parent_status != PCAA_STATUS_OK)
    return parent_status;
  if (!memory_.write(parent_address, parent.bytes, width))
    return PCAA_STATUS_MEMORY_ERROR;
  if (!ring(parent_address))
    return PCAA_STATUS_TRANSPORT_ERROR;
  batch_result_address_ = result_address;
  pending_batch_count_ = count;
  pending_ = true;
  return PCAA_STATUS_OK;
}

pcaa_status_t PcaaSystemCDevice::wait(pcaa_completion_t *completion) {
  if (!pending_)
    return PCAA_STATUS_NO_PENDING;
  uint32_t status = ACCEL_STATUS_IDLE;
  if (!mmio_read(ACCEL_MMIO_STATUS, &status))
    return PCAA_STATUS_TRANSPORT_ERROR;
  if (status == ACCEL_STATUS_IDLE || status == ACCEL_STATUS_BUSY)
    return PCAA_STATUS_BUSY;
  pending_ = false;
  if (pending_batch_count_ == 0)
    return status == ACCEL_STATUS_DONE ? PCAA_STATUS_OK : PCAA_STATUS_DEVICE_ERROR;
  accel_batch_result_t result{};
  if (!memory_.read(batch_result_address_, &result, sizeof(result)))
    return status == ACCEL_STATUS_DONE ? PCAA_STATUS_MEMORY_ERROR : PCAA_STATUS_DEVICE_ERROR;
  if (completion != nullptr && (result.completed != 0 || result.failed_index != UINT32_MAX)) {
    completion->has_batch_result = 1;
    completion->completed = result.completed;
    completion->failed_index = result.failed_index;
  }
  return status == ACCEL_STATUS_DONE && result.completed == pending_batch_count_ &&
                 result.failed_index == UINT32_MAX
             ? PCAA_STATUS_OK
             : PCAA_STATUS_DEVICE_ERROR;
}

bool PcaaSystemCDevice::mmio_write(uint64_t address, uint32_t value) {
  return transport(address, &value, true);
}

bool PcaaSystemCDevice::mmio_read(uint64_t address, uint32_t *value) {
  return transport(address, value, false);
}

bool PcaaSystemCDevice::transport(uint64_t address, uint32_t *value, bool write) {
  tlm::tlm_generic_payload transaction;
  sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
  transaction.set_command(write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
  transaction.set_address(address);
  transaction.set_data_ptr(reinterpret_cast<unsigned char *>(value));
  transaction.set_data_length(sizeof(*value));
  transaction.set_streaming_width(sizeof(*value));
  transport_.b_transport(transaction, delay);
  return transaction.get_response_status() == tlm::TLM_OK_RESPONSE;
}

bool PcaaSystemCDevice::ring(pcaa_guest_address_t descriptor_address) {
  return mmio_write(ACCEL_MMIO_DESC_ADDR_LO, static_cast<uint32_t>(descriptor_address)) &&
         mmio_write(ACCEL_MMIO_DESC_ADDR_HI,
                    static_cast<uint32_t>(descriptor_address >> kAddressLowBits)) &&
         mmio_write(ACCEL_MMIO_DOORBELL, kDoorbellSubmit);
}
