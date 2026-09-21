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

Keep CMake ownership local: a subsystem's `CMakeLists.txt` defines its targets and appends
its format and bare-metal inputs through the project collection helpers. Top-level and
shared CMake modules consume those collections; do not restore centralized long source lists.

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
* `workload`: host-only C++ graph generation and logical workload characterization.
* `tools/pbqp_run.cpp`: host-side PBQP text-format runner through the SystemC model.
* `examples`: user-facing PBQP text inputs for the host runner.

Host tests use GoogleTest. Keep SystemC tests behind the required `sc_main` entry point, which
initializes and runs GoogleTest; bare-metal ELFs remain freestanding and do not use GoogleTest.

Keep `doc/arch.md` confined to architectural block facts. Put workload methodology, trace schemas,
and unresolved interface-analysis material in `doc/design.md` and factual corpus output in
`doc/workload-characterization.md` or a scoped solver report in `doc/`. The workload generator is host-only and may use standard C++
containers; never enlarge bare-metal PBQP limits merely to characterize workloads.

`pcaa_graph_run` is the supported hands-on host entry point. Preserve its
line-oriented `nodes`/`node`/`edge` format and its `INF` literal unless a
versioned user-facing format migration is explicitly requested. It must submit
through the SystemC target socket rather than bypassing the accelerator. It
requires an explicit `--solver bare-metal|local` choice: bare-metal rejects
graphs outside the fixed C API, while local exercises the same shared solver
through the hosted model path. Strategy is orthogonal to mode: every shared
strategy may be selected in either mode when the graph fits the fixed C API.
Only `EXACT_CORE_ENUMERATION` and `EXACT_BRANCH_REDUCE` may label a completed
result exact; `LOCAL_SEARCH` must label its result a local optimum.
The runner default is `HEURISTIC_RN`.
The local mode's 65,536-choice practical domain limit is determined by the
runner's reusable guest-memory staging area, not the PCAA protocol.

`pcaa_graph_run` is intentionally untimed. Keep L1 reporting in the separate
`pcaa_graph_run_timed` executable, whose fixed four-lane streaming configuration
must report total, descriptor, operand-read, compute, and result-write cycles.
An irreducible PBQP core submits no PCAA primitives; the timed runner must say
explicitly that its zero device cycles reflect software-core solving.

## Semantics to retain

`ACCEL_INF` is `INT32_MAX / 4`. Any addition with `INF` produces `INF`; positive values reaching it saturate to it.  Argmin commands select the first equal minimum. `n == 0`, unsupported opcodes, missing required source addresses, and failed memory accesses produce `STATUS_ERROR`.

PBQP graph reduction remains software-owned. `software/pbqp/pbqp.h` is a C ABI; its implementation is C++17 and must remain freestanding-friendly (no heap, exceptions, RTTI, or C++ runtime requirement). Configure a `pbqp_solver_t` through `pbqp_solver_create`, then use `pbqp_solver_solve`; both software and accelerator modes must share that solver. Keep vector views explicit and account for accelerator scratch packing in `pbqp_statistics_t`.

PBQP strategies are explicit: `REDUCE_ONLY`, `HEURISTIC_RN`,
`EXACT_CORE_ENUMERATION`, `EXACT_BRANCH_REDUCE`, `LOCAL_SEARCH`, and the
`HEURISTIC_RN_LOCAL_SEARCH` hybrid share the same freestanding solver core and
CostKernel. Exact core enumeration reduces once then enumerates a residual
core; exact branch-and-reduce must condition one branch and re-run R0/R1/R2 at
every search node. It uses bounded static snapshots, never graph-sized stack
copies, and reports `PBQP_SEARCH_LIMIT` for an explicit node limit or snapshot
depth limit. The hybrid must never worsen its RN seed.
RN scoring projects each incident matrix against its neighbor unary through the
kernel using the value-only minimum primitive; argmin is reserved for phases
that need reconstruction or a chosen coordinate. Its software-only score
accumulation must remain distinct from shared conditioning/commit, which
applies the selected matrix slice exactly once. Account generic conditioning
traffic separately from RN-only commits: each updated element reads matrix and
unary then writes unary (12 logical bytes).
Keep the solver's generic operation mix current: `MINPLUS_PROJECT`,
`PROJECT_ACCUMULATE`, `SLICE_ACCUMULATE`, `MAP3_REDUCE`, and `ARGMIN_VECTOR`
must retain separately reportable elements, current primitive descriptors, and
logical bytes. The external characterization report's operation-mix table is
the primary comparison artifact; do not replace it with only phase-specific
counters.
Never add a PBQP-specific accelerator opcode for RN.

