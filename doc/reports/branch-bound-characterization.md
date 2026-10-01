# Branch-and-bound characterization

Original report revision `dd1402b`; search tables were replaced by a fresh
run of that implementation on 2026-10-01. The original real-sample filename
list was not saved. The new sample is explicit in
[data/recheck/branch-bound.csv](data/recheck/branch-bound.csv): the same 26
synthetic inputs plus the 39 real inputs retained in the fork study. This
verifies the historical scalar solver, not current compact-ISA timing.
See [the documentation index](../README.md) for current contracts.

This is a logical-work study of `exact-branch-reduce`'s search-tree
structure, not a speedup or hardware-parallelism claim. It does two things:
states the fact of whether incumbent-based pruning existed before this
pass, and (having added it) characterizes how much of the search tree it
actually cuts on both synthetic and real graphs.

## 1. Fact: no incumbent-based pruning existed before this pass

`SolveBranchAndReduce` in `software/pbqp/pbqp.cpp` recursed into every
child branch value unconditionally and only compared
`candidate.optimum < best.optimum` *after* each child fully returned. There
was no mechanism to skip a branch once it could no longer beat a
known-better solution — `exact-branch-reduce` was exhaustive branch+reduce,
bounded only by `--maximum-search-nodes`, not a true branch-and-bound.

## 2. The pruning added

A minimal incumbent-bound check was added, gated behind a new
`best_known` parameter threaded through the existing recursion (default
`ACCEL_INF`, i.e. no bound, at the top-level call — this guarantees the
change can never leave the root without a solution; see the proof sketch in
the code comment above `SolveBranchAndReduce`). Right after each call's own
R0/R1/R2 reduction loop converges — exactly the point where
`state->objective_offset` already holds the cost locked in by every
reduction on the path from the root — a cheap lower bound for whatever
remains active is added to it and compared against the caller's incumbent:

```text
bound = objective_offset + RemainingCoreLowerBound(state)
if bound >= best_known: prune this branch
```

`RemainingCoreLowerBound` sums each still-active node's own cheapest unary
choice plus each still-active edge's cheapest matrix entry. This bound is
valid **regardless of cost sign** — PBQP costs can be negative (see e.g.
`examples/triangle.pbqp`), so a bound that assumed non-negativity would be
unsound and could prune away the true optimum. The sum-of-minima bound
never has that problem: for any real assignment, each node/edge contributes
at least its own minimum entry, by definition, independent of sign.

A pruned call returns a new internal-only `PBQP_PRUNED` status that its
immediate caller consumes (skip this branch value, keep the aggregated
reduction-work statistics, move to the next value) rather than the usual
hard-error propagation the existing status codes trigger; `PBQP_PRUNED`
never reaches `pbqp_solver_solve`'s return value. Each child receives
`min(best_known, this level's own current best)` as its own bound, so
pruning strength compounds down the tree rather than only using
whatever the direct parent found. A new `search_nodes_pruned` statistic
counts prune events (exposed in `pcaa_graph_run --verbose`'s
`exact search ... pruned=N` line and in `pbqp_statistics_t`).

**Correctness**: `software/tests/pbqp_rn.c` gained a dedicated regression
using the same 12-vertex Chvatal graph as `examples/chvatal.pbqp` (its
irreducible RN core needs several sequential branch decisions), checked
under all three RN policies, asserting `search_nodes_pruned > 0` and that
software/accelerator kernels report identical pruning and identical
optima — passes under Spike. Separately, every file in `examples/*.pbqp`
was cross-checked: `exact-branch-reduce`'s optimum matches
`exact-core-enumeration`'s on every one, with and without this change. The
full existing test suite (27 CTest cases including `pbqp_unit`, both
`build` and `build-release`) and the required bare-metal ELF/Spike list
from AGENTS.md all still pass.

## 3. Synthetic families: depth, pruning, and tree size

Small points only — exact search is exponential in the irreducible core's
domain size, so these stay in the tens-of-nodes range (`--maximum-search-nodes
2000000`, 30s cap; nothing hit either limit). Two seeds per point at
degree-3 N=20/30 (D=2,4) and N=50 (D=2); degree-4 N=20/50 (D=2,4) and
N=100 (D=2); mixed-degree N=15/20/25 (D=2). All columns are measured
solver statistics; "DFS open bound" is derived from them in §5.

| Family | depth (min/median/max) | nodes visited (min/median/max) | pruned (median/min/max %) | DFS open bound (max) |
|---|---:|---:|---:|---:|
| degree-3 | 5 / 7 / 12 | 43 / 647 / 18,189 | 49.7 / 41.9 / 74.9 | 22 |
| degree-4 | 2 / 2 / 2 | 7 / 7 / 21 | 42.9 / 0.0 / 66.7 | 7 |
| mixed-degree | 5 / 16.5 / 22 | 57 / 28,012 / 178,701 | 49.9 / 47.4 / 50.0 | 23 |

This matches the graph-topology regimes already established in
[scaling-characterization.md](scaling-characterization.md#2-headline-finding-graph-topology-decides-the-regime-not-just-n-or-d):
degree-4's RN core stays tiny (depth pinned at 2, matching its
constant-RN-count finding) so there is almost nothing to prune or widen;
degree-3 and mixed-degree have larger, N-growing irreducible cores, so both
depth and total tree size grow with N — mixed-degree dramatically so
(depth 22 and 178,701 visited nodes at just N=25, with D=2). Pruning removes
roughly half the visited nodes for degree-3/mixed-degree at these sizes,
consistently across seeds and N.

