# iris

**Interactive machine learning for handmade instruments, in one C99 header.**

You demonstrate a handful of gestures paired with sounds. It learns the mapping
and fills in everything between. No laptop in the loop, no runtime, no build
system, no install — you include one file.

```c
#include "iris.h"

static unsigned char memory[IRIS_ARENA(2, 12, 3, 64)];   /* fixed, no malloc */

iris *k = iris_init(memory, sizeof memory, 2, 12, 3, 64, /*seed=*/1234);

iris_record(k, gesture, sound);        /* do this a few times */
iris_train(k);                         /* stops when it stops improving */
iris_predict(k, gesture, sound);       /* now play it */
```

```sh
cc -std=c99 -O2 -Wall -Wextra -I. -o min examples/00_minimal.c -lm && ./min
```

That example is nine lines of body and compiles with **zero warnings** under
`-Wall -Wextra`. If you can write those nine lines, you can use this library;
everything else is detail.

---

## Why this exists

An instrument you cannot rely on is not an instrument.

The tools musicians use to build gestural instruments keep dying. Not from bad
mathematics — from dependency rot. Wekinator's example patches need Processing 2
and Kinect SDKs that no longer exist. MnM will not load in 64-bit Max. GRT needs
patching for modern compilers. ml.lib breaks on each Max release. The piece you
wrote quietly becomes unperformable, and the practice you built around it goes
with it.

So the barrier is doubly unfair. It is already technical enough to exclude most
musicians before they start, and the ones who get through find the ground moving
under them faster than a practice can mature.

**You should own your instrument.** A stranger's commit should not be able to
change how yours responds. You cannot learn the guitar if someone restrings it
every few months and moves the notes around.

That is a design constraint, not a sentiment, and it is what most of this
library's odd decisions are for:

- **No dependencies**, so there is nothing to rot. One C compiler, forever.
- **A frozen behavioural contract**, pinned by golden hashes in the test suite.
  If a change alters what your instrument does, the tests fail — including
  changes we make.
- **Your instrument is a file you own.** Weights and demonstrations together,
  loadable bit-identically, permanently. The loader keeps reading every older
  format.
- **A permissive licence with no account, no cloud, no service.** Nothing to
  revoke.

The library is a tool for making instruments — so its own stability is not a
nice-to-have. It is the whole product.

---

## What it is

`iris.h` is a single header — 3,190 lines, 1,154 of them code — implementing the
interactive machine learning loop that Wekinator made standard in 2009, rebuilt
for targets that have no operating system.

- **No dependencies.** Compiles `-ffreestanding -nostdlib` and links with zero
  undefined symbols. No libc, no `math.h`, no `printf` — the transcendental
  functions are in the file. Three compiler flags are needed to hold that
  literally, none of which changes a single output bit, and all three are
  verified by `build.sh claims` on every compiler it can find:
  `-fno-stack-protector` (clang injects two stack-guard symbols by default),
  and on GNU compilers `-fno-math-errno` (otherwise a call to `sqrtf` is
  emitted for a negative input that never occurs) and
  `-fno-tree-loop-distribute-patterns` (otherwise `iris_reseed`'s zeroing loop
  is recognised and replaced with a call to `memset`).
- **No heap.** You hand it one block of memory and it never asks for more.
  `IRIS_ARENA()` computes the size at compile time. Measured zero allocator
  calls across record / train / 100,000 predictions / delete / retrain.
- **Deterministic.** Same seed and same data give the same bytes, and it is a
  stated contract with three layers of enforcement: `#error` on `-ffast-math`,
  an FP-contraction pragma, and golden output hashes pinned as live test
  assertions. Verified across 16 of 20 optimisation and contraction flag
  combinations on Apple clang 17/arm64. The four that fail are
  `-ffp-contract=fast` at `-O1/-O2/-O3/-Os` — `-O0 -ffp-contract=fast` passes —
  which is the case clang ignores the pragma for. GCC ignores it always,
  including xtensa-esp32s3, so those builds must pass `-ffp-contract=off`
  explicitly. **The ESP32-S3 determinism leg is untested**: no on-device
  hash check exists yet.
