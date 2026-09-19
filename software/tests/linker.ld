/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 PCAA contributors
 * Places bare-metal tests in Spike RAM and publishes the conventional host symbols. */
OUTPUT_ARCH(riscv)
RAM_BASE = 0x80000000;
TOHOST_ADDRESS = RAM_BASE + 0x1000;
FROMHOST_ADDRESS = TOHOST_ADDRESS + 8;
tohost = TOHOST_ADDRESS;
fromhost = FROMHOST_ADDRESS;
ENTRY(_start)
SECTIONS { . = RAM_BASE; .text : { *(.text.init) *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }
