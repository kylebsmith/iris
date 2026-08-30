# 0019 — The bad-example detector integrates the residual; it does not sample the endpoint

**Status:** accepted, 2026-08-22
**Affects:** `iris.h` PART 8f (`iris_example_stress`, `iris_worst_example`,
`iris_worst_example_id`, `IRIS_STRESS_FLAG`, `IRIS_STRESS_MIN_EX`), `IRIS_ARENA`,
`tests/audit.c` check 32, `firmware/app/core/surface.c` (the FIGHTING band).

## Context

The trial-and-error trap in interactive ML is that when the instrument feels
wrong the musician has no idea *which* demonstration is wrong, so they
re-record at random. A research pass proposed the obvious free detector: after
training, rank the examples by how badly the model still misses them. Measured
at 6,000 epochs it ranks a deliberately corrupted demonstration first in 16
trials out of 20 at +0.05, where leave-one-out cross-validation — which costs
*n* full retrains — manages 1.

## What killed the obvious version

[ADR 0017](0017-train-to-the-plateau-not-to-a-constant.md) landed first, and
it invalidates the measurement. Corrupt one demonstration of twenty by +mag on
one output; count how often it ranks first (nh=12, 8 outputs, 20 trials):

| corruption | +0.05 | +0.10 | +0.20 | +0.40 |
|---|---|---|---|---|
| final residual @ 6,000 epochs (as proposed) | 16/20 | 10/20 | 13/20 | 8/20 |
| final residual @ `iris_train_converge` | 8/20 | 5/20 | 2/20 | 2/20 |
| leave-one-out CV @ 6,000 epochs | 1/20 | 1/20 | 10/20 | 11/20 |
| **integrated residual (chosen)** | 6/20 | **18/20** | **17/20** | **17/20** |

Read the second row backwards: **the final residual gets worse as the mistake
gets bigger**, and 2/20 at +0.40 is barely above the 1/20 of guessing. Of
course it does — give the optimiser enough epochs and it bends the surface far
enough to fit the bad point too, after which the bad point looks like every
other point. A detector measured at one budget and shipped at another is a
feature that quietly does nothing.

## Decision

Sum each example's squared error over **every epoch** of the run, in a
`cap`-float ledger carved from the arena. This measures how long an example
*fought*, not whether it eventually lost. It is stable across budgets — 5/17/17/18
at 6,000 epochs, 9/17/17/16 at 20,000, 11/16/17/15 at 60,000 — and monotone in
corruption size, which is what a person expects a detector to do.

**The flag is the margin, not the level.** Worst-of-n stress rises with n on
clean data with nothing wrong at all: 1.76 at 5 examples, 2.95 at 20, 6.34 at
50, 9.23 at 100. A UI wired to a fixed level would be silent on small rigs and
would cry wolf on large ones. Worst divided by second-worst does not drift:
clean maximum 1.44 / 2.27 / 2.06 at 20 / 50 / 100. `IRIS_STRESS_FLAG` is 2.5.

**Below `IRIS_STRESS_MIN_EX` (12) it returns −1 and says nothing.** At 10
examples the clean margin reaches 5.04 — a false accusation. An example can
only be caught disagreeing with a crowd if there is a crowd. That is the
situation, not a tuning failure.

## What it costs — and this is where the research pass's claim was wrong

`sizeof(float) * cap`: **512 B at cap 128, 1 KB at cap 256**, plus one float
add per example per epoch (under 0.1% of the backprop already being done for
that example). The "zero bytes" claim applied to the endpoint version, which
does not work.

## The rejected alternatives

- **Leave-one-out CV.** *n* full retrains for a detector that is 13× worse at
  the smallest corruption and never better than the ledger at any size.
- **A k-NN leave-one-out residual** (free, training-independent, uses the
  existing sampler). Measured: 0–2/20 at +0.05 and +0.10. With twenty
  demonstrations in a 2-D space, a 5% displacement is inside the natural
  spread between neighbours.
- **Sampling the residual at a fixed early checkpoint.** 6,000 epochs is good
  at +0.05 and bad at +0.40; 600 is the reverse. Tuning a checkpoint to the
  corruption size means knowing the answer first.

## The caveat

The corruption model is a clean offset on one output of a smooth, noiseless
truth. A real inconsistent human demonstration is not that. Nothing here has
been checked against a recorded gesture, and the app's own test had to
*construct* a consistent mapping to exercise the detector at all — with the
app's synthetic per-mark candidates, every mark contradicts every other one
(settled train MSE 1.4e-2 at forty marks) and the margin sits at 1.15, which
is the detector correctly declining to accuse anybody.

## Relation to prior work — read this before claiming novelty

Integrating a per-example quantity along the training trajectory, and using it
to rank examples, is **TracIn**: Pruthi, Liu, Kale & Sundararajan, *Estimating
Training Data Influence by Tracing Gradient Descent*, NeurIPS 2020,
arXiv:2002.08484 (`iris-references/papers/pruthi-2020-tracin.pdf`).

This is not adjacent work, it is the same idea. TracIn defines *self-influence*
— the influence of a training point on its own loss — and evaluates it by "the
fraction of mislabelled data recovered", which is the metric the table above
uses under a different name. Their §4.1 and §5 do exactly what this record
does: corrupt a known fraction of the training set, rank, and count how many
corrupted points appear at the top.

The related earlier line is Koh & Liang, *Understanding Black-box Predictions
via Influence Functions*, ICML 2017, arXiv:1703.04730
(`iris-references/papers/koh-liang-2017-influence-functions.pdf`), which
estimates the same influence from the converged model rather than the
trajectory — the "sample the endpoint" approach whose failure this record
measures.

**So the honest claim is not "we invented this."** It is narrower and still
worth making:

1. TracIn needs gradients, saved checkpoints, and a separate influence
   computation. The ledger here is one float32 per example, accumulated inside
   the loop that was already running (`iris.h` PART 8f), costing no extra
   arithmetic and no extra pass — it is a scalar approximation of trajectory
   self-influence that fits on a microcontroller.
2. Nothing in that literature is run on-device, during an interactive session,
   to tell a musician which take to delete. TracIn's setting is offline dataset
   cleaning for large models.
3. The negative result above — that endpoint residual gets *worse* as the
   corruption grows, because convergence bends the surface to fit the bad point
   — is a concrete failure of the endpoint method at n = 20 on this
   architecture, and it is the reason the integrated form was chosen here.

**Before this is defended, read TracIn.** State the relation in those terms.
Claiming the mechanism as new would not survive a reviewer who knows it, and
the narrower claim is both true and sufficient.

**Also still open:** every number in this record comes from smooth synthetic
data. No human gesture has been tested. That bound belongs in any write-up of
this result.
