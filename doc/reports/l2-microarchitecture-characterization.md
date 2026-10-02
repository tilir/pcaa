# L2 microarchitecture characterization

MAS **1.0.0**, semantic ISA **1.0.0**, pcaalib **2.0.0**. This measures the
structural SystemC execution model, not hardware throughput or synthesized RTL.
The implemented baseline is serial: one context, one engine, one total memory
credit, one pending output, no phase overlap or inter-child operand retention.
L0 and analytical L1 remain separate, with their existing behavior.

## Revision and reproducibility

Base checkout: `a47b0a3e01b67adc887024c5adc8c4aecb079cd2`. Measurements include uncommitted L2
implementation changes, pinned by the complete measured-source overlay in
[data/l2/implementation.tar.gz](data/l2/implementation.tar.gz) and per-file
SHA-256 values in [metadata.json](data/l2/metadata.json). The exact implementation
fingerprint (SHA-256 of base commit concatenated with sorted source-hash JSON) is
`c82aabd535af54d8412fb15c0c20c125ed0c3e0a787139cbff875eea16129846`.
The overlay contains execution, integration, test, benchmark, CMake and
measurement-script sources. Restore it over that base checkout to reproduce
this implementation without relying on a later moving branch. The source
snapshot is an identification mechanism, not a claim that changes were committed.
It retains the original Python measurement driver as historical provenance.
Current automation is `scripts/l2_characterize.rb` and `scripts/l2_report.rb`,
using Ruby's standard library. Use these current scripts to orchestrate the
restored engine; the archived source fingerprint and original commands stay
unchanged. Ruby regeneration matches the archived summary, tables and CSVs.

Host tools: GCC 15.2.0, SystemC 3.0.2, C++17, Release measurements. The nominal
cycle is 1 ns; counts do not specify a realizable clock. `LANES=4`,
`MEM_BYTES=16`, `T_m=8`, `T_n=16`, memory response latency 1, acceptance delay 0.
An accepted beat consumes one issue cycle, one response wait cycle and one
retirement cycle. Reads/writes access `MemoryInterface` at response; write
visibility precedes acknowledgement and terminal status. No bus padding is
transferred: requested payload and transferred bytes coincide on successful
runs. Physical requests split at beat boundaries. This is a deterministic
port abstraction, not AXI/cache/DRAM timing.

Each header fetch completes before body fetch; decode costs one cycle after
each. Protection scans cost one cycle per addressed output element. Adds cost
one cycle/pass, a four-lane registered binary tree two cycles, merge one.
Tree depth follows `ceil(log2(LANES))` in the lane sweep. Operand banks and
states are bounded by configuration, not graph/command sizes. Controller
initialization, batch transitions and drain consume explicit cycles.

```sh
cmake -S . -B build -DSPIKE_SOURCE_DIR=../riscv-isa-sim -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 8
cmake --build build --target format
ctest --test-dir build --output-on-failure
cmake -S . -B build-release -DSPIKE_SOURCE_DIR=../riscv-isa-sim -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 8
ctest --test-dir build-release --output-on-failure
ruby scripts/l2_characterize.rb --build build-release --output doc/reports/data/l2
ruby scripts/l2_report.rb
ruby scripts/l2_report.rb --verify
ruby scripts/verify_report_data.rb
```

`pcaa_graph_run_l2 --solver local [--l2-lanes N --l2-mem-bytes N --l2-tm N
--l2-tn N --l2-memory-latency N] INPUT` prints an `l2 ` JSON line. The direct
`pcaa_l2_microbench` accepts the same options and emits JSONL. Generic pcaalib
never calls `sc_start`; hosted ModelKernel owns polling and advancement.
Polling in 64-cycle windows may advance idle simulation time; device statistics
exclude that idle tail and software staging/search.

## Verification before measurement

Debug and Release each passed **38/38 CTest tests**, including 13 L2 tests,
independent microbenchmark smoke and end-to-end characterization smoke.
`l2_unit` performs 4,380 shape/format/layout differential cases through real
MMIO against L0, plus focused arithmetic, alias, status, batch and failure tests.
Configurations include 1, 3 and 8 lanes with non-power-of-two beat/chunk sizes,
delayed acceptance and response. Lengths include 1/2/3/7/8/15/16/17/31/32/63/64;
output shapes straddle 8 and 16; successful results and all written memory
match L0. Maximum n and maximum m of 65,535 are tested separately; maximum
u16 strides and near-u64 overflow are covered without maximum-matrix allocation.

