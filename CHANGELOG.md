# Changelog

The library version is `IRIS_VERSION_STRING` in `iris.h`; `library.properties`,
`CITATION.cff` and the release tag state the same number. The save-file format
has a number of its own, which changes only when the bytes of a file or their
meaning change. What each part of a version number permits, and what is
promised about saved instruments, is in
[README.md](README.md#what-is-promised-and-what-is-not).

| Library | Writes format | Reads formats |
|---|---|---|
| 0.2.0 | 7 | 7 |
| 0.1.0 | 5 (6 for an instrument never fitted) | 1 to 6 |

## 0.2.0 — unreleased

The first release whose saved files are promised to keep playing: format 7 is
read by every later release, and what a saved instrument plays is frozen within
a major version. Training may still improve in a minor release, measured and
announced here, with the training hashes re-pinned.

The golden training hash `0x6805FB0D` is unchanged, and so are the starter kit's
two recipes (`0xB7FC47A0` and `0x203834ED`, now pinned in
`tests/starter_recipes.c`): the recipes those hashes pin train bit for bit the
same in 0.1.0 and 0.2.0.

### Breaking changes

Interface:

- `iris_train_epochs` is renamed `iris_continue`, and `iris_train_converge`
  is renamed `iris_continue_to_plateau`. Both carry on from the current
  weights; after deleting a take, call `iris_train`.
- `iris_retrain_new` is removed. A reroll is `iris_reseed(k, seed)` then
  `iris_train(k)`, or then `iris_train_elm` for the closed-form trainer.
  `iris_retrain_new(k, seed, n)` on a store it accepts is exactly
  `iris_reseed(k, seed)` then `iris_continue(k, n)`.
- `iris_correct` and `iris_retrain_elm_new` are removed.
- `iris_input_scaling` and `iris_migrate_scaling` are removed with the [0,1]
  input scaling (below).
- `iris_train_elm_ex`, `iris_fit_ranges` and `iris_get_l2`, and seventeen
  helpers that were public by accident (`iris_isbad`, `iris_tanh`,
  `iris_sigmoid`, `iris_sqrt`, `iris_absf`, `iris_clampf`, `iris_rand_u32`,
  `iris_rand_sym`, `iris_norm_in`, `iris_norm_out`, `iris_denorm_out`,
  `iris_forward_norm`, `iris_shape_fits`, `iris_zero_velocity`, `iris_artanh`,
  `iris_logit`, `iris_crc32`), are renamed `iris_internal_*`. Anything with
  that prefix is outside the interface and may change in any release. The
  interface is the 41 functions listed at the top of `iris.h`.
- `iris_predict`, `iris_knn_predict` and `iris_classify_1nn` take `iris *`,
  not `const iris *`: they write the status, and the neighbour functions fit
  the ranges of an instrument never fitted.
- `iris_train_begin` now reseeds from the instrument's seed, as `iris_train`
  does, so a sliced run is bit-identical to `iris_train`.
- `iris_suggest_smoothing` needs `IRIS_ARENA(n_in, n_hid, n_out, cap)` bytes of
  scratch, the size of the instrument's arena, instead of `iris_save_size`
  bytes, and ranks settings by each output's miss divided by its demonstrated
  range.
- `IRIS_ELM_SCRATCH` is larger (1,295 bytes at 12 hidden units and 3 outputs
  with the default maxima, from 884): it holds the ranges kept in case the
  solve fails, one frozen hidden unit, and padding so the buffer may start at
  any address.
- A collapsed closed-form solve reports the new status `IRIS_SOLVE_COLLAPSED`
  (7) instead of `IRIS_DIVERGED_STUCK`, which now means only a diverged
  gradient run.
- The header refuses to compile when float arithmetic is carried in a wider
  format (`__FLT_EVAL_METHOD__` other than 0, 16 or 32), as on 32-bit x86 using
  the x87 unit, its old 80-bit floating-point unit: there every instrument came
  out different with no diagnostic.
  Build with `-msse2 -mfpmath=sse`.
- The contraction pragmas now end with the header, so your own code after the
  `#include` contracts `a*b + c` as your compiler's default says. A program
  that computes its demonstrations itself and needs the same bits from two
  compilers must switch contraction off in its own code, as `tests/audit.c`
  does.

Save format:

- `iris_save` writes format 7 and `iris_load` reads format 7 only. Every field
  is little-endian and moved one byte at a time, so the buffer needs no
  alignment and a file means the same on every machine. The magic is `IRIS`.
  A flags word records `fitted` and `trained` separately. Every rule is checked
  before anything is written, and a refused load leaves the instrument and its
  status exactly as they were. The same instrument is 8 bytes larger than in
  format 5. Formats 1 to 6 are not read; to keep a 0.1.0 file, convert it once
  with a program built against the 0.1.0 tag. The `IRIS_FORMAT*` and
  `IRIS_MAGIC` macros are replaced by `IRIS_FILE_MAGIC`, `IRIS_FILE_VERSION`
  and `IRIS_FILE_HEADER`.

Removed:

- The [0,1] input scaling that instruments restored from format 1 and 2 files
  kept for life, with `in_center`, its golden hash `0xFEFAEDF6` and its
  fixtures. Every instrument scales its inputs to [-1, +1].
- `experimental/iris_lbfgs.h`, the limited-memory quasi-Newton trainer (L-BFGS,
  the Broyden–Fletcher–Goldfarb–Shanno method with a bounded history), with its
  checks and hashes. It had no caller outside the tests.

Behaviour changes on the playing path:

- An input that did not move across the demonstrations (width at most 1e-5 of
  its magnitude, or 1e-6) is ignored in training and playing. It used to become
  a gain of 1e5 to 1e6, so one count of movement at play time turned the
  instrument into a constant.
- An instrument never fitted plays the centre of the demonstrated output range,
  or 0 with no demonstrations, instead of the centre of whatever ranges it held.
- `iris_knn_predict` returns the stored value exactly when every neighbour
  agrees (a label of 3 used to come back as 2.9999998 on 29% of queries), and
  never leaves the demonstrated range.
- `iris_delete_nearest` measures distance in fractions of each input's range,
  as the neighbour functions do, so it deletes the take `iris_classify_1nn`
  names.
- The neighbour searches accept a finite query however far outside the
  demonstrations it is.

### Fixes

- `iris_load` wrote past a working array when a translation unit compiled
  with a smaller `IRIS_MAX_IN` loaded a larger instrument.
- Saving an instrument that had recorded a take since its last training
  marked it never fitted, so it came back silent after a load.
- `iris_load` accepted any content with a valid checksum: not-a-number weights,
  inverted or overflowing ranges, non-finite demonstrations, repeated or
  non-positive identifiers, a `next_id` at the top of the integer range, a
  not-a-number smoothing value.
- Saved files depended on the host's byte order and read the caller's buffer
  through pointer casts that need 4-byte alignment.
- Two refused loads wrote the status.
- A load kept the receiving arena's momentum velocities, learning rate and
  momentum; it now zeroes the velocities and restores the defaults.
- `iris_train`, `iris_loo_error` and the reroll calls reseeded before finding a
  non-finite demonstration, so a refusal destroyed the instrument;
  `iris_loo_error` returned a not-a-number where it promised -1.
- The stuck-divergence refusal fired only on every second warm call, and any
  call that overwrote the status let a warm run through; it now keys on the
  pinned weights and holds on every call.
- The closed-form trainer overwrote the hidden layer before its solve could
  fail, so a bad `lam0`, a bad gain or a failed factorisation destroyed a
  fitted instrument; it now validates first and solves in scratch.
- The closed-form trainer ignored smoothing; it now adds 1.2 times the
  smoothing to the ridge on the output weights, and at smoothing 0 the solve
  is bit-identical to before.
- The closed-form trainer left the previous run's worst-demonstration ledger,
  its training-progress fields and any sliced run in place, reported
  `IRIS_DIVERGED_STUCK` for constant outputs and for takes all made at one
  gesture, and needed a float-aligned scratch buffer.
- `iris_suggest_smoothing` restored the instrument through a save and a load,
  which lost the momentum velocities and silenced a stale instrument; it now
  restores every byte of the arena.
- A refused query on an instrument never fitted still fitted its ranges in
  the neighbour paths.
- `iris_record` could overflow a signed integer handing out the last
  identifier, reachable through a loaded file; it now refuses once
  identifiers run out.
- The square root called the C library's `sqrtf` on the ESP32-S3 and, for
  negative inputs, on GCC and Linux clang, so freestanding builds had an
  undefined symbol. It is now computed in integers and correctly rounded,
  checked against the hardware for all 2^32 inputs.
- On the ESP32-S3, `iris_suggest_smoothing` copied its table of settings
  with `memcpy`; the table is now `static const`.
- gcc-15 warned that `bi` may be used uninitialized in `iris_knn_predict`
  at `-O2` and `-O3`.
- The range searches and the neighbour searches started from sentinel values
  that very large demonstrations or queries could exceed.
- `examples/00_minimal.c` read the status and recorded its bad frame inside one
  `printf`, whose argument order C leaves open, so it printed status 0 on some
  compilers.
- `docs/gain-sweep.c` did not compile, and `docs/tiny.c` used an activation
  that differs from the header's.

### Additions

- Tests: `tests/load.c` (every rule of the save format, round trips, a
  committed format 7 instrument in `tests/golden/` played back bit for bit),
  `tests/fuzz_load.c` (a harness for libFuzzer, clang's coverage-guided
  fuzzer, that feeds `iris_load` mutated files and repairs their checksums so
  the mutations reach the content rules), `tests/train.c`, `tests/elm.c`, `tests/playing.c`,
  `tests/portability.c`, `tests/starter_recipes.c`, and the scripts
  `tests/freestanding.sh`, `tests/targets.sh` and `tests/pragma_leak.sh`.
- `tools/sqrt_exhaustive.c` compares the header's square root with the host's
  for every 32-bit pattern.
- `sh build.sh` runs every host check and stops at the first failure; the
  README lists each check. The `claims`, `bloat`, `sketches`, `golden` and
  `experiment` arms are gone, with `tools/check-claims.sh` and
  `tools/bloat.sh`.

### Documentation

- The comments in `iris.h` describe 0.2.0: the whole interface in eight groups,
  a glossary of every term the file uses, the failure rules regenerated from
  the functions' behaviour, and every figure with its source. Figures that did
  not replicate are replaced or removed.
- The README, CONTRIBUTING.md, `docs/SYSTEM-technical.md` and
  `docs/SYSTEM-plain-english.md` are rewritten for 0.2.0, and
  `docs/README.md` says what each document in `docs/` is.
- The README states the compatibility promise and the versioning policy.

## 0.1.0 — 2026-09-01

The first release.

- `iris.h`, one header in C99 (the 1999 C standard) with no dependencies and no
  heap: a one-hidden-layer
  network trained by backpropagation with momentum, a rational approximation to
  tanh, outputs scaled to [0.1, 0.9], and inputs scaled to [-1, +1] (to [0, 1]
  for instruments restored from format 1 and 2 files).
- Trainers: `iris_train` (reseed, then train to a plateau), the sliced
  `iris_train_begin` / `iris_train_slice` / `iris_train_progress` /
  `iris_train_busy`, `iris_train_epochs`, `iris_train_converge`, `iris_correct`,
  `iris_retrain_new`, the closed-form `iris_train_elm`, `iris_train_elm_ex` and
  `iris_retrain_elm_new`, and an experimental L-BFGS trainer in
  `experimental/iris_lbfgs.h`.
- Playing: `iris_predict`, `iris_novelty`, and the nearest-neighbour
  `iris_knn_predict` and `iris_classify_1nn`.
- A store of demonstrations with stable identifiers, deleted by identifier,
  position, newest or nearest.
- Smoothing (`iris_set_smoothing`), `iris_loo_error` and
  `iris_suggest_smoothing`; the worst-demonstration ledger
  (`iris_example_stress`, `iris_worst_example`, `iris_worst_example_id`).
- Status codes for numerical health, including `IRIS_TRAINING_DIVERGED`,
  `IRIS_DIVERGED_STUCK`, `IRIS_NOT_FITTED` and `IRIS_STORE_FULL`, and
  `iris_train_epochs_done`.
- Save format 5 with a CRC-32 checksum, a cyclic redundancy check (format 6
  for an instrument never fitted); formats 1 to 6 read.
- Build guards against `-ffast-math` and `-ffinite-math-only`, and
  file-scope contraction pragmas.
- Tests: `tests/audit.c` with the golden hashes `0x6805FB0D` and `0xFEFAEDF6`,
  `tests/regressions.c`, `tests/coverage.c`, `tests/fuzz.c`, the two-unit test
  in `tests/tu/`, the guards comparison, and golden fixtures for formats 1 and
  3; continuous integration on Ubuntu and macOS with gcc and clang.
- `extras/`: output ports (control change, polyphonic expression, Open Sound
  Control, a null port, a template, a WebAssembly shim), the sink and source
  interfaces, and a browser benchmark.
- Arduino packaging (`library.properties`, `keywords.txt`,
  `examples/iris_smallest/`), four C examples, and the BSD 3-Clause licence.
