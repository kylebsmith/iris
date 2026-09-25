# 0004 — Guards report, never silently mutate

**Status:** accepted, 2026-08-21. Governs `iris.h` 0.2.0. Two parts no longer
hold as first written: the weight limit fires on some good fits on sharp
targets (the figures, and why 16 is kept, are at `IRIS_W_LIMIT`), and
`iris_predict` takes a non-`const` instrument instead of writing the status
through a const cast.
**Affects:** `iris.h` (the `iris_status` enumeration, `iris_get_status`,
`iris_internal_isbad`, `iris_internal_trainable`, the guard in `iris_predict`,
`IRIS_W_LIMIT`), `tests/audit.c` ("NaN never reaches output; guards report",
"a refused train changes nothing but the status"), `tests/guards_ab.c`.

## Context

A clamp does not stop a not-a-number (NaN): every comparison with NaN is
false, so `clamp(NaN, lo, hi)` returns NaN. `iris_internal_clampf` still
behaves that way, which is why the guards below exist. Without them a glitched
sensor at play time turns every output into NaN, and one NaN in a stored
demonstration poisons every weight in the first epoch. A legal but hostile
learning rate or momentum runs the weights away while training still returns a
number (the table at `IRIS_W_LIMIT` in `iris.h`).

The tempting fix is silent repair: clamp the NaN, reseed quietly, carry on.
That turns a detectable fault into an undetectable one. The instrument plays
*something*, and the musician debugs their sensor, their mapping and their own
hands before suspecting the library.

## Decision

Every guard follows one rule: **report through a status that can be queried,
never change anything silently, never print** (the library has no C library
to print with).

- A status enumeration with `IRIS_STATUS_OK` = 0, read by `iris_get_status(k)`.
  Healthy is zero, so `if (iris_get_status(k))` reads as "is something
  wrong?". The other values and what sets each are listed at the enumeration
  in `iris.h`.
- NaN and infinity are detected by bit pattern (`(bits & 0x7F800000) ==
  0x7F800000`), with no C library, no floating-point environment, and no
  comparison an optimiser may fold away.
- **Every trainer refuses a poisoned store at the door**
  (`iris_internal_trainable`): the weights are left as they were, the status
  says `IRIS_NAN_TRAPPED`, and the bad demonstration stays in the store where
  the musician can find and delete it. Backpropagation and the closed-form
  trainer use the same door. A NaN that appears partway through a run
  reseeds the weights to a finite, unfitted start and sets the same status:
  a change, but a reported one.
- **`iris_predict` substitutes the centre of the demonstrated range for a
  non-finite output, and says so.** This is the one place a value is replaced,
  because the alternative is NaN in an audio parameter now; the status still
  reports it. An instrument that was never fitted plays the same substitute
  and reports `IRIS_NOT_FITTED`.
- **A weight limit of 16 and a divergence breaker.** A weight or bias past
  ±16 is clamped to exactly 16, the run stops, and the status says
  `IRIS_TRAINING_DIVERGED`; training reports instead of looping. The limit
  also fires on some good fits on sharp targets: `iris.h` gives the figures
  at `IRIS_W_LIMIT`, and why the limit is not raised.

The proof that the guards cost nothing on a healthy run is structural:
`sh build.sh audit` builds `tests/guards_ab.c` with the guards and with
`-DIRIS_NO_GUARDS`, and requires the same bits from every guarded region, and
different bits from control lines that a guard must change.

## Rejected alternatives

**Silent repair (clamp the NaN, reseed and retrain quietly).** Rejected: it
makes the library lie. The failure would keep happening; only the evidence
would disappear.

**Return codes on every function instead of a status.** Rejected: a
musician's control loop wants one cheap question after the fact, not error
plumbing through every call site. Functions whose failure is a mistake in the
call still report it by return value; the status is for numerical health.

**Assert or abort on NaN.** Rejected twice over: there is no C library to abort
with, and a crashed instrument on stage is worse than a reported one.

**Skip the playing-side backstop because training already refuses NaN.**
Rejected: the sensor can glitch at play time, after training was clean.

## Consequences

- `iris_predict` takes a non-`const` instrument, because it writes the
  network's activations and, when it has something to report, the status.
- `-DIRIS_NO_GUARDS` exists for the comparison above; a normal build always
  has the guards.
- Re-derive: `sh build.sh audit`.

Measurements: none; the checks named above are in this repository.
