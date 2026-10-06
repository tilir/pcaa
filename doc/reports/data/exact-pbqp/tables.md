# Generated exact PBQP tables

## Completion (strong CPU)

| Class | Incumbent | Inputs | Exact CPU | Search limit | Timeout/error | Matched L2 |
| --- | --- | --- | --- | --- | --- | --- |
| llvm | none | 491 | 196 | 287 | 8 | 196 |
| llvm | heuristic | 491 | 400 | 86 | 5 | 400 |
| synthetic | none | 24 | 18 | 6 | 0 | 18 |
| synthetic | heuristic | 24 | 18 | 6 | 0 | 18 |
| stress | none | 18 | 16 | 2 | 0 | 16 |
| stress | heuristic | 18 | 16 | 2 | 0 | 16 |

## Matched end-to-end budgets

| Class | Seed | CPU ms | Kernel ms | Kernel share | Control ms | L2 cycles | Ideal MHz | Amdahl max | Current host ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| llvm | none | 47.889 | 15.526 | 32.420% | 32.364 | 74342480 | 4788.380 | 1.480 | 169.035 |
| llvm | heuristic | 17.895 | 0.826 | 4.618% | 17.068 | 3126487 | 3783.174 | 1.048 | 37.261 |
| synthetic | none | 1.650 | 0.752 | 45.550% | 0.899 | 918465 | 1221.714 | 1.837 | 7.791 |
| synthetic | heuristic | 1.545 | 0.662 | 42.846% | 0.883 | 785483 | 1186.720 | 1.750 | 6.644 |
| stress | none | 0.700 | 0.214 | 30.586% | 0.486 | 394256 | 1841.173 | 1.441 | 4.704 |
| stress | heuristic | 0.735 | 0.212 | 28.867% | 0.523 | 388876 | 1833.084 | 1.406 | 4.625 |

## Break-even distributions (positive ideal frequencies only)

| Class | Seed | Median MHz | Geomean MHz | p75 MHz | p90 MHz | Max MHz | Finite current break-even |
| --- | --- | --- | --- | --- | --- | --- | --- |
| llvm | none | 4648.804 | 4369.943 | 5088.580 | 5592.989 | 6097.681 | 0 |
| llvm | heuristic | 3311.232 | 3234.671 | 3993.207 | 5168.697 | 6003.447 | 1 |
| synthetic | none | 1232.968 | 1278.245 | 1289.763 | 1612.733 | 1925.236 | 0 |
| synthetic | heuristic | 1279.425 | 1278.491 | 1409.400 | 1678.677 | 2269.587 | 0 |
| stress | none | 1503.850 | 1569.072 | 1916.023 | 2216.919 | 2580.331 | 0 |
| stress | heuristic | 1626.547 | 1627.737 | 2044.284 | 2195.249 | 2724.218 | 0 |

## Win fractions among matched completed inputs

| Class | Seed | 250 MHz ideal/current | 500 MHz ideal/current | 1 GHz ideal/current | 2 GHz ideal/current |
| --- | --- | --- | --- | --- | --- |
| llvm | none | 0.0% / 0.0% | 0.0% / 0.0% | 0.0% / 0.0% | 1.0% / 0.0% |
| llvm | heuristic | 0.0% / 0.2% | 0.0% / 0.2% | 0.2% / 0.2% | 8.8% / 0.2% |
| synthetic | none | 0.0% / 0.0% | 0.0% / 0.0% | 0.0% / 0.0% | 100.0% / 0.0% |
| synthetic | heuristic | 0.0% / 0.0% | 0.0% / 0.0% | 5.6% / 0.0% | 94.4% / 0.0% |
| stress | none | 0.0% / 0.0% | 0.0% / 0.0% | 0.0% / 0.0% | 75.0% / 0.0% |
| stress | heuristic | 0.0% / 0.0% | 0.0% / 0.0% | 0.0% / 0.0% | 62.5% / 0.0% |

## Exclusive diagnostic CPU phases (intrusive; all completed native solves)

