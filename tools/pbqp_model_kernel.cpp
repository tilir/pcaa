// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements hosted PBQP callbacks with guest staging through the SystemC socket.

#include "pbqp_model_kernel.h"
#include "accelerator.h"
#include "accel_protocol.h"
#include "memory_interface.h"
#include "l2_accelerator.h"
#include "pbqp/pbqp.h"
#include "pcaa.h"
#include "pcaa_device.h"
#include "pcaa_codec.h"
#include "pcaa_host_error.h"
#include "pcaa_systemc_device.h"
#include "timing_model.h"

#include <algorithm>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <utility>
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
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>

namespace {
constexpr size_t kInitialGuestMemoryBytes = 1024 * 1024;
constexpr unsigned kTimedRunnerLanes = 4;
constexpr int kL2PollCycles = 64;
constexpr unsigned kTimedRunnerBytesPerCycle = 16;
constexpr pcaa_guest_address_t kFirstAllocationAddress = 0x100;
constexpr size_t kAllocationAlignment = 8;
}  // namespace
class GuestMemory final : public MemoryInterface {
 public:
  explicit GuestMemory(size_t limit)
      : limit_(limit), bytes_(std::min(limit, kInitialGuestMemoryBytes)) {}

  bool read(pcaa_guest_address_t address, void *destination, size_t size) override {
    if (!contains(address, size)) {
      return false;
    }
    std::memcpy(destination, bytes_.data() + address, size);
    return true;
  }

  bool write(pcaa_guest_address_t address, const void *source, size_t size) override {
    if (!contains(address, size)) {
      return false;
    }
    std::memcpy(bytes_.data() + address, source, size);
    return true;
  }

  pcaa_guest_address_t allocate(size_t size, size_t alignment = kAllocationAlignment) {
    const pcaa_guest_address_t aligned = (next_address_ + alignment - 1) / alignment * alignment;
    if (aligned > std::numeric_limits<pcaa_guest_address_t>::max() - size) {
      return 0;
    }
    const pcaa_guest_address_t end = aligned + size;
    if (end > std::numeric_limits<size_t>::max() || end > limit_) {
      return 0;
    }
    if (end > bytes_.size()) {
      try {
        bytes_.resize(static_cast<size_t>(end));
      } catch (const std::exception &) {
        return 0;
      }
    }
    next_address_ = end;
    return aligned;
  }

  void reset() {
    next_address_ = kFirstAllocationAddress;
  }

 private:
  bool contains(pcaa_guest_address_t address, size_t size) const {
    return address <= bytes_.size() && size <= bytes_.size() - address;
  }

  size_t limit_;
  std::vector<unsigned char> bytes_;
  pcaa_guest_address_t next_address_ = kFirstAllocationAddress;
};

class Initiator final : public sc_core::sc_module {
 public:
  tlm_utils::simple_initiator_socket<Initiator> socket;

  explicit Initiator(sc_core::sc_module_name name) : sc_core::sc_module(name), socket("socket") {}
};

class ModelKernel::Impl {
 public:
  Impl(bool verbose, AccelTimingConfig timing, size_t staging_limit, const L2Config *l2)
      : memory_(staging_limit), initiator_(sc_core::sc_gen_unique_name("initiator")) {
    if (l2) {
      l2_ = std::make_unique<L2Accelerator>(sc_core::sc_gen_unique_name("l2"), memory_, *l2);
      initiator_.socket.bind(l2_->target_socket);
    } else {
      accelerator_ = std::make_unique<Accelerator>(sc_core::sc_gen_unique_name("accelerator"),
                                                   memory_, timing, verbose);
      initiator_.socket.bind(accelerator_->target_socket);
    }
    device_ = std::make_unique<PcaaSystemCDevice>(memory_, &memory_, allocate_device_storage,
                                                  *initiator_.socket.operator->());
  }

  void make_kernel(pbqp_cost_kernel_t &kernel) {
    kernel.context = this;
    kernel.min2_argmin = min2;
    kernel.min3_argmin = min3;
    kernel.min2_argmin_batch = min2_batch;
    kernel.min3_argmin_batch = min3_batch;
    kernel.min2_value = min2_value;
    kernel.min2_value_batch = min2_value_batch;
    kernel.cost_add_vector = cost_add_vector;
    kernel.minplus_project = minplus_project;
    kernel.minplus_map3_project = map3_project;
    kernel.project_add_batch = project_add_batch;
    kernel.map3_project_batch = map3_project_batch;
    kernel.set_statistics = set_statistics;
  }

