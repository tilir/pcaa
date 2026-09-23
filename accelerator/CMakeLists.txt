# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Defines host libraries for the PCAA protocol, timing model, and SystemC core.

add_library(pcaa_cost_math STATIC src/cost_math.cpp)
set_target_properties(pcaa_cost_math PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(pcaa_cost_math PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(pcaa_timing STATIC src/timing_model.cpp)
set_target_properties(pcaa_timing PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(pcaa_timing PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${SYSTEMC_INCLUDE_DIR})
target_link_libraries(pcaa_timing PUBLIC pcaalib ${SYSTEMC_LIBRARY})

add_library(pcaa_core STATIC src/accelerator.cpp)
set_target_properties(pcaa_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(pcaa_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${SYSTEMC_INCLUDE_DIR})
target_link_libraries(pcaa_core PUBLIC pcaa_cost_math pcaa_timing pcaalib ${SYSTEMC_LIBRARY})

pcaa_add_format_sources(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/accel_protocol.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/accelerator.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/cost_math.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/memory_interface.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/timing_model.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/accelerator.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/timing_model.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cost_math.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/spike_device.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/systemc_plugin_entry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/systemc_unit.cpp)

pcaa_add_baremetal_sources(${CMAKE_CURRENT_SOURCE_DIR}/src/cost_math.cpp)
pcaa_add_baremetal_headers(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/accel_protocol.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/cost_math.h)