- **The examples are the interface.** Every demonstration is individually
  listable, auditionable and deletable, because that is how practitioners
  actually repair these models — Fiebrink's 2011 study found composers never
  once used cross-validation; they deleted the bad take and recorded it again.

## Threading

**Never touch the same instrument from two places at once.** That is the whole
contract.

There is no mutable state outside the instrument you passed in — no globals, no
static buffers — so two instruments cannot interact, on any number of cores.
Verified: four instruments trained interleaved, 8,000 interleaved predictions,
zero cross-talk.

So this is fine: any number of instruments on one core called one after
another; one instrument per thread across many cores; one instrument living
only inside an interrupt. There is no per-core limit, and a single-core chip
runs one thing at a time anyway.

This is not: the *same* instrument from an interrupt and from the main loop.
`iris_predict` writes its working values inside the instrument, so an interrupt
landing mid-call leaves both answers wrong. Give the interrupt its own
instrument.

For audio: one prediction is **14.9 µs** on an ESP32-S3 against a 20.8 µs sample
period at 48 kHz. It fits inside a sample, but with about 1.4x of margin, not the
comfortable multiple an earlier version of this line claimed — that figure was
7.4–7.8 µs, which was a host measurement scaled by an estimated ratio and
presented as if it had been taken on the part. Measured on the board:
`device_torture.ino` test 9, 20,000 predictions in 298,915 µs on an ESP32-S3 at
240 MHz, 2 inputs / 12 hidden / 3 outputs. Reproduced within 0.001 µs across
runs and across two different boards.

Treat 1.4x as the real budget. It is enough to run per-sample, and it is not
enough to also do anything expensive in the same callback.

Training does not fit and is not close — measured on the same boards:
595 ms at 4 demonstrations, and 2.7-3.0 seconds at 8 to 20. Train in slices from
the main loop with iris_train_begin / iris_train_slice, which is bit-identical to
the blocking call, and never from an interrupt.

## Run the tests

```sh
sh build.sh audit     # 41 correctness checks, including the golden hashes
sh build.sh mpe       # the MPE encoder, byte level
sh build.sh sinks     # the CC and OSC output ports
```

One command, no flags, no packages. If `audit` prints `ALL CHECKS PASSED
(0 failed)` the library is behaving exactly as it did when those hashes were
frozen.

## Layout
| Path | What |
|---|---|
| `iris.h` | The whole library. Start at the masthead. |
| `examples/` | `00_minimal.c` draws the learned space; `01_hello.c` three gestures; `02_fix_a_mistake.c` the repair loop; `03_reroll.c` same demos, different instrument |
| `extras/` | Output ports, the two port interfaces, and the browser benchmark. **Nothing in the library calls any of it.** |
| `tests/` | The audit suite and the frozen golden fixtures |
| `experimental/` | L-BFGS. Not in the core, no production callers — read its header before citing it |
| `docs/` | Design notes, architecture decision records, and negative results |


## What it is not

**It is a reconstruction of Wekinator's lineage, not a reimplementation of
Wekinator.** It reproduces Weka's `MultilayerPerceptron` per-weight update
recursion, its one-hidden-layer depth, its per-sample update granularity, and
Wekinator's default 1-NN classifier. It deliberately diverges on activations,
hyperparameter defaults, output units and the stopping rule — each argued at
file and line in the audit.

**It is not numerically identical to Weka and cannot be.** We compute in
binary32; Weka computes in binary64. Exact agreement is impossible in
principle, not merely unachieved.

It implements **two** of Wekinator's nine algorithm families — the default of
each registry. The other seven are absent, including every temporal model
(DTW), and so is the analyst's toolkit: no cross-validation, no dataset table,
no algorithm chooser, no per-output input selection.

