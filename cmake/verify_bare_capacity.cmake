# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Verifies the graph-runner diagnostic for an input beyond bare-metal capacity.

execute_process(
  COMMAND "${RUNNER}" --solver bare-metal "${INPUT}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors
)

if(result EQUAL 0)
  message(FATAL_ERROR "bare-metal mode accepted an oversized graph")
endif()

string(FIND "${output}${errors}" "graph does not fit the shared PBQP solver" diagnostic)
if(diagnostic EQUAL -1)
  message(FATAL_ERROR "bare-metal mode did not report its capacity diagnostic")
endif()
