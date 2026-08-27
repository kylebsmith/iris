# 0005 — Warm-start is the default correction; reroll is the only reseed

**Status:** accepted for the LIBRARY, 2026-08-21 — **superseded for the DEVICE
by D11.3, 2026-08-22.**
**Affects:** `iris.h` (`iris_correct`, `iris_zero_velocity`), `tests/audit.c`
checks 14 and 16. ~~the firmware's button policy~~ — see below.

> ### ⚠️ THIS IS NOT THE FIRMWARE'S POLICY AND HAS NOT BEEN SINCE 2026-08-22
>
> D11.3 (`admin/DECISIONS.md:181`) dropped `iris_correct` from the device: *"It was
> the mechanism behind a proposed research question; the honest instrument comes
> first and the question follows the instrument."* The firmware's record and
> delete paths both go to the ELM solve instead. Verified 2026-08-26: **zero
> occurrences of `iris_correct` anywhere under `firmware/app/`.**
>
> Everything below remains true of the LIBRARY, where `iris_correct` still exists,
> is still tested by checks 14 and 16, and still measures what it says. It has
> no production caller. Also note the baseline: the 30.5x and 9x figures are
> against **our own equal-budget cold restart**, not against any competitor and
> not against a converged rebuild.

## Context

v0.1 had one retrain path: reseed from the stored seed and run 600 epochs.
Every correction — "record one more example, fix it" — therefore rebuilt the
instrument from scratch. E4 measured what that costs, at 20 examples plus one
corrective example:

| policy | train_rms | drift far from the correction | S3 est. |
|---|---|---|---|
| warm, 17–20 epochs | 0.0190 | **0.0031** | **~1.1 ms** |
| cold 600 (v0.1's path) | 0.0185 | 0.0030 | ~30 ms |
| cold 30 (v0.1 at equal budget) | 0.0421 | **0.0304** | ~1.5 ms |

The cold restart at an honest budget rewrites the mapping *everywhere* —
which is precisely the documented way musicians lose accumulated technique to
retraining (Fiebrink & Sonami, NIME 2020). Warm-start reaches cold-600 parity
at 27× less cost and 9× less collateral change. The fix was never a better
optimiser; it was not throwing the weights away.

E4 also caught a spec bug that shaped format v2: rng state alone does not
close determinism, because **momentum velocity is hidden warm state** — a
reloaded instrument's next correction diverged bitwise from the in-memory
one. The rescue is free: zero the velocity at correction entry (a converged
net's velocities are already ~0; every correction metric identical to 4
decimals). That turns the momentum arrays into transient scratch, so the file
needs one extra word, not four extra weight arrays.

## Decision

- `iris_correct(k, epochs)` = zero velocity, then a warm `iris_train_epochs`
  burst **without reseeding**. Default budget 20 epochs.
- **The policy, stated in the header and owed to the firmware:** record or
  delete → `iris_correct` (same instrument, fixed). Explicit reroll gesture →
  `iris_retrain_new` (deliberately a NEW instrument). Nothing else reseeds.
- No recency parameter. E4 measured presenting the new example k extra times
  per epoch: k=1 nearly doubles epochs-to-parity (31 vs 17); k≥2 never
  reaches parity and oscillates on contradictory corrections (swing 0.144 vs
  0.022). The experiment was named for the feature it killed.
- Determinism becomes **event-sourced**: replaying the identical operation
  history is bit-identical (check 14, chains of length 10, delete included).
  The seed alone now reproduces only a from-scratch retrain; a saved file
  carries the rng stream (ADR 0006).

## Rejected alternatives

**Keep cold retrain as the default and just lower the budget.** Rejected by
the table above: cold-30 is the same speed as warm and 10× the drift. The
speed was never the interesting axis; the technique preservation is.

**Recency boosting.** Rejected, measured, above. It ships nowhere, not even
as a knob.

**Serialize the momentum velocities instead of zeroing them.** Rejected:
+300 bytes per file to preserve state whose measured information content is
four decimal places of nothing.

**Shrink-and-perturb (re-init scaled noise before correcting).** Rejected:
strictly worse than plain warm-start at every small budget measured
(train_rms 0.0263 vs 0.0190; drift 0.0158 vs 0.0035). The warm-start
generalisation gap it exists to fix does not manifest at this scale — where
users actively *prefer* overfit models (Fiebrink, CHI 2011).

## Consequences

- The felt correction loop drops from ~30–73 ms to ~1–3 ms (S3 est.) — under
  the audible threshold; the instrument feels continuously editable.
- The shipped call was independently re-measured against the integrated core
  (not the E4 fork): `iris_correct(20)` at 20+1 examples runs 0.041 ms vs
  1.249 ms cold-600 — **30.5×** (29.1× at 51 examples) — with far-field
  drift 0.0040 against cold-600's 0.0030 (audit gate ≤ 0.0045). At equal
  budget the preservation factor is **7.4×** (0.0040 vs cold-30's 0.0300),
  not the 9× E4's table suggests: E4's 0.0031 came from a measurement loop
  that re-zeroed velocity every epoch, which the shipped call deliberately
  does not do. 7.4× is the honest headline; the audit's bounds are set on
  the shipped numbers.
- Ten-correction chains drift ~0.027 far-field — but so does cold full
  retraining (0.028); the drift belongs to ten new examples reshaping a
  12-hidden net, not to the policy. Stated honestly wherever chains come up;
  per-correction anchoring (E5, opt-in) is the tool for the first few.
- Re-derive: `./build.sh audit` (checks 14–16 and the correction timing
  lines). Measured on Apple M4 Max (arm64), Apple clang 17.0.0, `-O2`,
  2026-08-21.
