# Native CPU baseline versus structural L2

This study compares the same PBQP solve policy on a native CPU with the current
PCAA command stream. Device performance means **modeled service cycles**, not
SystemC wall time. ISA 1.0.0, compact encoding, MAS 1.0.0 and production solver
policy are unchanged. The experiment is host-only; it does not enlarge RV64
arenas or add an accelerator opcode.

## Conclusion

The strongest measured full-corpus implementation is **degree-cached dense
CPU**, at **89.099 ms** for one solve of each graph, compared with **5,161.659 ms**
for the existing solver: a **57.93× aggregate improvement** without changing
policy or assignments. Degree caching alone accounts for a 52.18× improvement.
The large original bottleneck is repeated whole-edge-array degree scanning,
not accelerator arithmetic.

Every executed projection in this corpus has a checked compact representation,
including those modified by R2. Structure-aware evaluation removes **99.965%**
of the logical matrix candidate additions. It wins warmed kernel comparisons,
but its **93.656 ms** whole-corpus time is 5.1% slower than dense CPU because
classification/cache management is significant at these small shapes. There
is no measured generic-dense projection residue in this workload revision.

Current L2 needs **43,019,109 service cycles** versus L1's **5,435,397**,
a newly reproduced **7.915×** ratio. Dividing by the strong CPU's corresponding
**18.540 ms of callbacks**, rather than its entire solve, gives an ideal
kernel-only break-even of **2,320 MHz** for L2 and **293 MHz** for L1. These are
required frequencies under model assumptions, not achievable-clock claims.
Offloading every batch loses even at 2 GHz with zero added latency. Current
hosted preparation alone takes **57.410 ms**, exceeding the available CPU
kernel budget even at infinite device frequency.

At 2 GHz and ideal preparation, a trained selective policy improves held-out
modeled solves by only **0.223 ms / 0.33%**; with measured preparation it chooses
CPU for every batch. This corpus does not justify progressing the current
offload-all boundary to RTL on performance grounds. A cheaper preparation path,
larger/different domains or a different workload needs fresh evidence; the
present result does not establish an accelerator benefit.

## Native solve timing and original bottleneck

Aggregate time is the sum of each graph's median, not an average of speedups.
Percentiles below rank the 491 graph medians. Kernel estimates come from
separate callback-timing passes.

| Implementation | Corpus solve ms | Kernel ms | Median µs | p90 µs | p99 µs | Maximum µs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Existing production scalar | 5,161.659 | 33.882 | 43.016 | 473.803 | 234,702.191 | 2,001,516.887 |
| Degree-cached scalar | 98.929 | 28.881 | 36.125 | 156.827 | 6,454.316 | 18,344.866 |
| Degree-cached dense | 89.099 | 18.540 | 21.409 | 112.666 | 6,146.645 | 19,549.999 |
| Degree-cached structured | 93.656 | 23.125 | 21.033 | 133.785 | 6,450.395 | 20,094.903 |

The profiling build with original scans takes 5,228.243 ms without enabling
scope timing, 1.29% above the linked production baseline. Its exclusive timed
profile attributes 5,172.567 ms, **96.883%**, to topology; node selection adds
67.464 ms (1.264%), reduction control 58.103 ms (1.088%), and kernels 34.316 ms
(0.643%). This independently supports the degree-scan diagnosis before the
optimization.

For dense CPU, the less intrusive decomposition is **18.540 ms callbacks
(20.8%)** plus approximately **70.559 ms other solve work (79.2%)**. The largest
ten graphs contribute **71.018%** of aggregate dense solve time. The most
expensive, `167-...-visitsub-n895-d17.pbqp`, takes 19.550 ms, of which callbacks
are only 1.391 ms. The top ten and every individual graph are in
[graphs.csv](data/cpu-baseline/graphs.csv) and
[the generated tail table](data/cpu-baseline/tables.md).

