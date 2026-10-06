// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Shares the checked cost implementation with hosted optimized kernels.
#pragma once
#include "accel_protocol.h"
#include <stdint.h>

namespace pcaa {
inline int CostAddChecked(int32_t left, int32_t right, int32_t *result) {
  if (result == nullptr || left > ACCEL_INF || right > ACCEL_INF)
    return -1;
  if (left == ACCEL_INF || right == ACCEL_INF) {
    *result = ACCEL_INF;
    return 0;
  }
  const int64_t sum = static_cast<int64_t>(left) + right;
  if (sum >= ACCEL_INF) {
    *result = ACCEL_INF;
    return 0;
  }
  if (sum < INT32_MIN)
    return -1;
  *result = static_cast<int32_t>(sum);
  return 0;
}
}  // namespace pcaa
