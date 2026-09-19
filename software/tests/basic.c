// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Runs deterministic end-to-end checks for every supported accelerator command.

#include "accel_driver.h"
#include "common.h"

int main(void) {
  const int32_t a[] = {ACCEL_INF, -4, 7, -4, 9};
  const int32_t b[] = {1, 2, -10, 2, ACCEL_INF};
  const int32_t c[] = {3, 4, 5, -1, 7};
  const size_t count = sizeof(a) / sizeof(a[0]);

  accel_init();
  if (accel_min_add(a, b, count) != ref_min2(a, b, count)) {
    finish(1);
  }
  if (accel_min_add3(a, b, c, count) != ref_min3(a, b, c, count)) {
    finish(2);
  }

  const accel_min_argmin_result_t result = accel_min_add_argmin(a, b, count);
  if (result.value != -3 || result.index != 2) {
    finish(3);
  }
  finish(0);
}
