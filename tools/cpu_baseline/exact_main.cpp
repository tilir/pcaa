// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Enters the native exact benchmark without linking SystemC.
#include "exact.h"
int main(int argc, char **argv) {
  return cpu_baseline::RunExact(argc, argv);
}
