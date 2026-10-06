// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements shared saturating arithmetic for the accelerator cost domain.

#include "cost_math.h"
#include "cost_math_impl.h"

int accel_cost_add_checked(int32_t left, int32_t right, int32_t *result) {
  return pcaa::CostAddChecked(left, right, result);
}

int32_t accel_cost_add(int32_t left, int32_t right) {
  int32_t result = INT32_MIN;
  (void)accel_cost_add_checked(left, right, &result);
  return result;
}
