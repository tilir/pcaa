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
estimate, and an RV64 bare-metal integration through Spike. A PBQP runner
exercises the accelerator; an independent Bellman–Ford probe checks the same
cost-algebra vocabulary on shortest paths. Neither algorithm is built into
the ISA. The command contract is in [the architecture specification](doc/arch.md),
with its rationale in [the ISA decision record](doc/isa-v1-decision.md).

## Run a graph through PCAA

For the host runner, install CMake, a C++17 compiler, SystemC 3.x, and
GoogleTest. Spike and the RISC-V toolchain are not needed for this path.

```sh
cmake -S . -B build
cmake --build build --target pcaa_graph_run pcaa_graph_run_timed
build/pcaa_graph_run --solver bare-metal examples/triangle.pbqp
build/pcaa_graph_run_timed --solver bare-metal examples/triangle.pbqp
```

Both commands print a cost and an assignment. `pcaa_graph_run` is untimed;
`pcaa_graph_run_timed` additionally reports estimated total, descriptor,
operand-read, compute, and result-write cycles for a fixed four-lane
configuration. Zero device cycles mean the selected solve needed no PCAA
primitives.

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
  up to 65,536 choices per node.

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

## Beyond PBQP: Bellman–Ford

The host-only [Bellman–Ford probe](doc/generality-probe.md) computes
single-source shortest paths with per-vertex min-plus/argmin relaxations—the
same operation shape exposed by PCAA's two-input argmin primitive. It tests
negative weights, unreachable vertices, ties, and negative cycles:

```sh
cmake --build build --target probes_unit
build/probes_unit --gtest_filter='BellmanFordTest.*'
```

This is an ISA-generality check, not a device benchmark: the probe does not
submit SystemC descriptors. Negative-cycle detection uses a separate exact
64-bit check because bounded PCAA costs cannot represent unbounded decreases.

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
details and test methodology, see [`doc`](doc); for the public C descriptor
ABI, see [`accel_protocol.h`](accelerator/include/accel_protocol.h).
