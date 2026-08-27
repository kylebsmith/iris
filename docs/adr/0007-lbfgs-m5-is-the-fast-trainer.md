# 0007 — L-BFGS(m=5) is the fast trainer; SGD+momentum is retained verbatim

**Status:** accepted, 2026-08-21 (decision made in experiment E2, before integration)
**Affects:** `iris.h` (`iris_train_lbfgs`, `iris_eval_loss`, `IRIS_LBFGS_WORK`),
`tests/audit.c` checks 17–19 and the cost table.

## Context

Training is the whole felt latency of the correction loop; prediction was
never the problem. E2 raced full-batch L-BFGS with Armijo backtracking
against the shipping 600-epoch SGD baseline, 16 seeds, to the baseline's
*exact* final error:

| examples | baseline | L-BFGS | speedup | worst seed |
|---|---|---|---|---|
| 20 | 0.903 ms | 0.214 ms | 4.3× | 3.2× |
| 50 | 2.245 ms | 0.519 ms | 4.7× | 2.5× |
| 200 | 9.145 ms | 4.489 ms | 2.3× | 1.1× |

At equal wall-clock the loss is 2.5–2.7× *lower* with better generalisation
at every count. The line search only accepts steps that reduce the loss, so
divergence is structurally impossible. Reroll character shifts in the
direction users like: ~1.7× livelier in the gaps, slightly steadier at the
demos. Honest miss, recorded: at 200 examples the mean is 2.3× and the worst
seed 1.1× — the headline belongs to 5–50 examples, where the musician lives.

The surprise worth this ADR's existence: **more history memory is worse.**
m=5 hits 16/16 seeds at 200 examples; m=8 has a runaway seed; m=12 halves
the hit rate at 3.6× the iterations. A double-precision diagnostic ruled out
float32 rounding — stale curvature pairs poison the inverse-Hessian model on
this nonconvex surface.

## Decision

- `iris_train_lbfgs(k, max_iters, work, bytes)` with **m=5 hard-coded** — a
  measured optimum, not a knob. A knob would invite the "more memory must
  be better" intuition that the measurement killed.
- Work buffer is caller-supplied transient scratch (`IRIS_LBFGS_WORK`), never
  serialized; too small refuses with weights untouched. 5,592 B at 2-16-4;
  176,560 B at the 32-64-16 maximum (fits the S3's 512 KB but is a third of
  it — sized consciously, refused loudly).
- Budget mapping for epoch migrants: iters ≈ epochs/6, floor ~70 iterations
  (~400 epoch-equivalents) — short-budget L-BFGS is measurably less steady
  near demos.
- **`iris_train_epochs` stays byte-for-byte, forever**: it is the
  Wekinator-fidelity reference, the byte-compatibility path for existing
  budget semantics, and the fallback if any on-device gate fails. Promotion
  of L-BFGS to the firmware's default train button is gated on the two
  board-owner measurements (same-seed memcmp on the S3; 50-example
  wall-clock < 50 ms).

## Rejected alternatives

**RMSProp (memlp's choice).** Rejected at survey with numbers: 1,991–13,884
iterations to match baseline — up to 10.7× slower than the shipping SGD.

**Levenberg-Marquardt as the interactive trainer.** Rejected by its own
pre-registered criterion: the LM hybrid reaches baseline-equal fit in
0.58–0.69× the wall-clock *of L-BFGS* — i.e. slower — and needs a W×W arena
slice (208 KB at 32 hidden). Preserved as a deferred "deep polish" tier: its
error floor is 2–5× lower than anything else measured, if a "finalize
instrument" gesture ever exists.

**m as a tunable parameter.** Rejected: every measured value other than 5 is
worse, and the failure mode (a runaway seed) is exactly the kind a user
cannot diagnose.

**LR schedules on SGD instead.** Rejected: the gap between any scheduled
first-order method and L-BFGS/LM measured 10–40×, larger than the most
optimistic schedule gain.

## Consequences

- Same seed under the *same* trainer is still bit-identical (check 17), but
  an old seed retrained under L-BFGS is a different, equally valid
  instrument than under SGD. Anyone who wants their exact old instrument
  uses the saved file, or `iris_train_epochs` — release-notes headline.
- The audit's cost table prints both trainers so the trade is visible on
  every machine it runs on.
- Re-derive: `./build.sh audit` (checks 17–19, cost table). Measured on
  Apple M4 Max (arm64), Apple clang 17.0.0, `-O2`, 2026-08-21.
