# Solver characterization

This report describes a reproducible workload study of the shared PBQP solver.
It is deliberately an experiment around the public command-line tools, not a
second implementation of solver logic. Recreate the CSV with:

```sh
cmake --build build --target solver_characterization
```

The target invokes `scripts/rn_characterize.rb`, which invokes only
`pbqp_graph_generate` and `pcaa_graph_run`. The resulting
`build/solver-characterization.csv` records status, objective, reduction and
conditioning work, local-search work, and exact-search work for every run.

## Methods

The general corpus has ten fixed seeds (1001--1010) for each of six 20-node
families: binary random-sparse, binary degree-3, binary degree-4, small-domain
mixed-degree, register-like trees, and register-like cycles. `REDUCE_ONLY`,
three RN policies, `LOCAL_SEARCH`, and each policy's
`HEURISTIC_RN_LOCAL_SEARCH` hybrid are measured on every graph. A separate
eight-node subset of random-sparse, degree-3, degree-4, and mixed-degree
graphs runs both exact solvers with a 100,000-node explicit search limit.
Those exact results are the reference for reported gaps; a missing reference
is left blank rather than guessed.

`EXACT_CORE_ENUMERATION` means: reduce once to a fixed point, then enumerate
the entire residual core. `EXACT_BRANCH_REDUCE` means: at every tree node,
reduce again, branch deterministically on the configured policy, condition a
state, and recur. Both return the same optima on all 40 exact-subset inputs in
this study. The branch solver therefore exercises a distinct reduction tree,
not merely a renamed residual enumerator.

## Results

All 20-node trees and cycles finish under exact low-degree reduction alone.
The 20-node degree-3 and degree-4 graphs are irreducible in all ten seeds;
mixed-degree graphs are irreducible in seven of ten, while random-sparse graphs
are reducible in this deterministic sample. Overall, 63 of the 100
reduce-only runs across the general and exact corpora report `IRREDUCIBLE`.
RN is consequently common for the regular general-graph families, but not a
property of every sparse graph.

On the exact eight-node degree-3 subset, the three-policy mean heuristic gap is
2.0 and the hybrid mean gap is 0.6; their worst gaps are 8 and 4 respectively.
For degree-4 those values are 4.6/23 and 1.3/10. The hybrid never worsens its
own RN starting assignment: it performs deterministic coordinate descent from
the RN result. In this corpus local search reaches the exact objective on the
small subset, but that observation is not an optimality guarantee on larger
graphs. Mixed-degree inputs show a small mean gap (1/6) for both RN and hybrid;
the particular restart schedule did not improve that sample.

The exact runs complete within the configured limit. Their CSV search counters
are full-tree counters: visited reduction states, created branches, maximum
depth, and limit hits. A `SEARCH_LIMIT` result has no claimed optimum, making
large-core use bounded and explicit. Exact branch-and-reduce uses eight static
graph snapshots rather than placing a graph copy on the freestanding stack;
the depth bound is another explicit capacity limit.

## Operation mix

This is the principal workload result. The table reports the mean logical work
per successful 20-node general-corpus run (six families and ten fixed seeds).
`PROJECT` includes both R1 projections and RN scoring projections;
`PROJECT_ACC` is the separate RN score-vector accumulation;
`SLICE` includes conditioning and local-search score construction; `MAP3` is
R2; and `ARGMIN` is the local-search vector choice. `descriptors` counts only
the current primitive descriptors (`PROJECT`, `MAP3`, and `ARGMIN`): slice and
accumulate work is currently software-side logical work. `bytes` is the sum of
the five categories' logical operand/result traffic, not a timed DRAM claim.

| Solver | PROJECT elems | PROJECT_ACC elems | SLICE elems | MAP3 elems | ARGMIN elems | descriptors | bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| RN, min-degree | 154.2 | 40.3 | 41.8 | 664.6 | 0.0 | 201.2 | 11,641.1 |
| RN, max-degree | 151.3 | 41.8 | 41.2 | 696.5 | 0.0 | 213.8 | 12,107.2 |
| RN, min-work | 148.3 | 37.3 | 42.3 | 680.8 | 0.0 | 201.8 | 11,776.7 |
| RN+local, min-degree | 154.2 | 40.3 | 413.6 | 664.6 | 99.9 | 236.2 | 17,181.5 |
| RN+local, max-degree | 151.3 | 41.8 | 388.6 | 696.5 | 96.4 | 248.1 | 17,321.6 |
| RN+local, min-work | 148.3 | 37.3 | 400.5 | 680.8 | 97.6 | 236.2 | 17,130.5 |
| Local search | 0.0 | 0.0 | 18,335.6 | 0.0 | 7,062.6 | 1,772.3 | 290,706.2 |

RN is therefore dominated by R2-style `MAP3_REDUCE` work on this corpus, not
by its irreducible-core projection rows. Local refinement changes the mix: it
adds modest slice/argmin work to RN, while standalone local search is dominated
by repeated `SLICE_ACCUMULATE` and `ARGMIN_VECTOR` passes. The exact solvers
are measured only on the distinct eight-node exact subset, so their operation
mix is deliberately not presented as a like-for-like 20-node comparison.

## Workload implications

The common vocabulary is `MINPLUS_PROJECT` for a matrix-plus-vector minimum,
`PROJECT_ACCUMULATE` for the analytical fused RN form,
`SLICE_ACCUMULATE` for conditioning and local-score construction,
`MAP3_REDUCE` for R2, and `ARGMIN_VECTOR` for choices that need an index. RN
scoring is a generic min-plus projection. For each candidate state it uses
the current `MAP_ADD_REDUCE_MIN` opcode, which returns only a minimum: no
argmin result is required while scoring. R1, R2, and coordinate descent do
need argmins for reconstruction or choice, and use the argmin commands.

Conditioning is also generic: a selected matrix row or column is added to a
neighbor unary vector. The CSV separates matrix reads, unary reads, and unary
writes; every conditioned element accounts for exactly 12 bytes of this
logical traffic. RN commit counters include only heuristic RN commits, while
the generic conditioning counters also include exact branch decisions.

For interface exploration, a structural matrix--vector min-plus projection
would replace the repeated scalar minimum descriptors used by RN scoring. A
second, fused projection-and-accumulate form could avoid materializing the
per-state projection results. `SLICE_ACCUMULATE` and `MAP3_REDUCE` remain
separate analytical categories, rather than being counted as those proposed
projection descriptors. These are comparisons of generic cost-algebra work,
not proposals for PBQP-specific ISA commands. The current results do not
justify changing the stable descriptor ABI: selection policy, branch search,
and batching remain software responsibilities.
