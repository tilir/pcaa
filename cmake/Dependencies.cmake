# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Finds host tools and libraries shared by PCAA target groups.

find_path(SYSTEMC_INCLUDE_DIR systemc REQUIRED)
find_library(SYSTEMC_LIBRARY systemc REQUIRED)
find_package(GTest REQUIRED)
find_program(RUBY_EXECUTABLE ruby REQUIRED)
find_program(CLANG_FORMAT NAMES clang-format)
find_program(INCLUDE_WHAT_YOU_USE NAMES include-what-you-use)
find_program(IWYU_TOOL NAMES iwyu_tool.py iwyu_tool)

set(RISCV_PREFIX "riscv64-unknown-elf-" CACHE STRING
  "Prefix of the bare-metal RISC-V toolchain")
find_program(RISCV_GCC NAMES ${RISCV_PREFIX}gcc)
find_program(RISCV_GXX NAMES ${RISCV_PREFIX}g++)
