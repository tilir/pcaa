# Vector-output primitive cycle projection

This is an analytical projection applied after RN per-node batching.  It does
not implement an opcode or change the descriptor ABI.

## Method

The timed runner observes the exact scalar job shapes issued by the shared
solver and applies the formulas from [the L1 model](l1-performance-model.md):

```text
descriptor = ceil(56 / 16) = 4 cycles
operand    = ceil(unique input bytes / 16)
compute    = sum ceil(reduction length / 4)
result     = ceil(output bytes / 16)
service    = descriptor + max(operand, compute) + result
```

PROJECT groups D scalar reductions sharing one neighbor unary and one matrix
into one D-output descriptor.  MAP3 is projected both as D descriptors each
producing D outputs (partial vector) and as one descriptor producing the D²
matrix.  Unique shared operands are read once per hypothetical descriptor, but
total internal reduction work is unchanged.  Replacing just those measured
scalar components in today's total leaves outer `EXECUTE_BATCH` service and all
other primitives unchanged.  The triangle calibration remains 50 current
cycles; its four scalar MAP3 descriptors project to 14 cycles in the partial
form and 10 in the full form, matching the formula directly.

Data are in `build/vector-cycle-projection.csv`: degree-3, degree-4, and
mixed-degree at N=20/50/100/200 and D=2/4/8/16/32 (three seeds each), plus all
491 LLVM graphs.  `build/vector-cycle-per-edge.csv` supplies the pre-Item-A
control.

## Synthetic result

The table reports mean current cycles divided by the projected total after
replacing both PROJECT and MAP3.  The range spans the three graph families.

| D | partial-vector MAP3 | full-matrix MAP3 | current descriptor share | current compute share |
|---:|---:|---:|---:|---:|
| 2 | 1.68-1.72x | 1.69-2.16x | 61-67% | 12-16% |
| 4 | 2.50-2.76x | 2.76-3.96x | 51-57% | 12-14% |
| 8 | 2.98-3.09x | 2.98-4.16x | 37-45% | 18-22% |
| 16 | 2.74-3.22x | 2.74-3.74x | 24-31% | 24-31% |
| 32 | 2.45-3.17x | 2.45-3.40x | 14-19% | 28-38% |

N changes absolute work almost linearly within a family but does not create a
qualitatively different ratio: descriptor shape is governed mainly by D and by
whether the family spends its work in RN/PROJECT or R2/MAP3.  The gain rises
through D=8-16, then flattens or falls at D=32.  At small D, descriptor fetch is
the dominant removable term.  By D=32, unchanged internal reductions and
operand reads dominate, so one descriptor cannot remove most service.

PROJECT alone is modest for regular degree-3/4 graphs (about 1.00-1.13x, tending
toward 1 as D rises) because MAP3 dominates them.  Mixed-degree PROJECT work is
substantial (1.69x at D=2 and 2.34x at D=32), consistent with its larger RN
cores.

## Real corpus

Across all 491 LLVM graphs, aggregate mean current service is 28,977 cycles per
solve.  The combined partial projection is 9,601 cycles (3.02x); the full MAP3
projection is 9,115 cycles (3.18x).  Current descriptors account for 34.9% and
compute for 24.9% of service.  The corpus's mostly D=7/16 structure lies in the
region where descriptor collapse remains valuable, but the small difference
between partial and full MAP3 shows that PROJECT plus shared operand traffic is
at least as important as collapsing the final D MAP3 descriptors.

Item A must not be confused with this result.  Per-node batching already cuts
the LLVM top-level batch count 78.4%, but only cuts modeled service 1.97%; the
3.02-3.18x ratios above are additional analytical gains from changing child
descriptor shape.

## Caveat

This projects the existing L1 model's fixed-cost formulas onto a hypothetical
descriptor shape.  It is not a validated timing model for hardware that does
not exist.  In particular it assumes the same four-lane internal throughput,
perfect reuse of operands named once by the hypothetical descriptor, no extra
vector-control latency, and no output-buffer backpressure.
