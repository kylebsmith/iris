# 0020 — SETTLE is a separate act from TRAIN, and it reseeds

**Status:** accepted, 2026-08-22
**Extends** [`0012`](0012-the-hold-is-the-explicit-act.md) and
[`0008`](0008-elm-same-network-better-math.md).
**Affects:** `firmware/app/core/app.h`, `core/app.c` (`button_poll`,
`ewa_tick`), `core/surface.c` (`ewa__settle_*`, the bar, the FIGHTING band),
`host/app_test.c` T-REVIEW2 §8 and T-SETTLE.

## Context

[ADR 0017](../../docs/adr/0017-train-to-the-plateau-not-to-a-constant.md) makes
backprop-to-convergence the library's recommended trainer, and the direction is
to make the app use the new trainer with an honest progress bar.

The app cannot simply do that. Its per-commit retrain runs **inside the hot
half's 5 ms model lock, on every demonstration**, and the instrument has to
keep playing through it — which is why it is the ELM closed-form solve at
~1 ms. Converged backprop is ~1 s on the S3. A second inside that lock is not
a retrain, it is a dropout on every mark the musician sets.

## Decision

Convergence is a **separate, explicit act**, in the spirit of ADR 0012: hold
the BOOT button past `EWA_SETTLE_HOLD_US`. A tap still toggles the mode on
release, exactly as before; the release after a hold is swallowed, because
"make it better" must never also mean "and put me in the other mode".

It runs `iris_train_slice` one slice per frame, each slice sized from the
**measured** duration of the previous one to fit inside the lock, with the bar
drawn from `iris_train_progress()`. A sliced run is bit-identical to the blocking
one (library check 31), so nothing about the instrument's identity depends on
how the frames fell. Touching the glass abandons it.

**It is worth the wait, measured on the app's own sixteen-mark test
instrument: ELM train MSE 1.47e-2 → 9.7e-7 settled.**

## The hard part: it reseeds, and that looks like it breaks ADR 0005

It does not. ADR 0005's rule is that *correction* never reseeds and *reroll*
is the only act that produces a new instrument. SETTLE reseeds from the
instrument's **own seed**, so same seed plus same demonstrations gives the same
instrument, bit for bit. It is `iris_retrain_new(k, iris_seed(k), …)` with the
plateau trainer.

The alternative — warm-starting backprop from the ELM weights — was tried and
**measured to fail on the app's own data: the first epoch drove a weight past
`IRIS_W_LIMIT` and the divergence guard correctly stopped the run after one
epoch at err 1.5e-1.** The ELM head is a different model family: a hidden layer
drawn at gain `2/sqrt(n_in)` and frozen, plus an output layer solved in logit
space. Dropping a first-order optimiser into weights another method chose, at
magnitudes it did not scale for, is not "more training".

Worth recording precisely because it does *not* reproduce in the library: on
the smooth reference task the same warm start diverges at no learning rate
between 0.001 and 0.1, across three example counts and nine seeds. It had to
be found in the app.

## Consequences, stated rather than hidden

- While the bar runs, the instrument is genuinely re-forming and sounds like
  it. That is what retraining is; the bar says how long.
- **Abandoning restores the instant solve.** A half-converged net is not a
  partial improvement on the ELM solve, it is a different and probably worse
  instrument, and the musician who touched the glass asked to *play*. ~1 ms.
- A settle the guard stopped is not a settled instrument: the band says
  DIVERGED and the instant solve is put back.
- A new demonstration after a settle re-solves instantly with ELM and sets
  `settle_due`, so the convergence resumes at the next idle frame rather than
  being silently forgotten.
- Only a settle can raise the FIGHTING band, because the evidence is the
  integrated training residual ([ADR 0019](0019-the-residual-ledger-integrates-it-does-not-sample.md))
  and the instant solve has no epochs to integrate over.
