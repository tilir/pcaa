# Report evidence

Collected/rechecked on **2026-10-01**. [metadata.json](metadata.json) records
source revisions, builds, binary hashes, tools, commands, and verification
scope. [SHA256SUMS](SHA256SUMS) covers every retained evidence file except
this checksum list. [verification.json](verification.json) records the
successful offline checks. From the repository root:

```sh
ruby scripts/verify_report_data.rb
```

The verifier fails on changed bytes, mismatched repeat runs, inconsistent
fork/search counters, incorrect road quantization, or changed checked table
rows. It also validates all 491 corpus input hashes against the repository.
It requires only Ruby's standard library and no network or build tree.

## Evidence inventory

`historical/*.csv.gz` are recovered, unmodified build artifacts (compressed
losslessly). Their original files did not embed source revisions. Their
attribution is established by complete repeated measurements against the
revisions below; it is not inferred solely from a report's commit date.
`recheck/*.csv.gz` retain those independent reruns. Empty unavailable values
are normalized when comparing older uneven CSV rows; wall times and temporary
synthetic graph filenames are excluded from deterministic comparisons.

| Report/data | Original evidence | Independent recheck |
| --- | --- | --- |
| Solver | [880 rows](historical/solver-characterization.csv.gz) | [880 rows](recheck/solver-characterization.csv.gz), [generated summary](recheck/solver-characterization-summary.md), `e80c21c` |
| Epoch boundaries | [49,522 rows](historical/hw-sw-epochs.csv.gz) | [49,522 rows](recheck/hw-sw-epochs.csv.gz), 30 graph inputs and 210 compressed JSONL files in `recheck/hw-sw-traces/`, `dd1402b` |
| Original scaling | [1,128 rows](historical/scaling-runs.csv.gz), [same rows with initial epochs](historical/scaling-runs-with-epochs.csv.gz) | Covered by the per-edge sweep and [270 local-search rows](recheck/local-search.csv.gz) |
| Per-node scaling, matrix views, primitive shapes | [898 rows](historical/isa-round3-scaling.csv.gz) | [898 rows](recheck/isa-round3-scaling.csv.gz), `dd1402b` |
| Per-edge scaling control | [898 rows](historical/isa-round3-per-edge.csv.gz) | [898 rows](recheck/isa-round3-per-edge.csv.gz), `dd1402b` |
| Timed per-node/projection | [671 rows](historical/vector-cycle-projection.csv.gz) | [671 rows](recheck/vector-cycle-projection.csv.gz), [triangle trace](recheck/historical-triangle-timing.txt), `dd1402b` |
| Timed per-edge control | [671 rows](historical/vector-cycle-per-edge.csv.gz) | [671 rows](recheck/vector-cycle-per-edge.csv.gz), `dd1402b` |
| Fork work | [298,434 rows](historical/fork-parallelism.csv.gz) | [65 heuristic medians](recheck/fork-heuristic.csv), all `heuristic-*.jsonl.gz` traces; completed exact fork sequences match |
| Exact search | Original search summaries were not recovered | [65 explicit inputs/results](recheck/branch-bound.csv), `search-*.txt`, `search-*.jsonl.gz`, synthetic `search-*.pbqp` inputs, `dd1402b` |
| LLVM corpus | [499 PBQP inputs](inputs/pbqp-inputs.tar.gz): 491 corpus graphs plus eight top-level examples | [491 graph records](recheck/llvm-corpus.csv), including full domain vectors, source headers and SHA-256 hashes |
| LLVM extraction | Four archived source/IR files and [instrumentation patch](inputs/llvm/RegAllocPBQP.patch), with LLVM/CUDD licenses | [491 byte-identical outputs](recheck/llvm-extraction.csv), [command log](recheck/llvm-extraction.txt), [exact commands](inputs/llvm/extraction-commands.json) |
| Cost representation | DIMACS [coordinates](inputs/USA-road-d.NY.co.gz) and [road arcs](inputs/USA-road-d.NY.gr.gz) | [733,846 derived arc records](recheck/road-arcs.csv.gz), [summary](recheck/road-summary.csv), [log weights](recheck/hmm-log-weights.csv), [literature facts/citations](recheck/literature.csv) |
| Compact encoding | Four example PBQP inputs in the input archive | [counts/bytes/aliases](recheck/compact-encoding.csv), `*-compact.txt`, [current triangle timing](recheck/current-triangle-timing.txt), `86d76b1` |
| Historical RV64 batch storage | `dd1402b` adapter/ELF | [layout source](recheck/historical-context-layout.c.txt), [RV64 size](recheck/historical-context-layout.s), [full disassembly](recheck/historical-pbqp-rn-disassembly.txt.gz), [Spike success](recheck/historical-pbqp-rn-spike.txt) |
| Current verification | `86d76b1` binaries plus measurement-script corrections | [Debug CTest](recheck/ctest-debug.txt), [Release CTest](recheck/ctest-release.txt); both 32/32 |

