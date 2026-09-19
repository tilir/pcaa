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
