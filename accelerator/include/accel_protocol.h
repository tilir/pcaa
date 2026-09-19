// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Defines the C-compatible software ABI for accelerator descriptors and MMIO.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This header deliberately has no C++ or SystemC dependencies. */
#define ACCEL_MMIO_DESC_ADDR_LO 0x00u
#define ACCEL_MMIO_DESC_ADDR_HI 0x04u
#define ACCEL_MMIO_DOORBELL     0x08u
#define ACCEL_MMIO_STATUS       0x0cu
#define ACCEL_MMIO_SIZE         0x1000u

#define ACCEL_STATUS_IDLE  0u
#define ACCEL_STATUS_BUSY  1u
#define ACCEL_STATUS_DONE  2u
#define ACCEL_STATUS_ERROR 3u

#define ACCEL_OPCODE_MAP_ADD_REDUCE_MIN        1u
#define ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN       2u
#define ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN 3u

#define ACCEL_INF (INT32_MAX / 4)

/*
 * Guest-memory command descriptor submitted through the accelerator MMIO
 * registers. The descriptor is a 56-byte, naturally 8-byte-aligned layout:
 * six uint32_t control/dimension fields followed by four uint64_t guest
 * physical addresses. src0, src1, src2, and dst are never host pointers.
 *
 * opcode selects the operation; flags, m, k, and reserved are retained for
 * future compatible commands. The current map/reduce commands use n as their
 * runtime vector length and interpret source elements as int32_t values.
 */
typedef struct accel_command {
  uint32_t opcode;
  uint32_t flags;
  uint32_t n;
  uint32_t m;
  uint32_t k;
  uint32_t reserved;
  uint64_t src0;
  uint64_t src1;
  uint64_t src2;
  uint64_t dst;
} accel_command_t;

typedef struct accel_min_argmin_result {
  int32_t value;
  uint32_t index;
} accel_min_argmin_result_t;

#ifdef __cplusplus
}  // extern "C"

static_assert(sizeof(accel_command_t) == 56, "accelerator ABI changed");
#endif
