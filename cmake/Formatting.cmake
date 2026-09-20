# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Defines source formatting and include-hygiene verification.

get_property(PCAA_FORMAT_SOURCES GLOBAL PROPERTY PCAA_FORMAT_SOURCES)

if(CLANG_FORMAT AND INCLUDE_WHAT_YOU_USE AND IWYU_TOOL)
  add_custom_target(format
    COMMAND ${CLANG_FORMAT} -i ${PCAA_FORMAT_SOURCES}
    COMMAND ${IWYU_TOOL} -p ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS pcaa_cost_math pcaa_timing pcaa_core systemc_unit
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Formatting PCAA sources and checking include hygiene"
    VERBATIM)
else()
  add_custom_target(format
    COMMAND ${CMAKE_COMMAND} -E echo
      "clang-format, include-what-you-use, and iwyu_tool are required for format"
    COMMAND ${CMAKE_COMMAND} -E false
    COMMENT "Formatting PCAA sources and checking include hygiene"
    VERBATIM)
endif()
