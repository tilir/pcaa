# Native CPU / L1 / L2 measurement archive

Collected on 6 October 2026 from all 491 retained LLVM inputs. This is a new
measurement revision; it does not overwrite or relabel earlier scalar or L2
archives. The [report](../../cpu-baseline-l2-comparison.md) explains its scope,
denominators and conclusions.

| File | Contents |
| --- | --- |
| `metadata.json` | Base revision, source/input/binary hashes, environment, collection command, repetitions and completion/failures |
| `implementation.tar.gz` | Exact measurement-start source overlay; extract over the pinned base checkout |
| `analysis.tar.gz` | Final table-generator overlay; extract after the measured overlay to reproduce the retained tables |
| `inputs.tar.gz` | All 491 measured graph files with repository-relative names |
| `CMakeCache.txt`, `compile_commands.json` | Release configuration and actual compiler invocations |
| `native-runs.jsonl.gz` | 2,455 subprocess records with complete stdout/stderr/status: five variants × 491 graphs, eleven solve samples plus separate profiles |
| `kernel-run.jsonl.gz` | Complete direct native microbenchmark output: 15 repetitions × 2,000 calls, 200 warmups, 9 shapes, 2 layouts, 3 forms, 2 operations, 3 levels |
| `model-runs.jsonl.gz` | 1,964 subprocess records: one L1 and three L2 runs per graph, full outputs and callback profiles |
| `graphs.csv` | Per-graph solve/kernel samples, diagnostic profiles, class counters, preparation and cycles; class fields begin with `projection_` |
| `batches.csv` | Corresponding CPU and L2 callback batches, logical work, timing, cycles and train/test split |
| `projection-shapes.csv` | Runtime class, dimensions, column stride, R2 origin and operation |
| `profiles.csv` | Exclusive diagnostic scopes; intrusive timing, separate from solve samples |
| `kernels.csv` | Median/p10/p90 direct kernels, logical elements/s and logical byte equivalents |
| `comparison.csv`, `sensitivity.csv`, `required-frequency.csv` | Matched work budgets and conditional device-frequency/overhead tables |
| `selective-offload.csv` | Trained threshold, CPU, offload-all and explicitly labelled oracle results; held-out and full sets |
| `summary.json`, `tables.md` | Deterministically regenerated aggregates and tables |
| `verification.json`, `*-verification.log`, `spike-verification.json` | Validation commands, scopes and retained test results |
| `SHA256SUMS` | Every top-level archive file except this checksum list |

The measured overlay retains the generator as it existed before collection.
The final analysis overlay fixes a CSV naming collision between the dense CPU
solve time and the generic-dense matrix-class timer, and adds correspondence
checks. This changes neither execution code nor raw measurements. Both versions
are retained; extraction order above makes table reproduction explicit.

From the repository root:

```sh
ruby scripts/cpu_baseline_report.rb
ruby scripts/cpu_baseline_report.rb --verify
```

The verifier checks raw sample counts, all native assignments/objectives and
reduction counts, CPU/model callback correspondence, repeated deterministic L2
statistics, cycle partitions, source/input members, file checksums and
byte-identical regeneration of all eleven outputs. It requires Ruby's standard
library and `tar`, without a build or network. It does not rerun native timing,
prove synthesized hardware timing or validate historical datasets, which have
their own verifiers.

To collect new results, rebuild the pinned base plus measured source overlay,
then apply the analysis overlay and use a fresh output directory:

```sh
cmake -S . -B build-cpu-release -DSPIKE_SOURCE_DIR=../riscv-isa-sim \
  -DCMAKE_BUILD_TYPE=Release -DPCAA_CPU_AVX2=ON
cmake --build build-cpu-release --parallel 8
ruby scripts/cpu_baseline_measure.rb --build build-cpu-release --output /tmp/cpu-study
ruby scripts/cpu_baseline_report.rb --data /tmp/cpu-study
ruby scripts/cpu_baseline_report.rb --data /tmp/cpu-study --verify
```

AVX2 requires a supporting host; the portable `PCAA_CPU_AVX2=OFF` kernel path
is separately unit-tested. Recorded times are specific to the pinned CPU0
processes on the archived WSL2 host. Metadata fixes the exact command and
configuration; reproduction on another environment should retain its own hashes
and samples rather than append to this archive.
