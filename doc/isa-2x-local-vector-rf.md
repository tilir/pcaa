# Exploratory ISA 2.x: batch-local vector register file

This is a deferred design record, not part of semantic ISA 1.0.0, its compact
encoding, or the current SystemC execution model. The first hardware baseline
remains memory-to-memory commands over guest physical addresses.

A future design may let commands consume guest-memory operands or batch-local
vector values and produce either kind of result. The local state would be a
small vector register file, not general scratch RAM; matrices would initially
remain guest-memory-only. A candidate starting point is four vector registers
of 128 signed 32-bit cost elements each (2 KiB total). Register count and
capacity would be hardware capabilities, not PBQP-specific ISA constants.

At the library boundary, local values could remain virtual. `pcaalib` could
assign them to physical registers within one batch using inexpensive linear
or liveness-based allocation; known batch shapes could use cached assignments.
This is not a case for graph-coloring-style compilation. Guest/local operand
references might keep RN projection temporaries and running scores on device.
Explicit LOAD/STORE operations might be unnecessary if arithmetic commands
can read or write either guest or local operands.

The design needs measurement against the simple memory-to-memory baseline:
saved guest traffic and descriptor bytes versus register-file area, control
complexity, and software allocation overhead. No part of it is adopted yet.
