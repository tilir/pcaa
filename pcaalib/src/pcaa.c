// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Builds semantic PCAA commands and checks output-byte intersections.

#include "pcaa.h"
#include "accel_protocol.h"
#include "pcaa_codec.h"

#include <stdint.h>

const char *pcaa_version(void) {
  return "2.0.0";
}

const char *pcaa_isa_version(void) {
  return "1.0.0";
}

pcaa_cost_vector_view_t pcaa_cost_vector(pcaa_guest_address_t base, size_t length, size_t stride) {
  pcaa_cost_vector_view_t view = {base, length, stride};
  return view;
}

pcaa_cost_matrix_view_t pcaa_cost_matrix(pcaa_guest_address_t base, size_t rows, size_t columns,
                                         size_t row_stride, size_t column_stride) {
  pcaa_cost_matrix_view_t view = {base, rows, columns, row_stride, column_stride};
  return view;
}

pcaa_output_view_t pcaa_cost_output(pcaa_guest_address_t base, size_t length, size_t stride) {
  pcaa_output_view_t view = {base, length, stride, PCAA_OUTPUT_COST};
  return view;
}

pcaa_output_view_t pcaa_argmin_output(pcaa_guest_address_t base, size_t length, size_t stride) {
  pcaa_output_view_t view = {base, length, stride, PCAA_OUTPUT_MIN_ARGMIN};
  return view;
}

static pcaa_status_t constructed(pcaa_command_t value, pcaa_command_t *command) {
  if (command == NULL)
    return PCAA_STATUS_INVALID_ARGUMENT;
  size_t bytes = 0;
  const pcaa_status_t status = pcaa_encoded_size(&value, &bytes);
  if (status == PCAA_STATUS_OK)
    *command = value;
  return status;
}

pcaa_status_t pcaa_make_reduce2(pcaa_cost_vector_view_t first, pcaa_cost_vector_view_t second,
                                pcaa_guest_address_t result, int with_argmin,
                                pcaa_command_t *command) {
  pcaa_command_t value = {0};
  value.kind = with_argmin ? PCAA_MAP_ADD_REDUCE_MIN_ARGMIN : PCAA_MAP_ADD_REDUCE_MIN;
  value.operation.reduce2.first = first;
  value.operation.reduce2.second = second;
  value.operation.reduce2.result = result;
  return constructed(value, command);
}

pcaa_status_t pcaa_make_reduce3(pcaa_cost_vector_view_t first, pcaa_cost_vector_view_t second,
                                pcaa_cost_vector_view_t third, pcaa_guest_address_t result,
                                int with_argmin, pcaa_command_t *command) {
  pcaa_command_t value = {0};
  value.kind = with_argmin ? PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN : PCAA_MAP_ADD3_REDUCE_MIN;
  value.operation.reduce3.first = first;
  value.operation.reduce3.second = second;
  value.operation.reduce3.third = third;
  value.operation.reduce3.result = result;
  return constructed(value, command);
}

pcaa_status_t pcaa_make_cost_add_vector(pcaa_cost_vector_view_t first,
                                        pcaa_cost_vector_view_t second, pcaa_output_view_t result,
                                        pcaa_command_t *command) {
  pcaa_command_t value = {0};
  value.kind = PCAA_COST_ADD_VECTOR;
  value.operation.vector_add.first = first;
  value.operation.vector_add.second = second;
  value.operation.vector_add.result = result;
  return constructed(value, command);
}

pcaa_status_t pcaa_make_minplus_project(pcaa_cost_matrix_view_t matrix,
                                        pcaa_cost_vector_view_t vector, pcaa_output_view_t result,
                                        pcaa_command_t *command) {
  pcaa_command_t value = {0};
  value.kind = PCAA_MINPLUS_PROJECT;
  value.operation.project.matrix = matrix;
  value.operation.project.vector = vector;
  value.operation.project.result = result;
  return constructed(value, command);
}

pcaa_status_t pcaa_make_minplus_map3_project(pcaa_cost_vector_view_t first,
                                             pcaa_cost_vector_view_t second,
                                             pcaa_cost_matrix_view_t third,
                                             pcaa_output_view_t result, pcaa_command_t *command) {
  pcaa_command_t value = {0};
  value.kind = PCAA_MINPLUS_MAP3_PROJECT;
  value.operation.map3_project.first = first;
  value.operation.map3_project.second = second;
  value.operation.map3_project.third = third;
  value.operation.map3_project.result = result;
  return constructed(value, command);
}

pcaa_status_t pcaa_make_ordered_batch(pcaa_guest_address_t child_descriptors, size_t count,
                                      size_t child_bytes, pcaa_guest_address_t result,
                                      pcaa_command_t *command) {
  pcaa_command_t value = {0};
  value.kind = PCAA_ORDERED_BATCH;
  value.operation.batch.child_descriptors = child_descriptors;
  value.operation.batch.count = count;
  value.operation.batch.child_bytes = child_bytes;
  value.operation.batch.result = result;
  return constructed(value, command);
}

int pcaa_output_overlaps(const pcaa_command_t *command, pcaa_guest_address_t begin,
                         pcaa_guest_address_t end) {
  if (command == NULL || command->kind == PCAA_ORDERED_BATCH || begin > end)
    return 1;
  pcaa_guest_address_t base = 0;
  size_t count = 1;
  size_t stride = 1;
  size_t width = sizeof(int32_t);
  switch (command->kind) {
    case PCAA_MAP_ADD_REDUCE_MIN:
      base = command->operation.reduce2.result;
      break;
    case PCAA_MAP_ADD_REDUCE_MIN_ARGMIN:
      base = command->operation.reduce2.result;
      width = sizeof(accel_min_argmin_result_t);
      break;
    case PCAA_MAP_ADD3_REDUCE_MIN:
      base = command->operation.reduce3.result;
      break;
    case PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN:
      base = command->operation.reduce3.result;
      width = sizeof(accel_min_argmin_result_t);
      break;
    case PCAA_COST_ADD_VECTOR:
      base = command->operation.vector_add.result.base;
      count = command->operation.vector_add.result.length;
      stride = command->operation.vector_add.result.stride;
      break;
    case PCAA_MINPLUS_PROJECT:
      base = command->operation.project.result.base;
      count = command->operation.project.result.length;
      stride = command->operation.project.result.stride;
      break;
    case PCAA_MINPLUS_MAP3_PROJECT:
      base = command->operation.map3_project.result.base;
      count = command->operation.map3_project.result.length;
      stride = command->operation.map3_project.result.stride;
      width = sizeof(accel_min_argmin_result_t);
      break;
    default:
      return 1;
  }
  if (base == 0 || count == 0 || stride == 0 || count > UINT16_MAX || stride > UINT16_MAX ||
      base > UINT64_MAX - width || (count - 1) * stride > (UINT64_MAX - base - width) / width)
    return 1;
  for (size_t index = 0; index < count; ++index) {
    const uint64_t address = base + (uint64_t)index * stride * width;
    if (address < end && begin < address + width)
      return 1;
  }
  return 0;
}
