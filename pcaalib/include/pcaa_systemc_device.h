// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Binds semantic PCAA submissions to a hosted SystemC/TLM device.

#pragma once

#include "memory_interface.h"
#include "pcaa_device.h"

#include <cstddef>
#include <cstdint>

#include <tlm_core/tlm_2/tlm_2_interfaces/tlm_fw_bw_ifs.h>

class PcaaSystemCDevice {
 public:
  using Allocate = pcaa_guest_address_t (*)(void *context, size_t size);

  PcaaSystemCDevice(MemoryInterface &memory, void *allocation_context, Allocate allocate,
                    tlm::tlm_fw_transport_if<> &transport);
  PcaaSystemCDevice(const PcaaSystemCDevice &) = delete;
  PcaaSystemCDevice &operator=(const PcaaSystemCDevice &) = delete;
  PcaaSystemCDevice(PcaaSystemCDevice &&) = delete;
  PcaaSystemCDevice &operator=(PcaaSystemCDevice &&) = delete;

  pcaa_device_t *device() {
    return &device_;
  }

 private:
  static pcaa_status_t submit_command_callback(void *context, const pcaa_command_t *command);
  static pcaa_status_t submit_batch_callback(void *context, const pcaa_command_t *commands,
                                             size_t count);
  static pcaa_status_t wait_callback(void *context, pcaa_completion_t *completion);
  pcaa_status_t submit_command(const pcaa_command_t *command);
  pcaa_status_t submit_batch(const pcaa_command_t *commands, size_t count);
  pcaa_status_t wait(pcaa_completion_t *completion);
  bool mmio_write(uint64_t address, uint32_t value);
  bool mmio_read(uint64_t address, uint32_t *value);
  bool transport(uint64_t address, uint32_t *value, bool write);
  bool ring(pcaa_guest_address_t descriptor_address);

  MemoryInterface &memory_;
  void *allocation_context_;
  Allocate allocate_;
  tlm::tlm_fw_transport_if<> &transport_;
  pcaa_device_t device_{};
  pcaa_guest_address_t batch_result_address_ = 0;
  size_t pending_batch_count_ = 0;
  bool pending_ = false;
};
