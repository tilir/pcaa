// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Declares the SystemC/TLM accelerator module and its control interface.

#pragma once

#include "accel_protocol.h"

#include <cstdint>
#include <map>
#include <vector>

#include <sysc/kernel/sc_dynamic_processes.h>
#include <sysc/kernel/sc_module.h>
#include <sysc/kernel/sc_module_name.h>
#include <sysc/kernel/sc_simcontext.h>
#include <sysc/kernel/sc_spawn.h>
#include <sysc/kernel/sc_time.h>
#include <sysc/kernel/sc_wait.h>
#include <tlm_core/tlm_2/tlm_2_interfaces/tlm_fw_bw_ifs.h>
#include <tlm_core/tlm_2/tlm_generic_payload/tlm_phase.h>
#include <tlm_utils/simple_target_socket.h>

class MemoryInterface;
namespace tlm {
class tlm_generic_payload;
}

class Accelerator : public sc_core::sc_module {
 public:
  tlm_utils::simple_target_socket<Accelerator> target_socket;

  Accelerator(sc_core::sc_module_name name, MemoryInterface &memory)
      : sc_core::sc_module(name), target_socket("target_socket"), memory_(memory) {
    target_socket.register_b_transport(this, &Accelerator::b_transport);
  }
  void b_transport(tlm::tlm_generic_payload &transaction, sc_core::sc_time &delay);

 private:
  bool is_valid_mmio_transaction(const tlm::tlm_generic_payload &transaction) const;
  bool read_register(uint64_t address, uint32_t *value) const;
  bool write_register(uint64_t address, uint32_t value);
  bool execute();
  bool execute_command(const accel_command_t &command);
  bool execute_batch(const accel_command_t &command);
  bool is_valid_command(const accel_command_t &command) const;
  bool read_i32(uint64_t address, int32_t *value);
  bool write_i32(uint64_t address, int32_t value);
  MemoryInterface &memory_;
  uint64_t descriptor_address_ = 0;
  uint32_t status_ = ACCEL_STATUS_IDLE;
};
