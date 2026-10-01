# Generated Solver Characterization Summary

## RN Cascade Characterization

Only successful runs with at least one RN episode contribute to this table.

| Family | Solver / policy | runs | RN decisions / solve | exact reductions / episode (mean / median / max) | zero / one / multiple episodes | R0 / R1 / R2 cascade composition |
|---|---|---:|---:|---:|---:|---:|
| degree-3 | heuristic-rn, min-degree | 10 | 5.0 | 3.0 / 3.0 / 3 | 0.0% / 0.0% / 100.0% | 6.7% / 6.7% / 86.7% |
| degree-3 | heuristic-rn, max-degree | 10 | 5.0 | 3.0 / 3.0 / 3 | 0.0% / 0.0% / 100.0% | 6.7% / 6.7% / 86.7% |
| degree-3 | heuristic-rn, min-work | 10 | 5.0 | 3.0 / 3.0 / 3 | 0.0% / 0.0% / 100.0% | 6.7% / 6.7% / 86.7% |
| degree-3 | heuristic-rn-local-search, min-degree | 10 | 5.0 | 3.0 / 3.0 / 3 | 0.0% / 0.0% / 100.0% | 6.7% / 6.7% / 86.7% |
| degree-3 | heuristic-rn-local-search, max-degree | 10 | 5.0 | 3.0 / 3.0 / 3 | 0.0% / 0.0% / 100.0% | 6.7% / 6.7% / 86.7% |
| degree-3 | heuristic-rn-local-search, min-work | 10 | 5.0 | 3.0 / 3.0 / 3 | 0.0% / 0.0% / 100.0% | 6.7% / 6.7% / 86.7% |
| degree-4 | heuristic-rn, min-degree | 10 | 2.0 | 9.0 / 9.0 / 18 | 50.0% / 0.0% / 50.0% | 5.6% / 5.6% / 88.9% |
| degree-4 | heuristic-rn, max-degree | 10 | 7.0 | 1.9 / 2.0 / 3 | 14.3% / 0.0% / 85.7% | 7.7% / 7.7% / 84.6% |
| degree-4 | heuristic-rn, min-work | 10 | 2.0 | 9.0 / 9.0 / 18 | 50.0% / 0.0% / 50.0% | 5.6% / 5.6% / 88.9% |
| degree-4 | heuristic-rn-local-search, min-degree | 10 | 2.0 | 9.0 / 9.0 / 18 | 50.0% / 0.0% / 50.0% | 5.6% / 5.6% / 88.9% |
| degree-4 | heuristic-rn-local-search, max-degree | 10 | 7.0 | 1.9 / 2.0 / 3 | 14.3% / 0.0% / 85.7% | 7.7% / 7.7% / 84.6% |
| degree-4 | heuristic-rn-local-search, min-work | 10 | 2.0 | 9.0 / 9.0 / 18 | 50.0% / 0.0% / 50.0% | 5.6% / 5.6% / 88.9% |
| mixed-degree | heuristic-rn, min-degree | 10 | 14.8 | 0.3 / 0.0 / 4 | 85.1% / 7.4% / 7.4% | 22.2% / 22.2% / 55.6% |
| mixed-degree | heuristic-rn, max-degree | 10 | 6.6 | 1.9 / 2.0 / 5 | 22.7% / 22.7% / 54.5% | 7.9% / 7.9% / 84.3% |
| mixed-degree | heuristic-rn, min-work | 10 | 13.9 | 0.4 / 0.0 / 5 | 82.0% / 9.4% / 8.6% | 18.5% / 18.5% / 63.0% |
| mixed-degree | heuristic-rn-local-search, min-degree | 10 | 14.8 | 0.3 / 0.0 / 4 | 85.1% / 7.4% / 7.4% | 22.2% / 22.2% / 55.6% |
| mixed-degree | heuristic-rn-local-search, max-degree | 10 | 6.6 | 1.9 / 2.0 / 5 | 22.7% / 22.7% / 54.5% | 7.9% / 7.9% / 84.3% |
| mixed-degree | heuristic-rn-local-search, min-work | 10 | 13.9 | 0.4 / 0.0 / 5 | 82.0% / 9.4% / 8.6% | 18.5% / 18.5% / 63.0% |

## Operation Mix by Graph Family

Values are means per successful 20-node run. Operation elements describe shapes of work, not equal hardware cost; no cycle or area percentage is implied.

| Family | Solver / policy | PROJECT | PROJECT_ACC | SLICE | MAP3 | ARGMIN | descriptors | batches | operand bytes | result bytes |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| degree-3 | heuristic-rn, min-degree | 64.0 | 30.0 | 30.0 | 104.0 | 0.0 | 84.0 | 29.0 | 2240.0 | 792.0 |
| degree-3 | heuristic-rn-local-search, min-degree | 64.0 | 30.0 | 174.0 | 104.0 | 48.0 | 108.0 | 53.0 | 3776.0 | 1560.0 |
| degree-3 | local-search, min-degree | 0.0 | 0.0 | 4896.0 | 0.0 | 1632.0 | 816.0 | 816.0 | 52224.0 | 26112.0 |
| degree-4 | heuristic-rn, min-degree | 32.0 | 14.0 | 14.0 | 128.0 | 0.0 | 80.0 | 24.0 | 2016.0 | 696.0 |
| degree-4 | heuristic-rn-local-search, min-degree | 32.0 | 14.0 | 206.0 | 128.0 | 48.0 | 104.0 | 48.0 | 3936.0 | 1656.0 |
| degree-4 | local-search, min-degree | 0.0 | 0.0 | 6576.0 | 0.0 | 1644.0 | 822.0 | 822.0 | 65760.0 | 32880.0 |
| mixed-degree RN-required | heuristic-rn, min-degree | 466.3 | 148.5 | 157.1 | 90.7 | 0.0 | 182.1 | 55.2 | 7263.6 | 2085.2 |
| mixed-degree RN-required | heuristic-rn-local-search, min-degree | 466.3 | 148.5 | 791.1 | 90.7 | 107.8 | 218.1 | 91.2 | 13198.0 | 4909.2 |
| mixed-degree | local-search, min-degree | 0.0 | 0.0 | 28906.4 | 0.0 | 4984.4 | 1668.0 | 1668.0 | 271126.4 | 128969.6 |
