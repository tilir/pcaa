# pcaalib API reference

`pcaalib` is the C-compatible, platform-neutral command-construction and
descriptor and submission library for PCAA. It represents operations
independently of the guest-memory descriptor, validates commands, and
translates between the two at submission and device ingress. Its common core
performs no MMIO, guest-memory access, cost arithmetic, or scheduling;
platform backends handle transport. The current library and semantic ISA
versions are both `1.0.0`, but they are independent SemVer versions.

The public headers are [`pcaa.h`](../pcaalib/include/pcaa.h) for semantic
commands, [`pcaa_submission.h`](../pcaalib/include/pcaa_submission.h) for
encoding-neutral submission storage, and
[`pcaa_device.h`](../pcaalib/include/pcaa_device.h) for device submission, and
[`pcaa_host_error.h`](../pcaalib/include/pcaa_host_error.h) for hosted diagnostics.
[`pcaa_codec.h`](../pcaalib/include/pcaa_codec.h) is for the descriptor boundary.
They are usable from C and C++ and have no SystemC dependency. The library
needs no heap, exceptions, RTTI, or C++ runtime; the CMake target is
`pcaalib`. The RV64 driver is a separate layer in
[`software/accel_driver.h`](../software/accel_driver.h).

## Versions and compatibility

| Function | Return value | Meaning |
| --- | --- | --- |
| `pcaa_version()` | `const char *`, currently `"1.0.0"` | SemVer version of the pcaalib API and its selected codec. |
| `pcaa_isa_version()` | `const char *`, currently `"1.0.0"` | SemVer version of the semantic ISA implemented by this library. |

Both strings have static lifetime; callers neither allocate nor free them.
The versions can change independently. Only one descriptor encoding is
supported at a time: a future compact encoding can replace the current one
without retaining it or changing the semantic ISA. Version numbers are not
embedded in function or header names.

## Address and view model

All `base`, `result`, and `child_descriptors` addresses use
`pcaa_guest_address_t`: **guest physical byte addresses**, not host pointers.
pcaalib does not dereference them.
The caller must arrange accessible guest-memory storage, preserve it through
execution, and use the platform's submission mechanism. On RV64, the driver
can convert its bare-metal addresses to guest physical addresses; a hosted
caller must instead stage data in guest memory. The exact descriptor and
result layouts, cost rules, and device errors are specified in
[the architecture](arch.md), not by the in-memory `pcaa_command_t` layout.

| Type | Address formula and purpose |
| --- | --- |
| `pcaa_cost_vector_view_t` | `base + i * stride * 4`; `length` signed 32-bit costs. |
| `pcaa_cost_matrix_view_t` | `base + (row * row_stride + column * column_stride) * 4`; `rows × columns` signed 32-bit costs. |
| `pcaa_output_view_t` | `base + i * stride * element_size`; `length` outputs. `PCAA_OUTPUT_COST` selects 4-byte costs; `PCAA_OUTPUT_MIN_ARGMIN` selects 8-byte `{value, index}` records. |

Strides count elements, not bytes. Vector and output strides must be nonzero
for commands that use them. A matrix's column stride must be nonzero; its row
stride may be zero only when it has one row. Required dimensions and addresses
must be nonzero, and address spans must not overflow `uint64_t`. All output
views are caller-owned guest-memory destinations. A command's semantic kind
is not a serialized opcode, and `pcaa_command_t` is not a wire ABI. It is a
caller-owned value with no allocation, destructor, or ownership transfer.
Dimensions, counts, and strides in semantic views use `size_t`. The current
descriptor stores them in 32-bit fields; encoding rejects values that do not
fit instead of truncating them. Its `kind` selects one union member of
`operation`:

| Kinds | Active member |
| --- | --- |
| `PCAA_MAP_ADD_REDUCE_MIN`, `PCAA_MAP_ADD_REDUCE_MIN_ARGMIN` | `reduce2` |
| `PCAA_MAP_ADD3_REDUCE_MIN`, `PCAA_MAP_ADD3_REDUCE_MIN_ARGMIN` | `reduce3` |
| `PCAA_COST_ADD_VECTOR` | `vector_add` |
| `PCAA_MINPLUS_PROJECT` | `project` |
| `PCAA_MINPLUS_MAP3_PROJECT` | `map3_project` |
| `PCAA_ORDERED_BATCH` | `batch` |