Table A deliberately labels the finer decomposition as an **intrusive profile**.
Its totals exceed uninstrumented solve times; clock overhead must not be
mistaken for useful work. Dense and structured columns each have their own
denominator. Native CPU constructs no device descriptors and does no staging.

| Component | Dense diagnostic ms | Share | Structured diagnostic ms | Share |
| --- | ---: | ---: | ---: | ---: |
| Topology + selection | 107.480 | 57.23% | 108.367 | 55.31% |
| Reduction control | 54.985 | 29.28% | 54.963 | 28.05% |
| Numerical kernels | 19.235 | 10.24% | 10.004 | 5.11% |
| Structured representation management | 0 | 0% | 16.506 | 8.42% |
| Reconstruction outside callbacks | 0.068 | 0.04% | 0.083 | 0.04% |
| Packing / descriptors | 0 | 0% | 0 | 0% |
| Other | 6.031 | 3.21% | 6.007 | 3.07% |

Median within-graph p90/p10 solve-time ratios are 1.145 for existing code,
1.144 for cached scalar, 1.201 for dense and 1.183 for structured. Small timing
differences should be interpreted with that noise. Three dense graph callback
estimates exceed their separate solve medians slightly; clamping their control
residuals adds only 10 µs to the aggregate residual used in selective modeling.

## Warmed kernels and dynamic structure

Table B gives median nanoseconds per warmed call: fifteen repetitions of
2,000 calls after 200 warmups. `PROJECT` is value projection; `MAP3` adds the
second vector in the exact checked order. Row exceptions include finite nonzero
backgrounds. Transposed views retain their column stride. All measurements,
including tails, generic matrices and the out-of-corpus 49-choice stress case,
are in [kernels.csv](data/cpu-baseline/kernels.csv).

| Kernel / shape / layout / form | Scalar ns | Dense ns | Structured ns | Dense / structured |
| --- | ---: | ---: | ---: | ---: |
| PROJECT 7×7, contiguous, matching | 79.3 | 76.8 | 30.0 | 2.56× |
| PROJECT 7×16, contiguous, matching | 163.0 | 115.2 | 39.5 | 2.91× |
| PROJECT 16×16, contiguous, matching | 353.1 | 215.6 | 58.1 | 3.71× |
| MAP3 16×16, contiguous, matching | 622.5 | 237.9 | 80.9 | 2.94× |
| PROJECT 17×17, contiguous, matching | 403.3 | 234.5 | 61.3 | 3.82× |
| PROJECT 16×16, contiguous, row exceptions | 334.6 | 215.7 | 104.4 | 2.07× |
| MAP3 16×16, contiguous, row exceptions | 602.9 | 247.6 | 96.6 | 2.56× |
| PROJECT 16×16, transposed, matching | 370.0 | 294.2 | 58.8 | 5.00× |
| PROJECT 16×16, transposed, row exceptions | 333.6 | 299.5 | 72.5 | 4.13× |
| PROJECT 16×16, transposed, generic | 346.2 | 320.5 | 312.8 | 1.02× |

The contiguous 16×16 matching structured call corresponds to **4.41 billion
logical elements/s**, versus 1.19 billion for dense. Its 19.8 GB/s logical byte
equivalent is **not physical bandwidth**: the compact algorithm does not read
the dense matrix again. These microbenchmarks reuse validated metadata; whole
solves pay for classification and invalidation, explaining the different
application result.

Table C separates arithmetic class from origin. Projection timer shares are
from separate structured instrumentation, including classification on cache
misses and excluding shared MAP3 pre-add. They are not fractions of the dense
solve time. Logical bytes count the original views, not actual memory transfers.

| Matrix class | Calls | Logical m×n | Projection timer ms | Timer share | Logical bytes | R2-modified calls |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Forbidden matching | 93,071 | 13,126,297 | 23.857 | 99.402% | 61,459,484 | 1,755 |
| One forbidden per row only | 0 | 0 | 0 | 0% | 0 | 0 |
| Row background + ≤2 exceptions | 389 | 50,930 | 0.143 | 0.598% | 238,612 | 389 |
| Generic dense | 0 | 0 | 0 | 0% | 0 | 0 |

