# Real LLVM RegAllocPBQP corpus characterization

This is a logical-workload study over PBQP graphs extracted from LLVM's own
`RegAllocPBQP` allocator, not a proposal to change PCAA's descriptor ABI,
PBQP solver semantics, or the accelerator instruction set. It answers
whether real register-allocation PBQP graphs look like the synthetic
`degree-3`/`degree-4`/`mixed-degree` families already characterized in
[scaling-characterization.md](scaling-characterization.md).

## 1. Checkout and reproduction

- LLVM checkout: `/home/tilir/llvm-project`, commit `9517d7182766f9d16dda05a97464660e27ce91f3`
  (`llvmorg-24-init`, 2026-09-22), built as `CMAKE_BUILD_TYPE=Release`,
  `LLVM_ENABLE_ASSERTIONS=OFF`, `LLVM_ENABLE_PROJECTS` empty (no clang
  configured in that tree — only core tools: `llc`, `opt`, etc.).
- IR generation used the system compiler, `Ubuntu clang version 21.1.8
  (6ubuntu1)`, *not* a compiler built from the checkout — `llc` (the tool
  that actually runs `RegAllocPBQP`) came from the checkout above; only .ll
  text needed to come from somewhere, and any reasonably current clang
  produces IR the checkout's `llc` accepts.
- pqbpaccel checkout: commit `e80c21cdf0794d0e0cbb38b6bd69dd7358573af6` plus
  the in-progress working-tree changes from this session (uncommitted at
  the time this report was written).
- Results are checkout-specific: a different LLVM revision, target, or
  optimization level would change the graphs (different spiller heuristics,
  register classes, or interference structure).

## 2. Instrumentation

Added `-pcaa-pbqp-dump-dir=<dir>` to
`/home/tilir/llvm-project/llvm/lib/CodeGen/RegAllocPBQP.cpp`: a self-contained,
always-compiled `cl::opt<std::string>` (unlike the existing `-pbqp-dump-graphs`
debug hook, which is `#ifndef NDEBUG`-gated and therefore compiled out of
this Release build). When set, it writes one `.pbqp` file per solved graph —
right after `initializeGraph`+constraints, immediately before
`PBQP::Solution Solution = PBQP::RegAlloc::solve(G)` — in pqbpaccel's
line-oriented text format. This is the entire diff: no other LLVM behavior
changes, and the flag defaults to off. It reuses `PBQPRAGraph::getNodeCosts`/
`getEdgeCosts` (the same accessors the existing `dump()` debug method uses)
and writes:

```text
nodes N
node D c0 c1 ... c(D-1)
edge i j <D_i*D_j costs, row-major>
```

### Quantization rule (LLVM's `PBQPNum` is `float`; pqbpaccel costs are `int32_t`)

Fixed-point, scale 1000 (three decimal digits of precision):
`cost = round(v * 1000)`, clamped to
`±pbqp_max_finite_cost(problem)` computed **the same way
`software/pbqp/pbqp.cpp:1414` does** — `(ACCEL_INF - 1) / (node_count +
edge_count)` for the graph being dumped, so every dumped graph is guaranteed
to pass pqbpaccel's own cost-range check. `+infinity` (LLVM's hard
interference sentinel, `RegAllocPBQP.cpp:411`) maps to the `INF` literal.
LLVM's zero-weight-spill sentinel (`numeric_limits<PBQPNum>::min()`, a
denormal ~1.18e-38) rounds to `0` under this scale — intentional, not a
truncation bug: it represents a free spill option. Over the full 491-graph
corpus collected for this report, **zero cost values required clamping**
(spill weights and CS-register nudges stayed well inside the safe range for
every graph observed) and **no negative cost values appeared** in the
allocator's own constraints, confirming the recon that produced this rule.

### Filename robustness

Heavily templated C++ produces mangled function names far past common
filesystem name limits. The dumper truncates a sanitized
`module.function.round` name to 160 bytes and appends an 8-byte FNV-1a hash
when truncation happens, rather than silently dropping the graph (an
earlier version of this patch used `ENAMETOOLONG` failures for ~13% of one
source file's graphs before this fix — see §4).

## 3. Corpus collection

Four real translation units, compiled to `.ll` with the include/define flags
LLVM's own build already uses for that file (pulled from
`build/compile_commands.json`, substituting `clang++ -S -emit-llvm -O2` for
the object-file flags — this keeps `-I` paths, standard version, and macros
identical to how the file is actually built, rather than guessing flags):

