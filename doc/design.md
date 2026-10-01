# Model and workload design

The block-visible command and memory contract is specified in [arch.md](arch.md).
This note describes implementation refinement and workload methodology;
service-cycle formulas belong in [l1-performance-model.md](l1-performance-model.md).

## Model refinement route

| Level | Model | Status and purpose |
| --- | --- | --- |
| L0 | Functional SystemC/TLM | Implemented; commands complete during doorbell processing, with no meaningful time advance |
| L1 | Loosely timed TLM | Implemented as an optional service estimate; descriptor, operand, compute, and result costs are separate |
| L2 | Pipelined / approximately timed | Future refinement for datapath scheduling and modeled memory concurrency |
| L3 | Mixed TLM and Verilated RTL | Future replacement of selected datapath blocks with RTL |

Each refinement preserves ISA arithmetic, first-index argmin, memory layouts,
ordered batch visibility, status/error semantics, and guest physical addressing.
Lanes affect the L1 estimate, not command dimensions. Submit and wait stay
separate even though L0 completes synchronously. No L2/L3 implementation is
implied by this route.

The generic accelerator annotates TLM delay without running a simulation loop.
Spike glue owns advancement of nonzero annotated delay. Host SystemC entry
points initialize the kernel with `sc_start(SC_ZERO_TIME)`; the current timed
runner reports service estimates rather than simulating CPU/device overlap.
The driver and semantic builders do not depend on a SystemC lifecycle.

## Workload methodology

Characterization measures actual executions rather than a topology-only
approximation. `scripts/rn_characterize.rb` invokes the public graph generator
and runner only, so its CSV follows the current solver algorithms and command
stream without duplicating PBQP logic.

The corpus has fixed-seed random-sparse, degree-3, degree-4, mixed-degree,
tree, and cycle families. It compares reduction-only, all RN policies, local
search, and RN-plus-local-search over 20-node inputs, then compares both exact
algorithms on an eight-node subset with an explicit search limit. The CSV
records status, objective/gap when an exact reference exists, reduction work,
generic conditioning traffic, local-search work, exact-tree work, RN episodes
and cascade-length histograms, plus the generic operation mix. The latter
separates operation elements, current descriptors and batches, and logical
operand/result traffic.

```sh
cmake --build build --target solver_characterization
```

The target writes the raw `build/solver-characterization.csv` and generated
`build/solver-characterization-summary.md`. Factual results and interpretation
belong in [solver-characterization.md](reports/solver-characterization.md), not in this
design note.

`hw_sw_characterization` consumes the runner's JSONL solver events to construct
hypothetical HW/SW epochs without reproducing solver decisions. Its results are
in [hw-sw-boundary-characterization.md](reports/hw-sw-boundary-characterization.md).

`scripts/scaling_characterize.rb` runs the same generator/runner pair over a
graph-size sweep, a domain-size sweep, a joint N x D grid, and an RN-policy
comparison, deduplicating points shared across sweeps and per-family node
lists (`--nodes-for`/`--domain-nodes-for`) so an expensive family such as
mixed-degree can be swept over a smaller N range than degree-3/degree-4
without editing the script. `cmake --build build --target
scaling_characterization` writes `build/scaling-runs.csv`; results are in
[scaling-characterization.md](reports/scaling-characterization.md).

The shared PBQP solver uses the same allocator-backed state representation in
both environments. Hosted runs use heap-backed storage sized from the parsed
graph; freestanding tests provide static LIFO arenas for graph state, temporary
primitive batches, and recursive snapshots. This keeps scaling experiments out
of the bare-metal capacity policy without creating a second solver.

## Interactive timing runner

`pcaa_graph_run` remains the fast functional path. `pcaa_graph_run_timed` is a
separate L1 view with streaming overlap, four lanes, 16 bytes per cycle for
descriptor/read/write traffic, and a
nominal 1 ns cycle period. It reports an estimate and does not change solver
policy or the descriptor ABI.

## Measurement revisions

The reports indexed in [README.md](README.md) include measurements made before
vector-output ISA 1.0.0 and before the compact encoding. Their logical workload,
scalar descriptor counts, descriptor bytes, and timing estimates describe those
snapshots. Running the same script on current code produces a new experiment;
it does not retroactively update the published tables.

Use a separate output CSV and build directory for each solver/codec revision.
In particular, `--resume` skips rows by graph/policy/seed identity, not by code
revision, so it must not append current runs to a historical CSV. Reports should
record the checkout, command/configuration, and whether traffic is logical,
staged guest-memory traffic, or modeled device service. The current verbose
operation mix and per-opcode descriptor counts provide the comparison inputs;
phase counters alone do not replace them.

The analytical `vector_cycle_project.rb` script subtracts scalar components
from the observed total before substituting projected vector components. Its
historical totals assumed that those scalar components were actually executed.
The current default executes vector projection/add and partial MAP3 already;
using that total in the old substitution is not a valid scalar-to-vector
comparison. Reproduce the published study with its historical scalar checkout,
or use the current script's `per-edge` scalar-control default for a fresh
projection. It rejects a vector baseline instead of subtracting scalar work
that did not execute. New results still use current compact sizes and cannot
replace the historical tables.

Store measurement reports under [reports/](reports/README.md) and raw evidence
under `reports/data/`. Include checksums, the source/build revision, commands,
input identities, and the verification scope. Preserve timeout traces separately
from completed measurements; never fill missing completion counters with zero.
The [evidence inventory](reports/data/README.md) records the recovered and
repeated datasets; `ruby scripts/verify_report_data.rb` checks them offline.