Of these, **91,316** calls / **12,897,626** elements use initial matching
matrices. After reductions, **2,144** calls / **279,601** elements remain
structured: 1,755 retain matching and 389 need row exceptions. The latter have
finite/negative backgrounds rather than being labelled dense merely because
they are nonzero. R2-modified projection timers sum to approximately 0.567 ms,
including 0.143 ms for row exceptions. Origin-group medians in the generated
table are independently aggregated, so sums of their medians need not equal
medians of sums.

Structured evaluation performs only **4,659** checked matrix candidate additions
for **13,177,227** logical matrix elements. Matching rows use minima/index tests
instead of adding zero or `INF`. MAP3 still needs **357,122** shared vector
pre-adds; first-use predicates inspect **9,531,561** matrix elements. Therefore
99.965% fewer candidate additions does not mean 99.965% less CPU time or bytes.
The observed residual expense is representation validation/management and
software graph control, rather than a large generic dense projection tail.

## CPU versus current L1 and L2

Table D uses the fixed dense CPU variant. Percentile rows are individual graphs
at the indicated **CPU-kernel-time rank**, not percentiles of ratios; all 491
graphs are ranked, including seven with no device work. The projection-heavy
tail is the largest 50 graphs by executed logical projection elements.

| Workload | CPU kernel µs | CPU solve µs | L1 cycles | L2 cycles | L2/L1 | L1 break-even MHz | L2 break-even MHz |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Entire corpus | 18,539.589 | 89,098.721 | 5,435,397 | 43,019,109 | 7.915 | 293.178 | 2,320.392 |
| Kernel p50 rank | 14.526 | 21.809 | 4,508 | 33,314 | 7.390 | 310.340 | 2,293.405 |
| Kernel p90 rank | 62.264 | 142.840 | 17,625 | 141,590 | 8.033 | 283.069 | 2,274.027 |
| Kernel p99 rank | 639.571 | 6,378.801 | 172,078 | 1,413,721 | 8.216 | 269.052 | 2,210.421 |
| Projection-heavy top 10% | 11,041.201 | 76,479.929 | 3,138,391 | 25,477,612 | 8.118 | 284.244 | 2,307.504 |
| Ten most expensive solves | 6,451.440 | 63,276.249 | 1,736,961 | 14,309,520 | 8.238 | 269.236 | 2,218.035 |

The streams contain **175,531 primitive children** in **15,363 submissions**.
No timing is attributed to SystemC wall-clock simulation. Using the original
CPU kernels would give a much weaker 1,270 MHz ideal L2 requirement; dense CPU
raises it to 2,320 MHz. Using structured's larger measured callback budget
would give 1,860 MHz, but that implementation also has larger total solve time.
All denominators are retained, rather than choosing a favourable one.

The new L2/L1 ratio reproduces the earlier approximately 7.9× finding from the
current code/stream. It is not copied from the historical report. The exact
**37,583,712-cycle** difference partitions as follows; memory waits are included
in request lifetime rather than counted again:

| L2 minus L1 category | Added cycles |
| --- | ---: |
| Descriptor service | 1,569,644 |
| Operand requests / reuse | 11,468,097 |
| ADD / tree / merge | 10,769,801 |
| Result acknowledgements | 5,853,169 |
| Decode / protection / drain / control | 4,228,433 |
| Removed L1 read/compute overlap | 3,694,568 |

Operand service is 24.262% of L2 cycles, memory waits 18.781%, reduction trees
16.464%, writeback 10.096%, and merge 8.232%; the full suspended-phase partition
is in generated tables. The CPU graph optimization changes total application
time by 57.93×; the L1→L2 change alters modeled device work by 7.915×. These
are different scopes and cannot be multiplied into an application speedup.

