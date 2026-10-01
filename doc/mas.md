# PCAA Microarchitecture Specification (MAS) 1.0.0

```text
MAS version: 1.0.0
Implements: ISA 1.0.0
```

## 1. Scope, status, and versioning

This specification selects the first PCAA microarchitecture for an L2
approximately timed model and subsequent RTL implementation. It specifies one
command context, one primitive engine, bounded operand storage, tiled
projections, and sequential batch control. It does not claim that L2 or RTL
already exists. The implemented reference remains the functional model with
optional analytical L1 timing.

The software-visible authority is [arch.md](arch.md), with the adopted feature
set explained in [isa-v1-decision.md](isa-v1-decision.md). MAS implements their
existing MMIO, compact encoding, arithmetic, aliasing, and completion contracts.
It adds no opcode, descriptor field, software-visible storage, register, or
completion code. Implementation ordering choices below select behavior within
the ISA; they do not create software guarantees about latency or error outputs.

In this document, **must** denotes a normative implementation obligation.
Unqualified execution schedules describe the selected MAS design. The baseline
configuration in section 14 is the initial L2/RTL bring-up profile; its numerical
parameters are tunable. Paragraphs marked **Rationale**, **Reference behavior**,
or **Future experiment**, and the explicit L1 comparison, distinguish evidence
from design.

MAS and ISA are independently versioned using SemVer:

- **MAJOR:** incompatible change to documented microarchitectural contracts.
- **MINOR:** compatible extension or refinement of the microarchitecture.
- **PATCH:** textual correction or clarification without an intended
  implementation change.

A future MAS version need not have the same version number as the ISA it
implements. Parameter sweeps within this specification are configurations,
not ISA revisions. The library's `pcaa_version()` is separately versioned;
`pcaa_isa_version()` continues to identify semantic ISA 1.0.0.

## 2. Design goals and boundary

The design must support architectural dimensions through 65,535 by iteration,
without storage proportional to the maximum matrix or batch length. It uses
data parallelism within a primitive and explicit backpressure, with a direct
path to finite-state controllers, registers, small SRAMs, and arithmetic lanes.
The implementation must preserve exact arithmetic grouping and first-index
argmin under all permitted lane and tile sizes.

Software supplies all work and its order. The engine receives guest physical
byte addresses and accesses them through a memory interface. It has no graph
topology, allocation policy, worker, frontier, or autonomous work discovery.
Internal tile buffers are temporary copies with no software names or lifetime
across commands; they are not the architectural vector RF proposed in
[the deferred ISA exploration](isa-2x-local-vector-rf.md).

## 3. Top-level decomposition

```text
 CPU MMIO
    |
    v
 +---------------------+      +------------------------+
 | MMIO/control        |<-----| completion/error       |
 | address + STATUS    |      | stop, drain, publish    |
 +----------+----------+      +-----------^------------+
            | latched submission          |
            v                             |
 +---------------------+      +-----------+------------+
 | descriptor frontend |<---->| batch sequencer        |
 | fetch/decode/check   |      | cursor/count/end/result|
 +----------+----------+      +------------------------+
            | one canonical primitive
            v
 +----------------------------------------------------+
 | primitive dispatcher/controller                    |
 | scalar/vector loops | projection tile controller   |
 +---------+----------------------------+-------------+
           |                            |
           v                            v
 +-----------------+        +-------------------------+
 | address         |        | operand banks -> lanes  |
 | generation      |        | -> reduction states     |
 +--------+--------+        +-------------+-----------+
          |                               | results
          v                               v
 +----------------------+       +---------------------+
 | memory request /     |<------| writeback           |
 | response adapter     |       | pending data/address|
 +----------+-----------+       +---------------------+
            |
     guest physical memory
```

The descriptor frontend and execution engine share one memory adapter. The
frontend owns a 64-byte fetch buffer and a decoded command latch. The batch
sequencer retains the parent context while that buffer is reused for each
child. The dispatcher selects loop bounds, source routing, ADD2/ADD3, reduction
enable, and result width. Address generation handles affine views and checked
byte spans; it does not infer layouts.

The projection controller holds output-tile and input-chunk coordinates.
Operand banks feed the same cost lanes used for scalar reductions and vector
addition. The reduction unit maintains value/index pairs. Writeback serializes
architectural little-endian results. Completion collects validation, memory,
and arithmetic failures, prevents further work, and waits for all accepted
requests to retire before releasing the context.

No block independently schedules another command. A child is a use of the
same engine, not a second architectural context.

## 4. Command lifecycle and MMIO

