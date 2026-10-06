#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Generates the human-facing exact PBQP report from checked measurement tables.
require_relative 'exact_pbqp_report'
require 'time'
data = ExactReport::ROOT.join('doc/reports/data/exact-pbqp')
summary = JSON.parse(data.join('summary.json').read)
meta = JSON.parse(data.join('metadata.json').read)
rows = CSV.read(data.join('workloads.csv'), headers: true).map(&:to_h)
profiles = CSV.read(data.join('phase-summary.csv'), headers: true).map(&:to_h)
batches = CSV.read(data.join('batches.csv'), headers: true).map(&:to_h)
incumbents = CSV.read(data.join('incumbents.csv'), headers: true).map(&:to_h)
controls = CSV.read(data.join('controls.csv'), headers: true).map(&:to_h)
predictors = CSV.read(data.join('predictors.csv'), headers: true).map(&:to_h)
def fmt(n, digits=3)
  ExactReport.number(n.nil? ? nil : n.to_f, digits)
end
def med(values)
  values.empty? ? nil : ExactReport.median(values.map(&:to_f))
end
def pct(n,d)
  d.to_f.positive? ? fmt(n.to_f/d.to_f*100,1)+'%' : '—'
end
def table(columns,values)
  ExactReport.table(columns,values)
end
matched = rows.select { |r|r['l2_cycles'] }
llvm = summary.fetch('cohorts').find { |c|c['class']=='llvm' && c['seed']=='none' }
seeded = summary.fetch('cohorts').find { |c|c['class']=='llvm' && c['seed']=='heuristic' }
phase_group=profiles.select { |r|r["class"]=="llvm" && r["seed"]=="none" }
phase_total=phase_group.sum { |r|r["diagnostic_ns"].to_f }
clone_ns=phase_group.select { |r|r["phase"]=="clone" }.sum { |r|r["diagnostic_ns"].to_f }
clone_share=pct(clone_ns,phase_total)
lower_share=pct(phase_group.select { |r|r["phase"]=="lower_bound" }.sum { |r|r["diagnostic_ns"].to_f },phase_total)
l2_group=matched.select { |r|r["class"]=="llvm" && r["seed"]=="none" }
l2_total=l2_group.sum { |r|r["l2_cycles"].to_f }
l2_rank=ExactReport::PHASES.map { |p|[p,l2_group.sum { |r|r["l2_#{p}_cycles"].to_f }] }.sort_by { |_,v|-v }.first(3).map { |p,v|"#{p} (#{pct(v,l2_total)})" }.join(", ")
text = <<~MD
# Exact PBQP: strong CPU versus current L2

## Result and scope

This is a bounded characterization of **#{summary['input_count']} inputs**, including all
**491 retained LLVM graphs**, 24 degree-3/degree-4/mixed synthetic graphs and
18 complete-graph stress cases. ISA 1.0.0 and MAS 1.0.0 are unchanged.
The common solver runs exact branch-and-reduce, with the same branch order,
sign-agnostic bound, arithmetic and workspace policy on CPU and L2.

The strongest aggregate native candidate is **#{summary['strong_variant']}**.
On the matched unseeded LLVM subset, current ISA kernels account for
**#{pct(llvm['kernel_ns'],llvm['cpu_ns'])}** of native time; even free kernels
would give only **#{fmt(llvm['ideal_amdahl'])}×** aggregate speedup.
The weighted ideal device break-even is **#{fmt(llvm['weighted_ideal_mhz'])} MHz**.
With a cheap heuristic incumbent it is **#{fmt(seeded['weighted_ideal_mhz'])} MHz**,
with a **#{fmt(seeded['ideal_amdahl'])}×** ideal Amdahl ceiling.
These are derived clock scenarios, not measured hardware performance.

Completion censoring matters: unseeded LLVM exact solves complete on
**#{llvm['cpu_status'].fetch('solved-exact',0)}/491** inputs and seeded solves on
**#{seeded['cpu_status'].fetch('solved-exact',0)}/491**. Hardware comparisons include only
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

All runs are Release, pinned to CPU #{meta['options']['cpu']}, with a search-node limit
of #{meta['options']['limit']}, a 512 MiB live workspace limit and subprocess ceilings
of #{meta['options']['cpu_timeout']} s native / #{meta['options']['model_timeout']} s model.
Workspace failures are capacity/errors, separate from node-limit hits. The LLVM capacity/error rows in this archive are explicit 512 MiB workspace exhaustion, not malformed corpus inputs. Failed
and timed-out runs do not acquire invented completion counters. Model runs are
attempted only after the AVX2 native run completes; three fresh model processes
must agree. Warmup counters are explicitly subtracted.

