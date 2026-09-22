# Fork-level parallelism characterization

`BRANCH_SELECT` JSONL events now add `branch_domain`, the number of independent
children created conceptually by that fork.  All prior fields remain unchanged,
and the schema regression parses traces through the existing generic JSON
consumer before requiring the additive field.

## Method

`scripts/fork_parallelism_characterize.rb` reruns the small exact points from
[the branch-and-bound study](branch-bound-characterization.md): two seeds at
degree-3 N=20/30 (D=2/4) and N=50 (D=2), degree-4 N=20/50 (D=2/4) and N=100
(D=2), and mixed N=15/20/25 (D=2).  It also uses the same 30 RN=1-3 plus nine
RN=4-8 sampling bands and 45-second/one-million-node real-search limits.  The
earlier report did not persist its filename list, so the script makes selection
reproducible from `build/scaling-runs.csv` order.

For each graph a heuristic run supplies the median Model-C epoch elements per
RN decision.  Each exact-search fork is crossed with that graph-level value:

```text
independent elements exposed at fork = branch_domain * median Model-C elements per child
```

This is deliberately a logical-work proxy.  It does not assert that a heuristic
epoch and an exact bound computation execute identical code or that all child
work can overlap.  Raw per-fork rows are in `build/fork-parallelism.csv`.

## Distribution

Values below are pooled per fork and shown as min / p25 / median / p75 / p90 /
p99 / max.

| Set | forks | branch domain | Model-C elements / child | independent elements / fork |
|---|---:|---:|---:|---:|
| degree-3 | 11,872 | 2 / 2 / 4 / 4 / 4 / 4 / 4 | 48 / 48 / 264 / 264 / 264 / 264 / 264 | 96 / 96 / 1,056 / 1,056 / 1,056 / 1,056 / 1,056 |
| degree-4 | 38 | 2 / 2 / 4 / 4 / 4 / 4 / 4 | 94 / 214 / 604 / 1,564 / 1,564 / 1,564 / 1,564 | 188 / 428 / 2,416 / 6,256 / 6,256 / 6,256 / 6,256 |
| mixed-degree | 166,997 | 2 / 2 / 2 / 2 / 2 / 2 / 2 | 24 / 32 / 32 / 40 / 40 / 40 / 40 | 48 / 64 / 64 / 80 / 80 / 80 / 80 |
| real sample | 119,527 | 7 / 7 / 7 / 7 / 7 / 16 / 16 | 379 / 810 / 810 / 810 / 1,134 / 1,134 / 14,111 | 2,653 / 5,670 / 5,670 / 5,670 / 7,938 / 12,960 / 225,784 |

The pooled real distribution is dominated by the largest trees, so it is a
per-fork answer, not a typical-graph answer.  The typical low-RN real graph has
only a few forks, while hard trees contribute many repeated rows.

## Answer and remaining gap

A typical fork exposes only 2-4 children synthetically and 7 children in the
fork-weighted real median.  Work per child is tiny for mixed-degree synthetic
graphs (32 elements median) but hundreds of elements in degree-3/4 and 810 in
the real median.  The combined median therefore ranges from 64 elements
(negligible beside sustained lane throughput) to 1,056/2,416 synthetic and
5,670 real elements (large enough that fork-level overlap could matter if the
cost of snapshots, scheduling, and shared memory is low).  The p99 real point
is 12,960 elements, with a 225,784-element outlier.

This narrows the question but does not settle architecture.  It measures work
made independent at one fork; it still does not measure the true peak
concurrent frontier width, parent/child lifetime overlap, memory bandwidth, or
snapshot traffic.  Depth and parent tracking remain intentionally absent.

