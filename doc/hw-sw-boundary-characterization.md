# HW/SW boundary characterization

This analytical study asks how much existing cost-algebra work can run between
mandatory host decisions. It does not add a graph-aware PCAA implementation,
an opcode, timing, area, power, or speedup estimate. Reproduce the raw epoch
data with `cmake --build build --target hw_sw_characterization`; it writes
`build/hw-sw-epochs.csv` from the public graph generator and runner.

## Trace and epoch definition

`pcaa_graph_run --trace FILE` writes deterministic JSONL solver events. An
event has sequence, phase, type, RN policy, node/choice, active graph state,
reduction counts, generic operation elements, descriptors, and logical
operand/result bytes. Events include R0/R1/R2, RN selection/scoring/commit,
local scoring/moves, and branch selection/conditioning. The solver, rather
than the analysis script, determines their order and work.

An epoch is the maximal sequence of those events that can execute without a
host decision under the stated boundary. Model A submits one structural
cost-algebra event at a time. Model B submits one selected graph reduction;
R0 is counted as a host-side graph action with zero accelerator arithmetic.
Model C starts with a host-selected RN and includes its RN work plus following
R0/R1/R2 reductions up to the next irreducible core. Model D runs the complete
heuristic for its configured policy. Thus the reported interaction count is a
host decision/submission count, not a latency estimate.

## RN boundaries

The table shows 20-node corpus means. `epochs/solve` is also the relevant
interaction granularity for B; for C it is the number of RN/cascade handoffs;
D has one start interaction. Element percentiles are across epochs, not across
solves. Bytes are median/max logical operand plus result traffic per epoch.

| Family | policy | boundary | epochs / solve | p10 / median / p90 elements | median / max bytes |
|---|---|---|---:|---:|---:|
| degree-3 | min-degree | B atomic reduction | 19.0 | 8 / 8 / 24 | 128 / 264 |
| degree-3 | min-degree | C RN+cascade | 5.0 | 36 / 48 / 48 | 648 / 648 |
| degree-3 | min-degree | D whole heuristic | 1.0 | 228 / 228 / 228 | 3,032 / 3,032 |
| degree-4 | min-degree | B atomic reduction | 19.0 | 8 / 8 / 24 | 128 / 352 |
| degree-4 | min-degree | C RN+cascade | 2.0 | 32 / 32 / 156 | 352 / 2,360 |
| degree-4 | min-degree | D whole heuristic | 1.0 | 188 / 188 / 188 | 2,712 / 2,712 |
| mixed-degree | min-degree | B atomic reduction | 18.9 | 18 / 45 / 67 | 504 / 896 |
| mixed-degree | min-degree | C RN+cascade | 15.3 | 33 / 52 / 81 | 552 / 2,004 |
| mixed-degree | min-degree | D whole heuristic | 1.0 | 718 / 847 / 996 | 9,092 / 11,080 |

Degree-3 and degree-4 show the direct benefit of autonomous exact cascades:
Model C reduces handoffs from about nineteen to five or two. Mixed-degree
min-degree remains fragmented because most RN decisions have no following
exact reduction, despite much larger total work. Model D is an upper bound on
granularity and requires full graph ownership.

RN policy changes synchronization structure without necessarily reducing total
work. Degree-4 max-degree produces seven C epochs per solve versus two for
min-degree/min-work; its median C epoch is 48 elements, compared with 32.
For mixed-degree, max-degree produces 7.1 C epochs with median 133 elements,
while min-degree/min-work produce about fifteen epochs of 52 elements. This is
a trade-off to retain for later workload studies, not a HW-aware policy.

## Local search and exact search

LS-A exposes one node-score epoch (slice accumulation plus argmin) per node
evaluation. LS-B groups the deterministic sequence of node scores into one
sweep; the raw CSV retains both forms. The large repeated sweep work makes
local search a stronger candidate for sweep-level autonomy than for one-score
submissions. RN+local-search is recorded as its RN epochs followed by the same
local-score trace, so reduction and refinement remain distinguishable.

Exact branch-and-reduce remains software-owned: branch selection, snapshots,
and traversal are host responsibilities. Its `BRANCH_CONDITION` event and the
following exact reductions identify the candidate graph-aware epoch; the study
does not model a whole search tree in hardware.

## Graph state and observability

Every trace event carries active nodes/edges, maximum degree, unary elements,
and pairwise-matrix elements. These are the state-access requirements of C/D,
not an SRAM proposal. In the 20-node binary regular families the initial graph
has 20 active nodes, 30 or 40 edges, 40 unary elements, and 120 or 160 matrix
elements (640 or 800 cost-table bytes). Mixed-degree traces expose their actual
larger, nonuniform state directly in the raw CSV path.

Useful future hardware telemetry would include completed structural commands,
logical elements, operand/result bytes, epochs, host-attention events, and
autonomous R0/R1/R2/RN counts. Solver telemetry such as objective gaps and
policy quality remains more accurate in software. Lane utilisation and memory
stall cycles become meaningful only with a later timing implementation.

## Limitations

These are logical-work partitions over synthetic graphs. They assign no cost to
host interaction, graph bookkeeping, memory hierarchy, or graph-state storage,
and make no claim that any boundary is worthwhile. The trace and CSV schema are
intended to be reused unchanged on future LLVM PBQP instances.
