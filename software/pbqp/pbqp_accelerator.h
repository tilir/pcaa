// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Declares the PCAA-backed PBQP cost kernel for RV64 bare-metal software.

#pragma once

#include "pbqp.h"
#include "pcaa.h"
#include "pcaa_baremetal_device.h"
#include "pcaa_submission.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  PBQP_MAX_BATCH_JOBS = PBQP_MAX_NODES * PBQP_MAX_DOMAIN,
  PBQP_MAX_BATCH_PACKED_VIEWS = PBQP_MAX_NODES * PBQP_MAX_DOMAIN,
};

typedef struct {
  pbqp_statistics_t *statistics;
  int32_t scratch0[PBQP_MAX_DOMAIN];
  int32_t scratch1[PBQP_MAX_DOMAIN];
  int32_t scratch2[PBQP_MAX_DOMAIN];
  int32_t packed_views[PBQP_MAX_BATCH_PACKED_VIEWS][PBQP_MAX_DOMAIN];
  const int32_t *packed_bases[PBQP_MAX_BATCH_PACKED_VIEWS];
  size_t packed_lengths[PBQP_MAX_BATCH_PACKED_VIEWS];
  size_t packed_strides[PBQP_MAX_BATCH_PACKED_VIEWS];
  unsigned packed_view_count;
  pcaa_command_t batch_commands[PBQP_MAX_BATCH_JOBS];
  pcaa_encoded_slot_t encoded_workspace[PBQP_MAX_BATCH_JOBS];
  pcaa_baremetal_device_t backend;
  pcaa_device_t *device;
  pcaa_status_t last_status;
  pcaa_completion_t last_completion;
} pbqp_accelerator_kernel_context_t;

void pbqp_make_accelerator_kernel(pbqp_cost_kernel_t *kernel,
                                  pbqp_accelerator_kernel_context_t *context,
                                  pbqp_statistics_t *statistics);

#ifdef __cplusplus
}  // extern "C"
#endif
