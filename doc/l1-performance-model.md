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
bytes per cycle, and the default setup latencies. The reproducible RN corpus
(`cmake --build build --target rn_characterization`) measures ten local,
synthetic 20-node binary graphs. For minimum-degree RN it reports:

| Work | Cycles | Descriptor | Operand | Compute | Result | Primitives / batches |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Ten-graph corpus | 7,695 | 1,213 | 4,056 | 860 | 2,426 | 860 / 353 |

Operand service dominates this small-domain corpus. RN scoring contributes 247
generic min-plus projections, expanded into 494 current scalar primitives.
A structural matrix-vector projection would halve that projection-descriptor
count; fused projection-accumulate would also remove intermediate result
traffic. These comparisons are analytical and do not change the ABI.