## End-to-end sensitivity

Table E gives the remaining budget after `C_L2/f` for the entire corpus.
The per-submission allowance assumes zero per-solve visibility latency; the
per-solve allowance assumes zero fixed submission latency. Negative values
mean the device service already exceeds CPU callbacks, even before preparation.

| Device MHz | Ideal additional budget ms/corpus | ns/submission | ns/solve | Budget with measured preparation ms/corpus |
| --- | ---: | ---: | ---: | ---: |
| 250 | −153.537 | −9,993.937 | −312,702.336 | −210.947 |
| 500 | −67.499 | −4,393.584 | −137,471.749 | −124.909 |
| 1,000 | −24.480 | −1,593.408 | −49,856.456 | −81.889 |
| 2,000 | −2.970 | −193.319 | −6,048.810 | −60.380 |

The measured 57.410 ms hosted preparation consists of 23.896 ms staging,
13.486 ms command-build/submit, 1.448 ms readback, and 18.579 ms remaining host
callback work. These observations include the current model-side bookkeeping
and measurement overhead. They expose avoidable implementation cost; they
are not a prediction of a physical driver. Even replacing them with zero
does not let offload-all win at any frequency in Table E.

| Fixed submission ns | Visibility ns/solve | Ideal required MHz | With current measured preparation |
| --- | ---: | ---: | --- |
| 0 | 0 | 2,320.392 | No finite frequency |
| 50 | 0 | 2,420.688 | No finite frequency |
| 100 | 0 | 2,530.046 | No finite frequency |
| 100 | 1,000 | 2,605.278 | No finite frequency |
| 500 | 0 | 3,961.941 | No finite frequency |
| 1,000 | 0 | 13,542.548 | No finite frequency |

[sensitivity.csv](data/cpu-baseline/sensitivity.csv) and
[required-frequency.csv](data/cpu-baseline/required-frequency.csv) retain the
full submission/visibility surface. No unsupported physical overhead value is
presented as a measured or realistic hardware latency.

## Selective offload

The training split has 164 graphs; 327 are held out. At 2 GHz with zero fixed
overhead and ideal preparation, the fitted decision takes these forms for
logical batch work `E` (nanoseconds versus cycles/2):

* R1 `min2_batch`: `73.843 + 0.776E > 34.128 + 1.829E`, approximately `E < 37.7`.
* R2 `map3_project_batch`: `596.504 + 0.983E > 99.681 + 1.260E`, approximately `E < 1,795`.
* RN `project_add_batch`: `283.599 + 0.927E > 245.747 + 1.272E`, approximately `E < 109.5`.

This is not a universal minimum-size threshold: at 2 GHz modeled L2 grows
faster with dense logical work than the measured CPU, so the learned profitable
region is often **below** a crossover. The intercept favours the device in some
small batches, while noisy sub-microsecond CPU estimates cause false positives.
With measured preparation, these fitted regions disappear. Coefficients and
100 ns fixed-overhead alternatives are retained in the archive.

Table F uses only held-out graphs and zero fixed submission/visibility cost.
CPU columns show **remaining kernel time**, not all control work. Modeled total
adds the same held-out control residual. Eight GHz is a sensitivity point,
not a proposed or demonstrated device clock.

| Policy / preparation | MHz | Remaining CPU kernel ms | L2 cycles | Submissions | Modeled solve ms | False offloads |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| CPU only | 2,000 | 12.417 | 0 | 0 | 68.459 | 0 |
| All batches / ideal | 2,000 | 0 | 28,719,032 | 10,211 | 70.401 | 8,291 |
| Estimated threshold / ideal | 2,000 | 10.348 | 3,694,987 | 1,323 | 68.236 | 442 |
| Oracle upper bound / ideal | 2,000 | 9.118 | 5,278,839 | 1,920 | 67.798 | 0 |
| All batches / measured | 2,000 | 0 | 28,719,032 | 10,211 | 109.868 | 10,210 |
| Estimated threshold / measured | 2,000 | 12.417 | 0 | 0 | 68.459 | 0 |
| All batches / ideal | 8,000 | 0 | 28,719,032 | 10,211 | 59.631 | 0 |
| Estimated threshold / measured | 8,000 | 12.417 | 0 | 0 | 68.459 | 0 |

