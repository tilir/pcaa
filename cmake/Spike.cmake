# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Defines the optional Spike external-device plugin.

set(SPIKE_SOURCE_DIR "" CACHE PATH
  "Path to the riscv-isa-sim source tree; enables the Spike plugin target")

if(SPIKE_SOURCE_DIR)
  add_library(pcaa_spike_device SHARED
    accelerator/src/spike_device.cpp
    accelerator/src/systemc_plugin_entry.cpp)
  target_include_directories(pcaa_spike_device PRIVATE
    ${SPIKE_SOURCE_DIR}
    ${SPIKE_SOURCE_DIR}/riscv
    ${SPIKE_SOURCE_DIR}/fesvr
    ${CMAKE_CURRENT_SOURCE_DIR}/accelerator/include)
  target_link_libraries(pcaa_spike_device PRIVATE pcaa_core)
  set_target_properties(pcaa_spike_device PROPERTIES OUTPUT_NAME pcaa_spike_device)
endif()
