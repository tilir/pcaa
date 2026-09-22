# ISA-decision data-gathering: index

This is a pure index over the six work items in the ISA-decision
data-gathering pass (`prompt-gather.md`). It points at each item's data and
report and lists the open questions that item raised but did not answer.
It contains no synthesis or recommendation — the ISA decision itself is a
separate, later conversation, using this data as input.

## Item 1: real PBQP graph extraction from LLVM RegAllocPBQP

- **Data**: `examples/regalloc/*.pbqp` (491 graphs). Instrumentation:
  `llc -pcaa-pbqp-dump-dir=<dir>` in a local LLVM checkout (patch not part
  of this repo).
- **Report**: [doc/llvm-corpus-characterization.md](llvm-corpus-characterization.md)
  §1-7.
- **Open questions this item raised but did not answer**:
  - Only 4 compilation units (2 LLVM, 2 CUDD) were sampled; how much would
    N/D distribution shift with a broader, more diverse corpus (different
    optimization levels, different targets with different register-class
    sizes, C++ template-heavy vs plain-C code)?
  - The bimodal D~{7,16} domain-size clustering is specific to x86-64's
    register classes at `-O2`; would a target with far fewer registers (an
    embedded ISA) or far more (a large vector ISA) change the shape a
    synthetic proxy family would need?
  - No purpose-built "register-allocation-like" synthetic family was
    built to close the gap this item identified — only the gap itself was
    measured.

## Item 2: existing scripts against the real corpus

- **Data**: same `build/scaling-runs.csv`, rows tagged `sweeps=corpus`,
  `family=llvm-regalloc` (491 rows). Script:
  `scripts/scaling_characterize.rb --corpus-dir`.
- **Report**: [doc/llvm-corpus-characterization.md](llvm-corpus-characterization.md)
  §8.
- **Open questions**:
  - RN-policy comparison (max-degree/min-work) was not run against the
    real corpus, only min-degree — `--corpus-policy` exists on the script
    for a future run.
  - The fill-in result is a net active-edge-count comparison, not a
    per-event fill-edge counter (the shared solver does not expose one);
    whether individual fill edges are created and simply outweighed by
    removals, or never created at all, is unresolved.
  - Only graphs that solved under the sweep's default timeout are
    represented; whether the ~450 uncharacterized real graphs (this item
    covered all 491, but item 3's exact-search subset did not) follow the
    same RN-fragmentation pattern at larger N is untested.

## Item 3: branch-and-bound pruning

- **Data**: `/tmp/.../branch-bound-synthetic.csv` and
  `branch-bound-corpus*.csv` (not committed — regenerate via the commands
  in the report; the pruning statistic itself, `search_nodes_pruned`, is
  in every `pcaa_graph_run --verbose` exact-branch-reduce run and in
  `pbqp_statistics_t`). Code: `software/pbqp/pbqp.cpp`
  (`SolveBranchAndReduce`, `RemainingCoreLowerBound`), test:
  `software/tests/pbqp_rn.c`.
- **Report**: [doc/branch-bound-characterization.md](branch-bound-characterization.md).
- **Open questions**:
  - True peak concurrent frontier width was not measured exactly — only a
    branches/depth average-branching-factor proxy — because the trace
    schema carries no depth field; extending it was scoped out as a real
    instrumentation change deserving its own review (§5 of that report).
  - Pruning effectiveness on real graphs with RN 4-8 was highly
    graph-dependent (0.5%-86.4% pruned in a 7-graph sample) and not
    predictable from RN count alone; the underlying cause (cost-value
    separation) was not characterized further.
  - Only 39 of 491 real corpus graphs were exact-search-tractable within
    this pass's timeouts; the larger-RN majority's branch-and-bound shape
    is unknown.

## Item 4: local search at scale

- **Data**: `build/scaling-runs.csv`, rows tagged `sweeps=local-search`
  (270 rows, strategies `local-search`/`heuristic-rn-local-search`, N up
  to 100/50, D=2,4,8). Script: `scripts/scaling_characterize.rb --sweeps
  local-search`.
- **Report**: [doc/scaling-characterization.md](scaling-characterization.md#10-local-search-at-scale).
- **Open questions**:
  - N was capped well below the main reduction/RN sweep (100 vs 1000) because
    of local search's cost; whether the observed linear-in-N sweep-count
    trend for `local-search` alone (and flat trend for the hybrid) continues
    at larger N is untested.
  - Only min-degree RN seeding was used for `heuristic-rn-local-search`;
    whether a different RN policy's seed changes hybrid convergence speed
    is unmeasured.
  - LS-A/LS-B were measured via trace parsing at these N/D points but not
    cross-referenced against the real LLVM corpus (item 1/2) at all — local
    search on real graphs is entirely uncharacterized.

## Item 5: primitive-shape / descriptor-count study

- **Data**: reuses `build/scaling-runs.csv`'s existing operation-mix
  columns (`project_elements`, `projection_primitives`, `map3_elements`,
  etc.) from the graph-size/domain-size sweeps; no new runs. Code read:
  `software/pbqp/pbqp.cpp` (`ReduceRN`, `ReduceR2`, `RunLocalDescent`).
- **Report**: [doc/primitive-shape-study.md](primitive-shape-study.md).
- **Open questions**:
  - The D-ratio findings (D for PROJECT, D or D^2 for MAP3 depending on
    primitive generality) are descriptor-*count* projections only; whether
    a fused primitive is worth its added hardware complexity at any
    specific D is explicitly out of scope (no cycle/area estimate exists
    yet to weigh against it).
  - PROJECT_ACCUMULATE and SLICE_ACCUMULATE have zero current descriptors
    (pure host arithmetic); how much host CPU time that actually costs
    today, and whether it is a bottleneck worth moving to the device, was
    not measured (this item is data-shape only, not a host-timing study).

## Item 6: generality probe (Bellman-Ford)

- **Data**: `probes/src/bellman_ford.cpp`, tested by `probes_unit` (5
  GoogleTest cases, in the main CTest suite).
- **Report**: [doc/generality-probe.md](generality-probe.md).
- **Open questions**:
  - Only single-source Bellman-Ford was implemented. Viterbi/HMM decoding
    was named as structurally similar (irregular per-node fan-in, same
    opcode-3 shape) but not implemented or verified.
  - All-pairs shortest path was named as needing a structurally different,
    matrix-output-tier primitive, but not implemented — whether that
    tier is worth building is unaddressed.
  - This is one workload; whether opcode 3's fit generalizes across a
    wider set of non-PBQP min-plus problems, or whether Bellman-Ford
    happened to be an unusually good fit, is untested with only one probe.
