# Programmable Cost Algebra Accelerator

## Architecture specification

Revision 0.3

## 1. Scope

The Programmable Cost Algebra Accelerator (PCAA) is a memory-mapped block for
regular arithmetic over runtime-sized vectors of costs. Its execution model is
map followed by reduction:

```text
guest memory → elementwise cost operation → reduction → guest memory
```

PCAA is intended for the regular kernels found in PBQP and cost-function
networks: min-plus reductions, min-plus reductions with three inputs, and
argmin. It is not a PBQP solver. Graph topology, scheduling, memory
allocation, and all irregular control flow remain the responsibility of the
host software.

The architectural boundary is deliberately independent of a particular CPU,
bus protocol, simulator, or implementation technology. The current reference
integration uses RISC-V MMIO and guest physical addresses, but the block ABI is
defined by this document.

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

Input and scalar output costs use signed two's-complement `int32_t`.

```c
#define ACCEL_INF (INT32_MAX / 4)
```

The cost addition used by currently defined commands has the following
semantics:

```text
INF + x   = INF
x + INF   = INF
finite sum >= INF = INF
finite sum < INT32_MIN = INT32_MIN
otherwise = exact signed sum
```

The last two rules make overflow behavior deterministic. `INF` is a value in
the data representation, not an out-of-band validity flag.

### 4.2 Tie breaking

Commands that return an index select the smallest index whose value equals the
minimum. This is part of the observable result.

## 5. Command descriptor

The host writes one `accel_command_t` descriptor into guest memory for each
submission.

```c
struct accel_command {
    uint32_t opcode;
    uint32_t flags;

    uint32_t n;
    uint32_t m;
    uint32_t k;
    uint32_t reserved;

    uint64_t src0;
    uint64_t src1;
    uint64_t src2;
    uint64_t dst;
};
```

The baseline layout is 56 bytes and naturally aligned to 8 bytes. The field
offsets are fixed:

| Offset | Field | Meaning |
| ---: | --- | --- |
| `0x00` | `opcode` | operation selector |
| `0x04` | `flags` | operation-specific flags; ignored by current commands |
| `0x08` | `n` | vector length for current commands |
| `0x0c` | `m` | reserved operation dimension; ignored by current commands |
| `0x10` | `k` | reserved operation dimension; ignored by current commands |
| `0x14` | `reserved` | reserved; ignored by current commands |
| `0x18` | `src0` | first source vector physical address |
| `0x20` | `src1` | second source vector physical address |
| `0x28` | `src2` | third source vector physical address when required |
| `0x30` | `dst` | result physical address |

For all baseline commands, `n` must be non-zero, and `src0`, `src1`, and
`dst` must be non-zero. Commands requiring `src2` additionally require a
non-zero `src2`.

Reserved fields preserve descriptor compatibility as the operation set grows.
Their current value has no effect.

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

The baseline command queue has one outstanding command. A doorbell submission
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

## 9. Error behavior

PCAA reports `ERROR` for a submission when any required descriptor, input, or
output memory access fails, or when the descriptor is malformed. Baseline
malformed cases are:

* zero descriptor address;
* unsupported opcode;
* `n == 0`;
* zero required source or destination address;
* zero `src2` for opcodes `2` and `4`.

An `ERROR` completion does not specify a result at `dst`. A subsequent valid
submission is permitted and is independent of the preceding error.

## 10. Internal datapath model

The architectural computation is a runtime-length stream reduction. The
logical vector length is unrelated to a future physical lane count.

```text
src0 ──┐
src1 ──┼──► cost-add lanes ──► min reduction ──► result
src2 ──┘
```

An implementation may process an input vector in strips:

```text
logical length: 10000
physical lanes: 8
strips: [0..7], [8..15], …
```

Strip size, lane count, reduction-tree shape, buffering, pipelining, and
memory-level parallelism are microarchitectural choices. They must not change
the results defined in sections 4 and 8.

## 11. Timing and refinement

The initial model is a functional, untimed realization of this block. It
preserves the same MMIO and descriptor contract as later implementations.

| Profile | Permitted refinement |
| --- | --- |
| functional | immediate command execution; no meaningful simulated time |
| loosely timed | annotated transaction delay and explicit command latency |
| approximately timed | pipelined map/reduce and modeled memory concurrency |
| mixed TLM/RTL | replacement of selected datapath blocks with RTL models |

No timing refinement may alter command encoding, result values, tie breaking,
status semantics, or guest-memory addressing.

## 12. Extension space

Future opcodes may use `flags`, `m`, `k`, and additional descriptor semantics
to express vector operations, reductions, broadcast operations, matrix/table
projections, and normalization. Extensions retain the following invariants:

* runtime-defined problem dimensions;
* descriptor-based submission;
* guest physical memory operands;
* deterministic cost arithmetic where specified;
* software ownership of graph topology and irregular control flow.

## 13. Exclusions

The baseline block does not define:

* RISC-V instruction encodings or custom CSRs;
* interrupts;
* virtual-memory translation, IOMMU, or cache coherency;
* DMA timing or cache behavior;
* multi-command queues beyond one outstanding command;
* PBQP graph storage, graph reductions, or solver heuristics;
* a general-purpose ALU instruction language;
* a fixed vector width as part of the programming model.
