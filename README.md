# Programmable Cost Algebra Accelerator

PCAA is a compact, programmable accelerator model for the regular cost-vector
work inside graph optimizers such as PBQP. It is both a runnable SystemC model
and a platform for exploring how batching, scheduling, and lane width affect
that workload.

The complete block contract and command semantics are in
[the architecture specification](doc/arch.md). The L1 modeled-cycle study is
in [the performance-model report](doc/l1-performance-model.md).

## Build and run a graph

The quickest path needs CMake, a C++17 compiler, SystemC 3.x, GoogleTest, and
Ruby (only for the optional characterization target):

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
result-write components using its fixed four-lane streaming configuration. A
zero device-cycle total can occur when the selected algorithm completes without
using accelerator operations.

Choose the solver mode explicitly:

- `--solver bare-metal` accepts graphs with at most 64 vertices, 6 choices per
  vertex, and 2,016 edges. It matches the solver available to RV64 programs.
- `--solver local` uses allocator-backed graph, reconstruction, and search
  storage. Its graph size is limited by host memory; the parser accepts up to
  65,536 choices per vertex.

Select an algorithm independently with `--strategy`. Each strategy is
available in either mode when its storage fits the selected environment. The
default is `heuristic-rn`.

- `heuristic-rn` is the default. It first applies exact low-degree reductions,
  then deterministically fixes a node when a general core remains, and repeats.
  It always returns a valid assignment for a graph that fits the shared solver,
  but that assignment is a heuristic result rather than a proof of optimum.
  `--rn-policy min-degree`, `max-degree`, or `min-work` chooses which eligible
  core node is fixed; equal candidates are resolved deterministically.
- `reduce-only` applies only exact low-degree reductions. Its answer is exact
  if it finishes; otherwise it reports `IRREDUCIBLE`, meaning that the input
  has a remaining general core and no assignment is returned. It is useful for
  recognizing graphs that need a general-graph algorithm.
- `exact-core-enumeration` applies exact reductions once, then enumerates all
  assignments of the residual core. It is an exact small-instance oracle.
- `exact-branch-reduce` branches on a remaining node and re-applies exact
  reductions below every branch, pruning a branch once its own reductions
  already cost at least as much as the best complete answer found so far
  (a true branch-and-bound, not exhaustive branch-and-reduce). A completed
  answer is exact; use `--maximum-search-nodes N` to put an explicit bound
  on the search. It is suitable for small cores.
  `N` is a non-negative whole number; zero leaves the search-node limit unset.
  `--verbose` reports how many branches were pruned this way.
- `local-search` starts from all zeroes and deterministic single-coordinate
  restarts, then repeatedly takes strict coordinate improvements. It reports a
  `local-optimum`, not a proof of global optimality.
- `heuristic-rn-local-search` first runs RN and then applies the same local
  improvement from that assignment. It never returns an objective worse than
  its RN starting point, but remains heuristic.

Run `pcaa_graph_run --help` for the complete command synopsis, including
`--version`.

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

[`examples/regalloc`](examples/regalloc) holds 491 real PBQP graphs
extracted from LLVM's `RegAllocPBQP` allocator (four real translation
units, `-O2`/x86-64) via a small, always-off-by-default `llc` flag added to
a local LLVM checkout — not part of this repo's own build. See
[doc/llvm-corpus-characterization.md](doc/llvm-corpus-characterization.md)
for how they were produced and how they compare to the synthetic families
above.

For example:

```sh
build/pcaa_graph_run --solver bare-metal examples/tie.pbqp
build/pcaa_graph_run --solver local examples/random-20.pbqp
```

Add `--verbose` to follow execution. The trace is written to stderr and shows
submitted work, executed operations, and intermediate results:

```sh
build/pcaa_graph_run --verbose --solver bare-metal examples/triangle.pbqp
```

For machine-readable solver events, use `--trace FILE`; each JSONL row records
one solver action and its logical cost-algebra work.

## Build and verify the complete integration

For Spike-backed bare-metal tests, install a RISC-V bare-metal toolchain and
give CMake the source tree matching the Spike executable:

```sh
cmake -S . -B build -DSPIKE_SOURCE_DIR=../riscv-isa-sim
cmake --build build
ctest --test-dir build --output-on-failure
cmake --build build --target basic_elf batch_elf randomized_elf pbqp_basic_elf pbqp_randomized_elf pbqp_rn_elf
```

