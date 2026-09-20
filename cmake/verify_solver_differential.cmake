# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Confirms that bare-metal and local solver modes agree on the reference corpus.

set(graphs triangle path tie unreachable petersen chvatal random-20)

foreach(graph IN LISTS graphs)
  execute_process(
    COMMAND "${RUNNER}" --solver bare-metal "${INPUT_DIRECTORY}/${graph}.pbqp"
    RESULT_VARIABLE bare_status
    OUTPUT_VARIABLE bare_output
    ERROR_VARIABLE bare_errors
  )
  if(NOT bare_status EQUAL 0)
    message(FATAL_ERROR "bare-metal solver failed for ${graph}: ${bare_errors}")
  endif()
  string(REGEX MATCH "optimum [-]?[0-9]+" bare_match "${bare_output}")
  string(REPLACE "optimum " "" bare_optimum "${bare_match}")

  execute_process(
    COMMAND "${RUNNER}" --solver local "${INPUT_DIRECTORY}/${graph}.pbqp"
    RESULT_VARIABLE local_status
    OUTPUT_VARIABLE local_output
    ERROR_VARIABLE local_errors
  )
  if(NOT local_status EQUAL 0)
    message(FATAL_ERROR "local solver failed for ${graph}: ${local_errors}")
  endif()
  string(REGEX MATCH "optimum [-]?[0-9]+" local_match "${local_output}")
  string(REPLACE "optimum " "" local_optimum "${local_match}")

  if(NOT bare_optimum STREQUAL local_optimum)
    message(FATAL_ERROR
      "solver mismatch for ${graph}: bare-metal=${bare_optimum}, local=${local_optimum}")
  endif()
endforeach()
