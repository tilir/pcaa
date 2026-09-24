// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Runs Bellman-Ford reductions through pcaalib and the hosted SystemC accelerator.

#include "bellman_ford.h"

#include "accelerator.h"
#include "accel_protocol.h"
#include "memory_interface.h"
#include "pcaa.h"
#include "pcaa_device.h"
#include "pcaa_systemc_device.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <sysc/kernel/sc_dynamic_processes.h>
#include <sysc/kernel/sc_externs.h>
#include <sysc/kernel/sc_module.h>
#include <sysc/kernel/sc_module_name.h>
#include <sysc/kernel/sc_simcontext.h>
#include <sysc/kernel/sc_spawn.h>
#include <sysc/kernel/sc_time.h>
#include <sysc/kernel/sc_wait.h>
#include <tlm_core/tlm_2/tlm_2_interfaces/tlm_fw_bw_ifs.h>
#include <tlm_core/tlm_2/tlm_generic_payload/tlm_phase.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>

#include <gtest/gtest.h>

namespace pcaa::probes {
namespace {

constexpr size_t kGuestMemoryBytes = 1 << 20;
constexpr pcaa_guest_address_t kFirstAllocation = 0x100;
constexpr pcaa_guest_address_t kAlignment = 16;

class GuestMemory final : public MemoryInterface {
 public:
  GuestMemory() : bytes_(kGuestMemoryBytes) {}

  bool read(uint64_t address, void *destination, size_t size) override {
    if (!contains(address, size))
      return false;
    std::memcpy(destination, bytes_.data() + address, size);
    return true;
  }

  bool write(uint64_t address, const void *source, size_t size) override {
    if (!contains(address, size))
      return false;
    std::memcpy(bytes_.data() + address, source, size);
    return true;
  }

  static pcaa_guest_address_t allocate(void *context, size_t size) {
    return static_cast<GuestMemory *>(context)->allocate(size);
  }

  pcaa_guest_address_t allocate(size_t size) {
    const pcaa_guest_address_t address = (next_ + kAlignment - 1) & ~(kAlignment - 1);
    if (!contains(address, size))
      return 0;
    next_ = address + size;
    return address;
  }

 private:
  bool contains(uint64_t address, size_t size) const {
    return address <= bytes_.size() && size <= bytes_.size() - address;
  }

  std::vector<unsigned char> bytes_;
  pcaa_guest_address_t next_ = kFirstAllocation;
};

class Initiator final : public sc_core::sc_module {
 public:
  tlm_utils::simple_initiator_socket<Initiator> socket;

