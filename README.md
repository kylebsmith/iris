# iris

**Interactive machine learning for handmade instruments, in one C99 header.**

BSD 3-Clause licensed. Version 0.2.0.

You demonstrate a handful of gestures paired with sounds. It learns the mapping
and fills in everything between. No laptop in the loop, no runtime, no build
system, no install: you include one file, written in C99, the 1999 edition of
the C language, which every microcontroller compiler accepts.

```c
#include "iris.h"

static unsigned char memory[IRIS_ARENA(2, 12, 3, 64)];   /* fixed, no malloc */

int main(void) {
  float gesture[2] = { 0.2f, 0.7f };               /* two sensor readings */
  float sound[3] = { 440.0f, 0.5f, 0.1f };         /* three sound parameters */
  iris *k = iris_init(memory, sizeof memory, 2, 12, 3, 64, /*seed=*/1234);

  iris_record(k, gesture, sound);      /* do this a few times */
  iris_train(k);                       /* fit them, starting from the seed */
  iris_predict(k, gesture, sound);     /* now play it */
  return 0;
}
```

Recorded a bad take? `iris_delete_id(k, id)`, then `iris_train(k)` again.
`iris_train` starts over from the instrument's seed every time, so the take you
deleted leaves nothing behind.

```sh
cd examples && cc -std=c99 -O2 -Wall -Wextra -I.. -o min 00_minimal.c && ./min
```

`cc` is your C compiler, run from a terminal. On macOS,
`xcode-select --install` provides it; on Debian or Ubuntu,
`sudo apt install gcc`; on Windows, use gcc under WSL (the Windows Subsystem
for Linux) or MSYS2. For the Arduino example, `examples/iris_smallest/`, the
library has to be installed first: in the Arduino IDE (its integrated
development environment), Sketch > Include Library > Add .ZIP Library with a
zip of this repository, or copy the repository folder into your
`Arduino/libraries` folder.

That example is short enough to read in one sitting and compiles with **zero
warnings** under `-Wall -Wextra`. If you can write the block above, you can use
this library; everything else is detail, and [`iris.h`](iris.h) explains it,
starting at the top of the file.

---

## Start here

1. **Read one sketch.** [`examples/iris_smallest/iris_smallest.ino`](examples/iris_smallest/iris_smallest.ino)
   records two demonstrations, trains, and asks about the points between them.
   Install the library, open it from File > Examples > iris, upload, and open
   the Serial Monitor at 115200 baud.
2. **Learn three calls.** `iris_record(k, input, output)` stores one
   demonstration: this gesture should make this sound. `iris_train(k)` fits a
   smooth curve through every demonstration you have recorded.
   `iris_predict(k, input, output)` asks the curve what to play for a gesture
   you never demonstrated. Everything else in the library refines one of these.