The view constructors fill fields; they do **not** validate a view in
isolation:

| Function | Result |
| --- | --- |
| `pcaa_cost_vector(base, length, stride)` | Cost-vector view. |
| `pcaa_cost_matrix(base, rows, columns, row_stride, column_stride)` | Cost-matrix view. |
| `pcaa_cost_output(base, length, stride)` | Output view for 32-bit costs. |
| `pcaa_argmin_output(base, length, stride)` | Output view for 8-byte minimum/argmin records. |

## Command builders

Each builder writes a `pcaa_command_t` through its final, non-null pointer.
It returns `PCAA_STATUS_OK` on success, `PCAA_STATUS_INVALID_ARGUMENT` for a
null destination, `PCAA_STATUS_RANGE` when a dimension or stride cannot fit
the current descriptor, or `PCAA_STATUS_INVALID_COMMAND` for an invalid
command shape. On failure it leaves the destination command unchanged. Builders
validate descriptor representability, dimensions, required addresses and
strides, and output kind. Vector primitives also validate address spans and
prohibited operand/output overlap. They do not access guest memory, inspect
cost values, or check overlap against descriptor storage; the device checks
the latter at execution.

| Function | Semantic operation and requirements |
| --- | --- |
| `pcaa_make_reduce2(first, second, result, with_argmin, command)` | Reduce `first[i] + second[i]` to one minimum; nonzero `with_argmin` also returns the first minimizing index. Both vectors have equal, nonzero length and unit stride. `result` is a guest physical destination for one 4-byte cost or 8-byte record. |
| `pcaa_make_reduce3(first, second, third, result, with_argmin, command)` | As above, adding `third[i]`; all three vectors have equal, nonzero length and unit stride. |
| `pcaa_make_cost_add_vector(first, second, result, command)` | Write `first[i] + second[i]` elementwise. All lengths agree and `result.kind` is `PCAA_OUTPUT_COST`. Exact full-view destination aliasing with either input is allowed; other output/input overlap is rejected. |
| `pcaa_make_minplus_project(matrix, vector, result, command)` | For each row `r`, write `min_i(matrix[r,i] + vector[i])`. Matrix columns equal vector length; matrix rows equal output length; output kind is cost. Output must not overlap an input. |
| `pcaa_make_minplus_map3_project(first, second, third, result, command)` | For each row `r` of `third`, write the minimum and first minimizing `i` of `first[i] + second[i] + third[r,i]`. Vector lengths equal matrix columns; matrix rows equal output length; output kind is `PCAA_OUTPUT_MIN_ARGMIN`. Output must not overlap an input. |
| `pcaa_make_ordered_batch(child_descriptors, count, result, command)` | Reference a nonempty, already encoded child-descriptor array and an 8-byte batch-result destination in guest memory. This does not choose, encode, or execute the children. |

`with_argmin` is a boolean: zero selects a cost-only result, any nonzero value
selects a minimum/argmin record. `PCAA_ORDERED_BATCH` is a transport command,
not a graph or dependency scheduler. A batch runs its primitive children in
caller-specified order; nested batches are not supported. Device-side
cost-domain failures are reported when the command executes, not by a builder.

## Device submission API

Ordinary clients build semantic commands and use a `pcaa_device_t *`. They do
not encode descriptors, write MMIO registers, or call a SystemC socket.
The platform setup creates the device and keeps it alive while commands are
outstanding. The common calls are:

| Function | Contract |
| --- | --- |
| `pcaa_device_submit(device, command)` | Submit one semantic command; returns a `pcaa_status_t`. It does not wait for completion. |
| `pcaa_device_submit_batch(device, commands, count)` | Submit a nonempty, ordered primitive-command array. Child descriptors and batch result are managed by the backend. Returns a `pcaa_status_t`. |
| `pcaa_device_wait(device, completion)` | Check or wait for the outstanding submission; returns a `pcaa_status_t`. `completion` may be null, or points to optional batch progress details. Keep operands and result storage valid until completion. |
| `pcaa_status_string(status)` | Short diagnostic string for any `pcaa_status_t`; available on hosted and bare-metal builds, with static lifetime. |
| `pcaa_perror(prefix, status)` | Hosted-only convenience function from `pcaa_host_error.h`; writes `prefix: status text` and a newline to `stderr`. A null or empty prefix prints only the status text. |

