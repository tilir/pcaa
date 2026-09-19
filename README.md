# Programmable Cost Algebra Accelerator

PCAA is a small programmable accelerator for recurring operations on cost vectors and tables. It is intended for workloads such as PBQP and cost-function networks, where software manages the graph or problem structure while the accelerator performs regular arithmetic over contiguous data.

The project currently runs bare-metal RISC-V programs under Spike. Programs submit work through a small driver library; inputs and results live in ordinary guest memory.

The complete block contract is in the [architecture specification](doc/arch.md).

## Supported commands

All input elements are signed 32-bit costs. The command descriptor supplies the vector length at run time, so callers are not limited to a fixed vector width.

### `MAP_ADD_REDUCE_MIN`

Adds two vectors element by element and writes their smallest sum:

```text
dst[0] = min_i (src0[i] + src1[i])
```

For example, for `src0 = {4, -2, 8}` and `src1 = {1, 5, -10}`, the result is `-2`.

### `MAP_ADD3_REDUCE_MIN`

The three-input counterpart of the preceding command:

```text
dst[0] = min_i (src0[i] + src1[i] + src2[i])
```

### `MAP_ADD_REDUCE_MIN_ARGMIN`

Computes the same two-input minimum and additionally returns where it occurred:

```text
dst.value = min_i (src0[i] + src1[i])
dst.index = first i whose sum equals dst.value
```

If several elements have the same minimum, the first is selected. `ACCEL_INF` (`INT32_MAX / 4`) represents an unreachable cost: adding it to any value remains `ACCEL_INF`. Positive values that would exceed it also saturate to `ACCEL_INF`.

### `MAP_ADD3_REDUCE_MIN_ARGMIN`

Computes a three-input minimum and the first index at which it occurs:

```text
dst.value = min_i (src0[i] + src1[i] + src2[i])
dst.index = first i whose sum equals dst.value
```

This operation is used by the bundled PBQP workload when eliminating a node with two neighbours.

### `EXECUTE_BATCH`

Submits an ordered array of the four primitive commands above with one doorbell.
Each child runs in array order. On success, the batch result reports the number
completed; if a child fails, earlier results remain, later children do not run,
and the result identifies the failed child. Batches cannot contain batches.

## Build and test

SystemC 3.x, GoogleTest, and a RISC-V bare-metal compiler must be installed. Give CMake the source tree of the exact Spike build used to run the plugin:

```sh
cmake -S . -B build -DSPIKE_SOURCE_DIR=../riscv-isa-sim
cmake --build build
ctest --test-dir build --output-on-failure
cmake --build build --target basic_elf batch_elf randomized_elf pbqp_basic_elf pbqp_randomized_elf
```

Run the deterministic and randomized bare-metal tests:

```sh
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/basic.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/batch.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/randomized.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/pbqp_basic.elf
spike --extlib=build/libpcaa_spike_device.so --device=pcaa,0x10002000,0x1000 build/pbqp_randomized.elf
```

`basic.elf` checks known examples of every supported command. `randomized.elf` compares accelerator results with an independent software implementation for 100 rounds and lengths 1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 63, and 64. Its fixed seed is `0x51a7c0de`.

The PBQP programs solve small cost graphs in both software and accelerator modes, then compare both reconstructed solutions with exhaustive enumeration. `pbqp_basic.elf` is deterministic; `pbqp_randomized.elf` uses a fixed seed.

## Workload characterization

`pbqp_workload` is a host tool for generating reproducible PBQP cost-kernel traces. It writes CSV
without host addresses and reports logical operation and traffic distributions; it is not a timing
benchmark. See the [characterization report](doc/workload-characterization.md) and run:

```sh
cmake --build build --target pbqp_workload
build/pbqp_workload --trace build/pbqp-workload.csv
```
