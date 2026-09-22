# Matrix access-pattern study

The shared solver already represents every matrix slice as
`{base, length, stride}` and counts whether `stride == 1`.  This pass only
surfaces those existing counters in verbose output and the existing scaling/RN
CSV schemas.

## Data

`build/isa-round3-scaling.csv` contains the normal synthetic sweeps, the D=32
extension, and all 491 LLVM inputs.  Counts are aggregated over successful
solves:

| family | solves | contiguous views | strided views | contiguous:strided | strided share |
|---|---:|---:|---:|---:|---:|
| degree-3 | 146 | 6,757,540 | 5,020,964 | 1.35:1 | 42.6% |
| degree-4 | 146 | 5,055,436 | 9,901,748 | 0.51:1 | 66.2% |
| mixed-degree | 115 | 6,281,986 | 2,004,098 | 3.14:1 | 24.2% |
| LLVM RegAllocPBQP | 491 | 2,204,698 | 526,736 | 4.19:1 | 19.3% |

Across synthetic points, contiguous:strided falls from 2.44:1 at D=2 to
0.84:1 at D=16 and 0.80:1 at D=32.  Larger matrices amplify the distinction
between row slices (`stride=1`) and column slices (`stride=domain_capacity`),
while topology determines how often each orientation is selected.  The real
corpus is much more contiguous-heavy than regular degree-4 despite its D≈16
cluster, so D alone does not predict access orientation.

## Scope and limitation

The statistics are global per solve.  They are incremented at view creation but
do not carry a reduction-kind tag, so the existing granularity cannot honestly
split RN/PROJECT from R2/MAP3.  Adding such attribution would require new
counters and is outside this item.

These numbers measure how often today's host-side view abstraction is
contiguous or strided under one row-major padded storage convention.  They are
not a performance claim for stride-aware hardware: no cache-line behavior,
packing overlap, bank conflicts, or stride-unit latency is modeled here.

