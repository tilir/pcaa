// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Owns allocator-backed PBQP state for the C++ solver implementation.

#pragma once

#include "pbqp.h"

#include <stddef.h>

namespace pcaa::pbqp_storage {

unsigned *Reconstruction(pbqp_problem_t &problem, unsigned node);
const unsigned *Reconstruction(const pbqp_problem_t &problem, unsigned node);
void CopyStatistics(pbqp_statistics_t *destination, const pbqp_statistics_t &source,
                    unsigned node_capacity, unsigned domain_capacity);

template <typename T>
class Array {
 public:
  Array(pbqp_allocator_t allocator, size_t count)
      : allocator_(allocator), count_(count), data_(nullptr) {
    if (allocator_.allocate != nullptr && count_ != 0) {
      data_ = static_cast<T *>(allocator_.allocate(allocator_.context, count_ * sizeof(T)));
      if (data_ != nullptr) {
        for (size_t index = 0; index < count_; ++index) data_[index] = {};
      }
    }
  }

  ~Array() {
    Reset();
  }

  Array(const Array &) = delete;
  Array &operator=(const Array &) = delete;

  Array(Array &&other) noexcept
      : allocator_(other.allocator_), count_(other.count_), data_(other.data_) {
    other.allocator_ = {};
    other.count_ = 0;
    other.data_ = nullptr;
  }

  Array &operator=(Array &&other) noexcept {
    if (this != &other) {
      Reset();
      allocator_ = other.allocator_;
      count_ = other.count_;
      data_ = other.data_;
      other.allocator_ = {};
      other.count_ = 0;
      other.data_ = nullptr;
    }
    return *this;
  }

  T *Get() const {
    return data_;
  }

 private:
  void Reset() {
    if (data_ != nullptr && allocator_.deallocate != nullptr)
      allocator_.deallocate(allocator_.context, data_, count_ * sizeof(T));
    allocator_ = {};
    count_ = 0;
    data_ = nullptr;
  }

  pbqp_allocator_t allocator_;
  size_t count_;
  T *data_;
};

class Problem {
 public:
  Problem(const pbqp_problem_t &source, pbqp_allocator_t allocator) : state_{}, status_{} {
    status_ = pbqp_problem_clone(&state_, &source, allocator);
  }

  ~Problem() {
    pbqp_destroy(&state_);
  }

  Problem(const Problem &) = delete;
  Problem &operator=(const Problem &) = delete;

  Problem(Problem &&other) noexcept : state_(other.state_), status_(other.status_) {
    other.state_ = {};
    other.status_ = PBQP_ARGUMENT_ERROR;
  }

  Problem &operator=(Problem &&other) noexcept {
    if (this != &other) {
      pbqp_destroy(&state_);
      state_ = other.state_;
      status_ = other.status_;
      other.state_ = {};
      other.status_ = PBQP_ARGUMENT_ERROR;
    }
    return *this;
  }

  pbqp_status_t Status() const {
    return status_;
  }
  pbqp_problem_t &State() {
    return state_;
  }

 private:
  pbqp_problem_t state_;
  pbqp_status_t status_;
};

}  // namespace pcaa::pbqp_storage