  const AccelTimingStatistics &timing_statistics() const {
    return accelerator_ ? accelerator_->timing_statistics() : untimed_;
  }

  const L2Statistics &l2_statistics() const {
    return l2_->statistics();
  }

  const VectorCycleProjection &vector_cycle_projection() const {
    return vector_cycle_projection_;
  }

 private:
  static uint64_t now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  uint64_t cycles() const {
    return l2_ ? l2_->statistics().cycles : timing_statistics().total_service_cycles;
  }
  class HostTimer {
   public:
    explicit HostTimer(uint64_t *total) : total_(total), start_(total ? now() : 0) {}
    ~HostTimer() {
      if (total_)
        *total_ += now() - start_;
    }

   private:
    uint64_t *total_;
    uint64_t start_;
  };
  class Measurement {
   public:
    Measurement(Impl &owner, const char *kind, size_t count)
        : owner_(owner), kind_(kind), count_(count) {
      if (owner_.measure_) {
        owner_.staging_ns_ = owner_.wait_ns_ = owner_.submission_ns_ = owner_.readback_start_ = 0;
        cycles_ = owner_.cycles();
        start_ = now();
      }
    }
    ~Measurement() {
      if (owner_.measure_) {
        const auto end = now();
        owner_.measurements_.push_back({kind_, count_, owner_.cycles() - cycles_,
                                        end - start_ - owner_.wait_ns_, owner_.staging_ns_,
                                        owner_.submission_ns_,
                                        owner_.readback_start_ ? end - owner_.readback_start_ : 0});
      }
    }

   private:
    Impl &owner_;
    const char *kind_;
    size_t count_;
    uint64_t start_ = 0, cycles_ = 0;
  };
  static pcaa_guest_address_t allocate_device_storage(void *context, size_t size) {
    return static_cast<GuestMemory *>(context)->allocate(size);
  }

  struct ViewKey {
    const int32_t *base;
    size_t length;
    size_t stride;
  };

  struct ViewKeyLess {
    bool operator()(const ViewKey &left, const ViewKey &right) const {
      if (left.base != right.base) {
        return std::less<const int32_t *>{}(left.base, right.base);
      }
      if (left.length != right.length) {
        return left.length < right.length;
      }
      return left.stride < right.stride;
    }
  };

  static uint64_t divide_round_up(uint64_t value, uint64_t divisor) {
    return (value + divisor - 1) / divisor;
  }

  static void record_cycles(CycleBreakdown *breakdown, size_t descriptor_bytes,
                            uint64_t operand_elements, uint64_t compute_chunks,
                            uint64_t result_bytes) {
    const uint64_t descriptor = divide_round_up(descriptor_bytes, kTimedRunnerBytesPerCycle);
    const uint64_t operands =
        divide_round_up(operand_elements * sizeof(int32_t), kTimedRunnerBytesPerCycle);
    const uint64_t result = divide_round_up(result_bytes, kTimedRunnerBytesPerCycle);
    breakdown->descriptor += descriptor;
    breakdown->operands += operands;
    breakdown->compute += compute_chunks;
    breakdown->result += result;
    breakdown->total += descriptor + std::max(operands, compute_chunks) + result;
    ++breakdown->descriptors;
  }

  void record_project_cycle_projection(const pbqp_min2_value_job_t *jobs, size_t count) {
    struct Group {
      uint64_t operand_elements = 0;
      uint64_t compute_chunks = 0;
      uint64_t outputs = 0;
    };
    std::map<ViewKey, Group, ViewKeyLess> groups;
    for (size_t index = 0; index < count; ++index) {
      const pbqp_vector_view_t matrix = jobs[index].a;
      const pbqp_vector_view_t unary = jobs[index].b;
      record_cycles(&vector_cycle_projection_.project_scalar, 32, 2 * matrix.length,
                    divide_round_up(matrix.length, kTimedRunnerLanes), sizeof(int32_t));
      Group &group = groups[{unary.base, unary.length, unary.stride}];
      if (group.outputs == 0) {
        group.operand_elements = unary.length;
      }
      group.operand_elements += matrix.length;
      group.compute_chunks += divide_round_up(matrix.length, kTimedRunnerLanes);
      ++group.outputs;
    }
    for (const auto &entry : groups) {
      const Group &group = entry.second;
      record_cycles(&vector_cycle_projection_.project_vector, 48, group.operand_elements,
                    group.compute_chunks, group.outputs * sizeof(int32_t));
    }
  }

