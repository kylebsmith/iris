# 0009 — Ridge is mandatory: the float32 Gram is rank-deficient even on friendly data

**Status:** accepted, 2026-08-21 (decision made in quantnum + experiment E1, before integration)
**Affects:** `iris.h` (the Cholesky inside `iris_train_elm_ex`),
`tests/audit.c` check 21.

## Context

The obvious reading of ridge regression is "regularisation you tune, maybe
to zero". That reading is wrong here, and this ADR exists because someone
will eventually propose λ=0 "for exactness".

Measured (quantnum, confirmed at scale by E1): the unridged tanh-feature
normal matrix is numerically rank-deficient in float32 in **every realistic
scenario tested** — including the benign one, 20 well-spread examples (min
eigenvalue ≤ 0 at float32; Cholesky fails at λ=0 in all three quantnum
scenarios). This is not hostile-input insurance; it is the friendly case.

## Decision

- **Relative ridge:** λ = λ0 · trace(A)/(nh+1), added to the diagonal — it
  scales with the data instead of being an absolute magic number.
- **Deterministic escalation:** if the factorisation fails, double λ and
  retry, at most 8 times, from a pristine copy of the matrix (the
  factorisation writes only the lower triangle; the diagonal is parked).
  The count is returned and any escalation is reported via
  `IRIS_RIDGE_ESCALATED`. The schedule is fixed, so same seed + same examples
  is bit-identical even through escalation.
- The result: the solve **cannot fail** — SPD by construction. Verified in
  the integrated core, check 21: six hostile scenarios (256 duplicates,
  conflicting duplicates, dead dimension, 1e6 outliers, tight clusters,
  all-equal outputs) × five λ values × three widths = 90 solves, zero
  unfixable failures, at most 2 escalations ever used, zero non-finite
  outputs on probes including outside the input box.
- If escalation is somehow exhausted (unreachable by construction; it would
  mean the Gram accumulated to non-finite), the core reseeds to a finite
  deterministic state and reports `IRIS_NAN_TRAPPED` — never sits on broken
  weights, per ADR 0004.

## Rejected alternatives

**λ = 0 "for exact interpolation".** Rejected by the measurement this ADR
exists to preserve: it fails Cholesky on *friendly* data. There is no
λ=0 regime in float32 on this feature matrix.

**Absolute λ.** Rejected: the right magnitude depends on the trace, which
depends on nh and the example count. An absolute default that works at
nh=12/20 examples is wrong at nh=48/256.

**SVD or QR instead of Cholesky.** Rejected: 3–10× the flops and a large
workspace, to buy robustness the ridge already guarantees more cheaply on a
(nh+1)² problem that tops out at 49×49.

**Compensated summation to postpone the rank deficiency.** Rejected — same
finding as ADR 0008: weight-space-only error, no audible or grid-level
effect; and it would not remove the deficiency, only move it.

## Consequences

- λ0 becomes the stability/liveliness knob with a floor, not an off switch
  (1e-5 never failed anywhere; the shipped defaults are 1e-4/1e-3).
- Escalation is visible to callers (return count + status), so "the data
  was harder than usual" is a fact the instrument can display, not a
  mystery.
- Re-derive: `./build.sh audit` (check 21); the eigenvalue evidence is in
  the quantnum scratch fork. Measured on Apple M4 Max (arm64), Apple clang
  17.0.0, `-O2`, 2026-08-21.
