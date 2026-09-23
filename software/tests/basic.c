// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Runs deterministic end-to-end checks for every supported accelerator command.

#include "accel_driver.h"
#include "common.h"

enum {
  kExitSuccess = 0,
  kExitMin2Mismatch = 1,
  kExitMin3Mismatch = 2,
  kExitMin3ArgminMismatch = 3,
  kExitMin2ArgminMismatch = 4,
  kExitCheckedErrorNotReported = 5,
  kExitMmioDestinationAccepted = 6,
  kExitRecoveryMismatch = 7,
};

int main(void) {
  const int32_t a[] = {ACCEL_INF, -4, 7, -4, 9};
  const int32_t b[] = {1, 2, -10, 2, ACCEL_INF};
  const int32_t c[] = {3, 4, 5, -1, 7};
  const size_t count = sizeof(a) / sizeof(a[0]);

  accel_init();
  if (accel_min_add(a, b, count) != ref_min2(a, b, count)) {
    finish(kExitMin2Mismatch);
  }
  if (accel_min_add3(a, b, c, count) != ref_min3(a, b, c, count)) {
    finish(kExitMin3Mismatch);
  }

  const accel_min_argmin_result_t add3_result = accel_min_add3_argmin(a, b, c, count);
  if (add3_result.value != -3 || add3_result.index != 3) {
    finish(kExitMin3ArgminMismatch);
  }

  const accel_min_argmin_result_t result = accel_min_add_argmin(a, b, count);
  if (result.value != -3 || result.index != 2) {
    finish(kExitMin2ArgminMismatch);
  }
  accel_min_argmin_result_t checked_result;
  if (accel_min_add_argmin_checked((const int32_t *)0x40000000UL, b, 1, &checked_result) == 0) {
    finish(kExitCheckedErrorNotReported);
  }
  pcaa_command_t command;
  if (pcaa_make_reduce2(pcaa_cost_vector((uintptr_t)a, 1, 1), pcaa_cost_vector((uintptr_t)b, 1, 1),
                        ACCEL_MMIO_BASE + ACCEL_MMIO_DOORBELL, 0, &command) != PCAA_STATUS_OK)
    finish(kExitMmioDestinationAccepted);
  if (accel_submit_command(&command) == 0 && accel_wait() == 0) {
    finish(kExitMmioDestinationAccepted);
  }
  if (accel_min_add(a, b, count) != ref_min2(a, b, count)) {
    finish(kExitRecoveryMismatch);
  }
  finish(kExitSuccess);
}