  void record_map3_cycle_projection(const pbqp_min3_job_t *jobs, size_t count) {
    if (count == 0) {
      return;
    }
    std::map<ViewKey, size_t, ViewKeyLess> first_slices;
    std::map<ViewKey, size_t, ViewKeyLess> second_slices;
    for (size_t index = 0; index < count; ++index) {
      const size_t length = jobs[index].a.length;
      record_cycles(&vector_cycle_projection_.map3_scalar, 48, 3 * length,
                    divide_round_up(length, kTimedRunnerLanes), sizeof(accel_min_argmin_result_t));
      first_slices.emplace(ViewKey{jobs[index].b.base, jobs[index].b.length, jobs[index].b.stride},
                           length);
      second_slices.emplace(ViewKey{jobs[index].c.base, jobs[index].c.length, jobs[index].c.stride},
                            length);
    }
    const uint64_t reduction_chunks = divide_round_up(jobs[0].a.length, kTimedRunnerLanes);
    uint64_t second_elements = 0;
    for (const auto &entry : second_slices) {
      second_elements += entry.second;
    }
    for (const auto &entry : first_slices) {
      const uint64_t operand_elements = jobs[0].a.length + entry.second + second_elements;
      record_cycles(&vector_cycle_projection_.map3_partial, 64, operand_elements,
                    second_slices.size() * reduction_chunks,
                    second_slices.size() * sizeof(accel_min_argmin_result_t));
    }
    uint64_t first_elements = 0;
    for (const auto &entry : first_slices) {
      first_elements += entry.second;
    }
    record_cycles(&vector_cycle_projection_.map3_full, 64,
                  jobs[0].a.length + first_elements + second_elements, count * reduction_chunks,
                  count * sizeof(accel_min_argmin_result_t));
  }

  void record_vector_project_projection(const pbqp_project_add_job_t *jobs, size_t count) {
    for (size_t index = 0; index < count; ++index) {
      const uint64_t columns = jobs[index].matrix.columns;
      const uint64_t rows = jobs[index].matrix.rows;
      const uint64_t chunks = divide_round_up(columns, kTimedRunnerLanes);
      for (size_t row = 0; row < rows; ++row)
        record_cycles(&vector_cycle_projection_.project_scalar, 32, 2 * columns, chunks,
                      sizeof(int32_t));
      record_cycles(&vector_cycle_projection_.project_vector, 48, columns * (rows + 1),
                    rows * chunks, rows * sizeof(int32_t));
    }
  }

  void record_vector_map3_projection(const pbqp_map3_project_job_t *jobs, size_t count) {
    uint64_t full_operands = 0;
    uint64_t full_chunks = 0;
    uint64_t full_results = 0;
    for (size_t index = 0; index < count; ++index) {
      const uint64_t columns = jobs[index].varying_edge.columns;
      const uint64_t rows = jobs[index].varying_edge.rows;
      const uint64_t chunks = divide_round_up(columns, kTimedRunnerLanes);
      for (size_t row = 0; row < rows; ++row)
        record_cycles(&vector_cycle_projection_.map3_scalar, 48, 3 * columns, chunks,
                      sizeof(accel_min_argmin_result_t));
      record_cycles(&vector_cycle_projection_.map3_partial, 64, columns * (rows + 2), rows * chunks,
                    rows * sizeof(accel_min_argmin_result_t));
      full_operands += columns * (rows + 1);
      full_chunks += rows * chunks;
      full_results += rows * sizeof(accel_min_argmin_result_t);
    }
    if (count != 0)
      record_cycles(&vector_cycle_projection_.map3_full, 64, full_operands, full_chunks,
                    full_results);
  }

  static int min2(void *opaque, pbqp_vector_view_t first, pbqp_vector_view_t second,
                  accel_min_argmin_result_t *result) {
    const pbqp_min2_job_t job = {first, second, result};
    return min2_batch(opaque, &job, 1);
  }

  static void set_statistics(void *opaque, pbqp_statistics_t *statistics) {
    static_cast<Impl *>(opaque)->statistics_ = statistics;
  }

