# Agent Guide: PCAA L0

## Purpose and non-goals

This repository is a durable L0 functional model for a descriptor-based map/reduce accelerator, not a throwaway demo.  Preserve the refinement route: L0 functional TLM -> L1 loosely timed TLM -> L2 pipelined/approximately timed -> L3 mixed TLM and Verilated RTL.

Do not add RISC-V instructions, CSRs, interrupts, DMA timing, cache coherency, virtual memory, PBQP graph knowledge, fixed vector-width assumptions, RTL, Verilator, or XLS unless a task explicitly advances the project past L0.

## Boundary invariants

1. `accelerator/include/accel_protocol.h` is shared C/C++ software-visible ABI. Keep it free of SystemC types and preserve explicit-width fields and layout assertions.
2. The guest writes a descriptor to physical memory, writes descriptor address registers, rings `DOORBELL`, then polls `STATUS`.
3. The hardware model must receive guest physical addresses only. Never dereference or `reinterpret_cast` one to a host pointer in `Accelerator`.
4. `MemoryInterface` is the memory boundary. The core model may use only `read` and `write`; Spike-specific code belongs in `accelerator/src/spike_device.cpp`.
5. MMIO must pass through the SystemC target socket's `b_transport`; do not bypass it with direct register calls in production glue.
6. Driver callers must not rely on implementation synchrony. L0 completion occurs during doorbell processing, but `accel_submit` and `accel_wait` remain separate for future asynchronous execution.
7. Functional accelerator math must remain independent from the software test reference.

## Type policy

Use `int` by default for local counters, status codes, loop variables, and ordinary computation. Use `size_t` for object sizes and indexes into host containers. Introduce fixed-width integer types only where their exact representation is part of the accelerator ABI, MMIO register format, guest-memory data format, serialized descriptor, or where a wider intermediate is required to make overflow behavior explicit. Do not spread `int32_t`/`uint32_t` through implementation code merely because the ABI uses them.

## Code style

`.clang-format` is authoritative for C, C++, and SystemC source. Keep new code compatible with C++17, use 2-space indentation, keep lines within 100 columns where practical, and avoid dense multi-statement lines. In each C/C++ file, include project headers first, then standard-library headers, then external-library headers; every header must remain self-sufficient. Do not hand-format around the configuration: run `cmake --build build --target format` after editing C/C++ sources. The target runs both `clang-format` and Include-What-You-Use against the CMake compilation database; it requires `clang-format`, `include-what-you-use`, and `iwyu_tool`.

Replace a numeric literal with a named constant when it expresses a protocol value, ABI width, address, capacity, algorithm parameter, test configuration, or other durable concept. Leave literals that are self-evident at the point of use, such as zero initialization, array indices, or simple arithmetic, in place.

Prefer `#pragma once` for project headers. Retain `extern "C"` guards around every stable C API when the header may be included from C++.

Define short, obvious C++ methods (including constructors and simple forwarding accessors) in the class definition. Reserve `.cpp` files for non-trivial control flow, algorithms, and platform-specific implementation details; do not split out a one-line definition solely by convention.

For fixed-size storage owned by C++ code, prefer `std::array` over a raw C array. Preserve raw arrays where they are part of a C ABI or where the freestanding RV64 C++ toolchain deliberately has no standard-library headers.

Every source module and public header begins with an SPDX GPL-3.0-only identifier, copyright notice, and a brief statement of its purpose. Avoid empty infinite loops in C/C++: use an explicit architecture-appropriate wait or halt instruction and mark terminal helpers `noreturn` where applicable.

## Documentation and public interfaces

Keep `README.md` concise and human-facing. It should explain what the project does, the supported commands and their observable semantics, plus build and test commands; it must not expose internal implementation or simulation-lifecycle details. Put development constraints in this guide and block architecture in `doc/arch.md`.

With every change, explicitly review `README.md`, `AGENTS.md`, and `doc/arch.md`. Update each document when the change affects its audience: README for user-visible behavior and commands, AGENTS for durable development rules, and the architecture specification for block-visible behavior or contracts.

Document public ABI structs directly where they are declared: state their purpose, binary layout, ownership/address-space rules, and the meaning of fields that current commands use. Stable C headers must remain usable from both C and C++ through `extern "C"` guards. `accel_protocol.h` is platform-neutral; `software/accel_driver.h` is an RV64 bare-metal interface and must reject unsupported compiler targets at compile time. Reuse `pcaa_cost_math` for accelerator cost-domain arithmetic; do not make the bare-metal differential reference depend on that implementation.

## Structure

* `accelerator/include`: ABI, guest-memory abstraction, SystemC module interface.
* `accelerator/include/cost_math.h` and `accelerator/src/cost_math.cpp`: reusable saturating cost arithmetic.
* `accelerator/src/accelerator.cpp`: generic vector primitives only.
* `accelerator/src/spike_device.cpp`: the sole Spike plugin and physical-memory adapter.
* `accelerator/tests`: host/SystemC tests, no Spike.
* `software`: bare-metal driver; callers use its API, not MMIO offsets.
* `software/pbqp`: C ABI and C++17 implementation of fixed-capacity PBQP reductions.
* `software/tests`: deterministic and fixed-seed differential ELFs.

## Semantics to retain

`ACCEL_INF` is `INT32_MAX / 4`. Any addition with `INF` produces `INF`; positive values reaching it saturate to it.  Argmin commands select the first equal minimum. `n == 0`, unsupported opcodes, missing required source addresses, and failed memory accesses produce `STATUS_ERROR`.

PBQP graph reduction remains software-owned. `software/pbqp/pbqp.h` is a C ABI; its implementation is C++17 and must remain freestanding-friendly (no heap, exceptions, RTTI, or C++ runtime requirement). Configure a `pbqp_solver_t` through `pbqp_solver_create`, then use `pbqp_solver_solve`; both software and accelerator modes must share that solver. Keep vector views explicit and account for accelerator scratch packing in `pbqp_statistics_t`.

## Spike integration

The current supported Spike tree has `--extlib` plus `--device` and `REGISTER_DEVICE` in `riscv/abstract_device.h`. The plugin receives `sim_t` from the factory and makes physical transactions using `sim_t::mmio_load` and `mmio_store`. Do not use `addr_to_mem`: it exposes a host pointer and violates the abstraction. Keep plugin options as `pcaa,<base>,<size>` and keep `ACCEL_MMIO_BASE` aligned with the documented default.

At L0 no meaningful SystemC time is advanced. Do not introduce global or hidden event loops. When timing is added, annotated delay and `sc_start()` advancement should be localized to Spike glue; the descriptor ABI and software driver must stay unchanged.

`accelerator/src/systemc_plugin_entry.cpp` supplies libsystemc's mandatory `sc_main` symbol for the Spike shared library. Spike owns the process and never invokes it. Host SystemC tests define their own `sc_main` and must explicitly call `sc_start(SC_ZERO_TIME)` to exercise kernel startup; do not link the plugin entry source into those tests.

## Required verification

Run from the repository root:

```sh
cmake -S . -B build -DSPIKE_SOURCE_DIR=../riscv-isa-sim
cmake --build build
ctest --test-dir build --output-on-failure
cmake --build build --target basic_elf randomized_elf pbqp_basic_elf pbqp_randomized_elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/basic.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/randomized.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/pbqp_basic.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/pbqp_randomized.elf
```

If changing an opcode, cover normal data, `n=1`, non-power-of-two lengths, negative values, `INF`, ties/argmin where applicable, and memory failure/error handling in the SystemC test. Keep bare-metal random lengths including 1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 63, and 64.
