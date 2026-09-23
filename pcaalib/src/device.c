// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Dispatches semantic submissions to the selected platform backend.

#include "pcaa_device.h"
#include "pcaa.h"

#include <stddef.h>
#include <stdint.h>

pcaa_status_t pcaa_device_submit(pcaa_device_t *device, const pcaa_command_t *command) {
  if (device == NULL || device->submit_command == NULL || command == NULL)
    return PCAA_STATUS_INVALID_ARGUMENT;
  return device->submit_command(device->context, command);
}

pcaa_status_t pcaa_device_submit_batch(pcaa_device_t *device, const pcaa_command_t *commands,
                                       size_t count) {
  if (device == NULL || device->submit_batch == NULL || commands == NULL || count == 0)
    return PCAA_STATUS_INVALID_ARGUMENT;
  return device->submit_batch(device->context, commands, count);
}

pcaa_status_t pcaa_device_wait(pcaa_device_t *device, pcaa_completion_t *completion) {
  if (completion != NULL) {
    completion->has_batch_result = 0;
    completion->completed = 0;
    completion->failed_index = UINT32_MAX;
  }
  if (device == NULL || device->wait == NULL)
    return PCAA_STATUS_INVALID_ARGUMENT;
  return device->wait(device->context, completion);
}

const char *pcaa_status_string(pcaa_status_t error) {
  switch (error) {
    case PCAA_STATUS_OK:
      return "ok";
    case PCAA_STATUS_INVALID_ARGUMENT:
      return "invalid argument";
    case PCAA_STATUS_INVALID_COMMAND:
      return "invalid command";
    case PCAA_STATUS_RANGE:
      return "dimension or stride exceeds descriptor range";
    case PCAA_STATUS_BUSY:
      return "submission in progress";
    case PCAA_STATUS_NO_PENDING:
      return "no pending submission";
    case PCAA_STATUS_NO_SPACE:
      return "submission storage exhausted";
    case PCAA_STATUS_MEMORY_ERROR:
      return "guest memory access failed";
    case PCAA_STATUS_TRANSPORT_ERROR:
      return "transport failed";
    case PCAA_STATUS_DEVICE_ERROR:
      return "device reported error";
  }
  return "unknown status";
}
