# Exact PBQP: strong CPU versus current L2

## Result and scope

This is a bounded characterization of **533 inputs**, including all
**491 retained LLVM graphs**, 24 degree-3/degree-4/mixed synthetic graphs and
18 complete-graph stress cases. ISA 1.0.0 and MAS 1.0.0 are unchanged.
The common solver runs exact branch-and-reduce, with the same branch order,
sign-agnostic bound, arithmetic and workspace policy on CPU and L2.

The strongest aggregate native candidate is **structured**.
On the matched unseeded LLVM subset, current ISA kernels account for
**32.4%** of native time; even free kernels
would give only **1.480×** aggregate speedup.
The weighted ideal device break-even is **4788.380 MHz**.
With a cheap heuristic incumbent it is **3783.174 MHz**,
with a **1.048×** ideal Amdahl ceiling.
These are derived clock scenarios, not measured hardware performance.

Completion censoring matters: unseeded LLVM exact solves complete on
**196/491** inputs and seeded solves on
**400/491**. Hardware comparisons include only
completed, matched exact solves; every other input retains its status and raw output.
The measured model-path host implementation includes instrumentation and staging.
It must not be described as an unavoidable physical host lower bound.

## Implementation and experimental controls

The default C solver behavior and RV64 capacity policy remain intact. Optional
C++ execution policies enable cached degrees, copying snapshots without a
redundant initialization pass, recycled workspace allocation and conditioning
through the existing vector cost-add callback. No search work moves into hardware.
`degrees_[node]` uses typed array access; typed array copies use `std::copy_n`
in hosted builds and a build-selected freestanding equivalent. Typed observers
keep profiling clocks and hosted containers outside the solver.

The original and degree-only controls use scalar conditioning. Dense AVX2,
portable dense and structured candidates use the same vector conditioning,
search tree and complete assignments. The strongest candidate is selected once
by aggregate unseeded CPU time over inputs completed by all three candidates;
there is no per-graph selection of the fastest sample. Structured kernels retain
explicit R1 metadata and invalidate cached representations when edge storage
changes or snapshots recycle addresses. Cloning still copies the full graph
capacity; changed-state sharing and incremental lower bounds are not implemented.

Native timing includes seed construction where selected, search, reductions,
allocation and reconstruction. Parsing, original graph construction and initial
clone setup precede the interval. Seven uninstrumented samples follow a warmup;
medians and p10/p90 are retained. Three event passes and three exclusive phase
passes are diagnostics, with their perturbation ratios recorded. Callback time
subtracts the calibrated clock-pair floor, clamped at zero. Fine-grained phase
measurements are intrusive and cannot be substituted for native wall time.

Seed solving remains native CPU work on both paths. A heuristic witness is validated against the original graph and counted in
seed cost. It initializes the incumbent and can prove optimality at the root
when the lower bound reaches its objective. Root pruning returns the validated
complete witness, never an externally visible solver-internal pruned status.
First-solution and optimum-found timestamps come from diagnostic passes;
completion is the proof timestamp. A witness can already equal the optimum
long before that fact is proved. Time-to-optimum is identified retrospectively.

All runs are Release, pinned to CPU 0, with a search-node limit
of 512, a 512 MiB live workspace limit and subprocess ceilings
of 8 s native / 20 s model.
Workspace failures are capacity/errors, separate from node-limit hits. The LLVM capacity/error rows in this archive are explicit 512 MiB workspace exhaustion, not malformed corpus inputs. Failed
and timed-out runs do not acquire invented completion counters. Model runs are
attempted only after the AVX2 native run completes; three fresh model processes
must agree. Warmup counters are explicitly subtracted.

The machine, compiler, complete flags and SystemC version are in
[metadata](data/exact-pbqp/metadata.json), [compilation commands](data/exact-pbqp/compile_commands.json)
and [CMake cache](data/exact-pbqp/CMakeCache.txt). Measurements started
2026-10-06T21:31:18Z and finished 2026-10-06T21:35:29Z. The source snapshot
is based on commit `22d3ba040124ca3f8b794b6d6e1f6fdd370db716`, with all measured source and binary
hashes archived; it is a new revision, not a relabeling of historical measurements.

