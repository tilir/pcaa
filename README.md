# Programmable Cost Algebra Accelerator

PCAA is a compact, programmable accelerator model for the regular cost-vector
work inside graph optimizers such as PBQP. Software keeps ownership of the
graph and its decisions; PCAA carries out the dense min-plus reductions. The
repository is useful both as a runnable SystemC model and as a starting point
for exploring how batching, scheduling and lane width change that workload.

The complete block contract and command semantics are in
[the architecture specification](doc/arch.md). The L1 modeled-cycle study is
in [the performance-model report](doc/l1-performance-model.md).

## Build and run a graph

The quickest path needs CMake, a C++17 compiler, SystemC 3.x, and GoogleTest:

```sh
cmake -S . -B build
cmake --build build --target pcaa_graph_run
build/pcaa_graph_run --solver bare-metal examples/triangle.pbqp
```

The command loads the graph, solves it through the SystemC PCAA model, and
prints an optimum and one corresponding assignment. No RISC-V toolchain or
Spike installation is needed for this host-side path.

`pcaa_graph_run` is deliberately untimed, for the quickest functional check.
For the L1 estimate, build and run its separate counterpart:

```sh
cmake --build build --target pcaa_graph_run_timed
build/pcaa_graph_run_timed --solver bare-metal examples/triangle.pbqp
```

It reports total modeled service cycles and descriptor, operand, compute, and
result-write components using its fixed four-lane streaming configuration.
Zero device cycles are valid when the graph is an irreducible PBQP core: that
core is solved in software and produces no PCAA primitive submissions.

Choose the solver mode explicitly:

- `--solver bare-metal` uses the exact fixed-capacity solver shared with the
  RV64 tests. It accepts at most 64 vertices, 6 choices per vertex, and 2,016
  edges; a larger graph is rejected with a diagnostic.
- `--solver local` supports arbitrary vertex counts and up to 65,536 choices
  per vertex (subject to host memory). It produces a deterministic local
  optimum through the same PCAA model and labels it `local-optimum`.

Input is a small line-oriented PBQP format. Blank lines and `#` comments are
allowed. Declare the node count, then each node and each edge. Node costs have
one value per choice; edge costs are row-major. `INF` denotes an unreachable
cost.

```text
nodes 3
node 2 2 -1
node 2 0 3
node 2 1 -2
edge 0 1 0 4 -3 2
edge 0 2 2 -1 5 0
edge 1 2 1 3 -2 4
```

A *choice* is one possible assignment for a vertex (its PBQP domain). The
limits above belong to the solver implementations, not the accelerator. Costs
may be finite PCAA costs or `INF`.

## Examples

[`examples`](examples) contains ready-to-run inputs:

- `triangle.pbqp` — a three-node, fully connected graph;
- `path.pbqp` — a small path with asymmetric choice costs;
- `tie.pbqp` — equal optima, showing deterministic tie breaking;
- `unreachable.pbqp` — uses the human-readable `INF` literal.
- `petersen.pbqp` and `chvatal.pbqp` — standard named graph topologies.
- `random-20.pbqp` — a deterministic 20-vertex pseudo-random input for the
  batched bare-metal path or local-mode comparison.
- `wide-domain.pbqp` — a seven-choice input for trying the local mode and the
  bare-metal capacity diagnostic.

For example:

```sh
build/pcaa_graph_run --solver bare-metal examples/tie.pbqp
build/pcaa_graph_run --solver local examples/random-20.pbqp
```

Add `--verbose` to follow the model as it handles the graph. The trace is
written to stderr and shows doorbells, batch walking, child descriptors, and
primitive min/argmin results:

```sh
build/pcaa_graph_run --verbose --solver bare-metal examples/triangle.pbqp
```

## Build and verify the complete integration

For Spike-backed bare-metal tests, install a RISC-V bare-metal toolchain and
give CMake the source tree matching the Spike executable:

```sh
cmake -S . -B build -DSPIKE_SOURCE_DIR=../riscv-isa-sim
cmake --build build
ctest --test-dir build --output-on-failure
cmake --build build --target basic_elf batch_elf randomized_elf pbqp_basic_elf pbqp_randomized_elf
```

Run the generated RISC-V checks with:

```sh
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/basic.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/batch.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/randomized.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/pbqp_basic.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/pbqp_randomized.elf
```

To regenerate workload traces and the L1 analysis inputs:

```sh
cmake --build build --target pbqp_workload
build/pbqp_workload --trace build/pbqp-workload.csv
```

## Build artifacts

The build directory contains these generated artifacts:

- `pcaa_graph_run` — host utility that runs a user-supplied PBQP file through
  the untimed SystemC accelerator model.
- `pcaa_graph_run_model` — implementation launched by `pcaa_graph_run`; it
  exists so the user-facing runner can suppress SystemC's startup banner.
- `pcaa_graph_run_timed` and `pcaa_graph_run_timed_model` — launcher and
  implementation of the L1 timing-reporting graph runner.
- `pbqp_workload` — host workload-trace and L1 replay generator.
- `systemc_unit`, `pbqp_unit`, `workload_unit` — host unit-test executables.
- `libpcaa_core.a`, `libpcaa_timing.a`, `libpcaa_cost_math.a`,
  `libpcaa_pbqp.a`, `libpcaa_workload.a` — reusable static libraries for the
  model, timing estimator, cost arithmetic, PBQP solver, and workload tool.
- `libpcaa_spike_device.so` — Spike external-device plugin, produced only when
  `SPIKE_SOURCE_DIR` is configured.
- `basic.elf`, `batch.elf`, `randomized.elf`, `pbqp_basic.elf`, and
  `pbqp_randomized.elf` — freestanding RV64 verification programs, produced by
  their corresponding `*_elf` build targets when the RISC-V toolchain is
  available.
