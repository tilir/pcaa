# L1 performance model

This document describes the current SystemC service-cycle **estimate**,
implemented in `accelerator/src/timing_model.cpp` and batch accounting in
`accelerator/src/accelerator.cpp`. It is independent of the normative
[command contract](arch.md): the untimed and timed modes execute the same
semantic ISA 1.0.0 using pcaalib 2.0.0's compact descriptors.

## Primitive accounting

Let `n` be vector/reduction length, `m` projection output count, and `L` the
configured lane count. The model uses these shapes:

| Primitive | Compute chunks `C` | Modeled input cost elements `E` | Result bytes `W` |
| --- | --- | --- | --- |
| Two-input scalar reduction (1, 3) | `ceil(n / L)` | `2*n` | 4 (1), 8 (3) |
| Three-input scalar reduction (2, 4) | `ceil(n / L)` | `3*n` | 4 (2), 8 (4) |
| Vector add (6) | `ceil(n / L)` | `2*n` | `4*n` |
| Min-plus projection (7) | `m*ceil(n / L)` | `n*(m+1)` | `4*m` |
| Three-input projection (8) | `m*ceil(n / L)` | `n*(m+2)` | `8*m` |

Projection input accounting assumes that each shared vector is read once per
primitive, while the matrix contributes `m*n` elements. This is idealized
reuse in the timing estimate; the functional executor may call `MemoryInterface`
more often. It is not evidence of a cache or operand buffer. Solver logical
traffic counters may also count repeated per-output inputs, so they are not
interchangeable with this timing model's input bytes.

For encoded descriptor size `B`, configured descriptor/read/write byte rates,
and setup latencies from `AccelTimingConfig`:

```text
descriptor   = ceil(B / descriptor_bytes_per_cycle)
operand read = ceil(4 * E / memory_read_bytes_per_cycle)
compute      = primitive_start_cycles + map_pipeline_latency + C
               + reduction_tree_latency + result_latency
               + add3_map_extra_latency for opcodes 2, 4, and 8
result write = ceil(W / memory_write_bytes_per_cycle)
```

`B` comes from the codec: scalar two-input reductions use 32 bytes,
three-input reductions 48, vector add 32 (exact in-place alias) or 48
(general), projection 48, and three-input projection 64. There is no common
56- or 80-byte primitive footprint in the current encoding. The model applies
the configured setup terms uniformly, including `reduction_tree_latency` to
vector add; this is a model parameter, not a claim that opcode 6 performs a
reduction.

Sequential service sums all four categories. Streaming service is
`descriptor + max(operand read, compute) + result write`; the overlap is
within one primitive. Ordered children are serviced in sequence. Untimed
mode records zero service cycles. A zero lane count or primitive byte rate
also suppresses the primitive estimate; use positive values for timed runs.

## Ordered batch accounting

In addition to its children, each `EXECUTE_BATCH` pays:

```text
outer descriptor = ceil(32 / descriptor_bytes_per_cycle)
batch setup      = batch_start_cycles
completion write = ceil(8 / memory_write_bytes_per_cycle)
```

Batch setup contributes to total service, separately from the four printed
categories. With streaming overlap, their sum need not equal total service.
The estimator excludes host packing/search time, MMIO round-trip time, CPU/device
overlap, cache, DMA, FIFO behavior, stride penalties, and pipeline hazards.

## Timed runner and calibration

`pcaa_graph_run` remains untimed. `pcaa_graph_run_timed` selects streaming mode,
four lanes, 16 descriptor/read/write bytes per cycle, zero setup latencies,
and a nominal 1 ns cycle period. It prints total, descriptor, operand-read,
compute, and result-write cycles, plus primitive and batch counts:

```sh
build/pcaa_graph_run_timed --solver bare-metal examples/triangle.pbqp
```

The current triangle regression is **28** total service cycles: 16 descriptor,
6 operand-read, 6 compute, and 6 result-write cycles. Its command stream has
two opcode-3 scalar reductions, two opcode-8 vector projections, and two
batch parents. The earlier scalar stream's 50-cycle result belongs to the
[historical vector-output study](reports/vector-primitive-cycle-projection.md), not
to the current regression.

Default RN scoring uses ordered `MINPLUS_PROJECT -> COST_ADD_VECTOR` chains,
one pair per incident edge and one batch for the selected node. The
`--rn-batching per-edge` control retains scalar value-only RN projection and
software accumulation; it also selects scalar R2 reductions. R1 and local
search use scalar argmin reductions; default R2 uses `MINPLUS_MAP3_PROJECT`.
An irreducible core solved entirely in software submits no primitives, so its
zero device cycles describe software-core solving rather than a fast device
execution.

Historical timing ratios used an earlier descriptor format and scalar command
stream. They remain evidence for that experiment; they must be regenerated
before comparison with compact-encoding service. The refinement route and
simulation ownership are described in [design.md](design.md#model-refinement-route).

The calibration output is retained with the measurement archive:
[current triangle trace](reports/data/recheck/current-triangle-timing.txt).
