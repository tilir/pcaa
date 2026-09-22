# Scaling characterization

This is a logical-work study, not a hardware speedup claim. It measures how
much cost-algebra work a PBQP instance actually contains as graph size `N`
and domain size `D` grow, and whether that work grows faster or slower than
the number of mandatory host (RN) decisions. All numbers come from real
solver executions through the shared allocator-backed PBQP solver; nothing
here is a topology-only estimate.

## 1. Experiment design

Reproduce the raw sweep with:

```sh
cmake --build build --target scaling_characterization
```

which runs `scripts/scaling_characterize.rb --resume build/scaling-runs.csv`
against the public `pbqp_graph_generate`/`pcaa_graph_run` CLIs. The script
takes `--runner`/`--generator` paths, `--help` documents every flag; the
notable ones added for this milestone are:

* `--sweeps LIST` — run any subset of `graph-size`, `domain-size`, `grid`,
  `policy`.
* `--nodes-for FAMILY=LIST` / `--domain-nodes-for FAMILY=LIST` — per-family
  node-count overrides, repeatable.
* `--graph-size-domains`, `--domain-size-domains`, `--policies`, `--seeds`,
  `--grid-seeds` — override the fixed domain/policy/seed lists.
* `--timeout SECONDS` (default 180) — kill a single run past this wall
  clock and record it as `time-limit` rather than block the whole sweep.
* `--resume` — skip `(family,nodes,domain,policy,strategy,seed)` points
  already present in the output CSV, so an interrupted sweep can continue.
* `--dry-run` — print the deduplicated job count per sweep without running
  anything.

The script builds one deduplicated job list across all four sweeps (the
same `(family,N,D,policy,seed)` point is solved once even if several
sweeps ask for it) and tags each row with which sweep(s) it belongs to.
Every job runs `pcaa_graph_run --solver local --strategy heuristic-rn
--verbose` and parses initial graph state from the generator's text output
plus final counters from the runner's diagnostics (`R0/R1/R2/RN`, RN core
size before/after cascades, RN cascade statistics, and the generic
operation mix). `build/scaling-runs.csv` retains every per-run row; the
tables below aggregate its 5 seeds (3 for the 2D grid) per point.

### Why the sweep is family-specific, and why mixed-degree stops at N=500

The original sweep used one fixed node list for every family. mixed-degree
graphs connect each node pair independently with probability 0.3, so their
edge count grows like `O(N^2)`, and a single N=1000/D=8 run measured about
19.6s at N=500 but did not finish in over 4 minutes at N=1000 before being
killed for this report. degree-3/degree-4 are circulant graphs with `O(N)`
edges and solved N=1000 in 0.2-1.0s. `DEFAULT_GRAPH_SIZE_NODES` in the
script reflects this directly: degree-3/degree-4 sweep `N = 20, 50, 100,
200, 500, 1000`; mixed-degree stops at `N = 20, 50, 100, 200, 500`. Every
point actually attempted in this run **succeeded** (`status=ok`, 367/367);
none hit the `--timeout 150` cap used for the production sweep, so no
scaling point is recorded as capacity- or time-limited. `--nodes-for`
exists precisely so a future run can push mixed-degree further, or pull
degree-3/degree-4 back, without editing the script.

### Sweeps run

| Sweep | Families | N | D | Policy | Seeds | Points |
|---|---|---|---|---|---|---:|
| graph-size | all three | family-specific (above) | 2, 4, 8 | min-degree | 1001-1005 | 255 |
| domain-size | all three | 20, 100 (+500 for degree-3/degree-4) | 2, 4, 8, 16 | min-degree | 1001-1005 | 160 |
| grid | degree-3, degree-4 | 20, 50, 100, 200 | 2, 4, 8, 16 | min-degree | 1001-1003 | 96 |
| policy | all three | (100,4) and (200,8) per family | — | min-degree, max-degree, min-work | 1001-1005 | 90 |

