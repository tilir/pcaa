# Documentation index

The current implementation uses semantic ISA **1.0.0** and pcaalib **2.0.0**
with compact 32/48/64-byte command streams. Earlier scalar experiments used
56-byte fixed descriptors; the first vector implementation used 80-byte fixed
descriptors before compact encoding replaced it. Start with the contract or API
reference below; historical reports record experiments made with earlier
solver/descriptor versions and are not alternate specifications.

## Current references and implementation notes

For an integrated account of the findings and the decisions they support,
start with [the research synthesis as of 1 October 2026](research-summary-2026-10-01.md).

| Document | Scope |
| --- | --- |
| [Architecture](arch.md) | MMIO, descriptor encoding, operand dimensions/strides, arithmetic, outputs, ordering, and errors |
| [Microarchitecture (MAS 1.0.0)](mas.md) | ISA 1.0.0 implementation specification for future L2/RTL: one engine, tiled projections, bounded storage, memory ordering, and errors |
| [pcaalib](pcaalib.md) | Semantic C API, codec, backends, and typed submission/completion statuses |
| [Model and workload design](design.md) | L0–L3 refinement route, simulation ownership, trace/CSV methodology, and measurement revisions |
| [L1 performance model](l1-performance-model.md) | Current service-cycle formulas, assumptions, and 28-cycle triangle calibration |
| [Workload overview](workload-characterization.md) | Meanings of logical work, descriptors, submissions, staging, and cycles |
| [Bellman–Ford probe](generality-probe.md) | Current software oracle/SystemC coverage and limits of the irregular edge-list mapping |
| [ISA 1.0.0 decision](isa-v1-decision.md) | Adopted primitives and rationale; current encoding links back to the architecture |
| [Compact encoding measurements](reports/compact-encoding-measurements.md) | Example command bytes with current encoding, compared with the previous 80-byte encoding |

## Historical measurement reports

These tables remain the recorded results, including original scalar command
counts, byte assumptions, timing ratios, and verification counts. “Current”
or “today” inside a historical experiment refers to that experiment's
implementation. New runs do not reproduce old tables merely by using the same
script name. The revision below is the last report revision before this
documentation audit, not a claim that all raw CSVs were committed there.

| Report | Report revision | Retained evidence |
| --- | --- | --- |
| [Solver comparison](reports/solver-characterization.md) | `e80c21c` | RN cascades, solver outcomes, and scalar-baseline operation mix |
| [HW/SW boundary](reports/hw-sw-boundary-characterization.md) | `dd1402b` | Logical graph-aware epoch partitions; those device boundaries are hypothetical |
| [Scaling](reports/scaling-characterization.md) | `dd1402b` | N/D/policy and local-search snapshots |
| [LLVM corpus](reports/llvm-corpus-characterization.md) | `fcdb6dd` | Committed extracted graphs, quantization, and earlier solver runs |
| [Branch-and-bound](reports/branch-bound-characterization.md) | `dd1402b` | Implemented pruning rule and its recorded exact-search sample |
| [Fork parallelism](reports/fork-parallelism-characterization.md) | `dd1402b` | Per-fork logical-work proxy, without measured frontier concurrency |
| [Primitive shapes](reports/primitive-shape-study.md) | `fcdb6dd` | Original scalar-to-vector descriptor-count argument; a new section maps current opcodes |
| [RN batch restructuring](reports/batch-restructuring-study.md) | `dd1402b` | Per-node versus per-edge batching with identical scalar children |
| [Vector cycle projection](reports/vector-primitive-cycle-projection.md) | `dd1402b` | Hypothetical vector shapes using 56-byte scalar descriptors and the old 50-cycle triangle |
| [Matrix access patterns](reports/matrix-access-pattern-study.md) | `dd1402b` | Contiguous/strided view frequencies supporting affine strides |
| [Cost representation](reports/cost-representation-study.md) | `dd1402b` | Fixed-point samples and arithmetic-order/FP32 tradeoffs; literature figures are not PCAA synthesis results |

All measurement reports now live under [reports/](reports/README.md).
Recovered raw CSVs, fixed source inputs, independent repeated runs, traces,
reproduction commands and checksums are retained in
[reports/data/](reports/data/README.md). Complete deterministic datasets
were rechecked against their scalar source revisions; the explicit replacement
branch sample and current compact measurements have separate evidence.
Run `ruby scripts/verify_report_data.rb` from the repository root to check
that archive offline. Historical ratios remain estimates under historical
model assumptions, not current hardware measurements.

## Decision inputs and deferred design

[The pre-ISA evidence index](isa-decision-inputs.md) records the questions and
measurements that preceded the adopted ISA decision. Several questions there
were resolved by opcodes 6–8 and affine strides; it is a historical input, not
the current feature list.

[The ISA 2.x local-vector-register proposal](isa-2x-local-vector-rf.md) is a
deferred exploration. Local registers, load/store variants, full-matrix
outputs, segmented reductions, and graph-aware scheduling are not current
commands. L2 and L3 are refinement plans, not implemented models.
