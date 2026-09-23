// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Binds the semantic PCAA device API to the RV64 bare-metal MMIO driver.

#pragma once

#include "accel_protocol.h"
#include "pcaa_device.h"
#include "pcaa_submission.h"

#include <stddef.h>

#if !defined(__riscv) || !defined(__riscv_xlen) || __riscv_xlen != 64
#error "PCAA bare-metal backend requires RV64"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pcaa_baremetal_device {
  pcaa_device_t device;
  pcaa_encoded_slot_t *encoded_workspace;
  size_t workspace_count;
  accel_batch_result_t batch_result;
  size_t pending_batch_count;
  int pending;
} pcaa_baremetal_device_t;

/* Caller owns context and workspace until completion. No heap is used. */
pcaa_device_t *pcaa_baremetal_device_init(pcaa_baremetal_device_t *context,
                                          pcaa_encoded_slot_t *workspace, size_t workspace_count);

#ifdef __cplusplus
}  // extern "C"
#endif
