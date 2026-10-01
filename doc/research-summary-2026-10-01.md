# PCAA research synthesis — 1 October 2026

## The result so far

The research supports a small, programmable accelerator for cost algebra, with
graph reduction and search controlled by software. The useful hardware work is
regular: add cost vectors, project a matrix against a vector, reduce sums to a
minimum, and retain a chosen coordinate when reconstruction requires it. The
useful software work is irregular: choose a reduction, update graph topology,
manage storage, branch, and decide when to stop. Keeping this boundary has made
it possible to improve command granularity without making the device depend on
PBQP or a particular solver strategy.

The implemented outcome is semantic ISA 1.0.0, exposed through pcaalib 2.0.0,
with runtime dimensions, affine strides, vector outputs, ordered batches, and
compact command encoding. There is a functional SystemC model, an RV64/Spike
integration, a shared PBQP solver, and a separate configurable L1 service-cycle
model. There is also a shortest-path probe that exercises the algebra outside
PBQP. These establish executable semantics and a basis for architectural
experiments. They do not establish silicon throughput, area, power, or
end-to-end acceleration on a physical system.

This report connects the research findings to the decisions they support. It
describes the state at **1 October 2026**; detailed measurements and their raw
evidence are in [reports](reports/README.md). The normative command contract
remains [the architecture specification](arch.md), and the adopted feature
set is recorded in [the ISA decision](isa-v1-decision.md).

## What the evidence measures

The experiments deliberately separate several quantities that can otherwise
look like interchangeable measures of performance:

| Quantity | What it establishes |
| --- | --- |
| Logical elements and operand/result bytes | The amount and shape of algebraic work required by the solver |
| Primitive descriptors | How that work is divided into accelerator commands |
| Top-level submissions | How often software hands an already constructed command stream to the device |
| Reduction and search traces | Where software decisions occur and how much work lies between them |
| L1 service cycles | A timing estimate under explicit lane, bandwidth, and stage-overlap assumptions |

Reducing submissions does not necessarily reduce operand traffic or arithmetic.
Reducing descriptors can help a descriptor-bound workload without changing its
mathematical work. Moving a software decision into hardware would change a
different boundary again. The studies are useful because they keep these
effects separate rather than treating every reduced count as a measured
speedup. Definitions and methodology are in [the design note](design.md) and
[the workload overview](workload-characterization.md).

The measurement history also matters. Most solver, scaling, batching, and
boundary studies used the earlier scalar implementation and 56-byte
descriptors. The first vector implementation used fixed 80-byte descriptors;
the current implementation uses 32-, 48-, or 64-byte commands. Historical
vector cycle ratios are projections from their historical baseline, not
measurements of today's compact stream. Current encoding measurements have
their own evidence.

All twelve measurement reports now have retained data and a documented
verification scope. Deterministic tables were replayed against their source
revisions; source inputs, traces, commands, and checksums are archived. The LLVM
graphs were re-extracted byte-identically. The branch-search report uses an
explicit replacement sample because the original real-input selection was not
retained. Time-limited partial traces remain evidence of the work observed
before interruption, not deterministic complete-tree measurements. These
checks make the reported logical results auditable; they cannot validate an
unimplemented hardware model. See [the evidence inventory](reports/data/README.md).

## Workload structure determines the opportunity

The synthetic studies reveal several distinct operating regimes. In the
degree-three family, RN heuristic decisions grow with graph size and each
decision exposes a relatively short exact-reduction cascade. In the measured
degree-four family, the min-degree and min-work policies require only two RN
decisions even as the graph grows to 1,000 nodes: most growth appears inside
the exact cascades. In the mixed-degree family, RN decisions occur throughout
the solve and the work rises much faster with graph size. Thus neither node
count nor domain size alone predicts submission frequency or useful work per
software decision.

