// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements independent MAS phase progression and acknowledged physical transfers.
#include "l2_accelerator.h"
#include "cost_math.h"
#include "pcaa_codec.h"
#include "memory_interface.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <ostream>
#include <stdexcept>

#include <tlm_core/tlm_2/tlm_generic_payload/tlm_gp.h>

namespace {
constexpr size_t kCostBytes = sizeof(int32_t);
constexpr size_t kPairBytes = sizeof(accel_min_argmin_result_t);
constexpr int kAddressHalfBits = 32;
constexpr size_t kMaximumParameter = UINT16_MAX;
constexpr int kMaximumLatency = UINT16_MAX;
constexpr std::array<const char *, static_cast<size_t>(L2Phase::Count)> kPhases = {
    "descriptor", "decode", "protection", "operand", "add1",        "add2",
    "tree",       "merge",  "writeback",  "drain",   "memory_wait", "control"};
bool overlaps(uint64_t a, uint64_t ae, uint64_t b, uint64_t be) {
  return a < be && b < ae;
}
uint32_t read32(const unsigned char *data) {
  uint32_t result = 0;
  for (size_t i = 0; i < sizeof(result); ++i) result |= uint32_t(data[i]) << (8 * i);
  return result;
}
void write32(unsigned char *data, uint32_t value) {
  for (size_t i = 0; i < sizeof(value); ++i) data[i] = static_cast<unsigned char>(value >> (8 * i));
}
int opcode(pcaa_command_kind_t kind) {
  constexpr std::array<int, 8> codes = {
      ACCEL_OPCODE_MAP_ADD_REDUCE_MIN,        ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN,
      ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN, ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN,
      ACCEL_OPCODE_COST_ADD_VECTOR,           ACCEL_OPCODE_MINPLUS_PROJECT,
      ACCEL_OPCODE_MINPLUS_MAP3_PROJECT,      ACCEL_OPCODE_EXECUTE_BATCH};
  return codes.at(static_cast<size_t>(kind));
}
}  // namespace

bool L2Config::valid() const {
  return lanes > 0 && lanes <= kMaximumParameter && mem_bytes > 0 &&
         mem_bytes <= kMaximumParameter && tm > 0 && tm <= kMaximumParameter && tn > 0 &&
         tn <= kMaximumParameter && memory_latency > 0 && memory_latency <= kMaximumLatency &&
         acceptance_delay >= 0 && acceptance_delay <= kMaximumLatency &&
         mmio_base <= UINT64_MAX - ACCEL_MMIO_SIZE && cycle_period > sc_core::SC_ZERO_TIME;
}

void l2_write_json(std::ostream &out, const L2Config &c, const L2Statistics &s) {
  out << "{\"mas\":\"1.0.0\",\"lanes\":" << c.lanes << ",\"mem_bytes\":" << c.mem_bytes
      << ",\"tm\":" << c.tm << ",\"tn\":" << c.tn << ",\"memory_latency\":" << c.memory_latency
      << ",\"acceptance_delay\":" << c.acceptance_delay
      << ",\"outstanding_limit\":1,\"phase_overlap\":false,\"cycle_ns\":"
      << c.cycle_period.to_seconds() * 1e9 << ",\"cycles\":" << s.cycles;
  for (size_t i = 0; i < kPhases.size(); ++i)
    out << ",\"" << kPhases[i] << "_cycles\":" << s.phase_cycles[i];
  for (size_t i = 1; i < s.primitives.size(); ++i)
    out << ",\"opcode" << i << "_count\":" << s.primitives[i] << ",\"opcode" << i
        << "_cycles\":" << s.opcode_cycles[i];
#define FIELD(name) out << ",\"" #name "\":" << s.name
  FIELD(submissions);
  FIELD(batches);
  FIELD(children);
  FIELD(tiles);
  FIELD(chunks);
  FIELD(lane_groups);
  FIELD(active_elements);
  FIELD(lane_slots);
  FIELD(tail_slots);
  FIELD(state_updates);
  FIELD(descriptor_bytes);
  FIELD(operand_bytes);
  FIELD(result_bytes);
  FIELD(descriptor_requests);
  FIELD(operand_requests);
  FIELD(result_requests);
  FIELD(header_requests);
  FIELD(body_requests);
  FIELD(requests);
  FIELD(transferred_bytes);
  FIELD(split_transfers);
  FIELD(unaligned_splits);
  FIELD(contiguous_transfers);
  FIELD(gather_elements);
  FIELD(shared_loads);
  FIELD(shared_reread_bytes);
  FIELD(max_outstanding);
  FIELD(max_writebacks);
  FIELD(protection_elements);
  FIELD(child_barriers);
#undef FIELD
  out << '}';
}

