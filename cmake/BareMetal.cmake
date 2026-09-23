# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Defines optional freestanding RV64 verification ELF targets.

get_property(PCAA_BAREMETAL_SOURCES GLOBAL PROPERTY PCAA_BAREMETAL_SOURCES)
get_property(PCAA_BAREMETAL_HEADERS GLOBAL PROPERTY PCAA_BAREMETAL_HEADERS)
get_property(PCAA_BAREMETAL_C_SOURCES GLOBAL PROPERTY PCAA_BAREMETAL_C_SOURCES)
get_property(PCAA_BAREMETAL_INCLUDE_DIRS GLOBAL PROPERTY PCAA_BAREMETAL_INCLUDE_DIRS)
set(PCAA_BAREMETAL_INCLUDE_FLAGS)
foreach(include_dir IN LISTS PCAA_BAREMETAL_INCLUDE_DIRS)
  list(APPEND PCAA_BAREMETAL_INCLUDE_FLAGS -I${include_dir})
endforeach()

function(add_baremetal_test test_name)
  set(output_elf ${CMAKE_CURRENT_BINARY_DIR}/${test_name}.elf)
  set(test_source ${CMAKE_CURRENT_SOURCE_DIR}/software/tests/${test_name}.c)
  add_custom_command(
    OUTPUT ${output_elf}
    COMMAND ${RISCV_GCC} -march=rv64imac -mabi=lp64 -mcmodel=medany -std=c11 -ffreestanding
      -fno-builtin -nostdlib -nostartfiles -O2
      -I${CMAKE_CURRENT_SOURCE_DIR}/accelerator/include -I${CMAKE_CURRENT_SOURCE_DIR}/software
      ${PCAA_BAREMETAL_INCLUDE_FLAGS}
      ${CMAKE_CURRENT_SOURCE_DIR}/software/tests/start.S
      ${CMAKE_CURRENT_SOURCE_DIR}/software/tests/runtime.c
      ${CMAKE_CURRENT_SOURCE_DIR}/software/accel_driver.c
      ${PCAA_BAREMETAL_C_SOURCES}
      ${test_source}
      -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/software/tests/linker.ld
      -Wl,--build-id=none -o ${output_elf}
    DEPENDS ${PCAA_BAREMETAL_SOURCES} ${PCAA_BAREMETAL_HEADERS} ${test_source}
    VERBATIM)
  add_custom_target(${test_name}_elf DEPENDS ${output_elf})
endfunction()

function(add_pbqp_baremetal_test test_name)
  set(output_elf ${CMAKE_CURRENT_BINARY_DIR}/${test_name}.elf)
  set(object_dir ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${test_name}_elf.dir)
  set(common_flags -march=rv64imac -mabi=lp64 -mcmodel=medany -ffreestanding -fno-builtin
    -nostdlib -nostartfiles -O2 -I${CMAKE_CURRENT_SOURCE_DIR}/accelerator/include
    -I${CMAKE_CURRENT_SOURCE_DIR}/software ${PCAA_BAREMETAL_INCLUDE_FLAGS})
  set(c_objects
    ${object_dir}/start.o ${object_dir}/runtime.o ${object_dir}/accel_driver.o
    ${object_dir}/pbqp_accelerator.o ${object_dir}/cost_math.o
    ${object_dir}/${test_name}.o)
  set(pbqp_object ${object_dir}/pbqp.o)
  set(pbqp_storage_object ${object_dir}/pbqp_storage.o)
  file(MAKE_DIRECTORY ${object_dir})

  macro(add_pbqp_object object compiler standard source)
    add_custom_command(
      OUTPUT ${object}
      COMMAND ${compiler} ${common_flags} ${standard} ${ARGN} -c ${source} -o ${object}
      DEPENDS ${source} ${PCAA_BAREMETAL_HEADERS}
      VERBATIM)
  endmacro()

  add_pbqp_object(${object_dir}/start.o ${RISCV_GCC} -std=c11
    ${CMAKE_CURRENT_SOURCE_DIR}/software/tests/start.S)
  add_pbqp_object(${object_dir}/runtime.o ${RISCV_GCC} -std=c11
    ${CMAKE_CURRENT_SOURCE_DIR}/software/tests/runtime.c)
  add_pbqp_object(${object_dir}/accel_driver.o ${RISCV_GCC} -std=c11
    ${CMAKE_CURRENT_SOURCE_DIR}/software/accel_driver.c)
  add_pbqp_object(${object_dir}/pbqp_accelerator.o ${RISCV_GCC} -std=c11
    ${CMAKE_CURRENT_SOURCE_DIR}/software/pbqp/pbqp_accelerator.c)
  add_pbqp_object(${object_dir}/${test_name}.o ${RISCV_GCC} -std=c11
    ${CMAKE_CURRENT_SOURCE_DIR}/software/tests/${test_name}.c)
  add_pbqp_object(${pbqp_object} ${RISCV_GXX} -std=c++17
    ${CMAKE_CURRENT_SOURCE_DIR}/software/pbqp/pbqp.cpp
    -fno-exceptions -fno-rtti -fno-threadsafe-statics)
  add_pbqp_object(${pbqp_storage_object} ${RISCV_GXX} -std=c++17
    ${CMAKE_CURRENT_SOURCE_DIR}/software/pbqp/pbqp_storage.cpp
    -fno-exceptions -fno-rtti -fno-threadsafe-statics)
  add_pbqp_object(${object_dir}/cost_math.o ${RISCV_GXX} -std=c++17
    ${CMAKE_CURRENT_SOURCE_DIR}/accelerator/src/cost_math.cpp
    -fno-exceptions -fno-rtti -fno-threadsafe-statics)
  foreach(codec_source IN LISTS PCAA_BAREMETAL_C_SOURCES)
    get_filename_component(codec_name ${codec_source} NAME_WE)
    set(codec_object ${object_dir}/${codec_name}.o)
    add_pbqp_object(${codec_object} ${RISCV_GCC} -std=c11 ${codec_source})
    list(APPEND c_objects ${codec_object})
  endforeach()
  add_custom_target(${test_name}_objects DEPENDS ${c_objects} ${pbqp_object} ${pbqp_storage_object})

  add_custom_command(
    OUTPUT ${output_elf}
    COMMAND ${RISCV_GXX} -nostdlib -nostartfiles ${c_objects} ${pbqp_object} ${pbqp_storage_object}
      -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/software/tests/linker.ld
      -Wl,--build-id=none -o ${output_elf}
    DEPENDS ${c_objects} ${pbqp_object} ${pbqp_storage_object}
    VERBATIM)
  add_custom_target(${test_name}_elf DEPENDS ${output_elf})
  add_dependencies(${test_name}_elf ${test_name}_objects)
endfunction()

if(RISCV_GCC AND RISCV_GXX)
  add_baremetal_test(basic)
  add_baremetal_test(batch)
  add_baremetal_test(randomized)
  add_pbqp_baremetal_test(pbqp_basic)
  add_pbqp_baremetal_test(pbqp_randomized)
  add_pbqp_baremetal_test(pbqp_rn)
else()
  message(STATUS "Bare-metal test targets disabled: ${RISCV_PREFIX}gcc and g++ are required")
endif()
