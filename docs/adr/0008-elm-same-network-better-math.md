# 0008 — ELM: same network, better math

**Status:** accepted, 2026-08-21 (decision made in experiment E1, before integration)
**Affects:** `iris.h` (`iris_train_elm`, `iris_retrain_elm_new`,
`iris_artanh`, `iris_logit`, `IRIS_ELM_SCRATCH`), `tests/audit.c` checks 20–22.

## Context

Keep the exact Wekinator topology; never backprop. Freeze the seeded random
hidden layer, solve the output layer exactly by ridge least squares — one
(nh+1)×(nh+1) Cholesky. E1's numbers against the 600-epoch baseline:

| nh | examples | backprop | ELM | speedup | bp recall/grid | ELM recall/grid |
|---|---|---|---|---|---|---|
| 12 | 50 | 2.590 ms | 0.0061 ms | 425× | .0074 / .0112 | .0103 / .0133 |
| 48 | 50 | 7.926 ms | 0.0268 ms | 296× | .0077 / .0116 | **.0066 / .0109** |

Retrain at 50 examples in well under a millisecond estimated on the S3, and
at nh=48 the fit is *better* than backprop on both recall and
generalisation. The reroll finding is the reason this is not merely a speed
feature: at nh=12 the ELM reroll is **simultaneously steadier at the demos
(0.016 vs 0.017) and ~45% livelier in the gaps (0.11 vs 0.076)**. The frozen
random layer delivers the seed's character undiluted — backprop partially
trains its randomness away — while the exact solve nails the demos
regardless of seed. A new seed literally *is* a new instrument.

Two findings are load-bearing:

- **Gain 2/√n_in** for the frozen layer (measured optimum of {0.71..4.0}).
  At the backprop init scale, tanh of a [0,1] input is nearly linear, the
  features are nearly collinear, and the float32 Gram is rank-deficient.
- **The ridge is mandatory** — separate ADR (0009), because it will be
  proposed for deletion independently.

## Decision

- `iris_train_elm(k, lam0, scratch, bytes)` and `iris_retrain_elm_new(...)`,
  caller-supplied scratch via `IRIS_ELM_SCRATCH` (884 B at nh=12, 10.4 KB at
  nh=48), hard refusal with weights untouched if short.
- Defaults: **λ0 = 1e-4 at nh=12** (reroll bands pass with room: near
  0.017, gap 0.110). **nh=48 / λ0 = 1e-3** documented as the
  "fits better than backprop" configuration.
- **Refuse nh < 8.** Four frozen random features cannot recall five demos —
  the near-band fails at every λ measured. Shipping that config would break
  the reroll promise; refusal is the honest interface.
- The solve targets logit space (`iris_logit`, the exact inverse of our Padé
  sigmoid) so the shipping sigmoid forward pass lands on the normalized
  targets, and the trained artifact is an ordinary v1/v2 instrument
  (bit-identical save/load round trip, 441 probes). **Stated plainly: this
  is a bounded-output *variant* of the backprop head, not an equivalent.**
  The measured disagreement between the logit-space sigmoid head and a
  linear-head ELM reaches 4.6e-2, largest *near the demos* — structural,
  not a bug. Nobody gets to claim equivalence later; this paragraph is why.
- Default trainer unchanged. ELM is chosen per-instrument.

## Rejected alternatives

**Make ELM the default trainer.** Rejected: its reroll character is a
different musical object (frozen randomness, linear-in-logit head), and the
default's character is the tested, documented behaviour. Character changes
are opt-in.

**Allow nh=4 "lively corner" ELM configs.** Rejected by measurement, above.
The lively corner simply lives at nh=12 for a frozen layer.

**Compensated (Kahan) summation in the Gram build.** Rejected: float32 β
misses the double-precision reference by 2–6e-4 *in weight space only*;
grid RMSE is unchanged. Paying cycles to fix an error nobody can hear or
measure downstream is decoration.

**Overlay the scratch on the momentum arrays.** Rejected: (nh+1)² floats vs
~nh·(ni+no) — ~24× too small at nh=48. A separate caller-supplied block is
honest about the real cost.

## Consequences

- The reroll button gets objectively better at nh=12, not just faster —
  release-notes headline ("a new reroll feel").
- `IRIS_RIDGE_ESCALATED` enters the status vocabulary: escalation is valid
  and reported, never silent.
- ELM does not advance `k->rng` (a closed-form solve is not an event in the
  correction history); `iris_retrain_elm_new` resets the stream exactly as
  `iris_retrain_new` does.
- Re-derive: `./build.sh audit` (checks 20–22, cost table). Measured on
  Apple M4 Max (arm64), Apple clang 17.0.0, `-O2`, 2026-08-21.