  static int min3(void *opaque, pbqp_vector_view_t first, pbqp_vector_view_t second,
                  pbqp_vector_view_t third, accel_min_argmin_result_t *result) {
    const pbqp_min3_job_t job = {first, second, third, result};
    return min3_batch(opaque, &job, 1);
  }

  static int min2_value(void *opaque, pbqp_vector_view_t first, pbqp_vector_view_t second,
                        int32_t *result) {
    const pbqp_min2_value_job_t job = {first, second, result};
    return min2_value_batch(opaque, &job, 1);
  }

  static int min2_value_batch(void *opaque, const pbqp_min2_value_job_t *jobs, size_t count) {
    Impl *kernel = static_cast<Impl *>(opaque);
    Measurement measurement(*kernel, "min2_value_batch", count);
    kernel->record_project_cycle_projection(jobs, count);
    kernel->begin_batch();
    std::vector<pcaa_command_t> commands;
    std::vector<pcaa_guest_address_t> result_addresses;
    commands.reserve(count);
    result_addresses.reserve(count);
    for (size_t index = 0; index < count; ++index) {
      const pcaa_guest_address_t first = kernel->copy_view(jobs[index].a);
      const pcaa_guest_address_t second = kernel->copy_view(jobs[index].b);
      const pcaa_guest_address_t result = kernel->allocate_storage(sizeof(int32_t));
      if (first == 0 || second == 0 || result == 0)
        return kernel->staging_status_;
      pcaa_command_t command{};
      const pcaa_status_t built =
          pcaa_make_reduce2(pcaa_cost_vector(first, jobs[index].a.length, 1),
                            pcaa_cost_vector(second, jobs[index].b.length, 1), result, 0, &command);
      if (built != PCAA_STATUS_OK)
        return built;
      commands.push_back(command);
      result_addresses.push_back(result);
    }
    const pcaa_status_t submitted = kernel->submit_batch(commands);
    if (submitted != PCAA_STATUS_OK)
      return submitted;
    for (size_t index = 0; index < count; ++index) {
      if (!kernel->memory_.read(result_addresses[index], jobs[index].result,
                                sizeof(*jobs[index].result)))
        return PCAA_STATUS_MEMORY_ERROR;
    }
    return 0;
  }

  static int min2_batch(void *opaque, const pbqp_min2_job_t *jobs, size_t count) {
    Impl *kernel = static_cast<Impl *>(opaque);
    Measurement measurement(*kernel, "min2_batch", count);
    kernel->begin_batch();
    std::vector<pcaa_command_t> commands;
    std::vector<pcaa_guest_address_t> result_addresses;
    commands.reserve(count);
    result_addresses.reserve(count);
    for (size_t index = 0; index < count; ++index) {
      const pcaa_guest_address_t first = kernel->copy_view(jobs[index].a);
      const pcaa_guest_address_t second = kernel->copy_view(jobs[index].b);
      const pcaa_guest_address_t result = kernel->allocate_result();
      if (first == 0 || second == 0 || result == 0) {
        return kernel->staging_status_;
      }
      pcaa_command_t command{};
      const pcaa_status_t built =
          pcaa_make_reduce2(pcaa_cost_vector(first, jobs[index].a.length, 1),
                            pcaa_cost_vector(second, jobs[index].b.length, 1), result, 1, &command);
      if (built != PCAA_STATUS_OK)
        return built;
      commands.push_back(command);
      result_addresses.push_back(result);
    }
    const pcaa_status_t submitted = kernel->submit_batch(commands);
    if (submitted != PCAA_STATUS_OK) {
      return submitted;
    }
    for (size_t index = 0; index < count; ++index) {
      if (!kernel->memory_.read(result_addresses[index], jobs[index].result,
                                sizeof(*jobs[index].result))) {
        return PCAA_STATUS_MEMORY_ERROR;
      }
    }
    return 0;
  }

