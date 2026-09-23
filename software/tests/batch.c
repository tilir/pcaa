// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Verifies ordered heterogeneous batch submission through the bare-metal driver.

#include "accel_driver.h"
#include "common.h"
#include "pcaa_codec.h"

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
static pcaa_encoded_slot_t workspace[2];

int main(void) {
  pcaa_command_t commands[2];
  if (pcaa_make_reduce2(pcaa_cost_vector((uintptr_t)first, 3, 1),
                        pcaa_cost_vector((uintptr_t)second, 3, 1), (uintptr_t)&minimum, 0,
                        &commands[0]) != PCAA_STATUS_OK ||
      pcaa_make_reduce3(pcaa_cost_vector((uintptr_t)first, 3, 1),
                        pcaa_cost_vector((uintptr_t)second, 3, 1),
                        pcaa_cost_vector((uintptr_t)third, 3, 1), (uintptr_t)&argmin, 1,
                        &commands[1]) != PCAA_STATUS_OK)
    finish(kExitValidBatchFailed);
  accel_init();
  if (accel_submit_command_batch(commands, 2, workspace, &batch_result) != 0)
    finish(kExitValidBatchFailed);
  if (batch_result.completed != 2 || batch_result.failed_index != UINT32_MAX || minimum != -1 ||
      argmin.value != 0 || argmin.index != 1)
    finish(kExitValidBatchResultMismatch);

  size_t child_bytes = 0;
  if (pcaa_encode_stream(commands, 2, workspace, sizeof(workspace), &child_bytes) != PCAA_STATUS_OK)
    finish(kExitFailedBatchWasAccepted);
  workspace[0].bytes[ACCEL_COMMAND_MIN_BYTES] = kUnsupportedOpcode;
  pcaa_encoded_slot_t parent;
  size_t parent_bytes = 0;
  if (pcaa_encode_batch((uintptr_t)workspace, 2, child_bytes, (uintptr_t)&batch_result, &parent,
                        &parent_bytes) != PCAA_STATUS_OK)
    finish(kExitFailedBatchWasAccepted);
  minimum = 0;
  if (accel_submit_encoded(parent.bytes, parent_bytes) != 0 || accel_wait() == 0)
    finish(kExitFailedBatchWasAccepted);
  if (batch_result.completed != 1 || batch_result.failed_index != 1 || minimum != -1)
    finish(kExitFailedBatchResultMismatch);
  finish(kExitSuccess);
}
