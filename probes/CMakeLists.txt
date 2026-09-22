# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Host-only generality probes: non-PBQP cost-algebra workloads expressed in
# terms of PCAA's existing conceptual primitives, to stress-test how much of
# the current ISA is PBQP-specific versus generically reusable. No RTL, ABI,
# or SystemC involvement; see doc/generality-probe.md.

add_library(pcaa_probes STATIC src/bellman_ford.cpp)
target_include_directories(pcaa_probes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(pcaa_probes PUBLIC pcaa_cost_math)

add_executable(probes_unit tests/bellman_ford_unit.cpp)
target_link_libraries(probes_unit PRIVATE GTest::gtest_main pcaa_probes)
add_test(NAME probes_unit COMMAND probes_unit)

pcaa_add_format_sources(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/bellman_ford.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/bellman_ford.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/bellman_ford_unit.cpp)