  static int min3_batch(void *opaque, const pbqp_min3_job_t *jobs, size_t count) {
    Impl *kernel = static_cast<Impl *>(opaque);
    Measurement measurement(*kernel, "min3_batch", count);
    kernel->record_map3_cycle_projection(jobs, count);
    kernel->begin_batch();
    std::vector<pcaa_command_t> commands;
    std::vector<pcaa_guest_address_t> result_addresses;
    commands.reserve(count);
    result_addresses.reserve(count);
    for (size_t index = 0; index < count; ++index) {
      const pcaa_guest_address_t first = kernel->copy_view(jobs[index].a);
      const pcaa_guest_address_t second = kernel->copy_view(jobs[index].b);
      const pcaa_guest_address_t third = kernel->copy_view(jobs[index].c);
      const pcaa_guest_address_t result = kernel->allocate_result();
      if (first == 0 || second == 0 || third == 0 || result == 0) {
        return kernel->staging_status_;
      }
      pcaa_command_t command{};
      const pcaa_status_t built =
          pcaa_make_reduce3(pcaa_cost_vector(first, jobs[index].a.length, 1),
                            pcaa_cost_vector(second, jobs[index].b.length, 1),
                            pcaa_cost_vector(third, jobs[index].c.length, 1), result, 1, &command);
      if (built != PCAA_STATUS_OK)
        return built;
      commands.push_back(command);
      result_addresses.push_back(result);
    }
    const pcaa_status_t submitted = kernel->submit_batch(commands);
    if (submitted != PCAA_STATUS_OK) {
      return submitted;
    }
    for (size_t index = 0; index < count; ++index) {
      if (!kernel->memory_.read(result_addresses[index], jobs[index].result,
                                sizeof(*jobs[index].result))) {
        return PCAA_STATUS_MEMORY_ERROR;
      }
    }
    return 0;
  }

  static int project_add_batch(void *opaque, const pbqp_project_add_job_t *jobs, size_t count) {
    Impl *kernel = static_cast<Impl *>(opaque);
    Measurement measurement(*kernel, "project_add_batch", count);
    if (count == 0)
      return 0;
    kernel->record_vector_project_projection(jobs, count);
    kernel->begin_batch();
    std::vector<pcaa_command_t> commands;
    commands.reserve(2 * count);
    std::vector<pcaa_guest_address_t> score_addresses;
    score_addresses.reserve(count);
    for (size_t index = 0; index < count; ++index) {
      const pbqp_project_add_job_t &job = jobs[index];
      const size_t rows = job.matrix.rows;
      const pcaa_guest_address_t scores = kernel->copy_view({job.scores, rows, 1});
      if (scores == 0)
        return kernel->staging_status_;
      score_addresses.push_back(scores);
      const pcaa_guest_address_t matrix = kernel->copy_matrix(job.matrix);
      const pcaa_guest_address_t unary = kernel->copy_view(job.unary);
      const pcaa_guest_address_t temporary = kernel->allocate_storage(rows * sizeof(int32_t));
      if (matrix == 0 || unary == 0 || temporary == 0)
        return kernel->staging_status_;
      pcaa_command_t project{};
      const pcaa_status_t built = pcaa_make_minplus_project(
          pcaa_cost_matrix(matrix, rows, job.matrix.columns, job.matrix.columns, 1),
          pcaa_cost_vector(unary, job.unary.length, 1), pcaa_cost_output(temporary, rows, 1),
          &project);
      if (built != PCAA_STATUS_OK)
        return built;
      commands.push_back(project);
      pcaa_command_t add{};
      const pcaa_status_t add_status = pcaa_make_cost_add_vector(
          pcaa_cost_vector(temporary, rows, 1), pcaa_cost_vector(scores, rows, 1),
          pcaa_cost_output(scores, rows, 1), &add);
      if (add_status != PCAA_STATUS_OK)
        return add_status;
      commands.push_back(add);
    }
    const pcaa_status_t submitted = kernel->submit_batch(commands);
    if (submitted != PCAA_STATUS_OK)
      return submitted;
    for (size_t index = 0; index < count; ++index) {
      if (!kernel->memory_.read(score_addresses[index], jobs[index].scores,
                                jobs[index].matrix.rows * sizeof(int32_t)))
        return PCAA_STATUS_MEMORY_ERROR;
    }
    return PCAA_STATUS_OK;
  }