367 unique jobs after deduplication; total solver wall time 356s.

## 2. Headline finding: graph topology decides the regime, not just N or D

The three families are not interchangeable "random graphs at different
density" — they have structurally different elimination behavior, and that
difference dominates every other trend in this report.

* **degree-4** connects `(i, i+1)` and `(i, i+2)` around a ring — a
  bandwidth-2 circulant. R1/R2 can eliminate almost the whole graph locally
  along the ring; the min-degree/min-work heuristic needs **exactly 2 RN
  decisions regardless of N** (measured identically at N=20 through
  N=1000). This is Regime 2 from the design prompt: total work grows with
  N (linearly: 8,816 to 510,576 elements at D=8) while host interactions
  stay flat, so work per RN episode grows from 4,408 to 255,288 elements.
* **degree-3** connects `(i, i+1)` and `(i, i+N/2)` — the second edge
  jumps across the whole ring, breaking the local elimination order.
  RN decisions grow linearly, close to `RN ≈ N/4`, at every measured point
  (5, 12, 25, 50, 125, 250 for N=20..1000). This is Regime 1: total work
  and host interactions grow together, so elements/episode stays flat
  (~1,738-1,781 at D=8 across the whole N range).
* **mixed-degree** (independent 0.3 edge probability) has `O(N^2)` edges,
  so R0/R1 barely reduce anything before RN takes over: RN decisions grow
  almost 1:1 with N (15, 47, 97, 197, 497), while total work grows
  quadratically (edges ~`0.3*N^2`). This is fragmented Regime 1 with much
  larger absolute work per node than the two structured families.

The same generator, the same solver, and the same min-degree policy
produce genuinely different regimes purely from topology. This is the
central result the graph-size sweep was designed to surface.

## 3. Graph-size scaling (fixed D)

All values are 5-seed means; degree-3/degree-4 D=8 is representative
(remaining D values follow the same pattern, see `scaling-runs.csv`).

### degree-3, D=8

| N | total elements | PROJECT | MAP3 | bytes | RN decisions | Model C epochs | mean elements/epoch | init/max active edges |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 20 | 7,920 | 1,024 | 6,656 | 98,144 | 5 | 5 | 1,584 | 30 / 30 |
| 50 | 21,376 | 2,368 | 18,432 | 266,688 | 12 | 12 | 1,781 | 75 / 75 |
| 100 | 43,440 | 4,864 | 37,376 | 541,664 | 25 | 25 | 1,738 | 150 / 150 |
| 200 | 87,840 | 9,664 | 75,776 | 1,096,064 | 50 | 50 | 1,757 | 300 / 300 |
| 500 | 221,040 | 24,064 | 190,976 | 2,759,264 | 125 | 125 | 1,768 | 750 / 750 |
| 1000 | 443,040 | 48,064 | 382,976 | 5,531,264 | 250 | 250 | 1,772 | 1,500 / 1,500 |

Local empirical exponent (`α = log(W2/W1)/log(N2/N1)`) for total elements:
1.08 (20→50), 1.02 (50→100), 1.02 (100→200), 1.01 (200→500), 1.00 (500→1000)
— converging to linear, as expected once the `N/4` RN-decision count
dominates the fixed per-node reduction cost.

### degree-4, D=8

| N | total elements | PROJECT | MAP3 | bytes | RN decisions | Model C epochs | mean elements/epoch | init/max active edges |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 20 | 8,816 | 512 | 8,192 | 112,224 | 2 | 2 | 4,408 | 40 / 40 |
| 50 | 24,176 | 512 | 23,552 | 311,904 | 2 | 2 | 12,088 | 100 / 100 |
| 100 | 49,776 | 512 | 49,152 | 644,704 | 2 | 2 | 24,888 | 200 / 200 |
| 200 | 100,976 | 512 | 100,352 | 1,310,304 | 2 | 2 | 50,488 | 400 / 400 |
| 500 | 254,576 | 512 | 253,952 | 3,307,104 | 2 | 2 | 127,288 | 1,000 / 1,000 |
| 1000 | 510,576 | 512 | 509,952 | 6,635,104 | 2 | 2 | 255,288 | 2,000 / 2,000 |

