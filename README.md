# Programmable Cost Algebra Accelerator

PCAA is a descriptor-driven accelerator for signed-integer cost algebra. Its
ISA 1.0.0 performs min-plus reductions and projections, vector cost addition,
argmin, and ordered batches over runtime-sized, strided data in guest physical
memory. Software chooses and submits the work; graph topology and search stay
outside the accelerator.

Costs are signed 32-bit integers below `INF = INT32_MAX / 4`, or `INF` itself.
Larger input values report `ERROR`; `INF` absorbs addition, positive sums
reaching it saturate, and negative underflow reports `ERROR`. Argmin ties
select the first index. A batch executes in order and stops at the first
failing command; completed results remain visible.

The repository provides a SystemC functional model, an optional L1 service-cycle
estimate, a structural approximately timed L2 model of MAS 1.0.0, and an RV64
bare-metal integration through Spike. A PBQP runner
exercises the accelerator; an independent Bellman–Ford probe checks the same
cost-algebra vocabulary on shortest paths. Neither algorithm is built into
the ISA. The command contract is in [the architecture specification](doc/arch.md),
with its rationale in [the ISA decision record](doc/isa-v1-decision.md).
The [documentation index](doc/README.md) distinguishes current references
from historical measurements and deferred designs. Measurement reports and their
verified raw data are in [doc/reports](doc/reports/README.md).
The [research synthesis](doc/research-summary-2026-10-01.md) explains the main
findings and design decisions as of 1 October 2026.

## Run a graph through PCAA

For the host runner, install CMake, a C++17 compiler, SystemC 3.x, and
GoogleTest, and Ruby. Spike and the RISC-V toolchain are not needed for this path.

```sh
cmake -S . -B build
cmake --build build --target pcaa_graph_run pcaa_graph_run_timed pcaa_graph_run_l2
build/pcaa_graph_run --solver bare-metal examples/triangle.pbqp
build/pcaa_graph_run_timed --solver bare-metal examples/triangle.pbqp
build/pcaa_graph_run_l2 --solver bare-metal examples/triangle.pbqp
```

All three commands print a cost and an assignment. `pcaa_graph_run` is untimed;
`pcaa_graph_run_timed` additionally reports estimated total, descriptor,
operand-read, compute, and result-write cycles for a fixed four-lane
configuration. Zero device cycles mean the selected solve needed no PCAA
primitives. `pcaa_graph_run_l2` reports executed MAS cycles and activity in an
`l2` JSON line. Its `--l2-lanes`, `--l2-mem-bytes`, `--l2-tm`, `--l2-tn`, and
`--l2-memory-latency` options configure the model without rebuilding. L1 and L2
cycles are different model quantities; see [the L2 measurements](doc/reports/l2-microarchitecture-characterization.md).

To see a completed exact solve on a larger graph, run:

```sh
build/pcaa_graph_run --solver bare-metal --strategy exact-branch-reduce \
  --maximum-search-nodes 100000 examples/random-20.pbqp
```

This input reports `optimum -18` and `solution exact`. Branching and graph
reduction remain in software; PCAA performs the submitted cost operations.
The exact label is emitted only when the search finishes, not when it reaches
its node limit.

Choose a solver environment explicitly:

- `--solver bare-metal` matches the bounded RV64 configuration: up to 64
  nodes, six choices per node, and 2,016 edges. It rejects graphs outside
  those capacities or its safe finite-cost range.
- `--solver local` uses host allocation for larger graphs; the runner accepts
  up to 65,536 choices per node at input validation. Individual accelerator
  command dimensions fit through 65,535; larger commands report a range error.

The default `--strategy heuristic-rn` returns a deterministic heuristic
assignment, not a proof of optimality. `reduce-only` reports `IRREDUCIBLE` if
an unsolved core remains. `exact-core-enumeration` and
`exact-branch-reduce` return exact results when they finish. `local-search`
returns a local optimum; `heuristic-rn-local-search` improves the RN result
without making an exactness claim. Use `--rn-policy` to choose RN node
selection, `--maximum-search-nodes N` to bound exact branch search, and
`--rn-batching per-edge` to compare against the retained scalar path. Run
`build/pcaa_graph_run --help` for all options, including `--verbose` and
machine-readable `--trace FILE`.
Host cost-kernel failures report the specific PCAA status on stderr, including
descriptor range errors and guest staging exhaustion.

Input is line-oriented. Declare the node count, then node costs and row-major
edge costs; blank lines and `#` comments are allowed. `INF` marks an
unreachable cost.

```text
nodes 3
node 2 2 -1
node 2 0 3
node 2 1 -2
edge 0 1 0 4 -3 2
edge 0 2 2 -1 5 0
edge 1 2 1 3 -2 4
```

More inputs are in [`examples`](examples), including
[LLVM RegAllocPBQP graphs](examples/regalloc). These are software workloads;
their graph-size limits are not accelerator ISA limits.

## Compare native CPU execution

The [host tools guide](tools/README.md) covers graph generation, runners, and
native CPU benchmarks. Measure CPU performance with a Release build:

```sh
cmake -S . -B build-cpu-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpu-release --parallel 8
build-cpu-release/pcaa_cpu_current current examples/triangle.pbqp
build-cpu-release/pcaa_cpu_bench structured examples/triangle.pbqp
```

On an AVX2-capable x86 CPU, add `-DPCAA_CPU_AVX2=ON` when configuring the
dense CPU baseline. The benchmarks report native solve samples without
SystemC. The [CPU/L2 comparison](doc/reports/cpu-baseline-l2-comparison.md)
uses the retained LLVM corpus and separates CPU kernel time, graph control,
host preparation, and modeled accelerator service cycles.

The [exact PBQP breakdown](doc/reports/exact-pbqp-cpu-l2-breakdown.md) studies
bounded branch-and-reduce, state copies, lower bounds and incumbent quality.
Its native and L2 benchmark commands are documented in the host tools guide;
search-limit results are reported separately from proven exact solutions.

## Beyond PBQP: Bellman–Ford

The host-only [Bellman–Ford probe](doc/generality-probe.md) runs
single-source shortest paths through pcaalib and the SystemC accelerator,
then checks the results against an independent software implementation. It
covers negative weights, unreachable vertices, ties, and fixed-point road
distances:

```sh
cmake --build build --target probes_device_unit probes_unit
ctest --test-dir build -R '^probes(_device)?_unit$' --output-on-failure
```

This checks ISA generality, not device speed. The software-only tests also
study negative cycles; the device test checks that a path causing finite
negative underflow reports an error instead of silently saturating.

## Build and verify

Run the host model and its tests with:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

For the RV64 path, install Spike and a RISC-V bare-metal toolchain, then
configure against the matching Spike source tree:

```sh
cmake -S . -B build -DSPIKE_SOURCE_DIR=../riscv-isa-sim
cmake --build build --target basic_elf randomized_elf pbqp_basic_elf pbqp_randomized_elf pbqp_rn_elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/pbqp_rn.elf
```

The other generated ELFs run with the same Spike options. For architecture
details and test methodology, see [`doc`](doc); for the compact wire-format
constants and MMIO ABI, see [`accel_protocol.h`](accelerator/include/accel_protocol.h).
