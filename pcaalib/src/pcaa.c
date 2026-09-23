// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Builds semantic commands and translates the current PCAA descriptor.

#include "pcaa.h"
#include "accel_protocol.h"
#include "pcaa_codec.h"

#include <stddef.h>
#include <stdint.h>

const char *pcaa_version(void) {
  return "1.0.0";
}

const char *pcaa_isa_version(void) {
  return "1.0.0";
}

static int span(uint64_t base, uint32_t rows, uint32_t columns, uint32_t outer_stride,
                uint32_t inner_stride, uint64_t element_bytes, uint64_t *end) {
  if (base == 0 || rows == 0 || columns == 0 || inner_stride == 0 ||
      (rows > 1 && outer_stride == 0))
    return 0;
  const uint64_t outer = (uint64_t)(rows - 1) * outer_stride;
  const uint64_t inner = (uint64_t)(columns - 1) * inner_stride;
  if (outer > UINT64_MAX - inner || base > UINT64_MAX - element_bytes ||
      outer + inner > (UINT64_MAX - base - element_bytes) / element_bytes)
    return 0;
  *end = base + (outer + inner + 1) * element_bytes;
  return 1;
}

static int overlaps(uint64_t first, uint64_t first_end, uint64_t second, uint64_t second_end) {
  return first < second_end && second < first_end;
}