Coverage includes negative costs, saturation, invalid beside INF, both ADD3
underflows, third-operand checking after INF, first/global ties, all-INF,
unit/strided/unaligned/padded views, exact alias with either/both sources,
invalid partial/general-format alias, mixed 32/48/64 streams, nested/truncated/
trailing/count-mismatched batches, descriptors in output padding, producer/
consumer children, split write and batch-record faults, recovery, BUSY rejection
and a stable submission snapshot. Error tests compare terminal status and
specified batch counts, not unspecified partial output bytes or L0 access order.
One-credit ownership, lane/byte/request/phase partitions and scheduled projection
rereads are independently checked by the archive verifier. There is no formal
liveness proof or RTL equivalence claim.

The existing `basic`, `randomized`, `pbqp_basic`, `pbqp_randomized`, `pbqp_rn`
ELFs were rebuilt and each run under Spike with
`--extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000`.
All returned zero; command/exit evidence is in [verification.json](data/l2/verification.json).
They verify the retained L0 RV64 path; L2 is a hosted integration.

Measurements contain **50,688 independently verified direct-command rows**,
**1,083 workload/configuration rows**, **33 distinct configurations**, and no
failures or time-limited rows. A complete repeat after final state cleanup
reproduced both datasets byte-for-byte after decompression. Every PBQP assignment/solution label matches a
separate current L1/L0 solve. Baseline covers all **491 LLVM graphs**, four
examples and 12 synthetic 20-node graphs: degree-3/degree-4/mixed-degree,
domains 2/4/8/16, seed 1001, generated by the existing generator.
Broad sweeps use the same 18 inputs: four examples, six synthetic graphs
(domains 4/16), and eight evenly spaced ranks after sorting the complete real
corpus by `(nodes, filename)`, including both endpoints. Exact filenames are
in metadata. This deterministic sample is not a universal workload weighting.
Direct commands use n=1/2/3/4/7/8/15/16/17/31/32/63/64/129/256/1024 and
m=1/2/4/7/8/9/15/16/17/31/32, plus ordered batches of 1/4/16/128 children.

## Full LLVM baseline: where cycles go

Total L2 elapsed service: **43,019,109 cycles**. The categories below are a
partition; response waits are counted only in `memory_wait`.

| Phase | Cycles | Share |
| --- | --- | --- |
| descriptor | 1,378,750 | 3.20% |
| decode | 381,788 | 0.89% |
| protection | 2,156,204 | 5.01% |
| operand | 10,437,188 | 24.26% |
| add1 | 2,877,811 | 6.69% |
| add2 | 1,049,958 | 2.44% |
| tree | 7,082,866 | 16.46% |
| merge | 3,541,433 | 8.23% |
| writeback | 4,343,134 | 10.10% |
| drain | 206,257 | 0.48% |
| memory_wait | 8,079,536 | 18.78% |
| control | 1,484,184 | 3.45% |

The issue/response portions of descriptor, operand and result service plus
memory waits total **24,238,608 cycles (56.34%)**. Adds account for **9.13%**,
and reduction tree/state merge for **24.69%**. High lane occupancy therefore
does not imply an arithmetic-only workload. Seven graphs issue no accelerator
primitives; their zero cycles reflect software-only solving, not missing rows.

| Opcode / work | Primitives or batches | Attributed cycles | Share |
| --- | --- | --- | --- |
| 3 | 16,182 | 872,305 | 2.03% |
| 6 | 67,335 | 6,122,790 | 14.23% |
| 7 | 67,335 | 24,624,499 | 57.24% |
| 8 | 24,679 | 10,436,220 | 24.26% |
| Batch parent/record | 15,363 | 184,356 | 0.43% |
| Unattributed header/control | — | 778,939 | 1.81% |

Attribution begins after header identification; header/control cycles without
an opcode are explicitly unattributed. Batch parents are not primitive commands.
Projection opcodes 7/8 account for 81.50% of elapsed cycles under this convention.

## Lane utilization, memory and protection