Current-script regression evidence is also retained: [88 solver rows](recheck/current-solver-smoke.csv.gz)
from `--seeds 1001`, [16 degree-3 grid rows](recheck/current-scaling-smoke.csv.gz)
from `--sweeps grid --families degree-3 --grid-seeds 1001`, and
[180 current scalar timing rows](recheck/current-vector-scalar.csv.gz) with an
empty corpus directory. The [vector-baseline rejection](recheck/current-projection-rejection.txt)
confirms that the projection script refuses to subtract hypothetical scalar
work from an already-vector execution. These are separate current-code checks,
not replacements for the historical datasets.

Synthetic inputs are identified by family, profile, N, D, and seed in the
CSVs and generated by the pinned revision's `pbqp_graph_generate`. All
fixed source inputs are archived here. Temporary synthetic filenames in the
old timed CSV are not identities; use their parameter columns. Search rows
add the SHA-256 of each actual input. Diagnostic summaries omit repetitive
per-primitive device logging; full logical solver traces are preserved.

## Reproduce the historical measurements

Use separate source/build/output directories. Never `--resume` a historical
CSV with current binaries. The following collects the same scalar workload
without altering your current checkout:

```sh
mkdir -p /tmp/pcaa-report-baseline /tmp/pcaa-report-solver
git archive dd1402b | tar -x -C /tmp/pcaa-report-baseline
git archive e80c21c | tar -x -C /tmp/pcaa-report-solver
cmake -S /tmp/pcaa-report-baseline -B /tmp/pcaa-report-baseline/build \
  -DSPIKE_SOURCE_DIR=/home/tilir/riscv-isa-sim -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/pcaa-report-baseline/build \
  --target pcaa_graph_run pcaa_graph_run_timed pbqp_graph_generate --parallel 6
cmake -S /tmp/pcaa-report-solver -B /tmp/pcaa-report-solver/build \
  -DSPIKE_SOURCE_DIR=/home/tilir/riscv-isa-sim -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/pcaa-report-solver/build \
  --target pcaa_graph_run pbqp_graph_generate --parallel 6
```

Adjust `SPIKE_SOURCE_DIR` for your environment. From this repository root:

```sh
ruby /tmp/pcaa-report-solver/scripts/rn_characterize.rb \
  --runner /tmp/pcaa-report-solver/build/pcaa_graph_run \
  --generator /tmp/pcaa-report-solver/build/pbqp_graph_generate \
  --output /tmp/solver-repeated.csv --summary /tmp/solver-repeated.md

PCAA_TRACE_DIR=/tmp/hw-sw-traces \
PCAA_RUNNER=/tmp/pcaa-report-baseline/build/pcaa_graph_run \
PCAA_GENERATOR=/tmp/pcaa-report-baseline/build/pbqp_graph_generate \
  ruby scripts/hw_sw_characterize.rb /tmp/epochs-repeated.csv

ruby /tmp/pcaa-report-baseline/scripts/scaling_characterize.rb \
  --runner /tmp/pcaa-report-baseline/build/pcaa_graph_run \
  --generator /tmp/pcaa-report-baseline/build/pbqp_graph_generate \
  --corpus-dir examples/regalloc --domain-size-domains 2,4,8,16,32 \
  --rn-batching per-node --quiet /tmp/scaling-node-repeated.csv
# Repeat with --rn-batching per-edge into a different CSV.
# Repeat with --sweeps local-search --rn-batching per-edge and no corpus option
# into a third CSV for the 270 local-search/hybrid rows.

ruby /tmp/pcaa-report-baseline/scripts/vector_cycle_project.rb \
  --runner /tmp/pcaa-report-baseline/build/pcaa_graph_run_timed \
  --generator /tmp/pcaa-report-baseline/build/pbqp_graph_generate \
  --rn-batching per-node --output /tmp/cycles-node-repeated.csv
# Repeat with --rn-batching per-edge into a different CSV.

ruby scripts/recheck_search_report.rb \
  --runner /tmp/pcaa-report-baseline/build/pcaa_graph_run \
  --generator /tmp/pcaa-report-baseline/build/pbqp_graph_generate \
  --output /tmp/search-repeated
# Repeat with --heuristic-only for the 65 Model-C medians/source traces.

ruby scripts/cost_representation_analyze.rb \
  doc/reports/data/inputs/USA-road-d.NY.co.gz \
  doc/reports/data/inputs/USA-road-d.NY.gr.gz /tmp/road-arcs-repeated.csv.gz \
  > /tmp/road-summary-repeated.csv

build/pcaa_graph_run --solver bare-metal --strategy heuristic-rn --verbose \
  examples/triangle.pbqp
# Repeat for petersen, chvatal, random-20.
```