PROJECT elements are pinned at 512 (all RN projection work happens at
exactly the same two decisions); MAP3 elements carry essentially all the
growth (α ≈ 1.0 throughout, i.e. almost exactly linear in N). This is the
cleanest Regime 2 case in the corpus: work per Model C epoch grows without
bound while host interactions never change.

### mixed-degree, D=8

| N | total elements | PROJECT | MAP3 | bytes | RN decisions | Model C epochs | mean elements/epoch | init/max active edges |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 20 | 5,674 | 3,405 | 1,434 | 57,632 | 15 | 15.6 | 367 | 58 / 56 |
| 50 | 30,320 | 23,859 | 512 | 280,877 | 47 | 47 | 645 | 375 / 375 |
| 100 | 120,752 | 96,205 | 512 | 1,112,851 | 97 | 97 | 1,245 | 1,505 / 1,505 |
| 200 | 480,272 | 383,821 | 512 | 4,420,435 | 197 | 197 | 2,438 | 5,999 / 5,999 |
| 500 | 2,995,552 | 2,396,045 | 512 | 27,561,011 | 497 | 497 | 6,027 | 37,440 / 37,440 |

Local exponent for total elements: 1.83 (20→50), 1.99 (50→100), 1.99
(100→200), 2.00 (200→500) — essentially quadratic once past the smallest
size, consistent with `O(N^2)` edges. RN decisions still grow only
linearly (`≈N`), so mean elements/episode also grows roughly linearly
(378→6,027 over a 25x range in N), which is still favorable for
amortization even though this family never reaches degree-4's constant-RN
regime.

"max active edges" never exceeds the initial edge count at any measured
point (see §9): under min-degree/min-work RN policy, no run in this sweep
grows the active edge count above its initial value. At mixed-degree N=20,
exact reductions before the first RN shrink the mean from 58 to 56.

## 4. Domain-size scaling (fixed N=100)

| D | degree-3 total el. | degree-3 MAP3 % | degree-4 total el. | degree-4 MAP3 % | mixed-degree total el. | mixed-degree PROJECT % |
|---:|---:|---:|---:|---:|---:|---:|
| 2 | 1,188 | 49.2% | 828 | 92.7% | 12,030 | 50.0% |
| 4 | 6,488 | 72.0% | 6,328 | 97.1% | 36,133 | 66.6% |
| 8 | 43,440 | 86.0% | 49,776 | 98.7% | 120,752 | 79.7% |
| 16 | 320,864 | 93.2% | 395,488 | 99.4% | 436,986 | 88.1% |

RN decisions and Model C epochs are **unaffected by D** at fixed N/family
(25 for degree-3, 2 for degree-4, 97 for mixed-degree, all constant across
D=2..16): domain size changes arithmetic intensity inside existing graph
decisions, not the reduction/branching structure itself, confirming the
prompt's Regime-3 hypothesis for the two structured families. mixed-degree
also holds RN/episode counts constant across D, so its total-work growth
with D is entirely an arithmetic-intensity effect too.

Local exponent (`α` vs D, N=100): degree-3 rises from 2.45 (2→4) to 2.88
(8→16); degree-4 from 2.93 to 2.99 (essentially the α≈3 predicted by the
prompt's own R2/MAP3 note — pairwise elimination work scales with a
product of domain sizes); mixed-degree is lower, 1.59 to 1.86, because a
much larger share of its work is already PROJECT/RN work at small D and
that scales closer to `D^2`. MAP3 dominates increasingly for degree-3/
degree-4 (49%→93% and 93%→99% of total elements as D grows 2→16); for
mixed-degree the dominant kernel is PROJECT (50%→88%), not MAP3, because
its irreducible RN core is nearly the whole graph and most work is RN
projection rather than R2 elimination.