  explicit Initiator(sc_core::sc_module_name name) : sc_core::sc_module(name), socket("socket") {}
};

struct DevicePaths {
  pcaa_status_t status = PCAA_STATUS_OK;
  std::vector<int32_t> distance;
  std::vector<int> predecessor;
  size_t submissions = 0;
};

DevicePaths bellman_ford_on_device(int vertex_count, const std::vector<Edge> &edges, int source,
                                   GuestMemory &memory, pcaa_device_t *device) {
  DevicePaths paths;
  paths.distance.assign(static_cast<size_t>(vertex_count), ACCEL_INF);
  paths.predecessor.assign(static_cast<size_t>(vertex_count), -1);
  paths.distance[static_cast<size_t>(source)] = 0;

  std::vector<std::vector<size_t>> incoming(static_cast<size_t>(vertex_count));
  for (size_t index = 0; index < edges.size(); ++index)
    incoming[static_cast<size_t>(edges[index].to)].push_back(index);

  for (int round = 0; round < vertex_count - 1; ++round) {
    bool changed = false;
    for (int vertex = 0; vertex < vertex_count; ++vertex) {
      const auto &incoming_edges = incoming[static_cast<size_t>(vertex)];
      if (incoming_edges.empty())
        continue;

      std::vector<int32_t> distances;
      std::vector<int32_t> weights;
      distances.reserve(incoming_edges.size());
      weights.reserve(incoming_edges.size());
      for (size_t edge_index : incoming_edges) {
        const Edge &edge = edges[edge_index];
        distances.push_back(paths.distance[static_cast<size_t>(edge.from)]);
        weights.push_back(edge.weight);
      }

      const auto first_address = memory.allocate(distances.size() * sizeof(int32_t));
      const auto second_address = memory.allocate(weights.size() * sizeof(int32_t));
      const auto result_address = memory.allocate(sizeof(accel_min_argmin_result_t));
      if (first_address == 0 || second_address == 0 || result_address == 0 ||
          !memory.write(first_address, distances.data(), distances.size() * sizeof(int32_t)) ||
          !memory.write(second_address, weights.data(), weights.size() * sizeof(int32_t))) {
        paths.status = PCAA_STATUS_MEMORY_ERROR;
        return paths;
      }

      pcaa_command_t command{};
      paths.status = pcaa_make_reduce2(pcaa_cost_vector(first_address, distances.size(), 1),
                                       pcaa_cost_vector(second_address, weights.size(), 1),
                                       result_address, 1, &command);
      if (paths.status != PCAA_STATUS_OK)
        return paths;
      paths.status = pcaa_device_submit(device, &command);
      if (paths.status != PCAA_STATUS_OK)
        return paths;
      ++paths.submissions;
      paths.status = pcaa_device_wait(device, nullptr);
      if (paths.status != PCAA_STATUS_OK)
        return paths;

      accel_min_argmin_result_t reduced{};
      if (!memory.read(result_address, &reduced, sizeof(reduced))) {
        paths.status = PCAA_STATUS_MEMORY_ERROR;
        return paths;
      }
      if (reduced.value < paths.distance[static_cast<size_t>(vertex)]) {
        paths.distance[static_cast<size_t>(vertex)] = reduced.value;
        paths.predecessor[static_cast<size_t>(vertex)] = edges[incoming_edges[reduced.index]].from;
        changed = true;
      }
    }
    if (!changed)
      break;
  }
  return paths;
}

TEST(BellmanFordDevice, IndependentShortestPathsUseRealSubmissions) {
  GuestMemory memory;
  Accelerator accelerator("probe_accelerator", memory);
  Initiator initiator("probe_initiator");
  initiator.socket.bind(accelerator.target_socket);
  PcaaSystemCDevice backend(memory, &memory, GuestMemory::allocate, *initiator.socket.operator->());
  sc_core::sc_start(sc_core::SC_ZERO_TIME);

  const std::vector<std::vector<Edge>> cases = {{{0, 1, 1}, {1, 2, -3}, {2, 3, 4}, {1, 3, 4}},
                                                {{0, 1, 5}},
                                                {{0, 1, 2}, {0, 2, 1}, {1, 3, 1}, {2, 3, 2}},
                                                {{0, 1, 80384},
                                                 {1, 2, 115347},
                                                 {2, 3, 19472},
                                                 {3, 4, 181062},
                                                 {4, 5, 100910},
                                                 {5, 6, 65003},
                                                 {6, 7, 90256},
                                                 {7, 8, 87238}}};
  const std::vector<int> vertex_counts = {4, 3, 4, 9};
  size_t total_submissions = 0;
  for (size_t index = 0; index < cases.size(); ++index) {
    const auto expected = BellmanFord(vertex_counts[index], cases[index], 0);
    ASSERT_FALSE(expected.has_negative_cycle);
    ASSERT_FALSE(expected.distance_saturated);
    const auto actual =
        bellman_ford_on_device(vertex_counts[index], cases[index], 0, memory, backend.device());
    ASSERT_EQ(actual.status, PCAA_STATUS_OK) << index;
    EXPECT_GT(actual.submissions, 0u) << index;
    EXPECT_EQ(actual.distance, expected.distance) << index;
    EXPECT_EQ(actual.predecessor, expected.predecessor) << index;
    total_submissions += actual.submissions;
  }
  EXPECT_GT(total_submissions, 0u);

  // The independent oracle can describe this path, but the device must report
  // finite negative underflow rather than return a saturated shortest path.
  const std::vector<Edge> underflow = {{0, 1, INT32_MIN}, {1, 2, -1}};
  const auto rejected = bellman_ford_on_device(3, underflow, 0, memory, backend.device());
  EXPECT_EQ(rejected.status, PCAA_STATUS_DEVICE_ERROR);
}

}  // namespace
}  // namespace pcaa::probes

int sc_main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
