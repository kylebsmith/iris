# 0006 — Save format v2 is one added word; the v1 loader is permanent

**Status:** accepted, 2026-08-21 (decision made in experiment E4, before integration)
**Affects:** `iris.h` (`iris_save`, `iris_load`, `IRIS_FORMAT`), `tests/audit.c`
checks 11 and 15, `tests/golden/`.

## Context

Warm-start correction (ADR 0005) advances the rng stream past the seed. v1's
loader resets `rng.s = seed`, so a corrected instrument saved and reloaded
would take a *different* shuffle path on its next correction than the
in-memory instrument — same weights, quietly diverging futures. The file
needs to carry the live rng state.

At the same moment, four other candidates were knocking on the format: ELM
weights, k-NN state, anchor pseudo-examples, ensemble member seeds. Every one
was measured to need nothing:

- ELM-trained weights are v1-natives — bit-identical round trip through the
  existing arrays (E1, 441 probes).
- k-NN's whole model is the examples the file already carries (E7) — every
  old file gains the mode with zero migration.
- Anchors are transient by construction and must never persist (E5).
- Ensemble seeds belong to the optional module's own blob (E6).
- Optimizer state is never serialized (L-BFGS work is transient scratch;
  momentum velocity is zeroed at correction entry — the E4 spec fix).

## Decision

- `IRIS_FORMAT` bumps 1 → 2. **v2 = v1 + one uint32** (`rng.s`) inserted after
  the 8-word header. Nothing else enters the file in v0.2.
- `iris_save` writes v2. `iris_load` accepts both: `h[1]==2` reads the extra
  word; `h[1]==1` takes the existing path unchanged, `rng.s = seed` exactly
  as v0.1 did. **The v1 loader is permanent.** Fiebrink & Sonami's users
  lost technique to retraining; a frozen old model is sacred, and the loader
  is the vow. The audit holds a frozen 852-byte v1 file (`tests/golden/`)
  that must load and predict bit-identically, forever.
- A persisted algorithm-selector tag (MLP vs k-NN) and shuffle-buffer
  persistence are *deferred together* to the next format bump — each is a
  format event, and format events are batched deliberately.

## Rejected alternatives

**Keep writing v1 and accept the divergence.** Rejected: it breaks the
event-sourced determinism promise (ADR 0005) in exactly the case that
matters — an instrument that lives on the device across power cycles.

**A rich v2 (algorithm tag, trainer choice, optimizer state, ensemble).**
Rejected by measurement, above: every candidate either round-trips through
v1 already or must not persist. One word is the entire measured need, and
every extra word is a compatibility surface forever.

**Migrate v1 files to v2 on load (rewrite in place).** Rejected: loading is
a read. A library that silently rewrites the musician's files on open is a
library that one day destroys one.

## Consequences

- File grows by 4 bytes (928 vs 924 on the check-15 instrument).
- Save → load → `iris_correct` is now bit-identical to the never-saved path
  (check 15) — save/load is transparent to the correction chain.
- The golden v1 file predates the format bump and is never regenerated as
  v2; check 12's training-path hash is taken over v1-layout bytes so the
  golden constant survives the format change.
- Re-derive: `./build.sh audit` (checks 11, 12, 15). Measured on Apple M4
  Max (arm64), Apple clang 17.0.0, `-O2`, 2026-08-21.