## 5. Joint N x D grid (degree-3, degree-4; 3 seeds)

Total logical elements:

| degree-3 N\D | 2 | 4 | 8 | 16 |
|---:|---:|---:|---:|---:|
| 20 | 228 | 1,208 | 7,920 | 57,824 |
| 50 | 580 | 3,184 | 21,376 | 158,080 |
| 100 | 1,188 | 6,488 | 43,440 | 320,864 |
| 200 | 2,388 | 13,088 | 87,840 | 649,664 |

| degree-4 N\D | 2 | 4 | 8 | 16 |
|---:|---:|---:|---:|---:|
| 20 | 188 | 1,208 | 8,816 | 67,808 |
| 50 | 428 | 3,128 | 24,176 | 190,688 |
| 100 | 828 | 6,328 | 49,776 | 395,488 |
| 200 | 1,628 | 12,728 | 100,976 | 805,088 |

Mean elements per Model C epoch:

| degree-3 N\D | 2 | 4 | 8 | 16 |
|---:|---:|---:|---:|---:|
| 20 | 46 | 242 | 1,584 | 11,565 |
| 50 | 48 | 265 | 1,781 | 13,173 |
| 100 | 48 | 260 | 1,738 | 12,835 |
| 200 | 48 | 262 | 1,757 | 12,993 |

| degree-4 N\D | 2 | 4 | 8 | 16 |
|---:|---:|---:|---:|---:|
| 20 | 94 | 604 | 4,408 | 33,904 |
| 50 | 214 | 1,564 | 12,088 | 95,344 |
| 100 | 414 | 3,164 | 24,888 | 197,744 |
| 200 | 814 | 6,364 | 50,488 | 402,544 |

Both dimensions compound independently and multiplicatively: for
degree-3, growing N 20→200 is a 10x total-work increase at fixed D, and
growing D 2→16 is up to a 253x increase at fixed N; the N and D effects
combine as expected (649,664/228 ≈ 2,850x, close to 10 * 253 ≈ 2,530x, the
gap coming from the N-driven change in RN-decision count that D alone does
not affect). For degree-4, work/epoch grows in both directions
independently since RN decisions never move off 2; this is the strongest
observed case of favorable amortization from growing either N or D.

## 6. Operation-mix scaling

Fraction of total logical elements at D=8, graph-size sweep:

| N | degree-3 MAP3% | degree-4 MAP3% | mixed-degree PROJECT% |
|---:|---:|---:|---:|
| 20 | 84.0% | 92.9% | 60.0% |
| 50 | 86.2% | 97.4% | 78.7% |
| 100 | 86.0% | 98.8% | 79.7% |
| 200 | 86.3% | 99.4% | 79.9% |
| 500 | 86.4% | 99.8% | 80.0% |

Increasing N changes only the *quantity* of work for degree-3/mixed-degree
(their kernel-mix percentages are essentially flat past N=50), but for
degree-4 the MAP3 share keeps climbing toward 100% as N grows, because its
2 RN decisions contribute a fixed, N-independent amount of PROJECT work
while every additional node contributes one more MAP3-bearing R2
elimination. ARGMIN elements are 0 throughout this sweep because ARGMIN
only fires under local search, which `heuristic-rn` does not use.

## 7. Host-interaction / HW-SW epoch scaling

Model C has one epoch per RN pick and one additional epoch when exact
reductions run before the first RN (or when an RN-free solve consists only
of exact reductions). The scaling script derives that initial epoch as
`R0+R1+R2 - (R0+R1+R2 after RN)`, matching the trace grouping in the
HW/SW-boundary study. Elements/epoch below is the 5-seed mean of each run's
own `total_elements / model_c_epochs`:

| Family (D=8) | N=20 el./episode | N=1000 (or N=500) el./episode | growth |
|---|---:|---:|---:|
| degree-3 | 1,584 | 1,772 | 1.1x |
| degree-4 | 4,408 | 255,288 | 58x |
| mixed-degree | 367 | 6,027 (N=500) | 16x |

This is the key accelerator-granularity metric from the prompt: degree-4
shows the strongest granularity growth (Regime 2), mixed-degree grows
substantially too despite its RN decisions scaling with N (because total
work grows faster still, quadratically), and degree-3 stays essentially
flat because both work and RN decisions grow at the same linear rate.

**Limitation**: this table reports the mean of per-run elements/episode
values, not a true within-solve percentile distribution across epochs
(p10/median/p90 as in the existing HW/SW-boundary study). Getting real
per-epoch percentiles requires `--trace` JSONL parsing as
`scripts/hw_sw_characterize.rb` already does, but only at a fixed N=20
corpus; extending that event-level tracing across this full N/D grid was
out of scope for this pass (mixed-degree N=500 already produces very large
traces) and is left for a follow-up. The median/p10/p90 columns in
`scaling-runs.csv` are seed-level statistics of the per-run mean, not
epoch-level statistics, and should not be read as the latter; within a
fixed `(family,N,D)` point they are nearly identical to the mean because
seed-to-seed variance is small (e.g. degree-3 N=1000/D=8: mean 1,772,
median 1,772, p90 1,772; mixed-degree N=20/D=16, the most variable point
measured: mean 1,826, median 1,680, p90 2,636).

## 8. Policy comparison (min-degree, max-degree, min-work)

| Family, point | policy | RN decisions | Model C epochs | total elements | mean el./epoch | cascade mean/max |
|---|---|---:|---:|---:|---:|---:|
| degree-3, N=100/D=4 | min-degree | 25 | 25 | 6,488 | 260 | 3.000 / 3 |
| | max-degree | 25 | 25 | 6,488 | 260 | 3.000 / 3 |
| | min-work | 25 | 25 | 6,488 | 260 | 3.000 / 3 |
| degree-4, N=100/D=4 | min-degree | 2 | 2 | 6,328 | 3,164 | 49.000 / 98 |
| | max-degree | 33 | 33 | 7,344 | 223 | 2.030 / 5 |
| | min-work | 2 | 2 | 6,328 | 3,164 | 49.000 / 98 |
| degree-4, N=200/D=8 | min-degree | 2 | 2 | 100,976 | 50,488 | 99.000 / 198 |
| | max-degree | 67 | 67 | 88,496 | 1,321 | 1.985 / 3 |
| | min-work | 2 | 2 | 100,976 | 50,488 | 99.000 / 198 |
| mixed-degree, N=200/D=8 | min-degree | 197 | 197 | 480,272 | 2,438 | 0.015 / 3 |
| | max-degree | 173 | 173 | 490,048 | 2,826 | 0.153 / 7 |
| | min-work | 197 | 197 | 480,272 | 2,438 | 0.015 / 3 |

Two findings stand out:

* **min-degree and min-work are indistinguishable on every measured
  point** in this corpus (identical RN counts, epoch counts, elements, and
  cascade statistics). This is a descriptive observation about these
  synthetic families, not a claim that the two policies are the same in
  general; the solver-characterization study's 20-node corpus with
  different graph families found real differences elsewhere, so this may
  be specific to circulant/dense-random structure. Left as-is per the
  "do not change RN scoring" constraint — it's reported, not adjusted.
* **degree-4's favorable Regime-2 property is policy-dependent, not just
  topology-dependent.** min-degree/min-work keep exactly 2 RN decisions
  regardless of N (§2-3). max-degree breaks that: RN decisions grow with N
  (33 at N=100, 67 at N=200, roughly N/3), turning degree-4 back into a
  fragmented Regime-1 case with a much lower elements/epoch (1,321 vs
  50,488 at N=200/D=8) despite almost the same total work. A policy that
  produces excellent granularity at one N is not guaranteed to keep that
  property as N grows, and here the mechanism is visible directly: the
  policy choice determines whether the circulant's local elimination order
  is preserved.

