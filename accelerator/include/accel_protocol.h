// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Defines the C-compatible PCAA compact encoding constants and MMIO ABI.

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

#define ACCEL_FORMAT_REDUCE2 1u
#define ACCEL_FORMAT_REDUCE3 2u
#define ACCEL_FORMAT_REDUCE2_ARGMIN 3u
#define ACCEL_FORMAT_REDUCE3_ARGMIN 4u
#define ACCEL_FORMAT_EXECUTE_BATCH 5u
#define ACCEL_FORMAT_COST_ADD_VECTOR_GENERAL 6u
#define ACCEL_FORMAT_COST_ADD_VECTOR_INPLACE 7u
#define ACCEL_FORMAT_MINPLUS_PROJECT 8u
#define ACCEL_FORMAT_MINPLUS_MAP3_PROJECT 9u

#define ACCEL_COMMAND_SLOT_BYTES 16u
#define ACCEL_COMMAND_HEADER_BYTES 8u
#define ACCEL_COMMAND_MIN_BYTES 32u
#define ACCEL_COMMAND_MAX_BYTES 64u
#define ACCEL_BATCH_COMMAND_BYTES 32u

#define ACCEL_INF (INT32_MAX / 4)

/*
 * A command is a little-endian stream of two, three, or four 16-byte slots.
 * Its first eight bytes are opcode:u8, format:u8, flags:u16, n:u16, m:u16.
 * The format determines its exact byte length; there is no universal command
 * struct or fixed stream stride. Addresses are 64-bit guest physical byte
 * addresses. See doc/arch.md for every format's offsets and invariants.
 * Encoded commands and their input storage remain caller-owned until completion.
 */

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
static_assert(sizeof(accel_batch_result_t) == 8, "accelerator ABI changed");
#else
_Static_assert(sizeof(accel_batch_result_t) == 8, "accelerator ABI changed");
#endif

#ifdef __cplusplus
}  // extern "C"
#endif
