# Solver characterization

This report describes a reproducible workload study of the shared PBQP solver.
It is deliberately an experiment around the public command-line tools, not a
second implementation of solver logic. Recreate its data with:

```sh
cmake --build build --target solver_characterization
```

The target invokes `scripts/rn_characterize.rb`, which invokes only
`pbqp_graph_generate` and `pcaa_graph_run`. It writes a raw
`build/solver-characterization.csv` and a generated
`build/solver-characterization-summary.md`.

## Methods

The general corpus has ten fixed seeds (1001--1010) for each of six 20-node
families: binary random-sparse, binary degree-3, binary degree-4, small-domain
mixed-degree, register-like trees, and register-like cycles. `REDUCE_ONLY`,
three RN policies, `LOCAL_SEARCH`, and each policy's
`HEURISTIC_RN_LOCAL_SEARCH` hybrid are measured on every graph. A separate
eight-node subset of random-sparse, degree-3, degree-4, and mixed-degree
graphs runs both exact solvers with a 100,000-node explicit search limit.

All 20-node trees and cycles finish under exact low-degree reduction alone.
The degree-3, degree-4, and mixed-degree graphs are irreducible in all ten
seeds. The exact solvers agree on all 40
small-subset inputs. Their results are the reference for reported heuristic
gaps; a missing reference is left blank rather than guessed.

## RN Cascade Characterization

An RN episode is one RN decision followed by all exact R0/R1/R2 reductions up
to the next RN decision or completion. The runner exports stable verbose fields
for episode count, R0/R1/R2 totals, total/mean/maximum length, and the exact
length histogram. The CSV contains `cascade_len_0` through `cascade_len_64`.
The script aggregates those solver counters; it does not recreate cascade logic
from graph state.

This table covers successful 20-node runs that execute RN. `zero / one /
multiple` is the fraction of episodes with that many exact reductions. The
composition column is the R0/R1/R2 fraction of all exact reductions in the
cascades. RN+local-search has an identical RN phase, and therefore the same
cascade rows as the corresponding RN policy.

| Family | RN policy | RN decisions / solve | exact reductions / episode (mean / median / max) | zero / one / multiple | R0 / R1 / R2 composition |
|---|---|---:|---:|---:|---:|
| degree-3 | min-degree | 5.0 | 3.0 / 3.0 / 3 | 0.0% / 0.0% / 100.0% | 6.7% / 6.7% / 86.7% |
| degree-3 | max-degree | 5.0 | 3.0 / 3.0 / 3 | 0.0% / 0.0% / 100.0% | 6.7% / 6.7% / 86.7% |
| degree-3 | min-work | 5.0 | 3.0 / 3.0 / 3 | 0.0% / 0.0% / 100.0% | 6.7% / 6.7% / 86.7% |
| degree-4 | min-degree | 2.0 | 9.0 / 9.0 / 18 | 50.0% / 0.0% / 50.0% | 5.6% / 5.6% / 88.9% |
| degree-4 | max-degree | 7.0 | 1.9 / 2.0 / 3 | 14.3% / 0.0% / 85.7% | 7.7% / 7.7% / 84.6% |
| degree-4 | min-work | 2.0 | 9.0 / 9.0 / 18 | 50.0% / 0.0% / 50.0% | 5.6% / 5.6% / 88.9% |
| mixed-degree | min-degree | 14.8 | 0.3 / 0.0 / 4 | 85.1% / 7.4% / 7.4% | 22.2% / 22.2% / 55.6% |
| mixed-degree | max-degree | 6.6 | 1.9 / 2.0 / 5 | 22.7% / 22.7% / 54.5% | 7.9% / 7.9% / 84.3% |
| mixed-degree | min-work | 13.9 | 0.4 / 0.0 / 5 | 82.0% / 9.4% / 8.6% | 18.5% / 18.5% / 63.0% |