| Status | Meaning |
| --- | --- |
| `PCAA_STATUS_OK` | Submission accepted or execution completed successfully. |
| `PCAA_STATUS_INVALID_ARGUMENT` | Null or otherwise missing API argument. |
| `PCAA_STATUS_INVALID_COMMAND` | Command cannot be encoded for the current descriptor. |
| `PCAA_STATUS_RANGE` | A semantic dimension, stride, or child count does not fit a descriptor field. |
| `PCAA_STATUS_BUSY` | A submission is already outstanding, or hosted completion has not arrived yet. A caller can retry `wait` after progress. |
| `PCAA_STATUS_NO_PENDING` | `wait` called without an outstanding submission. |
| `PCAA_STATUS_NO_SPACE` | Backend staging area or batch capacity is exhausted. |
| `PCAA_STATUS_MEMORY_ERROR` | Hosted guest-memory staging or result access failed. |
| `PCAA_STATUS_TRANSPORT_ERROR` | Transport failed before a device result could be observed. |
| `PCAA_STATUS_DEVICE_ERROR` | The accelerator reported `ERROR`, or a completed batch result was inconsistent. ISA 1.0.0 does not identify the cause of a primitive failure. |

For a batch, `pcaa_completion_t` has `has_batch_result`, `completed`, and
`failed_index`. If the device wrote the batch result, `has_batch_result` is
nonzero even when `wait` returns `PCAA_STATUS_DEVICE_ERROR`; then
`failed_index` identifies the first failing child and `completed` counts
successful children. If no result was written, `has_batch_result` is zero
and the other fields are not diagnostic. The public API cannot distinguish
bad cost data from a failed operand read on ISA 1.0.0: both yield device
`ERROR`. It does not invent a more specific reason.

These functions reject null device/callback pointers; submit also rejects
null commands, and batch submit rejects a zero count. Do not submit again
while a command is outstanding. A successful submit is not a successful
execution: always check `pcaa_device_wait`. The interface keeps submit and
wait separate to allow future asynchronous implementations. The common
`pcaa_device_t` callback fields are backend bindings, not fields that
algorithm code needs to fill.

The SystemC backend is the `pcaalib_systemc` CMake target. Platform setup
constructs a `PcaaSystemCDevice` from guest memory, its allocator, and a
bound TLM forward transport interface; `device()` supplies the common C
handle. It stages descriptors in guest memory and reaches the accelerator
only through the SystemC target socket. The RV64 freestanding backend is
[`pcaa_baremetal_device_init`](../pcaalib/include/pcaa_baremetal_device.h):
the caller provides a persistent `pcaa_baremetal_device_t` and an array of
`pcaa_encoded_slot_t` large enough for its maximum batch. It uses the
bare-metal driver for MMIO, with no heap. The repository's ELF targets select
this backend at build time. Backend setup is platform code; PBQP and other
algorithm code can use the same three submission calls with either backend.
`pcaa_perror` is supplied by the hosted `pcaalib_host` target and is not
linked into freestanding RV64 ELFs; those can pass `pcaa_status_string()` to
their own output routine.

The shared PBQP solver's cost-kernel callbacks retain their generic `int`
status contract. The RV64 adapter returns the numeric `pcaa_status_t` value
without collapsing it to `-1`. If a callback fails, `pbqp_solver_solve`
returns `PBQP_KERNEL_ERROR` and leaves the original callback status in
`pbqp_solver_t.last_kernel_status`; callers can interpret a PCAA-backed
solver's value with `pcaa_status_string`. A new solve clears that field.

For example, after allocating guest-memory operand and result regions:

```c
#include "pcaa_device.h"

pcaa_status_t run_projection(pcaa_device_t *device, pcaa_guest_address_t matrix_pa,
                             pcaa_guest_address_t vector_pa, pcaa_guest_address_t result_pa,
                             size_t rows, size_t columns, size_t row_stride,
                             size_t column_stride) {
  pcaa_command_t command;
  const pcaa_status_t built = pcaa_make_minplus_project(
          pcaa_cost_matrix(matrix_pa, rows, columns, row_stride, column_stride),
          pcaa_cost_vector(vector_pa, columns, 1),
          pcaa_cost_output(result_pa, rows, 1), &command);
  if (built != PCAA_STATUS_OK)
    return built;
  const pcaa_status_t submitted = pcaa_device_submit(device, &command);
  if (submitted != PCAA_STATUS_OK)
    return submitted;
  return pcaa_device_wait(device, NULL);
}
```

