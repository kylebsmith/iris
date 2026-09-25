# iris: a single-header C99 library for interactive supervised learning on embedded hardware

**Version 0.2.0.** BSD 3-Clause, `Copyright (c) 2026, Kyle Smith`. One header,
no build system: `cc -std=c99 -O2 -I. -o hello examples/01_hello.c`. This page
summarises the design for a technical reader; [`iris.h`](../iris.h) is the
authority. A figure below names the check or recipe that produces it, or says
that it comes from a study whose program is not in this repository.
The plain-language companion is [SYSTEM-plain-english.md](SYSTEM-plain-english.md).

## 1. System

iris is a supervised-learning runtime for the Wekinator interaction loop
(demonstrate gesture-to-parameter pairs, train, play) sized for a
microcontroller. `struct iris` is 248 bytes on a 64-bit host and 168 on the
ESP32-S3; everything else lives in a caller-supplied arena whose size a macro
computes at compile time. `IRIS_ARENA(2,12,3,256)` is 9,264 bytes on a 64-bit
host and equals `iris_size()` exactly (`tests/audit.c`, "arena macro covers
every array iris_init carves"). No allocator is ever called: `sh build.sh noheap` interposes the
allocator family, drives every entry point, and requires zero calls, with a
positive control that must be counted. Hard limits: 32 inputs, 16 outputs, 64
hidden units (8 minimum), and 4,096 demonstrations (255 where `size_t` is 16
bits), all overridable downwards before the `#include`.

The public interface is 41 functions, three types (`iris`, `iris_status`,
`iris_progress_fn`) and a handful of macros, listed at the top of the header.
Every other function is named `iris_internal_*` and is outside the version
promise.

Determinism is enforced in four layers:

1. `#error` tripwires for the flags that change the arithmetic or delete the
   not-a-number guards: `__FAST_MATH__`, `__FINITE_MATH_ONLY__`, and on GCC
   (the GNU Compiler Collection) `__RECIPROCAL_MATH__` and
   `__ASSOCIATIVE_MATH__`.
2. `#error` when `__FLT_EVAL_METHOD__` is anything but 0, 16 or 32, which
   refuses 32-bit x86 builds using the x87 floating-point unit. Built that way
   (clang 22, `--target=i686-linux-gnu -mno-sse -mfpmath=387`) the golden
   recipe hashes to `0x2B53B02B` instead of `0x6805FB0D`, with no diagnostic.
   `tests/targets.sh` checks that the guard fires there and nowhere else.
3. Contraction of `a*b + c` into a fused multiply-add is forbidden in the
   header's own code and only there: `#pragma STDC FP_CONTRACT OFF` inside
   `#pragma float_control(push)` / `(pop)` on clang, and
   `#pragma GCC optimize ("fp-contract=off")` inside `push_options` /
   `pop_options` on GCC. Clang ignores the standard pragma under
   `-ffp-contract=fast`, so such a build must also pass `-ffp-contract=off`;
   GCC honours its pragma even under `-ffp-contract=fast`.
   `tests/pragma_leak.sh` checks the generated assembly: user code after the
   `#include` still fuses, no iris function does.
4. Golden hashes (FNV-1a, the Fowler-Noll-Vo byte hash): `0x6805FB0D` for the
   fixed-epoch recipe in `tests/audit.c`; `0xB7FC47A0` and `0x203834ED` for the
   starter kit's two recipes in `tests/starter_recipes.c`; the closed-form
   solves in `tests/elm.c`; and a committed format 7 file whose 625 predictions
   `tests/load.c` compares bit for bit.

`sh build.sh determinism` checks the golden hash across `-O0` to `-Os` with
contraction off, on and at the default, and with gcc in GNU C (`-std=gnu99`).
Freestanding builds of the golden and starter recipes give the same three
hashes on 64-bit ARM Linux, x86-64 Linux and 32-bit x86 using SSE (Streaming
SIMD Extensions, SIMD meaning single instruction, multiple data: its
single-precision vector unit). On the ESP32-S3 the starter kit's
`device_torture` test 1 compares against `0xB7FC47A0`; no board run of it is
recorded yet.

The header includes `<stddef.h>` and `<stdint.h>` only. With `-ffreestanding
-fno-stack-protector` (and `-fno-tree-loop-distribute-patterns` on GCC) a unit
calling every function has zero undefined symbols and links with `-nostdlib
-static`, as C and C++, on Apple clang, clang 22 and gcc-15, and as C on
Debian's gcc 14 and clang 19. On the ESP32-S3 the playing path needs
`__divsf3` (the chip has no single float-divide instruction), and the whole file
adds the eight double-precision routines of the leave-one-out sweep, all from
libgcc, the compiler's support library. `tests/freestanding.sh` requires exactly
that list. The square root is computed in integers and is correctly rounded:
`tools/sqrt_exhaustive.c` compares it with the hardware for all 2^32 inputs.

## 2. Model

One hidden layer, not configurable: `iris_init` takes a width and no depth.
`hidden_h = tanh(Σ w1[h][i]·x_i + b1[h])`, `out_o = sigmoid(Σ w2[o][h]·hidden_h
+ b2[o])`, one hidden vector shared by all outputs. Inputs are min-max
normalised to `[-1, +1]` across the demonstrated range; an input whose
demonstrated width is at most 1e-5 of its magnitude (or 1e-6) is treated as
still and reads 0 in training, playing and every distance. Outputs are
normalised to `[0.1, 0.9]`, and at play time de-normalised and clamped, with no
opt-out, to the ranges the instrument was last fitted to. Every trainer refits
the ranges before it runs. `iris_reseed` draws `w1 ~ U[-1, +1]/√n_in` and
`w2 ~ U[-1, +1]/√n_hid`, biases exactly 0, from xorshift32; a seed of 0 is
taken as 1.

`tanh` is the rational `x(27+x²)/(27+9x²)` with its return value clamped to
[-1, +1]. Its maximum absolute error against true tanh is 0.0235, near
x = 1.566. Because `p(x) − 1 = (x−3)³/(27+9x²)` the clamp takes effect from
|x| = 3, and it keeps the backward factor `1 − a²` non-negative everywhere
(0 negatives over 66,368,438 finite inputs). Against true tanh and a rational
245 times more accurate, over 2,304 paired runs on six synthetic target shapes,
the accurate functions give 1.3% higher held-out error as a geometric mean
(worse on 57.1% of pairs; 0.2% without the one saturating target), and cost 12%
(the [7/6] rational) to 53% (the C library's `tanhf`) more per epoch. The
backward pass uses the true-tanh derivative, a surrogate; the exact derivative
of the rational makes no measurable difference (geometric mean 0.997, 95%
interval 0.988 to 1.005). The maximum error is in
[`docs/MATH-FIXES.md`](MATH-FIXES.md); the other figures in this paragraph come
from studies whose programs are not in this repository. The nonlinearity is frozen because saved instruments
depend on it.

## 3. Trainers

The update rule exists once, in `iris_internal_train_run`:
`v = momentum·v − lr·g·x; w += v`, biases with the input factor omitted;
`d_out = (y−t)·y·(1−y)`, `d_hid = (Σ w2·d_out)(1−a²)`. The learning rate and
momentum are 0.10 and 0.85 and are internal. Updates are per demonstration, and
the order is Fisher–Yates shuffled at the top of every epoch from the
instrument's persistent random state, which is why that state is in the file.
Smoothing is decoupled weight decay on the weights only, `0.3 × smoothing`,
scaled by `1/n_ex`.

- **`iris_train`**, the one to call: refuse a store it cannot train on, then
  reseed from the instrument's own seed, then train to a plateau. The plateau
  test runs every 2,000 epochs and stops when a window buys less than 10% of the
  error, under a ceiling of 60,000 epochs (30,000 where `int` is 16 bits). Two
  more rules end a run: an absolute floor of 1e-6 on the mean squared error,
  which sits outside the plateau test and at 10 or fewer demonstrations is
  usually what stops the run, and the divergence clamp at |w| > 16.
  `examples/01_hello.c` stops at 2,557 epochs with error 9.89e-7: not a
  multiple of 2,000, so the floor stopped it.
- **`iris_train_begin` / `iris_train_slice`** run the same training in slices
  and are bit-identical to `iris_train` (`tests/train.c`).
- **`iris_continue`** and **`iris_continue_to_plateau`** carry on from the
  current weights: fixed epochs, or to the plateau. `iris_continue` is the
  fixed-epoch recursion the golden hash pins. After a deleted bad take they keep
  its influence: on 20 demonstrations of a smooth target plus one contradictory
  take, trained, deleted and trained again, continuing leaves the instrument 14
  times further from the true mapping than `iris_train` (40 of 40 seeds, from a
  study whose program is not in this repository). While
  a weight sits exactly on ±16 they refuse with `IRIS_DIVERGED_STUCK` on every
  call; `iris_train` is the way out.
- **`iris_train_elm`**, the closed-form trainer (an extreme learning machine):
  a frozen random hidden layer at gain 2/√n_in, and the output layer solved as
  ridge least squares *in logit space* by one (nh+1)² Cholesky factorisation.
  The ridge is relative (`lam0·trace/K + 1e-7`) with up to 8 deterministic
  doublings on a failed factorisation; smoothing adds `1.2 × smoothing` to the
  ridge on the output weights (never the bias). Every refusal leaves the
  instrument bit-identical. `tests/audit.c` runs 90 hostile solves with 0
  unfixable and at most 2 doublings, and at 48 hidden units its recall beats
  backpropagation's (0.0030 against 0.0059). Output weights may exceed 16; such
  an instrument saves and plays normally, and `iris_train` is the way back to
  gradient training.
- **`iris_knn_predict`** and **`iris_classify_1nn`**: inverse-squared-distance
  regression (k clamped to 8, exact when the neighbours agree) and a
  bit-verbatim nearest-demonstration snap, both training-free, with distances
  in fractions of each input's range.
- **`iris_loo_error`** and **`iris_suggest_smoothing`**: leave-one-out over
  600-epoch fits, the second at five smoothing settings, restoring every byte
  of the arena afterwards.

The limited-memory quasi-Newton trainer (L-BFGS, the limited-memory
Broyden–Fletcher–Goldfarb–Shanno method) that 0.1.0 carried as an
experiment is no longer part of the library.

## 4. Relation to Wekinator and Weka 3.6.12

Wekinator never calls `setLearningRate`, `setMomentum`, `setTrainingTime` or
`setOptions`, so Weka's constructor defaults stand: learning rate 0.3, momentum
0.2, 500 epochs.

**Structurally faithful** (checkable by inspection, not by comparing outputs):
the per-weight recursion within a run is an exact rewrite of Weka's
`delta = lr·err·x + momentum·delta_prev` under the opposite sign convention;
the per-demonstration granularity (`n_ex` weight writes per epoch, not one);
one fully connected hidden layer; the bias placement and update; and
`iris_classify_1nn`, which follows Wekinator's default `KNNModelBuilder` at
k = 1 with range-normalised squared Euclidean distance and earliest-recorded
tie-breaking. `tests/audit.c` checks that last one at 441 probes against an
independent double-precision reference written to Weka IBk and LinearNNSearch
semantics: 441 agree, 0 disagree, 0 distance ties. The Java has never been run
against it, and agreement cannot be bit-exact in principle: float32
accumulation can reorder near-ties against Weka's doubles, and an input that
never moved is ignored here, which may not match Weka's treatment of a constant
attribute. The internal setter accepts Weka's pair, so
`iris_internal_set_learning(k, 0.3f, 0.2f)` is reachable.

**Deliberate divergence.** Ours: sigmoid outputs, always clamped; theirs:
`setEndsToLinear()`, clamped only on request. A bounded output matched to the
demonstrated range is a trade-off, not a win: for a filter cutoff on hardware,
unbounded extrapolation is a fault; for a musician reaching past their
demonstrations, the clamp is a wall. Ours: the rational tanh; theirs: the
double-precision logistic. Ours: one network with a shared hidden vector;
theirs: one `MultilayerPerceptron` per output, and which way that moves the fit
is unmeasured. Ours: no depth setting; theirs: a choice of 0 to 3 hidden
layers, and nothing measures a two-layer iris. Ours: learning rate 0.10 and
momentum 0.85, with no ablation in the tree comparing 0.85 against 0.2. Ours:
reshuffle every epoch; theirs: once. Biases start at 0 here and at U(−0.05,
0.05) there. And binary32 against binary64 throughout, which makes numerical
identity impossible in principle.

Wekinator has nine algorithm families; iris implements the multilayer
perceptron and nearest-neighbour. The largest hole is dynamic time warping
(FastDTW), 6,662 lines in Wekinator's source and no temporal machinery here: `iris_predict` is a
pure function of the current frame. Linear regression is the hardest omission
to defend, being index 1 of the numeric registry, inside the mode iris
reconstructs. Also absent: per-output input subsetting, a settable k on the
classification path (`iris_classify_1nn` is fixed at k = 1; `iris_knn_predict`
takes a k up to 8), and Wekinator's Open Sound Control command set.

## 5. The residual ledger

`iris_example_stress` and `iris_worst_example_id` expose an integrated
per-demonstration residual, summed over every epoch of the last training run,
as a ranking of which demonstration fights the others. Wekinator's nearest
equivalent is a per-model number; nothing there scores an individual example.
It is a *margin*, worst over second-worst, flagged at `IRIS_STRESS_FLAG` 2.5,
because the worst-of-n level rises with n on clean data; it is silent below 12
demonstrations. Measured with `tests/elm.c measure` (12 hidden units, 8
outputs, one take offset on one output): after `iris_train` the offset take is
ranked first in 18 to 20 of 20 sessions at offsets from 0.05 to 0.40 and 20,
50 or 100 demonstrations; clean sessions reach the flag in 7, 15 and 6 of 200.
After the closed-form trainer, whose ledger is the residual left by its solve,
the take is ranked first in 13 to 20 of 20 and clean sessions reach the flag in
25, 14 and 24 of 200. `tests/audit.c` ranks a corrupted demonstration first in
11 of 12 trials; `examples/02_fix_a_mistake.c` ranks the bad take first at
margin 2.19, below the flag, so it is a ranking rather than an accusation.

## 6. Measured cost, with scope

Host, the development laptop (Apple M4 Max, Apple clang `-O2`), from the
training-cost table `sh build.sh audit` prints:

| | 20 demonstrations | 50 demonstrations |
|---|---|---|
| `iris_train` (2-12-3, reference task) | 18,000 epochs, 31 ms | 12,000 epochs, 52 ms |
| 600 epochs of backpropagation | 1.1 ms | 2.6 ms |
| `iris_train_elm`, 12 hidden units | 4.2 µs | 8.5 µs |

One prediction takes 0.036 µs; `iris_knn_predict` takes 0.30 µs at 64
demonstrations and 1.2 µs at 256.

On the ESP32-S3, the only figures come from the starter kit's `device_torture`
sketch, run with iris 0.1.0 on two boards at 240 MHz; no log of those runs is
recorded yet. Test 9: one prediction of a 2-12-3 instrument takes 14.9 µs, the
mean of 20,000 calls (298,915 µs in total), with loop overhead of about 1–2%
folded in and no worst case reported. Test 5: `iris_train` takes 595 ms at 4
demonstrations and 2.7–3.0 s at 8 to 20, on a recipe whose two inputs are
exactly collinear and one output constant, which stops after 3,823 to 9,343
epochs. Data like the reference task of `tests/audit.c`, which takes 18,000
epochs at 20 demonstrations, has not been timed on the board; nor has the
closed-form trainer. No ratio
between host and board timings is stable enough to scale one into the other.

Fit quality on the reference task of `tests/audit.c` (20 demonstrations, 2-12-3):
600 epochs recall the demonstrations to a root-mean-square 0.0066; the plateau
run, 18,000 epochs, to 0.0012.

## 7. Limitations

These are load-bearing, not caveats.

1. **No head-to-head comparison with another implementation.**
   `sh build.sh reference` compares iris with an independent double-precision
   reference of the same algorithm; nothing compares it with another library.
2. **The default stopping rule and smoothing are provisional.** On six
   synthetic target shapes with Gaussian output noise of standard deviation
   0.05 or more, `iris_train` at smoothing 0 is 1.2 to 2.2 times worse on
   held-out error than a fixed 100-epoch run. Smoothing 0.33 repairs most of
   that and costs 16%, 29% and 66% on clean data at 10, 20 and 50
   demonstrations. The decision waits for a study of recorded human gesture.
3. **Nothing is verified on recorded human gesture**: not accuracy, not the
   plateau rule, not the ledger's premise.
4. **The weight limit flags good fits**: 40 of 2,304 default fits, nearly all
   on sharp targets at 50 demonstrations, report `IRIS_TRAINING_DIVERGED`
   although they play as well as the healthy ones (a study whose program is
   not in this repository).
5. **Hardware figures** are as scoped in section 6: no representative
   training time on the board, and no recorded log for any board figure.
6. **Recording order matters a little.** Recording the same demonstrations in
   a different order gives a bit-different instrument, because the shuffle
   works on positions: over 60 random orders the largest difference on the
   probe grid is 0.0037, against a median of 0.0116 for a change of seed (a
   study whose program is not in this repository). So
   deleting and re-recording a take does not return exactly the instrument you
   would have had.
7. The save format records no trainer, so a file cannot say which trainer
   produced it. Demonstrations spanning more than about 3.4e38 end to end give
   an infinite range width, which training traps and `iris_save` refuses.
