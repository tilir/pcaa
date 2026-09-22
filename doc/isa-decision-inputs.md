# ISA-decision data-gathering: index

This is a pure index over the first six work items (`prompt-gather.md`) and
round 3's seven follow-ups (`prompt-arch.md`). It points at each item's data
and report and lists the open questions that item raised but did not answer.
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
  - Level (parallel) frontier width was not measured: the trace carries no
    per-event depth. Only depth and the exact depth-first open-node bound
    `1 + depth * (D_max - 1)` are reported; tree sizes (up to ~178k visited
    nodes) leave level width unbounded by the data.
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

- **Data**: `probes/src/bellman_ford.cpp`, tested by `probes_unit` (8
  GoogleTest cases, in the main CTest suite).
- **Report**: [doc/generality-probe.md](generality-probe.md).
- **Open questions**:
  - Only single-source Bellman-Ford was implemented. Viterbi/HMM decoding
    was named as structurally similar (irregular per-node fan-in, same
    opcode-3 shape) but not implemented or verified.
  - All-pairs shortest path was named as needing a structurally different,
    matrix-output-tier primitive, but not implemented — whether that
    tier is worth building is unaddressed.
  - Saturation at `INT32_MIN` makes negative-cycle detection impossible
    with PCAA arithmetic alone (the probe uses an exact 64-bit shadow);
    whether a future ISA should expose a wider cost type, an overflow
    flag, or rely on per-workload input-range contracts is unaddressed.
  - This is one workload; whether opcode 3's fit generalizes across a
  wider set of non-PBQP min-plus problems, or whether Bellman-Ford
  happened to be an unusually good fit, is untested with only one probe.

## Round 3, Item A: RN batch restructuring

- **Data**: `build/isa-round3-scaling.csv` (per-node),
  `build/isa-round3-per-edge.csv` (control), and the corresponding
  `build/vector-cycle-*.csv` timed subsets. Code: `ReduceRN` and
  `--rn-batching per-node|per-edge`.
- **Report**: [doc/batch-restructuring-study.md](batch-restructuring-study.md).
- **Open questions**:
  - The L1 model prices each removed top-level batch descriptor at five
    cycles but contains no host/device round-trip latency; measured hardware
    may value the 78-88% submission reduction differently.
  - Per-node batching increases peak solver/adapter workspace. The fixed RV64
    configuration is covered, but a future smaller embedded arena may prefer
    chunking rather than the legacy per-edge extreme.

## Round 3, Item B: vector-output cycle projection

- **Data**: `build/vector-cycle-projection.csv`, generated from 180 synthetic
  D=2..32 points and all 491 LLVM graphs by
  `scripts/vector_cycle_project.rb`.
- **Report**:
  [doc/vector-primitive-cycle-projection.md](vector-primitive-cycle-projection.md).
- **Open questions**:
  - The projection assumes perfect descriptor-local reuse and today's
    four-lane throughput; control, buffering, and stride-unit costs need an
    actual microarchitecture before the 2.5-4x model ratios can be validated.
  - Whether partial-D MAP3 is materially cheaper to build than full-D² MAP3
    is an area/routing question, not answered by service-cycle formulas.

## Round 3, Item C: fork-level parallelism

- **Data**: `build/fork-parallelism.csv`; schema/code:
  `pbqp_solver_event_t::branch_domain`, the JSON writer, and
  `scripts/fork_parallelism_characterize.rb`.
- **Report**:
  [doc/fork-parallelism-characterization.md](fork-parallelism-characterization.md).
- **Open questions**:
  - Branching factor times a per-graph Model-C epoch is a work proxy, not the
    true cost or overlap of exact child bounds.
  - Peak concurrent frontier width, snapshot bandwidth, and profitable worker
    count remain unknown because depth/parent tracking intentionally remains
    outside the trace.

## Round 3, Item D: fixed-point versus FP32 evidence

- **Data**: DIMACS NY graph/coordinate analysis via
  `scripts/cost_representation_analyze.rb`; published every-100,000th-arc
  samples in `probes_unit`; direct source audit of PBQP, accelerator, and probe
  arithmetic.
- **Report**: [doc/cost-representation-study.md](cost-representation-study.md).
- **Open questions**:
  - A scale/rebasing policy belongs to each future non-PBQP workload; the
    descriptor ABI has no representation metadata today.
  - Historical literature gives only a rough int32/FP32 cost ratio. PCAA's
    eventual target, exception policy, and pipeline need their own synthesis.
  - Variable-length PBQP folds remain in host software; their numerical order
    contract must be chosen before any are parallelized in hardware.

## Round 3, Item E: arithmetic and future tie semantics

- **Specification diff**: [doc/arch.md](arch.md) §4.1 names `INF` as the
  unconditional absorbing element (`INF + x = INF` for either sign), and §12
  requires every output of a future vector-reduction primitive to apply its
  own local first-index tie break.
- **Open questions**:
  - No vector-output opcode exists, so output layout and how local argmins are
    represented remain future descriptor-design work.

## Round 3, Item F: negative underflow is an error

- **Data/code**: `accelerator/src/cost_math.cpp` exposes the checked
  command-path addition used by `accelerator/src/accelerator.cpp` and the
  software primitive kernel in `software/pbqp/pbqp.cpp`; regression:
  `accelerator/tests/systemc_unit.cpp`.
- **Specification diff**: [doc/arch.md](arch.md) §4.1 and §9.
- **Resolution**: negative saturation was replaced by command `ERROR`.
  Keeping the clamp would merge distinct very-negative sums and create a
  false argmin tie. A global input-bound contract was rejected because the
  descriptor ABI permits results to be fed into later commands and has no
  graph-wide accumulation bound; reporting the underflow at the operation
  that observes it is smaller and unambiguous. Positive saturation remains
  `INF`, whose absorbing semantics make that collapse sound.
- **Open questions**:
  - The host-only Bellman-Ford probe deliberately retains a wider exact
    shadow for cycle detection; whether a future non-PBQP API should expose
    checked arithmetic directly rather than treat command failure as the
    only signal remains undecided.

## Round 3, Item G: contiguous and strided matrix views

- **Data**: `contiguous_views` and `strided_views` in verbose output and the
  existing scaling/RN CSVs; full results are in
  `build/isa-round3-scaling.csv`.
- **Report**:
  [doc/matrix-access-pattern-study.md](matrix-access-pattern-study.md).
- **Design note**: if a future matrix/vector-output primitive is introduced,
  give every operand an explicit stride (using the descriptor's reserved
  `m`/`k` extension route) rather than a row/column layout mode. Current
  storage has one row-major padded representation; rows and columns are the
  same view type with different stride values, not different layouts.
- **Open questions**:
  - Existing counters are global per solve and cannot attribute shape to
    RN/PROJECT versus R2/MAP3 without new instrumentation.
  - Frequency alone says nothing about stride-unit latency, packing reuse,
    banking, or memory-system throughput.
