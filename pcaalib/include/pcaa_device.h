// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Declares the platform-independent PCAA submission interface.

#pragma once

#include "pcaa.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Optional detail from the batch result. Valid even on DEVICE_ERROR when the
 * failing child wrote a result; unavailable for failures before child execution. */
typedef struct pcaa_completion {
  int has_batch_result;
  uint32_t completed;
  uint32_t failed_index;
} pcaa_completion_t;

/* The platform library binds these callbacks; clients use only the functions below. */
typedef struct pcaa_device {
  void *context;
  pcaa_status_t (*submit_command)(void *context, const pcaa_command_t *command);
  pcaa_status_t (*submit_batch)(void *context, const pcaa_command_t *commands, size_t count);
  pcaa_status_t (*wait)(void *context, pcaa_completion_t *completion);
} pcaa_device_t;

/* Submission and completion are deliberately separate for asynchronous backends. */
pcaa_status_t pcaa_device_submit(pcaa_device_t *device, const pcaa_command_t *command);
pcaa_status_t pcaa_device_submit_batch(pcaa_device_t *device, const pcaa_command_t *commands,
                                       size_t count);
pcaa_status_t pcaa_device_wait(pcaa_device_t *device, pcaa_completion_t *completion);

#ifdef __cplusplus
}  // extern "C"
#endif
