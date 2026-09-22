// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Defines the C-compatible software ABI for accelerator descriptors and MMIO.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This header deliberately has no C++ or SystemC dependencies. */
#define ACCEL_MMIO_DESC_ADDR_LO 0x00u
#define ACCEL_MMIO_DESC_ADDR_HI 0x04u
#define ACCEL_MMIO_DOORBELL 0x08u
#define ACCEL_MMIO_STATUS 0x0cu
#define ACCEL_MMIO_SIZE 0x1000u

#define ACCEL_STATUS_IDLE 0u
#define ACCEL_STATUS_BUSY 1u
#define ACCEL_STATUS_DONE 2u
#define ACCEL_STATUS_ERROR 3u

#define ACCEL_OPCODE_MAP_ADD_REDUCE_MIN 1u
#define ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN 2u
#define ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN 3u
#define ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN 4u
#define ACCEL_OPCODE_EXECUTE_BATCH 5u
#define ACCEL_OPCODE_COST_ADD_VECTOR 6u
#define ACCEL_OPCODE_MINPLUS_PROJECT 7u
#define ACCEL_OPCODE_MINPLUS_MAP3_PROJECT 8u

#define ACCEL_INF (INT32_MAX / 4)

/*
 * Guest-memory command descriptor submitted through the accelerator MMIO
 * registers. ISA v1 uses an 80-byte, naturally 8-byte-aligned layout. The
 * first 56 bytes preserve the offsets and meaning of opcodes 1-5; appended
 * strides are ignored by those opcodes. Addresses are guest physical, never
 * host pointers. Strides are unsigned counts of the addressed element type.
 *
 * opcode selects the operation. The scalar map/reduce commands use n as their runtime
 * vector length and interpret source elements as int32_t values. For
 * EXECUTE_BATCH, n is a non-zero child-descriptor count, src0 is the guest
 * physical address of accel_command_t[n], dst is the guest physical address
 * of accel_batch_result_t, and src1/src2 are unused. All child descriptors
 * have this same 80-byte stride; old fields retain their 56-byte prefix.
 *
 * Opcodes 6-8 require flags, k, and reserved to be zero. n is the reduction
 * length for projection operations and the vector length for COST_ADD_VECTOR.
 * m is the output length for projections and zero for COST_ADD_VECTOR.
 * src{0,1,2}_stride and dst_stride count addressed elements, not bytes.
 * src0_outer_stride and src2_outer_stride are also element strides.
 * Required inner strides must be nonzero. An outer stride may be zero only
 * when its output dimension is one. Unused stride fields are ignored.
 *
 * COST_ADD_VECTOR reads src0[i], src1[i] and writes dst[i]. Exact full-view
 * dst aliasing with either input is allowed; all other output/input overlap
 * is rejected. MINPLUS_PROJECT reads src0[i,j] and src1[j], writes dst[i].
 * MINPLUS_MAP3_PROJECT reads src0[j], src1[j], src2[i,j] and writes
 * accel_min_argmin_result_t dst[i]. Projection output must not overlap any
 * input. Each result's argmin is local to that output, with first-index ties.
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
  uint32_t src0_stride;
  uint32_t src1_stride;
  uint32_t src2_stride;
  uint32_t dst_stride;
  uint32_t src0_outer_stride;
  uint32_t src2_outer_stride;
} accel_command_t;

/* Eight-byte guest-memory output: signed minimum cost and zero-based first
 * minimizing reduction coordinate. Caller owns the physical result address. */
typedef struct accel_min_argmin_result {
  int32_t value;
  uint32_t index;
} accel_min_argmin_result_t;

/*
 * Completion state written by an ordered, fail-stop EXECUTE_BATCH submission.
 * On success, completed equals the child count and failed_index is UINT32_MAX.
 * On child i failure, completed and failed_index both equal i; earlier child
 * outputs remain visible and later children do not execute.
 */
typedef struct accel_batch_result {
  uint32_t completed;
  uint32_t failed_index;
} accel_batch_result_t;

#if defined(__cplusplus)
static_assert(sizeof(accel_command_t) == 80, "accelerator ABI changed");
static_assert(offsetof(accel_command_t, src0) == 24, "accelerator ABI changed");
static_assert(offsetof(accel_command_t, dst) == 48, "accelerator ABI changed");
static_assert(offsetof(accel_command_t, src0_stride) == 56, "accelerator ABI changed");
static_assert(offsetof(accel_command_t, src2_outer_stride) == 76, "accelerator ABI changed");
static_assert(sizeof(accel_batch_result_t) == 8, "accelerator ABI changed");
#else
_Static_assert(sizeof(accel_command_t) == 80, "accelerator ABI changed");
_Static_assert(offsetof(accel_command_t, src0) == 24, "accelerator ABI changed");
_Static_assert(offsetof(accel_command_t, dst) == 48, "accelerator ABI changed");
_Static_assert(offsetof(accel_command_t, src0_stride) == 56, "accelerator ABI changed");
_Static_assert(offsetof(accel_command_t, src2_outer_stride) == 76, "accelerator ABI changed");
_Static_assert(sizeof(accel_batch_result_t) == 8, "accelerator ABI changed");
#endif

#ifdef __cplusplus
}  // extern "C"
#endif
