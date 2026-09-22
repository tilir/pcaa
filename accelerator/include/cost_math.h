// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Declares reusable cost-domain arithmetic with the accelerator's INF semantics.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Adds two costs for host-side bounded algorithms; negative underflow clamps for compatibility. */
int32_t accel_cost_add(int32_t left, int32_t right);

/* Command-path addition: returns non-zero instead of silently tying negative underflows. */
int accel_cost_add_checked(int32_t left, int32_t right, int32_t *result);

#ifdef __cplusplus
}  // extern "C"
#endif