| File | Source | Lines | `.ll` size |
|---|---|---:|---:|
| `llvm/lib/Transforms/InstCombine/InstCombineAddSub.cpp` | LLVM itself | — | 2.3 MB |
| `llvm/lib/CodeGen/MachineBasicBlock.cpp` | LLVM itself | — | 0.9 MB |
| `~/cudd/cudd/cuddAddAbs.c` | CUDD (BDD library, unrelated C project on disk) | 543 | 45 KB |
| `~/cudd/cudd/cuddBddAbs.c` | CUDD | 739 | 55 KB |

Each `.ll` was run once through the patched
`llc -O2 -regalloc=pbqp -march=x86-64 -filetype=asm -pcaa-pbqp-dump-dir=<dir>
<file>.ll -o /dev/null` (`-O2` is required — `TargetPassConfig`'s
`getOptimizeRegAlloc()` only honors a non-default `-regalloc=` choice at
`-O1` and above). Every one of the 491 collected `.pbqp` graphs round-trips
through `pcaa_graph_run --solver local --strategy heuristic-rn` with **zero
parse failures and zero capacity-limit failures** (`local` mode's
allocator-backed solver sizes itself to the input, so the 65,536-choice
guard was never a concern for this corpus).

All 491 graphs are committed under `examples/regalloc/`.

| Source `.ll` | Graphs dumped |
|---|---:|
| `InstCombineAddSub.ll` | 315 |
| `MachineBasicBlock.ll` | 154 |
| `cuddAddAbs.ll` | 9 |
| `cuddBddAbs.ll` | 13 |
| **Total** | **491** |

## 4. Graphs per compilation unit

447 distinct `(module, function)` pairs produced 491 graphs: 44 functions
needed a second PBQP round (one extra spill-and-resolve pass;
`RegAllocPBQP`'s outer `while (!PBQPAllocComplete)` loop in
`runOnMachineFunction`), none needed a third. So for this corpus, **the
number of independent PBQP graphs per compiled function is 1, with a small
tail of 2** — most register-allocation decisions for a function converge in
a single PBQP solve. Per-file graph counts (§3 table) show one compilation
unit can contain anywhere from a handful (9, `cuddAddAbs.c`) to a few
hundred (315, `InstCombineAddSub.cpp`) independent graphs, driven entirely
by how many functions it defines and how many of those have any virtual
registers left to allocate after earlier passes.

(The 62/154 filename collisions from the initial run of the file-length
fix — see §2's "Filename robustness" — are graphs, not compilation units;
after the fix, 0 graphs were lost to filename errors across the full 491.)

## 5. Distribution of N (nodes) and D (domain size)

Over all 491 graphs:

| Metric | min | median | p90 | max | mean |
|---|---:|---:|---:|---:|---:|
| N (nodes) | 2 | 16 | 54 | 966 | 33.5 |
| edges | 0 | 42 | 268 | 6,018 | 150.1 |
| max degree | 0 | 13 | 46 | 778 | 27.6 |
| mean degree (`2E/N`) | 0.0 | 5.14 | 9.89 | 34.5 | 5.93 |

