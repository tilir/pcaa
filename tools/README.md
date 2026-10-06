# Host tools

These tools solve PBQP graphs, generate inputs, and measure PCAA execution.
Build them from the repository root. Graph inputs use the `nodes`/`node`/`edge`
text format with `INF` costs; see the [main README](../README.md).

| Tool | Purpose | Output |
| --- | --- | --- |
| `pcaa_graph_run` | Solve through the functional SystemC model | Objective, assignment, solution kind |
| `pcaa_graph_run_timed` | Solve with the analytical four-lane L1 estimate | The solution and service-cycle breakdown |
| `pcaa_graph_run_l2` | Solve through the structural MAS 1.0.0 model | The solution and `l2` JSON statistics |
| `pbqp_graph_generate` | Generate deterministic synthetic PBQP inputs | Graph text on stdout |
| `pcaa_l2_microbench` | Verify and measure direct L2 commands across shapes/layouts | JSONL with modeled cycles and verification results |
| `pcaa_cpu_current` | Measure the existing native solver without SystemC | JSONL samples, callback timings, answer |
| `pcaa_cpu_bench` | Compare degree caching and scalar/dense/structured CPU kernels | JSONL samples, exclusive profiles, structure evidence, answer |
| `pcaa_cpu_microbench` | Measure warmed native projection/ADD3 kernels | Raw JSONL timings for each shape/layout/implementation |
| `pcaa_exact_cpu` | Measure bounded exact branch-and-reduce with native kernels | JSONL solve samples, work counters, diagnostic phases and callback batches |
| `pcaa_exact_cpu_portable` | Run the same exact experiment without AVX2 | The same schema and algorithm |
| `pcaa_exact_l2` | Run matched exact search through current MAS 1.0.0 | JSONL answers and attributed service cycles; warmup counters are retained separately |

## Solve or generate graphs

```sh
cmake -S . -B build
cmake --build build --target pcaa_graph_run pcaa_graph_run_timed pcaa_graph_run_l2 pbqp_graph_generate
build/pcaa_graph_run --solver local examples/triangle.pbqp
build/pcaa_graph_run_timed --solver local examples/triangle.pbqp
build/pcaa_graph_run_l2 --solver local examples/triangle.pbqp
build/pbqp_graph_generate --family degree-3 --profile small --nodes 20 --seed 1001
```

The runners require `--solver bare-metal|local`. Their default strategy is
`heuristic-rn`, with `min-degree` RN selection and per-node batching. Use
`--help` to select another strategy or change the L2 configuration. Only
completed exact strategies claim an exact solution.

`--trace FILE` writes solver events. `--verbose` reports workload counters on
stderr. `--kernel-profile FILE` records each cost callback batch's service
cycles and hosted preparation times after the solve; L2 polling/simulation
wall time is excluded from those preparation times. L1 executes synchronously,
so its hosted submission timing includes functional execution and is not a
CPU packing benchmark. Use L2 preparation measurements for that comparison.

## Native CPU performance experiment

Performance measurements require a Release build. AVX2 is an explicit option
for a supporting x86 CPU; omit it for the portable dense implementation.

```sh
cmake -S . -B build-cpu-release -DCMAKE_BUILD_TYPE=Release -DPCAA_CPU_AVX2=ON
cmake --build build-cpu-release --parallel 8
ctest --test-dir build-cpu-release -R cpu_kernel_unit --output-on-failure
taskset -c 0 build-cpu-release/pcaa_cpu_current current examples/triangle.pbqp
taskset -c 0 build-cpu-release/pcaa_cpu_bench structured examples/triangle.pbqp
ruby scripts/cpu_baseline_measure.rb --build build-cpu-release --output /tmp/cpu-study
ruby scripts/cpu_baseline_report.rb --data /tmp/cpu-study
```

`pcaa_cpu_bench current` retains full edge-array degree scans for profiling;
`scalar` adds degree caching; `dense` also uses checked SIMD/inline cost
arithmetic and shared ADD3 pre-adds; `structured` adds verified row-background
representations with at most two exceptions. The production solver and RV64
storage policy retain their existing implementation. Parsing, cloning the
input, printing, and trace serialization are outside solve timing. Matrix
classification and cache invalidation during a solve are inside it.