Run the generated RISC-V checks with:

```sh
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/basic.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/batch.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/randomized.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/pbqp_basic.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/pbqp_randomized.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/pbqp_rn.elf
```

To run the reproducible synthetic solver-characterization corpus through the CLI:

```sh
cmake --build build --target solver_characterization
```

The target writes `build/solver-characterization.csv` and a Markdown summary
alongside it. To choose seeds, exact limits, or stream CSV elsewhere, invoke
`ruby scripts/rn_characterize.rb --help` directly. The study and its scope are in
[the solver-characterization report](doc/solver-characterization.md).

For the related HW/SW boundary corpus:

```sh
cmake --build build --target hw_sw_characterization
```

It writes `build/hw-sw-epochs.csv`; see
[the HW/SW boundary report](doc/hw-sw-boundary-characterization.md).

`cmake --build build --target scaling_characterization` runs the graph/domain
scaling grid through allocator-backed local mode; it writes
`build/scaling-runs.csv`. The sweep is family-specific (mixed-degree stays
below N=1000 because its `O(N^2)` edge count makes large instances slow to
solve, while degree-3/degree-4 reach N=1000 quickly) and supports resuming
an interrupted run; invoke `ruby scripts/scaling_characterize.rb --help`
directly to pick sweeps, node/domain lists per family, seeds, policies, or
a per-run timeout. `--corpus-dir DIR` feeds a directory of pre-generated
`.pbqp` files (e.g. `examples/regalloc`) to the runner as one more family
instead of generating one. `--sweeps local-search` (opt-in; much slower per
point) additionally sweeps `local-search`/`heuristic-rn-local-search` and
reports LS-A/LS-B epoch-size percentiles from a `--trace` capture. See
[the scaling report](doc/scaling-characterization.md) and
[the LLVM-corpus report](doc/llvm-corpus-characterization.md).

To create a deterministic synthetic input yourself, use the host-only graph
generator and pass its output back to the runner:

```sh
build/pbqp_graph_generate --family degree-3 --profile binary --nodes 20 --seed 42 > /tmp/degree-3.pbqp
build/pcaa_graph_run --solver local /tmp/degree-3.pbqp
```

## Build artifacts

The build directory contains these generated artifacts:

- `pcaa_graph_run` — host utility that runs a user-supplied PBQP file through
  the untimed SystemC accelerator model.
- `pcaa_graph_run_model` — companion executable used by `pcaa_graph_run` to
  provide clean command-line output.
- `pcaa_graph_run_timed` and `pcaa_graph_run_timed_model` — launcher and
  companion executable for the L1 timing-reporting graph runner.
- `pbqp_graph_generate` — host-only deterministic generator that writes a
  synthetic PBQP graph in the runner's text format to standard output.
- `systemc_unit`, `pbqp_unit` — host unit-test executables.
- `libpcaa_core.a`, `libpcaa_timing.a`, `libpcaa_cost_math.a`,
  `libpcaa_pbqp.a` — reusable static libraries for the model, timing estimator,
  cost arithmetic, and PBQP solver.
- `libpcaa_spike_device.so` — Spike external-device plugin, produced only when
  `SPIKE_SOURCE_DIR` is configured.
- `basic.elf`, `batch.elf`, `randomized.elf`, `pbqp_basic.elf`, and
  `pbqp_randomized.elf`, and `pbqp_rn.elf` — freestanding RV64 verification programs, produced by
  their corresponding `*_elf` build targets when the RISC-V toolchain is
  available.
- `solver-characterization.csv` — reproducible strategy-comparison data,
  produced by the `solver_characterization` target.
- `solver-characterization-summary.md` — generated aggregate tables for the
  characterization corpus.
- `hw-sw-epochs.csv` — raw hypothetical HW/SW execution epochs from solver
  event traces.

## Repository layout

- `accelerator` — protocol headers, SystemC model, timing estimator, and Spike
  device adapter.
- `software` — RV64 bare-metal driver, PBQP solver, and freestanding tests.
- `tools` — host command-line runners and the PBQP graph generator.
- `examples` — ready-to-run PBQP text graphs.
- `scripts` — reproducible external characterization scripts.
- `workload` — host-only graph-generation library used by the generator tool.
- `doc` — architecture specification, design notes, performance studies, and
  solver-characterization reports.
- `cmake` and `.github` — test helpers and continuous-integration workflow.
