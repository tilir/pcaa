// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Checks completion diagnostics when guest batch-result reads fail.
#include "pcaa_systemc_device.h"
#include "accel_protocol.h"
#include "accelerator.h"
#include "memory_interface.h"
#include "pcaa.h"
#include "pcaa_device.h"

#include <vector>
#include <string>
#include <map>
#include <initializer_list>
#include <tlm_core/tlm_2/tlm_generic_payload/tlm_phase.h>
#include <tlm_core/tlm_2/tlm_2_interfaces/tlm_fw_bw_ifs.h>
#include <sysc/kernel/sc_wait.h>
#include <sysc/kernel/sc_spawn.h>
#include <sysc/kernel/sc_dynamic_processes.h>
#include <array>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>
#include <sysc/kernel/sc_externs.h>
#include <sysc/kernel/sc_module.h>
#include <sysc/kernel/sc_module_name.h>
#include <sysc/kernel/sc_simcontext.h>
#include <sysc/kernel/sc_time.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>

namespace {
constexpr size_t kMemoryBytes = 4096;
constexpr pcaa_guest_address_t kFirst = 64;
constexpr pcaa_guest_address_t kSecond = 128;
constexpr pcaa_guest_address_t kOutput = 192;
constexpr pcaa_guest_address_t kStaging = 256;

class FaultMemory final : public MemoryInterface {
 public:
  bool fail_reads = false;
  pcaa_guest_address_t next = kStaging;
  bool read(pcaa_guest_address_t address, void *destination, size_t size) override {
    if (fail_reads || address > data_.size() || size > data_.size() - address)
      return false;
    std::memcpy(destination, data_.data() + address, size);
    return true;
  }
  bool write(pcaa_guest_address_t address, const void *source, size_t size) override {
    if (address > data_.size() || size > data_.size() - address)
      return false;
    std::memcpy(data_.data() + address, source, size);
    return true;
  }
  static pcaa_guest_address_t allocate(void *opaque, size_t size) {
    auto *memory = static_cast<FaultMemory *>(opaque);
    const pcaa_guest_address_t address = memory->next;
    memory->next += size;
    return address;
  }

 private:
  std::array<unsigned char, kMemoryBytes> data_{};
};
class Initiator final : public sc_core::sc_module {
 public:
  tlm_utils::simple_initiator_socket<Initiator> socket;
  explicit Initiator(sc_core::sc_module_name name) : sc_core::sc_module(name), socket("socket") {}
};
FaultMemory memory;
pcaa_device_t *device = nullptr;

TEST(SystemCDevice, BatchResultReadFailurePreservesMemoryErrorAndRecovers) {
  for (const int32_t cost : {1, ACCEL_INF + 1}) {
    memory.next = kStaging;
    memory.fail_reads = false;
    const int32_t zero = 0;
    ASSERT_TRUE(memory.write(kFirst, &cost, sizeof(cost)));
    ASSERT_TRUE(memory.write(kSecond, &zero, sizeof(zero)));
    pcaa_command_t command{};
    ASSERT_EQ(pcaa_make_reduce2(pcaa_cost_vector(kFirst, 1, 1), pcaa_cost_vector(kSecond, 1, 1),
                                kOutput, 0, &command),
              PCAA_STATUS_OK);
    ASSERT_EQ(pcaa_device_submit_batch(device, &command, 1), PCAA_STATUS_OK);
    // Doorbell completed at L0. Only the backend's subsequent result read fails.
    memory.fail_reads = true;
    pcaa_completion_t completion{};
    EXPECT_EQ(pcaa_device_wait(device, &completion), PCAA_STATUS_MEMORY_ERROR);
    EXPECT_FALSE(completion.has_batch_result);
    EXPECT_EQ(completion.failed_index, UINT32_MAX);
    EXPECT_EQ(pcaa_device_wait(device, &completion), PCAA_STATUS_NO_PENDING);
    memory.fail_reads = false;
    ASSERT_EQ(pcaa_device_submit_batch(device, &command, 1), PCAA_STATUS_OK);
    EXPECT_EQ(pcaa_device_wait(device, &completion),
              cost == 1 ? PCAA_STATUS_OK : PCAA_STATUS_DEVICE_ERROR);
    EXPECT_TRUE(completion.has_batch_result);
    EXPECT_EQ(completion.completed, cost == 1 ? 1u : 0u);
    EXPECT_EQ(completion.failed_index, cost == 1 ? UINT32_MAX : 0u);
  }
}
}  // namespace

int sc_main(int argc, char **argv) {
  Accelerator accelerator("accelerator", memory);
  Initiator initiator("initiator");
  initiator.socket.bind(accelerator.target_socket);
  PcaaSystemCDevice backend(memory, &memory, FaultMemory::allocate, *initiator.socket.operator->());
  device = backend.device();
  sc_core::sc_start(sc_core::SC_ZERO_TIME);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
