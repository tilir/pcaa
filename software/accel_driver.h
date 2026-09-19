// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Declares the bare-metal driver API for submitting accelerator descriptors.

#pragma once

#include "accel_protocol.h"

#include <stddef.h>
#include <stdint.h>

#if !defined(__riscv) || !defined(__riscv_xlen)
#error "PCAA bare-metal driver requires a RISC-V compiler target"
#endif

#if __riscv_xlen != 64
#error "PCAA bare-metal driver currently requires RV64"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ACCEL_MMIO_BASE
#define ACCEL_MMIO_BASE 0x10002000UL
#endif
void accel_init(void);
int accel_submit(const accel_command_t *command);
int accel_wait(void);
int32_t accel_min_add(const int32_t *a, const int32_t *b, size_t n);
int32_t accel_min_add3(const int32_t *a, const int32_t *b, const int32_t *c, size_t n);
accel_min_argmin_result_t accel_min_add_argmin(const int32_t *a, const int32_t *b, size_t n);
accel_min_argmin_result_t accel_min_add3_argmin(const int32_t *a, const int32_t *b,
                                                const int32_t *c, size_t n);

#ifdef __cplusplus
}  // extern "C"
#endif