## Honest limitations

- **No measurement against another implementation exists inside the test
  harness.** Every accuracy and speed number here is either intra-project or
  against a reference we wrote ourselves. Treat all of them as bounded by that.
- **The recommended trainer is provisional.** Every accuracy figure was
  measured on smooth, noiseless, synthetic targets. A fair re-run with additive
  noise found that training to a plateau degrades badly once the data is noisy,
  where a fixed epoch ceiling holds up. Real sensors are noisy. See
  `docs/adr/0021`.
- **Only one training measurement exists on real hardware.** Everything else
  labelled "S3" is a scaling of a host measurement, and the scaling column says
  so.

## Next to the field

**Wekinator** is the ancestor and remains the richer tool; frozen since 2016,
needs a JVM and a screen. **RapidLib** is its C++ successor and the learner
inside MIMIC and InteractML; smaller and more portable than this. **FluCoMa**
is the live, funded, BSD-licensed regressor in this domain and the most serious
comparator. **MEMLNaut** is doing on-device training for musical mapping first,
better resourced, and shipping — this is not a competitor to it.
**scikit-learn** is the calibration instrument, not the rival.

## What is frozen, and what is not

The library has not shipped yet, so this is the moment things become permanent.
Once a student saves an instrument, anything that changes what those bytes
*mean* becomes a promise we cannot break — a stranger's commit should not
restring your guitar.

**Frozen permanently. These change what a saved instrument plays, and will not
be changed again:**

- The nonlinearity — `p(x) = x(27+x²)/(27+9x²)`, clamped to tanh's codomain.
  Measured against a 245× more accurate approximant and against true tanh over
  2,304 paired runs: the accurate versions are **3.8% worse** held-out and cost
  43% more training time. It is a chosen function, not a compromise.
- The update rule, per-example with classical momentum at 0.10 / 0.85.
- The output scaling to [0.1, 0.9] and the input scaling to [−1, +1].
- The save format's meaning: weights, demonstrations, RNG state, scaling version.

**Free to change, because they do not alter a saved instrument:** anything
additive to the API, the stopping rule's internals, diagnostics, ports, and the
surrogate-gradient refinement if it ever earns its measured 1.4%.

## The knobs, and why there are so few

Every parameter here was audited against one question: *could a musician
plausibly choose a good value, and what happens at the extremes?* Most could
not, and several were dangerous.

**Removed from the public API:** the learning rate and momentum. Momentum at
0.99 — one nudge from the 0.85 default — **diverged 21 of 40 runs and
permanently bricked 20 of them.** A learning rate of 2.0 destroyed 5 of 16. And
their safe ranges buy nothing: tuning the learning rate, the epoch ceiling, the
hidden width and the smoothing all land within 2–4% of each other, because they
were five spellings of one axis. Worse, the only number a UI could show points
*backwards* — the setting with the best-looking training error produced nearly
the worst instrument.

**What you actually choose:** the shape (`n_in`, `n_out`, `n_hid`, `cap`), the
seed — which is the reroll, a die rather than a setting — and **smoothing**,
0 to 1, the one knob that changes how good the instrument is. `0` sticks tightly
to your demonstrations; `1` smooths confidently between them. On clean
demonstrations 0 is best; on noisy ones 1 is **2.6× better**. Monotone across
its whole range, zero divergences at any value.

`iris_suggest_smoothing(k, scratch, bytes)` will run a leave-one-out sweep and
hand you a number. It wants `iris_save_size(k)` bytes of scratch, because the
sweep refits the network many times and it puts your instrument back exactly as
it found it — asking a question should not cost you what you trained.
It does not apply it, and it is not automatic — tested as an automatic default
it returned 2.36 different values from one dataset across 16 rerolls, and an
instrument whose character changes when you reroll is worse than one that is
merely unsmoothed.

## Licence

BSD 3-Clause. See `LICENSE`.
