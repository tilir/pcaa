# Primitive-shape / descriptor-count study

This is a descriptor-count and data-shape analysis only: how many scalar
primitive descriptors the current solver issues per logical operation, and
how that would change under a *hypothetical* vector-output primitive. It
does not implement any new primitive in the SystemC model, does not change
`accel_protocol.h`, and does not estimate cycles, area, or speedup.

## 1. Method

For each of the five generic operation categories, the exact call site in
`software/pbqp/pbqp.cpp` was read to determine, per logical operation,
how many scalar descriptors it issues today and what each one computes —
not inferred from the doc prose, from the code directly. The D-scaling was
then cross-checked empirically against the `D = 2, 4, 8, 16` domain-size
sweep already collected in `build/scaling-runs.csv`
(`scaling-characterization.md`'s corpus), at fixed N=100, for degree-3,
degree-4, and mixed-degree.

## 2. What each category actually does today

### MINPLUS_PROJECT (RN scoring): D scalar descriptors per edge

`ReduceRN` (`pbqp.cpp:701`), for the node being scored: for every edge
incident to it, `for (unsigned value = 0; value < node.domain; ++value)`
builds one `pbqp_min2_value_job_t` and submits it as a
`MAP_ADD_REDUCE_MIN`-shaped primitive (batched via `kernel_.Min2ValueBatch`,
one batch submission per edge, but each of the `node.domain` jobs inside it
is still one primitive descriptor — `rn_projection_primitives` counts
exactly these, confirmed by the empirical check below). So **one length-D
RN scoring pass against one neighbor costs D scalar descriptors today**,
each doing a length-`neighbor.domain` reduction.

A hypothetical vector-output primitive — one descriptor, taking the whole
neighbor unary vector and the node's conditioned matrix slice, producing
the length-D score-contribution vector in one call, with the D reductions
done internally — would cost **1 descriptor per edge** instead of D.

**Ratio: D**, confirmed empirically (`projection_primitives / D` is exactly
constant across the whole D=2..16 sweep for every family — 75.0 for
degree-3, 7.0 for degree-4, 1502.2 for mixed-degree, all at N=100 — because
`projection_primitives = D * (sum of RN branch-node degrees over the whole
solve)`, and RN decisions/degree don't depend on D, per
scaling-characterization.md §4).

### PROJECT_ACCUMULATE: 0 descriptors today — pure host arithmetic

Immediately after each edge's batched projection call, `ReduceRN`
(`pbqp.cpp:760-764`) folds the D just-computed results into the running
score vector with a plain host loop —
`scores[value] = accel_cost_add(scores[value], results[value])` — with
**no kernel call at all**. `project_accumulate_elements` is a purely
logical/accounting counter; today's implementation never submits a
descriptor for it. A fused "project-and-accumulate" primitive (project a
neighbor's contribution *and* fold it into the running score in one device
call) would not just collapse D descriptors into 1 like PROJECT above — it
would also move work that is *entirely host-side today* onto the device
for the first time. This is a different, larger kind of change than a
simple per-element-to-per-vector collapse.

### MAP3_REDUCE (R2 elimination): D² scalar descriptors per elimination

`ReduceR2` (`pbqp.cpp:873`) eliminates a degree-2 node between two
neighbors of domain `first.domain`/`second.domain`: for every
`(first_value, second_value)` pair — `first.domain * second.domain`
descriptors total — it submits one `MAP_ADD3_REDUCE_MIN_ARGMIN` descriptor
that reduces over the *eliminated* node's domain. For a uniform domain D,
that's **D² scalar descriptors per R2 elimination**, matching the prompt's
own expectation that an R2-style reduction exposes work proportional to a
product of three domain sizes (`D * D * D` total elements: D² descriptors
of length D each).

Two different hypothetical primitives collapse this differently:

- **Partial vector-output** (fix one dimension, vectorize the other — one
  descriptor per `first_value`, producing the length-`second.domain`
  output row in one call): D descriptors instead of D², **ratio D** — the
  same scaling as PROJECT.
- **Full matrix-output** (one descriptor for the whole `first.domain x
  second.domain` result/argmin matrix): 1 descriptor instead of D²,
  **ratio D²** — a structurally different, higher-generality primitive
  (a real matrix-valued output, not just a longer vector), the same
  generality tier item 6 flags separately for all-pairs shortest path.

Empirically: `r2 * D^2` (the D² hypothesis) tracks the actual
`map3_reduce`-related descriptor count exactly — for degree-3 at N=100 it
is 292, 1,168, 4,672, 18,688 at D=2,4,8,16, each a factor of exactly 4.00x
the previous (D² doubling when D doubles), confirming the quadratic
scaling directly rather than assuming it.

### ARGMIN_VECTOR (local search per-node evaluation): already 1 descriptor — ratio 1

`RunLocalDescent` (`pbqp.cpp:523`) calls `kernel_.Min2(score_view,
zero_view, &best)` **once** per node evaluation, over the whole
length-`node.domain` score vector, producing one argmin result in one call.
This category is already vector-shaped today — `argmin_vector_descriptors`
increments exactly once per node evaluation regardless of D. **There is no
further descriptor-count collapse available here**; a hypothetical vector
primitive would not change this category's descriptor count, only (if
fused with SLICE below) eliminate host-side accumulation feeding it.