3. **Watch a network learn.** The starter kit's
   [`iris_scope`](https://github.com/kylebsmith/iris-starter/tree/master/iris_scope)
   draws the curve on a laptop screen as you tilt a sensor: record two poses
   and it appears; record a bad one and it bends; delete that one and it
   re-fits.
4. **Understand why it works.** [`docs/SYSTEM-plain-english.md`](docs/SYSTEM-plain-english.md)
   explains the network from scratch, for someone who has never trained one;
   [`docs/SYSTEM-technical.md`](docs/SYSTEM-technical.md) says the same for a
   technical reader.

## The hardware half

The starter kit, https://github.com/kylebsmith/iris-starter, is what a
student receives: Arduino sketches for an ESP32-S3 board, a getting-started
walkthrough, and a Processing sketch that draws the learned mapping so you can
see it rather than only hear it. Start there if you have a board; start with
[`examples/`](examples/) here if you do not.

## Why this exists

An instrument you cannot rely on is not an instrument.

The tools musicians use to build gestural instruments keep dying. Not from bad
mathematics — from dependency rot. Wekinator's example patches need Processing
2 and Kinect software development kits that no longer exist. MnM depends on a
library whose published builds are 32-bit, so it will not load in 64-bit Max.
The Gesture Recognition Toolkit has not changed since 2019 and needs patching
for modern compilers. ml.lib's last release is from 2022, and an open report
from 2024 says it fails to load. The piece you wrote quietly becomes
unperformable, and the practice you built around it goes with it.

So the barrier is doubly unfair. It is already technical enough to exclude most
musicians before they start, and the ones who get through find the ground moving
under them faster than a practice can mature.

**You should own your instrument.** A stranger's commit should not be able to
change how yours responds. You cannot learn the guitar if someone restrings it
every few months and moves the notes around.

That is a design constraint, not a sentiment, and it is what most of this
library's odd decisions are for:

- **No dependencies**, so there is nothing to rot. One C compiler, for ever.
- **A frozen playing contract.** What a saved instrument plays is pinned, bit
  for bit, by a golden file in the test suite, and no release with the same
  major version may change it. Training may get better, but only measured,
  announced, and never behind your back: see
  [What is promised, and what is not](#what-is-promised-and-what-is-not).
- **Your instrument is a file you own.** Weights and demonstrations together,
  in a checksummed format whose every field is checked before it loads. Every
  release from 0.2.0 on reads every format from 7 on.
- **A permissive licence with no account, no cloud, no service.** Nothing to
  revoke.

The library is a tool for making instruments, so its own stability is not a
nice-to-have. It is the whole product.

---

## What it is

`iris.h` is a single header, most of it explanation, implementing the
interactive machine learning loop that Wekinator made standard in 2009, rebuilt for boards that have no
operating system. Every public function is listed with one line at the top of
the header.

- **No dependencies.** The header includes `<stddef.h>` and `<stdint.h>` and
  nothing else: no C library, no `math.h`, no `printf`. A unit that calls every
  function compiles with `-ffreestanding` and links with `-nostdlib -static`
  with zero undefined symbols, on Apple clang, clang 22 and gcc-15, and on
  Debian's gcc 14 and clang 19. Two or three compiler flags are needed to keep
  the *compiler* from inserting C library calls of its own, and none changes
  an output bit; the masthead of [`iris.h`](iris.h) lists them and says why.
  On the ESP32-S3 the list is not empty: every float division is a call to
  `__divsf3`, a routine in the compiler's own support library, and the
  leave-one-out diagnostic adds eight double-precision routines from the same
  library. `sh build.sh freestanding` checks all of it.
- **No heap.** You hand it one block of memory and it never asks for more.
  `IRIS_ARENA()` computes the size at compile time and equals `iris_size()`
  exactly. `sh build.sh noheap` counts allocator calls from every entry point
  and requires zero.
- **Deterministic.** Same seed and same demonstrations give the same bits.
  The header refuses to compile under `-ffast-math`, `-Ofast`,
  `-ffinite-math-only`, and, on GCC (the GNU Compiler Collection),
  `-freciprocal-math` and `-funsafe-math-optimizations`, and on 32-bit x86
  builds that compute floats on the old x87 unit; it stops the compiler
  fusing a*b + c into one multiply-add instruction (which rounds once
  instead of twice, and so changes the last bit) in its own code and only
  its own code; and golden hashes pin the results in the tests.
  `sh build.sh determinism` checks the golden hash at `-O0` to `-Os` with
  contraction off, on and at the compiler's default. What the header cannot
  see, and you must not use: clang's `-ffp-contract=fast`,
  `-freciprocal-math` and `-funsafe-math-optimizations`, clang 22's
  `-ffp-model=fast` (Apple clang 17 refuses it), and GCC's
  `-fassociative-math`.
- **The demonstrations are the interface.** Every demonstration is
  individually listable, auditionable and deletable, because that is how
  practitioners repair these models. In Fiebrink, Cook and Trueman's study of
  composers building with Wekinator (Human model evaluation in interactive
  supervised learning, CHI 2011, the conference on human factors in computing
  systems), none of the composers used cross-validation; they judged the model
  by playing it and fixed it by changing its examples.

Two more algorithms share the same demonstrations: `iris_knn_predict`, a
distance-weighted blend of the nearest demonstrations, and `iris_classify_1nn`,
which returns the nearest one exactly, the default classifier of desktop
Wekinator. A closed-form trainer, `iris_train_elm`, fits the same network in
microseconds by freezing its hidden layer and solving the output layer
directly. And the library points at the demonstration that fights the others
(`iris_worst_example_id`), so you know which take to listen to again.

For the design in more depth, read
[`docs/SYSTEM-technical.md`](docs/SYSTEM-technical.md); for the same thing
explained from scratch, [`docs/SYSTEM-plain-english.md`](docs/SYSTEM-plain-english.md).
[`docs/README.md`](docs/README.md) says what every other document in `docs/` is.

## Threading

**Never touch the same instrument from two places at once.** That is the whole
contract.

There is no writable state outside the instrument you passed in: no globals, no
static buffers. So two instruments cannot interact, on any number of cores.
`sh build.sh audit` checks it with four instruments trained alone and then
alive at once with every record, training run and prediction interleaved; all
four end byte-identical either way. `sh build.sh threads` runs the same idea
under ThreadSanitizer, the compiler instrumentation that reports two threads
touching the same memory, with a positive control that shares one instrument
between two threads and must be reported.

So this is fine: any number of instruments on one core called one after
another; one instrument per thread across many cores; one instrument living
only inside an interrupt.

This is not: the *same* instrument from two threads, or from an interrupt and
from the main loop, even if both only play it. `iris_predict` writes its working
values inside the instrument, so an interrupt landing mid-call leaves both
answers wrong. Give the interrupt its own instrument.

One platform exception, and it is the board most readers here have: on an ESP32
under FreeRTOS, the real-time operating system its Arduino core runs on, the
floating-point registers are not saved when an interrupt is taken, so float
arithmetic of any kind inside an interrupt handler can corrupt the interrupted
task. iris is float throughout, so on that chip do not call it
from an interrupt at all: read the sensor there, set a flag, and call iris from
the main loop.

## How fast it is, and where each number comes from

| What | Where | Figure | Source |
|---|---|---|---|
| One prediction, 2 inputs, 12 hidden units, 3 outputs | laptop | 0.036 µs | `sh build.sh audit`, training-cost table |
| `iris_train`, 20 demonstrations of the reference task | laptop | 18,000 epochs, 31 ms | same |
| `iris_train`, 50 demonstrations | laptop | 12,000 epochs, 52 ms | same |
| `iris_train_elm`, 50 demonstrations | laptop | 8.5 µs | same |
| `iris_knn_predict`, 256 demonstrations | laptop | 1.2 µs | same |
| One prediction, 2-12-3 | ESP32-S3, 240 MHz | 14.95 µs (median of batches, empty loop subtracted); 99.9% of calls within 19.8 µs, worst 47 µs | `board_probe`, [board log](docs/board/2026-09-25-es3c28p.txt) |
| One prediction, 6-16-8 / 12-32-8 | ESP32-S3 | 35.7 µs / 74.2 µs (medians) | same |
| `iris_train`, 20 demonstrations of the reference task | ESP32-S3 | 18,000 epochs, 13.4 s | same |
| `iris_train`, 10 / 50 demonstrations | ESP32-S3 | 10.6 s / 22.1 s | same |
| One slice of 64 epochs, 20 demonstrations | ESP32-S3 | 48 ms | same |
| `iris_train_elm`, 20 / 50 demonstrations | ESP32-S3 | 1.5 ms / 3.5 ms | same |

"Laptop" is the development machine, an Apple M4 Max, with Apple clang `-O2`.
The board figures are iris 0.2.0 on an ES3C28P (ESP32-S3 revision 2, 240 MHz,
Arduino-ESP32 3.3.3, gcc 14.2, `-Os`), measured on 2026-09-25 by the starter kit's
`board_probe` sketch; the [board log](docs/board/2026-09-25-es3c28p.txt) names the
build. Training on the board is slow: a sketch that must keep drawing trains in
slices (48 ms per 64 epochs), or uses the closed-form trainer.

For audio, 14.95 µs against the 20.8 µs of one sample at 48 kHz is 1.4 times of
margin: 72% of a core spent on playing alone. It is enough to run per sample and
not enough to do anything expensive in the same callback. At a control rate of
1,000 predictions a second it is 1.5% of a core. Training does not fit inside an
audio callback: train in slices from the main loop with `iris_train_begin` and
`iris_train_slice`, which is bit-identical to `iris_train`, and never from an
interrupt.

## Run the tests

```sh
sh build.sh           # every check below that needs only a C compiler
sh build.sh test      # the same
```

It stops at the first failure and says which check failed. One check at a time:

| Command | What it checks |
|---|---|
| `sh build.sh audit` | the correctness checks, including the golden hash `0x6805FB0D` |
| `sh build.sh regressions` | one test per defect the library must not repeat |
| `sh build.sh coverage` | the refusal paths: each case asks a function to refuse and checks that it does, and says so |
| `sh build.sh load` | the save format: round trips, every rule, a committed saved instrument played back bit for bit |
| `sh build.sh train` | refusals change nothing, sliced training equals `iris_train`, the stuck-divergence refusal, the smoothing suggestion |
| `sh build.sh elm` | the closed-form trainer |
| `sh build.sh playing` | the playing and neighbour paths |
| `sh build.sh portability` | the in-header square root against the host's, and the other stand-ins for the C library |
| `sh build.sh recipes` | the starter kit's two pinned recipes (`tests/starter_recipes.c`) |
| `sh build.sh tu` | two translation units with different maxima sharing one instrument |
| `sh build.sh fuzz` | random shapes and call sequences under AddressSanitizer |
| `sh build.sh examples` | every example, built with `-Werror` and run |
| `sh build.sh sanitize` | every test program and example under AddressSanitizer and UndefinedBehaviorSanitizer |
| `sh build.sh threads` | ThreadSanitizer, with a positive control |
| `sh build.sh noheap` | zero allocator calls, with a positive control |
| `sh build.sh freestanding` | the zero-dependency claim, flag by flag, and the ESP32-S3 symbol list |
| `sh build.sh targets` | the header builds, warning-free, for every target it supports, and refuses the x87 unit |
| `sh build.sh pragma` | the contraction pragmas do not leak into your code |
| `sh build.sh fuzz-load [seconds]` | coverage-guided fuzzing of `iris_load`: mutated files, steered toward code not yet reached (needs clang's libFuzzer) |
| `sh build.sh determinism` | the golden hash across optimisation levels and contraction settings |
| `sh build.sh cov` | line and branch coverage of `iris.h` across the test programs, with thresholds (needs clang's `llvm-cov`) |
| `sh build.sh mutate` | an advisory mutation run; it reports, it never fails the build (needs Python) |
| `sh build.sh reference` | an independent double-precision reference in Python (skips without numpy, Python's numerical library, and scikit-learn, its machine-learning library) |
| `sh build.sh tiny` | [`docs/tiny.c`](docs/tiny.c), `iris_train` written out again by hand, which must agree with the library to the bit |
| `sh build.sh docs` | `keywords.txt` lists every public function and type, `docs/tiny.c` and the first program in this README build with `-Werror` and the README program runs, `CONTRIBUTING.md` lists every command, and the release version check keeps its rule |
| `sh build.sh clean` | remove `build/` |

AddressSanitizer and UndefinedBehaviorSanitizer are compiler instrumentation
that stops a program at an out-of-bounds memory access or an operation C
leaves undefined. [`CONTRIBUTING.md`](CONTRIBUTING.md) says which checks to run
before a pull request.

## Layout

| Path | What |
|---|---|
| [`iris.h`](iris.h) | The whole library. Start at the top. |
| [`examples/`](examples/) | `00_minimal.c` draws the learned space; `01_hello.c` three gestures; `02_fix_a_mistake.c` the repair loop; `03_reroll.c` same demonstrations, different instrument; `iris_smallest/` the Arduino one |
| [`tests/`](tests/) | The test programs and the committed saved instrument in `tests/golden/` |
| [`tools/`](tools/) | Programs the tests use, such as the exhaustive square-root comparison |
| [`docs/`](docs/) | The two explanations of the system, decision records, the negative-results index and the board logs; [`docs/README.md`](docs/README.md) is the index |
| [`CHANGELOG.md`](CHANGELOG.md) | What changed in each release |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | How to change iris without breaking it |

## What it is not

**It is a reconstruction of Wekinator's lineage, not a reimplementation of
Wekinator.** It reproduces Weka's `MultilayerPerceptron` per-weight update
recursion, its one-hidden-layer depth, its per-demonstration update
granularity, and Wekinator's default nearest-neighbour classifier, which
answers with the single closest example it has seen. It deliberately diverges
on activations, defaults, output units and the stopping rule; the header argues
each one beside the code.

**It is not numerically identical to Weka and cannot be.** iris computes in
32-bit floats; Weka computes in 64-bit doubles. Exact agreement is impossible
in principle, not merely unachieved. The nearest-neighbour classifier agrees
with a double-precision reference written to Weka's rules on every probe the
tests try; the Java program itself has never been run against it.

It implements **two** of Wekinator's nine algorithm families, the default of
each registry. The other seven are absent, including every temporal model
(dynamic time warping, which matches gestures of differing speed), and so is
the analyst's toolkit: no dataset table, no algorithm chooser, no per-output
input selection. `iris_loo_error` gives a leave-one-out error for comparing
settings; it leaves the instrument refitted for a fixed 600 epochs (or the
count you pass), not the instrument you trained, so save first or call
`iris_train` afterwards. `iris_suggest_smoothing` runs the same sweep and puts
every byte back.

## Limitations

- **The machine-learning figures come from synthetic data.** Every accuracy
  figure in this repository was measured on synthetic target functions, many
  with added Gaussian noise, never on recorded human gesture. The figures
  quoted here are ones that were re-run and replicated. Those that name a
  program in this repository (`tests/elm.c measure`, `sh build.sh audit`)
  re-run from here. The rest come from re-runs and studies whose programs are
  not yet published, so for now neither this repository nor iris-studies can
  re-run them, and each says so where it is quoted (see
  [Where the numbers come from](#where-the-numbers-come-from)). Treat them as
  bounded by that.
- **Training to a plateau fits noise.** With smoothing at its default of 0, on
  demonstrations with output noise of standard deviation 0.05 or more,
  `iris_train` is 1.2 to 2.2 times worse on held-out error than a fixed
  100-epoch run. Smoothing 0.33 repairs most of that, but costs 16%, 29% and
  66% more error on clean demonstrations at 10, 20 and 50 of them. No single
  default serves both, so the default stays 0 until a study of real recorded
  gestures settles it. The figures are from six synthetic target shapes and
  12 seeds, tabulated beside `iris_set_smoothing` in the header (from a
  re-run whose program is not yet published; the original study is
  iris-studies S08).
- **The weight limit flags some good fits.** On 2,304 default fits of six
  synthetic shapes, 40, all at 50 demonstrations and nearly all on sharp
  targets, report `IRIS_TRAINING_DIVERGED` although they play as well as the
  healthy ones (from a study whose program is not yet published). The
  header's note on `IRIS_W_LIMIT` says why the limit stays.
- **The worst-demonstration flag can cry wolf and can miss.** After
  `iris_train` it reaches its threshold on 3–8% of clean sessions, and after
  the closed-form trainer on 7–13%; a take offset on one output is ranked
  first in 18 to 20 sessions of 20 after `iris_train` (`tests/elm.c measure`).
- **The board results come from one board.** On 2026-09-25 an ES3C28P
  reproduced every host hash bit for bit: `determinism_check` (`0x203834ED`, and
  the saved file's `0xD8666A69`), `device_torture` test 1 (`0xB7FC47A0`) and the
  golden recipe of `tests/audit.c` (`0x6805FB0D`) in `board_probe`
  ([board log](docs/board/2026-09-25-es3c28p.txt)). A second board has not been run.
- **The ESP32-S3 does not flush subnormal numbers to zero** (measured by
  `board_probe`). iris flushes its own decaying tails below `IRIS_TINY`, so
  host and board agree either way.

## Next to the field

**Wekinator** is the ancestor and remains the richer tool; its last release is
from 2016, with commits until 2019, and it needs a Java virtual machine and a
screen. **RapidLib** is its C++ successor and the learner inside MIMIC and
InteractML; it uses the C++ standard library and a heap, so it is not a
drop-in for a board without them. **FluCoMa** is the live, funded,
BSD-licensed toolkit in this domain and the most serious comparator.
**MEMLNaut** does on-device training for musical mapping on its own open
hardware, and is better resourced; this is not a competitor to it.
**scikit-learn** is the calibration instrument, not the rival.

## What is promised, and what is not

A saved instrument belongs to the person who trained it, and a stranger's commit
should not restring your guitar. So the promise is split by what a change would
do to that instrument.

**Playing a saved instrument is frozen.** A file saved by iris 0.2.0 or later
loads and plays bit for bit the same in every later release with the same major
version number, on every compiler and processor the tests cover. That covers
everything between a sensor reading and a sound: the input scaling to
[-1, +1], the nonlinearity p(x) = x(27+x²)/(27+9x²) clamped to [-1, +1], the
output scaling to [0.1, 0.9], the clamp to the range you demonstrated, the
nearest-neighbour paths, and what every byte of the file means. Changing any of
that needs a new major version and a new file-format number, and the old files
keep loading. The tests hold this with a golden file: a saved instrument,
committed in `tests/golden/`, whose `iris_predict` outputs at 625 points are
compared bit for bit, not within a tolerance. The nearest-neighbour paths and
`iris_novelty` are checked in `tests/audit.c` and `tests/playing.c` but have no
golden file yet.

**Training may get better.** Training again is something you choose to do. A
minor release (0.3, 0.4 and so on) may change how training reaches its weights:
when it stops, the defaults, the arithmetic of one update. It does so only when
the change is measured to be better, and the measurement is published with the
program that made it. When it happens, the training hashes in the test suite
are re-pinned in the same commit and [`CHANGELOG.md`](CHANGELOG.md) says what
changed and by how much. Your saved file still plays exactly as before. Only
retraining gives you a different instrument.

Within one release, the same seed and the same demonstrations give the same
instrument, bit for bit, on every build `sh build.sh determinism` checks
(`-O0` to `-Os`, contraction off, on and at the default, and `-std=gnu99` on
gcc; run with `CC` set to each of Apple clang, clang 22 and gcc-15 on 64-bit
ARM) and on the other builds it has been measured on: 64-bit ARM and x86-64 Linux,
and 32-bit x86 using its SSE vector unit (Streaming SIMD Extensions, SIMD
meaning single instruction, multiple data). On the ESP32-S3 it has been
recorded on one board: the [board log](docs/board/2026-09-25-es3c28p.txt)
matches every host hash.

That needs float arithmetic done in single precision, in the order written. The
header refuses to compile under `-ffast-math`, `-Ofast`, `-ffinite-math-only`,
on GCC under `-freciprocal-math` and `-funsafe-math-optimizations`, and on
32-bit x86 builds that use the old x87 floating-point unit. It cannot see
these, so do not use them: `-ffp-contract=fast`, `-freciprocal-math` and
`-funsafe-math-optimizations` on clang, `-ffp-model=fast` on clang 22, and
`-fassociative-math` on GCC.

**The functions follow semantic versioning.** The version is MAJOR.MINOR.PATCH.
A patch release changes no function and no output. A minor release may add
functions, and may change training as above. Removing or changing a function
needs a new major version, and the function is marked deprecated for at least
one minor release first. The interface block at the top of `iris.h` lists the
whole public interface. Anything named `iris_internal_` can change or disappear
in any release: do not call it.

**The file format has its own number.** 0.2.0 writes format 7. Every later
release reads every format from 7 on, for ever. A file loads into an
instrument made with the same shape (inputs, hidden units, outputs) and a
capacity at least the number of takes it holds. The file carries its shape
and its number of takes as little-endian 32-bit numbers, the shape at bytes
16, 20 and 24 and the takes at byte 28 (the table in PART 9 of `iris.h`), so a
program that receives a file it did not make can read them and call
`iris_init` to match.

A refused load returns 0 and leaves the instrument you passed in untouched,
whatever the reason. `iris_shape` reads back an instrument's shape and
capacity, and the file tells the rest, checked in this order. A file starts
with the letters `IRIS` and, at byte 4, the format number 7; any other start
means another format, such as a 0.1.0 file or a later release's, or no iris
file at all. A shape that differs, or more takes than the capacity, means the
file belongs to another instrument: make one to match with `iris_init` and
load into that. On an AVR board (AVR is the 8-bit processor of the Arduino Uno
and Mega, where `int` is 16 bits) the next identifier, at byte 36, must be at
most 32,767, the identifier limit there, so a file from an instrument that has
handed out identifier 32,767 or later is refused, even if those takes were
deleted since. A file that passes all of these and is still refused is
damaged, or was passed to `iris_load` with a length other than its own. One
kind of refusal says nothing about the file: `iris_load` refuses every file
when the instrument's inputs, hidden units or outputs exceed the
`IRIS_MAX_IN`, `IRIS_MAX_HID` or `IRIS_MAX_OUT` of the source file that calls
it (the maxima are set separately in each translation unit, the file the
compiler builds in one go).
Formats 1 to 6 belong to the 0.1.0 preview and are not read.

**Before 1.0.** While the version starts with 0, a minor release may break the
first three promises above, but only to fix something measured to be wrong, only
with a note in `CHANGELOG.md` saying what moved and what to do about it, and
never silently. The file-format promise holds from 0.2.0 regardless. 1.0 comes
when a study of real recorded gestures settles when training should stop and
how much smoothing is the default, and the determinism check has matched the
host on more than one ESP32-S3 board.

### What each part of the version number permits

| Release | Permitted | Not permitted |
|---|---|---|
| Patch (0.2.1) | Documentation, comments, tests, build tooling; a new refusal for an input the documentation already called invalid, or that crashed or corrupted memory; a fix that moves no golden hash | Any golden hash moving; any signature change; any change to a result for a valid input |
| Minor (0.3.0) | New functions and macros; training changes with re-pinned training hashes; deprecations; a new file-format version whose files older releases refuse cleanly. Before 1.0 only: other breaking changes, each with a migration note | Removing or renaming a public function without a completed deprecation; moving a playback hash (after 1.0) |
| Major (after 1.0) | Removing or changing public functions after deprecation; a playback change, which also takes a new file-format number | Dropping a reader: every format from 7 on stays readable |

A behaviour change is anything that moves a golden hash, changes a return value
or status for a valid input, changes which inputs are refused, or changes what a
saved file plays. Every one is announced in `CHANGELOG.md` with what changed,
which hashes moved (old value to new), and the measured effect.

## The knobs, and why there are so few

Every parameter here was tested against one question: *could a musician
plausibly choose a good value, and what happens at the extremes?* Most could
not, and several were dangerous.

**Not in the interface:** the learning rate and momentum. Momentum at 0.99, one
nudge from the 0.85 default, diverges 11 to 39 runs in 40 depending on the
target, against none at the default. A learning rate of 2.0 diverges only 4 runs
in 16 on a clean target but leaves 15 of the 16 more than five times worse than
the default, 12 of them with a healthy status. And their safe ranges buy little:
tuned per task by an oracle, the learning rate, the weight decay and the epoch
ceiling each improve held-out error by 22% to 59% over the defaults and land
within 8–10% of one another, three ways of saying how hard to chase the
demonstrations (24 synthetic tasks, 3 noise levels). Worse, the one number a
screen could show points *backwards*: across 7 learning rates on 24 noisy
tasks, the setting with the lowest training error was the worst or second-worst
instrument in 14.

**What you actually choose:** the shape (`n_in`, `n_out`, `n_hid`, `cap`), the
seed, which is the reroll, a die rather than a setting, and **smoothing**, 0 to
1, the one setting that changes how good the instrument is. `0` sticks tightly
to your demonstrations; `1` smooths confidently between them. On clean
demonstrations 0 is best, within 5%. On noisy ones smoothing helps, by how much
depends on the target: at output noise 0.10, smoothing 1 is between no better
and 4.1 times better than 0 across six synthetic shapes (2.8 times on the
smooth one), and on one periodic target at noise 0.05 it is worse. Held-out
error usually has its best value in the middle of the range rather than at 1,
and in the runs behind these figures no instrument with smoothing above 0
diverged (0 of 5,760).

The hidden width matters too (tuned between 8 and 64 by an oracle it gains
about 20% at the defaults), but tuning it does not substitute for smoothing on
noisy takes, and choosing it from the data does not work reliably: leave-one-out
selection gains 0 to 12%.

`iris_suggest_smoothing(k, scratch, bytes)` runs a leave-one-out sweep at five
settings and hands you a number. It wants `IRIS_ARENA(n_in, n_hid, n_out, cap)`
bytes of scratch, the size of the instrument's own arena, because the sweep
refits the network many times and it puts your instrument back exactly as it
found it: asking a question should not cost you what you trained. It does not
apply the number, and it is not automatic: rerolling the same demonstrations 16
times changes its answer, about two distinct values per dataset on average over
36 datasets, and an instrument whose character changes when you reroll is worse
than one that is merely unsmoothed.

The measurements behind this section come from re-runs whose programs are not
yet published: the learning rate, momentum, hidden width and smoothing
suggestion from one whose original study is iris-studies S02, and the
smoothing figures (clean and noisy demonstrations, the periodic target, no
divergence above 0) from one whose original study is iris-studies S08.
`iris.h` summarises them beside each setting, except the 5% on clean
demonstrations, the 2.8 times on the smooth target and the hidden width's two
figures, which are quoted only here. Some figures of both original studies did
not replicate and are not quoted here.

## Where the numbers come from

Where a test, a tool or a board log in this repository reproduces or records
a figure, the figure names it. A citation "(iris-studies Snn)" names a study
listed in [iris-studies at its tag v1](https://github.com/kylebsmith/iris-studies/tree/v1),
and stands beside a figure only when that study's record (archived there, or
kept in this repository's `docs/`) holds the figure, or its program prints it;
only S05 and S06 have a program there. A figure from a later re-run whose
program is not yet published says so, as "(from a re-run whose program is not
yet published; the original study is iris-studies Snn)", or as "(from a study
whose program is not yet published)" where no study fits.

## Licence

BSD 3-Clause. See [`LICENSE`](LICENSE).
