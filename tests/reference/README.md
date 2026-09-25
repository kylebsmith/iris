# The binary64 reference

Every other test in this repository was written by the library's author and
checks the library against that author's expectations. This directory checks it
against something it did not write. `reference.py` is the model of iris written
a second time, in NumPy, from the description of the model (the masthead and the
part comments of `iris.h`, and a written specification of the model) and not
from `iris.h`'s code. `export.c` runs a recipe through iris and prints every
number iris computed. `run.py` replays the recipe in the reference and compares.

The reference computes in binary64, the 64-bit floating-point format with 53
significant bits. iris computes in binary32, with 24. The two therefore agree
only up to binary32's rounding, and every tolerance below is derived from that
rounding, calibrated by measuring it, or set and then checked against both. Throughout, u = 2⁻²⁴ is binary32's
unit roundoff: a correctly rounded binary32 operation is wrong by at most u
relative to its exact result.

## Running it

```sh
python3 -m pip install -r tests/reference/requirements.txt
python3 tests/reference/run.py            # every check, then the task table: about 5 minutes
python3 tests/reference/run.py --quick    # every check, no task table: about 70 seconds
```

It runs from any directory and finds `iris.h` from its own location. It builds
`export.c` with `$CC` (default `cc`) at `-std=c99 -O2 -Wall -Wextra -Werror`
into a temporary directory, or into `--workdir DIR`, where each recipe's output
then stays to be read. That output is JSON (JavaScript Object Notation, a
plain-text data format). `--only NAME` runs the recipes whose names contain
NAME; `--seeds` and `--demos` size the task table; `--cc` names another
compiler.

It prints one line per check, PASS or FAIL, with the number the verdict rests
on. The exit status is 1 if any check failed and 2 if the export could not be
built or run. The task table is a report and never fails the run. It writes
nothing into the repository. The times above were measured on an Apple M4 Max.

## The files

- `export.c` builds an instrument from a recipe read on standard input, every
  number in C99 hexadecimal form so it arrives as exactly the binary32 value
  the generator meant. It uses the public interface to do everything and reads
  the instrument's structure, without writing it, for what the interface does
  not report: weights, momentum velocities, fitted ranges, and the order each
  epoch's shuffle produced. It trains with `iris_train_begin` and one-epoch
  `iris_train_slice` calls, printing the state after each; then makes the warm
  calls, `iris_continue(k, 1)` each; then plays the probes through
  `iris_predict`, `iris_knn_predict` and `iris_classify_1nn`. It also runs the
  recipe through the blocking call on a second instrument and reports whether
  the two saved files are byte-identical, and prints the instrument hash that
  `tests/audit.c` pins for the golden recipe.
- `reference.py` is the model: the facts iris states exactly (the random-number
  generator, the starting weights it draws, the shuffle, the fitted ranges),
  recomputed in binary32; the network, its per-demonstration update, the
  stopping rule and the neighbour samplers in binary64; and the rounding bounds.
- `run.py` holds the recipes, runs the export, makes every comparison and prints
  the task table.
- `requirements.txt` pins numpy, scikit-learn and scikit-learn's own
  dependencies at the versions every number here was measured with, on Python
  3.12.

## The recipes

| Recipe | Shape | Demonstrations | Seed | Smoothing | Run | For |
|---|---|---|---|---|---|---|
| golden | 2-12-3 | the 20 of `tests/audit.c`'s golden check | 1234 | 0 | 800 epochs, then 100 warm | the instrument the hash `0x6805FB0D` pins |
| golden-plateau | 2-12-3 | the same | 1234 | 0 | `iris_train`: 22,000 epochs, plateau; 50 warm | a long run and its plateau stop |
| noisy | 2-12-3 | 30 random points of the same function, noise 0.05 | 77 | 0 | 12,000, plateau; 50 warm | noisy data |
| noisy-smoothing | 2-12-3 | the same | 77 | 0.5 | 4,000, plateau; 50 warm | weight decay |
| plateau-edge | 2-12-3 | 20 random points, noise 0.03 | 6 | 0 | 10,000, plateau | two plateau tests just above the tolerance |
| sensors | 3-16-3 | 25: millimetres, g, and an input held at 500; hertz, 0 to 127, and an output held at 64 | 4242 | 0.15 | 4,000, plateau; 50 warm | raw units, a still input, the output floor |
| grid | 2-12-1 | 9 on a 3 × 3 grid | 9 | 0 | 6,040, error floor; 50 warm | the error-floor stop, and exact ties for the neighbours |
| classes | 5-12-4 | 40: millimetres, g, an input held at 3.3, and two inputs either side of the still-input threshold; a class label 0 to 3, two continuous outputs, and one held at 440 | – | – | not trained | the neighbour samplers on an instrument that fits its own ranges; the still-input threshold |

