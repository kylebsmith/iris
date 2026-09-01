# Changelog

Versioning: the library version is declared once, in `iris.h`
(`IRIS_VERSION_STRING`). Nothing else states one.

**The save-file format version (v1/v2/v3) is a separate axis.** It carries the
input-scaling semantics. A library bump never invalidates a saved instrument;
the loader keeps reading every older format, permanently. Only a format bump
changes what a file means, and `docs/adr/0006` and `0018` govern that.

## 0.1.0 — 2026-08-31

- **A note on the mutation-testing figures in this repository's git history.**
  Any mutation score quoted in a commit message before the commit "tools: the mutation harness has been reporting a fake score" was produced by
  a harness that was not measuring anything. It decided kill-or-survive by
  searching its own output for the word "failing", and the regression suite
  printed "0 of 16 failing" on every run, so every mutation was recorded as
  killed. Proved by mutating a word inside a comment: reported "killed".
  Fixed to decide by exit code, at which point the honest score was 70%.
  A second false-scoring mechanism was found later the same day and fixed in "tools: the mutation harness was scoring 100% by detecting
  nothing, again": the kill test included the documentation-consistency check, which
  compares the header's line count against a number in the README, so any edit
  to the source failed it and killed every mutation for a reason that had
  nothing to do with behaviour. The kill test now runs only the suites that
  test behaviour. The figures from that commit onward are real; the earlier ones
  are not, and the commits are left in place rather than rewritten because a
  record that corrects itself is worth more than one that looks clean.

- Internal helpers renamed from `iris__x` to `iris_internal_x`. C99 reserves
  every identifier containing a double underscore for the implementation, so
  the old names were formally undefined behaviour in a library that claims to
  build anywhere. Six names, thirteen files. Renamed in the dated audit records
  and decision records too, so the repository has one name for one function
  rather than a split you have to know about. No behaviour changed: the golden
  hashes are bit-identical and the mutation score is unmoved at 18 killed of 20, 90%.

**This is the first release. Nothing before it was ever published.**

The version numbers that appear in this project's git history — 0.2.0, 0.3.0,
0.4.0 — were development bookkeeping on a tree with no remote and no users.
They are archaeology, kept in the archive, and renumbering them to a single
0.1.0 is the honest description: one release, containing everything below.

The SAVE FILE format version is a separate axis and is NOT renumbered. It is at
5, it has been at 1, 2, 3 and 4 during development, and every one of those still
loads — that promise is real even though no file was ever in anyone's hands.

---

### What this release contains

### An independent blind audit, and the two bundles of fixes it produced

An auditor was given the code and the public promises and nothing else — no
sight of the test suite, no decision records, no changelog — and wrote its
torture tests before reading a line of the source. It found twenty-three
defects. Every one was re-verified here before anything was changed.

**Memory safety (no behaviour change, both golden hashes unmoved):**

- `k->order`, the trainer's shuffle buffer, was a pointer into memory nobody
  had written. Recording a demonstration during a sliced training run made the
  shuffle read *and write* one slot past what `iris_train_begin` had filled: a
  segmentation fault on a dirty arena, and on a zeroed one — which is what a
  global array on a microcontroller is — the new demonstration was silently
  ignored while the instrument reported seven demonstrations, a healthy status
  and a small error. Reachable from the exact user-interface loop the library
  documents. `iris_init` now fills it, and the trainer rebuilds it whenever the
  demonstration count changes under a running slice. The plateau reference is
  reset with it, because new data makes the error jump and a stale reference
  reads that rise as convergence. The epoch counter is deliberately *not*
  reset, so a caller who records between every slice still reaches the ceiling.
- The arena was aligned *after* the structure was placed, not before, so the
  structure itself was left wherever the caller's buffer began. A sanitizer
  reports it; a chip that faults on unaligned loads would crash.
- Fifty-six public functions dereferenced a null instrument. Every one now
  refuses, using that function's own failure convention rather than a blanket
  zero.
- Removed a store that was never read.

**Build and compiler promises (no behaviour change):**

