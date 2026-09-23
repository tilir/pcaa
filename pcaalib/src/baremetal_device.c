// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Sends semantic PCAA commands through the RV64 MMIO driver.

#include "pcaa_baremetal_device.h"
#include "accel_driver.h"

#include <stdint.h>

static pcaa_status_t submit_command(void *opaque, const pcaa_command_t *command) {
  pcaa_baremetal_device_t *context = opaque;
  if (context->pending)
    return PCAA_STATUS_BUSY;
  pcaa_encoded_slot_t encoded;
  const pcaa_status_t encoded_status = pcaa_encode_command(command, &encoded);
  if (encoded_status != PCAA_STATUS_OK)
    return encoded_status;
  if (accel_submit_command(command) != 0)
    return PCAA_STATUS_TRANSPORT_ERROR;
  context->pending_batch_count = 0;
  context->pending = 1;
  return PCAA_STATUS_OK;
}

static pcaa_status_t submit_batch(void *opaque, const pcaa_command_t *commands, size_t count) {
  pcaa_baremetal_device_t *context = opaque;
  if (context->pending)
    return PCAA_STATUS_BUSY;
  if (count > UINT32_MAX)
    return PCAA_STATUS_RANGE;
  if (count > context->workspace_count || count > SIZE_MAX / sizeof(*context->encoded_workspace))
    return PCAA_STATUS_NO_SPACE;
  const pcaa_status_t encoded_status = pcaa_encode_commands(
      commands, count, context->encoded_workspace, count * sizeof(*context->encoded_workspace));
  if (encoded_status != PCAA_STATUS_OK)
    return encoded_status;
  context->batch_result.completed = 0;
  context->batch_result.failed_index = UINT32_MAX;
  pcaa_command_t parent;
  const pcaa_status_t parent_status =
      pcaa_make_ordered_batch((pcaa_guest_address_t)(uintptr_t)context->encoded_workspace, count,
                              (pcaa_guest_address_t)(uintptr_t)&context->batch_result, &parent);
  if (parent_status != PCAA_STATUS_OK)
    return parent_status;
  if (accel_submit_command(&parent) != 0)
    return PCAA_STATUS_TRANSPORT_ERROR;
  context->pending_batch_count = count;
  context->pending = 1;
  return PCAA_STATUS_OK;
}

static pcaa_status_t wait_for_completion(void *opaque, pcaa_completion_t *completion) {
  pcaa_baremetal_device_t *context = opaque;
  if (!context->pending)
    return PCAA_STATUS_NO_PENDING;
  const int status = accel_wait();
  context->pending = 0;
  if (completion != NULL && context->pending_batch_count != 0 &&
      (context->batch_result.completed != 0 || context->batch_result.failed_index != UINT32_MAX)) {
    completion->has_batch_result = 1;
    completion->completed = context->batch_result.completed;
    completion->failed_index = context->batch_result.failed_index;
  }
  if (status != 0)
    return PCAA_STATUS_DEVICE_ERROR;
  return context->pending_batch_count == 0 ||
                 (context->batch_result.completed == context->pending_batch_count &&
                  context->batch_result.failed_index == UINT32_MAX)
             ? PCAA_STATUS_OK
             : PCAA_STATUS_DEVICE_ERROR;
}

pcaa_device_t *pcaa_baremetal_device_init(pcaa_baremetal_device_t *context,
                                          pcaa_encoded_slot_t *workspace, size_t workspace_count) {
  if (context == NULL || (workspace_count != 0 && workspace == NULL))
    return NULL;
  accel_init();
  context->encoded_workspace = workspace;
  context->workspace_count = workspace_count;
  context->pending_batch_count = 0;
  context->pending = 0;
  context->device.context = context;
  context->device.submit_command = submit_command;
  context->device.submit_batch = submit_batch;
  context->device.wait = wait_for_completion;
  return &context->device;
}
