# 0008 — The closed-form trainer: same network, different mathematics

**Status:** accepted, 2026-08-21. Governs `iris.h` 0.2.0 (`iris_train_elm`).
**Affects:** `iris.h` PART 8d (`iris_train_elm`,
`iris_internal_train_elm_ex`, `iris_internal_artanh`, `iris_internal_logit`,
`IRIS_ELM_SCRATCH`, `IRIS_ARENA_ELM`), `tests/audit.c` (the three "ELM:"
checks), `tests/elm.c`.

## Context

Keep the network Wekinator trains, one hidden layer feeding the outputs, and
fit it without backpropagation: freeze the seeded random hidden layer and solve
the output layer exactly by ridge least squares, one (nh+1) × (nh+1) Cholesky
factorisation, where nh is the number of hidden units. This is an extreme
learning machine (ELM). Against 600 epochs of backpropagation, on the
development laptop (Apple M4 Max, Apple clang 17, `-O2`; recorded, program in
iris-studies S15):

| nh | demonstrations | backpropagation | closed form | speed-up | backpropagation recall / grid | closed-form recall / grid |
|---|---|---|---|---|---|---|
| 12 | 50 | 2.590 ms | 0.0061 ms | 425× | .0074 / .0112 | .0103 / .0133 |
| 48 | 50 | 7.926 ms | 0.0268 ms | 296× | .0077 / .0116 | **.0066 / .0109** |

At 48 hidden units the fit is better than backpropagation's on both recall and
held-out error; `tests/audit.c` holds that ("ELM: nh=48 fit floor beats
backprop"). On the board the solve takes 1.5 ms at 20 demonstrations and 3.5 ms
at 50 (ES3C28P, [log](../board/2026-09-25-es3c28p.txt)).

The reroll is the reason this is more than a speed feature. A frozen random
layer delivers the seed's character undiluted, where backpropagation trains
part of its randomness away, while the exact solve holds the demonstrations
whatever the seed. At 12 hidden units the closed-form reroll moved the sound at
the demonstrations less than backpropagation's (0.016 against 0.017) and in the
gaps about 45% more (0.11 against 0.076) (recorded; iris-studies S15).
`tests/audit.c` ("ELM: same seed, same bits; reroll character") prints the
current figures. A new seed is a new instrument.

Two measurements make the solve work in single precision:

- **A hidden-layer gain of 2/√n_in** for the frozen layer. At the
  backpropagation starting scale, tanh of a normalised input is nearly
  linear, the features are nearly collinear, and the single-precision normal
  matrix is rank-deficient. The sweep behind the constant is printed in
  `iris.h` PART 8d; its program is iris-studies S06.
- **The ridge is mandatory**: a separate record,
  [0009](0009-ridge-is-mandatory.md), because it will be proposed for
  deletion on its own.

## Decision

- `iris_train_elm(k, lam0, scratch, bytes)`, with working memory the caller
  supplies, sized by `IRIS_ELM_SCRATCH(nh, n_out)`. A refusal leaves every byte
  of the instrument as it was, except that a poisoned demonstration sets
  `IRIS_NAN_TRAPPED`.
- Recommended `lam0`: **1e-4 at 12 hidden units**, and **1e-3 at 48**, the
  configuration that fits better than backpropagation.
- **Refuse fewer than 8 hidden units.** Four frozen random features cannot
  recall five demonstrations. (`iris_init` refuses fewer than 8 for every
  trainer.)
- The solve targets logit space (`iris_internal_logit`, the exact inverse of
  the library's sigmoid), so the ordinary forward pass lands on the normalised
  targets, and the result is an ordinary instrument that plays, saves and
  loads like any other. **It is a bounded-output variant of the
  backpropagation network's output layer, not an equivalent.** The
  disagreement between this logit-space head and a closed-form head with
  linear outputs reached 4.6e-2, largest near the demonstrations (recorded;
  iris-studies S15). That is structural, not a defect, and it is why no
  equivalence is claimed.
- The default trainer stays `iris_train`. The closed-form trainer is chosen per
  instrument.

## Rejected alternatives

**Make the closed-form trainer the default.** Rejected: its reroll character is
a different musical object (frozen randomness, a head that is linear in logit
space), and the default's character is the tested, documented behaviour.
Changes of character are opt-in.

**Allow 4 hidden units for a "lively corner".** Rejected by the recall failure
above. The lively corner lives at 12 hidden units for a frozen layer.

**Compensated (Kahan) summation in building the normal matrix.** Rejected: the
single-precision output weights miss a double-precision reference by 2e-4 to
6e-4 in weight space only, and the error on the probe grid is unchanged
(recorded; iris-studies S15). Paying cycles to fix an error nobody can hear or
measure downstream buys nothing.

**Overlay the working memory on the momentum arrays.** Rejected: the solve
needs (nh+1)² floats, the velocities are about nh·(n_in + n_out), far too few
at 48 hidden units. A separate block the caller supplies states the real cost.

## Consequences

- `IRIS_RIDGE_ESCALATED` is in the status vocabulary: escalation is valid and
  reported, never silent.
- The solve does not advance the instrument's random state (a closed-form
  solve is not an event in the training history); it draws the frozen layer
  from the seed with a local generator.
- Output weights may exceed the weight limit of 16. Such an instrument plays,
  saves and loads normally; `iris_train` is the way back to gradient training
  (see `IRIS_W_LIMIT` in `iris.h`).
- Re-derive: `sh build.sh audit` (the "ELM:" checks and the training-cost
  table) and `sh build.sh elm`.

Measurements: iris-studies S15, S06.
