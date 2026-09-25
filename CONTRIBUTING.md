# Contributing

How to change iris without breaking it. The library's design is in
[README.md](README.md), [`iris.h`](iris.h) and [`docs/`](docs/README.md); this
file is about what the library promises, how the tests hold it to that, and the
rules for anything you write here.

## What the library promises

A saved instrument belongs to the person who trained it. From 0.2.0 on, the
promise is split by what a change would do to that instrument (the README
states it in full, under
[What is promised, and what is not](README.md#what-is-promised-and-what-is-not)):

- **Playing a saved instrument is frozen** within a major version: the input
  and output scaling, the nonlinearity, the clamp to the demonstrated range,
  the nearest-neighbour paths, and what every byte of a format 7 file means.
- **Training may get better** in a minor release, if the change is measured to
  be better and announced in [CHANGELOG.md](CHANGELOG.md).
- **The functions follow semantic versioning**, and anything named
  `iris_internal_` is outside the promise.
- **The file format has its own number**, and every reader from format 7 on is
  kept.

The tests hold each promise with golden hashes: hashes of the exact bits a
fixed recipe produces, compared bit for bit, never within a tolerance. They come
in two sets, and they are treated differently.

| Set | What it pins | Where | May be re-pinned |
|---|---|---|---|
| Playback | What a saved file plays: `tests/golden/v7-instrument.bin` loads and plays the 625 predictions in `tests/golden/v7-expected.txt`, bit for bit | `tests/load.c` | Only with a new major version and a new file-format number; before 1.0, only to fix a measured defect, with a migration note |
| Training | How training reaches its weights: `0x6805FB0D` (`tests/audit.c`), the starter kit's `0xB7FC47A0` and `0x203834ED` (`tests/starter_recipes.c`), and the closed-form solves (`tests/elm.c`) | as listed | In a minor release, in the commit that changes training, with a changelog entry |

The playback file pins `iris_predict`. The nearest-neighbour paths and
`iris_novelty` have no golden file yet, so a change to what they play is caught
only by the checks in `tests/audit.c` and `tests/playing.c`.

If a change moves a hash you did not mean to move, you have made a mistake:
revert it. If you mean to change training, the pull request says so, re-pins
the training hashes in the same commit, and adds a CHANGELOG.md entry under
"Behaviour changes" with the old and new values and the measured effect. A
training change ships only if it is measured to be better than a reroll of the
seed on the same data, which is the noise floor; one that is not goes into
[`docs/negative-results/`](docs/negative-results/) with its numbers. A playback
change is a new major version.

Changes that move no hash are free: anything additive to the interface,
diagnostics, messages, documentation, the output ports in `extras/`, and the
starter kit's sketches.

## Commands

```sh
sh build.sh           # every host check that needs only a C compiler
sh build.sh test      # the same
```

It stops at the first failure and names the check. The checks one at a time:

```sh
sh build.sh audit          # the correctness checks and the golden hash 0x6805FB0D
sh build.sh regressions    # one test per fixed defect
sh build.sh coverage       # the refusal paths
sh build.sh load           # the save format, and the playback golden file
sh build.sh train          # the trainers' state logic, byte by byte
sh build.sh elm            # the closed-form trainer
sh build.sh playing        # the playing and neighbour paths
sh build.sh portability    # the square root and the other C-library stand-ins
sh build.sh recipes        # the starter kit's two recipes
sh build.sh tu             # two translation units with different maxima
sh build.sh fuzz           # random call sequences under AddressSanitizer
sh build.sh examples       # every example, with -Werror, run
sh build.sh sanitize       # every C test under AddressSanitizer and UndefinedBehaviorSanitizer
sh build.sh threads        # ThreadSanitizer, with a positive control
sh build.sh noheap         # zero allocator calls, with a positive control
sh build.sh freestanding   # zero undefined symbols, and the ESP32-S3 list
sh build.sh targets        # every target builds warning-free; x87 is refused
sh build.sh pragma         # the contraction pragmas stay inside iris.h
sh build.sh fuzz-load 300  # libFuzzer on iris_load for 300 seconds (clang)
sh build.sh determinism    # the golden hash across -O levels and contraction
sh build.sh cov            # line and branch coverage, with thresholds
sh build.sh mutate         # advisory mutation run; reports, never fails
sh build.sh reference      # the Python double-precision reference (numpy)
sh build.sh mpe            # the polyphonic-expression output port
sh build.sh sinks          # the control-change and Open Sound Control ports
sh build.sh tiny           # docs/tiny.c against the library
sh build.sh bench          # rebuild the browser benchmark
sh build.sh clean          # remove build/
```

AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer are compiler
instrumentation that stop a program at an out-of-bounds access, at an operation
C leaves undefined, and at two threads touching the same memory. libFuzzer is
clang's coverage-guided fuzzer.

Prerequisites: a C99 compiler (C as standardised in 1999) for everything
`sh build.sh` runs by default; clang with libFuzzer for `fuzz-load`; clang's
coverage tools (`llvm-cov`) for `cov`; Python for `mutate`, and Python with
numpy for `reference`; the cross-compilers (the ESP32-S3 Arduino core,
`arm-none-eabi-gcc`, avr-gcc) for the cross-target rows of `freestanding`,
`targets` and `pragma`; and Docker with a Debian image holding gcc and clang,
named in `IRIS_LINUX_IMAGE`, for the Linux rows of `freestanding`. A row whose
tool is missing prints SKIP, and a SKIP is not a pass for that target.

## The rule: prove every check can fail

A check that has never been seen to fail is decoration. When you add one,
break the behaviour it guards in a scratch copy of the code, watch the check go
red, and restore. Say in the commit body, in one line, what you broke and what
the check reported.

Prefer checks that encode as little of your own expectation as possible. A
test written by the author of the code it tests tends to agree with that
author; a sanitizer, a fuzzer, an exhaustive comparison with the hardware, or a
positive control that must be detected, decides without asking you.

## Traps that are still real

- **The shape exists in three places:** `IRIS_ARENA(...)`, `iris_init(...)`,
  and your own array declarations. C will not warn you when they drift apart.
  `iris_predict` writes one float per output into whatever you hand it.
- **Unsigned comparisons.** `iris_size` returns 0 for a shape it refuses, and
  `bytes < iris_size(...)` is then `bytes < 0`, which is false for ever on an
  unsigned type: the bound silently disappears. Test for 0 first. `iris_init`
  bounds the arena through `iris_internal_bytes` for this reason.
- **Structure sizes differ by architecture.** `struct iris` is 248 bytes on a
  64-bit laptop and 164 on the ESP32-S3, so `IRIS_ARENA(2,12,1,8)` is 1,024
  bytes on the laptop and 940 on the chip (Apple clang and
  `xtensa-esp32s3-elf-gcc`, read from the symbol sizes). Measure on the target;
  never quote one as the other.
- **The starter kit carries copies of `iris.h`**, one in each sketch folder, so
  students need no install step. After changing the header, run
  `sh sync-iris.sh` in the starter kit, or its copies go stale.
- **Your own arithmetic contracts.** The header switches fused multiply-add off
  for its own code only. A test or sketch that computes its demonstrations and
  must reproduce a pinned hash switches contraction off in its own code too, as
  `tests/audit.c` and `tests/starter_recipes.c` do.

## What goes where

| Path | What |
|---|---|
| [`iris.h`](iris.h) | The whole library. Start at the top. |
| [`examples/`](examples/) | `00_minimal.c` is the smallest one; `iris_smallest/` is the Arduino one. |
| [`tests/`](tests/) | The test programs and the playback golden file. |
| [`tools/`](tools/) | Programs the tests use. |
| [`extras/ports/`](extras/ports/) | Where sound goes out: control change, polyphonic expression, Open Sound Control. |
| [`docs/`](docs/README.md) | Design notes, decision records and negative results; [`docs/README.md`](docs/README.md) says which describe the current library. |

- **`iris.h` is the core.** It has no dependencies and no allocation, and both
  are checked. A change that adds either will be rejected.
- **New output formats go in `extras/ports/`.** Copy
  `extras/ports/template/` and implement the sink interface in
  `extras/iris_sink.h`. Nothing above the port layer may know what your
  hardware is.
- **New sensors go behind `extras/iris_source.h`.** Same rule in the other
  direction: the core must not be able to tell what produces the numbers.
- **Research goes elsewhere** until it has a caller and a measurement.
  Negative results go in `docs/negative-results/`: things that did not work are
  worth keeping, and not in the engine room.
- **No dead code, no commented-out code.** If it is worth keeping, it is worth
  a negative-results note saying what it measured.

## Writing anything a person will read

- **No bare acronyms.** Spell the term out and explain it where it is first
  used; the header's glossary is the model. The audience includes musicians and
  first-year students. This is not a request to simplify the content.
- **Comments explain the code as it is, and why.** Present tense, about the
  code. No "this used to", no dates, no account of how a problem was found, no
  sentences about other sentences. A defect belongs in a regression test,
  described as what the test guards; the history belongs in the commit message.
  If a comment is wrong, fix it in the same commit as the code it describes.
- **Every number names its source**: the test, the file, the program or the
  recipe that reproduces it, or it says it is an estimate and from what. A
  hardware figure names the board, the sketch and the toolchain, and a board
  figure without a recorded log says so. "Measured" on its own is not a source.
- **Say it once, next to the code it governs**, and point to it from
  elsewhere. Do not repeat counts of files, checks or lines in prose.
- **No marketing voice, no platitudes, no hedging.** Say the thing, and state
  its limits. Do not claim the library is better than, faster than or first at
  anything.
- **Zero dependencies is a promise about `iris.h` only.** The sketches use
  Adafruit's display drivers and that is fine: a display driver is replaceable
  plumbing, whereas the code deciding how a gesture becomes sound is not.

The header's comment density is deliberate: it is a teaching text as much as a
library, and the prose carries measurements and reasoning that would otherwise
be lost. Explain *why*, not *what*.

## Commit messages

- A subject in the imperative, under 72 characters, describing the change to
  the code or to what a user sees: "Validate every field when loading a saved
  instrument", "Correct the ESP32-S3 prediction time in the README".
- A body, a few short paragraphs, saying what changed and why, with the
  measurement that justifies it and the command that reproduces it, and one
  line on how each new check was seen to fail.
- No account of how the problem was found and no process vocabulary. A
  correction is a plain fix.

## Before you open a pull request

1. `sh build.sh` passes: every check, the golden hashes unmoved, or moved on
   purpose as described above.
2. If you touched `iris.h`: `sh build.sh sanitize`, `threads`, `noheap`,
   `freestanding`, `targets`, `pragma` and `determinism` pass, and
   `sh build.sh fuzz-load 300` finds nothing if you touched the save format.
3. Zero warnings with `-std=c99 -Wall -Wextra` on clang and GCC (the GNU
   Compiler Collection), as C and as C++.
4. Every new check was seen to fail, and the commit says how.
5. If `iris.h` changed: `sh sync-iris.sh` in the starter kit.
6. If a sketch changed: compile it with `--warnings all` and confirm no
   diagnostics from our own files.
7. Report what happened, including any step you skipped.

## Reporting a defect

Include the compiler and its version, the optimisation level, the board and
core if it happened on hardware, the iris version, and, for a difference in
behaviour, the seed and the demonstrations. Determinism is a promise here, so a
reproduction should reproduce exactly.
