# RN solver characterization

## Scope

This document records the first RN milestone. It characterizes shared PBQP
solver behavior and generic cost-algebra work; it does not add a PBQP-specific
accelerator opcode, RTL, or XLS design.

## Solver strategies

The shared freestanding C++ solver exposes these stable strategy names:

- `REDUCE_ONLY`: exact R0/R1/R2 reductions, returning `IRREDUCIBLE` when an
  active core remains;
- `HEURISTIC_RN`: minimum-degree reduction followed by classical RN whenever
  no R0/R1/R2 node remains;
- `EXACT_BRANCH_REDUCE`: exact reduction followed by deterministic exhaustive
  enumeration of the remaining core. It is the small-instance optimality
  oracle and supports an explicit search-node limit.

`LOCAL_SEARCH` remains a separate host-runner strategy. It is not an oracle.
The shared solver is used unchanged by the hosted fixed-capacity runner and
the RV64 bare-metal tests; only the CostKernel implementation differs.

## RN semantics

For selected node `X`, RN scores every state using:

\[
s_X(x)=c_X(x)+\sum_{Y\in N(X)}\min_y[C_{XY}(x,y)+c_Y(y)].
\]

The smallest equal state index wins. Scoring is followed by a separate shared
conditioning commit: add `c_X(x*)` once to the objective offset, add the
proper matrix row or column `C_XY(x*,y)` to each neighbor unary vector, then
remove `X` and its incident edges. Matrix-slice orientation is supplied by the
same `EdgeView` helper used by R1/R2.

RN policies are deterministic: `MIN_DEGREE`, `MAX_DEGREE`, and `MIN_RN_WORK`.
The latter minimizes `|D_X| sum_Y |D_Y|`, then degree, then node index.

## Cost-algebra trace

One RN neighbor contribution is a generic min-plus projection
`p(x)=min_y[M(x,y)+v(y)]`. The present interface represents it as `|D_X|`
batched `MAP_ADD_REDUCE_MIN_ARGMIN` descriptors of length `|D_Y|`. RN also
records software score-vector accumulation and matrix-slice-to-vector commit
traffic separately.

An analytical structural `MINPLUS_MATVEC` would replace those scalar output
descriptors with one matrix-vector descriptor. A fused
`ACCUMULATE_COST_PROJECT` would additionally avoid materializing `p`. RN
commit is separately modeled as generic `ADD_MATRIX_SLICE_TO_VECTOR`. These
are candidate generic cost-algebra operations, not current ABI commands.

## Deterministic baseline observations

On `examples/chvatal.pbqp`, with its all-zero binary costs and 24 edges:

| policy | RN | R0/R1/R2 after solving | projections | scalar projections |
| --- | ---: | ---: | ---: | ---: |
| MIN_DEGREE | 5 | 1 / 1 / 5 | 16 | 32 |
| MAX_DEGREE | 4 | 1 / 1 / 6 | 16 | 32 |
| MIN_RN_WORK | 5 | 1 / 1 / 5 | 16 | 32 |

All three return objective zero on this symmetric input. The result illustrates
that selection changes the RN/R2 mix even when projection volume does not.
The deterministic RV64 `pbqp_rn.elf` compares software and accelerator
CostKernels for every policy, including RN node/state sequence and primitive
counts.

## Corpus status and limits

The exact strategy deliberately uses exhaustive core enumeration after exact
reductions. It is appropriate only for tractable residual cores; its statistics
report visited search nodes, branches, depth, and explicit search-limit hits.
The checked-in random-sparse binary corpus now supplies ten 20-node local runs;
its results and seeds are in [the RN report](rn-report.md). Degree-3,
degree-4, mixed-degree, larger-domain, and 50/100/200-node families remain
future work. No conclusion about a future hardware interface should be drawn
from the small baseline alone.