For mixed-degree, all three policies stay in the same fragmented regime;
max-degree needs slightly fewer, larger-cascade decisions (173 vs 197) but
does not change the qualitative picture.

## 9. Graph-state and fill-in scaling

Every `(family, N, D)` point measured under min-degree/min-work in this
sweep shows **maximum active edges observed at RN entry <= initial active
edges** (see the graph-size tables in §3). The counts are equal except at
the smallest mixed-degree point, where initial exact reductions remove
edges before the first RN. No point grows past its initial edge count, all
the way to N=1000/mixed-degree N=500. This does not say that individual R2
steps create no fill edges; it says any such fill never causes net
active-edge expansion beyond the input graph. This is
consistent with the two structured families' bandwidth-bounded and
dense-random-adjacent-hop topologies respectively: R1/R2 eliminate nodes
whose neighbors are already adjacent (degree-3/degree-4), or R0/R1 barely
fire at all before RN dominates (mixed-degree), so the classic R2
fill-edge case (connecting two previously non-adjacent neighbors) does not
occur on this corpus. This is a property of these three generator
families and the min-degree/min-work policies, not a general solver
property — the shared reduction/RN implementation is unchanged and can
still produce fill-in on other graphs (e.g. `random-sparse`, not part of
this sweep).

Cost-table bytes scale exactly with `unary_elements + matrix_elements`
(4 bytes/element, `int32_t`, matching `pbqp.h`): from 640B at
degree-3/N=20/D=2 to 544KB at degree-4/N=1000/D=8, and 9.6MB at
mixed-degree/N=500/D=8 — driven by mixed-degree's `O(N^2)` edge count, not
by any fill-in. Graph-aware Models C/D would need to hold state
proportional to this initial cost table on this corpus, not a larger
elimination-driven working set. Separately, total *logical operation
traffic* (operand+result bytes moved across the whole solve, which
re-reads/re-writes cost-table entries many times during RN projection and
R2 elimination) is much larger than the static cost table — 6.6MB for
degree-4/N=1000/D=8 and 27.6MB for mixed-degree/N=500/D=8 — because it
counts repeated accesses to the same state, not additional state.

## 10. Local search at scale

`scripts/scaling_characterize.rb --sweeps local-search` extends the same
job/dedup/timeout machinery to `local-search` and
`heuristic-rn-local-search`, at a smaller, separately calibrated N range
per family (local search evaluates every node's every candidate value each
sweep, so it does not scale like the reduction/RN sweeps above): N up to
100 for degree-3/degree-4, up to 50 for mixed-degree, at D=2,4,8, 5 seeds.
270 points, all completed in 121s total. For local-search-strategy runs
the script also requests a `--trace` JSONL and groups it into LS-A
(node-score granularity: one epoch per node evaluation) and LS-B (sweep
granularity: one epoch per full pass over all nodes) exactly as
`hw-sw-boundary-characterization.md` defines them, reporting epoch-size
percentiles the same way the N=20 corpus there does — this is the
extension across N/D that report's "Local search and exact search" section
left as future work.

### Sweep count: the hybrid converges in a handful of sweeps; local search alone does not

| Family (D=4) | N | local-search sweeps | hybrid sweeps | local-search evaluations | hybrid evaluations |
|---|---:|---:|---:|---:|---:|
| degree-3 | 20 | 128.2 | 1.2 | 2,564 | 24 |
| degree-3 | 50 | 313.6 | 1.6 | 15,680 | 80 |
| degree-3 | 100 | 627.6 | 2.4 | 62,760 | 240 |
| degree-4 | 20 | 129.6 | 1.4 | 2,592 | 28 |
| degree-4 | 50 | 325.6 | 1.2 | 16,280 | 60 |
| degree-4 | 100 | 639.6 | 1.0 | 63,960 | 100 |
| mixed-degree | 15 | 93.4 | 1.4 | 1,401 | 21 |
| mixed-degree | 20 | 128.8 | 2.0 | 2,576 | 40 |
| mixed-degree | 50 | 320.6 | 3.4 | 16,030 | 170 |

