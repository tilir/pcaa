# L1 performance model

This is a parameterized architectural cycle model, not RTL timing or a claim
about frequency, throughput, or end-to-end speedup. It retains the functional
L0 ABI and lets SystemC annotate a nominal cycle period only when requested.

## Boundary and formulas

Software selects the PBQP reduction policy and constructs batches. The
accelerator walks the ordered batch and executes primitive map/reduce commands.
It has no graph state, dependency logic, or scheduling policy.

For a primitive of length `N`, `P` operands, and `L` lanes:

```text
chunks          = ceil(N / L)
descriptor      = ceil(56 / descriptor_bytes_per_cycle)
operand read    = ceil(P * N * 4 / memory_read_bytes_per_cycle)
compute         = primitive_start + map_latency + chunks
                  + add3_extra_when_applicable + reduction_latency + result_latency
result write    = ceil(result_bytes / memory_write_bytes_per_cycle)
```

The sequential mode adds all four categories. The idealized streaming mode is:

```text
descriptor + max(operand read, compute) + result write
```

An `EXECUTE_BATCH` also pays one outer descriptor fetch, `batch_start`, and its
completion-result write.
Child descriptors remain visible in the software-batch model. Lane utilisation
is reported as `N / (ceil(N/L) * L)`; it is a logical packing proxy.

## Configurations and organizations

The checked-in sweep uses no cycle period and therefore reports modeled cycles.

| Configuration | Mode | Lanes | Descriptor B/cycle | Read B/cycle | Write B/cycle | Batch / primitive start | Map / ADD3 extra / reduction / result |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| S4 | sequential | 4 | 16 | 16 | 16 | 4 / 2 | 2 / 1 / 3 / 1 |
| T8 | streaming | 8 | 32 | 32 | 16 | 4 / 2 | 2 / 1 / 3 / 1 |
| T16 | streaming | 16 | 32 | 64 | 16 | 4 / 2 | 2 / 1 / 3 / 1 |

Three interface organizations are replayed against identical traces: original
primitive submission (historical control baseline), implemented software-built
batches (outer plus child descriptors), and hypothetical structural descriptors
(outer descriptor only; child descriptor and primitive-start costs removed).
The optional stride-aware comparison keeps the same device operand bytes and
service formula, but reports software packing as zero; it does not speculate
about strided-memory efficiency.

## Corpus policy comparison

The hosted analyzer reproduces production edge slots, free-slot reuse, neighbor
enumeration, fill-edge orientation, and default first-index selection. The
corpus is synthetic and does not establish a universally best policy.

| Policy | R0/R1/R2 | Primitive descriptors | Batches | Logical read bytes | Original / reused packing bytes |
| --- | --- | ---: | ---: | ---: | ---: |
| production-index | 10/748/1112 | 433,899 | 1,860 | 107,235,704 | 39,278,564 / 2,800,456 |
| degree-priority | 10/1310/550 | 167,654 | 1,860 | 35,173,056 | 20,537,196 / 3,397,052 |
| minimum-kernel-work | 10/1256/604 | 179,423 | 1,860 | 36,488,804 | 19,775,176 / 3,267,424 |

| Policy | S4 software batch / structural | T8 software batch / structural | T16 software batch / structural |
| --- | ---: | ---: | ---: |
| production-index | 15,239,430 / 12,636,036 | 6,670,274 / 5,137,524 | 5,932,482 / 4,230,578 |
| degree-priority | 5,394,382 / 4,388,458 | 2,446,863 / 1,831,131 | 2,258,481 / 1,589,731 |
| minimum-kernel-work | 5,674,098 / 4,597,560 | 2,607,988 / 1,945,453 | 2,412,191 / 1,696,954 |

Within this corpus, degree-priority substantially reduces primitive work versus
production-index. Minimum-kernel-work slightly lowers packing relative to
degree-priority but does not lower logical accelerator work. Increasing lanes
from 8 to 16 still helps, but descriptor/control and memory components limit
the gain. Removing child descriptor/start costs is material in every modeled
configuration, so structural descriptors remain worth investigating rather
than implementing here.

Stride-aware hardware remains an open question: batch-local reuse already
removes most packing, but 2.8–3.4 MB remains in the two low-work policies. L2
should refine overlap, memory behavior, and descriptor handling without moving
software scheduling into hardware.

## Limitations

