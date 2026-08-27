# 0004 — Guards report, never silently mutate

**Status:** accepted, 2026-08-21 (decision made in experiment E8, before integration)
**Affects:** `iris.h` (status enum, `iris_isbad`, train/predict guards,
`IRIS_W_LIMIT`), `tests/audit.c` check 13, `tests/guards_ab.c`.

## Context

The v0.1 core had passive guards with holes that had teeth, all measured live:

- **`iris_clampf` passes NaN straight through** — every comparison with NaN is
  false, so `clamp(NaN, lo, hi)` returns NaN. A glitched sensor at play time
  NaN'd every output; a NaN-poisoned example destroyed the instrument (2,883
  NaNs reached predict in E8's trace).
- A hostile-but-legal momentum setting (0.99) silently pushed weights to
  |w| = 20.8–42.8 — training "succeeded" and returned a number.

The temptation is to fix these by silently repairing: clamp the NaN, reseed
quietly, carry on. That converts a detectable fault into an undetectable one
— the instrument plays *something*, and the musician debugs their sensor,
their mapping, and their own hands before suspecting the library.

## Decision

Every guard follows one rule: **report via a queryable status, never mutate
silently, never print** (the core has no libc to print with).

- A status enum (`IRIS_STATUS_OK` = 0, `IRIS_TRAINING_DIVERGED`, `IRIS_NAN_TRAPPED`,
  `IRIS_RIDGE_ESCALATED`) and `iris_get_status(k)`. Healthy is zero, so
  `if (iris_get_status(k))` reads as "is something wrong?".
- NaN/Inf detection is by bit pattern (`(bits & 0x7F800000) == 0x7F800000`)
  — no libc, no fenv, immune to finite-math optimisation of comparisons.
- **Training refuses poisoned examples at the door**: previous weights
  bit-preserved, the bad example left in the store where the musician can
  find and delete it. Every trainer (SGD, L-BFGS, ELM) uses the same door.
- **Predict substitutes mid-range for a non-finite output and says so** —
  the one place a mutation is allowed, because the alternative is NaN in an
  audio parameter *right now*; the status still reports it.
- **|w| ≤ 16 clamp + divergence breaker**: measured healthy peak is 2.8, so
  16 is 5.7× headroom and the clamp provably never fires on a healthy run;
  when it does fire, training stops and reports rather than looping.

The proof that guards cost nothing when healthy is structural: `build.sh`
builds the core twice, with guards and with `-DEW_NO_GUARDS`, runs the same
recipe, and requires bit-identical blobs. Overhead measured ≤2.5%.

## Rejected alternatives

**Silent repair (clamp the NaN, reseed and retrain quietly).** Rejected —
it makes the library lie. The measured failure would have kept happening;
only the evidence would have disappeared.

**Return codes on every function instead of a status field.** Rejected: the
existing API's signatures are frozen (everything in `ports/` compiles against
them), and a musician's control loop wants one cheap post-hoc question, not
error plumbing through every call site.

**Assert/abort on NaN.** Rejected twice over: no libc in the core, and a
crashed instrument on stage is strictly worse than a reported one.

**Skip the predict-side backstop because training already refuses NaN.**
Rejected: the sensor can glitch at play time, after training was clean. That
was E8's motivating measurement.

## Consequences

- `iris_predict` writes the status through a const cast — reporting beats
  const purity; the alternative (a new `iris_predict_st`) would have split the
  API in two for one word of state. Noted in the header at the cast.
- Guards are compiled out with `-DEW_NO_GUARDS` for the A/B proof only; the
  shipping build always has them.
- Re-derive: `./build.sh audit` (check 13 and the final guards A/B line).
  Measured on Apple M4 Max (arm64), Apple clang 17.0.0, `-O2`, 2026-08-21.
