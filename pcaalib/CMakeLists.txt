# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Owns the platform-neutral semantic command library and descriptor codec.

add_library(pcaalib STATIC src/pcaa.c src/submission.c src/device.c)
set_target_properties(pcaalib PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(pcaalib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/accelerator/include)

add_library(pcaalib_host STATIC src/error_host.c)
target_include_directories(pcaalib_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(pcaalib_host PUBLIC pcaalib)

add_library(pcaalib_systemc STATIC src/systemc_device.cpp)
target_include_directories(pcaalib_systemc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/accelerator/include ${SYSTEMC_INCLUDE_DIR})
target_link_libraries(pcaalib_systemc PUBLIC pcaalib_host ${SYSTEMC_LIBRARY})

pcaa_add_format_sources(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/pcaa.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/pcaa_submission.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/pcaa_device.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/pcaa_host_error.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/pcaa_baremetal_device.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/pcaa_systemc_device.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/pcaa_codec.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/pcaa.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/submission.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/device.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/error_host.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/baremetal_device.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/systemc_device.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/pcaa_unit.cpp)
pcaa_add_baremetal_c_sources(${CMAKE_CURRENT_SOURCE_DIR}/src/pcaa.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/submission.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/device.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/baremetal_device.c)
pcaa_add_baremetal_include_dirs(${CMAKE_CURRENT_SOURCE_DIR}/include)
pcaa_add_baremetal_sources(${CMAKE_CURRENT_SOURCE_DIR}/src/pcaa.c)
pcaa_add_baremetal_headers(${CMAKE_CURRENT_SOURCE_DIR}/include/pcaa.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/pcaa_submission.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/pcaa_device.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/pcaa_baremetal_device.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/pcaa_codec.h)

add_executable(pcaa_unit tests/pcaa_unit.cpp)
target_link_libraries(pcaa_unit PRIVATE GTest::gtest_main pcaalib_host)
add_test(NAME pcaa_unit COMMAND pcaa_unit)
