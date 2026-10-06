# Generated CPU baseline tables

## Native corpus timing

| Variant | Aggregate ms | Kernel ms | Median µs | p90 µs | p99 µs | Max µs | Median p90/p10 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| current_original | 5161.659 | 33.882 | 43.016 | 473.803 | 234702.191 | 2001516.887 | 1.145 |
| scalar | 98.929 | 28.881 | 36.125 | 156.827 | 6454.316 | 18344.866 | 1.144 |
| dense | 89.099 | 18.540 | 21.409 | 112.666 | 6146.645 | 19549.999 | 1.201 |
| structured | 93.656 | 23.125 | 21.033 | 133.785 | 6450.395 | 20094.903 | 1.183 |

## Exclusive diagnostic profiles (instrumented, not end-to-end measurements)

### current

| Component | Profile time ms | Share |
| --- | --- | --- |
| other | 6.396 | 0.120% |
| topology | 5172.567 | 96.883% |
| selection | 67.464 | 1.264% |
| reduction | 58.103 | 1.088% |
| reconstruction | 0.114 | 0.002% |
| kernel | 34.316 | 0.643% |
| metadata | 0.000 | 0.000% |

### structured

| Component | Profile time ms | Share |
| --- | --- | --- |
| other | 6.007 | 3.066% |
| topology | 53.409 | 27.259% |
| selection | 54.958 | 28.050% |
| reduction | 54.963 | 28.052% |
| reconstruction | 0.083 | 0.042% |
| kernel | 10.004 | 5.106% |
| metadata | 16.506 | 8.424% |

## Dynamic projection structure

| Class | Calls | Logical m*n | Projection timer ms | Time share | Logical bytes | After-R2 calls |
| --- | --- | --- | --- | --- | --- | --- |
| matching | 93071 | 13126297 | 23.857 | 99.402% | 61459484 | 1755 |
| forbidden_rows | 0 | 0 | 0.000 | 0.000% | 0 | 0 |
| row_exceptions | 389 | 50930 | 0.143 | 0.598% | 238612 | 389 |
| dense | 0 | 0 | 0.000 | 0.000% | 0 | 0 |

| Origin/class | Calls | Elements | Projection ms |
| --- | --- | --- | --- |
| initial forbidden matching | 91316 | 12897626 | 23.284 |
| structured after reductions | 2144 | 279601 | 0.565 |
| generic dense | 0 | 0 | 0.000 |

## Warmed kernel comparison (ns/call)

| Kernel/shape/form | Scalar | Dense | Structured | Dense/structured | Structured effective elements/s |
| --- | --- | --- | --- | --- | --- |
| PROJECT 7×7 0/INF | 79.3 | 76.8 | 30.0 | 2.559 | 1632489880 |
| MAP3 7×7 0/INF | 138.2 | 109.2 | 56.3 | 1.941 | 870739596 |
| PROJECT 7×7 row exceptions | 79.7 | 81.0 | 36.6 | 2.214 | 1340225924 |
| MAP3 7×7 row exceptions | 116.6 | 114.6 | 65.6 | 1.747 | 747264478 |
| PROJECT 7×16 0/INF | 163.0 | 115.2 | 39.5 | 2.914 | 2834259107 |
| MAP3 7×16 0/INF | 286.7 | 139.5 | 64.5 | 2.162 | 1735694084 |
| PROJECT 7×16 row exceptions | 164.1 | 115.1 | 47.2 | 2.438 | 2373007045 |
| MAP3 7×16 row exceptions | 263.1 | 142.0 | 70.5 | 2.013 | 1587886693 |
| PROJECT 16×7 0/INF | 172.2 | 164.7 | 52.4 | 3.143 | 2137730951 |
| MAP3 16×7 0/INF | 298.0 | 195.2 | 76.2 | 2.563 | 1470665476 |
| PROJECT 16×7 row exceptions | 161.4 | 166.3 | 62.9 | 2.644 | 1780561672 |
| MAP3 16×7 row exceptions | 276.0 | 195.5 | 86.6 | 2.257 | 1293026317 |
| PROJECT 16×16 0/INF | 353.1 | 215.6 | 58.1 | 3.712 | 4406954725 |
| MAP3 16×16 0/INF | 622.5 | 237.9 | 80.9 | 2.940 | 3162973442 |
| PROJECT 16×16 row exceptions | 334.6 | 215.7 | 104.4 | 2.066 | 2452154256 |
| MAP3 16×16 row exceptions | 602.9 | 247.6 | 96.6 | 2.563 | 2649678364 |
| PROJECT 17×16 0/INF | 383.1 | 230.0 | 68.6 | 3.353 | 3965390307 |
| MAP3 17×16 0/INF | 692.9 | 262.9 | 93.9 | 2.800 | 2896020102 |
| PROJECT 17×16 row exceptions | 371.0 | 235.0 | 75.6 | 3.107 | 3596408880 |
| MAP3 17×16 row exceptions | 620.5 | 241.8 | 104.9 | 2.306 | 2593440122 |
| PROJECT 17×17 0/INF | 403.3 | 234.5 | 61.3 | 3.824 | 4713711354 |
| MAP3 17×17 0/INF | 700.3 | 255.1 | 83.6 | 3.049 | 3455243242 |
| PROJECT 17×17 row exceptions | 385.5 | 246.1 | 71.8 | 3.427 | 4024901467 |
| MAP3 17×17 row exceptions | 699.6 | 256.2 | 97.7 | 2.624 | 2959306969 |
| PROJECT 49×49 0/INF | 3494.0 | 1328.5 | 169.8 | 7.824 | 14140706211 |
| MAP3 49×49 0/INF | 6322.1 | 1353.1 | 196.9 | 6.873 | 12194967100 |
| PROJECT 49×49 row exceptions | 3278.0 | 1299.2 | 201.6 | 6.444 | 11910017610 |
| MAP3 49×49 row exceptions | 6019.0 | 1336.5 | 228.3 | 5.853 | 10514584004 |

