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
iris_train_converge(k, 0, 0, 0);       /* stops when it stops improving */
iris_predict(k, gesture, sound);       /* now play it */
```

```sh
cc -std=c99 -O2 -I. -o hello examples/01_hello.c -lm && ./hello
```

---

## What it is

`iris.h` is a single header — 1,982 lines, 895 of them code — implementing the
interactive machine learning loop that Wekinator made standard in 2009, rebuilt
for targets that have no operating system.

- **No dependencies.** Compiles `-ffreestanding -nostdlib` and links with zero
  undefined symbols (with `-fno-stack-protector`; on a default macOS clang
  invocation the compiler injects two stack-guard symbols of its own — a
  toolchain default, not a call this source makes). No libc, no `math.h`, no
  `printf`. The transcendental functions are in the file.
- **No heap.** You hand it one block of memory and it never asks for more.
  `IRIS_ARENA()` computes the size at compile time. Measured zero allocator
  calls across record / train / 100,000 predictions / delete / retrain.
- **Deterministic.** Same seed and same data give the same bytes, and it is a
  stated contract with three layers of enforcement: `#error` on `-ffast-math`,
  an FP-contraction pragma, and golden output hashes pinned as live test
  assertions. Verified across 16 of 20 optimisation and contraction flag
  combinations — the four that fail are exactly `-ffp-contract=fast`, which the
  header documents as the case clang ignores the pragma for.
- **The examples are the interface.** Every demonstration is individually
  listable, auditionable and deletable, because that is how practitioners
  actually repair these models — Fiebrink's 2011 study found composers never
  once used cross-validation; they deleted the bad take and recorded it again.

## Run the tests

```sh
sh build.sh audit     # 35 correctness checks, including the golden hashes
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
| `iris_sink.h`, `iris_source.h` | The two port interfaces — where sound goes out and sensors come in |
| `examples/` | `01_hello.c` teaches three gestures; `02_fix_a_mistake.c` is the repair loop |
| `ports/` | Output ports: MIDI CC (the default), MPE, OSC, null, a template, a wasm shim |
| `tests/` | The audit suite and the frozen golden fixtures |
| `experimental/` | L-BFGS. Not in the core, no production callers — read its header before citing it |
| `docs/` | Design notes, architecture decision records, and negative results |
| `bench/` | The same core compiled to wasm, running in a browser |

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

## Licence

BSD 3-Clause. See `LICENSE`.