  static int map3_project_batch(void *opaque, const pbqp_map3_project_job_t *jobs, size_t count) {
    Impl *kernel = static_cast<Impl *>(opaque);
    Measurement measurement(*kernel, "map3_project_batch", count);
    if (count == 0)
      return 0;
    kernel->record_vector_map3_projection(jobs, count);
    kernel->begin_batch();
    std::vector<pcaa_command_t> commands;
    std::vector<pcaa_guest_address_t> results;
    commands.reserve(count);
    results.reserve(count);
    for (size_t index = 0; index < count; ++index) {
      const pbqp_map3_project_job_t &job = jobs[index];
      const pcaa_guest_address_t unary = kernel->copy_view(job.unary);
      const pcaa_guest_address_t fixed = kernel->copy_view(job.fixed_edge);
      const pcaa_guest_address_t varying = kernel->copy_matrix(job.varying_edge);
      const pcaa_guest_address_t result =
          kernel->allocate_storage(job.varying_edge.rows * sizeof(accel_min_argmin_result_t));
      if (unary == 0 || fixed == 0 || varying == 0 || result == 0)
        return kernel->staging_status_;
      pcaa_command_t command{};
      const pcaa_status_t built = pcaa_make_minplus_map3_project(
          pcaa_cost_vector(unary, job.unary.length, 1),
          pcaa_cost_vector(fixed, job.fixed_edge.length, 1),
          pcaa_cost_matrix(varying, job.varying_edge.rows, job.varying_edge.columns,
                           job.varying_edge.columns, 1),
          pcaa_argmin_output(result, job.varying_edge.rows, 1), &command);
      if (built != PCAA_STATUS_OK)
        return built;
      commands.push_back(command);
      results.push_back(result);
    }
    const pcaa_status_t submitted = kernel->submit_batch(commands);
    if (submitted != PCAA_STATUS_OK)
      return submitted;
    for (size_t index = 0; index < count; ++index) {
      if (!kernel->memory_.read(results[index], jobs[index].results,
                                jobs[index].varying_edge.rows * sizeof(accel_min_argmin_result_t)))
        return PCAA_STATUS_MEMORY_ERROR;
    }
    return 0;
  }

  static int cost_add_vector(void *opaque, pbqp_vector_view_t first, pbqp_vector_view_t second,
                             int32_t *result) {
    Impl *kernel = static_cast<Impl *>(opaque);
    Measurement measurement(*kernel, "add", 1);
    kernel->begin_batch();
    const pcaa_guest_address_t first_address = kernel->copy_view(first);
    const pcaa_guest_address_t second_address = kernel->copy_view(second);
    const pcaa_guest_address_t result_address =
        kernel->allocate_storage(first.length * sizeof(int32_t));
    if (first_address == 0 || second_address == 0 || result_address == 0)
      return kernel->staging_status_;
    pcaa_command_t command{};
    const pcaa_status_t built =
        pcaa_make_cost_add_vector(pcaa_cost_vector(first_address, first.length, 1),
                                  pcaa_cost_vector(second_address, second.length, 1),
                                  pcaa_cost_output(result_address, first.length, 1), &command);
    if (built != PCAA_STATUS_OK)
      return built;
    const pcaa_status_t submitted = kernel->submit_batch({command});
    if (submitted != PCAA_STATUS_OK)
      return submitted;
    return kernel->memory_.read(result_address, result, first.length * sizeof(int32_t))
               ? PCAA_STATUS_OK
               : PCAA_STATUS_MEMORY_ERROR;
  }

  static int minplus_project(void *opaque, pbqp_matrix_view_t matrix, pbqp_vector_view_t unary,
                             int32_t *result) {
    Impl *kernel = static_cast<Impl *>(opaque);
    Measurement measurement(*kernel, "project", 1);
    kernel->begin_batch();
    const pcaa_guest_address_t matrix_address = kernel->copy_matrix(matrix);
    const pcaa_guest_address_t unary_address = kernel->copy_view(unary);
    const pcaa_guest_address_t result_address =
        kernel->allocate_storage(matrix.rows * sizeof(int32_t));
    if (matrix_address == 0 || unary_address == 0 || result_address == 0)
      return kernel->staging_status_;
    pcaa_command_t command{};
    const pcaa_status_t built = pcaa_make_minplus_project(
        pcaa_cost_matrix(matrix_address, matrix.rows, matrix.columns, matrix.columns, 1),
        pcaa_cost_vector(unary_address, unary.length, 1),
        pcaa_cost_output(result_address, matrix.rows, 1), &command);
    if (built != PCAA_STATUS_OK)
      return built;
    const pcaa_status_t submitted = kernel->submit_batch({command});
    if (submitted != PCAA_STATUS_OK)
      return submitted;
    return kernel->memory_.read(result_address, result, matrix.rows * sizeof(int32_t))
               ? PCAA_STATUS_OK
               : PCAA_STATUS_MEMORY_ERROR;
  }