static int valid_wire(const accel_command_t *wire) {
  if (wire->opcode == ACCEL_OPCODE_EXECUTE_BATCH)
    return wire->n != 0 && wire->src0 != 0 && wire->dst != 0;
  if (wire->n == 0 || wire->src0 == 0 || wire->src1 == 0 || wire->dst == 0)
    return 0;
  if (wire->opcode >= ACCEL_OPCODE_COST_ADD_VECTOR &&
      wire->opcode <= ACCEL_OPCODE_MINPLUS_MAP3_PROJECT) {
    if (wire->flags != 0 || wire->k != 0 || wire->reserved != 0)
      return 0;
    const int add = wire->opcode == ACCEL_OPCODE_COST_ADD_VECTOR;
    const int map3 = wire->opcode == ACCEL_OPCODE_MINPLUS_MAP3_PROJECT;
    if ((add && wire->m != 0) || (!add && wire->m == 0) || (map3 && wire->src2 == 0))
      return 0;
    const uint32_t rows = add ? 1 : wire->m;
    uint64_t first_end = 0;
    uint64_t second_end = 0;
    uint64_t third_end = 0;
    uint64_t destination_end = 0;
    const uint64_t output_bytes = map3 ? sizeof(accel_min_argmin_result_t) : sizeof(int32_t);
    if (!span(wire->src0, map3 ? 1 : rows, wire->n, add || map3 ? 0 : wire->src0_outer_stride,
              wire->src0_stride, sizeof(int32_t), &first_end) ||
        !span(wire->src1, 1, wire->n, 0, wire->src1_stride, sizeof(int32_t), &second_end) ||
        !span(wire->dst, 1, add ? wire->n : rows, 0, wire->dst_stride, output_bytes,
              &destination_end) ||
        (map3 && !span(wire->src2, rows, wire->n, wire->src2_outer_stride, wire->src2_stride,
                       sizeof(int32_t), &third_end)))
      return 0;
    if (add) {
      const int first_alias = wire->dst == wire->src0 && wire->dst_stride == wire->src0_stride;
      const int second_alias = wire->dst == wire->src1 && wire->dst_stride == wire->src1_stride;
      return !(overlaps(wire->dst, destination_end, wire->src0, first_end) && !first_alias) &&
             !(overlaps(wire->dst, destination_end, wire->src1, second_end) && !second_alias);
    }
    return !overlaps(wire->dst, destination_end, wire->src0, first_end) &&
           !overlaps(wire->dst, destination_end, wire->src1, second_end) &&
           (!map3 || !overlaps(wire->dst, destination_end, wire->src2, third_end));
  }
  switch (wire->opcode) {
    case ACCEL_OPCODE_MAP_ADD_REDUCE_MIN:
    case ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN:
      return 1;
    case ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN:
    case ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN:
      return wire->src2 != 0;
    default:
      return 0;
  }
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

pcaa_status_t pcaa_encode_descriptor(const pcaa_command_t *command, accel_command_t *wire) {
  if (command == NULL || wire == NULL)
    return PCAA_STATUS_INVALID_ARGUMENT;
  accel_command_t value = {0};
  switch (command->kind) {
    case PCAA_MAP_ADD_REDUCE_MIN:
    case PCAA_MAP_ADD_REDUCE_MIN_ARGMIN:
      if (command->operation.reduce2.first.length > UINT32_MAX)
        return PCAA_STATUS_RANGE;
      value.opcode = command->kind == PCAA_MAP_ADD_REDUCE_MIN
                         ? ACCEL_OPCODE_MAP_ADD_REDUCE_MIN
                         : ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN;
      value.n = command->operation.reduce2.first.length;
      if (command->operation.reduce2.second.length != command->operation.reduce2.first.length ||
          command->operation.reduce2.first.stride != 1 ||
          command->operation.reduce2.second.stride != 1)
        return PCAA_STATUS_INVALID_COMMAND;
      value.src0 = command->operation.reduce2.first.base;
      value.src1 = command->operation.reduce2.second.base;
      value.dst = command->operation.reduce2.result;
      break;
    case PCAA_MAP_ADD3_REDUCE_MIN:
    case PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN:
      if (command->operation.reduce3.first.length > UINT32_MAX)
        return PCAA_STATUS_RANGE;
      value.opcode = command->kind == PCAA_MAP_ADD3_REDUCE_MIN
                         ? ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN
                         : ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN;
      value.n = command->operation.reduce3.first.length;
      if (command->operation.reduce3.second.length != command->operation.reduce3.first.length ||
          command->operation.reduce3.third.length != command->operation.reduce3.first.length ||
          command->operation.reduce3.first.stride != 1 ||
          command->operation.reduce3.second.stride != 1 ||
          command->operation.reduce3.third.stride != 1)
        return PCAA_STATUS_INVALID_COMMAND;
      value.src0 = command->operation.reduce3.first.base;
      value.src1 = command->operation.reduce3.second.base;
      value.src2 = command->operation.reduce3.third.base;
      value.dst = command->operation.reduce3.result;
      break;
    case PCAA_COST_ADD_VECTOR:
      if (command->operation.vector_add.first.length > UINT32_MAX ||
          command->operation.vector_add.first.stride > UINT32_MAX ||
          command->operation.vector_add.second.stride > UINT32_MAX ||
          command->operation.vector_add.result.stride > UINT32_MAX)
        return PCAA_STATUS_RANGE;
      value.opcode = ACCEL_OPCODE_COST_ADD_VECTOR;
      value.n = command->operation.vector_add.first.length;
      if (command->operation.vector_add.second.length !=
              command->operation.vector_add.first.length ||
          command->operation.vector_add.result.length !=
              command->operation.vector_add.first.length ||
          command->operation.vector_add.result.kind != PCAA_OUTPUT_COST)
        return PCAA_STATUS_INVALID_COMMAND;
      value.src0 = command->operation.vector_add.first.base;
      value.src1 = command->operation.vector_add.second.base;
      value.dst = command->operation.vector_add.result.base;
      value.src0_stride = command->operation.vector_add.first.stride;
      value.src1_stride = command->operation.vector_add.second.stride;
      value.dst_stride = command->operation.vector_add.result.stride;
      break;
    case PCAA_MINPLUS_PROJECT:
      if (command->operation.project.matrix.columns > UINT32_MAX ||
          command->operation.project.matrix.rows > UINT32_MAX ||
          command->operation.project.matrix.column_stride > UINT32_MAX ||
          command->operation.project.matrix.row_stride > UINT32_MAX ||
          command->operation.project.vector.stride > UINT32_MAX ||
          command->operation.project.result.stride > UINT32_MAX)
        return PCAA_STATUS_RANGE;
      value.opcode = ACCEL_OPCODE_MINPLUS_PROJECT;
      value.n = command->operation.project.matrix.columns;
      value.m = command->operation.project.matrix.rows;
      if (command->operation.project.vector.length != command->operation.project.matrix.columns ||
          command->operation.project.result.length != command->operation.project.matrix.rows ||
          command->operation.project.result.kind != PCAA_OUTPUT_COST)
        return PCAA_STATUS_INVALID_COMMAND;
      value.src0 = command->operation.project.matrix.base;
      value.src1 = command->operation.project.vector.base;
      value.dst = command->operation.project.result.base;
      value.src0_stride = command->operation.project.matrix.column_stride;
      value.src0_outer_stride = command->operation.project.matrix.row_stride;
      value.src1_stride = command->operation.project.vector.stride;
      value.dst_stride = command->operation.project.result.stride;
      break;
    case PCAA_MINPLUS_MAP3_PROJECT:
      if (command->operation.map3_project.first.length > UINT32_MAX ||
          command->operation.map3_project.third.rows > UINT32_MAX ||
          command->operation.map3_project.first.stride > UINT32_MAX ||
          command->operation.map3_project.second.stride > UINT32_MAX ||
          command->operation.map3_project.third.column_stride > UINT32_MAX ||
          command->operation.map3_project.third.row_stride > UINT32_MAX ||
          command->operation.map3_project.result.stride > UINT32_MAX)
        return PCAA_STATUS_RANGE;
      value.opcode = ACCEL_OPCODE_MINPLUS_MAP3_PROJECT;
      value.n = command->operation.map3_project.first.length;
      value.m = command->operation.map3_project.third.rows;
      if (command->operation.map3_project.second.length !=
              command->operation.map3_project.first.length ||
          command->operation.map3_project.third.columns !=
              command->operation.map3_project.first.length ||
          command->operation.map3_project.result.length !=
              command->operation.map3_project.third.rows ||
          command->operation.map3_project.result.kind != PCAA_OUTPUT_MIN_ARGMIN)
        return PCAA_STATUS_INVALID_COMMAND;
      value.src0 = command->operation.map3_project.first.base;
      value.src1 = command->operation.map3_project.second.base;
      value.src2 = command->operation.map3_project.third.base;
      value.dst = command->operation.map3_project.result.base;
      value.src0_stride = command->operation.map3_project.first.stride;
      value.src1_stride = command->operation.map3_project.second.stride;
      value.src2_stride = command->operation.map3_project.third.column_stride;
      value.src2_outer_stride = command->operation.map3_project.third.row_stride;
      value.dst_stride = command->operation.map3_project.result.stride;
      break;
    case PCAA_ORDERED_BATCH:
      if (command->operation.batch.count > UINT32_MAX)
        return PCAA_STATUS_RANGE;
      value.opcode = ACCEL_OPCODE_EXECUTE_BATCH;
      value.n = command->operation.batch.count;
      value.src0 = command->operation.batch.child_descriptors;
      value.dst = command->operation.batch.result;
      break;
    default:
      return PCAA_STATUS_INVALID_COMMAND;
  }
  if (!valid_wire(&value))
    return PCAA_STATUS_INVALID_COMMAND;
  *wire = value;
  return PCAA_STATUS_OK;
}

pcaa_status_t pcaa_decode_descriptor(const accel_command_t *wire, pcaa_command_t *command) {
  if (wire == NULL || command == NULL)
    return PCAA_STATUS_INVALID_ARGUMENT;
  if (!valid_wire(wire))
    return PCAA_STATUS_INVALID_COMMAND;
  pcaa_command_t value = {0};
  switch (wire->opcode) {
    case ACCEL_OPCODE_MAP_ADD_REDUCE_MIN:
    case ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN:
      value.kind = wire->opcode == ACCEL_OPCODE_MAP_ADD_REDUCE_MIN ? PCAA_MAP_ADD_REDUCE_MIN
                                                                   : PCAA_MAP_ADD_REDUCE_MIN_ARGMIN;
      value.operation.reduce2.first = pcaa_cost_vector(wire->src0, wire->n, 1);
      value.operation.reduce2.second = pcaa_cost_vector(wire->src1, wire->n, 1);
      value.operation.reduce2.result = wire->dst;
      break;
    case ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN:
    case ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN:
      value.kind = wire->opcode == ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN
                       ? PCAA_MAP_ADD3_REDUCE_MIN
                       : PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN;
      value.operation.reduce3.first = pcaa_cost_vector(wire->src0, wire->n, 1);
      value.operation.reduce3.second = pcaa_cost_vector(wire->src1, wire->n, 1);
      value.operation.reduce3.third = pcaa_cost_vector(wire->src2, wire->n, 1);
      value.operation.reduce3.result = wire->dst;
      break;
    case ACCEL_OPCODE_COST_ADD_VECTOR:
      value.kind = PCAA_COST_ADD_VECTOR;
      value.operation.vector_add.first = pcaa_cost_vector(wire->src0, wire->n, wire->src0_stride);
      value.operation.vector_add.second = pcaa_cost_vector(wire->src1, wire->n, wire->src1_stride);
      value.operation.vector_add.result = pcaa_cost_output(wire->dst, wire->n, wire->dst_stride);
      break;
    case ACCEL_OPCODE_MINPLUS_PROJECT:
      value.kind = PCAA_MINPLUS_PROJECT;
      value.operation.project.matrix = pcaa_cost_matrix(wire->src0, wire->m, wire->n,
                                                        wire->src0_outer_stride, wire->src0_stride);
      value.operation.project.vector = pcaa_cost_vector(wire->src1, wire->n, wire->src1_stride);
      value.operation.project.result = pcaa_cost_output(wire->dst, wire->m, wire->dst_stride);
      break;
    case ACCEL_OPCODE_MINPLUS_MAP3_PROJECT:
      value.kind = PCAA_MINPLUS_MAP3_PROJECT;
      value.operation.map3_project.first = pcaa_cost_vector(wire->src0, wire->n, wire->src0_stride);
      value.operation.map3_project.second =
          pcaa_cost_vector(wire->src1, wire->n, wire->src1_stride);
      value.operation.map3_project.third = pcaa_cost_matrix(
          wire->src2, wire->m, wire->n, wire->src2_outer_stride, wire->src2_stride);
      value.operation.map3_project.result =
          pcaa_argmin_output(wire->dst, wire->m, wire->dst_stride);
      break;
    case ACCEL_OPCODE_EXECUTE_BATCH:
      value.kind = PCAA_ORDERED_BATCH;
      value.operation.batch.child_descriptors = wire->src0;
      value.operation.batch.count = wire->n;
      value.operation.batch.result = wire->dst;
      break;
    default:
      return PCAA_STATUS_INVALID_COMMAND;
  }
  *command = value;
  return PCAA_STATUS_OK;
}

static pcaa_status_t constructed(pcaa_command_t value, pcaa_command_t *command) {
  accel_command_t wire;
  if (command == NULL)
    return PCAA_STATUS_INVALID_ARGUMENT;
  const pcaa_status_t status = pcaa_encode_descriptor(&value, &wire);
  if (status != PCAA_STATUS_OK)
    return status;
  *command = value;
  return PCAA_STATUS_OK;
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
                                      pcaa_guest_address_t result, pcaa_command_t *command) {
  pcaa_command_t value = {0};
  value.kind = PCAA_ORDERED_BATCH;
  value.operation.batch.child_descriptors = child_descriptors;
  value.operation.batch.count = count;
  value.operation.batch.result = result;
  return constructed(value, command);
}

size_t pcaa_descriptor_bytes(void) {
  return sizeof(accel_command_t);
}

size_t pcaa_batch_descriptor_bytes(size_t child_count) {
  if (child_count > SIZE_MAX / sizeof(accel_command_t) - 1)
    return 0;
  return (child_count + 1) * sizeof(accel_command_t);
}

int pcaa_output_overlaps(const pcaa_command_t *command, pcaa_guest_address_t begin,
                         pcaa_guest_address_t end) {
  if (command == NULL || command->kind == PCAA_ORDERED_BATCH)
    return 1;
  pcaa_guest_address_t base = 0;
  size_t count = 1;
  size_t stride = 1;
  uint64_t width = sizeof(int32_t);
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
  pcaa_guest_address_t output_end = 0;
  if (count > UINT32_MAX || stride > UINT32_MAX ||
      !span(base, 1, (uint32_t)count, 0, (uint32_t)stride, width, &output_end))
    return 1;
  for (size_t index = 0; index < count; ++index) {
    const pcaa_guest_address_t address = base + (uint64_t)index * stride * width;
    if (overlaps(address, address + width, begin, end))
      return 1;
  }
  return 0;
}
