# Compact encoding measurements

These are descriptor-byte measurements from the existing PBQP examples, not a
rerun of historical workload characterization. The command was
`build/pcaa_graph_run --solver bare-metal --strategy heuristic-rn --verbose
examples/<name>.pbqp`. All four runs completed with a heuristic solution.
The old comparison assigns 80 bytes to every primitive and batch parent. The
compact figure uses each selected encoded size and includes 32-byte batch parents.

| Graph | Primitive mix (op3 / op6 / op7 / op8) | Batches | Old bytes | Compact bytes | Reduction |
| --- | ---: | ---: | ---: | ---: | ---: |
| triangle | 2 / 0 / 0 / 2 | 2 | 480 | 256 | 46.7% |
| petersen | 2 / 6 / 6 / 12 | 9 | 2,800 | 1,600 | 42.9% |
| chvatal | 2 / 16 / 16 / 10 | 11 | 4,400 | 2,336 | 46.9% |
| random-20 | 2 / 18 / 18 / 24 | 19 | 6,480 | 3,648 | 43.7% |
| Total | 8 / 40 / 40 / 48 | 41 | 14,160 | 7,840 | 44.6% |

For this mix, op3 contributes 8 × 32 = 256 bytes, op6 contributes
40 × 32 = 1,280 bytes, op7 contributes 40 × 48 = 1,920 bytes, op8 contributes
48 × 64 = 3,072 bytes, and the 41 batch parents contribute 41 × 32 = 1,312
bytes. The raw semantic vector-add alias pattern is `dst == src0`: 0,
`dst == src1`: 40. The selected wire-format distribution is INPLACE: 40,
GENERAL: 0, so 100% of these vector adds use the 32-byte form. This describes
the measured RN workloads, not all possible clients of the semantic ISA.

Each measured RN `MINPLUS_PROJECT -> COST_ADD_VECTOR` pair uses 48 + 32 =
80 descriptor bytes, down from 80 + 80 = 160 bytes. The pair's descriptor
footprint is therefore 50% smaller, excluding its batch parent.