  static int map3_project(void *opaque, pbqp_vector_view_t unary, pbqp_vector_view_t fixed,
                          pbqp_matrix_view_t varying, accel_min_argmin_result_t *result) {
    const pbqp_map3_project_job_t job{unary, fixed, varying, result};
    return map3_project_batch(opaque, &job, 1);
  }

  // Starts a new staging-area lifetime: every view copied below stays valid
  // until the next batch, so identical views within one batch share one copy.
  void begin_batch() {
    memory_.reset();
    view_cache_.clear();
    staging_status_ = PCAA_STATUS_OK;
  }

  // Copies a strided view into guest memory once per batch. R2 alone submits
  // D^2 primitives that repeat one unary and 2*D matrix slices, so copying
  // each operand separately would exhaust the staging area at moderate D.
  pcaa_guest_address_t copy_view(pbqp_vector_view_t view) {
    HostTimer timer(measure_ ? &staging_ns_ : nullptr);
    const ViewKey key{view.base, view.length, view.stride};
    const auto cached = view_cache_.find(key);
    if (cached != view_cache_.end()) {
      return cached->second;
    }
    if (view.length > std::numeric_limits<size_t>::max() / sizeof(int32_t)) {
      staging_status_ = PCAA_STATUS_RANGE;
      return 0;
    }
    const pcaa_guest_address_t address = allocate_storage(view.length * sizeof(int32_t));
    if (address == 0) {
      return 0;
    }
    for (size_t index = 0; index < view.length; ++index) {
      if (!memory_.write(address + index * sizeof(int32_t), &view.base[index * view.stride],
                         sizeof(int32_t))) {
        staging_status_ = PCAA_STATUS_MEMORY_ERROR;
        return 0;
      }
    }
    view_cache_.emplace(key, address);
    return address;
  }

  pcaa_guest_address_t copy_matrix(pbqp_matrix_view_t matrix) {
    HostTimer timer(measure_ ? &staging_ns_ : nullptr);
    const pcaa_guest_address_t address =
        allocate_storage(matrix.rows * matrix.columns * sizeof(int32_t));
    if (address == 0)
      return 0;
    for (size_t row = 0; row < matrix.rows; ++row) {
      for (size_t column = 0; column < matrix.columns; ++column) {
        const int32_t value = matrix.base[row * matrix.row_stride + column * matrix.column_stride];
        if (!memory_.write(address + (row * matrix.columns + column) * sizeof(int32_t), &value,
                           sizeof(value))) {
          staging_status_ = PCAA_STATUS_MEMORY_ERROR;
          return 0;
        }
      }
    }
    return address;
  }

  pcaa_guest_address_t allocate_storage(size_t bytes) {
    const pcaa_guest_address_t address = memory_.allocate(bytes);
    if (address == 0 && staging_status_ == PCAA_STATUS_OK)
      staging_status_ = PCAA_STATUS_NO_SPACE;
    return address;
  }

  pcaa_guest_address_t allocate_result() {
    return allocate_storage(sizeof(accel_min_argmin_result_t));
  }

