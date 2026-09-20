# PBQP workload characterization

`scripts/rn_characterize.rb` emits CSV from real executions of the public
graph-runner CLI. It records seed, solver status, objective/optimality gap,
reduction counts, RN/core statistics, projection/commit traffic, and optional
L1 timing. It does not contain host addresses.

Run it with:

```sh
cmake --build build --target rn_characterization
```

The script intentionally does not approximate the solver: it asks the runner
for the observed statistics of the shared solver and, when requested, asks the
timed runner for the observed L1 accounting. Current factual policy and timing
summaries are checked into [the L1 performance-model report](l1-performance-model.md).

“Register-allocation-like” names a synthetic domain-size/workload shape, not a
trace extracted from LLVM register allocation.