A shape is inputs-hidden-outputs. The golden recipe's function is
`tests/audit.c`'s `truth()`, of two inputs in [0, 1]; the noisy recipes sample
it at uniform random points and add Gaussian noise to every output. Every
trained recipe plays 1,681 probes spread over the demonstrated input box widened
by a quarter of its width on every side (a 41 × 41 grid for two inputs, random
points for three), so about half the probes are readings beyond anything
demonstrated, where the hidden units saturate and the output clamp acts. The
grid recipe plays 25 more, on the quarter-step lattice over its takes: the takes
themselves, and points midway between two or four of them, where the nearest
take is an exact tie in binary32 too. The classes recipe plays 2,025.

## What it checks

**Exact facts, bit for bit.** For every recipe: the demonstrations arrive
intact; the fitted ranges, still-input rule and output floor included, equal
the documented rule evaluated in binary32 (the classes recipe straddles the
still-input threshold, 1e-5 of the magnitude: an input drifting across 0.01
around 500, twice the threshold, must move, and one drifting across 0.001
around 200, half of it, must be still); the learning rate, momentum and
decay iris holds are the documented 0.10, 0.85 and 0.3 × smoothing; the
starting weights, and the generator's state after drawing them, follow from the
seed; the shuffle order of every epoch, warm ones included, re-derives from the
seed; one-epoch slices leave the same saved file as the blocking call
(`iris_train`, or `iris_reseed` then `iris_continue_to_plateau` when the recipe
sets a ceiling); and the golden recipe's instrument hash is `0x6805FB0D`, which
ties the export to the instrument `tests/audit.c` pins.

**(a) Training.**

1. *Every epoch, replayed from iris's own state.* The reference starts from
   iris's weights and velocities after epoch e − 1, runs epoch e in binary64
   with iris's order, and compares every weight and velocity with iris's after
   epoch e. This is the check that iris computes the documented update. The
   same replay's mean squared error for the epoch is compared with the error
   iris reports for it: the number `iris_continue` returns and the error floor
   and the plateau test read. Run for all epochs at once, it covers every
   epoch of every run: 22,050 of them in 0.2 s for golden-plateau.
2. *The whole run, free-running.* From iris's starting weights the reference
   trains alone, in binary64, for as many epochs as iris ran, with iris's
   shuffle orders, and every epoch's weights are compared. Every 1,000 epochs
   the two sets of weights also play the probes, and their outputs are
   compared. A second binary64 run, started from weights nudged by one binary32
   step, measures how strongly the training dynamics amplify a small difference,
   and the line reports that growth.
3. *iris stops where its rule says.* The documented stopping rule is the
   divergence guard (a weight past ±16), the error floor (an epoch's mean
   squared error below 1e-6), the plateau test (every 2,000 epochs, stop when
   the window bought 10% or less of the error one window earlier) and the
   ceiling. Applied to iris's own exported per-epoch errors, with the plateau
   comparison evaluated in binary32 as iris evaluates it, the rule must end the
   run at exactly the epoch iris ended it.
4. *The reference stops at the same epoch.* The same rule on the reference's
   binary64 errors. A different epoch fails unless the reference's margin to
   the threshold there is within 4 times the relative difference between the
   two runs' errors, which makes it a decision rounding can flip.

