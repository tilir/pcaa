// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Defines platform-neutral semantic PCAA commands and affine guest-memory views.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SemVer strings for the library API and semantic ISA, respectively. */
const char *pcaa_version(void);
const char *pcaa_isa_version(void);

/* Guest physical byte address, never a host pointer. */
typedef uint64_t pcaa_guest_address_t;

/* Library outcomes are distinct from the device's coarse STATUS register. */
typedef enum pcaa_status {
  PCAA_STATUS_OK = 0,
  PCAA_STATUS_INVALID_ARGUMENT,
  PCAA_STATUS_INVALID_COMMAND,
  PCAA_STATUS_RANGE,
  PCAA_STATUS_BUSY,
  PCAA_STATUS_NO_PENDING,
  PCAA_STATUS_NO_SPACE,
  PCAA_STATUS_MEMORY_ERROR,
  PCAA_STATUS_TRANSPORT_ERROR,
  PCAA_STATUS_DEVICE_ERROR
} pcaa_status_t;

/* Static-lifetime English text; available on hosted and freestanding builds. */
const char *pcaa_status_string(pcaa_status_t status);

/* Addresses are guest physical byte addresses; strides count 32-bit costs. */
typedef struct pcaa_cost_vector_view {
  pcaa_guest_address_t base;
  size_t length;
  size_t stride;
} pcaa_cost_vector_view_t;

/* addr(row, column) = base + (row * row_stride + column * column_stride) * 4. */
typedef struct pcaa_cost_matrix_view {
  pcaa_guest_address_t base;
  size_t rows;
  size_t columns;
  size_t row_stride;
  size_t column_stride;
} pcaa_cost_matrix_view_t;

typedef enum pcaa_output_kind { PCAA_OUTPUT_COST, PCAA_OUTPUT_MIN_ARGMIN } pcaa_output_kind_t;

/* Stride counts output elements: 4-byte costs or 8-byte min/argmin records. */
typedef struct pcaa_output_view {
  pcaa_guest_address_t base;
  size_t length;
  size_t stride;
  pcaa_output_kind_t kind;
} pcaa_output_view_t;

/* Semantic kinds are not wire opcode numbers or a serialized ABI. */
typedef enum pcaa_command_kind {
  PCAA_MAP_ADD_REDUCE_MIN,
  PCAA_MAP_ADD3_REDUCE_MIN,
  PCAA_MAP_ADD_REDUCE_MIN_ARGMIN,
  PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN,
  PCAA_COST_ADD_VECTOR,
  PCAA_MINPLUS_PROJECT,
  PCAA_MINPLUS_MAP3_PROJECT,
  PCAA_ORDERED_BATCH
} pcaa_command_kind_t;

typedef struct pcaa_reduce2 {
  pcaa_cost_vector_view_t first;
  pcaa_cost_vector_view_t second;
  pcaa_guest_address_t result;
} pcaa_reduce2_t;

typedef struct pcaa_reduce3 {
  pcaa_cost_vector_view_t first;
  pcaa_cost_vector_view_t second;
  pcaa_cost_vector_view_t third;
  pcaa_guest_address_t result;
} pcaa_reduce3_t;

typedef struct pcaa_vector_add {
  pcaa_cost_vector_view_t first;
  pcaa_cost_vector_view_t second;
  pcaa_output_view_t result;
} pcaa_vector_add_t;

typedef struct pcaa_project {
  pcaa_cost_matrix_view_t matrix;
  pcaa_cost_vector_view_t vector;
  pcaa_output_view_t result;
} pcaa_project_t;

typedef struct pcaa_map3_project {
  pcaa_cost_vector_view_t first;
  pcaa_cost_vector_view_t second;
  pcaa_cost_matrix_view_t third;
  pcaa_output_view_t result;
} pcaa_map3_project_t;

/* Ordered child descriptors are a transport reference, not a graph or scheduler. */
typedef struct pcaa_batch_reference {
  pcaa_guest_address_t child_descriptors;
  size_t count;
  size_t child_bytes;
  pcaa_guest_address_t result;
} pcaa_batch_reference_t;

typedef struct pcaa_command {
  pcaa_command_kind_t kind;
  union {
    pcaa_reduce2_t reduce2;
    pcaa_reduce3_t reduce3;
    pcaa_vector_add_t vector_add;
    pcaa_project_t project;
    pcaa_map3_project_t map3_project;
    pcaa_batch_reference_t batch;
  } operation;
} pcaa_command_t;

pcaa_cost_vector_view_t pcaa_cost_vector(pcaa_guest_address_t base, size_t length, size_t stride);
pcaa_cost_matrix_view_t pcaa_cost_matrix(pcaa_guest_address_t base, size_t rows, size_t columns,
                                         size_t row_stride, size_t column_stride);
pcaa_output_view_t pcaa_cost_output(pcaa_guest_address_t base, size_t length, size_t stride);
pcaa_output_view_t pcaa_argmin_output(pcaa_guest_address_t base, size_t length, size_t stride);

pcaa_status_t pcaa_make_reduce2(pcaa_cost_vector_view_t first, pcaa_cost_vector_view_t second,
                                pcaa_guest_address_t result, int with_argmin,
                                pcaa_command_t *command);
pcaa_status_t pcaa_make_reduce3(pcaa_cost_vector_view_t first, pcaa_cost_vector_view_t second,
                                pcaa_cost_vector_view_t third, pcaa_guest_address_t result,
                                int with_argmin, pcaa_command_t *command);
pcaa_status_t pcaa_make_cost_add_vector(pcaa_cost_vector_view_t first,
                                        pcaa_cost_vector_view_t second, pcaa_output_view_t result,
                                        pcaa_command_t *command);
pcaa_status_t pcaa_make_minplus_project(pcaa_cost_matrix_view_t matrix,
                                        pcaa_cost_vector_view_t vector, pcaa_output_view_t result,
                                        pcaa_command_t *command);
pcaa_status_t pcaa_make_minplus_map3_project(pcaa_cost_vector_view_t first,
                                             pcaa_cost_vector_view_t second,
                                             pcaa_cost_matrix_view_t third,
                                             pcaa_output_view_t result, pcaa_command_t *command);
pcaa_status_t pcaa_make_ordered_batch(pcaa_guest_address_t child_descriptors, size_t count,
                                      size_t child_bytes, pcaa_guest_address_t result,
                                      pcaa_command_t *command);

#ifdef __cplusplus
}  // extern "C"
#endif
