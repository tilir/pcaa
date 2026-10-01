# PBQP workload characterization

The host generator and public runner measure shared-solver executions. The
methodology and trace/CSV boundaries are in [design.md](design.md); published
solver, scaling, and LLVM corpus snapshots are indexed in [README.md](README.md).
The [solver comparison](reports/solver-characterization.md) retains the generic
operation-mix table as its primary comparison artifact.

Logical operation elements, primitive descriptors, top-level submissions,
software staging, and modeled device cycles answer different questions.
One projection can reduce many elements and write several outputs; one batch
can contain several primitives; a software-only core can perform search while
issuing no descriptors. Logical per-output input traffic may exceed the L1
estimate's ideal shared-vector reads. Report these quantities separately.

Current RN scoring submits ordered projection/vector-add pairs and default R2
uses partial-vector MAP3. Historical scalar descriptor counts and cycle tables
must be interpreted with their report revision; rerun the scripts into a new
CSV when measuring the current implementation. `solver_characterization`
writes `build/solver-characterization.csv` and a generated summary, including
RN cascade statistics and operation mix.

“Register-allocation-like” names a synthetic domain/workload shape, not an LLVM
trace. The real extracted graphs are in `examples/regalloc`; their extraction
and quantization are documented in [llvm-corpus-characterization.md](reports/llvm-corpus-characterization.md).