The machine, compiler, complete flags and SystemC version are in
[metadata](data/exact-pbqp/metadata.json), [compilation commands](data/exact-pbqp/compile_commands.json)
and [CMake cache](data/exact-pbqp/CMakeCache.txt). Measurements started
#{meta['started_utc']} and finished #{meta['finished_utc']}. The source snapshot
is based on commit `#{meta['base_commit']}`, with all measured source and binary
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

MD
# Include compact generated tables, leaving the long phase and operation tables in the archive.
text << data.join('tables.md').read.split("## Completion (strong CPU)",2).last.split("## Exclusive diagnostic",2).first.prepend("### Completion (strong CPU)")
text << "\n## Native phase and algorithmic evidence\n\n"
phase_values=[]
%w[llvm synthetic stress].product(%w[none heuristic]).each do |klass,seed|
  group=profiles.select { |r|r['class']==klass && r['seed']==seed }
  total=group.sum { |r|r['diagnostic_ns'].to_f }
  group.sort_by { |r|-r['diagnostic_ns'].to_f }.first(6).each do |r|
    phase_values << [klass,seed,r['phase'],fmt(r['diagnostic_ns'].to_f/1e6),pct(r['diagnostic_ns'],total)]
  end
end
text << "Top exclusive diagnostic phases, including all completed native solves (not just L2 matches):\n\n"
text << table(%w[Class Seed Phase ms Diagnostic-share],phase_values)
text << "\nPerturbation is substantial on short solves. The detailed values and per-input ratios in [profiles.csv](data/exact-pbqp/profiles.csv) and [workloads.csv](data/exact-pbqp/workloads.csv) describe instrumented execution; they do not establish unbiased percentages of uninstrumented time.\n\n"
common=rows.select { |r|r['seed']=='none' && %w[avx2 portable structured].all? { |v|r["#{v}_ns"] } }
text << "### CPU candidates on the common unseeded completion subset\n\n"
text << table(['Candidate','Inputs','Aggregate native ms'],%w[avx2 portable structured].map { |v|[v,common.length,fmt(common.sum { |r|r["#{v}_ns"].to_f }/1e6)] })
text << "\nThe portable executable has no AVX2 build flag. AVX2 is not universally beneficial; structured layout wins this exact workload set. Original/degree controls below cover the first 20 LLVM inputs and every generated input, so their completed subset is much smaller and cannot establish full-corpus optimization speedup.\n\n"
control_values=%w[current degree].map do |v|
  group=controls.select { |r|r['variant']==v && r['speedup'] }
  [v,group.length,fmt(med(group.map { |r|r['speedup'] })),fmt(group.sum { |r|r['cpu_ns'].to_f }/[group.sum { |r|r['strong_ns'].to_f },1].max)]
end
text << table(['Original control','Matched samples','Median strong speedup','Aggregate strong speedup'],control_values)
inc_values=%w[llvm synthetic stress].map do |klass|
  group=incumbents.select { |r|r['class']==klass && r['algorithmic_speedup'] }
  [klass,group.length,fmt(med(group.map { |r|r['algorithmic_speedup'] })),fmt(med(group.map { |r|r['visited_reduction'] })),fmt(med(group.map { |r|r['exact_vs_heuristic_time_ratio'] }.compact))]
end
text << "\nHeuristic initialization is an algorithmic comparison, including seed time:\n\n"
text << table(['Class','Both exact complete','Median seed speedup','Median visited reduction','Median unseeded exact / heuristic time'],inc_values)
text << "\nFull witnesses, work histograms, allocation/clone bytes, pruning counts and first/found/proof times remain in the raw records and [incumbents.csv](data/exact-pbqp/incumbents.csv). Seeded and unseeded completion subsets differ; their aggregate clocks must not be interpreted as a paired hardware improvement.\n\n"
text << "### Native exact versus heuristic, same completed unseeded inputs\n\n"
comparison=%w[llvm synthetic stress].map do |klass|
  group=rows.select { |r|r['class']==klass && r['seed']=='none' && r['cpu_ns'] && r['heuristic_cpu_ns'] }
  [klass,group.length,pct(group.sum { |r|r['kernel_ns'].to_f },group.sum { |r|r['cpu_ns'].to_f }),
   pct(group.sum { |r|r['heuristic_kernel_ns'].to_f },group.sum { |r|r['heuristic_cpu_ns'].to_f })]