## CPU versus modeled device work

| Workload | CPU kernel µs | CPU total µs | L1 cycles | L2 cycles | L2/L1 | L1 MHz | L2 MHz |
| --- | --- | --- | --- | --- | --- | --- | --- |
| corpus | 18539.589 | 89098.721 | 5435397 | 43019109 | 7.915 | 293.178 | 2320.392 |
| CPU-kernel p50 rank | 14.526 | 21.809 | 4508 | 33314 | 7.390 | 310.340 | 2293.405 |
| CPU-kernel p90 rank | 62.264 | 142.840 | 17625 | 141590 | 8.033 | 283.069 | 2274.027 |
| CPU-kernel p99 rank | 639.571 | 6378.801 | 172078 | 1413721 | 8.216 | 269.052 | 2210.421 |
| projection-heavy top 10% by elements | 11041.201 | 76479.929 | 3138391 | 25477612 | 8.118 | 284.244 | 2307.504 |
| CPU most expensive ten | 6451.440 | 63276.249 | 1736961 | 14309520 | 8.238 | 269.236 | 2218.035 |

## End-to-end sensitivity (negative allowance means no break-even)

| Device MHz | Ideal max additional ms/corpus | ns/submission | ns/solve | With measured preparation ms/corpus |
| --- | --- | --- | --- | --- |
| 250 | -153.537 | -9993.937 | -312702.336 | -210.947 |
| 500 | -67.499 | -4393.584 | -137471.749 | -124.909 |
| 1000 | -24.480 | -1593.408 | -49856.456 | -81.889 |
| 2000 | -2.970 | -193.319 | -6048.810 | -60.380 |

| Submission ns | Visibility ns/solve | Ideal required MHz | With measured preparation MHz |
| --- | --- | --- | --- |
| 0 | 0 | 2320.392 | — |
| 0 | 100 | 2326.553 | — |
| 0 | 1000 | 2383.516 | — |
| 50 | 0 | 2420.688 | — |
| 50 | 100 | 2427.395 | — |
| 50 | 1000 | 2489.469 | — |
| 100 | 0 | 2530.046 | — |
| 100 | 100 | 2537.373 | — |
| 100 | 1000 | 2605.278 | — |
| 500 | 0 | 3961.941 | — |
| 500 | 100 | 3979.938 | — |
| 500 | 1000 | 4149.584 | — |
| 1000 | 0 | 13542.548 | — |
| 1000 | 100 | 13755.159 | — |
| 1000 | 1000 | 16018.501 | — |

## Selective offload on held-out graphs