## Completion, end-to-end budgets and break-even

Let `T` be uninstrumented native solve time, `K` calibrated callback time,
`H = max(T − K, 0)` and `C` modeled device service cycles. The optimistic matched
host scenario is `H + C/f`; its break-even is `C/K` and its Amdahl ceiling is
`T/H`. The observed current-host scenario uses measured model-path time outside
callbacks plus staging/build/readback time, and then adds `C/f`. Simulation
callback wall time is excluded from hardware timing. If this host denominator
already exceeds `T`, no finite current-path break-even exists. There is no
host/device overlap. Batch execution remains ordered.

### Completion (strong CPU)

| Class | Incumbent | Inputs | Exact CPU | Search limit | Timeout/error | Matched L2 |
| --- | --- | --- | --- | --- | --- | --- |
| llvm | none | 491 | 196 | 287 | 8 | 196 |
| llvm | heuristic | 491 | 400 | 86 | 5 | 400 |
| synthetic | none | 24 | 18 | 6 | 0 | 18 |
| synthetic | heuristic | 24 | 18 | 6 | 0 | 18 |
| stress | none | 18 | 16 | 2 | 0 | 16 |
| stress | heuristic | 18 | 16 | 2 | 0 | 16 |

## Matched end-to-end budgets

| Class | Seed | CPU ms | Kernel ms | Kernel share | Control ms | L2 cycles | Ideal MHz | Amdahl max | Current host ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| llvm | none | 47.889 | 15.526 | 32.420% | 32.364 | 74342480 | 4788.380 | 1.480 | 169.035 |
| llvm | heuristic | 17.895 | 0.826 | 4.618% | 17.068 | 3126487 | 3783.174 | 1.048 | 37.261 |
| synthetic | none | 1.650 | 0.752 | 45.550% | 0.899 | 918465 | 1221.714 | 1.837 | 7.791 |
| synthetic | heuristic | 1.545 | 0.662 | 42.846% | 0.883 | 785483 | 1186.720 | 1.750 | 6.644 |
| stress | none | 0.700 | 0.214 | 30.586% | 0.486 | 394256 | 1841.173 | 1.441 | 4.704 |
| stress | heuristic | 0.735 | 0.212 | 28.867% | 0.523 | 388876 | 1833.084 | 1.406 | 4.625 |

## Break-even distributions (positive ideal frequencies only)

| Class | Seed | Median MHz | Geomean MHz | p75 MHz | p90 MHz | Max MHz | Finite current break-even |
| --- | --- | --- | --- | --- | --- | --- | --- |
| llvm | none | 4648.804 | 4369.943 | 5088.580 | 5592.989 | 6097.681 | 0 |
| llvm | heuristic | 3311.232 | 3234.671 | 3993.207 | 5168.697 | 6003.447 | 1 |
| synthetic | none | 1232.968 | 1278.245 | 1289.763 | 1612.733 | 1925.236 | 0 |
| synthetic | heuristic | 1279.425 | 1278.491 | 1409.400 | 1678.677 | 2269.587 | 0 |
| stress | none | 1503.850 | 1569.072 | 1916.023 | 2216.919 | 2580.331 | 0 |
| stress | heuristic | 1626.547 | 1627.737 | 2044.284 | 2195.249 | 2724.218 | 0 |

## Win fractions among matched completed inputs

| Class | Seed | 250 MHz ideal/current | 500 MHz ideal/current | 1 GHz ideal/current | 2 GHz ideal/current |
| --- | --- | --- | --- | --- | --- |
| llvm | none | 0.0% / 0.0% | 0.0% / 0.0% | 0.0% / 0.0% | 1.0% / 0.0% |
| llvm | heuristic | 0.0% / 0.2% | 0.0% / 0.2% | 0.2% / 0.2% | 8.8% / 0.2% |
| synthetic | none | 0.0% / 0.0% | 0.0% / 0.0% | 0.0% / 0.0% | 100.0% / 0.0% |
| synthetic | heuristic | 0.0% / 0.0% | 0.0% / 0.0% | 5.6% / 0.0% | 94.4% / 0.0% |
| stress | none | 0.0% / 0.0% | 0.0% / 0.0% | 0.0% / 0.0% | 75.0% / 0.0% |
| stress | heuristic | 0.0% / 0.0% | 0.0% / 0.0% | 0.0% / 0.0% | 62.5% / 0.0% |


