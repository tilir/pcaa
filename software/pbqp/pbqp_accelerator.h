// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Declares the PCAA-backed PBQP cost kernel for RV64 bare-metal software.

#pragma once

#include "pbqp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  pbqp_statistics_t *statistics;
  int32_t scratch0[PBQP_MAX_DOMAIN];
  int32_t scratch1[PBQP_MAX_DOMAIN];
  int32_t scratch2[PBQP_MAX_DOMAIN];
} pbqp_accelerator_kernel_context_t;

void pbqp_make_accelerator_kernel(pbqp_cost_kernel_t *kernel,
                                  pbqp_accelerator_kernel_context_t *context,
                                  pbqp_statistics_t *statistics);

#ifdef __cplusplus
}  // extern "C"
#endif