L2Accelerator::L2Accelerator(sc_core::sc_module_name name, MemoryInterface &memory, L2Config config)
    : sc_core::sc_module(name), target_socket("target_socket"), memory_(memory), config_(config) {
  if (!config_.valid())
    throw std::invalid_argument("invalid L2 configuration");
  for (auto &bank : banks_) bank.resize(config_.tn);
  states_.resize(config_.tm);
  lanes_.resize(config_.lanes);
  request_data_.resize(config_.mem_bytes);
  fill_data_.resize(config_.mem_bytes);
  target_socket.register_b_transport(this, &L2Accelerator::b_transport);
  SC_THREAD(run);
}

void L2Accelerator::b_transport(tlm::tlm_generic_payload &t, sc_core::sc_time &delay) {
  (void)delay;
  if (!t.get_data_ptr() || t.get_data_length() != sizeof(uint32_t) ||
      t.get_streaming_width() != sizeof(uint32_t) || t.get_byte_enable_ptr() ||
      t.get_address() > ACCEL_MMIO_SIZE - sizeof(uint32_t)) {
    t.set_response_status(tlm::TLM_BURST_ERROR_RESPONSE);
    return;
  }
  uint32_t value = 0;
  bool valid = true;
  if (t.is_read()) {
    switch (t.get_address()) {
      case ACCEL_MMIO_DESC_ADDR_LO:
        value = static_cast<uint32_t>(descriptor_register_);
        break;
      case ACCEL_MMIO_DESC_ADDR_HI:
        value = descriptor_register_ >> kAddressHalfBits;
        break;
      case ACCEL_MMIO_STATUS:
        value = status_;
        break;
      default:
        valid = false;
    }
    std::memcpy(t.get_data_ptr(), &value, sizeof(value));
  } else if (t.is_write()) {
    std::memcpy(&value, t.get_data_ptr(), sizeof(value));
    switch (t.get_address()) {
      case ACCEL_MMIO_DESC_ADDR_LO:
        descriptor_register_ = (descriptor_register_ & (UINT64_MAX << kAddressHalfBits)) | value;
        break;
      case ACCEL_MMIO_DESC_ADDR_HI:
        descriptor_register_ =
            (descriptor_register_ & UINT32_MAX) | (uint64_t(value) << kAddressHalfBits);
        break;
      case ACCEL_MMIO_DOORBELL:
        if (status_ == ACCEL_STATUS_BUSY) {
          valid = false;
        } else {
          active_descriptor_ = descriptor_register_;
          diagnostic_ = {};
          status_ = ACCEL_STATUS_BUSY;
          ++statistics_.submissions;
          doorbell_.notify(sc_core::SC_ZERO_TIME);
        }
        break;
      default:
        valid = false;
    }
  } else {
    t.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
    return;
  }
  t.set_response_status(valid ? tlm::TLM_OK_RESPONSE : tlm::TLM_ADDRESS_ERROR_RESPONSE);
}

void L2Accelerator::tick(L2Phase phase, int cycles) {
  phase_ = phase;
  for (int i = 0; i < cycles; ++i) {
    wait(config_.cycle_period);
    ++statistics_.cycles;
    ++statistics_.phase_cycles[static_cast<size_t>(phase)];
    if (opcode_ != 0)
      ++statistics_.opcode_cycles[opcode_];
  }
}

bool L2Accelerator::fail(pcaa_status_t cause, uint64_t address) {
  if (diagnostic_.cause == PCAA_STATUS_OK) {
    diagnostic_.cause = cause;
    diagnostic_.phase = phase_;
    diagnostic_.address = address;
    diagnostic_.child = child_;
    diagnostic_.row = tile_ + row_;
    diagnostic_.column = chunk_ + group_;
  }
  return false;
}

