// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Defines the abstract guest physical-memory interface used by the accelerator.

#pragma once

#include <cstddef>
#include <cstdint>

class MemoryInterface {
 public:
  virtual bool read(uint64_t address, void *destination, size_t size) = 0;
  virtual bool write(uint64_t address, const void *source, size_t size) = 0;
  virtual ~MemoryInterface() = default;
};