| Memory role | Payload and transferred bytes | Physical requests |
| --- | --- | --- |
| Descriptor | 7,975,696 | 689,375 |
| Operand | 70,192,140 | 5,218,594 |
| Result | 9,930,544 | 2,171,567 |
| Total | 88,098,380 | 8,079,536 |

The 14,662,770 active arithmetic elements occupy 15,711,076 issued lane slots:
**93.33% utilization**, **6.67% tail slots** (1,048,306). Both ADD3 passes and
opcode-8 shared pre-add issues contribute; this is issued-slot occupancy,
not utilization over all elapsed device cycles. There are 3,927,769 lane-group
issues and 3,541,433 persistent-state updates.

Header/body requests are **190,894 / 498,481**. Header service alone is
572,682 cycles; two-stage decode is 381,788. Entire descriptor service with
response waits and decode is **2,449,913 cycles (5.70%)**. Compared with L1's
498,481 descriptor cycles, header/body serialization, acknowledgements and
per-request service matter more than descriptor byte footprint alone.

Batch preflight scans **2,156,204 elements**, costing **5.01%** of total.
The 175,531 successful child barriers retire before another child's header;
all drain/barrier work costs 206,257 cycles (0.48%). Completion records are
included in result bytes and request counts. Output acknowledgements are
significant: result service costs 6,514,701 cycles (15.14%), despite only
11.27% of transferred bytes. Results are acknowledged one element at a time.

Shared inputs are loaded 204,649 times. Tile rereads contribute **3,660,648
operand bytes**, 5.22% of actual operand traffic, versus ideal one-read
operand traffic of 66,531,492 bytes. Opcode 7 at m=17,n=31 reads 2,480 bytes,
including 248 reread bytes; opcode 8 reads 2,852, including 496 reread bytes.
No full shared vector survives output tiles.

Host PBQP staging packs affine views into contiguous guest storage; therefore
this corpus has **zero physical gather costs** despite original graph/solver
views being strided. There are 1,605,469 contiguous chunk fills and 1,765,936
segments crossing beats. These count different things and must not be added.
The corpus is evidence for its actual staged streams; direct commands below
isolate hardware gather cost. Logical operation mix is retained per row and
in full verbose diagnostics, separately from requested/physical bytes.

## Direct primitive shapes and striding

For m=17,n=31, layout 0 is row-major contiguous, 1 gathers shared vectors and
matrix columns, 2 has padded rows, and 3 is a transposed affine matrix.

| Opcode | Layout | Cycles | Requests | Gathered costs | Operand bytes |
| --- | --- | --- | --- | --- | --- |
| 7 | 0 | 1168 | 201 | 0 | 2480 |
| 7 | 1 | 2488 | 641 | 620 | 2480 |
| 7 | 2 | 1108 | 181 | 0 | 2480 |
| 7 | 3 | 2281 | 572 | 527 | 2480 |
| 8 | 0 | 1267 | 226 | 0 | 2852 |
| 8 | 1 | 2794 | 735 | 713 | 2852 |
| 8 | 2 | 1207 | 206 | 0 | 2852 |
| 8 | 3 | 2380 | 597 | 527 | 2852 |

Gathering increases opcode-7 requests from 201 to 641 and cycles from 1,168
to 2,488, without changing payload. Padded rows can be faster here because
n+5=36 aligns successive row bases to the 16-byte beat; padding itself is
never transferred. The transposed view gathers 527 matrix costs. Stride and
beat alignment, not just total bytes, control request count.
Exact in-place vector add at n=17 takes 100 cycles versus 103 for separate
output: the three-cycle difference is its shorter descriptor, not forwarding.
Short/long batches retain all child memory traffic and add real preflight,
record and barrier work; full rows are in the generated shape tables.

The following compares actual memory service (three cycles/request) with
ADD/tree/merge cycles for contiguous opcode 7. Neither includes controller
work in its category.

| n | m | Total cycles | Memory service cycles | Datapath cycles |
| --- | --- | --- | --- | --- |
| 1 | 1 | 30 | 21 | 4 |
| 17 | 9 | 415 | 222 | 180 |
| 31 | 17 | 1168 | 603 | 544 |
| 256 | 32 | 15248 | 7020 | 8192 |
| 1024 | 32 | 60560 | 27756 | 32768 |

