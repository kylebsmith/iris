# Changelog

Versioning: the library version is declared once, in `iris.h`
(`IRIS_VERSION_STRING`). Nothing else states one.

**The save-file format version (v1/v2/v3) is a separate axis.** It carries the
input-scaling semantics. A library bump never invalidates a saved instrument;
the loader keeps reading every older format, permanently. Only a format bump
changes what a file means, and `docs/adr/0006` and `0018` govern that.

## 0.4.0 — 2026-08-27

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
