# 0010 — k-NN/1-NN: the sampler beside the morpher

**Status:** accepted, 2026-08-21 (decision made in experiment E7, before integration)
**Affects:** `iris.h` PART 10 (`iris_knn_predict`, `iris_classify_1nn`),
`tests/audit.c` checks 23–25.

## Context

Desktop Wekinator ships k-NN as its default for discrete (classifier)
outputs; iris had no answer for "gesture selects preset". E7 built k-NN/IDW
regression and 1-NN classification as a weighted read of the example store —
zero training, zero seed, zero new arena bytes — and measured it against the
shipping MLP with pre-registered honesty about who wins what.

What k-NN buys, measured:

- **Exact recall**: 0.0 RMSE at every demonstration, every count — the
  property backprop never delivers (the MLP misses its own demos by ~1%).
- **The discrete-output answer**: 441/441 agreement with an independent
  double-precision Weka-IBk-semantics reference, earliest-recorded
  tie-breaking verified.
- **Structural safety**: output is a convex combination of demonstrated
  outputs — cannot NaN, cannot leave the demonstrated range.
- Every v1 file ever saved gains the mode with zero migration.

What it costs, measured and stated with equal volume:

- **Generalisation is the weakest of everything tested.** Grid RMSE 0.039
  at 20 examples vs the MLP's 0.014; the MLP beats the k=3 floor at *every*
  count — 2.4× at 5 examples, 2.1× at 200. The tiny-data regime where k-NN
  folklore says it should win is exactly where it loses.
- **Seams are real**: max step 31× the mean step on a line between demos
  (the MLP morphs at 1.9×). Sampler character, by design, documented.
- Prediction costs O(examples): ~12 µs S3 est. at the 256 cap — 15× the
  trained MLP's cost. The "zero-training" algorithm is the expensive one at
  play time; the roles reverse.

## Decision

- ~90 self-contained lines in `iris.h` (PART 10). k=3 default,
  `IRIS_KNN_MAXK` 8, IDW in min-max normalised space (the same space
  `iris_novelty` uses), 1e-9 zero-distance guard, division-free scan,
  earliest-recorded tie-break. No new struct fields, no format change;
  algorithm choice is a runtime call, and the persisted selector tag waits
  for the next format bump (ADR 0006).
- **This algorithm does not reroll** — nothing is random. Presented as a
  different *character* (sampler beside morpher), never a quality tier; the
  floor table above ships in the docs so nobody mistakes it for one.
- The label is **"Wekinator-compatible semantics"**, not
  "Wekinator-verified": agreement is against a faithful double-precision
  reference implementation of Weka IBk/LinearNNSearch, but the desktop Java
  binary itself has not been run on the same CSV. One run closes it; until
  then the weaker, true sentence is the one we print.

## Rejected alternatives

**RBF exact interpolation instead** (best-in-class gap fit, exact recall).
Deferred to v0.3, not taken now: its two musical properties are already
delivered in v0.2 by k-NN (exactly) and ELM (nearly, at 300×), its N×N
kernel buffer costs up to 256 KB at cap, it has no natural reroll, and its
kernel width is a genuinely audible hyperparameter without a data-driven
default yet. Deferring the best-fitting model in the study is a deliberate,
stated loss.

**GP regression as the mapper.** Rejected: strictly worse than RBF at equal
cost (grid 0.0239 vs 0.0220 at 20 examples) and it misses the demos by
~0.3%.

**Hide the seams (smooth the IDW weights).** Rejected: the seams are the
sampler character; smoothing them buys a worse MLP. Anyone who wants the
morph uses the MLP, which is one call away on the same examples.

**A separate header file.** Rejected: 90 lines, zero state, zero
dependencies beyond the struct — the cost of a second include (a second
thing to version, ship and explain) exceeds the cost of one more PART in
the file undergraduates already read end to end.

## Consequences

- iris now answers Wekinator's classifier use case; gesture→preset
  selection needs no new memory and no retraining, ever.
- `iris_fit_ranges` (or any train) must run after example edits and before
  k-NN predicts — the same contract `iris_novelty` already had.
- If the 256-example scan cost ever matters at high gesture rates, the
  designated fix is the PIE `IRIS_DOT` port hook, not the core.
- Re-derive: `./build.sh audit` (checks 23–25, k-NN latency lines).
  Measured on Apple M4 Max (arm64), Apple clang 17.0.0, `-O2`, 2026-08-21.
