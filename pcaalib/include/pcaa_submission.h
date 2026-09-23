// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Exposes encoding-neutral submission storage and the selected wire codec.

#pragma once

#include "accel_protocol.h"
#include "pcaa.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Caller-owned aligned maximum storage; capacity is not encoded length. */
enum { PCAA_ENCODED_SLOT_WORDS = ACCEL_COMMAND_MAX_BYTES / sizeof(uint64_t) };
typedef union pcaa_encoded_slot {
  uint64_t alignment;
  unsigned char bytes[PCAA_ENCODED_SLOT_WORDS * sizeof(uint64_t)];
} pcaa_encoded_slot_t;

pcaa_status_t pcaa_encode_batch(pcaa_guest_address_t child_descriptors, size_t count,
                                size_t child_bytes, pcaa_guest_address_t result,
                                pcaa_encoded_slot_t *output, size_t *bytes_written);

#ifdef __cplusplus
}  // extern "C"
#endif