- **Determinism did not hold on the library's own target.** GNU compilers
  ignore the standard contraction pragma, and in GNU mode — which is what the
  Arduino development environment builds with — they contract by default. The
  same seed and the same demonstrations produced a *different instrument*. A
  file-scope `#pragma GCC optimize ("fp-contract=off")` fixes it: twelve builds
  across three compilers and three optimisation levels now agree exactly.
- `-ffinite-math-only` is not `-ffast-math` and did not trip the tripwire, but
  it deleted every guard in the library: `iris_isbad` folded to false, poisoned
  demonstrations were accepted, and a broken sensor produced a plausible number
  and a healthy status. The tripwire now catches it, and `iris_isbad` reads the
  bits through a `volatile` the optimiser may not reason across.
- "Zero external symbols" was false on GNU compilers: `sqrtf` for a negative
  branch that never occurs, and `memset` from `iris_reseed`'s zeroing loop. Two
  flags fix both and neither changes an output bit. `check-claims.sh` now tests
  **every** compiler it can find, and a compiler it cannot build for is a
  failure rather than a silent skip — which is how this passed while being false.
- **The guards A/B check could not fail.** It asserts two builds are equal,
  which is also what you get when the flag does nothing: renaming
  `IRIS_NO_GUARDS` in the header left it passing. It now carries a positive
  control — a poisoned demonstration, which the two builds must handle
  *differently* — and that control correctly fails on the sabotaged header.

**Behaviour and format (one deliberate change, and one constant RESTORED):**

- **A sensor channel that never moved killed the instrument, above 32.** The
  guard against a zero-width range widened it by an absolute `1e-6`, and in
  32-bit floating point adding `1e-6` to anything above 32 changes nothing at
  all — so the range stayed zero, the normalisation divided zero by zero, and
  every prediction became not-a-number. A light sensor reads 0..4095 and a
  distance sensor reads millimetres, and `TASKS.md` (in the ESP32 starter kit)
  sends students at exactly
  those. Measured: worked to 31.77, dead from 32.72. The floor is relative now.
  The golden hashes did not move: the frozen data has no constant channel.
- `iris_init` accepted hidden widths below 8 while `iris_size` returned its
  "impossible shape" answer for them, so `malloc(iris_size(2,4,3,64))`
  allocated nothing. `docs/FREEZE.md` had said "raise the floor to 8 in
  iris_init. **NOW.**" since before this release; done. The audit's reroll
  corner moved from width 4 to width 8, the lowest legal one, and both of its
  thresholds still hold with headroom.
- `iris_suggest_smoothing` restored the smoothing SETTING but not the weights,
  so a performer who called it to ask a question silently got a different
  instrument — a 600-epoch refit in place of one trained to a plateau, drifting
  one output by 0.14 of full scale. It now saves and restores, and takes
  scratch space to do it. It refuses without that space rather than proceeding:
  refusing an answer is recoverable, replacing someone's instrument is not.
- **Format 5**: format 4 plus one float, the smoothing setting. It was the last
  unfinished item on `FREEZE.md`'s "cheap today and impossible tomorrow" list.
  Formats 1 through 4 still load; a v4 file was hand-built and verified to load
  and predict bit-identically, reporting smoothing 0, which is honest — v4 never
  recorded it.
- Documented which files are checksummed and which are not. An instrument
  restored from a v1 or v2 file keeps the legacy scaling for life and therefore
  saves itself as v2, which has no checksum — permanently, for that instrument.
  `iris_input_scaling` reports the same bit.

**The `[-1,+1]` golden constant is RESTORED to `0x6805FB0D`, its original
v0.3.0 value.** It had been re-pinned to `0x123FD0C8` when format 4 added a
trailing checksum — but that re-pin was never necessary. The test reduces a
saved file to a "v1-layout view" precisely so format bumps cannot move the
hash, and that helper only stripped the v2/v3 word; every later format dragged
its new trailing bytes into the hash. It now strips all of them, and the
original constant came straight back. Verified alongside: predictions over a
441-point grid are bit-identical across the whole format change.

That is the more useful lesson of this release. A hash that moves for
bookkeeping reasons teaches you to re-pin without asking why, which is exactly
what a frozen contract must never teach.

**Silent failures now report (additive, both golden hashes unmoved):**

