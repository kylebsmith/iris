# 0017 — Train to the plateau, not to a constant

**Status:** accepted, 2026-08-22
**Affects:** `iris.h` (`iris_train_converge`, `iris_train_begin`,
`iris_train_slice`, `iris_train_progress`, the masthead), `tests/audit.c` check 31
and the cost table, `README.md`, `docs/frontier/REPORT.md` §0.1 and §1.5.

## Context

`iris.h` recommended `iris_train_epochs(k, 600)` from v0.1. That number was
chosen when the felt latency of the correction loop was the constraint. It is
not a fit; it is a budget, and measured against the fit it is a bad one.

8 outputs, 20 examples, nh=12, mean of 9 seeds, the same code throughout:

| epochs | train MSE | recall | grid RMSE | host ms |
|---|---|---|---|---|
| 600 | 7.87e-4 | 0.0112 | 0.0158 | 1.5 |
| 6 000 | 2.58e-5 | 0.0032 | 0.0094 | 14.5 |
| 20 000 | 8.87e-6 | 0.0019 | 0.0086 | 50.0 |
| 60 000 | 3.23e-6 | 0.0012 | 0.0083 | 150.0 |
| 200 000 | 2.19e-6 | 0.0009 | 0.0083 | 501.1 |

**5.9× on recall of the musician's own demonstrations and 1.8× on held-out
error, for one integer and zero bytes.** The standing direction is explicit
that training time is the cheap thing.

## Decision

Add `iris_train_converge(k, ceiling, cb, user)`: run until the training error
plateaus — every `IRIS_CONV_WINDOW` (2,000) epochs, compare against the error one
window ago and stop when the window bought less than `IRIS_CONV_TOL` (10%) of it
— with a hard `IRIS_CONV_CEILING` of 60,000. It is now the masthead
recommendation.

`iris_train_epochs` is **unchanged, byte for byte**, and stays that way: it is
the Wekinator fidelity path, because fixed-epoch backprop is exactly what
Weka's `MultilayerPerceptron` does, and audit check 12 hashes what it produces.

Add `iris_train_begin` / `iris_train_slice` / `iris_train_progress` so a
single-threaded UI can run the same training in slices and draw a real bar.
The shuffle buffer is initialised once at `iris_train_begin` and carried across
slices, so **a sliced run is bit-identical to the unsliced one** — asserted by
check 31 with a `memcmp`. This closes, for the converged trainer, the trap E6
recorded against `iris_train_epochs`, where the chunk schedule was part of the
instrument's identity.

## The rejected alternative: just raise the constant

Rejected on a measurement, not on taste. **At 50 examples a fixed 200,000
epochs is worse on held-out grid error than 60,000 — 0.0065 against 0.0063.**
There is a point past which more convergence costs generalisation, it moves
with the example count, and no constant finds it. The plateau test stops before
it without being told where it is.

A window of 200–500 epochs was also rejected, and by the same method: it
mistakes the ordinary epoch-to-epoch noise of a shuffled SGD trace for a
plateau and stops at a quarter of the achievable fit (10 examples, window 200,
tolerance 2%: 3,577 epochs and train MSE 4.9e-4 against 1.07e-5 at 20,000).
The sweep covered 10 window/tolerance pairs at 10/20/50/100 examples.

## What it costs

Three `int32_t` and one `float` in `struct iris`; no arena growth beyond that.
Time: 20 examples → ~16,000 epochs, 27 ms host, ~0.9 s S3; 50 examples →
12,000 epochs, 47 ms host, ~1.5 s S3 (audit's 2-12-3 reference). At the
8-output D11 shape, ~1.1 s and ~1.9 s.

## The caveat, which is not decoration

The truth function every number above comes from is smooth and noiseless.
"More convergence never hurts" is precisely the conclusion most at risk from
real sensor noise and human inconsistency. None of this is verified on
hardware or on recorded human gesture. Re-run the table before treating the
ceiling as settled.
