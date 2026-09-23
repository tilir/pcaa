// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Packs semantic commands for submission without exposing descriptor layout to clients.

#include "pcaa_submission.h"
#include "pcaa.h"
#include "accel_protocol.h"
#include "pcaa_codec.h"

#include <stddef.h>

static void copy_bytes(void *destination, const void *source, size_t size) {
  unsigned char *target = destination;
  const unsigned char *origin = source;
  for (size_t index = 0; index < size; ++index) target[index] = origin[index];
}

size_t pcaa_encoded_command_bytes(void) {
  return pcaa_descriptor_bytes();
}

size_t pcaa_encoded_batch_bytes(size_t child_count) {
  return pcaa_batch_descriptor_bytes(child_count);
}

pcaa_status_t pcaa_encode_command(const pcaa_command_t *command, pcaa_encoded_slot_t *output) {
  if (command == NULL || output == NULL)
    return PCAA_STATUS_INVALID_ARGUMENT;
  if (pcaa_encoded_command_bytes() > sizeof(*output))
    return PCAA_STATUS_NO_SPACE;
  accel_command_t wire;
  const pcaa_status_t status = pcaa_encode_descriptor(command, &wire);
  if (status != PCAA_STATUS_OK)
    return status;
  copy_bytes(output->bytes, &wire, sizeof(wire));
  return PCAA_STATUS_OK;
}

pcaa_status_t pcaa_encode_commands(const pcaa_command_t *commands, size_t count, void *output,
                                   size_t capacity) {
  const size_t width = pcaa_encoded_command_bytes();
  if (commands == NULL || output == NULL || count == 0)
    return PCAA_STATUS_INVALID_ARGUMENT;
  if (width == 0 || count > capacity / width)
    return PCAA_STATUS_NO_SPACE;
  unsigned char *bytes = output;
  for (size_t index = 0; index < count; ++index) {
    pcaa_encoded_slot_t encoded;
    if (commands[index].kind == PCAA_ORDERED_BATCH)
      return PCAA_STATUS_INVALID_COMMAND;
    const pcaa_status_t status = pcaa_encode_command(&commands[index], &encoded);
    if (status != PCAA_STATUS_OK)
      return status;
    copy_bytes(bytes + index * width, encoded.bytes, width);
  }
  return PCAA_STATUS_OK;
}

pcaa_status_t pcaa_encode_batch(pcaa_guest_address_t child_descriptors, size_t count,
                                pcaa_guest_address_t result, pcaa_encoded_slot_t *output) {
  pcaa_command_t command;
  const pcaa_status_t status = pcaa_make_ordered_batch(child_descriptors, count, result, &command);
  if (status != PCAA_STATUS_OK)
    return status;
  return pcaa_encode_command(&command, output);
}