Domain size still matters strongly. Matrix projections and three-input
reductions expand with the product of the participating domains. Increasing
domain size can make a small graph computationally substantial, while a large
graph with small domains can remain dominated by control and command overhead.
The reduction policy changes this balance as well as solution quality. A
policy that is attractive on one topology is not a universal architecture
parameter. These distinctions motivate reporting operation mix and cascades
alongside graph size. See [solver comparison](reports/solver-characterization.md)
and [scaling](reports/scaling-characterization.md).

The LLVM corpus provides an essential check on the synthetic picture. It
contains 491 register-allocation graphs, with a median of 16 nodes and a long
tail reaching 966. Domains are nonuniform in 475 graphs, or 96.7% of the
corpus. Interference matrices and spill costs also differ from random cost
tables: zero and infinity have application meaning, and available registers
make domain sizes uneven. A fixed-domain synthetic sweep cannot stand in for
this workload.

The real corpus combines substantial projection work with irregular control.
Forty-five graphs need no RN decision, but the median needs nine. In the
historical logical operation mix, matrix projection accounts for 62.1% of
elements and three-input map/reduction for 27.3%. This shifts the priority
toward efficient projections without making exact reductions irrelevant.
These are properties of the retained corpus and measured solver, not a claim
about every compiler or allocation target. See
[the LLVM corpus report](reports/llvm-corpus-characterization.md).

## Solver strategy and hardware capability are separate choices

Exact low-degree reductions are valuable because one software decision can
unlock a cascade of regular kernels. RN extends the solver to irreducible
graphs by making a heuristic choice, after which exact reductions resume.
Its result is an assignment, not a proof of optimality. Exact core enumeration
and exact branch-and-reduce provide that proof only when their searches finish.
The accelerator vocabulary serves all these strategies without owning the
choice between them.

