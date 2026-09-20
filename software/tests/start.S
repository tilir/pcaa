# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Minimal bare-metal entry point and fallback termination path for Spike tests.

.equ TOHOST_EXIT_SHIFT, 1
.equ TOHOST_EXIT_VALID, 1
.equ STACK_SIZE, 1048576

.section .text.init
.globl _start
_start:
  la sp, _stack_top
  call main
  li a0, 1
  slli a0, a0, 1
  ori a0, a0, TOHOST_EXIT_VALID
  la t0, tohost
  sd a0, 0(t0)
1: j 1b
.section .bss
.space STACK_SIZE
.globl _stack_top
_stack_top:
