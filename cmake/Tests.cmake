# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Registers host unit tests and graph-runner end-to-end tests.

function(add_graph_run_e2e_test name input expected_output)
  add_test(NAME pcaa_graph_run_${name} COMMAND pcaa_graph_run --solver bare-metal
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/${input})
  set_tests_properties(pcaa_graph_run_${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expected_output}")
endfunction()

add_graph_run_e2e_test(triangle triangle.pbqp "assignment 1 0 1")
add_graph_run_e2e_test(path path.pbqp "assignment 0 0 1")
add_graph_run_e2e_test(tie tie.pbqp "assignment 0 0")
add_graph_run_e2e_test(unreachable unreachable.pbqp "assignment 1 0")
add_graph_run_e2e_test(petersen petersen.pbqp "assignment 0 0 0 0 0 0 0 0 0 0")
add_graph_run_e2e_test(chvatal chvatal.pbqp "assignment 0 0 0 0 0 0 0 0 0 0 0 0")
add_test(NAME pcaa_graph_run_random_20 COMMAND pcaa_graph_run --solver bare-metal
  --strategy exact-branch-reduce ${CMAKE_CURRENT_SOURCE_DIR}/examples/random-20.pbqp)
set_tests_properties(pcaa_graph_run_random_20 PROPERTIES PASS_REGULAR_EXPRESSION "solution exact")
add_test(NAME pcaa_graph_run_rn COMMAND pcaa_graph_run --solver bare-metal
  --strategy heuristic-rn --rn-policy min-degree ${CMAKE_CURRENT_SOURCE_DIR}/examples/chvatal.pbqp)
set_tests_properties(pcaa_graph_run_rn PROPERTIES PASS_REGULAR_EXPRESSION "strategy HEURISTIC_RN")
add_test(NAME pcaa_graph_run_local_rn COMMAND pcaa_graph_run --solver local
  --strategy heuristic-rn --rn-policy min-degree ${CMAKE_CURRENT_SOURCE_DIR}/examples/chvatal.pbqp)
set_tests_properties(pcaa_graph_run_local_rn PROPERTIES PASS_REGULAR_EXPRESSION "strategy HEURISTIC_RN")
add_test(NAME pcaa_graph_run_reduce_only COMMAND pcaa_graph_run --solver bare-metal
  --strategy reduce-only ${CMAKE_CURRENT_SOURCE_DIR}/examples/chvatal.pbqp)
set_tests_properties(pcaa_graph_run_reduce_only PROPERTIES PASS_REGULAR_EXPRESSION "status IRREDUCIBLE")
add_test(NAME pcaa_graph_run_local COMMAND pcaa_graph_run --solver local
  --strategy local-search ${CMAKE_CURRENT_SOURCE_DIR}/examples/random-20.pbqp)
set_tests_properties(pcaa_graph_run_local PROPERTIES PASS_REGULAR_EXPRESSION "solution local-optimum")
add_test(NAME pcaa_graph_run_bare_capacity
  COMMAND ${CMAKE_COMMAND}
    -DRUNNER=$<TARGET_FILE:pcaa_graph_run>
    -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/examples/wide-domain.pbqp
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_bare_capacity.cmake)
add_test(NAME pcaa_graph_run_differential
  COMMAND ${CMAKE_COMMAND}
    -DRUNNER=$<TARGET_FILE:pcaa_graph_run>
    -DINPUT_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/examples
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_solver_differential.cmake)
add_test(NAME pcaa_graph_run_timed COMMAND pcaa_graph_run_timed --solver bare-metal
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/triangle.pbqp)
set_tests_properties(pcaa_graph_run_timed PROPERTIES PASS_REGULAR_EXPRESSION "timing cycles=[1-9][0-9]*")
add_test(NAME pcaa_graph_run_timed_exact_branch COMMAND pcaa_graph_run_timed --solver bare-metal
  --strategy exact-branch-reduce ${CMAKE_CURRENT_SOURCE_DIR}/examples/random-20.pbqp)
set_tests_properties(pcaa_graph_run_timed_exact_branch
  PROPERTIES PASS_REGULAR_EXPRESSION "timing cycles=[1-9][0-9]*")
add_test(NAME pcaa_graph_run_verbose COMMAND pcaa_graph_run --verbose --solver bare-metal
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/triangle.pbqp)
set_tests_properties(pcaa_graph_run_verbose PROPERTIES PASS_REGULAR_EXPRESSION "pcaa: doorbell descriptor=")

set_source_files_properties(software/pbqp/pbqp_unit.c PROPERTIES LANGUAGE CXX)
add_executable(pbqp_unit software/pbqp/pbqp_unit.c)
target_link_libraries(pbqp_unit PRIVATE GTest::gtest_main pcaa_pbqp)
add_test(NAME pbqp_unit COMMAND pbqp_unit)

add_executable(systemc_unit accelerator/tests/systemc_unit.cpp)
target_link_libraries(systemc_unit PRIVATE GTest::gtest pcaa_core)
add_test(NAME systemc_unit COMMAND systemc_unit)