## Native phase and algorithmic evidence

Top exclusive diagnostic phases, including all completed native solves (not just L2 matches):

| Class | Seed | Phase | ms | Diagnostic-share |
| --- | --- | --- | --- | --- |
| llvm | none | kernel | 13.532 | 17.7% |
| llvm | none | clone | 12.426 | 16.3% |
| llvm | none | search | 9.377 | 12.3% |
| llvm | none | r2 | 9.291 | 12.2% |
| llvm | none | lower_bound | 6.907 | 9.0% |
| llvm | none | topology | 5.748 | 7.5% |
| llvm | heuristic | kernel | 9.183 | 28.4% |
| llvm | heuristic | selection | 6.038 | 18.7% |
| llvm | heuristic | topology | 5.883 | 18.2% |
| llvm | heuristic | reduction | 2.126 | 6.6% |
| llvm | heuristic | seed | 1.913 | 5.9% |
| llvm | heuristic | r2 | 1.790 | 5.5% |
| synthetic | none | kernel | 0.754 | 17.0% |
| synthetic | none | r2 | 0.732 | 16.5% |
| synthetic | none | search | 0.714 | 16.1% |
| synthetic | none | topology | 0.668 | 15.0% |
| synthetic | none | allocation | 0.422 | 9.5% |
| synthetic | none | selection | 0.385 | 8.7% |
| synthetic | heuristic | r2 | 0.705 | 17.1% |
| synthetic | heuristic | kernel | 0.695 | 16.9% |
| synthetic | heuristic | topology | 0.656 | 15.9% |
| synthetic | heuristic | search | 0.583 | 14.2% |
| synthetic | heuristic | allocation | 0.389 | 9.4% |
| synthetic | heuristic | selection | 0.387 | 9.4% |
| stress | none | search | 0.605 | 24.6% |
| stress | none | topology | 0.325 | 13.2% |
| stress | none | allocation | 0.285 | 11.6% |
| stress | none | kernel | 0.255 | 10.4% |
| stress | none | conditioning | 0.222 | 9.1% |
| stress | none | selection | 0.186 | 7.6% |
| stress | heuristic | search | 0.598 | 23.2% |
| stress | heuristic | topology | 0.351 | 13.6% |
| stress | heuristic | allocation | 0.305 | 11.8% |
| stress | heuristic | kernel | 0.268 | 10.4% |
| stress | heuristic | conditioning | 0.235 | 9.1% |
| stress | heuristic | selection | 0.200 | 7.8% |

Perturbation is substantial on short solves. The detailed values and per-input ratios in [profiles.csv](data/exact-pbqp/profiles.csv) and [workloads.csv](data/exact-pbqp/workloads.csv) describe instrumented execution; they do not establish unbiased percentages of uninstrumented time.

### CPU candidates on the common unseeded completion subset

| Candidate | Inputs | Aggregate native ms |
| --- | --- | --- |
| avx2 | 230 | 72.825 |
| portable | 230 | 90.569 |
| structured | 230 | 50.240 |

The portable executable has no AVX2 build flag. AVX2 is not universally beneficial; structured layout wins this exact workload set. Original/degree controls below cover the first 20 LLVM inputs and every generated input, so their completed subset is much smaller and cannot establish full-corpus optimization speedup.

| Original control | Matched samples | Median strong speedup | Aggregate strong speedup |
| --- | --- | --- | --- |
| current | 35 | 0.914 | 1.070 |
| degree | 35 | 0.817 | 1.009 |

Heuristic initialization is an algorithmic comparison, including seed time:

| Class | Both exact complete | Median seed speedup | Median visited reduction | Median unseeded exact / heuristic time |
| --- | --- | --- | --- | --- |
| llvm | 196 | 7.011 | 45.000 | 7.126 |
| synthetic | 18 | 0.906 | 1.000 | 7.802 |
| stress | 16 | 0.880 | 1.000 | 6.201 |

Full witnesses, work histograms, allocation/clone bytes, pruning counts and first/found/proof times remain in the raw records and [incumbents.csv](data/exact-pbqp/incumbents.csv). Seeded and unseeded completion subsets differ; their aggregate clocks must not be interpreted as a paired hardware improvement.

### Native exact versus heuristic, same completed unseeded inputs

| Class | Paired inputs | Exact kernel share | Heuristic kernel share |
| --- | --- | --- | --- |
| llvm | 196 | 32.4% | 61.9% |
| synthetic | 18 | 45.5% | 37.4% |
| stress | 16 | 30.6% | 32.9% |

These callback shares compare the same current native implementation. Repeated solve work is not arithmetic intensity: physical traffic per modeled active element is reported separately below.

## Modeled L2 accounting

| Class | Seed | L2 phase | Cycles | Share |
| --- | --- | --- | --- | --- |
| llvm | none | descriptor | 2875224 | 3.9% |
| llvm | none | decode | 747836 | 1.0% |
| llvm | none | protection | 2569825 | 3.5% |
| llvm | none | operand | 19012084 | 25.6% |
| llvm | none | add1 | 1383926 | 1.9% |
| llvm | none | add2 | 6238718 | 8.4% |
| llvm | none | tree | 13257386 | 17.8% |
| llvm | none | merge | 6628693 | 8.9% |
| llvm | none | writeback | 5293774 | 7.1% |
| llvm | none | drain | 450980 | 0.6% |
| llvm | none | memory_wait | 13590541 | 18.3% |
| llvm | none | control | 2293493 | 3.1% |
| llvm | heuristic | descriptor | 102766 | 3.3% |
| llvm | heuristic | decode | 25278 | 0.8% |
| llvm | heuristic | protection | 77822 | 2.5% |
| llvm | heuristic | operand | 853010 | 27.3% |
| llvm | heuristic | add1 | 56963 | 1.8% |
| llvm | heuristic | add2 | 283775 | 9.1% |
| llvm | heuristic | tree | 605274 | 19.4% |
| llvm | heuristic | merge | 302637 | 9.7% |
| llvm | heuristic | writeback | 157898 | 5.1% |
| llvm | heuristic | drain | 13766 | 0.4% |
| llvm | heuristic | memory_wait | 556837 | 17.8% |
| llvm | heuristic | control | 90461 | 2.9% |
| synthetic | none | descriptor | 161090 | 17.5% |
| synthetic | none | decode | 39426 | 4.3% |
| synthetic | none | protection | 46628 | 5.1% |
| synthetic | none | operand | 126456 | 13.8% |
| synthetic | none | add1 | 13780 | 1.5% |
| synthetic | none | add2 | 34972 | 3.8% |
| synthetic | none | tree | 73200 | 8.0% |
| synthetic | none | merge | 36600 | 4.0% |
| synthetic | none | writeback | 105122 | 11.4% |
| synthetic | none | drain | 25646 | 2.8% |
| synthetic | none | memory_wait | 196334 | 21.4% |
| synthetic | none | control | 59211 | 6.4% |
| synthetic | heuristic | descriptor | 136382 | 17.4% |
| synthetic | heuristic | decode | 32978 | 4.2% |
| synthetic | heuristic | protection | 39796 | 5.1% |
| synthetic | heuristic | operand | 108992 | 13.9% |
| synthetic | heuristic | add1 | 11578 | 1.5% |
| synthetic | heuristic | add2 | 30900 | 3.9% |
| synthetic | heuristic | tree | 63928 | 8.1% |
| synthetic | heuristic | merge | 31964 | 4.1% |
| synthetic | heuristic | writeback | 89414 | 11.4% |
| synthetic | heuristic | drain | 21400 | 2.7% |
| synthetic | heuristic | memory_wait | 167394 | 21.3% |
| synthetic | heuristic | control | 50757 | 6.5% |
| stress | none | descriptor | 87362 | 22.2% |
| stress | none | decode | 24220 | 6.1% |
| stress | none | protection | 18625 | 4.7% |
| stress | none | operand | 48076 | 12.2% |
| stress | none | add1 | 7351 | 1.9% |
| stress | none | add2 | 6119 | 1.6% |
| stress | none | tree | 16064 | 4.1% |
| stress | none | merge | 8032 | 2.0% |
| stress | none | writeback | 46768 | 11.9% |
| stress | none | drain | 16869 | 4.3% |
| stress | none | memory_wait | 91103 | 23.1% |
| stress | none | control | 23667 | 6.0% |
| stress | heuristic | descriptor | 86278 | 22.2% |
| stress | heuristic | decode | 23924 | 6.2% |
| stress | heuristic | protection | 18371 | 4.7% |
| stress | heuristic | operand | 47348 | 12.2% |
| stress | heuristic | add1 | 7253 | 1.9% |
| stress | heuristic | add2 | 6001 | 1.5% |
| stress | heuristic | tree | 15760 | 4.1% |
| stress | heuristic | merge | 7880 | 2.0% |
| stress | heuristic | writeback | 46160 | 11.9% |
| stress | heuristic | drain | 16671 | 4.3% |
| stress | heuristic | memory_wait | 89893 | 23.1% |
| stress | heuristic | control | 23337 | 6.0% |