  pcaa_status_t submit_batch(const std::vector<pcaa_command_t> &commands) {
    const uint64_t submit_start = measure_ ? now() : 0;
    size_t child_bytes = 0;
    const pcaa_status_t measured =
        pcaa_encoded_stream_size(commands.data(), commands.size(), &child_bytes);
    if (measured != PCAA_STATUS_OK) {
      pcaa_perror("pcaa measure batch", measured);
      return measured;
    }
    const pcaa_status_t submitted =
        pcaa_device_submit_batch(device_->device(), commands.data(), commands.size());
    if (submitted != PCAA_STATUS_OK) {
      pcaa_perror("pcaa submit", submitted);
      return submitted;
    }
    if (measure_)
      submission_ns_ += now() - submit_start;
    record_batch_submission(commands, child_bytes);
    const uint64_t wait_start = measure_ ? now() : 0;
    pcaa_completion_t completion{};
    pcaa_status_t completed = pcaa_device_wait(device_->device(), &completion);
    while (l2_ && completed == PCAA_STATUS_BUSY) {
      // The hosted model owns simulation advancement; pcaalib remains transport-only.
      sc_core::sc_start(l2_->config().cycle_period * kL2PollCycles);
      completed = pcaa_device_wait(device_->device(), &completion);
    }
    if (completed != PCAA_STATUS_OK) {
      pcaa_perror("pcaa wait", completed);
      if (l2_) {
        const auto &d = l2_->diagnostic();
        std::cerr << "L2 cause=" << pcaa_status_string(d.cause)
                  << " phase=" << static_cast<int>(d.phase) << " address=" << d.address
                  << " child=" << d.child << " row=" << d.row << " column=" << d.column << '\n';
      }
      if (completion.has_batch_result && completion.failed_index != UINT32_MAX)
        std::cerr << "pcaa: failed child=" << completion.failed_index << '\n';
      return completed;
    }
    if (measure_) {
      wait_ns_ += now() - wait_start;
      readback_start_ = now();
    }
    return PCAA_STATUS_OK;
  }

  void record_batch_submission(const std::vector<pcaa_command_t> &commands, size_t child_bytes) {
    if (statistics_ == nullptr) {
      return;
    }
    const size_t count = commands.size();
    const unsigned command_count = static_cast<unsigned>(count);
    ++statistics_->top_level_submissions;
    ++statistics_->batch_submissions;
    statistics_->batch_primitive_descriptors += command_count;
    if (command_count > statistics_->maximum_batch_size) {
      statistics_->maximum_batch_size = command_count;
    }
    statistics_->batch_descriptor_bytes += ACCEL_BATCH_COMMAND_BYTES + child_bytes;
    statistics_->batch_child_descriptor_bytes += child_bytes;
    for (const pcaa_command_t &command : commands) {
      if (command.kind != PCAA_COST_ADD_VECTOR)
        continue;
      const pcaa_vector_add_t &add = command.operation.vector_add;
      const bool alias0 =
          add.result.base == add.first.base && add.result.stride == add.first.stride;
      const bool alias1 =
          add.result.base == add.second.base && add.result.stride == add.second.stride;
      statistics_->vector_add_dst_src0 += alias0;
      statistics_->vector_add_dst_src1 += alias1;
      statistics_->vector_add_inplace_descriptors += alias0 || alias1;
      statistics_->vector_add_general_descriptors += !alias0 && !alias1;
    }
  }

 public:
  void enable_measurements() {
    measure_ = true;
  }
  const std::vector<HostKernelMeasurement> &measurements() const {
    return measurements_;
  }

 private:
  bool measure_ = false;
  uint64_t staging_ns_ = 0, wait_ns_ = 0, submission_ns_ = 0, readback_start_ = 0;
  std::vector<HostKernelMeasurement> measurements_;
  pcaa_status_t staging_status_ = PCAA_STATUS_OK;

  GuestMemory memory_;
  std::map<ViewKey, pcaa_guest_address_t, ViewKeyLess> view_cache_;
  VectorCycleProjection vector_cycle_projection_;
  std::unique_ptr<Accelerator> accelerator_;
  std::unique_ptr<L2Accelerator> l2_;
  AccelTimingStatistics untimed_;
  Initiator initiator_;
  std::unique_ptr<PcaaSystemCDevice> device_;
  pbqp_statistics_t *statistics_ = nullptr;
};

ModelKernel::ModelKernel(bool verbose, AccelTimingConfig timing, size_t staging_limit,
                         const L2Config *l2)
    : impl_(std::make_unique<Impl>(verbose, timing, staging_limit, l2)) {}
ModelKernel::~ModelKernel() = default;
void ModelKernel::make_kernel(pbqp_cost_kernel_t &kernel) {
  impl_->make_kernel(kernel);
}
const AccelTimingStatistics &ModelKernel::timing_statistics() const {
  return impl_->timing_statistics();
}
const VectorCycleProjection &ModelKernel::vector_cycle_projection() const {
  return impl_->vector_cycle_projection();
}

const L2Statistics &ModelKernel::l2_statistics() const {
  return impl_->l2_statistics();
}
void ModelKernel::enable_measurements() {
  impl_->enable_measurements();
}
const std::vector<HostKernelMeasurement> &ModelKernel::measurements() const {
  return impl_->measurements();
}
