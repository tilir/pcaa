# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Defines isolated native CPU benchmarks, tests and source formatting.

# An isolated host solver copy enables profiling and degree caching without
# adding hosted state, clocks, or containers to the freestanding solver target.
add_library(pcaa_cpu_solver STATIC
  ${PROJECT_SOURCE_DIR}/software/pbqp/pbqp.cpp
  ${PROJECT_SOURCE_DIR}/software/pbqp/pbqp_storage.cpp)
target_compile_definitions(pcaa_cpu_solver PRIVATE PCAA_CPU_EXPERIMENT)
target_include_directories(pcaa_cpu_solver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/software ${PROJECT_SOURCE_DIR}/accelerator/include)
add_library(pcaa_cpu_kernels STATIC kernels.cpp)
target_include_directories(pcaa_cpu_kernels PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/software ${PROJECT_SOURCE_DIR}/accelerator/include)
target_link_libraries(pcaa_cpu_kernels PUBLIC pcaa_cost_math)
target_link_libraries(pcaa_cpu_solver PUBLIC pcaa_cpu_kernels)
option(PCAA_CPU_AVX2 "Enable AVX2 in the isolated native CPU experiment" OFF)
if(PCAA_CPU_AVX2)
  target_compile_options(pcaa_cpu_kernels PRIVATE -mavx2)
endif()
add_executable(pcaa_cpu_bench bench.cpp)
target_link_libraries(pcaa_cpu_bench PRIVATE pcaa_cpu_solver pcaa_graph_input)
add_executable(pcaa_cpu_current bench.cpp)
target_link_libraries(pcaa_cpu_current PRIVATE pcaa_pbqp pcaa_cpu_kernels pcaa_graph_input)
add_executable(pcaa_cpu_kernel_unit kernel_unit.cpp)
target_link_libraries(pcaa_cpu_kernel_unit PRIVATE pcaa_cpu_solver GTest::gtest_main)
add_test(NAME cpu_kernel_unit COMMAND pcaa_cpu_kernel_unit)
add_executable(pcaa_cpu_microbench microbench.cpp)
target_link_libraries(pcaa_cpu_microbench PRIVATE pcaa_cpu_solver pcaa_graph_input)
pcaa_add_format_sources(
  ${CMAKE_CURRENT_SOURCE_DIR}/profile.h
  ${CMAKE_CURRENT_SOURCE_DIR}/kernels.h
  ${CMAKE_CURRENT_SOURCE_DIR}/kernels.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/bench.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel_unit.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/microbench.cpp)

foreach(cpu_variant current scalar dense structured)
  add_test(NAME cpu_solver_${cpu_variant}
    COMMAND pcaa_cpu_bench ${cpu_variant} ${PROJECT_SOURCE_DIR}/examples/petersen.pbqp 3)
  set_tests_properties(cpu_solver_${cpu_variant} PROPERTIES
    PASS_REGULAR_EXPRESSION "\"rn\":[1-9]")
endforeach()