To reproduce LLVM extraction from the archived IR, apply
[RegAllocPBQP.patch](inputs/llvm/RegAllocPBQP.patch) to LLVM commit
`9517d7182766f9d16dda05a97464660e27ce91f3`, build its `llc`, decompress the
four `inputs/llvm/*.ll.gz` files, then invoke `llc -O2 -regalloc=pbqp
-march=x86-64 -filetype=asm -pcaa-pbqp-dump-dir=OUTPUT FILE.ll -o /dev/null`.
For byte-identical source-header comments, restore the original
`/tmp/pcaa-corpus-ll/<name>.ll` paths. Graph bodies are independent of these
path comments. The exact source-to-IR and `llc` commands, including compiler
flags, are archived in `inputs/llvm/extraction-commands.json`.

DIMACS inputs were downloaded from the challenge's public source over HTTP
because its HTTPS endpoint failed TLS negotiation during collection:
[coordinates](http://www.diag.uniroma1.it/challenge9/data/USA-road-d/USA-road-d.NY.co.gz),
[arcs](http://www.diag.uniroma1.it/challenge9/data/USA-road-d/USA-road-d.NY.gr.gz).
Their sizes, headers, node/arc counts and derived statistics match the report;
checksums pin the downloaded bytes but are not publisher-supplied authenticity
checksums. The experiment derives great-circle distances from coordinates,
not the published integer road weights. The original weights are also retained
in each derived arc row. HMM numbers are analytic samples, not a speech corpus.
Literature rows are attributed factual values, not local synthesis raw data.

## Verification boundaries and corrections

All complete, deterministic rows match the appropriate original revision,
including old scalar descriptor counts. The original real branch-and-bound
selection had no saved filenames: its table is replaced with the explicit
39-file fork sample and new 45-second runs. Both timeout traces remain partial;
missing final counters and optima remain blank. All 63 completed exact runs
reproduce their original fork sequences, and all 65 heuristic medians match.
Wall time is host-dependent and includes trace/logging overhead.

Recalculation corrected the corpus's nonuniform-domain count to 475/491
(96.7%), mixed-degree contiguous:strided to 3.13:1, and a fork median to
14,111.5. The source papers show that the quoted FPGA LUT count belongs to a
merged DP/dual-SP adder; the separate untraceable textbook ASIC-area claim was
removed. Source-code accumulation-order conclusions remain code audits,
not performance measurements.

All four translation units were compiled again with the original system
Clang 21.1.8 and patched LLVM `llc` at `9517d7182766f9d16dda05a97464660e27ce91f3`.
All 491 resulting PBQP files match the saved inputs byte-for-byte. Source
files, IR, the local instrumentation patch, compiler commands, licenses and
extraction diagnostics are archived. The rerun reported no clamped values or
file-open errors. Original filename-failure counts before the truncation fix
had no saved logs and are removed from the report.
Likewise model projections are checked arithmetic, not validation of hardware
that has not been implemented.