At 2 GHz, even the unattainable per-batch ideal oracle saves only 0.661 ms,
**0.97%** of held-out solve time. The trained policy saves 0.33%, with 442 of
1,323 selected batches actually losing under the model. The current preparation
oracle selects only one batch at 2 GHz and nine at 8 GHz. This is a small
conditional opportunity, not evidence of a useful broad offload boundary.

## Environment and measurement scope

The measurement revision is base commit
`2f7d5b2bca0cc88769b196c3c6bba833db332378` plus the checksummed
[implementation overlay](data/cpu-baseline/implementation.tar.gz). It includes
all **491 retained LLVM graphs**, archived independently of future repository
edits. Metadata was written before timing, with compiler, CPU, binary hashes,
source/input hashes, CMake cache and compilation database. See
[metadata.json](data/cpu-baseline/metadata.json).

The host is an Intel Core Ultra X7 358H, 16 reported cores/threads, under the
Microsoft WSL2 hypervisor. Each benchmark process is pinned to CPU 0. GCC
15.2.0 builds C++17 Release code with `-O3 -DNDEBUG`; `-mavx2` is enabled only
for the experimental kernel library, without `-march=native`. AVX2 is present;
AVX-512 is absent. SystemC is 3.0.2. Native benchmark executables have ordinary
`main` entry points and do not link SystemC. No build or test job ran alongside
the final measurements. This is a virtualized host measurement, not a controlled
silicon-frequency or energy experiment.

Every variant uses `HEURISTIC_RN`, min-degree RN selection, per-node batching,
and hosted input-sized capacities. Graph parsing, construction, cloning/reset,
assignment allocation and output serialization are outside the solve interval.
Two warmups precede eleven timed solves per graph/variant. Reported solve times
are medians, with raw repetitions and p10/p90 retained. Lazy structured-matrix
classification and invalidation are inside the solve; matrix metadata is reset
between solves, while reusable arithmetic scratch retains its capacity.

Five additional passes time callback batches; another five collect exclusive
topology/selection/reduction/reconstruction profiles. Structured runs also have
five passes collecting matrix-class and shape evidence. These passes are
separate from end-to-end measurements. Each batch's median timer-pair floor is
subtracted and clamped at zero. Intrusive scope profiles are diagnostic: nested
clock reads disproportionately inflate short topology operations. They must
not be presented as an unbiased partition of an uninstrumented solve.

Each graph has one L1 and three independently launched L2 runs. The L2 baseline
configuration is the existing default in [MAS](../mas.md), including four lanes
and a three-cycle request lifetime. The same assignments, objective, reduction
counts, callback kinds and batch sizes are checked across CPU and model runs.
Repeated L2 cycle counts and phase partitions must match. The measured native
kernel denominator covers the corresponding complete callback stream, including
projection, addition, reconstruction argmin and small scalar batches; it does
not use total solve time as an accelerator budget.

## CPU implementations

`current_original` links the production native solver and software kernel.
`current` uses an isolated profiling build with the original complete edge-array
degree scans. This separation exposes profiling-build overhead before treating
any apparent improvement as an optimization.

`scalar` caches active degree per node, updating it on edge retirement and fill
creation. It retains the production scalar cost kernel. Reducible-node/RN
selection order, incident-edge order, fill-slot reuse and reconstruction remain
identical. Cached degree answers avoid scanning the complete edge array when
only a count is needed; scanning still supplies actual incident edges.

