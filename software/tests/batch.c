// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Verifies ordered heterogeneous batch submission through the bare-metal driver.

#include "accel_driver.h"
#include "common.h"

enum {
  kExitSuccess = 0,
  kExitValidBatchFailed = 1,
  kExitValidBatchResultMismatch = 2,
  kExitFailedBatchWasAccepted = 3,
  kExitFailedBatchResultMismatch = 4,
  kUnsupportedOpcode = 99,
};

static const int32_t first[] = {4, -3, 8};
static const int32_t second[] = {-1, 2, -9};
static const int32_t third[] = {5, 1, 2};
static int32_t minimum;
static accel_min_argmin_result_t argmin;
static accel_batch_result_t batch_result;

int main(void) {
  accel_command_t commands[] = {
      {.opcode = ACCEL_OPCODE_MAP_ADD_REDUCE_MIN,
       .n = 3,
       .src0 = (uintptr_t)first,
       .src1 = (uintptr_t)second,
       .dst = (uintptr_t)&minimum},
      {.opcode = ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN,
       .n = 3,
       .src0 = (uintptr_t)first,
       .src1 = (uintptr_t)second,
       .src2 = (uintptr_t)third,
       .dst = (uintptr_t)&argmin},
  };
  accel_init();
  if (accel_submit_batch(commands, 2, &batch_result) != 0)
    finish(kExitValidBatchFailed);
  if (batch_result.completed != 2 || batch_result.failed_index != UINT32_MAX || minimum != -1 ||
      argmin.value != 0 || argmin.index != 1)
    finish(kExitValidBatchResultMismatch);

  commands[1].opcode = kUnsupportedOpcode;
  minimum = 0;
  if (accel_submit_batch(commands, 2, &batch_result) == 0)
    finish(kExitFailedBatchWasAccepted);
  if (batch_result.completed != 1 || batch_result.failed_index != 1 || minimum != -1)
    finish(kExitFailedBatchResultMismatch);
  finish(kExitSuccess);
}