The `_pa` values are guest physical addresses. Platform setup maps or stages
those buffers; the algorithm sees neither MMIO nor SystemC.

## Encoding-neutral submission API

`pcaa_encoded_slot_t` is caller-owned aligned storage large enough for one
currently selected descriptor. Treat its bytes as opaque outside the
transport boundary. The following functions encode only; none submits or
waits for a command:

| Function | Contract |
| --- | --- |
| `pcaa_encoded_command_bytes()` | Bytes used by one encoded command; currently 80. |
| `pcaa_encoded_batch_bytes(child_count)` | Bytes for `child_count` encoded children plus one batch descriptor; currently `(child_count + 1) * 80`, or `0` if the size overflows. |
| `pcaa_encode_command(command, output)` | Encode one semantic command into `output`; return a `pcaa_status_t`. A batch command is accepted here. |
| `pcaa_encode_commands(commands, count, output, capacity)` | Encode a nonempty array of primitive commands contiguously into caller storage. `capacity` is in bytes. Rejects batch children, insufficient capacity, invalid commands, and null pointers; returns a `pcaa_status_t`. |
| `pcaa_encode_batch(child_descriptors, count, result, output)` | Build and encode the parent ordered-batch descriptor. `child_descriptors` names an already encoded array in guest memory; this function does not encode it. Returns a `pcaa_status_t`. |

For an ordered batch:

1. Build the primitive `pcaa_command_t` array in execution order.
2. Allocate guest memory for `count * pcaa_encoded_command_bytes()` bytes
   and encode the children there with `pcaa_encode_commands` (or encode into
   caller storage and copy the resulting bytes into guest memory).
3. Allocate an 8-byte batch-result destination. Pass its physical address
   and the child-array physical address to `pcaa_encode_batch`; stage the
   encoded parent descriptor separately.
4. Submit the parent descriptor address and wait before releasing the child
   array or reading the batch result. Completed children remain visible if
   a later child fails; the batch is not transactional.

For `pcaa_encode_commands`, the output buffer must remain available until
the device has consumed it. On an error after earlier elements were encoded,
the buffer may contain a partial prefix; do not submit it. The caller must
place the encoded bytes at the guest physical address used for submission.
The RV64 `accel_submit_command` and `accel_submit_command_batch` helpers do
this transport work; a hosted runner stages the bytes into its guest-memory
model and submits through the SystemC target socket. `accel_submit_command`
and `accel_wait` remain separate, so callers must not assume synchronous
completion.

These helpers are primarily for transport adapters, explicit codec tests,
and encoded-size accounting. Algorithm code normally uses the device API
above.

## Descriptor-boundary API

Include `pcaa_codec.h` only where the current `accel_command_t` representation
is intentionally handled: the codec, device ingress, transport adapters, or
wire-format tests. Production algorithms should use semantic builders and the
submission API instead of assigning descriptor fields.

| Function | Contract |
| --- | --- |
| `pcaa_encode_descriptor(command, wire)` | Validate and encode a semantic command into a caller-owned descriptor. Returns a `pcaa_status_t`; on failure leaves `*wire` unchanged. Unused descriptor fields are emitted in canonical zero form. |
| `pcaa_decode_descriptor(wire, command)` | Validate a descriptor and reconstruct its semantic command. Returns a `pcaa_status_t`; on failure leaves `*command` unchanged. Ignored wire fields need not round-trip byte-for-byte. |
| `pcaa_descriptor_bytes()` | Size of one current descriptor, currently 80 bytes. |
| `pcaa_batch_descriptor_bytes(child_count)` | Bytes for children plus parent, or `0` on size overflow. |
| `pcaa_output_overlaps(command, begin, end)` | Return nonzero if a primitive's actual output elements intersect the half-open guest-address interval `[begin, end)`; conservatively return nonzero for a null, batch, or unrecognized command or invalid output span. Pass a validated command. |

The codec performs structural validation and, for vector primitives,
address-span checks; it performs no guest-memory access or arithmetic. The
device separately rejects inaccessible memory, invalid cost data, and illegal
overlap with the active descriptor storage. The exact descriptor fields,
opcode values, endian rules, and batch
result format are defined by [ISA 1.0.0](arch.md#5-semantic-commands-and-descriptor-encoding).