The warm `iris_continue(k, 1)` epochs after each run go through checks 1 and 2
like any other epoch.

**(b) The forward pass.** The reference's binary64 forward pass on iris's final
weights, against `iris_predict` on every probe, each output within its own
rounding bound.

**(c) The neighbour samplers.**

- *On tie-free queries, against scikit-learn.* `KNeighborsRegressor` with
  `n_neighbors=k`, `algorithm="brute"` and `weights=lambda d: 1 / (d*d + 1e-9)`,
  on each input's distance from the low end of its range times 1 / width (0 for
  a still input), for k = 1, 3, 5 and
  8, within 1e-6 of each output's largest demonstrated magnitude. iris weights a
  neighbour 1 / (d² + 1e-9); scikit-learn's named options are uniform and
  1 / d, but a weight function handed to it computes exactly iris's weight, so
  the comparison is with scikit-learn itself. reference.py's own blend is
  printed beside it and agrees with scikit-learn to within 1.4e-11. The nearest
  demonstration iris names is scikit-learn's nearest, its outputs are the
  stored demonstration's bits, and on the classes recipe
  `KNeighborsClassifier(n_neighbors=1)` gives the same labels. A query is
  tie-free when its k-th and (k + 1)-th nearest distances differ by more than
  1e-4 relative; scikit-learn breaks ties its own way. The inputs go to
  scikit-learn as fractions of their range counted from the low end, as iris
  counts them: its brute-force distance expands |a − b|² as
  |a|² − 2a·b + |b|², which loses digits when coordinates sit far from zero,
  and an input drifting across 0.01 around 500, merely scaled, sits near
  51,000 (the blends then came out 2e-3 from reference.py's own).
- *On every query, ties included, against the documented rule.* iris breaks a
  tie in favour of the earliest-recorded demonstration, on the distances it
  computes, which are binary32. Two demonstrations at the same exact distance
  can be a rounding apart in binary32, so the reference evaluates the
  documented distance (each input's difference times the reciprocal of its
  width, squared, summed input by input) in binary32 to choose the neighbours,
  and blends them in binary64. Every choice then matches, and every blend is
  within 1e-6, on every query: on the grid recipe, 174 of the 1,706 queries
  have a binary32 tie at a neighbour boundary.

**(d) The task table**, a report: below.

## What it does not check

- The closed-form trainer (`iris_train_elm`), `iris_loo_error`,
  `iris_suggest_smoothing`, `iris_novelty`, the worst-example ledger, and saving
  and loading. None is modelled here.
- The guards' recovery. No recipe diverges or meets a not-a-number, so the
  reference's divergence clamp runs and never fires, and the reseed that follows
  a not-a-number is not modelled.
- A proof of the whole run. No a-priori bound exists for thousands of epochs of
  a non-convex trajectory, because the dynamics amplify a difference (see
  Tolerances), so check (a)2 is calibrated. A longer noisy run can pass its
  ceiling for that reason alone; the nudge figure on the line tells that case
  from a defect, and check (a)1 still bounds every single epoch.
- Other machines. Everything here was measured on macOS on an Apple M4 Max with
  Apple clang 17 and numpy 2.2.4. iris's side is bit-for-bit reproducible by
  its own contract; the reference's binary64 matrix products can round
  differently in their last bit on another machine, about 1e-16 relative, nine
  orders of magnitude below anything compared.
- Mistakes the description shares with the code. The reference follows the
  description, so where the description and the code agree on something wrong,
  both pass. The places the description left open are listed next, with how
  each was settled.
- Large shapes: the recipes have 2 to 5 inputs, 12 to 16 hidden units and 1 to 4
  outputs.
- A change the size of one rounding, made once. Every tolerance above is a
  few binary32 roundings wide, so a constant moved by one unit in the last
  place passes all of them. Measured on scratch copies of `iris.h`: the
  learning rate or the momentum one unit up fails the exact constants check
  and the golden hash and nothing else; the output band's 0.9 one unit up, or
  every hidden activation one unit up, fails the golden hash alone. A
  one-unit error made at every weight update does accumulate, and fails
  checks (a)1 and (a)2 (the table at the end).

The golden recipe's demonstrations are computed in `run.py` with the header's
rational function evaluated in binary32. That is how the data is made, not part
of the model, and the matching hash confirms the data is tests/audit.c's.

## What the description left open

Each is settled by an experiment the harness runs against the library, not by
the library's code.

1. **The starting weights' scale.** "Dividing by the square root of the number
   of inputs" has two binary32 evaluations, draw / √n and draw × (1 / √n),
   which round differently. The library multiplies by the rounded reciprocal:
   that form matches every starting weight of every recipe (75 of 75 at 2-12-3,
   115 of 115 at 3-16-3, 49 of 49 at 2-12-1), and the quotient form matches only
   48 to 50 of 75, 85 of 115 and 33 of 49. The exact check prints both counts.
2. **The shuffle.** "Fisher-Yates" leaves the direction open, and no one place
   says when the order starts again. The reference takes the usual descending
   form (for each position i from the last down to 1, swap it with position
   draw mod (i + 1)), the order carrying from one epoch to the next within a
   training session and starting from the identity at the start of each
   session. Every blocking call is a session of its own, so every
   `iris_continue(k, 1)` starts from the identity. Every epoch of every run
   re-derives this way.
3. **The output floor.** The header says an output that never moved keeps a
   small width, relative to its value, without giving the formula. The
   reference takes max(1e-5 × |lo|, 1e-6), the input rule's thresholds applied
   to the output, and the two recipes with a constant output (64 and 440)
   confirm it bit for bit. An output that moved by less than the floor is not
   exercised.
4. **Which value of each constant.** The description gives decimals (0.10, 0.85,
   0.1, 0.9, 0.10) and iris holds their binary32 values; the reference takes the
   binary32 values, because those are the numbers the model is defined by. It
   changes little: replaying golden-plateau with the decimals moves the measured
   difference by under 4% (1.19e-7 against 1.15e-7 after one epoch, 1.309e-5
   against 1.302e-5 at epoch 22,000).
5. **Decisions near a threshold.** The plateau test and the neighbour choice are
   comparisons, and binary64 arithmetic can decide a knife-edge case differently
   from binary32. The checks that ask whether iris obeys its own rule therefore
   evaluate the comparison in binary32: the plateau test on iris's errors, and
   the neighbour distances in the tie check.

## Tolerances

**Exact facts:** none.

**One epoch from iris's state (derived).** Every weight and velocity within
(2 + d) × n × u × max(|w|, 1), where n is the number of demonstrations, w the
weight and d is 1 when weight decay is on and 0 otherwise. Within an epoch each
weight is visited n times. Each visit rounds it once when its velocity is added,
an error of at most u × |w|, and once more when decay is on, when w − wd × w
rounds again. The velocity's own roundings and the gradient's are proportional
to |v| and to the learning rate times the error signal times the input, small
beside |w| once training is under way; the floor of 1 on |w| covers small
weights, and the factor 2 covers those terms and their spread through the rest
of the epoch. A velocity is held to its weight's bound. Measured: at worst 0.372
of the bound (grid), between 0.36u and 0.75u per visit, because roundings
partly cancel.

**The epoch's error from the same replay (derived form, calibrated
constant).** Within 8u(√err + n × m × err), with m the number of outputs.
Each error y − t is a few u from its binary64 value, through the forward
pass and the target scaling, and that moves the mean of the squares by at
most twice as much times √err (the mean of |y − t| is at most √err); adding
n × m squares in binary32 and dividing adds up to (n × m + 1)u relative. The
constant is measured: at worst 1.39 (grid, where the error sits near the
floor), so 8 leaves 5.8 times.