end
text << table(['Class','Paired inputs','Exact kernel share','Heuristic kernel share'],comparison)
text << "\nThese callback shares compare the same current native implementation. Repeated solve work is not arithmetic intensity: physical traffic per modeled active element is reported separately below.\n\n"
text << "## Modeled L2 accounting\n\n"
l2_values=[]
%w[llvm synthetic stress].product(%w[none heuristic]).each do |klass,seed|
  group=matched.select { |r|r['class']==klass && r['seed']==seed }
  cycles=group.sum { |r|r['l2_cycles'].to_f }
  ExactReport::PHASES.each do |phase|
    value=group.sum { |r|r["l2_#{phase}_cycles"].to_f }
    l2_values << [klass,seed,phase,value.to_i,pct(value,cycles)] if value.positive?
  end
end
text << table(['Class','Seed','L2 phase','Cycles','Share'],l2_values)
text << "\n[operation-mix.csv](data/exact-pbqp/operation-mix.csv) separates R1 min-plus/argmin batches, R2 three-input reductions and branch conditioning vector-add, with elements and logical bytes. [batches.csv](data/exact-pbqp/batches.csv) retains batch sizes, opcode counts, phase cycles and host staging/build/readback costs. Physical descriptor/operand/result bytes and requests, child barriers and active/tail lane slots remain in workloads.csv. Counter partitions are checked exactly; matrix views remain asymmetric/strided and dimensions runtime-sized.\n\n"
mix=CSV.read(data.join('operation-mix.csv'),headers:true).map(&:to_h)
text << "### Generic operation mix for matched exact solves\n\n"
text << table(['Class','Seed','Phase','Semantic operation','Batches','Elements','Logical bytes','L2 cycles'],mix.map do |r|
  semantic={'min2_batch'=>'MINPLUS_PROJECT','map3_project_batch'=>'MAP3_REDUCE','add'=>'SLICE_ACCUMULATE'}.fetch(r['kind'],r['kind'])
  [r['class'],r['seed'],r['phase'],semantic,r['batches'],r['elements'],r['logical_operand_bytes'].to_i+r['logical_result_bytes'].to_i,r['cycles']]
end)
text << "\nSemantic operation names describe software work. Actual primitive descriptors/opcodes are retained separately in batches.csv and workloads.csv; R1/R2 callbacks need reconstruction argmins. No PROJECT_ACCUMULATE or independent ARGMIN_VECTOR phase occurs in these completed branch solves.\n\n"
traffic=%w[llvm synthetic stress].product(%w[none heuristic]).map do |klass,seed|
  group=matched.select { |r|r['class']==klass && r['seed']==seed }
  operand=group.sum { |r|r['l2_operand_bytes'].to_f };reread=group.sum { |r|r['l2_shared_reread_bytes'].to_f }
  slots=group.sum { |r|r['l2_lane_slots'].to_f };active=group.sum { |r|r['l2_active_elements'].to_f }
  [klass,seed,group.sum { |r|r['l2_requests'].to_i },operand.to_i,reread.to_i,pct(reread,operand),pct(active,slots)]
