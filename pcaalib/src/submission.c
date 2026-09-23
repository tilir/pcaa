// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Builds a compact parent descriptor for an ordered child stream.

#include "pcaa_submission.h"
#include "pcaa.h"
#include "pcaa_codec.h"

#include <stddef.h>

pcaa_status_t pcaa_encode_batch(pcaa_guest_address_t child_descriptors, size_t count,
                                size_t child_bytes, pcaa_guest_address_t result,
                                pcaa_encoded_slot_t *output, size_t *bytes_written) {
  if (output == NULL || bytes_written == NULL)
    return PCAA_STATUS_INVALID_ARGUMENT;
  pcaa_command_t command;
  const pcaa_status_t status =
      pcaa_make_ordered_batch(child_descriptors, count, child_bytes, result, &command);
  if (status != PCAA_STATUS_OK)
    return status;
  return pcaa_encode_one(&command, output->bytes, sizeof(output->bytes), bytes_written);
}
