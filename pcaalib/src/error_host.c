// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Prints PCAA status messages on hosted systems only.

#include "pcaa_host_error.h"
#include "pcaa.h"

#include <stdio.h>

void pcaa_perror(const char *prefix, pcaa_status_t status) {
  if (prefix != NULL && prefix[0] != '\0')
    fprintf(stderr, "%s: %s\n", prefix, pcaa_status_string(status));
  else
    fprintf(stderr, "%s\n", pcaa_status_string(status));
}
