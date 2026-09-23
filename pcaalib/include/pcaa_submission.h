// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Exposes encoding-neutral submission storage and the selected wire codec.

#pragma once

#include "pcaa.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Caller-owned aligned storage, currently sufficient for one descriptor.
 * Compact encodings can use the same storage without changing clients. */
enum { PCAA_ENCODED_SLOT_WORDS = 10 };
typedef union pcaa_encoded_slot {
  uint64_t alignment;
  unsigned char bytes[PCAA_ENCODED_SLOT_WORDS * sizeof(uint64_t)];
} pcaa_encoded_slot_t;

size_t pcaa_encoded_command_bytes(void);
size_t pcaa_encoded_batch_bytes(size_t child_count);
pcaa_status_t pcaa_encode_command(const pcaa_command_t *command, pcaa_encoded_slot_t *output);
pcaa_status_t pcaa_encode_commands(const pcaa_command_t *commands, size_t count, void *output,
                                   size_t capacity);
pcaa_status_t pcaa_encode_batch(pcaa_guest_address_t child_descriptors, size_t count,
                                pcaa_guest_address_t result, pcaa_encoded_slot_t *output);

#ifdef __cplusplus
}  // extern "C"
#endif
