# 0019 — The bad-example detector integrates the residual; it does not sample the endpoint

**Status:** accepted, 2026-08-22. Governs `iris.h` 0.2.0 PART 8f.
**Affects:** `iris.h` PART 8f (`iris_example_stress`, `iris_worst_example`,
`iris_worst_example_id`, `IRIS_STRESS_FLAG`, `IRIS_STRESS_MIN_EX`), `IRIS_ARENA`,
`tests/audit.c` ("finds the demonstration that fights, stays quiet
otherwise"), `tests/elm.c measure`.

## Context

The trial-and-error trap in interactive machine learning is that when the
instrument feels wrong the musician has no idea *which* demonstration is wrong,
so they re-record at random. The obvious free detector: after training, rank
the demonstrations by how badly the model still misses them. At 6,000 epochs
it ranks a deliberately corrupted demonstration first in 16 trials out of 20 at
+0.05, where leave-one-out cross-validation, which costs *n* full retrains,
manages 1. Every measurement in this record is on smooth synthetic data. All
but the 0.2.0 figures under "The caveat", which `tests/elm.c measure` prints,
are from a study whose program is not yet published.

## Why the obvious version fails

Training to the plateau ([0017](0017-train-to-the-plateau-not-to-a-constant.md))
invalidates that measurement. Corrupt one demonstration of twenty by +mag on
one output; count how often it ranks first (12 hidden units, 8 outputs, 20
trials):

| corruption | +0.05 | +0.10 | +0.20 | +0.40 |
|---|---|---|---|---|
| final residual @ 6,000 epochs (as proposed) | 16/20 | 10/20 | 13/20 | 8/20 |
| final residual after training to the plateau (`iris_train`) | 8/20 | 5/20 | 2/20 | 2/20 |
| leave-one-out cross-validation @ 6,000 epochs | 1/20 | 1/20 | 10/20 | 11/20 |
| **integrated residual (chosen)** | 6/20 | **18/20** | **17/20** | **17/20** |

Read the second row backwards: **the final residual gets worse as the mistake
gets bigger**, and 2/20 at +0.40 is barely above the 1/20 of guessing. Of
course it does — give the optimiser enough epochs and it bends the surface far
enough to fit the bad point too, after which the bad point looks like every
other point. A detector measured at one budget and shipped at another is a
feature that quietly does nothing.

## Decision

Sum each demonstration's squared error over **every epoch** of the run, in a
ledger of one float per stored demonstration (`cap` floats) carved from the
arena. This measures how long an example
*fought*, not whether it eventually lost. It is stable across budgets — 5/17/17/18
at 6,000 epochs, 9/17/17/16 at 20,000, 11/16/17/15 at 60,000 — and monotone in
corruption size, which is what a person expects a detector to do.

**The flag is the margin, not the level.** Worst-of-n stress rises with n on
clean data with nothing wrong at all: 1.76 at 5 examples, 2.95 at 20, 6.34 at
50, 9.23 at 100. An interface wired to a fixed level would be silent on small rigs and
would cry wolf on large ones. Worst divided by second-worst does not drift:
clean maximum 1.44 / 2.27 / 2.06 at 20 / 50 / 100. `IRIS_STRESS_FLAG` is 2.5.

The full margin table:

| examples | clean margin (median / max) | with a +0.40 demo (median / max) |
|---|---|---|
| 5 | 1.12 / 1.55 | 1.33 / 5.10 |
| 10 | 1.35 / 5.04 | 1.24 / 2.35 |
| 20 | 1.12 / 1.44 | 2.00 / 4.14 |
| 50 | 1.43 / 2.27 | 5.07 / 30.89 |
| 100 | 1.37 / 2.06 | 27.95 / 83.47 |

Read the last two rows against the first two: the signal is enormous at 50 and
100 demonstrations and absent at 5 and 10, which is the whole argument for
`IRIS_STRESS_MIN_EX`.

**Below `IRIS_STRESS_MIN_EX` (12) it returns −1 and says nothing.** At 10
examples the clean margin reaches 5.04 — a false accusation. An example can
only be caught disagreeing with a crowd if there is a crowd. That is the
situation, not a tuning failure.

## What it costs

`sizeof(float) * cap`: **512 B at a capacity of 128, 1 KB at 256**, plus one
float addition per demonstration per epoch, beside the backpropagation already
being done for that demonstration. The endpoint version would cost no bytes,
and it does not work.

## The rejected alternatives

- **Leave-one-out cross-validation.** *n* full retrains for a detector that is 13× worse at
  the smallest corruption and never better than the ledger at any size.
- **A nearest-neighbour leave-one-out residual** (free, independent of
  training, uses the existing sampler). Measured: 0–2/20 at +0.05 and +0.10. With twenty
  demonstrations in a 2-D space, a 5% displacement is inside the natural
  spread between neighbours.
- **Sampling the residual at a fixed early checkpoint.** 6,000 epochs is good
  at +0.05 and bad at +0.40; 600 is the reverse. Tuning a checkpoint to the
  corruption size means knowing the answer first.

## The caveat

The corruption model is a clean offset on one output of a smooth, noiseless
truth. A real inconsistent human demonstration is not that. Nothing here has
been checked against a recorded gesture. On demonstrations that all
contradict one another there is no crowd to disagree with, and the margin
stays low: the detector declines to accuse anybody.

The current figures for 0.2.0 come from `tests/elm.c measure` (12 hidden
units, 8 outputs, one take offset on one output): after `iris_train` the
offset take is ranked first in 18 to 20 of 20 sessions at offsets from 0.05 to
0.40 and 20, 50 or 100 demonstrations, and clean sessions reach the flag in 7,
15 and 6 of 200. `iris.h` PART 8d prints the table, with the closed-form
trainer beside it.

## Relation to prior work

Integrating a per-example quantity along the training trajectory, and using it
to rank examples, is **TracIn**: Pruthi, Liu, Kale & Sundararajan, *Estimating
Training Data Influence by Tracing Gradient Descent*, NeurIPS 2020 (the
Conference on Neural Information Processing Systems),
arXiv:2002.08484.

This is not adjacent work, it is the same idea. TracIn defines *self-influence*
— the influence of a training point on its own loss — and evaluates it by "the
fraction of mislabelled data recovered", which is the metric the table above
uses under a different name. Their §4.1 and §5 do exactly what this record
does: corrupt a known fraction of the training set, rank, and count how many
corrupted points appear at the top.

The related earlier line is Koh & Liang, *Understanding Black-box Predictions
via Influence Functions*, ICML 2017 (the International Conference on Machine
Learning), arXiv:1703.04730, which
estimates the same influence from the converged model rather than the
trajectory — the "sample the endpoint" approach whose failure this record
measures.

**So the claim is not "this is new."** It is narrower:

1. TracIn needs gradients, saved checkpoints, and a separate influence
   computation. The ledger here is one single-precision float per
   demonstration, accumulated inside the loop that was already running
   (`iris.h` PART 8f), costing one addition per demonstration per epoch and
   no extra pass: a scalar approximation of trajectory self-influence that
   fits on a microcontroller.
2. TracIn's setting is offline cleaning of datasets for large models. Here the
   ledger runs on the device, during an interactive session, to tell a
   musician which take to listen to again.
3. The negative result above — that endpoint residual gets *worse* as the
   corruption grows, because convergence bends the surface to fit the bad point
   — is a concrete failure of the endpoint method at n = 20 on this
   architecture, and it is the reason the integrated form was chosen here.

State the relation to TracIn in those terms in any write-up. Every number in
this record comes from smooth synthetic data, and no human gesture has been
tested; that bound belongs in any write-up too.

Measurements: `tests/elm.c measure` for the 0.2.0 figures; the rest are from
a study whose program is not yet published, which iris-studies lists as S10
and points back to this record.
