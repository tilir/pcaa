# Measurement reports

Reports below retain their measurement configuration and source revision.
Historical scalar results are independently reproduced against their original
implementation; they are not measurements of today's compact/vector path.
All datasets, input snapshots, checksums, and commands are in
[the evidence inventory](data/README.md). Run the offline checks from the
repository root:

```sh
ruby scripts/verify_report_data.rb
```

| Report | Measurement scope | Recheck |
| --- | --- | --- |
| [Solver comparison](solver-characterization.md) | Scalar solver at `e80c21c` | All 880 result rows match |
| [HW/SW boundary](hw-sw-boundary-characterization.md) | Hypothetical epoch boundaries at `dd1402b` | All 49,522 epoch rows match; 210 source traces retained |
| [Scaling](scaling-characterization.md) | Scalar N/D/policy/local-search sweeps | All 1,128 original rows match, excluding wall times |
| [LLVM corpus](llvm-corpus-characterization.md) | 491 extracted register-allocation graphs | All 491 graphs re-extracted byte-identically; graph metrics and solver rows verified |
| [Branch-and-bound](branch-bound-characterization.md) | 26 synthetic and 39 explicit real inputs | All rerun; 63 complete, two timeout; original unspecified real table replaced |
| [Fork parallelism](fork-parallelism-characterization.md) | 298,434 historical fork rows | Table arithmetic, all 65 heuristic medians and all 63 completed fork sequences verified |
| [Primitive shapes](primitive-shape-study.md) | Scalar descriptor-count argument | Uniform-domain PROJECT/MAP3 identities checked against all 407 sweep rows |
| [RN batching](batch-restructuring-study.md) | Per-node/per-edge scalar children | Both 898-row sweeps and both 671-row timed datasets match |
| [Vector cycle projection](vector-primitive-cycle-projection.md) | Hypothetical shapes, four lanes, 56-byte descriptors | All 671 repeated rows match; 50-cycle triangle rechecked |
| [Matrix access patterns](matrix-access-pattern-study.md) | Contiguous/strided view counts | All 898 source rows match; rounded ratio corrected |
| [Cost representation](cost-representation-study.md) | DIMACS distances, log weights, literature anchors | All 733,846 arcs recomputed; inputs retained; FPGA scope corrected, unsupported area claim removed |
| [L2 microarchitecture](l2-microarchitecture-characterization.md) | Current MAS 1.0.0 execution and controlled sweeps | Full corpus assignments checked against L1/L0; independent microbenchmark oracle and archived cycle partitions |
| [Compact encoding](compact-encoding-measurements.md) | Current 32/48/64-byte encoding | All four examples rerun against `86d76b1`; byte totals and aliases verified |

These rechecks validate functional/logical measurements and model arithmetic.
They do not validate hardware throughput, energy, or the assumptions of
hypothetical epoch/vector architectures. The LLVM instrumentation patch, translation-unit sources, generated IR and
extraction commands are retained with the raw data.