Degree-3 graphs consistently have useful three-reduction cascades, principally
R2. Degree-4 behavior depends on the RN policy: min-degree and min-work
alternate zero-length episodes with long cascades, while max-degree makes more
RN decisions and usually obtains a short cascade. In mixed-degree graphs,
min-degree and min-work commonly lead immediately to another RN, though their
less frequent cascades still lean toward R2. Exact reduction work between RN
decisions is therefore substantial for regular degree-3 and degree-4 inputs,
but it cannot be assumed for every general graph or policy. These are factual
software/hardware-boundary observations, not a timing model or speedup claim.

## Operation Mix by Graph Family

The following values are mean logical work per successful 20-node run.
`PROJECT` (`MINPLUS_PROJECT`) includes R1 and RN scoring projections;
`PROJECT_ACC` (`PROJECT_ACCUMULATE`) is RN score-vector accumulation;
`SLICE` (`SLICE_ACCUMULATE`) includes conditioning and local-search score
construction; `MAP3` (`MAP3_REDUCE`) is R2; and `ARGMIN`
(`ARGMIN_VECTOR`) is local-search vector choice. `descriptors` counts current
primitive descriptors and `batches` their submitted batches. Operand and
result bytes are logical traffic across the five categories.

One `SLICE_ACCUMULATE`, `MINPLUS_PROJECT`, or `MAP3_REDUCE` element represents
a different arithmetic and reduction structure. These counts characterize
operation shape, not equal hardware cost: no cycle, area, or hardware-time
percentage is derived from element percentages.

| Family | Solver | PROJECT | PROJECT_ACC | SLICE | MAP3 | ARGMIN | descriptors | batches | operand bytes | result bytes |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| degree-3 | RN | 64.0 | 30.0 | 30.0 | 104.0 | 0.0 | 84.0 | 29.0 | 2,240.0 | 792.0 |
| degree-3 | RN+local | 64.0 | 30.0 | 174.0 | 104.0 | 48.0 | 108.0 | 53.0 | 3,776.0 | 1,560.0 |
| degree-3 | local search | 0.0 | 0.0 | 4,896.0 | 0.0 | 1,632.0 | 816.0 | 816.0 | 52,224.0 | 26,112.0 |
| degree-4 | RN | 32.0 | 14.0 | 14.0 | 128.0 | 0.0 | 80.0 | 24.0 | 2,016.0 | 696.0 |
| degree-4 | RN+local | 32.0 | 14.0 | 206.0 | 128.0 | 48.0 | 104.0 | 48.0 | 3,936.0 | 1,656.0 |
| degree-4 | local search | 0.0 | 0.0 | 6,576.0 | 0.0 | 1,644.0 | 822.0 | 822.0 | 65,760.0 | 32,880.0 |
| mixed-degree, RN-required | RN | 466.3 | 148.5 | 157.1 | 90.7 | 0.0 | 182.1 | 55.2 | 7,263.6 | 2,085.2 |
| mixed-degree, RN-required | RN+local | 466.3 | 148.5 | 791.1 | 90.7 | 107.8 | 218.1 | 91.2 | 13,198.0 | 4,909.2 |
| mixed-degree | local search | 0.0 | 0.0 | 28,906.4 | 0.0 | 4,984.4 | 1,668.0 | 1,668.0 | 271,126.4 | 128,969.6 |

All mixed-degree 20-node inputs in this corpus require RN; the label makes the
subset explicit for future corpus extensions. Standalone local search remains
over the full family because it has no RN episode. The aggregate MAP3-heavy
result remains true for the regular degree-3 and degree-4 RN workloads. The
mixed-degree RN-required subset has substantially more `MINPLUS_PROJECT` work
because it makes many more RN decisions.

For comparison, the raw CSV also retains the whole-general-corpus aggregate,
per-policy results, exact-search counters, and every cascade-length bin. The
exact solvers run only on the separate eight-node subset, so they are not
presented as a like-for-like 20-node operation-mix comparison.
