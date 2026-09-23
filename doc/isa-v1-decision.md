# PCAA ISA 1.0.0 decision

ISA 1.0.0 is an ordered, descriptor-driven cost-algebra engine. Software owns
PBQP topology, graph reductions, allocation, search, and scheduling. The
block owns only regular cost operations over runtime-sized affine views.
The pre-decision evidence index remains [isa-decision-inputs.md](isa-decision-inputs.md).

Semantic commands are distinct from their descriptor representation.
`pcaalib` owns the semantic builders and current 80-byte descriptor codec;
the functional engine consumes decoded commands. A later compact encoding
can replace this codec without changing PBQP scheduling or arithmetic; it
need not coexist with the current encoding. See [the architecture](arch.md#5-semantic-commands-and-descriptor-encoding)
for the descriptor contract and [pcaalib](pcaalib.md) for the library API.

## Primitive set and representation

Opcodes 1–5 retain their arithmetic and control semantics: two- and three-input
scalar-output map/min (with or without argmin), and finite `EXECUTE_BATCH`.
Opcodes 6–8 are `COST_ADD_VECTOR`, `MINPLUS_PROJECT`, and
`MINPLUS_MAP3_PROJECT`. The last writes a vector of independent
`{int32_t value, uint32_t index}` reductions for one software-fixed R2
coordinate, not a full matrix. RN scoring uses one projection and one vector
add per edge in an ordered batch; R2 uses one MAP3 descriptor per fixed
coordinate. R1 and local-search argmin retain baseline scalar-output forms.
Local-search slice accumulation remains software-side: making its current
per-coordinate score update a device operation would add staging and control
complexity without a descriptor-count gain demonstrated by the evidence.

Every descriptor is 80 bytes: the original 56-byte prefix, then six 32-bit
element strides at offsets `0x38` through `0x4c`. The prefix offsets and
meaning for opcodes 1–5 are unchanged, although producers and consumers must
agree on the new descriptor size. See [arch.md](arch.md#5-command-descriptor)
for exact offsets, dimension rules, and operand formulas. Affine addressing
expresses rows, columns, and padding without row-major/transpose modes.
There is no architectural lane width.

All multi-byte guest-memory quantities are little-endian. Valid costs are
finite signed 32-bit values below `INF = INT32_MAX / 4`, or exactly `INF`;
larger values cause `ERROR` even when another operand is `INF`. Valid `INF`
absorbs addition, positive finite sums saturate to `INF`, and finite underflow
below `INT32_MIN` causes `ERROR`. Within each independent reduction, the first
minimum index wins. A vector command may have written earlier output elements
when it fails; no-error atomicity is promised. `COST_ADD_VECTOR` allows exact
full-view destination aliasing with an input; other output/input address-span
overlap is invalid. Opcode 7–8 outputs may not overlap inputs. Opcodes 6–8
require `flags`, `k`, and `reserved` to be zero, while unused source addresses
and strides are ignored. A successful child batch write is visible to the
immediately next child, but neither child outputs nor the batch result may
overlap child or top-level descriptor bytes. Batches remain ordered, fail-stop,
non-transactional, and unnested.

## Evidence and exclusions

| Omitted from ISA 1.0.0 | Reason |
| --- | --- |
| Full-matrix MAP3 output | The [cycle projection](vector-primitive-cycle-projection.md) shows a small incremental corpus gain over partial-vector output; buffering and interface cost are not justified. |
| Fused `PROJECT_ACCUMULATE` and `SLICE_ACCUMULATE` | The [primitive-shape study](primitive-shape-study.md) identifies these as software-only arithmetic, not descriptor-count bottlenecks; ordered projection→vector-add already composes the needed RN operation. |
| PBQP graph, branch/frontier, and snapshot instructions | The [fork study](fork-parallelism-characterization.md) exposes irregular graph-dependent state and uncertain frontier width. It supports keeping this control in software, not encoding it in the cost engine. |
| Architectural workers or lanes | The [cycle projection](vector-primitive-cycle-projection.md) uses lanes as a model parameter; runtime work size, not implementation parallelism, is the ISA contract. |
| Row/column layout modes | The [access-pattern study](matrix-access-pattern-study.md) finds both contiguous and strided views; affine element strides directly cover both and padded matrices. |
| FP32 costs, negative saturation, graph-wide overflow | The [cost study](cost-representation-study.md) finds no ISA 1.0.0 case compelling enough to replace exact signed-int32/INF behavior, and explicitly identifies order-sensitive arithmetic concerns. |
| Cross-output argmin tie semantics | Outputs are independent reductions; only the existing first-index tie within each output is meaningful. |
| Nested batches or dependency scheduling | Ordered producer-consumer visibility supplies the needed composition without a new scheduler or graph protocol. |

Historical characterization reports describe the pre-1.0.0 scalar descriptor
mix. New counters separately report scalar projects, vector projects, vector
adds, scalar MAP3, and partial-vector MAP3; do not reinterpret old CSV rows
as measurements of the new ISA.