### SLICE_ACCUMULATE: 0 descriptors today — pure host arithmetic, same as PROJECT_ACCUMULATE

The per-edge slice fold inside `RunLocalDescent` (`pbqp.cpp:542-563`,
`for (unsigned value...) scores[value] = accel_cost_add(scores[value],
slice.base[...])`) is, like PROJECT_ACCUMULATE, a plain host loop with no
kernel call. `slice_accumulate_operations`/`slice_accumulate_elements` are
logical-only counters. A fused primitive here has the same character as
PROJECT_ACCUMULATE: it would move currently host-only work onto the
device, not just shrink an existing descriptor count.

## 3. Descriptor-count ratio vs D (current / hypothetical vector primitive)

| Category | Current descriptors (per logical op) | Hypothetical vector primitive | Ratio vs D |
|---|---|---|---|
| MINPLUS_PROJECT | D (per edge) | 1 (per edge) | **D** (confirmed empirically, exact) |
| PROJECT_ACCUMULATE | 0 (host-only) | 0 -> 1 if fused into PROJECT | n/a — currently no device descriptor to shrink |
| MAP3_REDUCE | D² (per R2 elimination) | D (partial-vector) or 1 (full-matrix) | **D** or **D²** depending on primitive generality (confirmed empirically for the D² baseline) |
| ARGMIN_VECTOR | 1 (per node evaluation) | 1 | **1** — already vector-shaped |
| SLICE_ACCUMULATE | 0 (host-only) | 0 -> 1 if fused | n/a — currently no device descriptor to shrink |

As D grows 2->4->8->16, the descriptor-count reduction a vector-output
PROJECT primitive would offer grows linearly (D); the reduction a
matrix-output MAP3 primitive would offer grows quadratically (D²) — R2
elimination is where a richer primitive would collapse the most descriptor
traffic, by a wide and widening margin, consistent with MAP3 already
dominating total logical elements at high D for the fixed-degree families
(scaling-characterization.md §4/§6: 86-99% of total elements at D=8).

## 4. Limitations

- This is a descriptor-*count* analysis, not a cycle, latency, or area
  estimate — no claim is made about whether collapsing D or D² descriptors
  into one is worth its own added complexity.
- PROJECT_ACCUMULATE and SLICE_ACCUMULATE's "no current descriptor" finding
  is a fact about *this* implementation's kernel-call boundary, not a
  statement that the corresponding arithmetic is free — it is real host
  CPU work, just not currently expressed as an accelerator primitive.
- No new primitive, opcode, or descriptor field was implemented or added to
  `accel_protocol.h`; this is purely an analytical projection from existing
  counters and code reading.
