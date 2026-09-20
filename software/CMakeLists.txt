# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Defines the hosted PBQP solver library and registers software source formatting.

add_library(pcaa_pbqp STATIC pbqp/pbqp.cpp)
target_include_directories(pcaa_pbqp
  PUBLIC ${PROJECT_SOURCE_DIR}/accelerator/include ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pcaa_pbqp PUBLIC pcaa_cost_math)

pcaa_add_format_sources(
  ${CMAKE_CURRENT_SOURCE_DIR}/accel_driver.c
  ${CMAKE_CURRENT_SOURCE_DIR}/accel_driver.h
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/basic.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/batch.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/common.h
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/randomized.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/pbqp_basic.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/pbqp_randomized.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/pbqp_rn.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/pbqp_reference.h
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp/pbqp.h
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp/pbqp.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp/pbqp_accelerator.c
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp/pbqp_accelerator.h)

pcaa_add_baremetal_sources(
  ${CMAKE_CURRENT_SOURCE_DIR}/accel_driver.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/start.S
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime.c
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp/pbqp.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp/pbqp_accelerator.c)
pcaa_add_baremetal_headers(
  ${CMAKE_CURRENT_SOURCE_DIR}/accel_driver.h
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/common.h
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/linker.ld
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/pbqp_reference.h
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp/pbqp.h
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp/pbqp_accelerator.h)
