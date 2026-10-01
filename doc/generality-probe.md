# Generality probe: Bellman-Ford shortest paths

This checks whether PCAA's opcode set is expressible for a non-PBQP
min-plus workload, or accidentally PBQP-specific. It is a host-only,
non-PBQP-graph probe (`probes/`) with two paths: an independent software
oracle and a pcaalib client executing reductions through the hosted SystemC
model. It adds no RTL, ABI change, or new opcode. PBQP topology and
RN/branch-and-bound control flow are absent here, so whatever this probe *does* need is a generic
cost-algebra requirement, not a PBQP one.

## 1. What it computes and how

`probes/src/bellman_ford.cpp` implements the independent software oracle for
single-source shortest paths over a directed, weighted graph in the min-plus
semiring. `probes/tests/bellman_ford_device_unit.cpp` executes the same
vertex-local schedule through pcaalib's `pcaa_make_reduce2` builder,
`pcaa_device_submit`/`pcaa_device_wait`, guest-memory staging, and the SystemC
target socket. Each relaxation round does one
`MAP_ADD_REDUCE_MIN_ARGMIN` call **per vertex**,
over that vertex's incoming edges: `value[i] = cost_add(dist[u_i], w_i)`
for each predecessor `u_i`, then the minimum value and the edge that
achieved it (for path reconstruction). This is the natural vertex-local
framing of the min-plus relaxation `dist[v] = min(dist[v], min_u(dist[u] +
w(u,v)))` — not the more common textbook per-edge framing, chosen
specifically because it maps directly onto PCAA's existing reduce-to-a-scalar
shape instead of needing per-edge synchronization. `ACCEL_INF`
(`accel_protocol.h`) represents an unreached vertex; the oracle has its own
cost addition, independent from the accelerator implementation. The device
path runs in `probes_device_unit` and
compares the resulting distances and predecessors with the oracle for a
negative-weight detour, an unreachable vertex, a tie, and the road-distance
sample. It also verifies that finite negative underflow becomes a typed
device error, not a saturated result. Software-only GoogleTest cases
(`probes_unit`, in CTest) check negative-weight detours, unreachable vertices,
invalid sources and edge endpoints, reachable negative cycles (including one
whose vertices already sit at the `INT32_MIN` floor), saturated-but-acyclic distances, and — deliberately
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

## 3. Limits of the current mapping

Opcode 3 handles one vertex's incoming-edge reduction and returns the local
argmin index needed for predecessor reconstruction. The current probe retains
that vertex-local schedule and gathers predecessor distances in software.
It does not use opcodes 6–8.

ISA 1.0.0 now has vector-output projections, but they require a rectangular
affine matrix and shared vectors. A general edge list has varying in-degree
and arbitrary predecessor indices. Opcode 7 also returns values without
argmins; opcode 8 returns argmins, but still requires a regular matrix/vector
shape. Neither directly consumes a flattened edge list with destination ids.

A dense, padded adjacency matrix could express several vertices using opcode 8
with an additional zero vector and `INF` for absent edges. That changes staging
and processes absent edges. Using one shared distance snapshot also changes
the current in-place vertex-by-vertex relaxation schedule, so equivalence of
predecessor/tie behavior would need a separate test. The existing probe does
not establish that such a conversion is useful.

Finite negative underflow is another limit: it is a device error. The software
oracle retains saturated 32-bit distances only to study out-of-domain paths;
its exact 64-bit shadow detects negative cycles and distances below
`INT32_MIN`. Those cases are not successful accelerator workloads. An
unbounded-negative workload needs an input-range contract or wider arithmetic.

The public oracle validates `vertex_count`, `source`, and both endpoints of
every edge before relaxation. Invalid input returns all vertices unreached,
all predecessors -1, and both diagnostic flags false; no valid prefix is
partially relaxed. This is covered by `probes_unit`.

## 4. Possible larger operations

A segmented reduction over a flattened edge list could produce all vertex
results in one command, preserving separate reductions for each destination.
That would require a different operand contract (segment boundaries or
destination indices, and a predecessor gather), not merely increasing `m`
on an existing projection. It is not part of ISA 1.0.0 or this probe.

All-pairs shortest paths require another shape: for example,
`D[i,j] = min_k(D[i,k] + D[k,j])` has a matrix output over all `(i,j)` pairs.
Current opcode 8 produces one vector of independent reductions, not a complete
matrix/matrix product. The [historical primitive-shape study](reports/primitive-shape-study.md)
compares partial-vector and full-matrix work, but no full-matrix opcode was
adopted. This probe implements single-source Bellman–Ford only.

## 5. Limitations

- Host-only SystemC accelerator exercise plus independent software oracle;
  no timing, cycle, or speedup claim.
- Only single-source Bellman-Ford was implemented; Viterbi/HMM decoding and
  all-pairs shortest path are named as structurally similar or
  structurally different (respectively) but not implemented here.
- The probe adds no opcode, descriptor field, or ABI change. Segmented and
  matrix-output operations remain exploratory.