bool L2Accelerator::transfer(bool write, uint64_t address, unsigned char *data, size_t bytes,
                             L2Phase phase) {
  if (address == 0 || bytes > UINT64_MAX - address ||
      overlaps(address, address + bytes, config_.mmio_base, config_.mmio_base + ACCEL_MMIO_SIZE))
    return fail(PCAA_STATUS_RANGE, address);
  uint64_t *payload = phase == L2Phase::Descriptor ? &statistics_.descriptor_bytes
                      : write                      ? &statistics_.result_bytes
                                                   : &statistics_.operand_bytes;
  *payload += bytes;
  const bool split = bytes > config_.mem_bytes - address % config_.mem_bytes;
  if (split) {
    ++statistics_.split_transfers;
    if (address % kCostBytes != 0)
      ++statistics_.unaligned_splits;
  }
  for (size_t offset = 0; offset < bytes;) {
    assert(!outstanding_);
    request_address_ = address + offset;
    request_bytes_ =
        std::min(bytes - offset, config_.mem_bytes - request_address_ % config_.mem_bytes);
    if (write)
      std::copy_n(data + offset, request_bytes_, request_data_.begin());
    request_valid_ = true;
    tick(L2Phase::MemoryWait, config_.acceptance_delay);
    outstanding_ = true;
    request_valid_ = false;
    statistics_.max_outstanding = 1;
    ++statistics_.requests;
    if (phase == L2Phase::Descriptor)
      ++statistics_.descriptor_requests;
    else if (write)
      ++statistics_.result_requests;
    else
      ++statistics_.operand_requests;
    statistics_.transferred_bytes += request_bytes_;
    tick(phase);  // Acceptance: stable address/data and the shared credit are now owned.
    tick(L2Phase::MemoryWait, config_.memory_latency);
    const bool ok = write ? memory_.write(request_address_, request_data_.data(), request_bytes_)
                          : memory_.read(request_address_, request_data_.data(), request_bytes_);
    tick(phase);  // Response retirement; writes become visible before releasing ownership.
    outstanding_ = false;
    if (!ok)
      return fail(PCAA_STATUS_MEMORY_ERROR, request_address_);
    if (!write)
      std::copy_n(request_data_.begin(), request_bytes_, data + offset);
    offset += request_bytes_;
  }
  return true;
}

bool L2Accelerator::fetch(uint64_t address, size_t available, size_t *bytes) {
  opcode_ = 0;
  descriptor_fill_ = 0;
  phase_ = L2Phase::Descriptor;
  const auto requests_before_header = statistics_.requests;
  if (available < ACCEL_COMMAND_HEADER_BYTES ||
      !transfer(false, address, descriptor_.data(), ACCEL_COMMAND_HEADER_BYTES, phase_))
    return fail(PCAA_STATUS_INVALID_COMMAND, address);
  statistics_.header_requests += statistics_.requests - requests_before_header;
  descriptor_fill_ = ACCEL_COMMAND_HEADER_BYTES;
  tick(L2Phase::Decode);
  if (pcaa_wire_format_size(descriptor_[0], descriptor_[1], bytes) != PCAA_STATUS_OK ||
      *bytes > available || *bytes > UINT64_MAX - address)
    return fail(PCAA_STATUS_INVALID_COMMAND, address);
  opcode_ = descriptor_[0];
  const auto requests_before_body = statistics_.requests;
  if (!transfer(false, address + descriptor_fill_, descriptor_.data() + descriptor_fill_,
                *bytes - descriptor_fill_, L2Phase::Descriptor))
    return false;
  statistics_.body_requests += statistics_.requests - requests_before_body;
  descriptor_fill_ = *bytes;
  tick(L2Phase::Decode);
  const auto status = pcaa_decode_one(descriptor_.data(), descriptor_fill_, &command_, bytes);
  if (status != PCAA_STATUS_OK)
    return fail(status, address);
  return true;
}

void L2Accelerator::run() {
  while (true) {
    wait(doorbell_);
    child_ = tile_ = chunk_ = row_ = group_ = 0;
    opcode_ = 0;
    bank_valid_.fill(0);
    tick(L2Phase::Control);
    size_t bytes = 0;
    bool ok = fetch(active_descriptor_, ACCEL_COMMAND_MAX_BYTES, &bytes);
    if (ok)
      ok = command_.kind == PCAA_ORDERED_BATCH ? batch(bytes) : primitive();
    tick(L2Phase::Drain);
    assert(!outstanding_ && !writeback_valid_ && !request_valid_);
    lane_valid_ = 0;
    bank_valid_.fill(0);
    status_ = ok ? ACCEL_STATUS_DONE : ACCEL_STATUS_ERROR;
    opcode_ = 0;
  }
}

