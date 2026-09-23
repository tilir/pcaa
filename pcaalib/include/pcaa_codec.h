// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Declares the current PCAA descriptor codec at the semantic/device boundary.

#pragma once

#include "accel_protocol.h"
#include "pcaa.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* These functions never access guest memory or perform cost arithmetic. */
pcaa_status_t pcaa_encode_descriptor(const pcaa_command_t *command, accel_command_t *wire);
pcaa_status_t pcaa_decode_descriptor(const accel_command_t *wire, pcaa_command_t *command);
size_t pcaa_descriptor_bytes(void);
size_t pcaa_batch_descriptor_bytes(size_t child_count);

/* Output byte-set intersection, used to protect active descriptor storage. */
int pcaa_output_overlaps(const pcaa_command_t *command, pcaa_guest_address_t begin,
                         pcaa_guest_address_t end);

#ifdef __cplusplus
}  // extern "C"
#endif
