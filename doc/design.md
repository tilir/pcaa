# Workload characterization design

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
belong in [solver-characterization.md](solver-characterization.md), not in this
design note.

`hw_sw_characterization` consumes the runner's JSONL solver events to construct
hypothetical HW/SW epochs without reproducing solver decisions. Its results are
in [hw-sw-boundary-characterization.md](hw-sw-boundary-characterization.md).

`scripts/scaling_characterize.rb` runs the same generator/runner pair over a
graph-size sweep, a domain-size sweep, a joint N x D grid, and an RN-policy
comparison, deduplicating points shared across sweeps and per-family node
lists (`--nodes-for`/`--domain-nodes-for`) so an expensive family such as
mixed-degree can be swept over a smaller N range than degree-3/degree-4
without editing the script. `cmake --build build --target
scaling_characterization` writes `build/scaling-runs.csv`; results are in
[scaling-characterization.md](scaling-characterization.md).

The shared PBQP solver uses the same allocator-backed state representation in
both environments. Hosted runs use heap-backed storage sized from the parsed
graph; freestanding tests provide static LIFO arenas for graph state, temporary
primitive batches, and recursive snapshots. This keeps scaling experiments out
of the bare-metal capacity policy without creating a second solver.

## Interactive timing runner

`pcaa_graph_run` remains the fast functional path. `pcaa_graph_run_timed` is a
separate L1 view with streaming overlap, four lanes, default byte rates, and a
nominal 1 ns cycle period. It reports an estimate and does not change solver
policy or the descriptor ABI.
