// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Provides independent reference arithmetic and termination helpers for guest tests.

#pragma once

#include "accel_protocol.h"

#include <stdint.h>
#include <stddef.h>

extern volatile uint64_t tohost;

static inline int32_t ref_add(int32_t left, int32_t right) {
  if (left == ACCEL_INF || right == ACCEL_INF) {
    return ACCEL_INF;
  }

  const int64_t sum = (int64_t)left + right;
  if (sum >= ACCEL_INF) {
    return ACCEL_INF;
  }
  return sum < INT32_MIN ? INT32_MIN : (int32_t)sum;
}

static inline int32_t ref_min2(const int32_t* first, const int32_t* second, size_t count) {
  int32_t minimum = ACCEL_INF;
  for (size_t index = 0; index < count; ++index) {
    const int32_t value = ref_add(first[index], second[index]);
    if (index == 0 || value < minimum) {
      minimum = value;
    }
  }
  return minimum;
}

static inline int32_t ref_min3(const int32_t* first, const int32_t* second,
                               const int32_t* third, size_t count) {
  int32_t minimum = ACCEL_INF;
  for (size_t index = 0; index < count; ++index) {
    const int32_t value = ref_add(ref_add(first[index], second[index]), third[index]);
    if (index == 0 || value < minimum) {
      minimum = value;
    }
  }
  return minimum;
}

static inline __attribute__((noreturn)) void finish(int code) {
  tohost = ((uint64_t)code << 1) | 1;
  for (;;) {
    __asm__ volatile("wfi");
  }
}
