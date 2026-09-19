# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Builds the host accelerator model, Spike plugin, and bare-metal test ELFs.

cmake_minimum_required(VERSION 3.20)

project(pcaa LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

find_path(SYSTEMC_INCLUDE_DIR systemc REQUIRED)
find_library(SYSTEMC_LIBRARY systemc REQUIRED)

add_library(pcaa_cost_math STATIC accelerator/src/cost_math.cpp)
set_target_properties(pcaa_cost_math PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(pcaa_cost_math
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/accelerator/include
)

add_library(pcaa_core STATIC accelerator/src/accelerator.cpp)
set_target_properties(pcaa_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(pcaa_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/accelerator/include
    ${SYSTEMC_INCLUDE_DIR}
)
target_link_libraries(pcaa_core PUBLIC pcaa_cost_math ${SYSTEMC_LIBRARY})

add_executable(systemc_unit accelerator/tests/systemc_unit.cpp)
target_link_libraries(systemc_unit PRIVATE pcaa_core)
add_test(NAME systemc_unit COMMAND systemc_unit)

set(SPIKE_SOURCE_DIR "" CACHE PATH
  "Path to the riscv-isa-sim source tree; enables the Spike plugin target")
if(SPIKE_SOURCE_DIR)
  add_library(pcaa_spike_device SHARED
    accelerator/src/spike_device.cpp
    accelerator/src/systemc_plugin_entry.cpp
  )
  target_include_directories(pcaa_spike_device
    PRIVATE
      ${SPIKE_SOURCE_DIR}/riscv
      ${CMAKE_CURRENT_SOURCE_DIR}/accelerator/include
  )
  target_link_libraries(pcaa_spike_device PRIVATE pcaa_core)
  set_target_properties(pcaa_spike_device PROPERTIES OUTPUT_NAME pcaa_spike_device)
endif()

set(RISCV_PREFIX "riscv64-unknown-elf-" CACHE STRING
  "Prefix of the bare-metal RISC-V toolchain")
find_program(RISCV_GCC NAMES ${RISCV_PREFIX}gcc)
find_program(CLANG_FORMAT NAMES clang-format)
find_program(INCLUDE_WHAT_YOU_USE NAMES include-what-you-use)
find_program(IWYU_TOOL NAMES iwyu_tool.py iwyu_tool)

set(PCAA_FORMAT_SOURCES
  accelerator/include/accel_protocol.h
  accelerator/include/accelerator.h
  accelerator/include/cost_math.h
  accelerator/include/memory_interface.h
  accelerator/src/accelerator.cpp
  accelerator/src/cost_math.cpp
  accelerator/src/spike_device.cpp
  accelerator/src/systemc_plugin_entry.cpp
  accelerator/tests/systemc_unit.cpp
  software/accel_driver.c
  software/accel_driver.h
  software/tests/basic.c
  software/tests/common.h
  software/tests/randomized.c
)

if(CLANG_FORMAT AND INCLUDE_WHAT_YOU_USE AND IWYU_TOOL)
  add_custom_target(format
    COMMAND ${CLANG_FORMAT} -i ${PCAA_FORMAT_SOURCES}
    COMMAND ${IWYU_TOOL} -p ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS
      pcaa_cost_math
      pcaa_core
      systemc_unit
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Formatting PCAA sources and checking include hygiene"
    VERBATIM
  )
else()
  add_custom_target(format
    COMMAND ${CMAKE_COMMAND} -E echo
      "clang-format, include-what-you-use, and iwyu_tool are required for format"
    COMMAND ${CMAKE_COMMAND} -E false
    COMMENT "Formatting PCAA sources and checking include hygiene"
    VERBATIM
  )
endif()

function(add_baremetal_test test_name)
  set(output_elf ${CMAKE_CURRENT_BINARY_DIR}/${test_name}.elf)
  set(test_source ${CMAKE_CURRENT_SOURCE_DIR}/software/tests/${test_name}.c)

  add_custom_command(
    OUTPUT ${output_elf}
    COMMAND ${RISCV_GCC}
      -march=rv64imac
      -mabi=lp64
      -mcmodel=medany
      -std=c11
      -ffreestanding
      -nostdlib
      -nostartfiles
      -O2
      -I${CMAKE_CURRENT_SOURCE_DIR}/accelerator/include
      -I${CMAKE_CURRENT_SOURCE_DIR}/software
      ${CMAKE_CURRENT_SOURCE_DIR}/software/tests/start.S
      ${CMAKE_CURRENT_SOURCE_DIR}/software/accel_driver.c
      ${test_source}
      -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/software/tests/linker.ld
      -Wl,--build-id=none
      -o ${output_elf}
    DEPENDS
      accelerator/include/accel_protocol.h
      software/accel_driver.c
      software/accel_driver.h
      software/tests/common.h
      software/tests/linker.ld
      software/tests/start.S
      ${test_source}
    VERBATIM
  )
  add_custom_target(${test_name}_elf DEPENDS ${output_elf})
endfunction()

if(RISCV_GCC)
  add_baremetal_test(basic)
  add_baremetal_test(randomized)
else()
  message(STATUS "Bare-metal test targets disabled: ${RISCV_PREFIX}gcc was not found")
endif()