[operation-mix.csv](data/exact-pbqp/operation-mix.csv) separates R1 min-plus/argmin batches, R2 three-input reductions and branch conditioning vector-add, with elements and logical bytes. [batches.csv](data/exact-pbqp/batches.csv) retains batch sizes, opcode counts, phase cycles and host staging/build/readback costs. Physical descriptor/operand/result bytes and requests, child barriers and active/tail lane slots remain in workloads.csv. Counter partitions are checked exactly; matrix views remain asymmetric/strided and dimensions runtime-sized.

### Generic operation mix for matched exact solves

| Class | Seed | Phase | Semantic operation | Batches | Elements | Logical bytes | L2 cycles |
| --- | --- | --- | --- | --- | --- | --- | --- |
| llvm | heuristic | r2 | MAP3_REDUCE | 693 | 1115328 | 5872816 | 2837891 |
| llvm | heuristic | r1 | MINPLUS_PROJECT | 434 | 73771 | 628400 | 288596 |
| llvm | none | r2 | MAP3_REDUCE | 10714 | 24251297 | 125445700 | 62057110 |
| llvm | none | r1 | MINPLUS_PROJECT | 7844 | 1510800 | 12963800 | 6160485 |
| llvm | none | conditioning | SLICE_ACCUMULATE | 58504 | 708754 | 8505048 | 6124885 |
| synthetic | none | conditioning | SLICE_ACCUMULATE | 2898 | 10028 | 120336 | 158930 |
| synthetic | none | r2 | MAP3_REDUCE | 2569 | 135800 | 1102752 | 703941 |
| synthetic | none | r1 | MINPLUS_PROJECT | 466 | 6040 | 61344 | 55594 |
| synthetic | heuristic | conditioning | SLICE_ACCUMULATE | 2304 | 7832 | 93984 | 125792 |
| synthetic | heuristic | r2 | MAP3_REDUCE | 2295 | 119720 | 973280 | 623275 |
| synthetic | heuristic | r1 | MINPLUS_PROJECT | 312 | 3888 | 39616 | 36416 |
| stress | none | conditioning | SLICE_ACCUMULATE | 3525 | 10593 | 127116 | 186897 |
| stress | none | r2 | MAP3_REDUCE | 617 | 20141 | 178468 | 141269 |
| stress | none | r1 | MINPLUS_PROJECT | 617 | 6119 | 64256 | 66090 |
| stress | heuristic | conditioning | SLICE_ACCUMULATE | 3495 | 10491 | 125892 | 185259 |
| stress | heuristic | r2 | MAP3_REDUCE | 607 | 19723 | 174908 | 138691 |
| stress | heuristic | r1 | MINPLUS_PROJECT | 607 | 6001 | 63040 | 64926 |

