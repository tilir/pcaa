// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Declares reusable cost-domain arithmetic with the accelerator's INF semantics.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Adds two costs, propagating ACCEL_INF and saturating positive overflow to INF. */
int32_t accel_cost_add(int32_t left, int32_t right);

#ifdef __cplusplus
}  // extern "C"
#endif