**The whole run (calibrated).** With d(e) the largest weight difference after e
epochs relative to max(1, largest weight),

    d(e) <= min(c × e × P × u, H),   c = 2e-3,  H = 1e-3,

where P is the number of roundings in one epoch: n times the count per
demonstration visit that `roundings_per_visit` makes stage by stage (839 for
2-12-3, so 16,780 an epoch at 20 demonstrations). The linear term is the case
in which every rounding pushes the same way. Measured, the difference grows far
more slowly, roughly as the square root of the epoch count (golden-plateau: 1.9u
after one epoch, 26u after 100, 61u after 1,000, 218u after 22,050), so the
ratio d / (e × P × u) is largest at epoch 1: 3.06e-4 at most (grid), which c
exceeds 6.5 times. The largest d anywhere is 1.8e-4 (noisy, epoch 12,050),
which H exceeds 5.6 times. Predictions of the two sets of weights at the
checkpoints must agree within 2e-3 of each output's range, a quarter of one step
of a 7-bit controller value (1/128); the largest difference is 5.7e-4 (noisy),
3.5 times inside.

Why the ceiling is measured rather than derived: on the noisy recipe the
difference doubles about every 1,000 epochs from epoch 9,000 (1.96e-5, 4.13e-5,
8.79e-5 and 1.74e-4 at epochs 9,000 to 12,000). A binary32 replay in NumPy
drifts from binary64 at the same rate over the same stretch, and a binary64 run
started one binary32 step from the reference grows by the same factor
(6.1e-10 at epoch 10,000 to 2.75e-9 at 12,050, 4.5 times, against 4.4 times for
iris). The growth belongs to the training dynamics, which amplify any small
difference, and every one of those epochs passes check (a)1. So the free-running
check catches what one epoch cannot show, a systematic difference too small to
see per epoch and the stopping decisions, and its ceiling is set from
measurement.