| Policy | MHz | Fixed ns | Preparation | CPU kernel ms | L2 cycles | Submissions | Modeled solve ms | False offloads |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| cpu | 2000 | 0 | ideal | 12.417 | 0 | 0 | 68.459 | 0 |
| all_offload | 2000 | 0 | ideal | 0.000 | 28719032 | 10211 | 70.401 | 8291 |
| estimated_threshold | 2000 | 0 | ideal | 10.348 | 3694987 | 1323 | 68.236 | 442 |
| oracle_upper_bound | 2000 | 0 | ideal | 9.118 | 5278839 | 1920 | 67.798 | 0 |
| cpu | 2000 | 0 | measured | 12.417 | 0 | 0 | 68.459 | 0 |
| all_offload | 2000 | 0 | measured | 0.000 | 28719032 | 10211 | 109.868 | 10210 |
| estimated_threshold | 2000 | 0 | measured | 12.417 | 0 | 0 | 68.459 | 0 |
| oracle_upper_bound | 2000 | 0 | measured | 12.404 | 9665 | 1 | 68.457 | 0 |
| cpu | 8000 | 0 | ideal | 12.417 | 0 | 0 | 68.459 | 0 |
| all_offload | 8000 | 0 | ideal | 0.000 | 28719032 | 10211 | 59.631 | 0 |
| estimated_threshold | 8000 | 0 | ideal | 0.000 | 28719032 | 10211 | 59.631 | 0 |
| oracle_upper_bound | 8000 | 0 | ideal | 0.000 | 28719032 | 10211 | 59.631 | 0 |
| cpu | 8000 | 0 | measured | 12.417 | 0 | 0 | 68.459 | 0 |
| all_offload | 8000 | 0 | measured | 0.000 | 28719032 | 10211 | 99.098 | 10202 |
| estimated_threshold | 8000 | 0 | measured | 12.417 | 0 | 0 | 68.459 | 0 |
| oracle_upper_bound | 8000 | 0 | measured | 12.335 | 81417 | 9 | 68.447 | 0 |

## L2 phase cycles

| Phase | Cycles | Share |
| --- | --- | --- |
| descriptor_cycles | 1378750 | 3.205% |
| decode_cycles | 381788 | 0.887% |
| protection_cycles | 2156204 | 5.012% |
| operand_cycles | 10437188 | 24.262% |
| add1_cycles | 2877811 | 6.690% |
| add2_cycles | 1049958 | 2.441% |
| tree_cycles | 7082866 | 16.464% |
| merge_cycles | 3541433 | 8.232% |
| writeback_cycles | 4343134 | 10.096% |
| drain_cycles | 206257 | 0.479% |
| memory_wait_cycles | 8079536 | 18.781% |
| control_cycles | 1484184 | 3.450% |

## Current L2 minus L1

| Difference | Cycles |
| --- | --- |
| descriptor service | 1569644 |
| operand requests/reuse | 11468097 |
| ADD/tree/merge | 10769801 |
| result acknowledgements | 5853169 |
| decode/protection/control | 4228433 |
| removed L1 read/compute overlap | 3694568 |

## Most expensive native graphs

| Input | Nodes | Edges | CPU µs | Kernel µs | L2 cycles |
| --- | --- | --- | --- | --- | --- |
| 167-instcombine-instcombinerimpl-visitsub-n895-d17.pbqp | 895 | 6018 | 19549.999 | 1390.727 | 2972166 |
| 168-instcombine-instcombinerimpl-visitsub-n966-d17-r1.pbqp | 966 | 4031 | 14643.965 | 996.178 | 2163293 |
| 166-instcombine-instcombinerimpl-visitadd-n657-d17-r1.pbqp | 657 | 2574 | 6378.801 | 639.571 | 1413721 |
| 368-mbb-machinebasicblock-splitcriticaledge-n301-d17.pbqp | 301 | 5188 | 6157.902 | 797.050 | 1994736 |
| 165-instcombine-instcombinerimpl-visitadd-n544-d17.pbqp | 544 | 3152 | 6146.645 | 690.940 | 1577008 |
| 447-mbb-machinebasicblock-print-n227-d17.pbqp | 227 | 2623 | 2545.617 | 469.129 | 1129298 |
| 369-mbb-machinebasicblock-splitcriticaledge-n423-d17-r1.pbqp | 423 | 1395 | 2315.287 | 484.308 | 895150 |
| 154-instcombine-instcombinerimpl-foldaddwithconstant-n343-d17.pbqp | 343 | 1555 | 2203.860 | 361.170 | 821302 |
| 155-instcombine-instcombinerimpl-foldaddwithconstant-n358-d17-r1.pbqp | 358 | 1371 | 2094.090 | 337.554 | 767427 |
| 448-mbb-machinebasicblock-print-n234-d17-r1.pbqp | 234 | 1075 | 1240.083 | 284.813 | 575419 |