Semantic operation names describe software work. Actual primitive descriptors/opcodes are retained separately in batches.csv and workloads.csv; R1/R2 callbacks need reconstruction argmins. No PROJECT_ACCUMULATE or independent ARGMIN_VECTOR phase occurs in these completed branch solves.

| Class | Seed | Physical requests | Operand bytes | Shared reread bytes | Reread/operand | Lane occupancy |
| --- | --- | --- | --- | --- | --- | --- |
| llvm | none | 13590541 | 139868292 | 10677328 | 7.6% | 97.1% |
| llvm | heuristic | 556837 | 6249320 | 370680 | 5.9% | 98.2% |
| synthetic | none | 196334 | 951520 | 0 | 0.0% | 95.8% |
| synthetic | heuristic | 167394 | 819840 | 0 | 0.0% | 95.5% |
| stress | none | 91103 | 263212 | 0 | 0.0% | 79.8% |
| stress | heuristic | 89893 | 258836 | 0 | 0.0% | 79.6% |

Shared reread bytes measure repeated shared-input reads inside the existing tiled primitive implementation. They establish a concrete reuse opportunity there, but not a cache hit rate across changing search snapshots.

## Answers to the architectural questions

1. **Kernels or control?** The offload shares and Amdahl ceilings above make search/state management the primary constraint on matched LLVM solves. Exact work is not exclusively cost algebra; cloning, topology, branch selection and proof remain on the CPU.
2. **Intensity versus heuristic?** Exact search repeats algebra but also snapshots, bounds and conditioning. The paired native exact/heuristic time ratios above measure increased solve work, not increased arithmetic per transferred byte. The paired native kernel-share table and physical traffic above distinguish repeated work from useful offload. Historical heuristic comparison uses its own archived revision.
3. **Cloning cost?** Unseeded LLVM cloning takes 12.426 ms, 16.3% of instrumented phase time. The complete Clone/Allocation rows in phase-summary.csv quantify it; complete-capacity snapshot bytes and allocated/peak bytes are per input. Fast copying and recycling reduce avoidable work, but graph state is still copied at every branch. Instrumentation prevents claiming these phase shares as unbiased native fractions.
4. **Lower bound?** LowerBound phase time and scanned unary/matrix elements are retained independently of kernels. Its unseeded LLVM share of instrumented phase time is 9.0%. It remains software work, with negative costs and INF covered by exact-oracle tests.
5. **Conditioning?** It uses existing vector cost-add in the matched paths, with independent Conditioning element/byte/cycle accounting. It changes the software mapping, not ISA semantics. Pruned children still pay their conditioning and reduction work before their bound is evaluated.
6. **Offload fraction?** The kernel shares in the end-to-end table are calibrated diagnostic estimates against uninstrumented native totals. Current ISA cannot replace the remaining search/state/proof work. Outliers and timing perturbations are available per input.
7. **Amdahl?** The table gives ideal matched-host ceilings. The measured current host path is a separate, less favorable scenario; its instrumentation and staging are implementation costs, not immutable architectural constants.
8. **Required clock?** Distributions and 250/500/1000/2000 MHz win fractions are above. Zero-device-work cases have no hardware speedup. None of these hypothetical frequencies is a synthesized or measured clock.
9. **Predictors?** Exploratory log-space correlations below use completed unseeded pairs only. Completion censoring and small generated cohorts prevent causal claims. Matrix/kernel work and host-control share are more directly relevant than input N alone; the bound and incumbent can radically change the tree on the same input.
10. **MAS bottleneck?** On matched unseeded LLVM solves the largest categories are operand (25.6%), memory_wait (18.3%), tree (17.8%). The cycle table separates frontend, operand service, arithmetic, reduction, writeback, memory waits and control. Small ordered callbacks and host preparation are additional end-to-end constraints. [counterfactuals.csv](data/exact-pbqp/counterfactuals.csv) removes individual cycle categories optimistically; it is not a redesigned MAS or proof that bandwidth alone fixes performance.
11. **REDUCE_MIN?** No opcode is added or recommended from this experiment. A LowerBound percentage from intrusive profiling is insufficient by itself: a useful generic min primitive would need enough measured bound work to repay dispatch, operand transfer and host combination. Bounds scan changing state and include the objective offset.
12. **Operand retention?** Measured shared reread bytes quantify operand reuse within tiled primitives, so retention could remove that traffic. Input cells and logical work also show repeated processing across branches, but do not measure valid cross-snapshot residency or cache hit rate. Changed unary/edge data require invalidation. Retention remains a hypothesis requiring an address/version reuse experiment, not a conclusion from total transferred bytes.
13. **Case for generic PCAA?** Exact PBQP adds substantial software proof/state work and does not establish a broad end-to-end advantage at practical hypothetical clocks. Any wins in the tables are workload- and clock-dependent derived scenarios. Heuristic seeding is valuable independently of hardware. This evidence favors improving integration granularity and measuring other generic cost-algebra workloads before changing ISA/MAS.