**Stopping decisions.** Exact on iris's own errors. The reference's decision may
differ only when its margin to the threshold is within 4 times the relative
difference between the two runs' errors at that epoch plus the previous window's.
Measured: the reference stops at iris's epoch on every recipe. The largest
relative difference between the two runs' epoch errors is 9.7e-5.

**Forward pass (derived, first order).** For each probe and output, stage by
stage from the playing chain:

| Stage | Bound on the difference |
|---|---|
| input, x = 2(v − lo)/(hi − lo) − 1 | 4u(\|x\| + 1) |
| hidden sum, s = b1 + Σ w1·x | γ(n_in + 1)(\|b1\| + Σ\|w1\|\|x\|) + Σ\|w1\|·dx |
| rational, a = p(s): six roundings, slope at most 1 | ds + 7u |
| output sum, z = b2 + Σ w2·a | γ(n_hid + 1)(\|b2\| + Σ\|w2\|\|a\|) + Σ\|w2\|·da |
| squash, 0.5(p(z/2) + 1): slope at most 1/4 | dz/4 + 4.5u |
| to the range, t = (y − 0.1)/0.8, out = lo + t(hi − lo) | width(dy/0.8 + 5u·max(\|t\|, 1)) + 2u·max(\|lo\|, \|hi\|) |
| clamp to [lo, hi] | cannot increase it |

γ(n) = nu / (1 − nu) is the bound on a sum of n rounded products taken in
order (Higham, *Accuracy and Stability of Numerical Algorithms*, section 3.1).
Measured: at worst 0.082 of the bound (noisy-smoothing); the largest difference
is 3.9e-7 on outputs of order 1 (noisy) and 2.5e-4 on the sensors recipe's
pitch in hertz, 42u relative.

**Neighbours.** 1e-6 of each output's largest demonstrated magnitude. The first
order count for iris's blend: a distance is good to (n_in + 6)u relative, a
weight to (n_in + 8)u, a weight's share of the total to (2 n_in + k + 16)u, so
the blend to about (2 n_in + 2k + 18)u times the spread of the neighbours'
values when every rounding aligns: at five inputs and eight neighbours that is
44u, 2.6e-6 of the spread, above 1e-6.
Measured, the worst is 1.14e-7 (classes, eight neighbours), nearly nine times
inside 1e-6, so the tolerance holds in practice without being an a-priori bound
for the largest k.

## The task table

Held-out error of `iris_train` against scikit-learn's multilayer perceptron
(`MLPRegressor`) on the golden recipe's function. Both arms of scikit-learn use
`hidden_layer_sizes=(12,)` and `activation="tanh"`:

- SGD, stochastic gradient descent one demonstration at a time as iris does:
  `solver="sgd"`, `batch_size=1`, `learning_rate="constant"`,
  `learning_rate_init=0.1`, `momentum=0.85`, `nesterovs_momentum=False`,
  `shuffle=True`, run for exactly as many epochs as `iris_train` ran on the same
  data (`max_iter` set to it, `tol=0` and `n_iter_no_change` above it, so
  nothing stops it early);
- L-BFGS, the limited-memory Broyden-Fletcher-Goldfarb-Shanno quasi-Newton
  method, which fits the same model class close to its optimum:
  `solver="lbfgs"`, `max_iter=5000`, `tol=1e-10`.

scikit-learn sees what iris's network sees: inputs scaled to [−1, +1] and
targets to [0.1, 0.9] across the training ranges, its predictions mapped back
and held inside the demonstrated range. Alpha comes from smoothing. iris shrinks
each weight by l2 × lr / n per visit, outside the momentum (l2 = 0.3 ×
smoothing, lr the learning rate); scikit-learn adds alpha × w to the gradient,
where momentum multiplies a steady push by 1 / (1 − momentum), so the same
shrink needs alpha = l2 × (1 − momentum) / n: 0.0011 at smoothing 0.5 and 20
demonstrations. What still differs: scikit-learn's output is linear where iris's
is the rational logistic, its starting weights are Glorot's rather than a
uniform draw over the square root of the fan-in, and its backward pass is the
true gradient where iris's is the surrogate the header describes. The table
therefore compares results on a task, not algorithms; checks (a) to (c) compare
algorithms.

Each row is 8 seeds: 20 demonstrations at uniform random points, iris seeded
with the seed and scikit-learn with the same number, and the error is the
root-mean-square difference from the clean function on a 21 × 21 grid of
[0, 1]². Ratios are geometric means of iris's error over scikit-learn's with
95% bootstrap intervals; below 1 favours iris. "Reroll" is the standard
deviation of the logarithm of the ratio between iris at two seeds on the same
data, the scale on which a difference means something.

| Noise | Smoothing | Alpha | iris | SGD | L-BFGS | iris / SGD | iris / L-BFGS | Reroll | Epochs |
|---|---|---|---|---|---|---|---|---|---|
| 0 | 0 | 0 | 0.0145 | 0.0190 | 0.0157 | 0.76 [0.60, 0.94] | 0.92 [0.84, 1.01] | 0.062 | 17,414 |
| 0.05 | 0 | 0 | 0.0938 | 0.0573 | 0.1393 | 1.64 [1.39, 1.96] | 0.67 [0.52, 0.88] | 0.181 | 13,000 |
| 0.05 | 0.5 | 0.0011 | 0.0381 | 0.0513 | 0.0556 | 0.74 [0.67, 0.82] | 0.68 [0.55, 0.83] | 0.038 | 4,000 |

On clean data iris's held-out error is three quarters of scikit-learn's SGD at
the same epoch count, and not distinguishable from L-BFGS's (the interval
includes 1). On noisy data without smoothing iris fits more of the noise than
the SGD arm does, and less than L-BFGS does. With smoothing 0.5 it has the
lowest error of the three, and stops at 4,000 epochs against 13,000 without
smoothing.

The band is both intervals inside [0.5, 2]: iris within a factor of 2, either
way, of a standard implementation of the same model class. A factor of 2 is
about what iris's one quality knob, smoothing, is worth at realistic noise
(2.4 times, in the table above `iris_set_smoothing`), so an iris outside the band
would be further from a standard fit than its own knob moves it. Every row above
is inside. A row outside prints OUTSIDE and fails nothing: a ratio of held-out
errors depends on the task, the noise and the number of demonstrations, and 8
seeds on one function calibrate; they do not judge.

## Each check fails when it should

A scratch copy of `iris.h` with one behaviour broken, run through the unchanged
harness:

| Broken in the copy | Recipe | What fails, and by how much |
|---|---|---|
| momentum 0.85 → 0.80 | golden | the constants (iris holds 0.800000012); every epoch replayed, at 16,200 times its bound, and its error at 1,660 times; the whole run; the golden hash |
| hidden error signal × 0.99 | golden | every epoch replayed, 407 times its bound, and its error 15.4 times; the whole run, 553 times; the golden hash |
| weight decay l2 × lr → l2 / lr | noisy-smoothing | every epoch replayed, 107,000 times its bound, and its error 8,460 times; the whole run, predictions 0.61 of the range apart |
| weight decay applied before the velocity is added instead of after | noisy-smoothing | every epoch replayed, 23.6 times its bound, and its error 4.52 times; the whole run, 42.2 times |
| shuffle draw mod (i + 1) → draw mod i | golden | the shuffle order: 0 of 900 epochs re-derive; the golden hash |
| output error signal y(1 − y) → y(1.001 − y) | golden | every epoch replayed, 170 times its bound, and its error 12.6 times; the whole run, 203 times; the golden hash |
| epoch error divided by the demonstration count alone, not by demonstrations times outputs | every recipe | every epoch's error, 44,300 to 65,300 times its bound, on the six recipes with three outputs; nothing else |
| epoch error divided by one more than demonstrations times outputs | every recipe | every epoch's error, 243 to 15,700 times its bound, on all seven trained recipes; nothing else |
| plateau tolerance 0.10 → 0.12 | plateau-edge | iris stops at epoch 6,000, where its documented rule does not; the reference's margin there is 0.015, against 4 × 1e-6 |
| error floor 1e-6 → 2e-6 | grid | iris stops at epoch 5,213, where its documented rule does not; margin 0.99 |
| neighbour guard 1e-9 → 1e-6 | classes | the 3-, 5- and 8-nearest blends, 34 to 50 times the tolerance; the tie check |
| neighbour distance squared → absolute | classes | every neighbour check; blends up to 1.11 of an output's largest magnitude apart |
| input scaling 2t − 1 → 2t − 0.99 | golden | every epoch replayed, 3,470 times its bound, and its error 2,070 times; the forward pass, 3,310 times; the whole run; the golden hash |
| output clamp removed from `iris_predict` | golden | the forward pass, 25,800 times its bound (0.072 outside the range) |
| `iris_continue` zeroes the velocities first | golden | every epoch replayed (the warm ones), 1,150 times its bound, and its error 1,340 times; the whole run |
| rational 27 + 9s² → 27 + 9.001s² | golden | the forward pass, 9.45 times its bound; every epoch replayed, 4.18 times, and its error 3.26 times; the whole run; the golden hash |
| first-layer starting weights × 1.0001 | golden | the starting weights: 51 of 75 match; the golden hash |
| velocity flush below 1e-30 → below 1e-6 | golden-plateau | every epoch replayed, 11 times its bound; the whole run, 1.94 times |
| every weight update rounded up by one unit in the last place | golden | every epoch replayed, 1.13 times its bound (1.23 on golden-plateau); the whole run on golden-plateau, 30.8 times; the golden hash |
| still-input threshold 1e-5 → 1e-4 | classes | the fitted ranges (3 still inputs where there are 2) |
| still-input threshold 1e-5 → 1e-6 | classes | the fitted ranges (1 still input where there are 2) |
| k-nearest ties go to the latest recorded | grid | the tie check: blends 0.5 of the output's magnitude apart |
| the nearest's ties go to the latest recorded | grid | the tie check: 1,690 of 1,706 nearest demonstrations are the earliest recorded |

The golden hash, where it fires, says only that something differs; the other
lines say which part of the model differs, at which epoch, and by how much.

## Changing it

- `reference.py` is written from the description, never from `iris.h`'s code.
  A commit that changes `iris.h`'s arithmetic and `reference.py` together says
  why in its message, because changing both at once defeats the comparison.
- A tolerance changes only with the measurement that justifies it, written
  beside the constant in `run.py` and here.
- A newly found ambiguity in the description is settled by an experiment the
  harness runs against the library, and listed above.