These native tools have an ordinary `main` and do not link SystemC. All
`*_model` runner binaries are implementation companions of the launchers;
use the launcher names above as the supported entry points. The CPU experiment
owns its targets and formatting inputs in [cpu_baseline/CMakeLists.txt](cpu_baseline/CMakeLists.txt).

## Exact PBQP experiment

```sh
build-cpu-release/pcaa_exact_cpu dense examples/chvatal.pbqp none 128 7 vector
build-cpu-release/pcaa_exact_cpu_portable dense examples/chvatal.pbqp none 128 7 vector
build-cpu-release/pcaa_exact_l2 dense examples/chvatal.pbqp heuristic 128 3 vector
ruby scripts/exact_pbqp_measure.rb --build build-cpu-release --output /tmp/exact-study
ruby scripts/exact_pbqp_report.rb --data /tmp/exact-study
ruby scripts/exact_pbqp_report.rb --data /tmp/exact-study --verify
```

Arguments are kernel/policy variant, input, `none|heuristic` incumbent,
positive search-node limit, sample count (at least three), `vector|scalar`
conditioning, and optional `branch|enumeration|heuristic` strategy. `current`
retains scans and original snapshots; `degree` adds cached degrees; `dense`
also uses warmed workspace recycling and avoids zeroing immediately overwritten
snapshots; `structured` additionally uses verified row representations. The
seed's heuristic execution and cloning are included in total solve time.
Native executables do not link SystemC. Execution options use the shared
solver's C++ policy; the stable C API retains its default behavior.

A JSON `status` of zero means completion; `-5` means a search/workspace limit,
with no completed assignment. Process success alone is insufficient. Only
branch or enumeration strategies prove an optimum. Warmup L2 statistics occur
in the `baseline` row; subtract them from the completed model totals or use
the per-callback deltas. Diagnostic phase profiles are separate from native
solve samples and can perturb short solves substantially. See the
[exact report](../doc/reports/exact-pbqp-cpu-l2-breakdown.md) for limits,
completion fractions, overhead assumptions and the reproducible archive.

## Read the measurements

L1 cycles are analytical estimates. L2 cycles are structural model service
counts. Neither establishes a silicon clock frequency, and SystemC simulation
wall time is not device latency. Native CPU times are nanoseconds on the
recorded machine. Compare corresponding kernel work rather than the device
stream with CPU topology/search time.

The [CPU/L2 report](../doc/reports/cpu-baseline-l2-comparison.md) documents the
experiment, uncertainty, raw evidence, and regeneration commands. The
[L2 report](../doc/reports/l2-microarchitecture-characterization.md) documents
MAS activity and configuration sweeps. Measurement automation lives in
[`scripts`](../scripts); generated evidence belongs in `doc/reports/data/`.

## Source layout and tests

`pbqp_input.{h,cpp}` owns the shared reader, checked graph construction and
RAII graph handle. `pbqp_run.cpp` implements the three model runners;
`pbqp_model_kernel.{h,cpp}` stages callbacks and owns hosted L2 advancement.
The launcher keeps the public command separate from its SystemC companion.
`cpu_baseline/` owns native profiling, CPU kernels and their CMake targets.

Run the complete host suite with `ctest --test-dir BUILD --output-on-failure`.
For the tools' focused checks:

```sh
ctest --test-dir build-cpu-release \
  -R 'pbqp_input_unit|pbqp_model_(kernel|profile)_unit|cpu_(kernel_unit|solver_)|tools_cli_regression' \
  --output-on-failure
```

The tests cover malformed graph syntax, rectangular matrices, capacity/cost
errors and ownership; callback staging, failure recovery and measurement cycle
partitions; independent kernel differential cases and RN policy preservation;
and CLI validation plus unchanged solutions/cycles when recording is enabled.
`exact_policy_unit` adds randomized exhaustive-oracle checks, snapshot binding,
incumbent validation and conditioning error propagation; `exact_cli_regression`
checks seeded CPU/L2 correspondence, cycle attribution, enumeration and limits.
