# 0010 — Nearest neighbour: the sampler beside the morpher

**Status:** accepted, 2026-08-21. Governs `iris.h` 0.2.0 PART 10.
**Affects:** `iris.h` PART 10 (`iris_knn_predict`, `iris_classify_1nn`,
`IRIS_KNN_MAXK`), `tests/audit.c` ("k-NN: exact recall on every demo",
"k-NN: convex, finite, does not reroll", "1-NN: agrees with the Weka-IBk
reference", "samplers survive hostile queries and poisoned outputs").

## Context

Desktop Wekinator's default for discrete (classifier) outputs is k-nearest
neighbours (k-NN). Without it iris had no answer for "this gesture selects that
preset". k-NN regression, weighted by inverse squared distance, and 1-nearest
neighbour (1-NN) classification can be built as a weighted read of the
demonstrations the instrument already stores: no training, no seed, no extra
arena bytes. The multilayer perceptron (MLP), iris's network, was measured
against it with the comparison fixed in advance.

What nearest neighbour buys:

- **Exact recall.** Standing on a demonstration returns it: to the bit from
  `iris_classify_1nn`, and to within rounding from `iris_knn_predict`
  (`tests/audit.c` measures a worst error of 6e-8 at 5, 20, 50 and 200
  demonstrations). The network misses its own demonstrations slightly.
- **The discrete-output answer.** `iris_classify_1nn` agrees at 441 of 441
  probes with an independent double-precision reference written to the rules
  of Weka's IBk classifier and LinearNNSearch, including the earliest-recorded
  demonstration winning a tie (`tests/audit.c`).
- **Structural safety.** The output is a weighted average of demonstrated
  outputs: it cannot be a not-a-number and cannot leave the demonstrated
  range.
- Every saved instrument can play this way with nothing added to its file.

What it costs, stated as plainly (the first two from a study whose program is
not yet published):

- **It generalises worst of everything tested.** Held-out grid error 0.039 at
  20 demonstrations against the network's 0.014; the network beats k = 3 at
  *every* count, 2.4 times at 5 demonstrations and 2.1 times at 200. The
  tiny-data regime where nearest neighbour is supposed to win is exactly where
  it loses.
- **Seams are real.** Between two demonstrations the output steps up to 31
  times more sharply than its mean step, where the network steps 1.9 times.
  That is the sampler character, by design.
- **Playing costs grow with the demonstrations.** On the development laptop
  (Apple M4 Max, Apple clang 17, `-O2`) a prediction takes 0.30 µs at 64
  demonstrations and 1.2 µs at 256, against 0.036 µs for the network
  (`sh build.sh audit`, training-cost table). The algorithm with no training
  is the expensive one at play time. It has not been timed on the board.

## Decision

- A self-contained PART 10 in `iris.h`: k chosen by the caller (3 is the
  suggested default), clamped to `IRIS_KNN_MAXK` = 8; inverse squared distance
  with a 1e-9 guard at zero distance; distances counted in fractions of each
  input's demonstrated range, so an input that never moved counts not at all;
  a scan with no division; the earliest-recorded demonstration wins a tie. No
  new fields in the instrument and nothing added to the file: which algorithm
  plays an instrument is a choice made at run time.
- **Nearest neighbour does not reroll**: nothing in it is random. It is
  presented as a different *character*, a sampler beside the network's morph,
  never as a quality tier; the accuracy figures above are kept so nobody
  mistakes it for one.
- The label is **"Wekinator-compatible semantics"**, not "Wekinator-verified":
  agreement is with a double-precision reference written to Weka's rules; the
  Java program itself has not been run on the same data.

## Rejected alternatives

**Radial basis function (RBF) interpolation instead** (the best gap fit in the
comparison, and exact recall). Not taken: its two musical properties are
already delivered by nearest neighbour (exact recall) and the closed-form
trainer (a near-exact fit), its N × N kernel buffer is up to 256 KB at 256
demonstrations, it has no natural reroll, and its kernel width is an audible
parameter with no default derived from the data. Leaving out the
best-fitting model is a deliberate, stated loss.

**Gaussian-process regression as the mapper.** Rejected: worse than RBF at
equal cost (grid error 0.0239 against 0.0220 at 20 demonstrations) and it
misses the demonstrations by about 0.3% (the same study).

**Hide the seams by smoothing the weights.** Rejected: the seams are the
sampler character; smoothing them produces a worse network. Anyone who wants
the morph uses the network, one call away on the same demonstrations.

**A separate header file.** Rejected: no state and no dependency beyond the
instrument, so a second file to version, ship and explain costs more than one
more PART in the file people already read end to end.

## Consequences

- iris answers Wekinator's classifier use: gesture-to-preset selection needs
  no extra memory and no training.
- The ranges nearest neighbour measures in are the instrument's own: the ones
  it was last fitted to, or, on an instrument never fitted, the current
  demonstrations' ranges. After recording or deleting, train again to measure
  in the new ranges.
- Re-derive: `sh build.sh audit`.

Measurements: every figure that no check or command prints is from a study
whose program is not yet published, which iris-studies lists as S14 and points
back to this record.