`dense` adds inlined checked arithmetic, AVX2 contiguous vector addition with
scalar tails and strided fallback, and reusable scratch. MAP3 computes the
shared `cost_add(unary, fixed)` vector once, then adds each matrix element.
This preserves `cost_add(cost_add(a,b),c)` exactly; reassociation would be
incorrect near saturation. Matrix reductions retain stable first-index ties.

`structured` adds checked, oriented matrix representations cached by base,
dimensions and strides. R2 changes and edge-slot reuse invalidate every cached
orientation of that base. R1 supplies an explicit matrix view to the kernel;
the implementation never infers a matrix by subtracting unrelated pointers.
Unknown scalar job lists safely use the generic path.

The dense and structured variants are both retained. One **fixed** strong
variant is selected by aggregate full-corpus solve time, rather than choosing
the fastest implementation separately for each graph. Structured arithmetic
elimination alone is not evidence of an application speedup.

## Structured predicate and arithmetic

Initial matching matrices contain `0/INF`, with at most one `INF` per row and
per column. A second class requires only the per-row condition. For either,
find the first two global minima of the input vector ordered by `(cost,index)`;
each row selects the first candidate outside its forbidden index. This replaces
logical dense `m*n` arithmetic by `O(n+m)` after classification.

The more general representation stores a constant background `b_i` per row
and at most **two arbitrary exceptions** `(j,e_ij)`. Background and exceptions
may be finite, negative or `INF`. A full predicate pass reconstructs every
matrix entry before this fast path is used. The first three global input minima
suffice to find the best index outside two exceptions. Evaluate that candidate
with `b_i`, evaluate each exception separately and choose the first-index minimum.
Thus evaluation is `O(n+m)` with bounded row metadata, even when R2 produces
finite nonzero matrices. First-use validation still reads `m*n` entries; it is
charged to the solve and is not claimed to disappear.

For a valid fixed background, checked addition is monotone in the input.
Consequently the smallest allowed input minimizes the background group. A
negative-underflow error anywhere in that group is also detected at its minimum.
All vector values and every matrix background/exception are validated; `INF`
is a value, never an error sentinel. Exception candidates preserve error
propagation. If all candidates saturate to `INF`, index zero remains the first
minimum, including when a skipped background candidate would tie. MAP3 first
validates and pre-adds its two vectors in the required order.

The direct tests compare against the independent production scalar callbacks
on 4,000 random rectangular/strided matrices, both inside and outside the
predicates. They include unequal dimensions, negative values, ties, all-`INF`,
zero rows/columns, saturation, invalid costs, underflow and MAP3 ordering.
Predicate reconstruction, cache invalidation, SIMD aliases/tails and unrelated
scalar jobs have separate checks. A nontrivial irreducible graph checks solver
policy and assignment preservation across all levels and degree modes.

## Preparation and hypothetical offload

For a fixed same-policy solve, use

```
T_sw  = T_control + T_kernel_cpu
T_acc = T_control + T_pack + T_build + T_submission + T_visibility + C_L2 / f
f_ideal = C_L2 / T_kernel_cpu
```

`T_control` is the uninstrumented solve median minus the corrected callback
estimate. This residual includes topology, selection, reduction bookkeeping,
reconstruction outside callbacks and native representation management. It is
an estimate from separate passes; tiny negative residuals are clamped and counted
in `summary.json`. No acceleration is assigned to software control.

The L2 callback profile separately measures staged data copies, command
construction/submit and output readback. It excludes the L2 wait, polling and
`sc_start` window. Its remaining `host_ns` includes allocations, current model
memory access, cycle-projection bookkeeping and timer overhead. It measures the
current hosted implementation, not unavoidable hardware MMIO or cache-coherence
cost. L1 executes during submission, so L1 host timing is not used as packing
evidence. Unknown physical submission and visibility latencies remain parameters.

