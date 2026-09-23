// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Declares hosted stderr diagnostics for PCAA statuses.

#pragma once

#include "pcaa.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Prints "prefix: status text\n" to stderr, or just the text for a null/empty prefix. */
void pcaa_perror(const char *prefix, pcaa_status_t status);

#ifdef __cplusplus
}  // extern "C"
#endif
