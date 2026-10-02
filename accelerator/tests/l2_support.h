// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Provides guest RAM and real socket submission for L2 tests and microbenchmarks.
#pragma once
#include "accelerator.h"
#include "l2_accelerator.h"
#include "memory_interface.h"
#include "pcaa_codec.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>

namespace l2_test {
constexpr uint64_t kDescriptor = 0x100;
constexpr uint64_t kChildren = 0x200;
constexpr uint64_t kBatchResult = 0x8000;
constexpr uint64_t kA = 0x10000, kB = 0x100000, kC = 0x200000, kOut = 0x500000;
constexpr size_t kMemorySize = 8 * 1024 * 1024;
constexpr int kPollCycles = 64;

class Ram : public MemoryInterface {
 public:
  std::vector<unsigned char> data = std::vector<unsigned char>(kMemorySize);
  uint64_t fail_begin = 0, fail_end = 0;
  bool fail_read = false, fail_write = false;
  uint64_t reads = 0, writes = 0;
  bool access(uint64_t address, size_t size, bool write) {
    if (write)
      ++writes;
    else
      ++reads;
    return address <= data.size() && size <= data.size() - address &&
           !((write ? fail_write : fail_read) && address < fail_end && fail_begin < address + size);
  }
  bool read(uint64_t address, void *dst, size_t size) override {
    if (!access(address, size, false))
      return false;
    std::memcpy(dst, data.data() + address, size);
    return true;
  }
  bool write(uint64_t address, const void *src, size_t size) override {
    if (!access(address, size, true))
      return false;
    std::memcpy(data.data() + address, src, size);
    return true;
  }
  void cost(uint64_t address, int32_t value) {
    std::memcpy(data.data() + address, &value, sizeof(value));
  }
  int32_t cost(uint64_t address) const {
    int32_t value = 0;
    std::memcpy(&value, data.data() + address, sizeof(value));
    return value;
  }
  void encode(const pcaa_command_t &command, uint64_t address = kDescriptor) {
    size_t bytes = 0;
    if (pcaa_encode_one(&command, data.data() + address, ACCEL_COMMAND_MAX_BYTES, &bytes) !=
        PCAA_STATUS_OK)
      throw std::runtime_error("fixture encode failed");
  }
};
class Port : public sc_core::sc_module {
 public:
  tlm_utils::simple_initiator_socket<Port> socket;
  explicit Port(sc_core::sc_module_name name) : sc_core::sc_module(name), socket("socket") {}
  bool mmio(uint64_t offset, uint32_t *value, bool write) {
    tlm::tlm_generic_payload t;
    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    t.set_command(write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
    t.set_address(offset);
    t.set_data_ptr(reinterpret_cast<unsigned char *>(value));
    t.set_data_length(sizeof(*value));
    t.set_streaming_width(sizeof(*value));
    socket->b_transport(t, delay);
    return t.is_response_ok();
  }
  bool start(uint64_t address = kDescriptor) {
    uint32_t lo = static_cast<uint32_t>(address), hi = static_cast<uint32_t>(address >> 32),
             bell = 1;
    return mmio(ACCEL_MMIO_DESC_ADDR_LO, &lo, true) && mmio(ACCEL_MMIO_DESC_ADDR_HI, &hi, true) &&
           mmio(ACCEL_MMIO_DOORBELL, &bell, true);
  }
  uint32_t status() {
    uint32_t value = 0;
    if (!mmio(ACCEL_MMIO_STATUS, &value, false))
      throw std::runtime_error("status MMIO failed");
    return value;
  }
  uint32_t finish(const L2Config &config) {
    while (status() == ACCEL_STATUS_BUSY) sc_core::sc_start(config.cycle_period * kPollCycles);
    return status();
  }
};
class Rig {
 public:
  Ram reference, memory;
  Accelerator l0;
  L2Accelerator l2;
  Port p0, p2;
  explicit Rig(L2Config config = {})
      : l0(sc_core::sc_gen_unique_name("l0"), reference),
        l2(sc_core::sc_gen_unique_name("l2"), memory, config),
        p0(sc_core::sc_gen_unique_name("p0")),
        p2(sc_core::sc_gen_unique_name("p2")) {
    p0.socket.bind(l0.target_socket);
    p2.socket.bind(l2.target_socket);
  }
  uint32_t run(uint64_t address = kDescriptor) {
    if (!p2.start(address))
      throw std::runtime_error("submission failed");
    return p2.finish(l2.config());
  }
  bool differential(uint64_t address = kDescriptor) {
    reference.data = memory.data;
    reference.fail_begin = memory.fail_begin;
    reference.fail_end = memory.fail_end;
    reference.fail_read = memory.fail_read;
    reference.fail_write = memory.fail_write;
    if (!p0.start(address))
      throw std::runtime_error("reference submit failed");
    const uint32_t status = run(address);
    return status == p0.status() && (status != ACCEL_STATUS_DONE || reference.data == memory.data);
  }
};
inline pcaa_command_t command(int op, size_t n, size_t m = 1, size_t inner = 1, size_t outer = 0,
                              size_t vector_stride = 1, size_t output_stride = 1, uint64_t skew = 0,
                              bool inplace = false) {
  pcaa_command_t c{};
  const auto a = pcaa_cost_vector(kA + skew, n, vector_stride);
  const auto b = pcaa_cost_vector(kB + skew, n, vector_stride);
  const auto third = pcaa_cost_vector(kC + skew, n, 1);
  const auto matrix = pcaa_cost_matrix(kC + skew, m, n, outer ? outer : n * inner, inner);
  pcaa_status_t status = PCAA_STATUS_INVALID_ARGUMENT;
  switch (op) {
    case 1:
    case 3:
      status = pcaa_make_reduce2(pcaa_cost_vector(kA + skew, n, 1),
                                 pcaa_cost_vector(kB + skew, n, 1), kOut + skew, op == 3, &c);
      break;
    case 2:
    case 4:
      status =
          pcaa_make_reduce3(pcaa_cost_vector(kA + skew, n, 1), pcaa_cost_vector(kB + skew, n, 1),
                            third, kOut + skew, op == 4, &c);
      break;
    case 6:
      status = pcaa_make_cost_add_vector(a, b,
                                         pcaa_cost_output(inplace ? kA + skew : kOut + skew, n,
                                                          inplace ? vector_stride : output_stride),
                                         &c);
      break;
    case 7:
      status =
          pcaa_make_minplus_project(matrix, b, pcaa_cost_output(kOut + skew, m, output_stride), &c);
      break;
    case 8:
      status = pcaa_make_minplus_map3_project(
          a, b, matrix, pcaa_argmin_output(kOut + skew, m, output_stride), &c);
      break;
  }
  if (status != PCAA_STATUS_OK)
    throw std::runtime_error("fixture builder failed");
  return c;
}
inline void fill(Ram &memory, size_t n, size_t m, size_t inner = 1, size_t outer = 0,
                 size_t stride = 1, uint64_t skew = 0) {
  for (size_t j = 0; j < n; ++j) {
    memory.cost(kA + skew + j * stride * sizeof(int32_t), static_cast<int>(j % 7) - 3);
    memory.cost(kB + skew + j * stride * sizeof(int32_t), static_cast<int>(j % 5) - 2);
  }
  for (size_t i = 0; i < m; ++i)
    for (size_t j = 0; j < n; ++j)
      memory.cost(kC + skew + (i * (outer ? outer : n * inner) + j * inner) * sizeof(int32_t),
                  static_cast<int>((j + i) % 11) - 5);
}
}  // namespace l2_test
