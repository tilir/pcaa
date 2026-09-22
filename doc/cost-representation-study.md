# Cost representation study

This study revisits only dynamic range, precision, and rough arithmetic cost.
`ACCEL_INF` handling is already an inexpensive integer absorbing-element rule
and is not treated as an FP32 advantage.

## D.1 Road distances

The source is the public [9th DIMACS Shortest Paths Challenge road
corpus](https://www.diag.uniroma1.it/challenge9/download.shtml), specifically
the 264,346-node/733,846-arc New York graph and its published coordinates.  The
[DIMACS format](https://www.diag.uniroma1.it/challenge9/format.shtml) publishes
integer graph weights; to test real-valued precision rather than merely
re-encode those integers, `scripts/cost_representation_analyze.rb` derives a
great-circle distance in metres from each arc's endpoint coordinates.  This is
a representation experiment, not a claim that straight-line distance equals
the corpus's road length.

Across 733,846 arcs, derived lengths range from 0.083560 m to 3,696.969005 m;
p1/median/p99 are 13.930056/95.182226/546.511387 m.  One global scale of 1000
(millimetres) has maximum rounding error 0.000500 m and mean 0.000250 m; the
largest encoded edge is 3,696,969, far below `ACCEL_INF`.  Thus it represents
every individual edge with sub-millimetre absolute quantization and no clamp.
The eight every-100,000th-arc samples are exercised by `probes_unit`, rather
than using hand-picked small weights.

Accumulated paths are the limitation: at scale 1000 only 536,870 metres of
positive cost fit below `ACCEL_INF`; scale 100 retains centimetre resolution
and extends that to 5,368 km.  The best global scale therefore depends on the
required path-length range, even though the individual published NY edges pose
no problem.

## D.1 HMM/Viterbi log weights

Rabiner's standard HMM tutorial describes scaling because products of many
probabilities underflow ([DOI 10.1109/5.18626](https://doi.org/10.1109/5.18626)).
Contemporary speech decoding commonly consumes log likelihoods: Kaldi's
[`DecodableInterface`](https://www.kaldi-asr.org/doc/classkaldi_1_1DecodableInterface.html)
returns a floating `LogLikelihood`, and its lattice decoder stores per-frame
cost offsets specifically to keep totals near a numerically useful range
([source documentation](https://www.kaldi-asr.org/doc/lattice-faster-decoder_8h_source.html)).

Negative logs for probabilities from 1e-2 through 1e-30 span 4.605 to 69.078.
Scale 1000 resolves them to 0.001 with at most 0.0005 quantization error, but a
positive accumulated-cost budget of 536,870 supports only about 67,000 frames
at an average cost of 8 (about 11 minutes at 100 frames/s).  Scale 100 extends
that tenfold at 0.01 resolution.  Zero-probability arcs map naturally to the
existing `INF` sentinel.

Therefore common Viterbi edge weights do not by themselves require FP32:
fixed-point is adequate with a chosen precision and the same periodic
offset/rebasing discipline used by floating decoders.  Without rebasing, no
single practical fixed scale provides both millilog precision and unbounded
sequence length; FP32 offers far more exponent range and convenient direct
interchange with acoustic-model outputs, but practical FP decoders still
rebase because relative precision degrades as totals grow.  This class wants
float-like range operationally, not a proof that fixed-point decoding is
structurally impossible.

## D.2 Accumulation-order audit

The narrow device claim holds.  `accelerator/src/accelerator.cpp` forms one
2-term or 3-term sum per element and then performs MIN; lane count cannot alter
the addition grouping.  `probes/src/bellman_ford.cpp` has the same two-term
edge relaxation followed by MIN.

The repository-wide claim does **not** hold.  `software/pbqp/pbqp.cpp` has
variable-length host accumulations: RN scores fold one projected term per
incident edge, local-search scores fold incident matrix slices, objective
evaluation folds every node/edge contribution, and the exact-search lower
bound folds all active node/edge minima.  Their order is deterministic software
iteration today, so current hardware lane count still cannot affect them.  But
they are counterexamples to “every accumulation is only two or three terms,”
and a future parallel implementation could not assume FP reassociation is
semantics-free.

## D.3 Rough arithmetic-cost delta

The widely reproduced 45 nm synthesis/energy table in Hennessy and Patterson,
attributed to Horowitz's ISSCC 2014 keynote
([DOI 10.1109/ISSCC.2014.6757323](https://doi.org/10.1109/ISSCC.2014.6757323)),
places a 32-bit integer add at about 137 µm² and 0.1 pJ and an FP32 add at about
4,184 µm² and 0.9 pJ: roughly 30x area and 9x energy for that historical
library point.  These are ballparks, not estimates for PCAA or a current node.
A published fully pipelined FPGA FP adder reports six cycles and roughly
1.6k LUTs/ALUTs ([Zhang et al.](https://doi.org/10.1049/IET-CDT.2016.0200));
an integer add/comparator normally fits a carry-chain-scale stage.

FP comparison is cheaper than FP addition but still needs sign/exponent/
significand and special-value handling.  Berkeley HardFloat's
[`compareRecFN`](https://www.jhauser.us/arithmetic/HardFloat-1/doc/HardFloat-Verilog.html)
exposes ordered/unordered results and exception flags, whereas signed-int MIN
uses an ordinary comparator and the existing first-index control.  Exact ratios
are implementation- and target-dependent; only synthesis in the eventual
technology can replace these order-of-magnitude anchors.

## Tradeoff summary

Int32 fixed-point handles the sampled road edges exactly enough and practical
HMM log weights when the software chooses a scale and rebases long sequences.
It gives up FP32's enormous exponent range, relative precision, and direct
compatibility with floating likelihood producers; in return it keeps addition
and comparison dramatically simpler and bit-reproducible.  The finding that
most changes the current picture is not INF—integer INF was already free—but
the host accumulation audit: current device reductions are order-independent,
yet the full PBQP software is not composed solely of fixed-arity sums, so any
future move of those folds into parallel hardware needs an explicit numerical
order contract.
