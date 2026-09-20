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

add_custom_target(rn_characterization
  COMMAND ${RUBY_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/rn_characterize.rb
    --runner $<TARGET_FILE:pcaa_graph_run>
    --timed-runner $<TARGET_FILE:pcaa_graph_run_timed>
    --generator $<TARGET_FILE:pbqp_graph_generate>
    --timed
    --output ${PROJECT_BINARY_DIR}/rn-characterization.csv
  DEPENDS pcaa_graph_run pcaa_graph_run_timed pbqp_graph_generate
  BYPRODUCTS ${PROJECT_BINARY_DIR}/rn-characterization.csv
  COMMENT "Characterizing deterministic synthetic RN workloads through the CLI")

pcaa_add_format_sources(
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp_run.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp_run_launcher.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/pbqp_graph_generate.cpp)