pcaa_output_view_t L2Accelerator::output_view() const {
  switch (command_.kind) {
    case PCAA_COST_ADD_VECTOR:
      return command_.operation.vector_add.result;
    case PCAA_MINPLUS_PROJECT:
      return command_.operation.project.result;
    case PCAA_MINPLUS_MAP3_PROJECT:
      return command_.operation.map3_project.result;
    default: {
      const bool add3 = command_.kind == PCAA_MAP_ADD3_REDUCE_MIN ||
                        command_.kind == PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN;
      const bool pair = command_.kind == PCAA_MAP_ADD_REDUCE_MIN_ARGMIN ||
                        command_.kind == PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN;
      return {add3 ? command_.operation.reduce3.result : command_.operation.reduce2.result, 1, 1,
              pair ? PCAA_OUTPUT_MIN_ARGMIN : PCAA_OUTPUT_COST};
    }
  }
}

bool L2Accelerator::protect(uint64_t begin, uint64_t end, uint64_t parent, size_t parent_bytes) {
  const auto view = output_view();
  const size_t width = view.kind == PCAA_OUTPUT_COST ? kCostBytes : kPairBytes;
  for (size_t i = 0; i < view.length; ++i) {
    const uint64_t address = view.base + i * view.stride * width;
    tick(L2Phase::Protection);
    ++statistics_.protection_elements;
    if (overlaps(address, address + width, begin, end) ||
        overlaps(address, address + width, parent, parent + parent_bytes))
      return fail(PCAA_STATUS_INVALID_COMMAND, address);
  }
  return true;
}

bool L2Accelerator::batch(size_t parent_bytes) {
  parent_ = command_.operation.batch;
  ++statistics_.batches;
  phase_ = L2Phase::Decode;
  if (parent_.child_bytes > UINT64_MAX - parent_.child_descriptors ||
      kPairBytes > UINT64_MAX - parent_.result)
    return fail(PCAA_STATUS_RANGE);
  stream_end_ = parent_.child_descriptors + parent_.child_bytes;
  if (overlaps(parent_.result, parent_.result + kPairBytes, parent_.child_descriptors,
               stream_end_) ||
      overlaps(parent_.result, parent_.result + kPairBytes, active_descriptor_,
               active_descriptor_ + parent_bytes))
    return fail(PCAA_STATUS_INVALID_COMMAND);
  cursor_ = parent_.child_descriptors;
  bool ok = true;
  for (child_ = 0; child_ < parent_.count; ++child_) {
    size_t bytes = 0;
    tick(L2Phase::Control);
    ++statistics_.children;
    if (!fetch(cursor_, stream_end_ - cursor_, &bytes) || command_.kind == PCAA_ORDERED_BATCH ||
        !protect(parent_.child_descriptors, stream_end_, active_descriptor_, parent_bytes) ||
        !primitive()) {
      fail(PCAA_STATUS_INVALID_COMMAND, cursor_);
      ok = false;
      break;
    }
    tick(L2Phase::Drain);
    ++statistics_.child_barriers;
    bank_valid_.fill(0);
    lane_valid_ = 0;
    cursor_ += bytes;
  }
  if (ok && cursor_ != stream_end_)
    ok = fail(PCAA_STATUS_INVALID_COMMAND, cursor_);
  tick(L2Phase::Drain);
  opcode_ = ACCEL_OPCODE_EXECUTE_BATCH;
  const bool written = record(parent_.result, static_cast<uint32_t>(child_),
                              ok ? UINT32_MAX : static_cast<uint32_t>(child_));
  return written && ok;
}

