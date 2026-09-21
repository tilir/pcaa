# L1 performance model

L1 is a configurable architectural estimate, not RTL timing. It retains the
L0 descriptor ABI. Software builds ordered batches; the device models only
descriptor fetch, operand reads, generic map/reduce computation, and result
writes.

For a primitive of length `N`, `P` operands, and `L` lanes:

```text
chunks       = ceil(N / L)
descriptor   = ceil(56 / descriptor_bytes_per_cycle)
operand read = ceil(P * N * 4 / read_bytes_per_cycle)
compute      = primitive start + map + chunks + optional add3 + reduction + result
result write = ceil(result_bytes / write_bytes_per_cycle)
```

Streaming service is `descriptor + max(operand read, compute) + result write`.
An `EXECUTE_BATCH` additionally pays outer-descriptor, batch-start, and
completion-result costs. No host packing/search time, cache, DMA, FIFO, or
pipeline hazard is modeled.

The timed graph runner uses streaming mode, four lanes, 16 descriptor/read/write
bytes per cycle, and the default setup latencies. It remains intentionally
separate from the functional characterization target: the latter compares
solver algorithms and records logical work, while L1 reports only device
service for the descriptors that a selected algorithm emits.

As a regression point, the untimed triangle's command stream reports 50 L1
service cycles in the timed runner: 32 descriptor, 10 operand-read, 6 compute,
and 8 result-write cycles. The host end-to-end test fixes these values.

RN scoring emits the value-only `MAP_ADD_REDUCE_MIN` primitive. Its result is
four bytes rather than an argmin pair, because RN does not reconstruct a
candidate-state choice from that operation. R1, R2, and coordinate descent use
argmin primitives where their result index is required.

The structural comparison remains useful: a matrix--vector min-plus projection
would collapse RN's repeated scalar descriptors, and a fused
projection-and-accumulate form would avoid materialized projection values.
Those are analytical generic cost-algebra alternatives; they do not change the
descriptor ABI or claim RTL performance.
