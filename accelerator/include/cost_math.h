// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Declares reusable cost-domain arithmetic with the accelerator's INF semantics.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Adds valid-domain costs for bounded host algorithms; negative underflow clamps for compatibility.
 * Callers must ensure both operands are at most ACCEL_INF. */
int32_t accel_cost_add(int32_t left, int32_t right);

/* Command-path addition: rejects operands above ACCEL_INF and finite negative underflow. */
int accel_cost_add_checked(int32_t left, int32_t right, int32_t *result);

#ifdef __cplusplus
}  // extern "C"
#endif
