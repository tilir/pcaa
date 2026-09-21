# PBQP workload characterization

The generic graph generator and public runner provide the measurement path for
PBQP workload studies. The current solver corpus is documented in
[solver-characterization.md](solver-characterization.md); its CMake target is
`solver_characterization` and writes both the raw
`build/solver-characterization.csv` and generated
`build/solver-characterization-summary.md`. The corpus records RN cascade
statistics and generic operation mix in addition to solver outcomes.

Keep this document for workload-wide methodology that is not specific to the
PBQP solver comparison. “Register-allocation-like” names a synthetic
domain-size/workload shape, not a trace extracted from LLVM register allocation.
