# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Configures the PCAA project and delegates target groups to CMake modules.

cmake_minimum_required(VERSION 3.20)

project(pcaa VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

include(CTest)

function(pcaa_add_format_sources)
  set_property(GLOBAL APPEND PROPERTY PCAA_FORMAT_SOURCES ${ARGN})
endfunction()

function(pcaa_add_baremetal_sources)
  set_property(GLOBAL APPEND PROPERTY PCAA_BAREMETAL_SOURCES ${ARGN})
endfunction()

function(pcaa_add_baremetal_headers)
  set_property(GLOBAL APPEND PROPERTY PCAA_BAREMETAL_HEADERS ${ARGN})
endfunction()

include(cmake/Dependencies.cmake)
add_subdirectory(accelerator)
add_subdirectory(software)
add_subdirectory(workload)
add_subdirectory(tools)
include(cmake/Tests.cmake)
include(cmake/Spike.cmake)
include(cmake/BareMetal.cmake)
include(cmake/Formatting.cmake)