At short shapes memory/frontend dominate. At long columns and enough rows
for shared reuse (e.g. n=256,m=32), reduction/datapath exceeds memory service.
A single output row rereads no shared tile but has less amortization, so even
n=1024,m=1 remains memory dominated. This is a shape-dependent crossover,
not a claim that all large commands are compute bound.

## Controlled parameter sweeps

One-factor sweeps are deduplicated with the focused `LANES × MEM_BYTES`
grid: 33 configurations rather than a large Cartesian product. All ratios
below use total cycles on the same deterministic 18-input application sample.

| Varied parameter | Values | Elapsed cycles / baseline |
| --- | --- | --- |
| lanes | 1, 2, 4, 8, 16 | 1.3022, 1.1642, 1.0000, 0.8754, 0.8450 |
| mem_bytes | 4, 8, 16, 32 | 1.9603, 1.3008, 1.0000, 0.8488 |
| tm | 1, 2, 4, 8, 16, 32 | 1.3120, 1.1382, 1.0472, 1.0000, 0.9801, 0.9796 |
| tn | 4, 8, 16, 32, 64 | 1.0792, 1.0131, 1.0000, 0.9993, 0.9993 |
| memory_latency | 1, 2, 4, 8, 16 | 1.0000, 1.1880, 1.5639, 2.3157, 3.8193 |

Widening lanes from 4 to 8 saves 12.46%; doubling again saves only 3.48%
relative to eight lanes. Sixteen-byte memory with 16 lanes remains constrained
by unchanged memory requests and increased tree depth for short groups.
Increasing T_m from 8 to 16 saves 1.99%, with little further sample benefit
at 32; shared traffic is useful but not the dominant baseline cost.
T_n beyond 16 saves less than 0.1% on this sample while increasing storage.
Widening memory to 32 saves 15.12%; narrower beats impose a large penalty.
At response latency 16, cycles rise to 3.8193× baseline, exposing serial-credit
sensitivity independently of lane/tile parameters.

Focused two-factor ratios (relative to four lanes / 16-byte baseline):

| LANES / MEM_BYTES | 4 | 8 | 16 | 32 |
| --- | --- | --- | --- | --- |
| 1 | 2.2626 | 1.6030 | 1.3022 | 1.1510 |
| 2 | 2.1245 | 1.4650 | 1.1642 | 1.0130 |
| 4 | 1.9603 | 1.3008 | 1.0000 | 0.8488 |
| 8 | 1.8358 | 1.1762 | 0.8754 | 0.7242 |
| 16 | 1.8053 | 1.1458 | 0.8450 | 0.6938 |

At four-byte width, changing four to sixteen lanes reduces cycles only 7.91%;
at 32 bytes it reduces them 18.27%. Arithmetic width pays off more after
memory service is reduced. The combined 16-lane/32-byte point is 0.6938×
baseline on this sample; it is a configuration measurement, not area/energy
optimization or hardware speedup.

Direct opcode-7 m × T_m grid, n=31 (cycles):

| m / T_m | 1 | 2 | 4 | 8 | 16 | 32 |
| --- | --- | --- | --- | --- | --- | --- |
| 7 | 631 | 559 | 511 | 487 | 487 | 487 |
| 8 | 718 | 622 | 574 | 550 | 550 | 550 |
| 9 | 802 | 706 | 658 | 634 | 610 | 610 |
| 16 | 1420 | 1228 | 1132 | 1084 | 1060 | 1060 |
| 17 | 1504 | 1312 | 1216 | 1168 | 1144 | 1120 |
| 32 | 2824 | 2440 | 2248 | 2152 | 2104 | 2080 |

Direct opcode-7 n × T_n grid, m=17 (cycles):

| n / T_n | 4 | 8 | 16 | 32 | 64 |
| --- | --- | --- | --- | --- | --- |
| 15 | 728 | 656 | 620 | 620 | 620 |
| 16 | 596 | 596 | 596 | 596 | 596 |
| 17 | 868 | 796 | 760 | 724 | 724 |
| 31 | 1384 | 1240 | 1168 | 1132 | 1132 |
| 32 | 1108 | 1108 | 1108 | 1108 | 1108 |
| 63 | 2696 | 2408 | 2264 | 2192 | 2156 |
| 64 | 2132 | 2132 | 2132 | 2132 | 2132 |
| 129 | 5460 | 4884 | 4596 | 4452 | 4380 |