Selective offload keeps each existing ordered callback batch intact. Every
third graph in filename order trains nonnegative affine models per callback
kind for CPU nanoseconds, L2 cycles and hosted preparation versus logical
elements; the remaining graphs are held out. The decision compares estimated
CPU cost with estimated device plus chosen overhead cost. It does not choose
using the measured held-out execution time. A separately labelled oracle gives
an unattainable upper bound; false-positive offloads are reported. This is an
economic what-if, not a new runtime policy or a hardware implementation.

## Test coverage and contract review

The final host suite passes **46/46 CTest entries** in both Debug and Release.
The expanded tools checks include six graph-reader GoogleTests (with eighteen
malformed-input cases), six CPU-kernel GoogleTests, four SystemC profile tests,
four native solver CLI variants and **56** model-runner CLI checks. They cover
typed graph-construction failures, move ownership, malformed/truncated inputs,
rectangular/oriented matrices, callback aliases, device cycle partitions,
recording enabled/disabled, range-failure recovery and CLI configuration errors.
The portable non-AVX2 kernel path separately passes its six unit tests.

All five required RV64 ELFs (`basic`, `randomized`, `pbqp_basic`,
`pbqp_randomized`, `pbqp_rn`) pass under Spike. The formatting/IWYU target and
the historical L2 evidence verifier pass. Logs and verification scope are
retained in the archive. README, AGENTS and architecture were explicitly
reviewed: README and the new tools guide document commands, AGENTS records
modern C++ and measurement rules, and `doc/arch.md` needs no contract change.

## Reproduction and verification

Start from the pinned base commit and extract the source overlay into that
checkout, then extract the [final analysis overlay](data/cpu-baseline/analysis.tar.gz).
The original generator is preserved; the final overlay resolves CSV column
names and strengthens correspondence checks without changing measurement code.
The archived inputs are also available independently. Build and collect
new measurements into a **fresh** directory; never resume an old archive with
new execution code.

```sh
cmake -S . -B build-cpu-release -DSPIKE_SOURCE_DIR=../riscv-isa-sim \
  -DCMAKE_BUILD_TYPE=Release -DPCAA_CPU_AVX2=ON
cmake --build build-cpu-release --parallel 8
ruby scripts/cpu_baseline_measure.rb --build build-cpu-release --output /tmp/cpu-study
ruby scripts/cpu_baseline_report.rb --data /tmp/cpu-study
ruby scripts/cpu_baseline_report.rb --data /tmp/cpu-study --verify
```

To regenerate and verify the retained evidence without running timings again:

```sh
ruby scripts/cpu_baseline_report.rb
ruby scripts/cpu_baseline_report.rb --verify
```

The [archive inventory](data/cpu-baseline/README.md) explains raw files and
verification scope. [Generated tables](data/cpu-baseline/tables.md) and CSVs
contain the complete graph, batch, shape and sensitivity results. Verification
checks checksums, archived source/input members, raw sample coverage, answers,
stream correspondence, deterministic model counts and byte-identical regenerated
tables. It does not reproduce native wall times or validate a silicon clock.

## Limits

The extracted LLVM graphs are one x86 register-allocation workload with small
domains, not universal PBQP. Warmed microbenchmarks omit classification and
invalidation while whole solves include them. The 49-choice stress shape is
outside this corpus and cannot justify a corpus-wide benefit. Logical elements
and bytes describe eliminated dense work, not retired instructions or physical
memory bandwidth. Matrix-class timers and intrusive scope profiles are separate
observations, not additive pieces of the eleven-sample solve median.

CPU execution is sampled on a virtualized machine without locked frequency or
thermal controls. Sub-microsecond calls approach the clock floor, and subtracting
that floor is an estimate. Repeated samples and dispersion are retained rather
than hiding that uncertainty. L2 is structural simulation with explicit service
cycles; no synthesis, RTL timing, area, energy, bus integration or achievable
clock is established here. Hosted preparation and hypothetical selective offload
must be read under their explicit assumptions.
