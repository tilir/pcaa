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
generic conditioning traffic, local-search work, and exact-tree work.

```sh
cmake --build build --target solver_characterization
```

The target writes `build/solver-characterization.csv`. Factual results and
interpretation belong in [solver-characterization.md](solver-characterization.md),
not in this design note.

## Interactive timing runner

`pcaa_graph_run` remains the fast functional path. `pcaa_graph_run_timed` is a
separate L1 view with streaming overlap, four lanes, default byte rates, and a
nominal 1 ns cycle period. It reports an estimate and does not change solver
policy or the descriptor ABI.
