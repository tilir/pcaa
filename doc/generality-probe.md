# Generality probe: Bellman-Ford shortest paths

This checks whether PCAA's opcode set is expressible for a non-PBQP
min-plus workload, or accidentally PBQP-specific. It is a host-only,
non-PBQP-graph probe (`probes/`) — no RTL, ABI change, SystemC use, or new
opcode. PBQP's topology and RN/branch-and-bound control flow are absent
here on purpose, so whatever this probe *does* need is a generic
cost-algebra requirement, not a PBQP one.

## 1. What it computes and how

`probes/src/bellman_ford.cpp` implements single-source shortest paths over
a directed, weighted graph in the min-plus semiring. Each relaxation round
does one conceptual `MAP_ADD_REDUCE_MIN_ARGMIN`-shaped call **per vertex**,
over that vertex's incoming edges: `value[i] = cost_add(dist[u_i], w_i)`
for each predecessor `u_i`, then the minimum value and the edge that
achieved it (for path reconstruction). This is the natural vertex-local
framing of the min-plus relaxation `dist[v] = min(dist[v], min_u(dist[u] +
w(u,v)))` — not the more common textbook per-edge framing, chosen
specifically because it maps directly onto PCAA's existing reduce-to-a-scalar
shape instead of needing per-edge synchronization. `ACCEL_INF`
(`accel_protocol.h`) represents an unreached vertex; `accel_cost_add`
(`pcaa_cost_math`, the same function PBQP's own solver uses) does the
add-with-saturation. Eight GoogleTest cases (`probes_unit`, in the main
CTest suite) check a negative-weight detour, an unreachable vertex, a
reachable negative cycle (also one whose vertices already sit at the
`INT32_MIN` floor), saturated-but-acyclic distances, and — deliberately
constructed so the tie is presented to a single argmin call rather than
resolved by relaxation-round ordering — a genuine tie, confirming PCAA's
"first equal minimum" rule produces a valid predecessor.

## 2. Which of opcodes 1-4 it could use as-is

**`MAP_ADD_REDUCE_MIN_ARGMIN` (opcode 3) fits directly**, with no
reinterpretation: `src0` = predecessor distances, `src1` = edge weights,
`n` = in-degree, `dst` = `{value, index}`. The index needs one host-side
translation PBQP's own driver code already does for its own reconstruction
arrays: opcode 3's `index` is local to the `n`-length arrays passed in
(the vertex's incoming-edge list), so the host maps it back to an actual
predecessor vertex id — the same "local choice index -> reconstructed
global choice" pattern PBQP's R1/R2 reconstruction already uses. No other
adaptation is PBQP-specific about this mapping.

`MAP_ADD_REDUCE_MIN` (opcode 1, value-only) would suffice if only
distances were needed, without path reconstruction — Bellman-Ford's
distance-only variant is a strict subset of what opcode 3 already covers.
Opcodes 2/4 (`ADD3`) are not naturally used: relaxation here is a two-input
map (predecessor distance + edge weight), not three.

## 3. Where it needs something different from opcodes 1-4

Nothing about *arity or tie-breaking* needed to change — opcode 3's
two-input, argmin-with-first-tie-wins shape was sufficient exactly as
specified for PBQP. The one real gap is **granularity, not shape**: today,
one call to this primitive handles one vertex's relaxation (`n` = that
vertex's in-degree). A full Bellman-Ford round relaxes every vertex, i.e.
issues one such call *per vertex per round*, exactly mirroring PBQP RN
scoring's "one scalar-shaped op per graph decision" pattern (see
[primitive-shape-study.md](primitive-shape-study.md)) — this is a property
of the current opcode set's per-vertex granularity, not of PBQP itself:
any min-plus DP with an irregular, per-node fan-in (Viterbi/HMM decoding
has the same shape) would hit the same one-vertex-at-a-time limit.

**The cost representation, not the opcode shape, is where this probe hit a
real limit.** PCAA's `cost_add` saturates finite sums at `INT32_MIN`. For
PBQP this is harmless: `pbqp_max_finite_cost` restricts inputs so no
reduction can reach the floor. Shortest paths have no such guarantee — a
negative cycle keeps lowering distances — and once a cycle's vertices sit
at `INT32_MIN`, saturating addition leaves them there, so a relaxation
built only from PCAA arithmetic cannot tell "still decreasing" from
"converged". The probe therefore answers negative-cycle detection (and
flags distances below `INT32_MIN` as `distance_saturated`) with a separate
exact 64-bit relaxation, outside anything PCAA would compute. A non-PBQP
workload with unbounded negative accumulation needs either an input-range
contract like PBQP's or a wider cost type/overflow flag; the current
architecture offers neither.

## 4. Would it benefit from the item-5 vector-output primitive, or needs something structurally different?

**Yes, the same way RN projection would.** A hypothetical primitive that
relaxes *all* vertices of a round in one descriptor — taking a
flattened edge list (predecessor distances, weights, and a
destination-vertex index per edge) and producing the whole updated
distance/predecessor vector via internal segmented reduction — would
collapse "one descriptor per vertex per round" to "one descriptor per
round." That is architecturally the *same* generalization
`primitive-shape-study.md` §3 already identifies for RN's PROJECT category
(a length-D output computed from many independent reductions in one call);
Bellman-Ford is further evidence it is a generic cost-algebra need, not a
PBQP one.

**All-pairs shortest path is a different, higher generality tier**, and
this probe deliberately did not implement it: all-pairs (Floyd-Warshall,
or repeated Bellman-Ford squaring) is a genuine **matrix-matrix min-plus
product** — `D[i][j] = min_k(D[i][k] + D[k][j])` over *all* `(i,j)` pairs
at once, an `O(V^3)`-shaped operation with a full matrix output, not a
vector one. This is exactly the same distinction
`primitive-shape-study.md` §2 draws for PBQP's own R2/MAP3_REDUCE between
a "partial vector-output" primitive (ratio D) and a "full matrix-output"
primitive (ratio D^2) — all-pairs shortest path would want the matrix-output
tier, single-source Bellman-Ford only needs the vector-output tier. Do not
conflate the two when scoping a future primitive: single-source (this
probe) motivates a vector primitive; all-pairs would motivate a
structurally different, larger one.

## 5. Limitations

- Host-only reference implementation; no timing, cycle, or speedup claim.
- Only single-source Bellman-Ford was implemented; Viterbi/HMM decoding and
  all-pairs shortest path are named as structurally similar or
  structurally different (respectively) but not implemented here.
- No new opcode, descriptor field, or ABI change was made or proposed;
  this is evidence for a later ISA discussion, not that discussion itself.
