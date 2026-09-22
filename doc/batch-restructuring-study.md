# RN batch restructuring study

This study isolates a software-only change: `ReduceRN` now submits all scalar
projection jobs needed to score one RN node in one `EXECUTE_BATCH`.  The old
one-batch-per-edge path remains selectable with `pcaa_graph_run --rn-batching
per-edge`; `per-node` is the default.  Neither primitive descriptors nor the
accelerator ABI changed.

## Correctness and method

`pbqp_unit` records every projected scalar term in edge/value order and checks
that both paths produce identical terms, hence bit-identical RN score vectors
after the unchanged ordered host fold.  It also checks identical assignments
and optima.  `pcaa_graph_run_rn_batching_regression` repeats the solution and
descriptor checks over every `examples/*.pbqp` input and 12 evenly spaced LLVM
RegAllocPBQP inputs.  The normal host and RV64/Spike suites provide the remaining
coverage.

For RV64, the largest legal RN batch is 63 neighbors × 6 choices = 378 jobs.
The adapter reserves 384 jobs and packed strided views; its context is 40,032
bytes.  Disassembly of `pbqp_rn.elf` shows a 41,376-byte `main` frame (the
context plus other locals), a 2,688-byte recursive branch frame, and measured
maximum exact depth 5 on the bare-metal RN regression.  Including `Solve` and
the largest reduction helper gives a conservative call-chain bound below
60 KiB, well inside the 1 MiB startup stack.  Graph/search snapshots remain in
their caller arenas rather than on that stack.

The full before/after data are `build/isa-round3-per-edge.csv` and
`build/isa-round3-scaling.csv`: the standard synthetic scaling sweep plus all
491 `examples/regalloc` graphs.  `build/vector-cycle-per-edge.csv` and
`build/vector-cycle-projection.csv` apply the fixed four-lane timed runner to
180 representative synthetic points (three families, N=20/50/100/200,
D=2/4/8/16/32, three seeds) and the full real corpus.  Generated CSVs are build
artifacts, not source inputs.

## Result

Full sweep/corpus logical counters:

| Set | solves | batches, per-edge | batches, per-node | reduction | child descriptors |
|---|---:|---:|---:|---:|---:|
| synthetic sweep through D=32 | 407 | 868,034 | 92,838 | 89.3% | 13,122,640 / 13,122,640 |
| LLVM corpus | 491 | 71,288 | 15,363 | 78.4% | 1,225,955 / 1,225,955 |

Timed representative data:

| Timed set | solves | batches, per-edge | batches, per-node | reduction | child descriptors | L1 cycles, per-edge | L1 cycles, per-node |
|---|---:|---:|---:|---:|---:|---:|---:|
| synthetic subset | 180 | 133,170 | 16,470 | 87.6% | 4,109,484 / 4,109,484 | 91,120,186 | 90,536,686 |
| LLVM corpus | 491 | 71,288 | 15,363 | 78.4% | 1,225,955 / 1,225,955 | 14,507,467 | 14,227,842 |

The cycle difference is exactly the removed top-level batch-descriptor service
in the present L1 model: 5 cycles per eliminated submission.  It is 0.64% of
the synthetic aggregate and 1.97% of the LLVM aggregate.  Child descriptor
count, primitive compute, operand bytes, result bytes, and the RN scoring order
do not change.

## Interpretation

The restructuring captures most of the opportunity that is specifically a
software round trip: roughly four-fifths to seven-eighths of top-level batch
submissions disappear, and every RN decision now needs one.  It does **not**
capture the much larger descriptor-shape opportunity.  Scalar child
descriptors still dominate service at useful D, which is why removing most
submissions changes modeled cycles by only 0.6-2.0%.  A new primitive is not
needed merely to batch across a node, but would still be needed to collapse the
D or D² scalar child descriptors analyzed in the vector projection study.

This is L1 modeled service, not host wall-clock speedup or a measured hardware
round-trip latency.  The model intentionally contains no CPU/device overlap.