bool L2Accelerator::load(pcaa_cost_vector_view_t view, size_t start, size_t count, size_t bank) {
  bank_valid_[bank] = 0;
  // Use bounded byte staging; no full view packing or reads of stride gaps.
  if (view.stride == 1) {
    // Assemble costs through the request staging directly, in maximal beat-limited spans.
    const uint64_t base = view.base + start * kCostBytes;
    size_t filled = 0;
    std::array<unsigned char, kCostBytes> cost{};
    size_t cost_fill = 0;
    ++statistics_.contiguous_transfers;
    if (count * kCostBytes > config_.mem_bytes - base % config_.mem_bytes) {
      ++statistics_.split_transfers;
      if (base % kCostBytes)
        ++statistics_.unaligned_splits;
    }
    while (filled < count * kCostBytes) {
      const size_t bytes = std::min(count * kCostBytes - filled,
                                    config_.mem_bytes - (base + filled) % config_.mem_bytes);
      // A separate bounded assembly bank prevents the response buffer owning decoded bank data.
      if (!transfer(false, base + filled, fill_data_.data(), bytes, L2Phase::Operand))
        return false;
      for (size_t i = 0; i < bytes; ++i) {
        cost[cost_fill++] = fill_data_[i];
        if (cost_fill == kCostBytes) {
          const uint32_t bits = read32(cost.data());
          std::memcpy(&banks_[bank][bank_valid_[bank]++], &bits, kCostBytes);
          cost_fill = 0;
        }
      }
      filled += bytes;
    }
  } else {
    for (size_t i = 0; i < count; ++i) {
      std::array<unsigned char, kCostBytes> bytes{};
      ++statistics_.gather_elements;
      if (!transfer(false, view.base + (start + i) * view.stride * kCostBytes, bytes.data(),
                    bytes.size(), L2Phase::Operand))
        return false;
      const uint32_t bits = read32(bytes.data());
      std::memcpy(&banks_[bank][i], &bits, kCostBytes);
      ++bank_valid_[bank];
    }
  }
  return true;
}

bool L2Accelerator::add(size_t left, size_t right, size_t start, size_t count, L2Phase phase) {
  assert(start + count <= bank_valid_[left] && start + count <= bank_valid_[right]);
  lane_valid_ = count;
  ++statistics_.lane_groups;
  statistics_.active_elements += count;
  statistics_.lane_slots += config_.lanes;
  statistics_.tail_slots += config_.lanes - count;
  bool ok = true;
  for (size_t i = 0; i < count; ++i) {
    lanes_[i].index = static_cast<uint32_t>(chunk_ + start + i);
    if (accel_cost_add_checked(banks_[left][start + i], banks_[right][start + i],
                               &lanes_[i].value) != 0) {
      diagnostic_.invalid_cost |=
          banks_[left][start + i] > ACCEL_INF || banks_[right][start + i] > ACCEL_INF;
      diagnostic_.underflow |=
          banks_[left][start + i] <= ACCEL_INF && banks_[right][start + i] <= ACCEL_INF;
      ok = false;
    }
  }
  tick(phase);
  if (!ok)
    return fail(PCAA_STATUS_DEVICE_ERROR);
  for (size_t i = 0; i < count; ++i) banks_[left][start + i] = lanes_[i].value;
  return true;
}

void L2Accelerator::reduce(size_t start, size_t count, size_t state) {
  (void)start;
  assert(count == lane_valid_);
  // Registered binary tree levels, valid leaves only, global-index comparator.
  size_t live = count;
  size_t width = config_.lanes;
  while (width > 1) {
    for (size_t i = 0; i < live / 2; ++i) {
      const auto a = lanes_[2 * i], b = lanes_[2 * i + 1];
      lanes_[i] = (b.value < a.value || (b.value == a.value && b.index < a.index)) ? b : a;
    }
    if (live % 2)
      lanes_[live / 2] = lanes_[live - 1];
    live = (live + 1) / 2;
    width = (width + 1) / 2;
    tick(L2Phase::Tree);
  }
  tick(L2Phase::Merge);
  auto &s = states_[state];
  if (lanes_[0].value < s.value || (lanes_[0].value == s.value && lanes_[0].index < s.index))
    s = lanes_[0];
  ++statistics_.state_updates;
  lane_valid_ = 0;
}

bool L2Accelerator::record(uint64_t address, uint32_t first, uint32_t second) {
  write32(pending_write_.data(), first);
  write32(pending_write_.data() + kCostBytes, second);
  writeback_valid_ = true;
  statistics_.max_writebacks = 1;
  const bool ok = transfer(true, address, pending_write_.data(), kPairBytes, L2Phase::Writeback);
  writeback_valid_ = false;
  return ok;
}

