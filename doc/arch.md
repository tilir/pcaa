# Programmable Cost Algebra Accelerator

## Architecture specification

Revision 1.1 (semantic ISA 1.0.0; compact encoding used by pcaalib 2.0.0)

## 1. Scope

The Programmable Cost Algebra Accelerator (PCAA) is a memory-mapped block for
regular arithmetic over runtime-sized vectors of costs. Its primitives are
elementwise cost addition and min-plus map/reduction, with scalar or vector
outputs:

```text
guest memory → cost operation / reduction → guest memory
```

PCAA is intended for the regular kernels found in PBQP and cost-function
networks: min-plus reductions, min-plus reductions with three inputs, and
argmin. It is not a PBQP solver. Graph topology, scheduling, memory
allocation, and all irregular control flow remain the responsibility of the
host software.

The architectural boundary is deliberately independent of a particular CPU,
bus protocol, simulator, or implementation technology. The current reference
integration uses RISC-V MMIO and guest physical addresses, but the block ABI is
defined by this document. It specifies commands, memory-visible results,
ordering, and errors. Service-cycle estimates are described separately in
[the L1 performance model](l1-performance-model.md); the implementation
refinement route is in [the design note](design.md#model-refinement-route).

For batch-capable operation, responsibility is divided as follows:

```text
algorithm-level scheduling  → software
batch construction          → software/runtime
batch iteration             → accelerator
primitive execution         → accelerator
```

Software decides what work exists and in what order. Hardware may autonomously
drain a finite ordered batch of already-scheduled primitive operations. The
block does not infer dependencies, reorder work, or inspect PBQP topology.
Reduction policy is deliberately software-selectable; it may change the
observable workload but never changes the PCAA descriptor ABI.

## 2. Terminology

| Term | Meaning |
| --- | --- |
| host | CPU software that submits PCAA commands |
| guest memory | the physical address space shared by host and accelerator |
| descriptor | command structure placed in guest memory by the host |
| submission | writing a descriptor address then ringing the doorbell |
| cost | signed 32-bit value used by all currently defined operations |
| `INF` | distinguished unreachable-cost value, `INT32_MAX / 4` |

## 3. Block boundary

PCAA has two architecturally distinct interfaces:

1. A control interface, exposed as four 32-bit MMIO registers.
2. A memory interface, used by the block to read descriptors and operands and
   to write results.

All descriptor and operand addresses are byte addresses in guest physical
memory. They are not virtual addresses and are never host-language pointers.

```text
                  control transactions
host CPU ───────────────────────────────────► PCAA
   │                                            │
   └──────────── guest physical memory ─────────┘
                         memory transactions
```

The control and memory paths are separate by definition. A future
implementation may attach either path to a concrete interconnect without
changing the descriptor ABI.

## 4. Data representation

### 4.1 Cost values

Input and output costs use signed two's-complement `int32_t`.
All multi-byte architectural quantities in descriptors, cost operands, scalar
outputs, `accel_min_argmin_result_t`, and `accel_batch_result_t` are little-endian.

```c
#define ACCEL_INF (INT32_MAX / 4)
```

The cost addition used by currently defined commands has the following
semantics:

```text
input cost > INF           = command ERROR
INF + x                    = INF
x + INF                    = INF
finite sum >= INF          = INF
finite sum < INT32_MIN     = command ERROR
otherwise                  = exact signed sum
```

The valid input domain is `INT32_MIN <= finite_cost < ACCEL_INF`, or exactly
`ACCEL_INF` for infinity. A cost greater than `ACCEL_INF` is invalid and makes
the command fail with `ERROR`, even when another addend is `INF`. This applies
to every source element read by opcodes 1–4 and 6–8, including the third
operand of ADD3 after an intermediate result has reached `INF`.

`INF` is the absorbing element of cost addition for valid operands, so
`INF + x = INF` for every valid cost `x`. This is a
deliberate algebraic rule, not ordinary saturation, and `INF` remains a value
in the data representation rather than an out-of-band validity flag.

Positive and negative range excess are intentionally asymmetric. A finite sum
at or above `INF` becomes `INF`: all such costs are forbidden/unreachable, so
their ordering is immaterial. A finite sum below `INT32_MIN` has no analogous
negative-infinity meaning. Clamping two distinct sums there would create a
false tie and could change first-index argmin, so any command encountering
negative underflow fails with `ERROR`; the output on error is unspecified.

### 4.2 Tie breaking

Commands that return an index select the smallest index whose value equals the
minimum. This is part of the observable result.

## 5. Semantic commands and descriptor encoding

ISA 1.0.0 consists of the operations in section 8 over affine guest
physical-memory views. The current `pcaalib` supports this ISA through the
descriptor encoding specified below; see the [pcaalib API reference](pcaalib.md)
for its API and independent SemVer versioning. The selected compact encoding
uses a stream of 16-byte slots: a command occupies exactly 32, 48, or 64 bytes.
There is no universal C descriptor struct or fixed child-command stride. The
device reads an eight-byte common header, derives the canonical size from its
`opcode`/`format` pair, then reads that many bytes.

All multi-byte fields are little-endian. The common header is `opcode:u8` at
`0x00`, `format:u8` at `0x01`, `flags:u16` at `0x02`, `n:u16` at `0x04`, and
`m:u16` at `0x06`. `flags` and every reserved byte must be zero. `n` is the
reduction/vector length and `m` the projection output count; both are nonzero
where used. Their meanings are opcode-specific:

| Opcode | `n` | `m` | Output shape |
| --- | --- | --- | --- |
| 1–4, scalar map/reduce | Number of input elements reduced | Zero, unused | One cost or one minimum/argmin record |
| 5, batch | Zero, unused | Zero, unused | One batch completion record; count is `child_count` |
| 6, vector add | Number of elements in each input and the output | Zero, unused | `n` costs |
| 7–8, projection | Number of columns reduced per output | Number of output rows | `m` costs (7) or `m` minimum/argmin records (8) |

For primitives, used dimensions range from 1 through 65,535. A nonzero `m`
on opcode 1–6 is malformed; it does not request additional vector-add outputs.
Neither dimension encodes a lane count or a byte length. Address fields are
full 64-bit guest physical byte addresses; required addresses are nonzero.
Dimensions and strides are unsigned 16-bit values. Required strides are
nonzero; a projection's outer stride may be zero only when `m == 1`. Strides
count elements (32-bit costs or 8-byte output records), not bytes. Address
spans must not overflow 64-bit physical addresses.

| Format ID | Opcode | Bytes | Fields after the common header (offset: field) |
| ---: | ---: | ---: | --- |
| 1 `REDUCE2` | 1 | 32 | `08:src0:u64`, `10:src1:u64`, `18:dst:u64`; `m=0`, implicit unit strides |
| 2 `REDUCE3` | 2 | 48 | `08:src0`, `10:src1`, `18:src2`, `20:dst` (all u64); `28..2f` reserved, `m=0` |
| 3 `REDUCE2_ARGMIN` | 3 | 32 | Same offsets as `REDUCE2` |
| 4 `REDUCE3_ARGMIN` | 4 | 48 | Same offsets as `REDUCE3` |
| 5 `EXECUTE_BATCH` | 5 | 32 | `08:child_stream:u64`, `10:result:u64`, `18:child_count:u32`, `1c:child_bytes:u32` |
| 6 `COST_ADD_VECTOR_GENERAL` | 6 | 48 | `08:src0:u64`, `10:src1:u64`, `18:dst:u64`; `20:src0_stride:u16`, `22:src1_stride:u16`, `24:dst_stride:u16`; `26..2f` reserved, `m=0` |
| 7 `COST_ADD_VECTOR_INPLACE` | 6 | 32 | `08:src0_and_dst:u64`, `10:src1:u64`; `18:src0_dst_stride:u16`, `1a:src1_stride:u16`; `1c..1f` reserved, `m=0` |
| 8 `MINPLUS_PROJECT` | 7 | 48 | `08:src0:u64`, `10:src1:u64`, `18:dst:u64`; `20:src0_stride:u16`, `22:src0_outer_stride:u16`, `24:src1_stride:u16`, `26:dst_stride:u16`; `28..2f` reserved |
| 9 `MINPLUS_MAP3_PROJECT` | 8 | 64 | `08:src0:u64`, `10:src1:u64`, `18:src2:u64`, `20:dst:u64`; `28:src0_stride:u16`, `2a:src1_stride:u16`, `2c:src2_stride:u16`, `2e:src2_outer_stride:u16`, `30:dst_stride:u16`; `32..3f` reserved |

The format ID is distinct from the semantic opcode. Every opcode/format pair
has one exact size; incompatible pairs and nonzero reserved fields are malformed.
The in-place format reconstructs ordinary semantic `COST_ADD_VECTOR` with
`dst == src0` and equal destination/source stride. Since `cost_add` is
commutative and has symmetric error checks, an exact `dst == src1` alias is
encoded by swapping the two source references. This does not mutate the
semantic command. A separate destination uses the general format; partial or
other output/input overlap remains illegal. No other opcode has an in-place
variant. Exchanging a matrix's inner and outer strides still represents a
different affine view, not a transpose or lane bit.

## 6. MMIO control interface

The device occupies a 4 KiB MMIO region. Registers are little-endian,
32-bit-wide accesses.

| Offset | Register | Access | Description |
| ---: | --- | --- | --- |
| `0x00` | `DESC_ADDR_LO` | R/W | low 32 bits of descriptor physical address |
| `0x04` | `DESC_ADDR_HI` | R/W | high 32 bits of descriptor physical address |
| `0x08` | `DOORBELL` | W | submits the descriptor at `DESC_ADDR` |
| `0x0c` | `STATUS` | R | completion state |

The descriptor address is the concatenation of `DESC_ADDR_HI` and
`DESC_ADDR_LO`. `DOORBELL` write data is currently ignored. Reads from
`DOORBELL`, writes to `STATUS`, accesses to undefined offsets, non-32-bit
accesses, byte enables, and burst accesses are invalid transactions.

### 6.1 Status values

| Value | Name | Meaning |
| ---: | --- | --- |
| `0` | `IDLE` | no completion is available |
| `1` | `BUSY` | the submitted command is executing |
| `2` | `DONE` | the command completed and its result was written |
| `3` | `ERROR` | the command could not complete |

The block accepts one outstanding command. A doorbell submission
sets `BUSY`, then eventually sets either `DONE` or `ERROR`. The functional
reference model may complete before the doorbell transaction returns; software
must nevertheless treat completion as asynchronous and poll `STATUS`.

## 7. Submission and ordering

A host submits a command in this order:

1. Populate the descriptor and all input data in guest memory.
2. Ensure those writes are visible to the device according to the host memory
   ordering rules.
3. Write the descriptor address to `DESC_ADDR_LO` and `DESC_ADDR_HI`.
4. Write `DOORBELL`.
5. Poll `STATUS` until it is `DONE` or `ERROR`.
6. After `DONE`, make the result read visible according to host memory ordering
   rules and read `dst`.

The driver uses I/O-inclusive fences around submission, status observation, and
result consumption. A command whose guest-memory operand overlaps the block's
own MMIO control region fails with `ERROR`; operands must refer to ordinary
guest physical memory, not the PCAA control registers.

The descriptor is owned by the host until doorbell submission and by PCAA
until completion. The host must not modify the descriptor or referenced input
and output regions while the command is outstanding. Overlap between source
and destination regions is not defined for the baseline commands.
Opcode 6 permits exact full-view `dst` aliasing with either input (same base,
length, and element stride), but otherwise rejects overlapping output and input
address spans. Opcode 7 and 8 outputs must not overlap their input address
spans. These span checks include gaps between strided elements. On `ERROR`,
including arithmetic underflow, output contents are unspecified: earlier
elements may have been written. A successful command writes every output.

## 8. Baseline operations

### 8.1 `MAP_ADD_REDUCE_MIN`

Opcode: `1`

For `0 <= i < n`:

```text
value[i] = cost_add(src0[i], src1[i])
dst[0]   = min(value[i])
```

`src0` and `src1` point to arrays of `n` signed 32-bit costs. `dst` points to
one signed 32-bit cost.

### 8.2 `MAP_ADD3_REDUCE_MIN`

Opcode: `2`

For `0 <= i < n`:

```text
value[i] = cost_add(cost_add(src0[i], src1[i]), src2[i])
dst[0]   = min(value[i])
```

`src0`, `src1`, and `src2` point to arrays of `n` signed 32-bit costs. `dst`
points to one signed 32-bit cost.

### 8.3 `MAP_ADD_REDUCE_MIN_ARGMIN`

Opcode: `3`

For `0 <= i < n`:

```text
value[i] = cost_add(src0[i], src1[i])
```

The result stored at `dst` is:

```c
struct accel_min_argmin_result {
    int32_t value;
    uint32_t index;
};
```

`value` is the minimum of `value[i]`; `index` is its first occurrence.

### 8.4 `MAP_ADD3_REDUCE_MIN_ARGMIN`

Opcode: `4`

For `0 <= i < n`:

```text
value[i] = cost_add(cost_add(src0[i], src1[i]), src2[i])
```

The result stored at `dst` has the same `accel_min_argmin_result` representation
as section 8.3. `value` is the minimum of `value[i]`; `index` is its first
occurrence. All three source addresses are required.

### 8.5 `EXECUTE_BATCH`

Opcode: `5`. Both common-header dimensions `n` and `m` are zero.

`child_count` is a non-zero 32-bit count of commands in the encoded stream at
guest physical address `child_stream`; `child_bytes` is its exact non-zero
byte length, a multiple of 16 and representable in 32 bits. `result` points
to `accel_batch_result_t`. The block walks children by each decoded format's
32/48/64-byte size and executes them in order. Exactly `child_count` primitive
commands must consume exactly `child_bytes`: truncation, overrun, trailing
bytes, malformed lengths, and nested batches cause `ERROR`.

All writes by a successful child are visible to the next child through guest
memory before that next child begins. Thus a batch may produce a temporary
projection and consume it with a vector add.

On success, `{ completed = child_count, failed_index = UINT32_MAX }` is stored and status
is `DONE`. If child `i` cannot be read, is invalid, or fails, descriptors before
it remain completed, descriptors after it are not executed, and
`{ completed = i, failed_index = i }` is stored before status becomes `ERROR`.
Batch execution is fail-stop and non-transactional. If all `child_count`
children complete but trailing stream bytes remain, status is `ERROR` and the
result is `{ completed = child_count, failed_index = child_count }`; that index
identifies the invalid stream end, not an executed child. If the completion
record itself cannot be written, status is `ERROR` and its contents are
unavailable or unspecified.
The batch result and every child output must avoid all bytes of
`[child_stream, child_stream + child_bytes)` and the 32-byte top-level batch
descriptor. A batch with an overlapping
result is rejected before child execution; a child whose output overlaps either
descriptor region fails at that child. Child input reads may overlap descriptor
storage, but software must still keep descriptors and inputs stable until
completion. Output overlap is checked at addressed elements, not padding gaps.

### 8.6 `COST_ADD_VECTOR`

Opcode: `6`

This is elementwise addition of two length-`n` cost vectors, producing a
length-`n` cost vector. `n` must be nonzero; `m` is unused and must be zero.
There is no reduction and no argmin result.

| Field | Meaning |
| --- | --- |
| `src0`, `src1` | Guest physical base addresses of the two input cost vectors |
| `dst` | Guest physical base address of the output cost vector |
| `src0_stride`, `src1_stride` | Distance between consecutive input elements, in 4-byte costs |
| `dst_stride` | Distance between consecutive output elements, in 4-byte costs |

For every `0 <= i < n`:

```text
a = read_cost(src0 + 4 * i * src0_stride)
b = read_cost(src1 + 4 * i * src1_stride)
write_cost(dst + 4 * i * dst_stride, cost_add(a, b))
```

All three strides must be nonzero. Exact full-view destination aliasing with
either input is allowed: the base address, length, and stride must match.
Other output/input span overlap is invalid, including overlap across stride
gaps (section 7). A separate destination uses the 48-byte general format;
an exact alias uses the 32-byte in-place format described in section 5.
These formats implement the same operation.

For example, `n = 3`, unit strides, `src0 = [2, INF, -4]`, and
`src1 = [3, 7, 1]` produce `[5, INF, -3]`. Here the arrays describe contents
at the supplied base addresses, not values embedded in a descriptor.

### 8.7 `MINPLUS_PROJECT`

Opcode: `7`

This is an affine matrix/vector min-plus projection. `src0` supplies a logical
`m × n` matrix; `src1` supplies one length-`n` vector reused for every row.
The output is `m` costs. Both `n` (columns per reduction) and `m` (independent
output rows) must be nonzero. The command returns minimum values only.

| Field | Meaning |
| --- | --- |
| `src0` | Guest physical base address of the matrix |
| `src0_stride` | Matrix column/inner stride, in 4-byte costs |
| `src0_outer_stride` | Matrix row/outer stride, in 4-byte costs |
| `src1`, `src1_stride` | Base address and element stride of the shared input vector |
| `dst`, `dst_stride` | Base address and element stride of the `m` output costs |

For every `0 <= i < m`:

```text
for 0 <= j < n:
  a = read_cost(src0 + 4 * (i * src0_outer_stride + j * src0_stride))
  b = read_cost(src1 + 4 * j * src1_stride)
  candidate[j] = cost_add(a, b)
write_cost(dst + 4 * i * dst_stride, min(candidate[0..n-1]))
```

Column, vector, and output strides must be nonzero. The matrix row stride may
be zero only when `m = 1`, since there is then no next row. A contiguous
row-major matrix uses `src0_stride = 1` and `src0_outer_stride = n`; padding
or exchanging the row/column strides expresses other affine views. No layout
mode or implicit transpose is involved. Output/input span overlap is invalid.

For example, `m = 2`, `n = 3`, matrix rows `[1, 4, 0]`, `[5, -2, 3]`, and
vector `[2, 1, 3]` produce `[3, -1]`. Each row is reduced separately. If every
candidate in a row is `INF`, that row's output is `INF`.

### 8.8 `MINPLUS_MAP3_PROJECT`

Opcode: `8`

This computes `m` independent three-input min-plus reductions and returns a
minimum/argmin record for each. `src0` and `src1` are length-`n` vectors shared
by all outputs; `src2` is a logical `m × n` matrix. Both `n` and `m` must be
nonzero. Each output's index refers to its own reduced column `j`, not to a
matrix byte offset or the output row `i`.

| Field | Meaning |
| --- | --- |
| `src0`, `src1` | Guest physical base addresses of the shared cost vectors |
| `src0_stride`, `src1_stride` | Input-vector element strides, in 4-byte costs |
| `src2` | Guest physical base address of the matrix |
| `src2_stride`, `src2_outer_stride` | Matrix column and row strides, in 4-byte costs |
| `dst` | Guest physical base address of the `m` output records |
| `dst_stride` | Distance between outputs, in **8-byte records** |

For every `0 <= i < m`:

```text
for 0 <= j < n:
  a = read_cost(src0 + 4 * j * src0_stride)
  b = read_cost(src1 + 4 * j * src1_stride)
  c = read_cost(src2 + 4 * (i * src2_outer_stride + j * src2_stride))
  candidate[j] = cost_add(cost_add(a, b), c)
value = min(candidate[0..n-1])
index = smallest j with candidate[j] == value
write_min_argmin(dst + 8 * i * dst_stride, {value, index})
```

The two additions use exactly the grouping shown; saturation and underflow
are checked at each addition as specified in section 4.1. Every output has
an `int32_t value` at record offset 0 and a `uint32_t index` at offset 4, both
little-endian. All vector, column, and output strides must be nonzero; the
matrix row stride may be zero only for `m = 1`. Output/input span overlap is
invalid. A contiguous output vector uses `dst_stride = 1`, so records begin
8 bytes apart, unlike the 4-byte outputs of opcode 7.

Using the matrix from section 8.7, `src0 = [2, 1, 3]`, and
`src1 = [0, 0, 0]` gives `[{3, 0}, {-1, 1}]`: row 0 has two candidates equal
to 3, and index 0 wins. If all candidates in a row are `INF`, its result is
`{INF, 0}`. Tie breaking is local to each row; outputs have no cross-row
ordering rule. Software may use this operation for any affine three-input
reduction; it has no graph-coordinate fields.

## 9. Error behavior

PCAA reports `ERROR` for a submission when any required descriptor, input, or
output memory access fails, or when the descriptor is malformed. Baseline
malformed cases are:

* zero descriptor address;
* unsupported opcode;
* an incompatible format, truncated command, or nonzero reserved field;
* `n == 0` for a primitive, or `child_count == 0` for a batch;
* zero required source or destination address;
* zero `src2` for opcodes `2` and `4`;
* `EXECUTE_BATCH` with a nested batch child.

A well-formed command also completes with `ERROR` if it reads a cost above
`ACCEL_INF` or if a finite addition underflows below `INT32_MIN`, including
either addition of an ADD3 primitive.

An `ERROR` completion does not specify a result at `dst`. A subsequent valid
submission is permitted and is independent of the preceding error.
For opcodes 6–8, invalid dimensions/strides, arithmetic address-span
overflow, or forbidden output/input overlap also cause `ERROR`.

## 10. Compatibility boundary

Timing and implementation parallelism do not change command encoding,
cost arithmetic and its grouping, result layouts, first-index tie breaking,
submission ordering, error semantics, or guest physical addressing. The ISA
does not guarantee a clock frequency, a service latency, or a lane count.
Software must observe completion through `STATUS` rather than assuming a
fixed duration.

Future encoding or semantic changes require an explicit versioned contract;
reserved bytes in the current encoding remain zero. Deferred interface ideas
are described in [the ISA exploration](isa-2x-local-vector-rf.md), and the
implementation refinement route is documented in [design.md](design.md#model-refinement-route).

## 11. Exclusions

The baseline block does not define:

* RISC-V instruction encodings or custom CSRs;
* interrupts;
* virtual-memory translation, IOMMU, or cache coherency;
* DMA timing or cache behavior;
* multi-command queues beyond one outstanding command;
* PBQP graph storage, graph reductions, or solver heuristics;
* a general-purpose ALU instruction language;
* a fixed vector width as part of the programming model.