Seeding local descent from the RN heuristic's result (`heuristic-rn-local-search`)
cuts sweep count by roughly 50-300x versus starting from all zeros across
every family and N tested here, and the gap widens as N grows (degree-3:
128 -> 1.2 sweeps at N=20 is 107x; 628 -> 2.4 at N=100 is 261x). Pure
`local-search` sweep count also grows essentially linearly in N at fixed D
(degree-3: 128, 314, 628 at N=20/50/100 — ratios 6.4, 6.28, 6.28 sweeps per
node, stable), while the hybrid's sweep count stays small and roughly flat
(1-3.4 sweeps) across the whole tested N range: RN gives local descent a
starting point close enough to a local optimum that only a few full passes
are needed regardless of graph size, on this corpus.

### LS-A/LS-B epoch size: LS-A tracks D only; LS-B tracks N x D

| Family | N | D | LS-A median elements | LS-B median elements |
|---|---:|---:|---:|---:|
| degree-3 | 50 | 2 | 8.0 | 400.0 |
| degree-3 | 50 | 4 | 16.0 | 800.0 |
| degree-3 | 50 | 8 | 32.0 | 1,600.0 |
| degree-4 | 50 | 2 | 10.0 | 500.0 |
| degree-4 | 50 | 4 | 20.0 | 1,000.0 |
| degree-4 | 50 | 8 | 40.0 | 2,000.0 |
| mixed-degree | 50 | 2 | 31.8 | 1,599.2 |
| mixed-degree | 50 | 4 | 63.6 | 3,198.4 |
| mixed-degree | 50 | 8 | 127.2 | 6,396.8 |

For degree-3/degree-4, LS-A's median elements/node-evaluation is
**independent of N** (16.0 at D=4 for both N=20, N=50, and N=100 —
degree-3's fixed degree-3 neighborhood makes each node-score evaluation a
constant-size operation regardless of graph size) and scales linearly with
D alone. LS-B is exactly `LS-A median x N` in every row checked (e.g.
degree-3 N=50/D=4: 16.0 x 50 = 800.0, matching exactly, since one sweep
touches every node once) — so LS-B inherits both the D-driven per-node
growth and an additional N-driven factor from sweeping the whole graph.
This is the same node-granularity-vs-sweep-granularity split
`hw-sw-boundary-characterization.md` found at N=20, now confirmed to hold
up to N=100: LS-B is the only one of the two that grows with graph size at
all.

mixed-degree does **not** hold LS-A constant across N (20.8, 26.0, 63.6
elements at N=15, 20, 50 and D=4 — not shown in the D-only table above but
present in the raw CSV `sweeps=local-search` rows) because each node's
degree itself grows with N for this family (scaling-characterization.md
§3), so a node-score evaluation's slice-accumulate cost grows too — the
same topology distinction that separates mixed-degree from the two
fixed-degree families throughout this report shows up again here.

### Limitations specific to this section

- N is capped well below the main reduction/RN sweep's range specifically
  because local search's cost (evaluations = sweeps x N, each touching a
  node's full degree x D slice) grows much faster with N than RN's does;
  this is expected, not a defect, and mirrors the calibrate-first approach
  used for mixed-degree in the main sweep.
- `RN+local-search` is reported only as the whole-run sweep/evaluation
  counts above; its RN-seed phase's own epoch shape is unchanged from the
  `heuristic-rn` sections earlier in this report (the hybrid's RN phase is
  identical to a plain `heuristic-rn` run, followed by local descent from
  that seed).

## 11. Architectural interpretation

