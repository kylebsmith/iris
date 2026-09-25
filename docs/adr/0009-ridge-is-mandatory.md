# 0009 — The ridge is mandatory: the single-precision normal matrix is rank-deficient even on friendly data

**Status:** accepted, 2026-08-21. Governs `iris.h` 0.2.0; the handling of an
exhausted escalation is superseded, see the Decision.
**Affects:** `iris.h` PART 8d (the Cholesky factorisation inside
`iris_internal_train_elm_ex`), `tests/audit.c` ("ELM: solve cannot fail on
hostile data").

## Context

The obvious reading of ridge regression is "regularisation you tune, maybe to
zero". That reading is wrong here, and this record exists because someone will
propose λ = 0 "for exactness".

Without a ridge, the normal matrix of the closed-form trainer's tanh features
is numerically rank-deficient in single precision in every realistic scenario
tested, including the benign one of 20 well-spread demonstrations: its smallest
eigenvalue is at or below zero, and the Cholesky factorisation fails at λ = 0
in all three scenarios examined (from a study whose program is not yet
published). This is not insurance against hostile input; it is the friendly
case.

## Decision

- **A relative ridge:** λ = lam0 · trace(A)/(nh+1), plus a floor of 1e-7,
  added to the diagonal, where A is the normal matrix and nh the number of
  hidden units. It scales with the data instead of being an absolute
  constant.
- **Deterministic escalation:** if the factorisation fails, double λ and try
  again, at most 8 times, from an untouched copy of the matrix (the
  factorisation writes only the lower triangle, and the diagonal is kept
  aside). The number of doublings is returned, and any escalation is reported
  as `IRIS_RIDGE_ESCALATED`. The schedule is fixed, so the same seed and the
  same demonstrations give the same bits even through escalation.
- On friendly and hostile data at the recommended `lam0` the solve does not
  fail: `tests/audit.c` runs six scenarios (50 ordinary demonstrations, and
  five hostile ones: one demonstration repeated 256 times, two tight
  clusters, output outliers of ±1e6, an input that never moves, outputs all
  equal) at five values of `lam0` and three widths, 90 solves, with no
  unfixable failure, at most 2 doublings, and no non-finite output on probes
  inside and outside the demonstrated range.
- Escalation can run out, and then the solve refuses: `lam0` = 0 on 128
  demonstrations all made at one gesture fails all nine attempts (`iris.h`
  PART 8d). A refusal leaves every byte of the instrument as it was and
  reports through the return value, following
  [0004](0004-guards-report-never-mutate.md). This replaces the earlier
  handling of an exhausted escalation, which reseeded the instrument.

## Rejected alternatives

**λ = 0 "for exact interpolation".** Rejected by the measurement this record
exists to preserve: the factorisation fails on friendly data. There is no
λ = 0 regime in single precision on this feature matrix.

**An absolute λ.** Rejected: the right magnitude depends on the trace, which
depends on the number of hidden units and of demonstrations. An absolute value
that works at 12 hidden units and 20 demonstrations is wrong at 48 and 256.

**Singular value or QR decomposition instead of Cholesky.** Rejected: several
times the arithmetic and a large workspace (an estimate from operation counts,
not a measurement), to buy robustness the ridge already provides more cheaply
on a problem of at most 65 × 65.

**Compensated summation to postpone the rank deficiency.** Rejected for the
reason in [0008](0008-elm-same-network-better-math.md): the error it removes is
in weight space only, with no effect on the probe grid, and it would move the
deficiency, not remove it.

## Consequences

- `lam0` is a knob between stability and liveliness with a floor, not an off
  switch (1e-5 never failed in the scenarios the same study examined; the
  recommended values are 1e-4 at 12 hidden units and 1e-3 at 48).
- Escalation is visible to callers through the return count and the status,
  so "the data was harder than usual" is a fact the instrument can display.
- Re-derive: `sh build.sh audit`.

Measurements: every figure that no check prints is from a study whose program
is not yet published, which iris-studies lists as S16 and points back to this
record.
