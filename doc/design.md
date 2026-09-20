# Workload characterization design

Characterization runs through the public graph-runner CLI, rather than through
a topology-only C++ approximation. `scripts/rn_characterize.rb` invokes the
untimed runner for solver results and optional timed runner for L1 estimates.
It uses `--solver local` deliberately: the hosted model is the experiment
environment, while the selected shared strategy remains identical to the
bounded RV64 solver whenever the generated graph fits its capacity.

The default corpus is ten deterministic 20-node binary synthetic random graphs
with seeds 1001 through 1010. Each has independently sampled edges at 20%; its
unary and binary costs are sampled from -5 through 5. These are synthetic
topologies, not extracted register-allocation workloads.

For every seed the script runs `REDUCE_ONLY`, the exact bounded reference, and
`HEURISTIC_RN` with all three node-selection policies. Its CSV records solver
status, objective and absolute gap, reductions, RN/core statistics, projection
and commit traffic, plus optional L1 timing for the minimum-degree policy.

```sh
cmake --build build --target rn_characterization
```

The target writes `build/rn-characterization.csv`.

## Interactive timing runner

`pcaa_graph_run` is intentionally untimed, so graph-format experiments keep a
fast functional path. `pcaa_graph_run_timed` is the separate interactive L1
view: it uses streaming overlap, four lanes, the default byte rates from
`AccelTimingConfig`, and a nominal 1 ns cycle period. Its output keeps total
service cycles separate from descriptor, operand-read, compute, and
result-write components. This runner reports an estimate; it does not change
the PBQP descriptor ABI or the software solver policy.
