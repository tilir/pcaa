// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Declares the bounded, serial MAS 1.0.0 SystemC execution engine.
#pragma once

#include "accel_protocol.h"
#include "pcaa.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <vector>
#include <map>

#include <sysc/kernel/sc_dynamic_processes.h>
#include <sysc/kernel/sc_event.h>
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

struct L2Config {
  uint64_t mmio_base = 0x10002000;
  size_t lanes = 4;
  size_t mem_bytes = 16;
  size_t tm = 8;
  size_t tn = 16;
  int memory_latency = 1;
  int acceptance_delay = 0;
  sc_core::sc_time cycle_period = sc_core::sc_time(1, sc_core::SC_NS);
  bool valid() const;
};

enum class L2Phase {
  Descriptor,
  Decode,
  Protection,
  Operand,
  Add1,
  Add2,
  Tree,
  Merge,
  Writeback,
  Drain,
  MemoryWait,
  Control,
  Count
};

struct L2Statistics {
  uint64_t cycles = 0;
  std::array<uint64_t, static_cast<size_t>(L2Phase::Count)> phase_cycles{};
  std::array<uint64_t, ACCEL_OPCODE_MINPLUS_MAP3_PROJECT + 1> primitives{};
  std::array<uint64_t, ACCEL_OPCODE_MINPLUS_MAP3_PROJECT + 1> opcode_cycles{};
  uint64_t submissions = 0, batches = 0, children = 0;
  uint64_t tiles = 0, chunks = 0, lane_groups = 0;
  uint64_t active_elements = 0, lane_slots = 0, tail_slots = 0, state_updates = 0;
  uint64_t descriptor_bytes = 0, operand_bytes = 0, result_bytes = 0;
  uint64_t descriptor_requests = 0, operand_requests = 0, result_requests = 0;
  uint64_t header_requests = 0, body_requests = 0;
  uint64_t requests = 0, transferred_bytes = 0, split_transfers = 0, unaligned_splits = 0;
  uint64_t contiguous_transfers = 0, gather_elements = 0;
  uint64_t shared_loads = 0, shared_reread_bytes = 0;
  uint64_t max_outstanding = 0, max_writebacks = 0;
  uint64_t protection_elements = 0, child_barriers = 0;
};

struct L2Diagnostic {
  pcaa_status_t cause = PCAA_STATUS_OK;
  L2Phase phase = L2Phase::Control;
  uint64_t address = 0;
  bool invalid_cost = false, underflow = false;
  size_t child = 0, row = 0, column = 0;
};

// Emits one JSON object; callers own framing and simulation advancement.
void l2_write_json(std::ostream &out, const L2Config &config, const L2Statistics &statistics);

class L2Accelerator : public sc_core::sc_module {
 public:
  tlm_utils::simple_target_socket<L2Accelerator> target_socket;
  SC_HAS_PROCESS(L2Accelerator);
  L2Accelerator(sc_core::sc_module_name name, MemoryInterface &memory, L2Config config = {});
  void b_transport(tlm::tlm_generic_payload &transaction, sc_core::sc_time &delay);
  const L2Statistics &statistics() const {
    return statistics_;
  }
  const L2Diagnostic &diagnostic() const {
    return diagnostic_;
  }
  const L2Config &config() const {
    return config_;
  }
  void reset_statistics();

 private:
  void run();
  void tick(L2Phase phase, int cycles = 1);
  bool fail(pcaa_status_t cause, uint64_t address = 0);
  bool transfer(bool write, uint64_t address, unsigned char *data, size_t bytes, L2Phase phase);
  bool fetch(uint64_t address, size_t available, size_t *bytes);
  bool batch(size_t parent_bytes);
  bool protect(uint64_t begin, uint64_t end, uint64_t parent, size_t parent_bytes);
  bool primitive();
  bool load(pcaa_cost_vector_view_t view, size_t start, size_t count, size_t bank);
  bool add(size_t left, size_t right, size_t start, size_t count, L2Phase phase);
  void reduce(size_t start, size_t count, size_t state);
  bool record(uint64_t address, uint32_t completed, uint32_t failed);
  pcaa_output_view_t output_view() const;

  MemoryInterface &memory_;
  L2Config config_;
  L2Statistics statistics_;
  L2Diagnostic diagnostic_;
  sc_core::sc_event doorbell_;
  uint64_t descriptor_register_ = 0, active_descriptor_ = 0;
  uint32_t status_ = ACCEL_STATUS_IDLE;
  L2Phase phase_ = L2Phase::Control;
  int opcode_ = 0;
  std::array<unsigned char, ACCEL_COMMAND_MAX_BYTES> descriptor_{};
  size_t descriptor_fill_ = 0;
  pcaa_command_t command_{};
  pcaa_batch_reference_t parent_{};
  uint64_t cursor_ = 0, stream_end_ = 0;
  size_t child_ = 0, tile_ = 0, chunk_ = 0, row_ = 0, group_ = 0;
  std::array<std::vector<int32_t>, 3> banks_;
  std::array<size_t, 3> bank_valid_{};
  std::vector<accel_min_argmin_result_t> states_;
  std::vector<accel_min_argmin_result_t> lanes_;
  size_t lane_valid_ = 0;
  std::array<unsigned char, sizeof(accel_min_argmin_result_t)> pending_write_{};
  bool outstanding_ = false, writeback_valid_ = false, request_valid_ = false;
  uint64_t request_address_ = 0;
  size_t request_bytes_ = 0;
  std::vector<unsigned char> request_data_, fill_data_;
};
