# RN solver characterization report

The checked-in `rn_characterization` target measures the public local runner,
not a topology-only approximation. It generates ten synthetic 20-node binary
random-sparse graphs (edge probability 20%, costs -5..5), using seeds
1001--1010, then runs `REDUCE_ONLY`, exhaustive `EXACT_BRANCH_REDUCE`, and all
three `HEURISTIC_RN` policies. The CSV is written to
`build/rn-characterization.csv` and is reproducible with:

```sh
cmake --build build --target rn_characterization
```

These are synthetic graphs, not register-allocation traces.

Every one of the ten graphs reaches an irreducible core under `REDUCE_ONLY`.
The first core contains 9--20 nodes (mean 14.4) and 16--52 edges. Thus RN is
not an occasional boundary case in this corpus. Minimum-degree RN performs 81
steps in total (8.1 per graph); maximum-degree performs 36 (3.6 per graph).
The former unlocks 13 R0, 29 R1, and 77 R2 reductions across the corpus,
showing that RN is commonly followed by an exact-reduction cascade rather than
only by further RN steps.

The exhaustive strategy finishes all ten graphs without a search-limit hit. It
visits 3,044,306 search nodes and creates 2,892,780 branches after its initial
3 R0, 19 R1, and 34 R2 reductions. It is therefore a useful small-instance
oracle, but clearly creates much more software search than heuristic RN.

The heuristic quality result is deliberately reported as a gap, not as a
claim of correctness. Minimum-degree and minimum-work each have mean absolute
gap 11.0 and maximum gap 30; maximum-degree has mean gap 12.2 and maximum gap
28. None reaches the exact optimum in these ten asymmetric instances.
Maximum-degree does less RN/projection work, but does not improve solution
quality in this corpus. Ties in the binary domain make minimum-work identical
to minimum-degree here.

RN scoring is entirely generic min-plus projection: minimum-degree produces
247 projections, represented today by 494 scalar reductions. They read 7,904
bytes and write 3,952 bytes of intermediate minima/argmins; score accumulation
touches 494 elements. Conditioning commits another 494 vector elements
(3,952 logical bytes). A structural `MINPLUS_MATVEC` would use one descriptor
per projection, halving the scalar projection descriptor count; a fused
projection-accumulate operation would also avoid the intermediate 3,952-byte
result stream. These are generic cost-algebra candidates, not proposed
PBQP-specific commands.

At four modeled lanes, minimum-degree RN consumes 7,695 L1 service cycles
across the ten graphs: 1,213 descriptor, 4,056 operand-read, 860 compute, and
2,426 result-write cycles, with 860 primitive descriptors in 353 batches.
This is an architectural estimate only; it excludes host search time and is
not RTL timing. The current evidence supports keeping search, policy choice,
and batching in software while continuing to evaluate generic projection and
vector-accumulation support on larger synthetic families.
