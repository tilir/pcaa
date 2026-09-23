// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Declares the selected compact PCAA codec at the semantic/device boundary.

#pragma once

#include "accel_protocol.h"
#include "pcaa.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* These functions never access guest memory or perform cost arithmetic. */
pcaa_status_t pcaa_wire_format_size(uint8_t opcode, uint8_t format, size_t *bytes);
pcaa_status_t pcaa_encoded_size(const pcaa_command_t *command, size_t *bytes);
pcaa_status_t pcaa_encoded_stream_size(const pcaa_command_t *commands, size_t count, size_t *bytes);
pcaa_status_t pcaa_encode_one(const pcaa_command_t *command, void *destination, size_t capacity,
                              size_t *bytes_written);
pcaa_status_t pcaa_decode_one(const void *source, size_t available_bytes, pcaa_command_t *command,
                              size_t *bytes_consumed);
pcaa_status_t pcaa_encode_stream(const pcaa_command_t *commands, size_t count, void *destination,
                                 size_t capacity, size_t *bytes_written);

/* Output byte-set intersection, used to protect active descriptor storage. */
int pcaa_output_overlaps(const pcaa_command_t *command, pcaa_guest_address_t begin,
                         pcaa_guest_address_t end);

#ifdef __cplusplus
}  // extern "C"
#endif