## 4. Real corpus: shallower trees, more pruning at low RN, much less at high RN

The filenames are the fork report's deterministic sample: the first 30
successful scaling rows with `1 <= RN <= 3`, then the first nine with
`4 <= RN <= 8`. Search limits are one million nodes and 45 seconds per
real graph. This replaces the original unrecorded real selection; its pruning
percentages and wall times could not be independently reconstructed.

| Subset | n | pruned (median/min/max %) | wall time (median/max) |
|---|---:|---:|---:|
| RN 1-3 | 30 | 82.4 / 62.5 / 89.7 | 0.10s / 0.20s |
| RN 4-8 | 9 (7 completed) | 83.2 / 1.9 / 87.7 | 0.20s / 15.12s |

Percentages and wall-time summaries use completed runs only; median is the
average of the middle pair when the sample count is even. Two inputs timed
out at 45 seconds: `028-instcombine-faddend-scale-n17-d16.pbqp` and
`044-instcombine-instcombiner-replaceinstuseswith-n16-d16-r1.pbqp`. Their
partial traces are retained, but finish-only counters/optima are left blank,
not presented as completed search results. Wall times include verbose logging,
trace I/O, and concurrent report collection on this host; they are
reproduction observations, not hardware or solver speedup measurements.

Across all 37 completed real graphs: depth min 1, median 2, max 6, much
shallower than the synthetic maximum of 22. The real domains go up to 17,
so search is wider but shallower. Within the high-RN subset the completed
pruning fraction ranges from 1.9% to 87.7%; RN count alone cannot predict
pruning strength. The earlier claim that its median was 17.9% is withdrawn:
it belonged to an unspecified selection, whereas the explicit replacement
sample has median 83.2%.

## 5. Is frontier width large enough to matter?

An earlier revision of this report divided `search_branches_created` by
`search_maximum_depth` and called the result an "average branching factor
proxy". That quantity is neither a branching factor nor a bound on frontier
width (for a complete binary tree of depth `d` it grows like `2^(d+1)/d`
while every node has two children), so it has been removed, together with
the "tens-to-low-hundreds" conclusion drawn from it. What the data does
support:

**Depth** is measured exactly (`search_maximum_depth`): median 2, max 6 on
the real-corpus sample; 2-22 on the synthetic points.

**Open nodes of the search as implemented** are bounded exactly by depth
and domain. `exact-branch-reduce` is a sequential depth-first search: at
any instant the open (created, not yet finished or pruned) nodes are the
current node plus the not-yet-tried sibling values of each ancestor on the
current path, so their number is at most `1 + sum over path levels
(D_level - 1) <= 1 + depth * (D_max - 1)`. With the measured depths this
gives at most 7 (degree-4), 22 (degree-3), 23 (mixed-degree, D=2) and
`1 + 6 * 16 = 97` for the deepest real-corpus sample (D up to 17); for the
median real graph (depth 2, D=16) it is 31.

**Independent bound computations available to a parallel search** are a
different quantity — the width of a tree level (or of any antichain) — and
are bounded only by the tree itself: at most `min(D^k, nodes visited)` at
level `k`. Those trees reach tens of thousands of nodes on the
mixed-degree points and on some real RN 4-8 graphs (§4), so the measured
data neither shows that this width stays small nor that it is large; the
current trace carries no per-event depth, so level widths were not
recorded. **This question remains open.** Answering it needs a trace or
counter of open nodes per depth, not a derived ratio.

## 6. Limitations

- No CPU/accelerator cycle, timing, area, or speedup claims. "Parallelism"
  in §5 refers to a purely combinatorial opportunity (independent bound
  computations), not a hardware parallelism claim.
- Synthetic points are capped at tens of nodes and small domains (D≤4 for
  degree families, D=2 for mixed-degree) because exact search is
  exponential in irreducible-core domain size; larger points were not
  attempted per AGENTS.md's "do not run exact exhaustive solvers on large
  scaling points."
- The real-corpus sample (39 of 491 graphs, chosen by low-to-moderate RN
  count for tractability) is not a claim about the other ~450 graphs, many
  of which have RN counts well beyond what this pass's timeouts could
  characterize with exact search — that is expected and consistent with
  `heuristic-rn`/`local-search` existing as the strategies actually used at
  scale.
- Level (parallel) frontier width was not measured; §5 gives only the exact
  depth-first open-node bound and states why it does not bound level width.
- No change to reduction rules, RN scoring, or the accelerator ISA. The
  only solver-visible change is the new `search_nodes_pruned` statistic and
  the internal `PBQP_PRUNED` status, which never escapes
  `pbqp_solver_solve`.

## Evidence and verification

All 65 explicit inputs were rerun at `dd1402b`: 63 complete, two timeout. Per-input `search-*.txt`, raw `search-*.jsonl.gz` and synthetic input files are retained alongside the CSV. Completion counters stay blank for timeout rows. The real table replaces an unspecified historical sample.

Raw evidence:

- [recheck/branch-bound.csv](data/recheck/branch-bound.csv)
- [recheck/fork-heuristic.csv](data/recheck/fork-heuristic.csv)

See [the evidence inventory](data/README.md) for pinned commands and provenance,
[checksums](data/SHA256SUMS), and [offline verification results](data/verification.json).