| Class | Predictor | Pairs | Log Pearson r |
| --- | --- | --- | --- |
| llvm | matrix_elements | 189 | 0.631 |
| llvm | work_r2 | 189 | 0.440 |
| llvm | work_conditioning | 189 | 0.412 |
| llvm | work_clone_bytes | 189 | 0.408 |
| synthetic | work_depth | 18 | 0.817 |
| synthetic | matrix_elements | 18 | -0.559 |
| synthetic | domain | 18 | -0.521 |
| synthetic | work_visited | 18 | 0.371 |
| stress | nodes | 16 | 0.903 |
| stress | work_depth | 16 | 0.888 |
| stress | work_clone_bytes | 16 | 0.747 |
| stress | work_visited | 16 | 0.732 |

## Reproduction and verification

```sh
cmake -S . -B build-cpu-release -DSPIKE_SOURCE_DIR=../riscv-isa-sim \
  -DCMAKE_BUILD_TYPE=Release -DPCAA_CPU_AVX2=ON
cmake --build build-cpu-release -j 6
ruby scripts/exact_pbqp_measure.rb --build build-cpu-release \
  --output /tmp/exact-pbqp-new --limit 512
ruby scripts/exact_pbqp_report.rb --data /tmp/exact-pbqp-new
ruby scripts/exact_pbqp_report.rb --verify
ruby scripts/exact_pbqp_publish.rb --verify
ruby scripts/exact_pbqp_stack.rb
```

Reproduction needs the archived measured source overlay and dependencies; CPU
wall times are environment-sensitive. The collector refuses an existing archive.
It never resumes samples with changed execution code. A preliminary generator
wrote invalid multiline stress edges; that trial was discarded and all final
inputs were regenerated with the supported line-oriented format.

Both Debug and Release passed all 48 CTest entries, including 480 small exact
oracle combinations and 31 CLI regressions. All five required RV64 ELFs passed
Spike. The conservative complete verification stack bound is **568,480 bytes**
against a **1,048,576-byte** reserve, including 65 copies of all identified
recursive frames and every emitted nonrecursive frame. Raw stack reports,
compile commands and test/Spike logs are archived. Historical CPU and L2 data
remain separate and were verified without relabeling.

The offline verifier checks all input/source/archive hashes, retained statuses,
CPU/oracle/seed objectives, assignments, search-work/stream agreement and L2
cycle/request/byte/lane partitions, and byte-identical table regeneration. It
validates retained evidence, not physical silicon timing, repeatability of wall
time, exhaustive optimality for every large input, or unimplemented overlap.
README, AGENTS and architecture were reviewed: tools/user commands and durable
C++ rules are updated; the observable ISA/MAS contracts in doc/arch.md remain
unchanged. [Raw archive guide](data/exact-pbqp/README.md) lists its evidence files.