The four 32-bit registers, access restrictions, and status values are exactly
[architecture section 6](arch.md#6-mmio-control-interface). In particular,
doorbell data is ignored; invalid offsets, reads of `DOORBELL`, writes of
`STATUS`, byte enables, bursts, and non-32-bit accesses are transport errors.
They do not submit work or replace an active command's status.

The control state machine is:

```text
initial IDLE / retained DONE / retained ERROR
  -> accept doorbell, latch descriptor address, publish BUSY
  -> FETCH_HEADER -> FETCH_BODY -> VALIDATE
  -> primitive EXECUTE -> WRITEBACK -> DRAIN -> publish DONE
                         any failure -> STOP -> DRAIN -> publish ERROR
```

`BUSY` covers fetching, validation, execution, and all result acknowledgements.
The internal ready state retains the last `DONE` or `ERROR` until a new
submission; it must not erase completion by automatically publishing `IDLE`.
Only initialization produces the initial `IDLE` state. There is no new reset
register. Initialization clears internal valid bits and outstanding state.

An accepted doorbell snapshots both address halves. Subsequent address-register
writes update the submission registers, not that active snapshot. A doorbell
while `BUSY` is rejected as a control transaction without disturbing the active
context, matching the reference frontend. Status reads remain serviceable
while the engine waits on memory. The host must obey the existing submission
and result-consumption fences; this design adds no coherent-memory contract.

For a batch, validation enters `BATCH_NEXT -> FETCH_HEADER -> FETCH_BODY ->
VALIDATE_CHILD -> EXECUTE -> CHILD_DRAIN`. A successful child returns to
`BATCH_NEXT`. The final stream check and batch-result write precede terminal
status. Child failure enters stop/drain, then attempts the failure record,
then publishes `ERROR`. The parent remains `BUSY` throughout; child completion
never publishes top-level `DONE`.

**Reference behavior:** [accelerator.cpp](../accelerator/src/accelerator.cpp)
currently executes the entire operation during doorbell processing. L2 may
make `BUSY` observable over time. Software already must treat completion as
asynchronous; submit and wait remain separate.

## 5. Descriptor frontend and validation

The frontend performs these steps for the top-level command or current child:

1. Check nonzero descriptor address and checked header span. For a child,
   require at least eight remaining stream bytes before issuing a read.
2. Fetch exactly the eight-byte common header into the descriptor buffer.
3. Decode the opcode/format pair and derive its one legal encoded size.
   Reject unsupported or incompatible pairs before fetching the body.
4. Check the full descriptor span and, for a child, that its size fits the
   remaining `child_bytes`. Fetch exactly the remaining 24, 40, or 56 bytes.
5. Validate flags, reserved bytes, dimensions, strides, required addresses,
   output kind, spans, alias restrictions, and batch-specific conditions.
6. Latch the canonical command and dispatch only after validation succeeds.

| Opcode | Format(s) | Encoded bytes |
| --- | --- | --- |
| 1, 3 | `REDUCE2`, `REDUCE2_ARGMIN` respectively | 32 |
| 2, 4 | `REDUCE3`, `REDUCE3_ARGMIN` respectively | 48 |
| 5 | `EXECUTE_BATCH` | 32 |
| 6 | `COST_ADD_VECTOR_GENERAL`, `COST_ADD_VECTOR_INPLACE` | 48, 32 |
| 7 | `MINPLUS_PROJECT` | 48 |
| 8 | `MINPLUS_MAP3_PROJECT` | 64 |

The exact offsets remain in [architecture section 5](arch.md#5-semantic-commands-and-descriptor-encoding)
and [accel_protocol.h](../accelerator/include/accel_protocol.h). There is no
fixed descriptor stride, speculative next-header fetch, or 64-byte overfetch.
The eight-byte header is not fetched again with the body. Slot-sized encodings
do not impose a new address-alignment requirement.

Validation must match [the codec](../pcaalib/src/codec.c), including its
canonical-format check: opcode 6 with an exact full-view destination alias
uses the in-place format; a general-format wire descriptor encoding that alias
is rejected by the current decoder. In-place decoding reconstructs `dst ==
src0` and equal strides. The source swap for a semantic `dst == src1` is a
codec operation, not another hardware opcode. Opcodes 1–4 have implicit unit
strides, `m == 0`, and nonzero `n`; opcode 5 has `m == n == 0`; opcode 6 has
`m == 0`; opcodes 7–8 have both dimensions nonzero. Every reserved byte and
flags field must be zero.

Descriptor validation reads no operand costs. Batch child output protection
uses the preflight scan in section 11 before any child operand execution.
The frontend never validates future children early: a malformed later child
must not suppress successfully completed earlier children.

## 6. Memory subsystem and address generation

### 6.1 Request/response contract

The conceptual memory port is independent of AXI, TileLink, or a particular
SoC. A request contains operation (read/write), 64-bit physical byte address,
byte count, write data where applicable, and an internal destination tag. A
response identifies that request and supplies read data or write completion,
plus success/failure. Tags route data to descriptor bytes, operand bank/offset,
or writeback; they are not architectural fields.

Requests and responses use ready/valid handshakes. A producer must hold the
request stable until accepted. Each accepted request gets exactly one terminal
response; its storage cannot be reused earlier. Read failure data is invalid.
Successful write completion means the bytes are committed and visible to a
later device read, not merely queued in a posted-write buffer. An integration
with weaker bus acknowledgements must implement the required drain/barrier
inside the adapter.

The bring-up profile has **one accepted memory request total outstanding**,
shared by descriptor reads, operand reads, and result writes. It issues the
next request after the previous response. Fetch, operand fill, arithmetic,
and writeback phases do not overlap in this profile. This deliberately simple
schedule is implementable without a reorder buffer. Queue and concurrency
parameters may be increased while preserving all dependencies, bounded
credits, failure draining, and the no-cross-child barrier. Such configurations
must state their arbitration and intra-primitive overlap explicitly.

There is no automatic retry, timeout completion, speculative access beyond
the active primitive, or cache in the baseline. Eventual completion assumes
the memory adapter eventually answers accepted requests. In a future L2
implementation, physical transactions must still use `MemoryInterface::read`
and `write`; modeled request state and delay do not authorize dereferencing
physical addresses as host pointers. MMIO continues through `b_transport`.

### 6.2 Affine accesses and bounds

For element width `w`, positive strides, and dimensions `r,c`, compute:

```text
offset_last = (r - 1) * outer_stride + (c - 1) * inner_stride
end         = base + w * (offset_last + 1)     // exclusive end
address(i,j)= base + w * (i * outer_stride + j * inner_stride)
```

Use checked wide intermediates; never allow 16- or 32-bit arithmetic to wrap
before extension. The decoded u16 products and their sum fit in 64 bits;
base-plus-span validation needs a carry bit or equivalent subtraction checks.
As in the codec, the exclusive end itself must fit in u64. An interval ending
at mathematical `2^64` is rejected. Batch cursor/end arithmetic receives the
same checks. Loop counters must represent terminal counts and tile increments
without u16 wrap; the baseline uses 32-bit primitive counters and 64-bit byte
cursor arithmetic, with a checked u32 child count/index.

Costs have `w=4`; minimum/argmin outputs and the batch record have `w=8`.
Opcode 8 destination stride counts **records**, not costs. Matrix outer stride
can be zero only for `m=1`. Source views may overlap each other, and matrix
rows may overlap: neither is a reason to reject an otherwise valid command.
No constraint such as `outer_stride >= n` may be invented.

Preflight checks for opcodes 6–8 compare bounding half-open input/output spans,
including stride gaps. Opcode 6 exempts only exact equal base/length/stride
views, checking each input separately; projection outputs have no exemption.
The baseline scalar opcodes have no defined input/output-alias guarantee;
this MAS does not impose the vector alias validator on them. Batch descriptor
protection instead checks actual output bytes (section 11).

The port uses at most `MEM_BYTES` data bytes per transaction. The baseline
adapter splits at its beat boundaries and transfers only requested bytes.
For each contiguous source chunk or descriptor segment it issues maximal
consecutive spans up to those boundaries, in increasing address order.
Different source roles are filled separately. Strided elements are gathered
as individual four-byte accesses, split further if unaligned. Padding gaps
and inactive tail lanes are never read or written.
Results are written one four- or eight-byte output element at a time, split
as required. Wider internal transfers do not imply wider atomic writes or
permission to touch neighbouring bytes. Unaligned architectural addresses
must be handled by splitting, not rejected for an invented alignment rule.

The adapter assembles split reads before making their element valid and
decodes little-endian costs into lane values. Writeback performs the inverse
conversion. An argmin record places value at byte offset zero and index at
offset four; a batch record places completed count at zero and failed index
at four. Bus width and the model host's endianness cannot alter these layouts.

Every physical request is checked against the device's own control aperture;
an intersecting access fails without recursively issuing MMIO. Descriptor
fetches and completion writes use the same guard. Other memory-access failures
are reported by the adapter. Physical reads are ordinary memory accesses;
this design does not provide semantics for side-effecting peripheral operands.

### 6.3 Ordering and visibility

For an in-place vector add, both inputs of a chunk must be loaded before any
output from that chunk is stored. Exact aliasing and positive strides ensure
those stores cannot destroy a future chunk's input. If both inputs alias the
destination, both still represent the pre-update values.

A result write must be acknowledged before its pending storage is released.
All writes of child `i` must have committed before even fetching child `i+1`.
No operand bank survives the child boundary as a valid cached value; a later
child rereads guest memory, including a producer's output. `DONE` requires all
result writes, and for batches the completion record, to have committed.
`ERROR` also requires draining accepted requests so that no stale write can
land in a subsequent command.

## 7. Checked cost arithmetic datapath

Each active lane implements `checked_add(a,b)` over signed 32-bit costs:

1. Reject either input greater than `INF = INT32_MAX / 4`, even if the other
   is `INF`.
2. If either valid input equals `INF`, return `INF`.
3. Sign-extend both finite inputs and add in at least **33 signed bits**.
4. If the sum is below `INT32_MIN`, signal underflow. Otherwise, if it is at
   least `INF`, return `INF`; otherwise return its exact signed 32-bit value.

Two signed 32-bit operands fit exactly in a signed 33-bit sum. Software models
may use 64-bit intermediates, as [cost_math.cpp](../accelerator/src/cost_math.cpp)
does. A lane carries an error bit separately from its value: `INF` is never an
error sentinel. Invalid tail lanes neither access memory nor raise arithmetic
errors nor enter the minimum tree.

ADD3 is exactly `checked_add(checked_add(a,b),c)`, with a range/domain check
at each step. It must not use a reassociated or single wide three-input sum.
The baseline reuses the same add lanes in two passes and stores the first
result in an operand bank. A first-add underflow fails even if `c` is `INF`.
After a successful first add yielding `INF`, `c` still must be read and checked:
an invalid `c > INF` must fail. For example, `(INF-1)+1` saturates, so adding
`-1` afterwards still gives `INF`; algebraically cancelling `+1` and `-1`
would be incorrect.

No minimum value, including `INT32_MIN`, permits skipping remaining inputs:
those inputs may contain an invalid cost, underflow, or memory failure.

## 8. Chunked reduction and argmin

A lane processing column `j` emits a valid `(value,j)` candidate. The reduction
tree orders candidates lexicographically by signed value, then unsigned
**global reduction index**. Inactive leaves have `valid=false`, rather than
an arbitrary index paired with `INF`. The tree combines its winner with the
persistent state for that output using the same comparator.

Persistent state initializes to `{value=INF,index=0}`. Since `n>0`, this gives
`{INF,0}` for an all-`INF` row. A valid finite candidate replaces it; equal
values only replace a larger index. Chunk indices are `chunk_start + local
group_start + lane`, never row numbers, byte offsets, or tile-local indices.
The reduction is independent of tree shape and lane count. Value-only opcodes
may discard indices at writeback; the baseline uses the same pair datapath.

One scalar reduction needs one persistent state. A projection needs one per
live output row, at most `T_m`. The baseline issues one lane group, waits for
its arithmetic/tree/state-update completion, then issues the next group. This
avoids read-after-write hazards even if a pipeline has multiple stages. A
more overlapped implementation must provide forwarding or scoreboard stalls
for a pending update to the same state; it cannot silently use stale minima.

## 9. Primitive execution schedules

`L=LANES`, `T_n` is the input-buffer capacity in cost elements, and `T_m` is
the output-state capacity. All are positive implementation parameters. A chunk
has `k=min(T_n, remaining_n)` valid elements, consumed in `ceil(k/L)` lane
groups with explicit tail masks. Phases advance on completion handshakes, not
on an assumed memory latency. Pipeline latency parameters determine the wait
between issue and retirement; these schedules assert no measured throughput.

| Opcode | Selected schedule | Reused resources and writeback |
| --- | --- | --- |
| 1 `MAP_ADD_REDUCE_MIN` | Initialize one state; for each chunk load two vectors, ADD2 and reduce each lane group; retain state across chunks | Two operand banks, cost lanes, minimum tree; one 4-byte value after the complete reduction |
| 2 `MAP_ADD3_REDUCE_MIN` | Load three vectors per chunk; first checked add, then checked add of third, then reduce; repeat chunks | Three banks, same lanes in two passes, one state; 4-byte value |
| 3 `MAP_ADD_REDUCE_MIN_ARGMIN` | Opcode-1 schedule with global indices | Same tree/state; one 8-byte `{value,index}` |
| 4 `MAP_ADD3_REDUCE_MIN_ARGMIN` | Opcode-2 schedule with global indices | Same tree/state; one 8-byte `{value,index}` |
| 6 `COST_ADD_VECTOR` | Load both input chunks; compute all active ADD2 groups into the first bank; after the chunk succeeds, store its elements in increasing index order and await acknowledgements; repeat | Two banks, cost lanes, reduction bypass; 4-byte costs at destination stride |
| 7 `MINPLUS_PROJECT` | Tiled-output loop of section 10, one shared vector and one matrix row chunk | Shared bank, matrix bank, lanes and `T_m` states; one 4-byte cost per row |
| 8 `MINPLUS_MAP3_PROJECT` | Same loop, two shared vectors pre-added with checks, then matrix ADD2 and per-row argmin | Three banks, two uses of cost lanes, `T_m` pair states; one 8-byte record per row |

For scalar ADD3 the first-pass result overwrites the first bank only after its
inputs have been consumed. All required sources for the chunk are fetched
before arithmetic. A read or arithmetic failure suppresses that scalar's
write. Opcode 6 stages the current chunk before writing; an error may therefore
leave earlier chunks visible while the current chunk remains unwritten.
Write faults may leave part of that chunk visible. This is within the ISA's
unspecified output-on-error contract, not an atomicity promise.

## 10. Projection traversal, tiling, and reuse

### 10.1 Choice of traversal

Let `q` be the number of shared input vectors: one for opcode 7 and two for
opcode 8. The following counts are successful-execution logical cost-element
reads without cross-command caching or deduplication of overlapping sources:

| Strategy | Live reduction states | Shared-vector reads | Principal tradeoff |
| --- | --- | --- | --- |
| One complete output row at a time | 1 | `q*m*n` with only a chunk buffer | Minimal state, repeated shared-vector traffic; avoiding rereads requires a full length-`n` buffer |
| All outputs concurrently, input chunks outermost | `m` | `q*n` | Full reuse, but state capacity proportional to architectural `m` and more state addressing |
| Tiles of `T_m` outputs, input chunks inside each tile | `min(m,T_m)` | `q*n*ceil(m/T_m)` | Bounded state and shared reuse within a tile, explicit rereads between tiles |

MAS 1.0.0 selects **tiled outputs**, with lanes parallelizing columns of one
row at a time. It does not require `m` reductions simultaneously in hardware,
nor one lane per output. Input chunks are buffered; matrix chunks are consumed
one row at a time. No full matrix tile is stored.

**Rationale:** the [primitive-shape study](reports/primitive-shape-study.md)
supports vector-shaped commands but does not choose hardware storage. The
[historical cycle projection](reports/vector-primitive-cycle-projection.md)
examines synthetic domains 2–32 and a corpus largely around 7/16; these suggest
testing small tiles, not limiting the ISA to these dimensions. Its perfect
reuse assumption does not establish a feasible universal buffer. The
[access-pattern study](reports/matrix-access-pattern-study.md) records strided
views in 19.3% of the LLVM sample and 24.2–66.2% of the synthetic families.
Those are global view frequencies, not per-opcode bandwidth measurements.
They justify explicit gather behavior; they do not justify a mandatory
transpose engine or an optimal `T_m`.

### 10.2 Selected loop nest

All loops below run in increasing logical-coordinate order. Loads, computation,
state updates, and writes wait for their handshakes as described above.

```text
for row_base = 0; row_base < m; row_base += T_m:
    rows = min(T_m, m - row_base)
    state[0..rows) = {INF, 0}
    for col_base = 0; col_base < n; col_base += T_n:
        cols = min(T_n, n - col_base)
        if opcode == 7:
            A[0..cols) = load shared src1 vector chunk
        else:  // opcode 8
            A[0..cols) = load shared src0 vector chunk
            B[0..cols) = load shared src1 vector chunk
            A[0..cols) = checked_add(A, B) in lane groups
        for row = 0; row < rows; ++row:
            C[0..cols) = load matrix row (row_base + row), columns of chunk
            for each lane group in cols:
                if opcode == 7:
                    candidates = checked_add(C, A)
                else:
                    candidates = checked_add(A, C)
                attach global column indices
                reduce candidates and merge into state[row]
                wait for state update before next dependent group
    for row = 0; row < rows; ++row:
        write state[row] value or pair at destination(row_base + row)
        wait for write completion
```

Any failure transfers to stop/drain; it does not continue this loop. For
opcode 7, bank `A` remains unchanged until every row in the tile has consumed
the current input chunk. For opcode 8, the checked first-add results remain
in `A` for that same lifetime. This is exact common-subexpression reuse of
`cost_add(src0[j],src1[j])`, not reassociation. It reduces the number of first
adds to `n*ceil(m/T_m)`; the matrix-dependent second add still runs `m*n`
times. Matrix costs must be read and validated even for a pre-added `INF`.
Pre-add failure stops before matrix processing of that chunk.

Each row state persists through every input chunk. Matrix bank `C` may be
overwritten after the current row chunk has retired; shared bank `A` may be
overwritten after the last row chunk retires. The final tile may contain fewer
than `T_m` rows; the final chunk or lane group may be short. No padded row or
column participates in memory accesses or arithmetic.

For arbitrary strides, the same loop uses the descriptor's affine addresses.
Unit inner stride permits contiguous row-chunk transfers; a transposed or
padded view uses gather loads. The design neither silently packs an entire
matrix nor changes orientation to improve locality. Stride gaps are absent
from data traffic even though they participate in the alias span checks.

### 10.3 Traffic, early errors, and partial outputs

For a successful command with `K=ceil(m/T_m)`:

```text
opcode 7 operand bytes = 4 * (m*n + K*n)
opcode 8 operand bytes = 4 * (m*n + 2*K*n)
opcode 7 output bytes  = 4*m
opcode 8 output bytes  = 8*m
```

These are MAS logical transfer payloads, excluding descriptor traffic, bus
framing, and beat underutilization; they are not cycle formulas. No capacity
for the full shared vector is assumed. Even when `n <= T_n`, the baseline
invalidates and reloads shared inputs for each output tile, making the same
control and accounting apply to all lengths. With `m <= T_m`, shared vectors
are read once per command. With `m > T_m`, their rereads are explicit.

For example, `m=17,n=31,T_m=8,T_n=16` uses three output tiles and two input
chunks per tile. Opcode 7 reads 527 matrix costs plus 93 shared-vector costs;
opcode 8 reads those 527 plus 186 shared costs. At most eight reduction states
and three 16-cost banks are live. This differs from L1's respective shared
counts of 31 and 62.

A tile produces no output until all its rows have completed all input chunks.
An input/arithmetic failure in a tile leaves earlier tiles committed; none of
the current tile's outputs has yet been issued. A failure during tile writeback
may leave a prefix and part of a failing split record. Later tiles are not
started. Different tile/lane sizes or memory failures can change error-output
contents; software must not rely on a particular prefix. Successful output
values and indices must be identical across configurations.

**Reference behavior:** the current executor walks complete output rows and
can write a row before encountering a fault in the next. Tile execution need
not reproduce its exact read order or partial-output pattern. The ISA allows
unspecified outputs on error and requires the same final results on success.

## 11. Ordered batch sequencer

The parent context retains the latched top-level descriptor address, child
stream base/end, cursor, `child_count`, `child_bytes`, completed count/current
index, and batch-result address. It is constant-sized, regardless of batch
length. No list of decoded children is stored.

Before fetching child zero, validate the parent descriptor, nonzero count and
byte length, 16-byte-multiple stream length, checked stream/result/parent spans,
and non-overlap of the eight-byte batch result with either descriptor region.
Invalid parent metadata is rejected without child execution and without
assuming that a safe completion-record destination exists. Its record is
unspecified, consistent with the functional model.

For current child index `i`:

1. Fetch/decode exactly one child using the bytes remaining to the stream end.
   Reject a nested batch at this child without following its stream pointer.
2. Preflight every addressed output element against the entire child stream
   and the 32-byte parent descriptor. A small scan uses the output AGU and two
   interval comparators, with no memory accesses and no graph-sized storage.
   Complete the scan before executing any part of this child. A scalar output
   takes one check. Vector padding gaps are **not** protected output bytes.
3. Execute the child with the same primitive engine. On success, drain all its
   writes and invalidate operand banks. Only then increment completed/index,
   advance cursor by the decoded 32/48/64-byte size, and begin the next child.
4. After exactly `child_count` successes, require `cursor == stream_end`.
   Do not fetch an additional descriptor to inspect trailing bytes.

This follows [pcaa_output_overlaps](../pcaalib/src/pcaa.c). Replacing its
element scan with a bounding-span overlap test would reject legal outputs
whose padding contains descriptors. Checking only just before a write would
also permit execution before a known invalid output; the selected design uses
the complete preflight scan.

Child inputs may read descriptor bytes. The result record may overlap operand
or child-output storage if it avoids both descriptor regions: do not add an
extra prohibition. The record is written after all child activity, so its
architectural final write can overwrite such storage. Source/destination
restrictions within each primitive remain separately applicable.

| Termination | Batch record `{completed, failed_index}` | Top-level status |
| --- | --- | --- |
| All children succeed and consume exactly the stream | `{child_count, UINT32_MAX}` | `DONE` after record commit |
| Child `i` fetch, decode, protection check, memory, or arithmetic fails | `{i, i}` | `ERROR` after drain and record attempt |
| All children succeed but bytes remain | `{child_count, child_count}` | `ERROR` after record attempt |
| Record write fails | Contents unavailable/unspecified | `ERROR` after draining that write |

Truncation or exhaustion before an expected child is a failure at that child.
The sequencer must not pre-reject a count/length mismatch that is only exposed
by walking later children: their earlier successful outputs and completed
count must be retained. There is no rollback, dependency discovery, fusion,
or command overlap. A projection/addition pair communicates through committed
guest memory, not a hidden retained intermediate across child boundaries.

## 12. Errors and partial completion

The internal error latch retains failure class and context (phase, child index,
and address or element coordinate where relevant) for model diagnostics and
verification. These are not new architectural registers. Software still sees
the existing coarse `ERROR` and, where writable, the batch completion record.
An error bit is never encoded as a valid cost.

| Error class | Detection point | Required action |
| --- | --- | --- |
| Malformed descriptor | Header/body decoder and validator | Suppress dispatch; fail current top-level command or child |
| Address/span error or forbidden alias | Checked AGU/preflight validation | Suppress primitive execution; no wrapped physical request |
| Access to own MMIO aperture | Memory request guard | Fail request without issuing recursive control traffic |
| Memory read failure | Descriptor/operand response | Invalidate returned data; stop issuing primitive work |
| Input cost greater than `INF` | Checked-add lane validation | Latch arithmetic failure even beside `INF`; suppress affected retirement |
| Negative finite underflow | Either checked-add pass | Latch failure; never clamp to `INT32_MIN` |
| Memory write failure | Writeback response | Stop subsequent outputs, drain accepted requests; written bytes may remain |
| Malformed batch parent | Parent validation | No children; completion record unspecified |
| Malformed child, nested batch, or stream overrun | Child fetch/validation | Fail current index; do not execute later children |
| Trailing stream bytes | Final cursor check | Record `{count,count}` and fail after completed children |
| Child execution failure | Primitive completion path | Drain child, attempt `{i,i}`, then top-level `ERROR` |
| Batch-record failure | Final write response | Retain failure, drain, publish `ERROR` without retry |

On a failure, cancel only work not yet accepted by memory. Accepted reads must
retire even if their data will be discarded; accepted writes must retire and
cannot be assumed cancelled. Kill unretired arithmetic tokens and unsent
output writes. Wait until pipelines, responses, and pending writeback no
longer can modify state, then attempt the batch failure record if applicable.
Reporting `ERROR` must leave the engine quiescent and ready for an independent
valid submission. Do not recursively attempt a failure record after its own
write fails.

The single-outstanding baseline makes this drain small. A configuration with
more outstanding requests must retain their ownership until retirement and
must not issue a later child while a failed child's requests remain. No
transactional output guarantee, whole-record atomicity on write failure, or
architectural precedence between simultaneous error causes is added.

## 13. Internal state and storage

All entries below are implementation-only. Contents are not directly readable
by software; only committed guest results and the existing status/record are
architectural. Logical byte counts exclude valid bits, tags, addresses,
pipeline control, and SRAM implementation overhead.

| State/buffer | Purpose and lifetime | Capacity/scaling | Parameterized? | Architectural visibility |
| --- | --- | --- | --- | --- |
| Submission snapshot and status | Active descriptor address through top-level completion | One context | No context-count tuning in MAS 1 | Only existing address registers/status |
| Descriptor bytes and fill mask | Header/body assembly until canonical decode | 64 bytes | Maximum fixed by current encoding | None |
| Canonical primitive latch | Validated fields through child/primitive drain | One maximum command | Fixed bounded fields | None |
| Batch context | Parent addresses, cursor/end, counts, result pointer through record retirement | Constant; no per-child array | No | Only final batch record |
| Operand banks `A`, `B`, `C` | Up to three sources; shared/first-add chunk through all tile rows; matrix chunk through current row | `3 * 4 * T_n` bytes | `T_n` | None |
| Reduction-state bank | Persistent `{value,index}` per active row across all input chunks | `8 * T_m` bytes; one entry for scalar ops | `T_m` | Only serialized final results |
| Lane operands/results and valid/error/index tokens | One issued group through arithmetic/tree retirement | `O(L)` plus configured pipeline registers | `L`, stage depths | None |
| Pending writeback | One output element or batch record plus address until acknowledgement | 8 data bytes baseline | Queue depth | Only completed guest writes |
| Memory request/response staging | Retain request data and route returned bytes | `O(MEM_BYTES * queue depth)` | Width/depth/credits | None |
| AGU, loop/protection-scan counters | Affine generation, tile/chunk traversal, preflight scan | Constant number of counters | Counter widths sufficient for bounds | None |
| Error latch and drain state | Cause/context and outstanding obligations until terminal publication | Constant plus outstanding tags | Credits | Existing `ERROR`/batch record only |

The reduction-state bank itself holds a finished projection tile during
writeback; no separate `T_m`-sized result buffer is required. Opcode 6 reuses
its first operand bank for the finished chunk. Banks may be registers or
SRAM with explicit access cycles. The baseline must provide `L` cost reads
per operand bank for a lane group (banking or a wide word), first-add/result
writes to bank `A`, and one state read/modify/write at a time. Memory fill and
lane consumption are separate phases, so dual simultaneous memory/compute
ports are unnecessary.

Storage scales as `O(T_n + T_m + L + queues)`, never as `m*n`, `n` at its
architectural maximum, or `child_count`. Host-owned descriptor/input/output
regions stay stable until top-level completion, even after internal copies
are fetched. Internal validity is cleared between primitives; a physical
buffer's old bits do not extend their semantic lifetime.

## 14. Parameters and initial bring-up profile

| Parameter | Initial value | Meaning/constraint |
| --- | ---: | --- |
| Architectural contexts / primitive engines | 1 / 1 | Fixed design structure, not a tunable command scheduler |
| `LANES` | 4 | Parallel columns per issued arithmetic group; positive |
| `MEM_BYTES` | 16 | Maximum internal transfer payload; baseline adapter splits at 16-byte boundaries |
| `T_m` | 8 | Live output states; positive, independent of `LANES` |
| `T_n` | 16 | Costs per operand bank; positive; baseline groups four costs per bank word |
| Read request depth | 1 | At most one pending read request |
| Write request depth | 1 | At most one pending write request |
| Total accepted outstanding requests | 1 | Shared credit; read/write maxima cannot be used simultaneously |
| Pending writeback entries | 1 | One output element or completion record |
| Checked-add pipeline latency | 1 cycle/pass | Initial L2 abstraction, to be changed after implementation timing work |
| Lane-reduction latency | 2 cycles at four lanes | Initial two registered binary compare levels |
| Persistent-state merge latency | 1 cycle | Read/compare/update completion abstraction |
| Phase overlap | Disabled | Controller waits for each fill/compute/write phase |

Latencies are model configuration, not frequency or synthesis claims. A model
must expose or record actual controller/validation stalls, adapter response
latency, and pipeline settings; zero-cost control must not be silently inferred
from these datapath values. Reduction bypass on opcode 6 incurs no minimum
tree latency. ADD3 uses two checked-add passes. Alternative lane counts need
an explicitly configured tree depth/latency and tail handling, including
non-power-of-two counts.

At these values, the operand banks hold 192 bytes, pair states 64 bytes, the
descriptor buffer 64 bytes, and pending write data 8 bytes: **328 bytes of
listed payload storage**, plus all control, canonical fields, lane registers,
and memory staging in section 13. This is not an area estimate.

**Rationale:** four lanes provide a small tree and a useful baseline for the
observed short reductions; 16-byte transport can fill a four-cost lane group
from a contiguous source. `T_m=8` tests useful shared reuse without allocating
state for every architectural output; `T_n=16` holds several groups while
keeping operand storage small. The same four-lane/16-byte numbers used by the
timed runner are an engineering starting point, not an optimum established by
that model. Serial fills also mean four lanes do not imply four completed
candidates per elapsed cycle.

**Future experiment:** sweep `T_m`, `T_n`, lanes, memory latency, and strided
transfer efficiency on current compact command traces. Measure shared rereads,
state-update stalls, lane occupancy, descriptor/protection-scan cost, and
writeback stalls before enabling intra-primitive prefetch or additional credits.
Numerical tuning must not require rejecting a valid architectural dimension.

## 15. Relationship to L1 and existing evidence

[The L1 model](l1-performance-model.md) and
[timing_model.h](../accelerator/include/timing_model.h) define an analytical
service estimate. They are neither the scheduling implementation nor a latency
oracle for this MAS. The [refinement route](design.md#model-refinement-route)
preserves semantics while adding implementation detail.

| L1 assumption or accounting | MAS mechanism / consequence for L2 |
| --- | --- |
| Descriptor service `ceil(B / bandwidth)` | Header response precedes body fetch, decode and validation; splitting, response latency and child output-protection scans have real events |
| Shared projection inputs read once per command | Chunk banks reuse within one output tile; rereads across `ceil(m/T_m)` tiles must be counted |
| `m*ceil(n/L)` projection compute chunks | Explicit row/chunk/groups; `T_n` boundaries and pipeline/state waits can add bubbles; MAP3 shares its checked first add within a tile |
| Streaming `max(read,compute)` | Bring-up schedule serializes phases; any later overlap must arise from actual buffers, credits and dependencies |
| Uniform reduction/setup terms | Vector add bypasses reduction; ADD3 has two passes; tile initialization and state updates consume modeled events |
| Byte-rate operand cost without stride penalty | Gather requests, tails, unaligned splits and response stalls determine effective bandwidth |
| Summed result bytes | Per-element writeback acknowledgements and child barriers govern completion |

L2 should report descriptor, operand-read, compute, result-write activity and
total elapsed device cycles, with explicit wait/stall accounting and stated
overlap. Logical payload bytes, bus transfers, primitive descriptors, and
top-level submissions must stay distinguishable. Software packing/search time
is external to device service. No numerical equality with the current L1
28-cycle triangle calibration is required.

The [research synthesis](research-summary-2026-10-01.md) motivates projection
efficiency and software-owned control. Historical vector ratios used 56-byte
scalar descriptors, perfect shared reuse, and an earlier command stream;
they are not hardware results or predicted MAS speedups. The
[compact-encoding measurements](reports/compact-encoding-measurements.md)
establish byte savings and frequent exact aliasing on four examples, not
frontend latency or a corpus-wide optimal buffer size. Historical reports and
the existing L1 formulas remain unchanged; new L2 measurements need their own
implementation revision, configuration, inputs, and verification scope.

## 16. Verification invariants and acceptance cases

These are obligations for subsequent L2 unit tests, assertions, and RTL/formal
properties. They are not a claim of L2 verification in this documentation change.
Host ownership/stability and eventual memory responses are environment
assumptions; internal ordering, arithmetic, and request accounting are device
assertions. A liveness proof must state those response-progress assumptions.

1. At most one top-level context and one executing primitive exist. A rejected
   busy doorbell cannot alter their address, state, or eventual completion.
2. Descriptor body reads occur only after a valid header-derived size; no read
   crosses the current descriptor or child-stream limit. Dispatch requires all
   reserved-byte, span, dimension, stride, and format checks to have passed.
3. Accepted requests retain stable ownership until exactly one response;
   outstanding credits are never exceeded, including under backpressure.
4. No arithmetic token consumes an unfilled bank element; no shared chunk or
   pending write data is overwritten while a consumer/request still owns it.
5. Every required successful-path source element is validated, including a
   third addend after `INF`. Masked tail lanes generate no accesses or errors.
6. Finite underflow never saturates silently. ADD3 grouping is preserved;
   invalid costs cannot be hidden by absorption, minima, or early termination.
7. The reduction winner has the least global column index among equal minima.
   An all-`INF` reduction yields `INF` and index zero where an index is returned.
8. Lane/chunk/tile partitioning preserves every successful result and output
   byte layout, including rectangular, strided and single-row matrices.
9. No address generation or exclusive-end calculation wraps. No issued access
   touches tail padding, stride gaps, or the PCAA control aperture.
10. Exact vector aliases read original chunk inputs before stores. Illegal
    vector span overlap is rejected; batch descriptor protection examines
    actual output bytes and completes before executing the offending child.
11. Child `i+1` cannot fetch or execute before all child `i` writes have committed.
    All bank validity is cleared across the boundary; producer results are
    observable through the next child's guest-memory reads.
12. A failed child cannot execute any later child. Completed count includes
    only fully successful children; trailing bytes report `{count,count}`.
13. `DONE` implies no pending requests, no unretired compute, every required
    output committed, and a committed successful batch record where applicable.
14. `ERROR` implies all accepted requests drained and no future stale writes;
    a subsequent valid submission is independent. No rollback is assumed.
15. Descriptor ownership/stability holds for the full required lifetime;
    the reusable child fetch buffer cannot destroy retained parent metadata.

Acceptance tests should compare successful memory results and terminal status
against the functional model through the real MMIO path. Error tests compare
status, batch counts, protected regions, and absence of later-child effects;
they must not require identical failing-command output bytes or read order.
Inject failure and backpressure at descriptor header/body, each operand role,
split output writes, and completion-record writes. Check recovery after each.

Include all formats; mixed 32/48/64-byte streams; nested, truncated, trailing,
and count-mismatched batches; descriptors in output padding; producer/consumer
children; exact aliases with either/both inputs; invalid overlap; maximum u16
strides/dimensions and near-u64 address limits. Arithmetic cases include
negative values, positive saturation, both ADD3 underflow stages, invalid input
beside `INF`, all-`INF`, and ties across lane/chunk/tile boundaries. Lengths
include 1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 63, 64 and architectural-boundary
cases; sweep output counts around `T_m` and input lengths around `T_n` and `L`.
Maximum-shape verification may use generated memory responses and bounded
state assertions rather than allocating a maximum-sized host matrix.

## 17. Deferred features

MAS 1.0.0 excludes architectural local vector registers, software-addressable
scratchpads, multiple command contexts, out-of-order commands, autonomous
graph processing, workers/frontiers, nested or dependency-scheduled batches,
full-matrix MAP3, and fusion that requires an ISA extension. It adds no
interrupt, virtual address translation, coherent-memory protocol, CPU
instruction, or CSR. A cache with architecturally visible behavior is outside
this contract; even transparent operand caching is absent from the baseline.

Potential implementation refinements such as extra memory credits, bank
overlap, or a larger tile must preserve the existing ISA and the documented
microarchitectural obligations or be versioned as MAS changes. Architectural
local-RF proposals remain a separate ISA exploration. This document supplies
the control, traversal, storage and ordering decisions for the first L2 model;
it does not implement that model or advance the ISA version.
