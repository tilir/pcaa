# PBQP workload characterization

`pbqp_workload` emits deterministic logical traces for the representative
synthetic corpus. The trace records policy, graph family, seed, reduction,
layout, primitive size, packing, and batch metadata. It is not a timing
benchmark and does not contain host addresses.

Run it with:

```sh
cmake --build build --target pbqp_workload
build/pbqp_workload --trace build/pbqp-workload.csv
```

The analyzer faithfully mirrors the production solver's edge-slot insertion,
free-slot reuse, neighbor enumeration, fill-edge orientation, and default
first-index reduction policy. It can also emit degree-priority and
minimum-kernel-work policy traces. Current factual policy and timing summaries
are checked into [the L1 performance-model report](l1-performance-model.md),
whose tables identify the software policy, interface organization, and timing
configuration used for every comparison.

“Register-allocation-like” names a synthetic domain-size/workload shape, not a
trace extracted from LLVM register allocation.