The fixed bare-metal PBQP C API supports 64 nodes and all simple edges between
them. Its graph state and exact-search snapshots are statically allocated by
the current ELFs; the solver's largest transient R2 frame is below 4 KiB and
the startup reserve is 1 MiB.
Do not raise this capacity without recalculating static-storage and stack use,
then re-running all PBQP ELFs under Spike.

`EXECUTE_BATCH` is an ordered, finite control operation, not a scheduler: the
runtime constructs child primitive descriptors and the accelerator drains them
in order. Do not add dependency discovery, reordering, graph awareness, or
batching across PBQP reductions. Within one software-built batch, cache packed
strided views by `(base, length, stride)` and keep their storage valid through
completion. Keep statistics for primitive descriptors separate from top-level
MMIO submissions.

When changing freestanding PBQP working storage, calculate the complete call
chain's stack use. The bare-metal startup reserve is 1 MiB and PBQP ELFs must
be built and run under Spike before handoff; host-only CTest does not cover
their stack or driver path.

PBQP admits `ACCEL_INF` and finite costs only in the documented safe range
`[PBQP_MIN_FINITE_COST, PBQP_MAX_FINITE_COST]`; reject other costs at graph construction so
saturating arithmetic cannot make mathematically equivalent reductions disagree. Bare-metal PBQP
differential tests must use `software/tests/pbqp_reference.h`, not production cost math, as their
exhaustive oracle. Error-valued accelerator submissions must propagate through cost-kernel callbacks;
never treat `ACCEL_INF` as an error sentinel.

## Spike integration

The current supported Spike tree has `--extlib` plus `--device` and `REGISTER_DEVICE` in `riscv/abstract_device.h`. The plugin receives `sim_t` from the factory and makes physical transactions using `sim_t::mmio_load` and `mmio_store`. Do not use `addr_to_mem`: it exposes a host pointer and violates the abstraction. Keep plugin options as `pcaa,<base>,<size>` and keep `ACCEL_MMIO_BASE` aligned with the documented default.

At L0 no meaningful SystemC time is advanced. Do not introduce global or hidden event loops. When timing is added, annotated delay and `sc_start()` advancement should be localized to Spike glue; the descriptor ABI and software driver must stay unchanged.

At L1, timing is an explicit configurable architectural estimate, not a
cycle-accurate implementation. Keep descriptor, operand-read, compute, and
result-write cycles separate; preserve an untimed mode for correctness tests.
Lanes affect only the timing calculation. Keep PBQP reduction policies and
batch construction in software, and report software packing independently of
device service cycles.

`accelerator/src/systemc_plugin_entry.cpp` supplies libsystemc's mandatory `sc_main` symbol for the Spike shared library. Spike owns the process and never invokes it. Host SystemC tests define their own `sc_main` and must explicitly call `sc_start(SC_ZERO_TIME)` to exercise kernel startup; do not link the plugin entry source into those tests.

## Required verification

Run from the repository root:

```sh
cmake -S . -B build -DSPIKE_SOURCE_DIR=../riscv-isa-sim
cmake --build build
ctest --test-dir build --output-on-failure
cmake -S . -B build-release -DSPIKE_SOURCE_DIR=../riscv-isa-sim -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
cmake --build build --target basic_elf randomized_elf pbqp_basic_elf pbqp_randomized_elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/basic.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/randomized.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/pbqp_basic.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/pbqp_randomized.elf
cmake --build build --target pbqp_rn_elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/pbqp_rn.elf
```

If changing an opcode, cover normal data, `n=1`, non-power-of-two lengths, negative values, `INF`, ties/argmin where applicable, and memory failure/error handling in the SystemC test. Keep bare-metal random lengths including 1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 63, and 64.
