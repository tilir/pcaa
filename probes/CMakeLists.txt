# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Host-only non-PBQP workloads with both an independent software oracle and
# execution through pcaalib's hosted SystemC backend; see doc/generality-probe.md.

add_library(pcaa_probes STATIC src/bellman_ford.cpp)
target_include_directories(pcaa_probes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/accelerator/include)

add_executable(probes_unit tests/bellman_ford_unit.cpp)
target_link_libraries(probes_unit PRIVATE GTest::gtest_main pcaa_probes)
add_test(NAME probes_unit COMMAND probes_unit)

add_executable(probes_device_unit tests/bellman_ford_device_unit.cpp)
target_link_libraries(probes_device_unit PRIVATE GTest::gtest pcaa_probes pcaa_core
  pcaalib_systemc)
add_test(NAME probes_device_unit COMMAND probes_device_unit)

pcaa_add_format_sources(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/bellman_ford.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/bellman_ford.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/bellman_ford_unit.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/bellman_ford_device_unit.cpp)
