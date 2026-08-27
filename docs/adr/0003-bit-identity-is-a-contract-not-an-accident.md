# 0003 — Bit-identity is a contract, not an accident

**Status:** accepted, 2026-08-21 (decision made in experiment E8, before integration)
**Affects:** `iris.h` (pragma, tripwire, `IRIS_FLUSH`), `tests/audit.c`
checks 11–12, `tests/golden/`, port CFLAGS guidance.

## Context

"Same seed, same instrument" is the project's oldest promise, and check 5 has
always passed. E8 measured what the check never asked: the same source, same
seed, same recipe, compiled with three different fused-multiply-add contraction
settings on Apple clang 17 / M4, produces **three different weight blobs**:

| flags | saved-blob fnv1a |
|---|---|
| `-O2 -ffp-contract=off` | `0xFEFAEDF6` |
| plain `-O2` (clang default = contract on) | `0x1C284AC8` |
| `-O2 -ffp-contract=fast` | `0xF237919E` |

The integration session extended the matrix and found contract=on is not even
self-consistent across optimisation levels: `-O0/-O1` with contraction form a
*third* class distinct from `-O2`, because the optimiser changes which
expressions get contracted. Only contract=off is invariant across O0/O1/O2.
The divergence is musically nil (≤4 ulp, ≤2.4e-7) and bitwise fatal — for
golden-blob CI, for cross-platform identity, and for format v2's rng-state
semantics, which assume a reloaded instrument continues *exactly*.

A second, stranger hazard: momentum velocities never reach zero on IEEE hosts
— they get stuck at denormal minimum (49,477 of 50,000 iterations in E8's
trace) — while the ESP32-S3's LX7 FPU flushes denormals to zero in hardware.
A permanent, silent host-vs-device state divergence that no single-platform
test could ever see.

## Decision

Three defences, cheapest first, all verified:

1. **`#error` under `__FAST_MATH__`.** fast-math implies contract=fast *and*
   removes the NaN semantics the guards depend on. Refuse to compile.
2. **`#pragma STDC FP_CONTRACT OFF`** in the header (clang honours it at
   default and `=on`; it is ignored under `=fast` and by GCC — those builds
   must pass `-ffp-contract=off` in CFLAGS, and xtensa GCC's `-O2` default is
   fast contraction, so the S3 port must).
3. **The golden blob** (checks 11–12): a frozen 852-byte v1 instrument that
   must load and predict bit-identically, and a training-path run whose saved
   bytes must hash to `0xFEFAEDF6`. This catches whatever the preprocessor
   cannot.

Plus **`IRIS_FLUSH`**: velocities below 1e-30 flush to exact zero, closing the
denormal divergence identically on both platforms. 1e-30 is provably absorbed
(≪ 1 ulp of any weight ≥ 2⁻⁹⁰); the golden blob is unchanged by it, so the
flush is inert on healthy runs — verified by the two-build A/B in `build.sh`.

Host cost of contraction-off: +2.3–4.0% training time. Paid willingly.

The cross-*platform* half of the contract is still owned by the board
measurement (`E8 handoff/S3_BUILD_MATRIX.txt`): if contraction-off costs >10%
on the S3, the shipped contract becomes "bit-identical per platform" instead
of unconditional. Either way it will be a *stated* contract.

## Rejected alternatives

**Do nothing; the audit passes.** Rejected — the audit passed because it only
ever compared a binary against itself. The guarantee held by accident, and
accidents drift.

**Document "use -ffp-contract=off" and skip the pragma.** Rejected: a flag in
a README is advice; a pragma in the header plus a hash in the audit is a
mechanism. Users compile single-header libraries with their own flags.

**Normalise the golden hash per flag-class.** Rejected: three blessed hashes
is three instruments per seed, which is the bug wearing a lab coat.

**Flush denormals via hardware FTZ/DAZ modes.** Rejected: that is a
platform-specific control register (libc/fenv territory) and it changes *all*
arithmetic, not just the one array whose tail we can prove is dead.

## Consequences

- The golden baseline had to be re-frozen once, deliberately, when the pragma
  landed (the OFF-class blob was already recorded by E8; the audit's expected
  hash is that number). `./build.sh golden` documents regeneration as a
  baseline-redefinition act.
- GCC builds that omit `-ffp-contract=off` fail check 12 loudly rather than
  drifting silently. That is the check working.
- Re-derive: `./build.sh audit` (checks 11–12); the flag matrix lives in E8's
  scratch fork. Measured on Apple M4 Max (arm64), Apple clang 17.0.0, `-O2`,
  2026-08-21.
