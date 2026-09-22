// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements shared saturating arithmetic for the accelerator cost domain.

#include "cost_math.h"
#include "accel_protocol.h"

int accel_cost_add_checked(int32_t left, int32_t right, int32_t *result) {
  if (result == nullptr) {
    return -1;
  }
  if (left == ACCEL_INF || right == ACCEL_INF) {
    *result = ACCEL_INF;
    return 0;
  }

  const int64_t sum = static_cast<int64_t>(left) + right;
  if (sum >= ACCEL_INF) {
    *result = ACCEL_INF;
    return 0;
  }
  if (sum < INT32_MIN) {
    return -1;
  }
  *result = static_cast<int32_t>(sum);
  return 0;
}

int32_t accel_cost_add(int32_t left, int32_t right) {
  int32_t result = INT32_MIN;
  (void)accel_cost_add_checked(left, right, &result);
  return result;
}