bool L2Accelerator::primitive() {
  opcode_ = opcode(command_.kind);
  ++statistics_.primitives[opcode_];
  bank_valid_.fill(0);
  const bool projection =
      command_.kind == PCAA_MINPLUS_PROJECT || command_.kind == PCAA_MINPLUS_MAP3_PROJECT;
  const bool map3 = command_.kind == PCAA_MINPLUS_MAP3_PROJECT;
  const bool vector = command_.kind == PCAA_COST_ADD_VECTOR;
  const bool add3 =
      command_.kind == PCAA_MAP_ADD3_REDUCE_MIN || command_.kind == PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN;
  pcaa_cost_vector_view_t a{}, b{}, c{};
  pcaa_cost_matrix_view_t matrix{};
  if (projection) {
    if (map3) {
      a = command_.operation.map3_project.first;
      b = command_.operation.map3_project.second;
      matrix = command_.operation.map3_project.third;
    } else {
      a = command_.operation.project.vector;
      matrix = command_.operation.project.matrix;
    }
  } else if (vector) {
    a = command_.operation.vector_add.first;
    b = command_.operation.vector_add.second;
  } else if (add3) {
    a = command_.operation.reduce3.first;
    b = command_.operation.reduce3.second;
    c = command_.operation.reduce3.third;
  } else {
    a = command_.operation.reduce2.first;
    b = command_.operation.reduce2.second;
  }
  const auto out = output_view();
  const size_t outputs = projection ? matrix.rows : 1;
  const auto write_result = [&](size_t index, int32_t value, uint32_t winner) {
    const bool pair = out.kind == PCAA_OUTPUT_MIN_ARGMIN;
    const size_t width = pair ? kPairBytes : kCostBytes;
    write32(pending_write_.data(), static_cast<uint32_t>(value));
    write32(pending_write_.data() + kCostBytes, winner);
    writeback_valid_ = true;
    statistics_.max_writebacks = 1;
    const bool ok = transfer(true, out.base + index * out.stride * width, pending_write_.data(),
                             width, L2Phase::Writeback);
    writeback_valid_ = false;
    return ok;
  };
  for (tile_ = 0; tile_ < outputs; tile_ += config_.tm) {
    const size_t rows = std::min(config_.tm, outputs - tile_);
    ++statistics_.tiles;
    for (row_ = 0; row_ < rows; ++row_) {
      states_[row_] = {ACCEL_INF, 0};
      tick(L2Phase::Control);
    }
    for (chunk_ = 0; chunk_ < a.length; chunk_ += config_.tn) {
      const size_t cols = std::min(config_.tn, a.length - chunk_);
      ++statistics_.chunks;
      if (!load(a, chunk_, cols, 0))
        return false;
      if (projection) {
        ++statistics_.shared_loads;
        if (tile_ != 0)
          statistics_.shared_reread_bytes += cols * kCostBytes;
      }
      if (!projection || map3) {
        if (!load(b, chunk_, cols, 1))
          return false;
        if (map3) {
          ++statistics_.shared_loads;
          if (tile_ != 0)
            statistics_.shared_reread_bytes += cols * kCostBytes;
        }
      }
      if (add3 && !load(c, chunk_, cols, 2))
        return false;
      if (map3) {
        for (group_ = 0; group_ < cols; group_ += config_.lanes)
          if (!add(0, 1, group_, std::min(config_.lanes, cols - group_), L2Phase::Add1))
            return false;
      }
      for (row_ = 0; row_ < rows; ++row_) {
        if (projection && !load({matrix.base + (tile_ + row_) * matrix.row_stride * kCostBytes,
                                 matrix.columns, matrix.column_stride},
                                chunk_, cols, 2))
          return false;
        for (group_ = 0; group_ < cols; group_ += config_.lanes) {
          const size_t count = std::min(config_.lanes, cols - group_);
          if (projection) {
            // Shared A must survive every row. Store matrix-dependent results in C.
            if (!add(2, 0, group_, count, map3 ? L2Phase::Add2 : L2Phase::Add1))
              return false;
          } else {
            if (!add(0, 1, group_, count, L2Phase::Add1))
              return false;
            if (add3 && !add(0, 2, group_, count, L2Phase::Add2))
              return false;
          }
          if (!vector)
            reduce(group_, count, row_);
        }
      }
      if (vector) {
        for (size_t i = 0; i < cols; ++i)
          if (!write_result(chunk_ + i, banks_[0][i], 0))
            return false;
      }
    }
    if (!vector)
      for (row_ = 0; row_ < rows; ++row_)
        if (!write_result(tile_ + row_, states_[row_].value, states_[row_].index))
          return false;
  }
  return true;
}

void L2Accelerator::reset_statistics() {
  if (status_ == ACCEL_STATUS_BUSY)
    throw std::logic_error("L2 statistics reset while busy");
  statistics_ = {};
}
