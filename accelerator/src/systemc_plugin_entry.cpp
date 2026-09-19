// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Provides SystemC's required entry symbol when the accelerator is loaded by Spike.

#include <sysc/kernel/sc_externs.h>

/*
 * Spike owns the host process and loads this code as a shared object, so it
 * never invokes sc_main. The definition satisfies libsystemc's required
 * symbol; host-side tests have their own sc_main and explicitly start the
 * SystemC kernel.
 */
int sc_main(int, char **) {
  return 0;
}