Local search introduces another tradeoff: improving an assignment can require
many repeated node evaluations. Its initialization matters greatly. In the
degree-three, 100-node, four-choice scaling sample, standalone local search
averaged 627.6 sweeps, whereas RN followed by local search averaged 2.4. This
is a strong reason to retain the hybrid rather than interpret local search as
a replacement for reduction. The hybrid guarantees that it does not worsen
its RN seed; the small sweep counts observed in the study are not a bound on
arbitrary inputs. See [the local-search scaling study](reports/scaling-characterization.md#10-local-search-at-scale).

One shared solver core now serves host and bare-metal environments through
different storage policies and cost-kernel backends. This keeps comparisons
focused on execution and storage rather than two drifting algorithm
implementations. The bounded RV64 environment remains a deliberate deployment
configuration; larger host experiments do not justify silently increasing its
arenas. Public API and backend responsibilities are described in
[pcaalib](pcaalib.md).

## Vector commands address the strongest measured inefficiency

The original scalar commands divided a natural projection into too many small
descriptors. For a uniform domain of size D, RN projection required D scalar
reductions per incident edge. A vector-output projection expresses the same
work in one command. Similarly, the original R2 map/reduction required D²
scalar commands; the adopted partial vector form reduces this to D commands.
This changes command granularity while leaving the underlying algebra and
software reduction policy intact.

The measurements therefore support `MINPLUS_PROJECT`, vector cost addition,
and the vector-output three-input map/reduction. They do not support a
PBQP-specific RN instruction. RN scoring is composed from a projection and an
addition; conditioning still applies the selected matrix slice exactly once.
The same primitives remain usable by other algorithms. Argmin stays available
where a coordinate is needed, while value-only phases avoid unnecessary
reconstruction output. See [the primitive-shape study](reports/primitive-shape-study.md).

The historical L1 projection strengthens this choice but should be read at its
proper level. Across the LLVM corpus, combined partial vectorization reduced
modeled device service from an aggregate mean of 28,977 to 9,601 cycles per
solve, a 3.02× ratio. A full-matrix output variant projected 9,115 cycles, or
3.18×. The modest additional benefit in this model did not justify immediately
adding the larger interface and storage commitment. These are analytical
comparisons using the old scalar encoding, not observed current-system
speedups. See [the vector cycle projection](reports/vector-primitive-cycle-projection.md).

This reasoning also explains why every logical operation has not become an
opcode. A fused operation needs evidence that it removes a significant cost,
not merely that a software helper has a convenient name. Slice updates,
accumulation, staging, and CPU work remain visible in the accounting so that a
future offload proposal can be judged against an actual bottleneck.

## Batching, strides, and encoding solve different problems

Ordered per-node batching was selected because RN already knows the sequence
of projection/addition commands for one selected node. In the historical
controlled comparison, per-node and per-edge modes had identical scalar child
commands and results. Per-node batching reduced top-level submissions by
89.3% in the synthetic sweep and 78.4% in the LLVM corpus. It did not remove
the children or their operand work.

The distinction is visible in timing: the corresponding historical L1 model
estimated only a 1.97% device-service reduction for LLVM. CPU/MMIO handoff cost
was not modeled as physical latency, so this experiment cannot supply an
end-to-end batching speedup. The current default combines batching with vector
primitives; its total improvement must not all be attributed to batching.
`EXECUTE_BATCH` consequently remains a finite, ordered stream executor. It
does not discover dependencies, reorder work, or schedule across graph
reductions. See [the batching study](reports/batch-restructuring-study.md).

Affine strides were selected because both matrix orientations occur in real
work. Strided views account for 19.3% of measured LLVM accesses and substantial
shares of the synthetic families. Supporting only one contiguous orientation
would push transpose or packing costs into software and obscure the device's
actual requirements. Explicit strides describe the views without prescribing
a PBQP layout or a lane count. See
[the matrix-access study](reports/matrix-access-pattern-study.md).

Compact encoding addresses descriptor traffic separately. In four current
PBQP examples, it reduced total command bytes from 14,160 under the previous
80-byte representation to 7,840, a 44.6% reduction. The frequently observed
in-place vector addition receives a smaller encoding, while other commands
retain the fields they need. This is a measured byte saving on those examples;
it is not a demonstrated latency saving or a corpus-wide percentage.
Semantic builders keep algorithm code independent of this wire-format choice.
See [compact encoding measurements](reports/compact-encoding-measurements.md).

## Larger device autonomy remains a research question

The HW/SW boundary study asks what would happen if a device could execute more
than regular kernels. Grouping each exact reduction is one hypothetical
boundary; grouping an RN decision and its ensuing exact cascade is another.
For the 20-node degree-four sample under min-degree, the latter reduces the
logical handoff count from 20 to two. The corresponding degree-three count
falls from 20 to five. These differences show that topology determines the
possible control amortization.

They do not establish that graph-aware hardware is worthwhile. Such a device
would need graph state, topology updates, reconstruction data, allocation, and
an execution policy. Logical epoch sizes omit the cost of implementing or
moving that state. The current ISA therefore takes the measured advantage of
regular vector kernels and explicit batches without adopting the hypothetical
graph boundary. See [the boundary study](reports/hw-sw-boundary-characterization.md).

Exact search offers a second possible source of parallelism, but its evidence
is also incomplete. Incumbent-based pruning is now implemented using the
accumulated objective plus a sum of remaining unary and edge minima. The bound
is valid for negative costs; assuming nonnegative costs would make pruning
unsound. In the explicit rechecked sample, all 26 synthetic searches completed,
and 37 of 39 real searches completed within the cap. Pruning varied widely,
and two real cases timed out. A strong median reduction in visited work does
not remove the difficult tail. See [branch-and-bound](reports/branch-bound-characterization.md).

The fork study measures branch-domain width and combines it with a heuristic
estimate of work per child. Typical synthetic forks expose two to four choices;
the fork-weighted real median is seven. This indicates opportunities to
investigate independent branches, but it does not measure simultaneous
frontier width, worker utilization, snapshot bandwidth, or memory demand. A
depth-first open-node bound is not a measurement of useful parallel occupancy.
Consequently there is no adopted graph-worker, snapshot, or search-scheduler
instruction. See [fork parallelism](reports/fork-parallelism-characterization.md).

## Integer cost semantics remain justified, with explicit limits

Signed integer costs with an explicit infinity give deterministic comparisons,
tie breaking, and reduction behavior. They also impose a finite range. Positive
sums saturate to infinity; negative underflow is an error. PBQP construction
restricts finite inputs to a capacity-dependent safe range so that alternative
reduction orders remain mathematically consistent. Infinity is a valid cost,
never a device-failure sentinel. These constraints belong to the contract and
must survive any future implementation refinement.

The fixed-point study found useful precision in workloads beyond register
allocation. Recomputing all 733,846 arcs in the New York road dataset showed
that millimetre units represent individual distances with at most half a
millimetre quantization error. The limiting factor is accumulation: the
positive cost budget at that scale is approximately 536.87 km. Applications
with longer paths need a different scale or a carefully designed rebasing
scheme. Log-probability samples likewise support quantization, while leaving
long-sequence range management unresolved. They are not a complete Viterbi
implementation. See [the cost-representation study](reports/cost-representation-study.md).

FP32 is therefore deferred rather than assumed necessary. It could extend
dynamic range, but it would introduce rounding-order questions into reductions,
objective comparisons, and bounds, as well as different arithmetic hardware.
Published energy and FPGA examples offer context; they are not local PCAA
synthesis measurements and cannot determine its area or energy. An FP32
proposal needs an application that cannot meet its accuracy and range needs
under an explicit integer policy, together with reproducible numerical and
implementation evidence.

## Generality has been demonstrated at the kernel boundary

The Bellman–Ford probe submits shortest-path cost operations through pcaalib
and the SystemC device and checks them against an independent software oracle.
It covers negative weights, unreachable vertices, ties, and error propagation.
This is useful evidence that the min-plus vocabulary is not an encoding of
PBQP graph rules.

The probe also exposes a boundary: irregular incoming edges are gathered by
software before the regular reduction is submitted. It does not demonstrate
hardware graph traversal, segmented reduction, or an efficient complete
shortest-path engine. The distinction matters when arguing for new commands:
kernel reuse is established, while whole-application acceleration still needs
measurement. See [the generality probe](generality-probe.md).

## The decisions and the next experiments

The adopted design follows the strongest common evidence: retain generic
integer cost algebra, make natural projection outputs vector-shaped, expose
strides, batch software-known sequences, and reduce their descriptor footprint.
Keep graph control and search outside the device. Preserve semantic commands
across host and RV64 backends so that encoding and timing can evolve without
rewriting the algorithms.

The next useful experiments should resolve the remaining costs rather than
repeat historical descriptor ratios:

1. Measure the current compact/vector path on the retained synthetic and LLVM
   workloads, separating device service from CPU decisions, MMIO, and packing.
2. Measure intermediate-value traffic and reuse before committing to local
   vector registers or additional fused commands. The
   [local-register proposal](isa-2x-local-vector-rf.md) remains an exploration.
3. If search parallelism is pursued, measure live frontier occupancy, snapshot
   traffic, memory capacity, and pruning interactions before selecting workers
   or a device scheduling interface.
4. Extend independent application probes where they test a concrete missing
   capability or numerical limit, rather than treating one successful kernel
   mapping as proof of universal min-plus acceleration.
5. Refine the timing model toward a pipelined implementation and eventually RTL
   while preserving functional semantics and untimed correctness tests. The
   [L1 model](l1-performance-model.md) is an architectural estimate; L2 and L3
   are planned refinement stages, not completed hardware results.

The research has produced a defensible initial interface and reproducible
evidence for its main choices. Its remaining uncertainty concerns physical
execution costs and the value of deeper autonomy. Those questions now have
specific workloads and measurements to guide them.