Increasing T_m removes real rereads across output tiles. Increasing T_n below
useful beat/group lengths changes request segmentation and tails; beyond n
it adds no reuse. Larger n buffers do not introduce overlap in this MAS.

## L1 versus L2 on the same current stream

| Input | L1 cycles | L2 cycles | L2 / L1 |
| --- | --- | --- | --- |
| triangle | 28 | 190 | 6.786 |
| petersen | 179 | 1241 | 6.933 |
| chvatal | 271 | 1895 | 6.993 |
| random-20 | 413 | 2875 | 6.961 |

Full corpus: **5,435,397 L1 cycles**, **43,019,109 L2 cycles**, ratio **7.915**.
This compares models of different fidelity, not measured hardware slowdown.
L1 uses fixed four-lane streaming formulas, even in an L2 parameter sweep.
The baseline discrepancy of 37,583,712 cycles decomposes exactly as follows:

| Difference from L1 | Additional cycles |
| --- | ---: |
| Descriptor request service versus L1 descriptor estimate | 1,569,644 |
| Operand request service versus L1 ideal shared byte rate | 11,468,097 |
| ADD passes, tree and merge versus L1 compute | 10,769,801 |
| Per-element result acknowledgements versus L1 summed byte rate | 5,853,169 |
| Decode, protection, drain and other controller work | 4,228,433 |
| Removal of L1 read/compute overlap | 3,694,568 |
| Total discrepancy | 37,583,712 |

Operand difference includes tile rereads, maximal beat-limited spans,
short chunks, alignment and serial response lifetime. Original graph strides
were packed by this host backend; direct gathered shapes demonstrate the
additional penalty that un-packed streams would incur. Datapath difference
includes two ADD3 passes (shared first add for opcode 8) and 10,624,299
explicit tree/merge cycles that L1 does not model at baseline latency.
No L1 estimate is used to advance L2 simulation.

## Next MAS experiment and limitations

The strongest first experiment is **bounded operand double buffering with
fill/compute phase overlap in projections**, while retaining one context,
software-owned batches and the existing ISA. Baseline memory service consumes
56.34%, while tree/merge/add work consumes 33.82%; both are currently serialized.
Long contiguous projection shapes show substantial concurrent-work opportunity,
and increasing lanes alone reaches diminishing returns. Add explicit bank
ownership and reduction-state hazard tracking, then measure actual overlap
and stalls against this retained baseline. No overlap mechanism is implemented
in this revision.

A separate follow-up should investigate additional memory credits under
increased response latency, because the latency sweep is strongly sensitive
to the single accepted request. Current results cannot predict its pipelined
bandwidth or arbitrate between credits and overlap: that requires a new MAS
experiment. Larger T_m is a low-complexity tuning candidate but gives only
about 2% on the deterministic application sample; a larger T_n is weakly
justified there. Wider memory is beneficial but results remain element-sized
and individually acknowledged, so a wider port cannot eliminate every cost.

The memory abstraction has no cache, DRAM banking or AXI framing. There is no
CPU packing/search/MMIO latency, wall-time speedup, synthesis frequency, area
or power measurement. Workload sweeps are a deterministic subset; full-corpus
results are baseline only. Fixed synthetic seed/domain shapes do not establish
universality. Error outputs are unspecified; delayed acceptance is tested but
not swept. MAS and ISA versions remain unchanged: implementation found no
contract contradiction requiring a specification bug fix.

## Retained raw evidence

[data/l2/](data/l2/) contains raw JSONL, flattened CSVs, source overlay, input
hashes, generated synthetic inputs, command/exit logs and generated tables.
`microbench.jsonl.gz` and `workloads.jsonl.gz` are complete verified rows;
`runs.jsonl.gz` and `workload-runs.jsonl.gz` preserve full subprocess outputs.
Failures/timeouts would retain explicit status and missing counters rather
than zero; this dataset has none. [tables.md](data/l2/tables.md) and
[summary.json](data/l2/summary.json) are regenerated by `scripts/l2_report.rb`.
The offline verifier checks source snapshot/input hashes, all phase/request/
byte partitions, bounded queues, exact scheduled payloads and sweep coverage.
The archive is pinned by [SHA256SUMS](data/SHA256SUMS).
