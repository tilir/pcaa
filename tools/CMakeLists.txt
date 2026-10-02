# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Defines host graph-runner, generator, and reproducible characterization targets.

add_library(pcaa_runner_kernel STATIC pbqp_model_kernel.cpp)
target_link_libraries(pcaa_runner_kernel PUBLIC pcaa_core pcaa_l2 pcaa_pbqp pcaalib_systemc)
target_include_directories(pcaa_runner_kernel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(pbqp_model_kernel_unit tests/pbqp_model_kernel_unit.cpp)
target_link_libraries(pbqp_model_kernel_unit PRIVATE pcaa_runner_kernel GTest::gtest)
add_test(NAME pbqp_model_kernel_unit COMMAND pbqp_model_kernel_unit)

add_executable(pbqp_graph_generate pbqp_graph_generate.cpp)
target_link_libraries(pbqp_graph_generate PRIVATE pcaa_graph_generator)

add_executable(pcaa_graph_run_model pbqp_run.cpp)
target_compile_definitions(pcaa_graph_run_model PRIVATE PCAA_VERSION="${PROJECT_VERSION}")
target_link_libraries(pcaa_graph_run_model PRIVATE pcaa_runner_kernel)

add_executable(pcaa_graph_run pbqp_run_launcher.cpp)
add_dependencies(pcaa_graph_run pcaa_graph_run_model)

add_executable(pcaa_graph_run_timed_model pbqp_run.cpp)
target_compile_definitions(pcaa_graph_run_timed_model PRIVATE
  PCAA_GRAPH_RUN_TIMED
  PCAA_VERSION="${PROJECT_VERSION}")
target_link_libraries(pcaa_graph_run_timed_model PRIVATE pcaa_runner_kernel)

add_executable(pcaa_graph_run_timed pbqp_run_launcher.cpp)
target_compile_definitions(pcaa_graph_run_timed PRIVATE
  PCAA_GRAPH_RUN_MODEL_NAME="pcaa_graph_run_timed_model")
add_dependencies(pcaa_graph_run_timed pcaa_graph_run_timed_model)

add_custom_target(solver_characterization
  COMMAND ${RUBY_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/rn_characterize.rb
    --runner $<TARGET_FILE:pcaa_graph_run>
    --generator $<TARGET_FILE:pbqp_graph_generate>
    --output ${PROJECT_BINARY_DIR}/solver-characterization.csv
    --summary ${PROJECT_BINARY_DIR}/solver-characterization-summary.md
  DEPENDS pcaa_graph_run pbqp_graph_generate
  BYPRODUCTS ${PROJECT_BINARY_DIR}/solver-characterization.csv
             ${PROJECT_BINARY_DIR}/solver-characterization-summary.md
  COMMENT "Characterizing deterministic synthetic solver workloads through the CLI")

add_custom_target(rn_characterization DEPENDS solver_characterization)

add_test(NAME solver_characterization_smoke
  COMMAND ${RUBY_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/rn_characterize.rb
    --runner $<TARGET_FILE:pcaa_graph_run>
    --generator $<TARGET_FILE:pbqp_graph_generate>
    --seeds 1001
    --output ${PROJECT_BINARY_DIR}/solver-characterization-smoke.csv
    --summary ${PROJECT_BINARY_DIR}/solver-characterization-smoke.md)
set_tests_properties(solver_characterization_smoke
  PROPERTIES PASS_REGULAR_EXPRESSION "rows=88 reduce-only-irreducible=")

add_custom_target(hw_sw_characterization
  COMMAND ${CMAKE_COMMAND} -E env
    PCAA_RUNNER=$<TARGET_FILE:pcaa_graph_run>
    PCAA_GENERATOR=$<TARGET_FILE:pbqp_graph_generate>
    ${RUBY_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/hw_sw_characterize.rb
    ${PROJECT_BINARY_DIR}/hw-sw-epochs.csv
  DEPENDS pcaa_graph_run pbqp_graph_generate
  BYPRODUCTS ${PROJECT_BINARY_DIR}/hw-sw-epochs.csv
  COMMENT "Characterizing hypothetical HW/SW execution epochs through solver traces")

add_custom_target(scaling_characterization
  COMMAND ${RUBY_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/scaling_characterize.rb
    --runner $<TARGET_FILE:pcaa_graph_run>
    --generator $<TARGET_FILE:pbqp_graph_generate>
    --resume
    ${PROJECT_BINARY_DIR}/scaling-runs.csv
  DEPENDS pcaa_graph_run pbqp_graph_generate
  BYPRODUCTS ${PROJECT_BINARY_DIR}/scaling-runs.csv
  COMMENT "Recording supported PBQP scaling points and capacity limits")

add_custom_target(fork_parallelism_characterization
  COMMAND ${RUBY_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/fork_parallelism_characterize.rb
    --runner $<TARGET_FILE:pcaa_graph_run>
    --generator $<TARGET_FILE:pbqp_graph_generate>
    --scaling-csv ${PROJECT_BINARY_DIR}/scaling-runs.csv
    --corpus-dir ${PROJECT_SOURCE_DIR}/examples/regalloc
    --output ${PROJECT_BINARY_DIR}/fork-parallelism.csv
  DEPENDS pcaa_graph_run pbqp_graph_generate
  BYPRODUCTS ${PROJECT_BINARY_DIR}/fork-parallelism.csv
  COMMENT "Characterizing exact-search fork domains against Model-C epoch work")

pcaa_add_format_sources(
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp_model_kernel.h
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp_model_kernel.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/pbqp_model_kernel_unit.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp_run.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp_run_launcher.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp_graph_generate.cpp)

add_executable(pcaa_graph_run_l2_model pbqp_run.cpp)
target_compile_definitions(pcaa_graph_run_l2_model PRIVATE PCAA_GRAPH_RUN_L2
                          PCAA_VERSION="${PROJECT_VERSION}")
target_link_libraries(pcaa_graph_run_l2_model PRIVATE pcaa_runner_kernel)
add_executable(pcaa_graph_run_l2 pbqp_run_launcher.cpp)
target_compile_definitions(pcaa_graph_run_l2 PRIVATE PCAA_GRAPH_RUN_MODEL_NAME="pcaa_graph_run_l2_model")
add_dependencies(pcaa_graph_run_l2 pcaa_graph_run_l2_model)
add_executable(pcaa_l2_microbench l2_microbench.cpp)
target_include_directories(pcaa_l2_microbench PRIVATE ${PROJECT_SOURCE_DIR}/accelerator/tests)
target_link_libraries(pcaa_l2_microbench PRIVATE pcaa_l2 pcaa_core)
add_test(NAME l2_microbench_smoke COMMAND pcaa_l2_microbench --smoke)
pcaa_add_format_sources(${CMAKE_CURRENT_SOURCE_DIR}/l2_microbench.cpp)

add_test(NAME l2_characterization_smoke
  COMMAND ${RUBY_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/l2_characterize.rb
          --smoke --build ${PROJECT_BINARY_DIR} --output ${PROJECT_BINARY_DIR}/l2-smoke)
add_test(NAME pcaa_graph_run_l2_triangle
  COMMAND pcaa_graph_run_l2 --solver local ${PROJECT_SOURCE_DIR}/examples/triangle.pbqp)
set_tests_properties(pcaa_graph_run_l2_triangle PROPERTIES PASS_REGULAR_EXPRESSION "optimum -3")
add_test(NAME pcaa_graph_run_l2_invalid_config
  COMMAND pcaa_graph_run_l2 --solver local --l2-lanes 0 ${PROJECT_SOURCE_DIR}/examples/triangle.pbqp)
set_tests_properties(pcaa_graph_run_l2_invalid_config PROPERTIES WILL_FAIL TRUE)