- `iris_clear` did not end a training run in flight, so the documented
  slice loop `while (iris_train_slice(k, 500))` never terminated: the trainer
  returns at once with no demonstrations, so the epoch counter never advanced
  and the run never finished. Both Arduino sketches wire clear to a button and
  the library recommends slicing, so those two were one press apart.
- `iris_retrain_new(k, seed, 0)` reseeded first and refused second, so a caller
  got a refusal AND lost their instrument. It now checks what the trainer will
  refuse before destroying anything.
- The closed-form trainer reported success while collapsing every gesture to
  one sound under a large enough ridge or a zero gain. It now measures whether
  the model still separates the corners of the demonstrated input range and
  sets `IRIS_DIVERGED_STUCK` if it does not. Measured in NORMALISED output
  space, because comparing against the raw demonstrated range let a single
  outlier report an honest solve as collapsed; and skipped entirely when every
  demonstration carried the same sound, because a constant is then correct.
  It reports rather than refusing: this trainer's stated property is that it
  always produces something on hostile data, and the audit asserts that.
- `iris_classify_1nn` started its search at demonstration 0, so a query
  containing a not-a-number made every comparison false and it returned the
  FIRST demonstration and its identifier as a confident answer, with a healthy
  status. For a classifier that is a wrong class every time a sensor is
  unplugged. It now refuses, exactly as `iris_knn_predict` always has.
- A loaded instrument reported `iris_last_error` of 1.0 -- the worst possible
  value -- however well it had been trained, because the error is not in the
  file. The demonstrations are, so it is now measured at load: one forward pass
  each, once.
- Documented what `iris_get_status` covers. It reports numerical health, not
  argument mistakes; those come back through the return value. Making them
  sticky here would leave a polled interface showing a fault for ever after one
  out-of-range query.

### Memory safety: the arena bound could be absent entirely

`iris_init` accepted any `n_hid` from 1 upward; `iris_size` floors it at 8 and
returns `0` below that. The arena check was `bytes < iris_size(...)`, and on
unsigned types `bytes < 0` is false always — so for `n_hid` in 1..7 there was no
arena bound at all. `iris_init` with a one-byte arena and `n_hid = 4` returned a
live instrument, and training wrote **611 bytes past the end**.

Fixed by separating the two jobs that were conflated in one function:
`iris_internal_bytes()` is arithmetic with no opinion, and `iris_size()` is that plus
the quality floor. `iris_init` bounds the arena with the former, so the bound
applies to every shape it accepts. The quality floor is unchanged and still
belongs to callers choosing a shape.

Behaviour for valid shapes is unaffected: golden hash `0xFEFAEDF6` before and
after, full audit green. A right-sized arena is accepted, one byte short is
refused, and the `n_hid = 4` overrun is refused.

### The golden fixtures could destroy themselves

`build.sh golden` regenerated **both** frozen baselines. `v1` came back
faithfully, because a legacy writer still exists. `v3` did not: `iris_save`
writes whatever the current format is, which is now v4, so every run overwrote
the v3 backward-compatibility fixture with a v4 file — deleting the only
artifact proving a v3 instrument still loads, and turning the audit's v3 check
into a failure that reads like a code defect. It happened on 2026-08-27 and was
recovered from git.

A fixture whose purpose is to be *old* cannot be rebuilt by the new code. It is
a historical artifact: preserved, never regenerated. `make_golden` now writes
only the v1 pair, and `build.sh golden` refuses to run at all unless
`IRIS_REGENERATE_GOLDEN=yes-i-mean-it` is set, because a command that redefines
what "correct" means should not be one keystroke from a command that checks it.

### Also

- Include guards were still spelled `EMBWEK_*` after the rename. Now `IRIS_*`,
  verified inert by the same hash. `check-claims.sh` fails if they come back.
- The masthead comment said `v0.3.0` while `IRIS_VERSION_STRING` said `0.4.0`.
  The version check only ever counted the macro, so nothing noticed.
  `check-claims.sh` now compares them, and I confirmed the check fails when the
  two disagree rather than assuming it would.
- Removed a dead `#ifdef EMBWEK_IO_H` guard against a header that exists
  nowhere in the tree.