N by bucket: 54 graphs at N=1-5, 93 at N=6-10, 173 at N=11-20, 118 at
N=21-50, 27 at N=51-100, 26 at N>100 (max 966, from
`InstCombineAddSub.cpp`'s largest function). Most real PBQP graphs are
**small** (median N=16 — well below even the smallest synthetic sweep point,
N=20) but the tail is long: 26 graphs exceed the synthetic sweep's largest
tested point in at least one dimension of density.

**Domain size is not uniform within a graph** — 484/491 graphs (98.6%) have
`domain_min != domain_max` for at least one node, unlike every synthetic
family (`degree-3`/`degree-4` are exactly uniform by construction;
`mixed-degree`'s "small" profile draws each node's domain independently
from `{2,3,4}`, a small discrete *set* but still applied per-node
independently, not correlated the way real register classes are). Per-node
domain size, flattened across all 16,462 nodes in the corpus:

| D | 7 | 8 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| count | 2,773 | 2 | 51 | 126 | 219 | 643 | 870 | 1,660 | 9,568 | 550 |
| % | 16.8% | 0.0% | 0.3% | 0.8% | 1.3% | 3.9% | 5.3% | 10.1% | 58.1% | 3.3% |

This is **bimodal**: a large cluster at D=16 (58.1%, the x86-64 general
register class's allocatable-register-plus-spill count) and a second
cluster at D=7 (16.8%, a smaller register subclass — e.g. a byte/legacy
GPR class with fewer allocatable choices). The remaining 25% spread from
D=10 to D=17 are graphs mixing register classes or classes with reserved
registers removed. Per-graph `domain_max` is far more concentrated (median
16, p90 17, max 17 — see §-table above) since almost every graph contains
at least one full-width GPR-class node.

## 6. Is any synthetic family a structurally reasonable proxy?

**No single synthetic family matches.** Specifically:

- **Domain-size shape**: none of `degree-3`/`degree-4`/`mixed-degree` are
  non-uniform *within* a graph — the real corpus's defining feature (98.6%
  of graphs mix domain sizes) has no synthetic analogue at all. The
  scaling-characterization.md domain sweep (`D = 2, 4, 8, 16`, uniform per
  graph) never modeled a bimodal per-node mixture like D∈{7,16} dominating.
  If a future synthetic family is added for this purpose, it should draw
  each node's domain from a bimodal distribution concentrated near two
  register-class-like values, not a uniform range.
- **Graph size**: real N is dramatically smaller at the median (16) than
  every synthetic sweep point (which started at N=20) but has a long tail
  reaching N=966 — well past the synthetic sweep's N=500/1000 points for
  some individual functions. Neither a fixed small N nor the synthetic
  sweep's evenly-spaced N list reflects this skew; a log-normal-like N
  distribution would be closer.
- **Degree/density**: real mean degree (median 5.14, p90 9.89) sits between
  `degree-4`'s fixed 4 and `mixed-degree`'s N-dependent growth, but real
  max degree can reach 778 at N=966 (ratio ≈0.8), denser than
  `mixed-degree`'s ≈0.3·N edge-probability construction and far denser than
  the fixed-degree families. Degree also varies enormously *within* the
  corpus (many small, near-degree-0 graphs alongside a few very dense
  large ones) — no single fixed-degree or fixed-probability synthetic
  family produces that same spread.

The practical implication for future workload studies: `mixed-degree` is
the closer of the three existing families in spirit (non-fixed degree,
independent per-node domain draws), but it does not reproduce either the
real corpus's size skew or its bimodal domain-size clustering. A
purpose-built "register-allocation-like" synthetic family — small median N,
long tail, bimodal domain size concentrated near two class sizes — would be
a more faithful proxy than any of the three currently defined ones. This
report does not add that family; it only establishes that the gap exists
and roughly what shape would close it.

## 8. RN cascade, operation mix, and fill-in on the real corpus

`scripts/scaling_characterize.rb` gained a `--corpus-dir DIR` option: it
feeds every `*.pbqp` file in `DIR` straight to `pcaa_graph_run` (skipping
the generator entirely) as one more family, tagged `corpus`, reusing every
existing column and the same `run_point` path the synthetic sweep uses —
no third script, no C++ changes. `nodes`/`domain` stay `0` in these rows
(there is no swept target for a real file); the measured values are in
`initial_nodes`/`domain_min`/`domain_mean`/`domain_max` as for every other
row. Reproduce with:

```sh
ruby scripts/scaling_characterize.rb --sweeps none \
  --corpus-dir examples/regalloc --resume build/scaling-runs.csv
```

All 491 real graphs solved successfully (`status=ok`) under
`heuristic-rn`/`min-degree` in 31s total; results are appended to the same
`build/scaling-runs.csv` the synthetic sweep uses, tagged `sweeps=corpus`,
`family=llvm-regalloc`.

### RN-decision fragmentation: a third regime

| | min | median | p90 | max |
|---|---:|---:|---:|---:|
| RN decisions | 0 | 9 | 40 | 773 |
| RN decisions / N | 0.000 | 0.544 | 0.833 | 0.954 |

Unlike *any* synthetic family in
[scaling-characterization.md](scaling-characterization.md#2-headline-finding-graph-topology-decides-the-regime-not-just-n-or-d),
**45/491 real graphs (9.2%) need zero RN decisions** — they fully resolve
via R0/R1/R2 alone, with no branching at all. Neither `degree-3`/`degree-4`
(RN pinned low but never zero) nor `mixed-degree` (RN≈N) produced this
outcome anywhere in the synthetic sweep. For the remaining 446 graphs, the
median RN/N ratio (0.544) sits between `degree-4`'s near-zero ratio and
`mixed-degree`'s near-1.0 ratio — real register-allocation graphs are
fragmented, but less uniformly and less severely than the `mixed-degree`
proxy, and with a real "fully free" tail synthetic data never produced.

### Fill-in: shrinks, never grows, but through pre-RN reduction, not equality

Every synthetic point in scaling-characterization.md §9 showed
`initial_edges == max_rn_edges` because R0/R1/R2 never found anything to
reduce before hitting the irreducible RN core. The real corpus is
different: **372/491 graphs (76%) have R0/R1/R2 fire before RN**
(`initial_edges != max_rn_edges`) — real code contains far more locally
reducible structure than any of the three synthetic families modeled.
Critically, **all 372 of those shrink (max_rn_edges < initial_edges);
zero grow**. So while R2 elimination can in principle create a fill edge
between two previously non-adjacent neighbors, on this corpus the net
active-edge count never exceeds the initial count for a single graph —
whatever fill-in occurs is always outweighed by the edges R0/R1/R2 remove
outright. (The runner's current diagnostics report only before/at-RN active
edge *counts*, not a distinct "fill edges created" event counter, so this
is a net-count result, not proof that zero individual fill edges were ever
created — see Limitations.)

### Operation mix

Aggregated over all 491 graphs' element counts: **PROJECT 62.1%, PROJECT_ACC
6.3%, SLICE 4.4%, MAP3 27.3%, ARGMIN 0.0%.** This is the opposite balance
from the synthetic `degree-3`/`degree-4` sweep, where MAP3 (R2 elimination)
dominates at 84–99% and PROJECT is a small fraction — because real graphs'
RN cores are large relative to their R0/R1/R2-reduced remainder (matching
the RN/N ratios above), so RN projection work outweighs R2 elimination
work. This is closer in shape to `mixed-degree`'s D=2 domain-sweep point
(PROJECT 50%, see scaling-characterization.md §4) than to either
fixed-degree family, though real D clusters near 16 rather than 2. ARGMIN
is 0% for the same reason as the synthetic sweep: `heuristic-rn` alone
never invokes local search.

### Work per RN episode

| | min | median | p90 | max |
|---|---:|---:|---:|---:|
| elements / RN episode | 499 | 1,358 | 4,927 | 28,223 |

Comparable in magnitude to the synthetic sweep's mid-size points (e.g.
`degree-3` D=4 at N≥50, scaling-characterization.md §3) despite the real
corpus's much smaller median N — real graphs pack more logical work per
host decision than a same-size synthetic `degree-3`/`degree-4` point would,
consistent with their higher median degree (§5) and D≈16 domain size (§5).

### R2 reductions and cascades

R2 reductions/N: median 0.192, max 0.714. `cascade_max` (the longest
exact-reduction run following one RN pick) is 3 at the p90 and the overall
max across the whole corpus — capped, never runaway, matching the
synthetic families' small cascade lengths.

## 9. Limitations

- Four compilation units, two from LLVM's own sources and two from CUDD
  (chosen for being self-contained C files that compile standalone without
  a config/build system to stand in for "a project other than LLVM" — see
  the prompt's diversity requirement). This is not a claim of statistical
  representativeness across all C/C++ code; it is a real-vs-synthetic
  structural comparison on the graphs that were actually collected.
- `-O2`, `x86_64` only. Different optimization levels or targets (fewer
  registers, e.g. a target with 8 GPRs, or vector-heavy code exercising
  much larger register classes) would shift the domain-size distribution;
  this was not swept.
- The fill-in result in §8 is a net active-edge-count comparison
  (before vs. at-RN-entry), not a per-event fill-edge counter. The shared
  solver does not currently expose "fill edges created" vs "existing edges
  updated" as separate statistics (scaling-characterization.md §13's fuller
  ask); adding that counter was out of scope for this read-only
  data-gathering pass and would need its own review as a solver
  instrumentation change.
- RN-policy comparison (max-degree/min-work) was not run against the real
  corpus in this pass — `--corpus-policy` exists on the extended script for
  a future run, but only `min-degree` was measured here, matching the
  primary comparison basis used throughout scaling-characterization.md.
