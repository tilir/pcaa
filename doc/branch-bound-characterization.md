# Branch-and-bound characterization

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
full existing test suite (23 CTest cases including `pbqp_unit`, both
`build` and `build-release`) and the required bare-metal ELF/Spike list
from AGENTS.md all still pass.

## 3. Synthetic families: depth, pruning, and branching factor

Small points only — exact search is exponential in the irreducible core's
domain size, so these stay in the tens-of-nodes range (`--maximum-search-nodes
2000000`, 30s cap; nothing hit either limit). Two seeds per point; "branches
/ depth" is `search_branches_created / search_maximum_depth`, an average
branching-factor proxy for how wide the tree tends to be (see the frontier
width caveat in §5).

| Family | depth (min/median/max) | pruned (median/min/max %) | branches/depth (median/max) |
|---|---:|---:|---:|
| degree-3 | 5 / 7 / 12 | 49.9 / 41.9 / 74.9 | 166 / 2,598 |
| degree-4 | 2 / 2 / 2 | 42.9 / 0.0 / 66.7 | 3.0 / 10.0 |
| mixed-degree | 5 / 17 / 22 | 50.0 / 47.4 / 50.0 | 1,863 / 8,123 |

This matches the graph-topology regimes already established in
[scaling-characterization.md](scaling-characterization.md#2-headline-finding-graph-topology-decides-the-regime-not-just-n-or-d):
degree-4's RN core stays tiny (depth pinned at 2, matching its
constant-RN-count finding) so there is almost nothing to prune or widen;
degree-3 and mixed-degree have larger, N-growing irreducible cores, so both
depth and branching factor grow with N — mixed-degree dramatically so
(depth 22, tree width in the thousands at just N=25). Pruning removes
roughly half the visited nodes for degree-3/mixed-degree at these sizes,
consistently across seeds and N.

## 4. Real corpus: shallower trees, more pruning at low RN, much less at high RN

Sampled from the real corpus in `examples/regalloc/` (§4/§5's methodology
mirrors item 1/2's runs): 30 graphs with `1 <= RN <= 3` (all completed
in ~0.1-0.2s) and 9 graphs with `4 <= RN <= 8` (`--maximum-search-nodes
1000000`, 45s cap; 2 of 9 hit the cap — see §5).

| Subset | n | pruned (median/min/max %) | wall time (median/max) |
|---|---:|---:|---:|
| RN 1-3 | 30 | 82.4 / 62.5 / 89.4 | 0.11s / 0.21s |
| RN 4-8 | 9 (7 completed) | 17.9 / 0.5 / 86.4 | 1.8s / 33.7s |

Across all 37 completed real graphs: depth min 1, median 2, max 7 —
**far shallower** than the synthetic families at comparable RN counts
(degree-3/mixed-degree reach depth 7-22 even at their smaller sizes). Real
graphs' domain sizes cluster near 16 (see
[llvm-corpus-characterization.md](llvm-corpus-characterization.md#5-distribution-of-n-nodes-and-d-domain-size)),
so each branch level is wide (up to 17-way) but shallow, the opposite shape
from the synthetic families' narrower (D=2-4), deeper trees.

At low RN, pruning is dramatically more effective on real graphs (median
82.4%) than on any synthetic family (~42-50%) — real unary/edge costs
(quantized spill weights and 0/∞ interference) apparently produce tighter,
more separated branch values than the synthetic generator's uniform random
costs, so the first branch explored is very often close to optimal. At
higher RN (4-8), that advantage disappears and becomes highly
graph-dependent: one RN=7 graph pruned 86.4% of 3,063 visited nodes in 2.1s,
another RN=7 graph did not finish 44,573+ visited nodes in 45s (pruning
only 234, i.e. 0.5%), and two more (RN=7, RN=8) did not finish at all
within the cap. **Pruning effectiveness on this corpus is not a simple
function of RN count** — it depends on how separated the actual cost
values are, which this small sample cannot predict from RN alone.

## 5. Is frontier width large enough to matter?

**Depth** is measured exactly (`search_maximum_depth`): small on the real
corpus (median 2, max 7 in this sample) and small-to-moderate on synthetic
families (2-22 depending on family/size). None of these are so deep that a
hardware-side stack or frontier-tracking structure would need to be large.

**True peak concurrent frontier width** (the prompt's "how many independent
bound computations could run in parallel") was **not measured exactly** in
this pass. `exact-branch-reduce` is strictly sequential DFS — only one path
is ever active — and the existing JSONL trace schema
(`pbqp_solver_event_t`) carries no depth or per-level domain field, so
reconstructing a true per-instant open-node count from the trace would
require extending that schema, which was treated as out of scope for a
data-gathering pass (a real, not cosmetic, instrumentation change,
deserving its own review). Instead, `branches/depth` — this solve's total
branch count divided by its max depth, i.e. an *average* branching factor —
is reported as a coarse proxy. On synthetic families this ranges from 3
(degree-4, matching its always-domain-≤4, depth-2 tree) up to several
thousand (mixed-degree at N=25); on the real corpus sample it ranges from
about 5 to several thousand as well (median 22).

Given the shallow depths measured directly, and that even the *proxy*
branching-factor stays in the tens-to-low-thousands range rather than
exploding, the honest answer is: **frontier width plausibly stays in the
tens-to-low-hundreds range for the real corpus and small synthetic points
tested, but this sample cannot rule out much wider frontiers on graphs with
higher RN and larger domain** — precisely the RN 4-8 real-graph subset in
§4 that already showed 44,000+ visited nodes and did not always finish.
Whether that translates into a *concurrently open* frontier worth
architecting for, versus just a long sequential tail, requires the
depth-tagged trace this pass did not build. **This question is left open**,
not answered small-or-large, and is the most concrete follow-up item this
report raises.

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
- Frontier width is a depth/branches-per-solve proxy, not a per-instant
  reconstruction — see §5.
- No change to reduction rules, RN scoring, or the accelerator ISA. The
  only solver-visible change is the new `search_nodes_pruned` statistic and
  the internal `PBQP_PRUNED` status, which never escapes
  `pbqp_solver_solve`.
