# Exact PBQP CPU/L2 evidence

This is a new bounded exact-search measurement revision. It preserves the full
491-input LLVM corpus, 24 fixed-seed synthetic graphs and 18 exact-stress inputs.
Historical heuristic CPU and L2 archives are independent revisions.

* `metadata.json`: source/input/binary hashes, machine/compiler/dependencies,
  exact invocation, seeds, finite limits, L2 configuration and timestamps.
* `implementation.tar.gz`: complete measured execution source overlay; apply to
  the recorded base commit for reproduction. No execution source changed during
  the final measurement.
* `inputs/`: every actual graph, with source hashes and generator configuration.
* `cpu-runs.jsonl.gz`, `model-runs.jsonl.gz`: raw subprocess command, stdout,
  stderr, process/solver statuses; warmup baseline and all timing/counter passes.
* `compile_commands.json`, `CMakeCache.txt`: exact native build flags.
* `statuses.csv`: complete classifications, including limits, errors and timeouts.
* `workloads.csv`: native timing distributions, trees, logical work, allocation,
  modeled cycles/requests/bytes/lanes and per-input hypothetical break-even.
* `profiles.csv`, `phase-summary.csv`: intrusive exclusive diagnostic phase times.
* `batches.csv`, `operation-mix.csv`: R1/R2/conditioning callback sizes, logical
  bytes, host staging/build/readback, opcode counts and modeled phase cycles.
* `scenarios.csv`: derived no-overlap 250/500/1000/2000 MHz estimates, separate
  optimistic matched CPU host and observed current model-path host scenarios.
* `incumbents.csv`, `controls.csv`: algorithmic seed comparisons and original /
  degree-only native controls. Seeding is not a hardware speedup.
* `counterfactuals.csv`: optimistic cycle category removals, not modified hardware.
* `predictors.csv`: exploratory correlations on censored completed subsets.
* `summary.json`, `tables.md`: regenerated aggregate results.
* `verification/`: Debug/Release test, Spike and historical archive verification
  logs; complete conservative RV64 stack reports and compilation commands.
* `analysis/`: report-generation source, separate from measured execution code.
* `SHA256SUMS`: all archived evidence files, recursively.

From the repository root:

```sh
ruby scripts/exact_pbqp_report.rb --verify
ruby scripts/exact_pbqp_publish.rb --verify
```

Verification checks retained evidence and deterministic regeneration. It does
not repeat CPU wall time or assert physical silicon timing. Completion fractions
are reported over all inputs; hardware speedups use only matched completed exact
solves. Diagnostic perturbation and simulation wall times are never silently
converted into hardware performance. The prior malformed stress-generator trial
was discarded; all final archived stress inputs use one line per `edge`.

Final analysis sources are retained separately under analysis/. The strict current-host
win fraction excludes zero-device-service cases; measured execution files and raw
samples remain unchanged. analysis/provenance.json records the analysis hashes.