| Class | Seed | Phase | Profile ms |
| --- | --- | --- | --- |
| llvm | heuristic | other | 0.264 |
| llvm | heuristic | topology | 5.883 |
| llvm | heuristic | selection | 6.038 |
| llvm | heuristic | reduction | 2.126 |
| llvm | heuristic | reconstruction | 0.040 |
| llvm | heuristic | kernel | 9.183 |
| llvm | heuristic | metadata | 0.281 |
| llvm | heuristic | search | 0.050 |
| llvm | heuristic | clone | 0.000 |
| llvm | heuristic | allocation | 1.205 |
| llvm | heuristic | conditioning | 1.672 |
| llvm | heuristic | r0 | 0.031 |
| llvm | heuristic | r1 | 0.359 |
| llvm | heuristic | r2 | 1.790 |
| llvm | heuristic | lower_bound | 1.461 |
| llvm | heuristic | incumbent | 0.028 |
| llvm | heuristic | seed | 1.913 |
| llvm | none | other | 0.095 |
| llvm | none | topology | 5.748 |
| llvm | none | selection | 3.278 |
| llvm | none | reduction | 0.000 |
| llvm | none | reconstruction | 0.081 |
| llvm | none | kernel | 13.532 |
| llvm | none | metadata | 3.720 |
| llvm | none | search | 9.377 |
| llvm | none | clone | 12.426 |
| llvm | none | allocation | 4.737 |
| llvm | none | conditioning | 3.794 |
| llvm | none | r0 | 0.208 |
| llvm | none | r1 | 2.088 |
| llvm | none | r2 | 9.291 |
| llvm | none | lower_bound | 6.907 |
| llvm | none | incumbent | 1.114 |
| llvm | none | seed | 0.000 |
| synthetic | none | other | 0.005 |
| synthetic | none | topology | 0.668 |
| synthetic | none | selection | 0.385 |
| synthetic | none | reduction | 0.000 |
| synthetic | none | reconstruction | 0.003 |
| synthetic | none | kernel | 0.754 |
| synthetic | none | metadata | 0.202 |
| synthetic | none | search | 0.714 |
| synthetic | none | clone | 0.136 |
| synthetic | none | allocation | 0.422 |
| synthetic | none | conditioning | 0.179 |
| synthetic | none | r0 | 0.014 |
| synthetic | none | r1 | 0.086 |
| synthetic | none | r2 | 0.732 |
| synthetic | none | lower_bound | 0.075 |
| synthetic | none | incumbent | 0.065 |
| synthetic | none | seed | 0.000 |
| synthetic | heuristic | other | 0.007 |
| synthetic | heuristic | topology | 0.656 |
| synthetic | heuristic | selection | 0.387 |
| synthetic | heuristic | reduction | 0.018 |
| synthetic | heuristic | reconstruction | 0.002 |
| synthetic | heuristic | kernel | 0.695 |
| synthetic | heuristic | metadata | 0.182 |
| synthetic | heuristic | search | 0.583 |
| synthetic | heuristic | clone | 0.106 |
| synthetic | heuristic | allocation | 0.389 |
| synthetic | heuristic | conditioning | 0.156 |
| synthetic | heuristic | r0 | 0.010 |
| synthetic | heuristic | r1 | 0.062 |
| synthetic | heuristic | r2 | 0.705 |
| synthetic | heuristic | lower_bound | 0.078 |
| synthetic | heuristic | incumbent | 0.050 |
| synthetic | heuristic | seed | 0.030 |
| stress | none | other | 0.003 |
| stress | none | topology | 0.325 |
| stress | none | selection | 0.186 |
| stress | none | reduction | 0.000 |
| stress | none | reconstruction | 0.012 |
| stress | none | kernel | 0.255 |
| stress | none | metadata | 0.070 |
| stress | none | search | 0.605 |
| stress | none | clone | 0.120 |
| stress | none | allocation | 0.285 |
| stress | none | conditioning | 0.222 |
| stress | none | r0 | 0.016 |
| stress | none | r1 | 0.113 |
| stress | none | r2 | 0.165 |
| stress | none | lower_bound | 0.022 |
| stress | none | incumbent | 0.055 |
| stress | none | seed | 0.000 |
| stress | heuristic | other | 0.004 |
| stress | heuristic | topology | 0.351 |
| stress | heuristic | selection | 0.200 |
| stress | heuristic | reduction | 0.015 |
| stress | heuristic | reconstruction | 0.012 |
| stress | heuristic | kernel | 0.268 |
| stress | heuristic | metadata | 0.072 |
| stress | heuristic | search | 0.598 |
| stress | heuristic | clone | 0.121 |
| stress | heuristic | allocation | 0.305 |
| stress | heuristic | conditioning | 0.235 |
| stress | heuristic | r0 | 0.016 |
| stress | heuristic | r1 | 0.116 |
| stress | heuristic | r2 | 0.168 |
| stress | heuristic | lower_bound | 0.026 |
| stress | heuristic | incumbent | 0.054 |
| stress | heuristic | seed | 0.015 |

## Exact operation mix (matched solves)

| Class | Seed | Phase | Callback | Batches | Elements | CPU ms | L2 cycles |
| --- | --- | --- | --- | --- | --- | --- | --- |
| llvm | heuristic | r2 | map3_project_batch | 693 | 1115328 | 0.683 | 2837891 |
| llvm | heuristic | r1 | min2_batch | 434 | 73771 | 0.143 | 288596 |
| llvm | none | r2 | map3_project_batch | 10714 | 24251297 | 11.910 | 62057110 |
| llvm | none | r1 | min2_batch | 7844 | 1510800 | 2.225 | 6160485 |
| llvm | none | conditioning | add | 58504 | 708754 | 1.391 | 6124885 |
| synthetic | none | conditioning | add | 2898 | 10028 | 0.037 | 158930 |
| synthetic | none | r2 | map3_project_batch | 2569 | 135800 | 0.668 | 703941 |
| synthetic | none | r1 | min2_batch | 466 | 6040 | 0.047 | 55594 |
| synthetic | heuristic | conditioning | add | 2304 | 7832 | 0.027 | 125792 |
| synthetic | heuristic | r2 | map3_project_batch | 2295 | 119720 | 0.604 | 623275 |
| synthetic | heuristic | r1 | min2_batch | 312 | 3888 | 0.031 | 36416 |
| stress | none | conditioning | add | 3525 | 10593 | 0.048 | 186897 |
| stress | none | r2 | map3_project_batch | 617 | 20141 | 0.110 | 141269 |
| stress | none | r1 | min2_batch | 617 | 6119 | 0.056 | 66090 |
| stress | heuristic | conditioning | add | 3495 | 10491 | 0.049 | 185259 |
| stress | heuristic | r2 | map3_project_batch | 607 | 19723 | 0.108 | 138691 |
| stress | heuristic | r1 | min2_batch | 607 | 6001 | 0.055 | 64926 |
