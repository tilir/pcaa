# Workload characterization design

`pbqp_workload` is a host-only C++ tool. It intentionally does not reuse the
fixed-capacity bare-metal PBQP representation: it characterizes graph topology
and logical CostKernel submissions using dynamic host containers.

The generator library is `pcaa_workload`. It provides deterministic trees,
paths, stars, cycles, 2-trees, and irreducible complete cores, with small,
register-allocation-like synthetic, and large-domain profiles. It records the
generator family, profile, node count, and seed in every CSV row.

Run the representative corpus with:

```sh
cmake --build build --target pbqp_workload
build/pbqp_workload --trace build/pbqp-workload.csv
```

## CSV trace format

Each row is one logical current-ABI min/argmin submission. It contains no host
pointers. `reduction` is `R1` or `R2`; `opcode` is the current logical
primitive; `x_domain`, `y_domain`, and `z_domain` describe the reduction; and
the remaining fields describe operand layout, logical traffic, and explicit
scratch packing. `z_domain` is zero for R1.

`batch_start` marks exactly the first primitive in an implemented R1 or R2
software batch. `batch_size` is that batch's primitive-descriptor count and
`batch_scratch_bytes` is packing performed once for its unique strided views.
The trace preserves the production solver's first-index reducible-node policy
and edge orientation, including fill edges, so its R1/R2 and layout counters
model the same scheduling and `EdgeView` rules as the solver.

The current baseline records one result per row. The report derives alternatives
without changing the ABI: stride-aware operands retain the row count and remove
`scratch_bytes`; implemented software batches use one command per R1/R2 and
reuse packed views; structural batches remain analytical. Logical map elements
and device operand bytes remain unchanged by these transformations.

The tool also replays traces through the configured L1 model. It prints raw
descriptor/control, read, compute, and write demand separately from exposed
streaming service cycles, plus a controlled lane-only sweep. The checked-in
interpretation and parameter values are in `doc/l1-performance-model.md`.

## Interactive timing runner

`pcaa_graph_run` is intentionally untimed, so graph-format experiments keep a
fast functional path. `pcaa_graph_run_timed` is the separate interactive L1
view: it uses streaming overlap, four lanes, the default byte rates from
`AccelTimingConfig`, and a nominal 1 ns cycle period. Its output keeps total
service cycles separate from descriptor, operand-read, compute, and
result-write components. This runner reports an estimate; it does not change
the PBQP descriptor ABI or the software solver policy.
