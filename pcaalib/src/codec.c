// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Encodes and decodes compact, little-endian PCAA command streams.

#include "pcaa_codec.h"
#include "accel_protocol.h"
#include "pcaa.h"

#include <stddef.h>
#include <stdint.h>

static void put16(unsigned char *bytes, size_t offset, uint16_t value) {
  bytes[offset] = (unsigned char)value;
  bytes[offset + 1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *bytes, size_t offset, uint32_t value) {
  for (size_t index = 0; index < 4; ++index)
    bytes[offset + index] = (unsigned char)(value >> (8 * index));
}

static void put64(unsigned char *bytes, size_t offset, uint64_t value) {
  for (size_t index = 0; index < 8; ++index)
    bytes[offset + index] = (unsigned char)(value >> (8 * index));
}

static uint16_t get16(const unsigned char *bytes, size_t offset) {
  return (uint16_t)bytes[offset] | ((uint16_t)bytes[offset + 1] << 8);
}

static uint32_t get32(const unsigned char *bytes, size_t offset) {
  uint32_t value = 0;
  for (size_t index = 0; index < 4; ++index)
    value |= (uint32_t)bytes[offset + index] << (8 * index);
  return value;
}

static uint64_t get64(const unsigned char *bytes, size_t offset) {
  uint64_t value = 0;
  for (size_t index = 0; index < 8; ++index)
    value |= (uint64_t)bytes[offset + index] << (8 * index);
  return value;
}

static int zero_bytes(const unsigned char *bytes, size_t first, size_t last) {
  for (size_t offset = first; offset < last; ++offset) {
    if (bytes[offset] != 0)
      return 0;
  }
  return 1;
}

static int overlap(uint64_t first, uint64_t first_end, uint64_t second, uint64_t second_end) {
  return first < second_end && second < first_end;
}

static int span(uint64_t base, size_t rows, size_t columns, size_t outer_stride,
                size_t inner_stride, size_t width, uint64_t *end) {
  if (base == 0 || rows == 0 || columns == 0 || inner_stride == 0 ||
      (rows > 1 && outer_stride == 0))
    return 0;
  const uint64_t outer = (uint64_t)(rows - 1) * outer_stride;
  const uint64_t inner = (uint64_t)(columns - 1) * inner_stride;
  if (outer > UINT64_MAX - inner || base > UINT64_MAX - width ||
      outer + inner > (UINT64_MAX - base - width) / width)
    return 0;
  *end = base + (outer + inner + 1) * width;
  return 1;
}

pcaa_status_t pcaa_wire_format_size(uint8_t opcode, uint8_t format, size_t *bytes) {
  if (bytes == NULL)
    return PCAA_STATUS_INVALID_ARGUMENT;
  size_t result = 0;
  switch (format) {
    case ACCEL_FORMAT_REDUCE2:
    case ACCEL_FORMAT_REDUCE2_ARGMIN:
      if (opcode != (format == ACCEL_FORMAT_REDUCE2 ? ACCEL_OPCODE_MAP_ADD_REDUCE_MIN
                                                    : ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN))
        return PCAA_STATUS_INVALID_COMMAND;
      result = 32;
      break;
    case ACCEL_FORMAT_REDUCE3:
    case ACCEL_FORMAT_REDUCE3_ARGMIN:
      if (opcode != (format == ACCEL_FORMAT_REDUCE3 ? ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN
                                                    : ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN))
        return PCAA_STATUS_INVALID_COMMAND;
      result = 48;
      break;
    case ACCEL_FORMAT_EXECUTE_BATCH:
      if (opcode != ACCEL_OPCODE_EXECUTE_BATCH)
        return PCAA_STATUS_INVALID_COMMAND;
      result = 32;
      break;
    case ACCEL_FORMAT_COST_ADD_VECTOR_GENERAL:
      if (opcode != ACCEL_OPCODE_COST_ADD_VECTOR)
        return PCAA_STATUS_INVALID_COMMAND;
      result = 48;
      break;
    case ACCEL_FORMAT_COST_ADD_VECTOR_INPLACE:
      if (opcode != ACCEL_OPCODE_COST_ADD_VECTOR)
        return PCAA_STATUS_INVALID_COMMAND;
      result = 32;
      break;
    case ACCEL_FORMAT_MINPLUS_PROJECT:
      if (opcode != ACCEL_OPCODE_MINPLUS_PROJECT)
        return PCAA_STATUS_INVALID_COMMAND;
      result = 48;
      break;
    case ACCEL_FORMAT_MINPLUS_MAP3_PROJECT:
      if (opcode != ACCEL_OPCODE_MINPLUS_MAP3_PROJECT)
        return PCAA_STATUS_INVALID_COMMAND;
      result = 64;
      break;
    default:
      return PCAA_STATUS_INVALID_COMMAND;
  }
  *bytes = result;
  return PCAA_STATUS_OK;
}

static pcaa_status_t fit16(size_t value) {
  return value > UINT16_MAX ? PCAA_STATUS_RANGE : PCAA_STATUS_OK;
}

static pcaa_status_t valid_command(const pcaa_command_t *command, uint8_t *opcode, uint8_t *format,
                                   size_t *bytes) {
  if (command == NULL)
    return PCAA_STATUS_INVALID_ARGUMENT;
  uint64_t a_end = 0, b_end = 0, c_end = 0, d_end = 0;
  switch (command->kind) {
    case PCAA_MAP_ADD_REDUCE_MIN:
    case PCAA_MAP_ADD_REDUCE_MIN_ARGMIN: {
      const pcaa_reduce2_t *value = &command->operation.reduce2;
      if (fit16(value->first.length) != PCAA_STATUS_OK)
        return PCAA_STATUS_RANGE;
      if (value->first.length != value->second.length || value->first.stride != 1 ||
          value->second.stride != 1 ||
          !span(value->first.base, 1, value->first.length, 0, 1, 4, &a_end) ||
          !span(value->second.base, 1, value->second.length, 0, 1, 4, &b_end) ||
          !span(value->result, 1, 1, 0, 1, command->kind == PCAA_MAP_ADD_REDUCE_MIN ? 4 : 8,
                &d_end))
        return PCAA_STATUS_INVALID_COMMAND;
      *opcode = command->kind == PCAA_MAP_ADD_REDUCE_MIN ? ACCEL_OPCODE_MAP_ADD_REDUCE_MIN
                                                         : ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN;
      *format = command->kind == PCAA_MAP_ADD_REDUCE_MIN ? ACCEL_FORMAT_REDUCE2
                                                         : ACCEL_FORMAT_REDUCE2_ARGMIN;
      break;
    }
    case PCAA_MAP_ADD3_REDUCE_MIN:
    case PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN: {
      const pcaa_reduce3_t *value = &command->operation.reduce3;
      if (fit16(value->first.length) != PCAA_STATUS_OK)
        return PCAA_STATUS_RANGE;
      if (value->first.length != value->second.length ||
          value->first.length != value->third.length || value->first.stride != 1 ||
          value->second.stride != 1 || value->third.stride != 1 ||
          !span(value->first.base, 1, value->first.length, 0, 1, 4, &a_end) ||
          !span(value->second.base, 1, value->second.length, 0, 1, 4, &b_end) ||
          !span(value->third.base, 1, value->third.length, 0, 1, 4, &c_end) ||
          !span(value->result, 1, 1, 0, 1, command->kind == PCAA_MAP_ADD3_REDUCE_MIN ? 4 : 8,
                &d_end))
        return PCAA_STATUS_INVALID_COMMAND;
      *opcode = command->kind == PCAA_MAP_ADD3_REDUCE_MIN ? ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN
                                                          : ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN;
      *format = command->kind == PCAA_MAP_ADD3_REDUCE_MIN ? ACCEL_FORMAT_REDUCE3
                                                          : ACCEL_FORMAT_REDUCE3_ARGMIN;
      break;
    }
    case PCAA_COST_ADD_VECTOR: {
      const pcaa_vector_add_t *value = &command->operation.vector_add;
      if (fit16(value->first.length) != PCAA_STATUS_OK ||
          fit16(value->first.stride) != PCAA_STATUS_OK ||
          fit16(value->second.stride) != PCAA_STATUS_OK ||
          fit16(value->result.stride) != PCAA_STATUS_OK)
        return PCAA_STATUS_RANGE;
      if (value->first.length != value->second.length ||
          value->first.length != value->result.length || value->result.kind != PCAA_OUTPUT_COST ||
          !span(value->first.base, 1, value->first.length, 0, value->first.stride, 4, &a_end) ||
          !span(value->second.base, 1, value->second.length, 0, value->second.stride, 4, &b_end) ||
          !span(value->result.base, 1, value->result.length, 0, value->result.stride, 4, &d_end))
        return PCAA_STATUS_INVALID_COMMAND;
      const int alias0 =
          value->result.base == value->first.base && value->result.stride == value->first.stride;
      const int alias1 =
          value->result.base == value->second.base && value->result.stride == value->second.stride;
      if ((overlap(value->result.base, d_end, value->first.base, a_end) && !alias0) ||
          (overlap(value->result.base, d_end, value->second.base, b_end) && !alias1))
        return PCAA_STATUS_INVALID_COMMAND;
      *opcode = ACCEL_OPCODE_COST_ADD_VECTOR;
      *format = alias0 || alias1 ? ACCEL_FORMAT_COST_ADD_VECTOR_INPLACE
                                 : ACCEL_FORMAT_COST_ADD_VECTOR_GENERAL;
      break;
    }
    case PCAA_MINPLUS_PROJECT: {
      const pcaa_project_t *value = &command->operation.project;
      if (fit16(value->matrix.rows) != PCAA_STATUS_OK ||
          fit16(value->matrix.columns) != PCAA_STATUS_OK ||
          fit16(value->matrix.row_stride) != PCAA_STATUS_OK ||
          fit16(value->matrix.column_stride) != PCAA_STATUS_OK ||
          fit16(value->vector.stride) != PCAA_STATUS_OK ||
          fit16(value->result.stride) != PCAA_STATUS_OK)
        return PCAA_STATUS_RANGE;
      if (value->matrix.columns != value->vector.length ||
          value->matrix.rows != value->result.length || value->result.kind != PCAA_OUTPUT_COST ||
          !span(value->matrix.base, value->matrix.rows, value->matrix.columns,
                value->matrix.row_stride, value->matrix.column_stride, 4, &a_end) ||
          !span(value->vector.base, 1, value->vector.length, 0, value->vector.stride, 4, &b_end) ||
          !span(value->result.base, 1, value->result.length, 0, value->result.stride, 4, &d_end) ||
          overlap(value->result.base, d_end, value->matrix.base, a_end) ||
          overlap(value->result.base, d_end, value->vector.base, b_end))
        return PCAA_STATUS_INVALID_COMMAND;
      *opcode = ACCEL_OPCODE_MINPLUS_PROJECT;
      *format = ACCEL_FORMAT_MINPLUS_PROJECT;
      break;
    }
    case PCAA_MINPLUS_MAP3_PROJECT: {
      const pcaa_map3_project_t *value = &command->operation.map3_project;
      if (fit16(value->third.rows) != PCAA_STATUS_OK ||
          fit16(value->third.columns) != PCAA_STATUS_OK ||
          fit16(value->first.stride) != PCAA_STATUS_OK ||
          fit16(value->second.stride) != PCAA_STATUS_OK ||
          fit16(value->third.row_stride) != PCAA_STATUS_OK ||
          fit16(value->third.column_stride) != PCAA_STATUS_OK ||
          fit16(value->result.stride) != PCAA_STATUS_OK)
        return PCAA_STATUS_RANGE;
      if (value->first.length != value->third.columns ||
          value->second.length != value->third.columns ||
          value->result.length != value->third.rows ||
          value->result.kind != PCAA_OUTPUT_MIN_ARGMIN ||
          !span(value->first.base, 1, value->first.length, 0, value->first.stride, 4, &a_end) ||
          !span(value->second.base, 1, value->second.length, 0, value->second.stride, 4, &b_end) ||
          !span(value->third.base, value->third.rows, value->third.columns, value->third.row_stride,
                value->third.column_stride, 4, &c_end) ||
          !span(value->result.base, 1, value->result.length, 0, value->result.stride, 8, &d_end) ||
          overlap(value->result.base, d_end, value->first.base, a_end) ||
          overlap(value->result.base, d_end, value->second.base, b_end) ||
          overlap(value->result.base, d_end, value->third.base, c_end))
        return PCAA_STATUS_INVALID_COMMAND;
      *opcode = ACCEL_OPCODE_MINPLUS_MAP3_PROJECT;
      *format = ACCEL_FORMAT_MINPLUS_MAP3_PROJECT;
      break;
    }
    case PCAA_ORDERED_BATCH: {
      const pcaa_batch_reference_t *value = &command->operation.batch;
      if (value->count > UINT32_MAX || value->child_bytes > UINT32_MAX)
        return PCAA_STATUS_RANGE;
      if (value->count == 0 || value->child_bytes == 0 ||
          value->child_bytes % ACCEL_COMMAND_SLOT_BYTES != 0 || value->child_descriptors == 0 ||
          value->result == 0 || value->child_bytes > UINT64_MAX - value->child_descriptors ||
          sizeof(accel_batch_result_t) > UINT64_MAX - value->result)
        return PCAA_STATUS_INVALID_COMMAND;
      *opcode = ACCEL_OPCODE_EXECUTE_BATCH;
      *format = ACCEL_FORMAT_EXECUTE_BATCH;
      break;
    }
    default:
      return PCAA_STATUS_INVALID_COMMAND;
  }
  return pcaa_wire_format_size(*opcode, *format, bytes);
}

pcaa_status_t pcaa_encoded_size(const pcaa_command_t *command, size_t *bytes) {
  if (bytes == NULL)
    return PCAA_STATUS_INVALID_ARGUMENT;
  uint8_t opcode = 0, format = 0;
  size_t result = 0;
  const pcaa_status_t status = valid_command(command, &opcode, &format, &result);
  if (status == PCAA_STATUS_OK)
    *bytes = result;
  return status;
}

pcaa_status_t pcaa_encode_one(const pcaa_command_t *command, void *destination, size_t capacity,
                              size_t *bytes_written) {
  if (destination == NULL || bytes_written == NULL)
    return PCAA_STATUS_INVALID_ARGUMENT;
  uint8_t opcode = 0, format = 0;
  size_t width = 0;
  const pcaa_status_t status = valid_command(command, &opcode, &format, &width);
  if (status != PCAA_STATUS_OK)
    return status;
  if (capacity < width)
    return PCAA_STATUS_NO_SPACE;
  unsigned char bytes[ACCEL_COMMAND_MAX_BYTES] = {0};
  bytes[0] = opcode;
  bytes[1] = format;
  switch (command->kind) {
    case PCAA_MAP_ADD_REDUCE_MIN:
    case PCAA_MAP_ADD_REDUCE_MIN_ARGMIN:
      put16(bytes, 4, (uint16_t)command->operation.reduce2.first.length);
      put64(bytes, 8, command->operation.reduce2.first.base);
      put64(bytes, 16, command->operation.reduce2.second.base);
      put64(bytes, 24, command->operation.reduce2.result);
      break;
    case PCAA_MAP_ADD3_REDUCE_MIN:
    case PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN:
      put16(bytes, 4, (uint16_t)command->operation.reduce3.first.length);
      put64(bytes, 8, command->operation.reduce3.first.base);
      put64(bytes, 16, command->operation.reduce3.second.base);
      put64(bytes, 24, command->operation.reduce3.third.base);
      put64(bytes, 32, command->operation.reduce3.result);
      break;
    case PCAA_ORDERED_BATCH:
      put64(bytes, 8, command->operation.batch.child_descriptors);
      put64(bytes, 16, command->operation.batch.result);
      put32(bytes, 24, (uint32_t)command->operation.batch.count);
      put32(bytes, 28, (uint32_t)command->operation.batch.child_bytes);
      break;
    case PCAA_COST_ADD_VECTOR: {
      const pcaa_vector_add_t *value = &command->operation.vector_add;
      const int alias0 =
          value->result.base == value->first.base && value->result.stride == value->first.stride;
      const pcaa_cost_vector_view_t first =
          !alias0 && format == ACCEL_FORMAT_COST_ADD_VECTOR_INPLACE ? value->second : value->first;
      const pcaa_cost_vector_view_t second =
          !alias0 && format == ACCEL_FORMAT_COST_ADD_VECTOR_INPLACE ? value->first : value->second;
      put16(bytes, 4, (uint16_t)first.length);
      put64(bytes, 8, first.base);
      put64(bytes, 16, second.base);
      if (format == ACCEL_FORMAT_COST_ADD_VECTOR_INPLACE) {
        put16(bytes, 24, (uint16_t)first.stride);
        put16(bytes, 26, (uint16_t)second.stride);
      } else {
        put64(bytes, 24, value->result.base);
        put16(bytes, 32, (uint16_t)first.stride);
        put16(bytes, 34, (uint16_t)second.stride);
        put16(bytes, 36, (uint16_t)value->result.stride);
      }
      break;
    }
    case PCAA_MINPLUS_PROJECT:
      put16(bytes, 4, (uint16_t)command->operation.project.matrix.columns);
      put16(bytes, 6, (uint16_t)command->operation.project.matrix.rows);
      put64(bytes, 8, command->operation.project.matrix.base);
      put64(bytes, 16, command->operation.project.vector.base);
      put64(bytes, 24, command->operation.project.result.base);
      put16(bytes, 32, (uint16_t)command->operation.project.matrix.column_stride);
      put16(bytes, 34, (uint16_t)command->operation.project.matrix.row_stride);
      put16(bytes, 36, (uint16_t)command->operation.project.vector.stride);
      put16(bytes, 38, (uint16_t)command->operation.project.result.stride);
      break;
    case PCAA_MINPLUS_MAP3_PROJECT:
      put16(bytes, 4, (uint16_t)command->operation.map3_project.third.columns);
      put16(bytes, 6, (uint16_t)command->operation.map3_project.third.rows);
      put64(bytes, 8, command->operation.map3_project.first.base);
      put64(bytes, 16, command->operation.map3_project.second.base);
      put64(bytes, 24, command->operation.map3_project.third.base);
      put64(bytes, 32, command->operation.map3_project.result.base);
      put16(bytes, 40, (uint16_t)command->operation.map3_project.first.stride);
      put16(bytes, 42, (uint16_t)command->operation.map3_project.second.stride);
      put16(bytes, 44, (uint16_t)command->operation.map3_project.third.column_stride);
      put16(bytes, 46, (uint16_t)command->operation.map3_project.third.row_stride);
      put16(bytes, 48, (uint16_t)command->operation.map3_project.result.stride);
      break;
    default:
      return PCAA_STATUS_INVALID_COMMAND;
  }
  unsigned char *output = destination;
  for (size_t offset = 0; offset < width; ++offset) output[offset] = bytes[offset];
  *bytes_written = width;
  return PCAA_STATUS_OK;
}

pcaa_status_t pcaa_decode_one(const void *source, size_t available_bytes, pcaa_command_t *command,
                              size_t *bytes_consumed) {
  if (source == NULL || command == NULL || bytes_consumed == NULL)
    return PCAA_STATUS_INVALID_ARGUMENT;
  if (available_bytes < ACCEL_COMMAND_HEADER_BYTES)
    return PCAA_STATUS_INVALID_COMMAND;
  const unsigned char *bytes = source;
  size_t width = 0;
  if (pcaa_wire_format_size(bytes[0], bytes[1], &width) != PCAA_STATUS_OK ||
      available_bytes < width || !zero_bytes(bytes, 2, 4))
    return PCAA_STATUS_INVALID_COMMAND;
  const size_t n = get16(bytes, 4);
  const size_t m = get16(bytes, 6);
  pcaa_command_t value = {0};
  switch (bytes[1]) {
    case ACCEL_FORMAT_REDUCE2:
    case ACCEL_FORMAT_REDUCE2_ARGMIN:
      if (m != 0)
        return PCAA_STATUS_INVALID_COMMAND;
      value.kind = bytes[1] == ACCEL_FORMAT_REDUCE2 ? PCAA_MAP_ADD_REDUCE_MIN
                                                    : PCAA_MAP_ADD_REDUCE_MIN_ARGMIN;
      value.operation.reduce2.first = pcaa_cost_vector(get64(bytes, 8), n, 1);
      value.operation.reduce2.second = pcaa_cost_vector(get64(bytes, 16), n, 1);
      value.operation.reduce2.result = get64(bytes, 24);
      break;
    case ACCEL_FORMAT_REDUCE3:
    case ACCEL_FORMAT_REDUCE3_ARGMIN:
      if (m != 0 || !zero_bytes(bytes, 40, 48))
        return PCAA_STATUS_INVALID_COMMAND;
      value.kind = bytes[1] == ACCEL_FORMAT_REDUCE3 ? PCAA_MAP_ADD3_REDUCE_MIN
                                                    : PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN;
      value.operation.reduce3.first = pcaa_cost_vector(get64(bytes, 8), n, 1);
      value.operation.reduce3.second = pcaa_cost_vector(get64(bytes, 16), n, 1);
      value.operation.reduce3.third = pcaa_cost_vector(get64(bytes, 24), n, 1);
      value.operation.reduce3.result = get64(bytes, 32);
      break;
    case ACCEL_FORMAT_EXECUTE_BATCH:
      if (n != 0 || m != 0)
        return PCAA_STATUS_INVALID_COMMAND;
      value.kind = PCAA_ORDERED_BATCH;
      value.operation.batch.child_descriptors = get64(bytes, 8);
      value.operation.batch.result = get64(bytes, 16);
      value.operation.batch.count = get32(bytes, 24);
      value.operation.batch.child_bytes = get32(bytes, 28);
      break;
    case ACCEL_FORMAT_COST_ADD_VECTOR_GENERAL:
      if (m != 0 || !zero_bytes(bytes, 38, 48))
        return PCAA_STATUS_INVALID_COMMAND;
      value.kind = PCAA_COST_ADD_VECTOR;
      value.operation.vector_add.first = pcaa_cost_vector(get64(bytes, 8), n, get16(bytes, 32));
      value.operation.vector_add.second = pcaa_cost_vector(get64(bytes, 16), n, get16(bytes, 34));
      value.operation.vector_add.result = pcaa_cost_output(get64(bytes, 24), n, get16(bytes, 36));
      break;
    case ACCEL_FORMAT_COST_ADD_VECTOR_INPLACE:
      if (m != 0 || !zero_bytes(bytes, 28, 32))
        return PCAA_STATUS_INVALID_COMMAND;
      value.kind = PCAA_COST_ADD_VECTOR;
      value.operation.vector_add.first = pcaa_cost_vector(get64(bytes, 8), n, get16(bytes, 24));
      value.operation.vector_add.second = pcaa_cost_vector(get64(bytes, 16), n, get16(bytes, 26));
      value.operation.vector_add.result = pcaa_cost_output(get64(bytes, 8), n, get16(bytes, 24));
      break;
    case ACCEL_FORMAT_MINPLUS_PROJECT:
      if (!zero_bytes(bytes, 40, 48))
        return PCAA_STATUS_INVALID_COMMAND;
      value.kind = PCAA_MINPLUS_PROJECT;
      value.operation.project.matrix =
          pcaa_cost_matrix(get64(bytes, 8), m, n, get16(bytes, 34), get16(bytes, 32));
      value.operation.project.vector = pcaa_cost_vector(get64(bytes, 16), n, get16(bytes, 36));
      value.operation.project.result = pcaa_cost_output(get64(bytes, 24), m, get16(bytes, 38));
      break;
    case ACCEL_FORMAT_MINPLUS_MAP3_PROJECT:
      if (!zero_bytes(bytes, 50, 64))
        return PCAA_STATUS_INVALID_COMMAND;
      value.kind = PCAA_MINPLUS_MAP3_PROJECT;
      value.operation.map3_project.first = pcaa_cost_vector(get64(bytes, 8), n, get16(bytes, 40));
      value.operation.map3_project.second = pcaa_cost_vector(get64(bytes, 16), n, get16(bytes, 42));
      value.operation.map3_project.third =
          pcaa_cost_matrix(get64(bytes, 24), m, n, get16(bytes, 46), get16(bytes, 44));
      value.operation.map3_project.result =
          pcaa_argmin_output(get64(bytes, 32), m, get16(bytes, 48));
      break;
    default:
      return PCAA_STATUS_INVALID_COMMAND;
  }
  uint8_t opcode = 0, format = 0;
  size_t canonical = 0;
  if (valid_command(&value, &opcode, &format, &canonical) != PCAA_STATUS_OK || opcode != bytes[0] ||
      format != bytes[1] || canonical != width)
    return PCAA_STATUS_INVALID_COMMAND;
  *command = value;
  *bytes_consumed = width;
  return PCAA_STATUS_OK;
}

pcaa_status_t pcaa_encoded_stream_size(const pcaa_command_t *commands, size_t count,
                                       size_t *bytes) {
  if (commands == NULL || count == 0 || bytes == NULL)
    return PCAA_STATUS_INVALID_ARGUMENT;
  if (count > UINT32_MAX)
    return PCAA_STATUS_RANGE;
  size_t total = 0;
  for (size_t index = 0; index < count; ++index) {
    if (commands[index].kind == PCAA_ORDERED_BATCH)
      return PCAA_STATUS_INVALID_COMMAND;
    size_t width = 0;
    const pcaa_status_t status = pcaa_encoded_size(&commands[index], &width);
    if (status != PCAA_STATUS_OK)
      return status;
    if (width > UINT32_MAX - total)
      return PCAA_STATUS_RANGE;
    total += width;
  }
  *bytes = total;
  return PCAA_STATUS_OK;
}

pcaa_status_t pcaa_encode_stream(const pcaa_command_t *commands, size_t count, void *destination,
                                 size_t capacity, size_t *bytes_written) {
  if (destination == NULL || bytes_written == NULL)
    return PCAA_STATUS_INVALID_ARGUMENT;
  size_t total = 0;
  const pcaa_status_t status = pcaa_encoded_stream_size(commands, count, &total);
  if (status != PCAA_STATUS_OK)
    return status;
  if (capacity < total)
    return PCAA_STATUS_NO_SPACE;
  unsigned char *cursor = destination;
  for (size_t index = 0; index < count; ++index) {
    size_t width = 0;
    const pcaa_status_t encoded = pcaa_encode_one(&commands[index], cursor, capacity, &width);
    if (encoded != PCAA_STATUS_OK)
      return encoded;
    cursor += width;
    capacity -= width;
  }
  *bytes_written = total;
  return PCAA_STATUS_OK;
}