- `build.sh` usage omitted `claims`, the target CI depends on. Added, along with
  a `tiny` target — `docs/tiny.c` was reachable from no build target, no CI job
  and no document, which is why its self-reported line count had been wrong by
  43 lines without anything catching it.
- Added `library.properties` so the repo installs as an Arduino library and its
  examples appear under File → Examples, with `examples/iris_smallest/`.


Renamed from `embwek` to `iris`. Verified semantically inert: every golden
output hash is byte-identical across the rename (`0xFEFAEDF6`, `0x6805FB0D`,
guards blob `0x0FEC913D`).

### Fixed — behaviour

- **The divergence trap.** A contradictory demonstration could diverge the
  weights; the guard clamped them and stopped. On the next fresh run one pinned
  weight tripped the guard again on epoch 1, so training silently did nothing —
  forever — while reporting a healthy status. Deleting the bad example did not
  help. Training now refuses with `IRIS_DIVERGED_STUCK`; `iris_retrain_new`
  recovers the instrument. Pinned by audit check 35.
- **Playing an instrument that was never fitted** ran the forward pass over
  random weights and returned plausible numbers with no symptom. Now returns the
  centre of the demonstrated range with `IRIS_NOT_FITTED`. Guards on a new
  `fitted` flag rather than `trained`, so a stale-but-real instrument keeps
  playing — the property audit check 13 protects.
- **NaN/Inf is refused at `iris_record`** instead of being accepted silently and
  blocking every later training run. The trainer's pre-scan remains as defence
  in depth for examples arriving via `iris_load`.
- **`iris_train_epochs(k, 0)`** reported a freshly randomised network as trained
  with a perfect fit. Now refuses.
- **One refusal convention.** A train call that did no training returns `-1.0`.
  Previously the SGD path returned the *previous* run's error, so a caller could
  not distinguish refusal from repetition.
- **Capacity overflow.** `iris_size` multiplied capacity by dimensions with no
  overflow test; on a 32-bit target a large enough capacity wrapped, the arena
  check passed, and the store ran off the end of the caller's buffer. Bounded by
  `IRIS_MAX_EX`.

### Fixed — L-BFGS (experimental)

- **Relative curvature test.** The safeguard was an absolutely-scaled
  `sTy > 1e-12`; at this problem's gradient scale it discarded a median of ~680
  of 1000 curvature pairs, leaving the limited-memory history mostly empty.
- **Double-precision loss accumulation.** In float32 the Armijo test was
  resolving rounding noise, halving the step ~10 times per iteration.
- Effect: the training-error gap against SGD fell from 3.8× to 1.61×. The golden
  L-BFGS hash was deliberately re-pinned; the core SGD hash is unchanged.

### Added

- `iris_train_epochs_done()` — how many epochs actually ran. There was no way to
  distinguish a completed run from an early stop.
- `IRIS_DIVERGED_STUCK`, `IRIS_NOT_FITTED` status codes.
- CI: audit on {ubuntu, macos} × {gcc, clang}, a zero-external-symbols check,
  and golden hashes across five optimisation levels.
- `LICENSE` (BSD 3-Clause). The project previously shipped none.

### Changed — documentation honesty

Audit check 33 no longer asserts that SGD beats L-BFGS; it asserts the plateau
property and *reports* the ratio, because the original gap was mostly our own
defects and the assertion did not survive repairing them.

The host→ESP32-S3 scaling in the cost tables was ×32; the one on-device
measurement puts it near ×270. At 20 examples the table implied ~29 ms where the
board measures 321 ms. Corrected, columns relabelled, footnoted.

Corrected claims that were false where they stood: a comment saying `iris_load`
calls the legacy-scaling function (it does not); the warm-start banner claiming
the firmware uses it (removed from the device 2026-08-22); `iris_migrate_scaling`
claiming an app dependency that does not exist; a prescribed idiom with no
caller; an ELM gain figure citing a sweep no longer in the tree; the Padé tanh
accuracy claim (0.001 → 0.0283, wrong by 28×); and the algorithm count, settled
from `LearningAlgorithmRegistry.java` at nine, of which we implement two.

## Earlier

Developed as `embwek` from August 2026. Prior history, the literature corpus,
the audits and the decision log live in the private research repository.
