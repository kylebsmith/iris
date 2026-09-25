# 0017 — Train to the plateau, not to a constant

**Status:** accepted, 2026-08-22. Governs `iris.h` 0.2.0: `iris_train` is this
decision. Whether the plateau is the right default on noisy demonstrations is
contested (iris-studies S08; see "The caveat" below).
**Affects:** `iris.h` PART 8 (`iris_train`, `iris_continue`,
`iris_continue_to_plateau`, `iris_train_begin`, `iris_train_slice`,
`iris_train_progress`, `IRIS_CONV_WINDOW`, `IRIS_CONV_TOL`,
`IRIS_CONV_CEILING`), `tests/audit.c` ("trains to convergence; sliced ==
unsliced; progress rises to 1" and the training-cost table), `tests/train.c`.

## Context

A fixed budget of 600 epochs was the recommended way to train. That number was
chosen when the felt latency of the correction loop was the constraint. It is
not a fit; it is a budget, and measured against the fit it is a bad one.

8 outputs, 20 demonstrations, 12 hidden units, mean of 9 seeds, the same code
throughout (recorded on the development laptop, an Apple M4 Max; program in
iris-studies S11):

| epochs | training mean squared error | recall | grid root-mean-square error | host ms |
|---|---|---|---|---|
| 600 | 7.87e-4 | 0.0112 | 0.0158 | 1.5 |
| 6 000 | 2.58e-5 | 0.0032 | 0.0094 | 14.5 |
| 20 000 | 8.87e-6 | 0.0019 | 0.0086 | 50.0 |
| 60 000 | 3.23e-6 | 0.0012 | 0.0083 | 150.0 |
| 200 000 | 2.19e-6 | 0.0009 | 0.0083 | 501.1 |

**5.9 times better recall of the musician's own demonstrations and 1.8 times
better held-out error, for one integer and no bytes.** Training time is the
cheap thing; the fit is not.

## Decision

`iris_train` runs until the training error plateaus: every `IRIS_CONV_WINDOW`
(2,000) epochs it compares the error with the error one window earlier, and
stops when the window bought less than `IRIS_CONV_TOL` (10%) of it, under a
ceiling of `IRIS_CONV_CEILING` (60,000 epochs; 30,000 where `int` is 16 bits).
An absolute floor of 1e-6 on the mean squared error also ends a run. It starts
from the instrument's own seed every time, and it is the call the header
recommends.

`iris_continue(k, epochs)` is the fixed-epoch update of Weka's
`MultilayerPerceptron`, the network Wekinator runs, and the golden hash in
`tests/audit.c` pins what it produces. It is kept for that fidelity, not as
the recommended trainer.

`iris_train_begin`, `iris_train_slice` and `iris_train_progress` let a
single-threaded interface run the same training in slices and draw a real
progress bar. The shuffle order is set up once at `iris_train_begin` and
carried across slices, so **a sliced run is bit-identical to the unsliced
one**; `tests/audit.c` and `tests/train.c` compare them byte for byte. The way
the run is cut into slices is therefore not part of the instrument's identity.

## The rejected alternative: just raise the constant

Rejected on a measurement, not on taste. **At 50 demonstrations a fixed
200,000 epochs is worse on held-out grid error than 60,000: 0.0065 against
0.0063** (recorded; iris-studies S11). There is a point past which more
training costs generalisation, it moves with the number of demonstrations, and
no constant finds it. The plateau test stops before it without being told
where it is.

A window of 200 to 500 epochs was also rejected, by the same method: it
mistakes the ordinary epoch-to-epoch noise of a shuffled stochastic gradient
descent trace for a plateau and stops at a quarter of the achievable fit (10
demonstrations, window 200, tolerance 2%: 3,577 epochs and training error
4.9e-4, against 1.07e-5 at 20,000; recorded, iris-studies S11). The sweep
covered 10 pairs of window and tolerance at 10, 20, 50 and 100
demonstrations.

## What it costs

A few counters in `struct iris`; no arena growth. Time, on the reference task
of `tests/audit.c` (2 inputs, 12 hidden units, 3 outputs): 18,000 epochs at 20
demonstrations and 12,000 at 50. On the development laptop that is tens of
milliseconds (`sh build.sh audit`, training-cost table). On the board it is
13.4 s at 20 demonstrations and 22.1 s at 50, and a slice of 64 epochs at 20
demonstrations takes 48 ms (ES3C28P, ESP32-S3 at 240 MHz,
[log](../board/2026-09-25-es3c28p.txt)).

## The caveat

Every number above comes from a smooth, noiseless target. "More training never
hurts" is the conclusion most at risk from sensor noise and human
inconsistency, and on noisy synthetic demonstrations it does not hold: at
output noise of standard deviation 0.02 or more, a fixed budget of 100 epochs
beat the plateau default on held-out error by 1.2 to 3.7 times in 14 to 16 of
16 trials (recorded; iris-studies S08). Smoothing (`iris_set_smoothing`)
repairs part of that at a cost on clean data. Nothing here is verified on
recorded human gesture, and the default waits for that study.

Measurements: iris-studies S11, S08.
