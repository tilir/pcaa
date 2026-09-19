// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements shared saturating arithmetic for the accelerator cost domain.

#include "cost_math.h"
#include "accel_protocol.h"

int32_t accel_cost_add(int32_t left, int32_t right) {
  if (left == ACCEL_INF || right == ACCEL_INF) {
    return ACCEL_INF;
  }

  const int64_t sum = static_cast<int64_t>(left) + right;
  if (sum >= ACCEL_INF) {
    return ACCEL_INF;
  }
  if (sum < INT32_MIN) {
    return INT32_MIN;
  }
  return static_cast<int32_t>(sum);
}