* **Does work per Model C epoch stay constant as N grows, or increase?**
  Depends entirely on topology and policy: constant for degree-3 (both
  work and RN decisions scale together), strongly increasing for degree-4
  under min-degree/min-work (RN decisions pinned at 2), and increasing but
  less dramatically for mixed-degree (work grows quadratically, RN
  decisions only linearly).
* **Does domain size strongly increase work per software decision?** Yes,
  for all three families, and RN/episode counts are unaffected by D, so
  the entire D-driven growth lands inside existing epochs (§4).
* **Which operation grows fastest with D?** MAP3 for degree-3/degree-4
  (local exponent approaching 3, matching the R2 three-domain product
  cost); PROJECT for mixed-degree, because its RN core is nearly the whole
  graph.
* **Does R2/MAP3 become increasingly dominant for larger domains?** Yes
  for degree-3/degree-4 (49%→93% and 93%→99% of total elements, D=2→16 at
  N=100). Not for mixed-degree, where PROJECT dominates throughout and
  grows its share with D (50%→88%).
* **Does graph fill-in materially change scaling with N?** No fill-in was
  observed at all on this corpus (§9); scaling is driven entirely by
  initial graph size and elimination-order structure, not by fill-in
  growth.
* **Do RN policies retain their differences at larger N?** No for
  degree-4: max-degree's fragmentation advantage/disadvantage relative to
  min-degree changes qualitatively with N because min-degree's RN count
  stays flat while max-degree's grows with N (§8). min-degree and min-work
  are identical at every N tested here.
* **At what measured ranges does Model C materially improve granularity
  over Model B?** This report does not re-measure Model B directly (it
  derives Model C boundaries from `rn_episodes` plus an initial exact
  epoch); the
  existing N=20 HW/SW-boundary study already shows Model C reducing
  handoffs 20→5 (degree-3) and 20→2 (degree-4) relative to Model B at that
  size. Given degree-4's RN count stays at 2 through N=1000, that
  advantage over Model B's presumably N-scaling reduction count should
  only widen with N; confirming this quantitatively for Model B at scale
  is future work.
* **How much additional granularity does Model D provide beyond Model
  C?** Not measured in this pass (Model D was only characterized at
  N=20 previously); left as future work alongside the local-search
  extension.

## 12. Limitations

* This remains a logical-work study: no CPU/accelerator cycles, MMIO/DMA
  latency, clock frequency, speedup, throughput, energy, or area claims
  are made or implied anywhere in this report.
* Elements/epoch percentiles in §3/§7 are seed-level statistics of a
  per-run mean, not true within-solve epoch percentiles; only the existing
  N=20 HW/SW-boundary corpus has genuine epoch-level percentile data.
* Local search (§10) and a scaled-up Model B/D comparison are not covered;
  they are flagged as follow-up work rather than silently omitted.
* All 367 points in this production run succeeded; the script's
  `time-limit`/`capacity-limit` classification exists and was validated
  (a synthetic N=100/D=1000 point correctly reports `capacity-limit`, and
  a synthetic 3-second timeout on mixed-degree N=500/D=8 correctly reports
  `time-limit` with initial graph-state fields preserved), but neither
  condition was hit by any point in the actual sweep, so this corpus does
  not yet demonstrate a censored/limited case in practice.
* `REDUCE_ONLY` was not included as a baseline strategy in this sweep
  (optional per the design prompt); only `heuristic-rn` was measured.
* No new ISA command, XLS/RTL, or accelerator timing model was introduced.
  The generic scaling summary schema (`family/profile/seed/nodes/domain`
  alongside solver-derived counters) is unchanged in spirit from before
  and remains reusable for a later LLVM-PBQP corpus that lacks synthetic
  `(N,D,family)` parameters: `initial_nodes/initial_edges/domain_min/
  domain_mean/domain_max/unary_elements/matrix_elements` are already
  generic fields independent of how the graph was produced.