The model has no FIFOs, cache/coherency behavior, DMA latency, clock-frequency
assumption, pipeline hazards, or CPU-side packing-cycle model. Workload stress
profiles may exceed bare-metal PBQP capacity; they characterize interface shape
only. Results are modeled service cycles under the stated parameters.

## Cycle Breakdown

The following degree-priority corpus totals use the existing T8 and T16
configurations. `read` and `compute` are raw component demands. `exposed` is
the streaming formula's service estimate, so raw columns must not be summed to
derive it. `control` includes descriptors and configured setup; `write` includes
primitive and batch-result writes.

| Configuration | Interface | Control | Raw read | Raw compute | Write | Exposed modeled cycles |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| T8 | software-built batch | 681,776 | 1,149,506 | 1,917,819 | 169,514 | 2,446,863 |
| T8 | structural descriptor | 11,160 | 1,149,506 | 1,582,511 | 169,514 | 1,831,131 |
| T16 | software-built batch | 681,776 | 613,280 | 1,742,499 | 169,514 | 2,258,481 |
| T16 | structural descriptor | 11,160 | 613,280 | 1,407,191 | 169,514 | 1,589,731 |

Streaming overlap is evaluated per primitive, then summed:

```text
primitive exposed = control + max(read_i, compute_i) + write
```

Thus aggregate raw reads and raw compute are demand metrics, but neither alone
determines the sum of per-primitive maxima. The structural rows use the exact
structural assumption: their raw compute excludes the removed primitive-start
cost. The software-batch rows include it.

| Configuration | Interface | Compute / memory / balanced primitives | Compute / memory / balanced max-demand cycles |
| --- | --- | --- | --- |
| T8 | software-built batch | 163,153 (97.3%) / 1,866 (1.1%) / 2,635 (1.6%) | 1,843,937 (95.5%) / 44,784 (2.3%) / 42,160 (2.2%) |
| T8 | structural descriptor | 117,271 (69.9%) / 50,383 (30.1%) / 0 | 1,003,737 (60.8%) / 646,720 (39.2%) / 0 |
| T16 | software-built batch | 167,654 (100.0%) / 0 / 0 | 1,742,499 (100.0%) / 0 / 0 |
| T16 | structural descriptor | 165,788 (98.9%) / 1,866 (1.1%) / 0 | 1,386,665 (98.4%) / 22,392 (1.6%) / 0 |

Classes compare only the exact per-primitive `read_i` and `compute_i` used by
the relevant interface model. Weighted values are sums of
`max(read_i, compute_i)` over primitives in each class; they intentionally
exclude descriptor/control and result-write cycles.

At T8, software-built batching is overwhelmingly compute-dominated under this
model, while removing primitive-start costs exposes a material memory-dominated
minority. At T16, the higher read bandwidth and lane scaling leave the
software-batch primitives compute-dominated; the limited 8→16 total reduction
combines short-vector lane utilisation, fixed control, and residual memory
terms rather than a single aggregate-demand comparison.

## Controlled Lane Sweep

This sweep holds all T8 parameters fixed—streaming mode, 32 descriptor and read
bytes/cycle, 16 write bytes/cycle, and every setup/latency parameter—and varies
only lanes. It uses the complete corpus, degree-priority policy, and the same
trace for both interface organizations. Relative change is versus the preceding
lane count.

| Lanes | Software batch cycles | Change | Mean lane utilisation | Structural cycles | Change | Mean lane utilisation |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 5,138,904 | — | 1.000 | 4,468,288 | — | 1.000 |
| 2 | 3,602,138 | −29.9% | 0.978 | 2,931,522 | −34.4% | 0.978 |
| 4 | 2,816,048 | −21.8% | 0.958 | 2,147,298 | −26.8% | 0.958 |
| 8 | 2,446,863 | −13.1% | 0.896 | 1,831,131 | −14.7% | 0.896 |
| 16 | 2,335,429 | −4.6% | 0.747 | 1,765,579 | −3.6% | 0.747 |
| 32 | 2,311,408 | −1.0% | 0.533 | 1,751,072 | −0.8% | 0.533 |

The visible knee is around 8 lanes in this model: 8→16 has a diminishing
modeled benefit and 16→32 is nearly flat. The flattening combines falling lane
utilisation from short vectors with fixed descriptor/control and memory terms.
Removing child descriptors shifts the curve downward but does not make 32 lanes
materially more effective. This evidence does not justify carrying a 32-lane
assumption into L2 without a different workload or a more detailed model.