end
text << table(['Class','Seed','Physical requests','Operand bytes','Shared reread bytes','Reread/operand','Lane occupancy'],traffic)
text << "\nShared reread bytes measure repeated shared-input reads inside the existing tiled primitive implementation. They establish a concrete reuse opportunity there, but not a cache hit rate across changing search snapshots.\n\n"
text << "## Answers to the architectural questions\n\n"
text << <<~MD
1. **Kernels or control?** The offload shares and Amdahl ceilings above make search/state management the primary constraint on matched LLVM solves. Exact work is not exclusively cost algebra; cloning, topology, branch selection and proof remain on the CPU.
2. **Intensity versus heuristic?** Exact search repeats algebra but also snapshots, bounds and conditioning. The paired native exact/heuristic time ratios above measure increased solve work, not increased arithmetic per transferred byte. The paired native kernel-share table and physical traffic above distinguish repeated work from useful offload. Historical heuristic comparison uses its own archived revision.
3. **Cloning cost?** Unseeded LLVM cloning takes #{fmt(clone_ns/1e6)} ms, #{clone_share} of instrumented phase time. The complete Clone/Allocation rows in phase-summary.csv quantify it; complete-capacity snapshot bytes and allocated/peak bytes are per input. Fast copying and recycling reduce avoidable work, but graph state is still copied at every branch. Instrumentation prevents claiming these phase shares as unbiased native fractions.
4. **Lower bound?** LowerBound phase time and scanned unary/matrix elements are retained independently of kernels. Its unseeded LLVM share of instrumented phase time is #{lower_share}. It remains software work, with negative costs and INF covered by exact-oracle tests.
5. **Conditioning?** It uses existing vector cost-add in the matched paths, with independent Conditioning element/byte/cycle accounting. It changes the software mapping, not ISA semantics. Pruned children still pay their conditioning and reduction work before their bound is evaluated.
6. **Offload fraction?** The kernel shares in the end-to-end table are calibrated diagnostic estimates against uninstrumented native totals. Current ISA cannot replace the remaining search/state/proof work. Outliers and timing perturbations are available per input.
7. **Amdahl?** The table gives ideal matched-host ceilings. The measured current host path is a separate, less favorable scenario; its instrumentation and staging are implementation costs, not immutable architectural constants.
8. **Required clock?** Distributions and 250/500/1000/2000 MHz win fractions are above. Zero-device-work cases have no hardware speedup. None of these hypothetical frequencies is a synthesized or measured clock.
9. **Predictors?** Exploratory log-space correlations below use completed unseeded pairs only. Completion censoring and small generated cohorts prevent causal claims. Matrix/kernel work and host-control share are more directly relevant than input N alone; the bound and incumbent can radically change the tree on the same input.
10. **MAS bottleneck?** On matched unseeded LLVM solves the largest categories are #{l2_rank}. The cycle table separates frontend, operand service, arithmetic, reduction, writeback, memory waits and control. Small ordered callbacks and host preparation are additional end-to-end constraints. [counterfactuals.csv](data/exact-pbqp/counterfactuals.csv) removes individual cycle categories optimistically; it is not a redesigned MAS or proof that bandwidth alone fixes performance.
11. **REDUCE_MIN?** No opcode is added or recommended from this experiment. A LowerBound percentage from intrusive profiling is insufficient by itself: a useful generic min primitive would need enough measured bound work to repay dispatch, operand transfer and host combination. Bounds scan changing state and include the objective offset.
12. **Operand retention?** Measured shared reread bytes quantify operand reuse within tiled primitives, so retention could remove that traffic. Input cells and logical work also show repeated processing across branches, but do not measure valid cross-snapshot residency or cache hit rate. Changed unary/edge data require invalidation. Retention remains a hypothesis requiring an address/version reuse experiment, not a conclusion from total transferred bytes.
13. **Case for generic PCAA?** Exact PBQP adds substantial software proof/state work and does not establish a broad end-to-end advantage at practical hypothetical clocks. Any wins in the tables are workload- and clock-dependent derived scenarios. Heuristic seeding is valuable independently of hardware. This evidence favors improving integration granularity and measuring other generic cost-algebra workloads before changing ISA/MAS.

MD
pred_values=%w[llvm synthetic stress].flat_map do |klass|
  predictors.select { |r|r['class']==klass && r['log_pearson_r'] }.sort_by { |r|-r['log_pearson_r'].to_f.abs }.first(4).map { |r|[klass,r['predictor'],r['completed_pairs'],fmt(r['log_pearson_r'])] }
end
text << table(['Class','Predictor','Pairs','Log Pearson r'],pred_values)
text << <<~MD

## Reproduction and verification

```sh
cmake -S . -B build-cpu-release -DSPIKE_SOURCE_DIR=../riscv-isa-sim \\
  -DCMAKE_BUILD_TYPE=Release -DPCAA_CPU_AVX2=ON
cmake --build build-cpu-release -j 6
ruby scripts/exact_pbqp_measure.rb --build build-cpu-release \\
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
MD
path = ExactReport::ROOT.join('doc/reports/exact-pbqp-cpu-l2-breakdown.md')
if ARGV == ['--verify']
  raise 'report drift' unless path.read == text
  puts 'verified human-facing exact PBQP report'
elsif ARGV.empty?
  path.write(text)
  puts 'generated exact PBQP report'
else
  abort 'usage: exact_pbqp_publish.rb [--verify]'
end
