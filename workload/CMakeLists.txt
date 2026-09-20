# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Defines the host-only PBQP graph generator library.

add_library(pcaa_graph_generator STATIC src/graph_generator.cpp)
target_include_directories(pcaa_graph_generator PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

pcaa_add_format_sources(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/pbqp_graph_generator/graph_generator.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/graph_generator.cpp)
