# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Defines host graph-runner, generator, and reproducible characterization targets.

add_executable(pbqp_graph_generate pbqp_graph_generate.cpp)
target_link_libraries(pbqp_graph_generate PRIVATE pcaa_graph_generator)

add_executable(pcaa_graph_run_model pbqp_run.cpp)
target_compile_definitions(pcaa_graph_run_model PRIVATE PCAA_VERSION="${PROJECT_VERSION}")
target_link_libraries(pcaa_graph_run_model PRIVATE pcaa_core pcaa_pbqp)

add_executable(pcaa_graph_run pbqp_run_launcher.cpp)
add_dependencies(pcaa_graph_run pcaa_graph_run_model)

add_executable(pcaa_graph_run_timed_model pbqp_run.cpp)
target_compile_definitions(pcaa_graph_run_timed_model PRIVATE
  PCAA_GRAPH_RUN_TIMED
  PCAA_VERSION="${PROJECT_VERSION}")
target_link_libraries(pcaa_graph_run_timed_model PRIVATE pcaa_core pcaa_pbqp)

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
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp_run.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp_run_launcher.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp_graph_generate.cpp)
