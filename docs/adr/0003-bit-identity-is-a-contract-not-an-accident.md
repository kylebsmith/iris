# 0003 — Bit-identity is a contract, not an accident

**Status:** accepted, 2026-08-21. Governs `iris.h` 0.2.0.
**Affects:** `iris.h` (the compiler tripwires, the contraction pragmas,
`IRIS_FLUSH`), the golden hashes in `tests/audit.c`, `tests/load.c` and
`tests/starter_recipes.c`, `sh build.sh determinism`.

## Context

"Same seed, same instrument" is the library's first promise, and the check
"same seed -> identical instrument" in `tests/audit.c` has always passed. That
check compares one binary against itself. It cannot see what a different
compiler setting does to the same source.

Fused multiply-add contraction (the compiler merging `a*b + c` into one
instruction that rounds once instead of twice) changes the bits. The same
source, seed and recipe, compiled on Apple clang 17 on an Apple M4 Max with
three contraction settings, saved three different instruments (recorded;
program in iris-studies S18):

| flags | saved-file FNV-1a hash (the Fowler-Noll-Vo byte hash) |
|---|---|
| `-O2 -ffp-contract=off` | `0xFEFAEDF6` |
| plain `-O2` (clang's default, contraction on) | `0x1C284AC8` |
| `-O2 -ffp-contract=fast` | `0xF237919E` |

With contraction on, `-O0` and `-O1` gave a class of their own, distinct from
`-O2`, because the optimiser changes which expressions get contracted. Only
contraction off was the same at every optimisation level. The differences are
musically nil (at most 4 units in the last place, at most 2.4e-7; recorded,
iris-studies S18) and fatal to a golden hash, to identity across machines, and
to a saved random state, which assumes a reloaded instrument continues
exactly.

A second hazard is subnormal numbers (floats so close to zero that the format
gives up precision to represent them). Momentum velocities decay towards zero
and, left alone, settle among the subnormals: 49,477 of 50,000 iterations in
one recorded trace (iris-studies S18). Some processors flush subnormals to zero
in hardware and others compute them, so a decaying tail is a place where a
host and a board can part ways without any test on one machine seeing it. The
ESP32-S3 does not flush them (`board_probe`'s subnormal lines in
[`docs/board/2026-09-25-es3c28p.txt`](../board/2026-09-25-es3c28p.txt)).

## Decision

Four defences, cheapest first:

1. **`#error` under the flags that change the arithmetic or remove the
   not-a-number guards**: `__FAST_MATH__`, `__FINITE_MATH_ONLY__`, and on GCC
   (the GNU Compiler Collection) `__RECIPROCAL_MATH__` and
   `__ASSOCIATIVE_MATH__`; and `#error` when `__FLT_EVAL_METHOD__` says float
   arithmetic is carried in a wider format.
2. **Contraction off in the header's own code only**: `#pragma STDC
   FP_CONTRACT OFF` inside `float_control(push)` / `(pop)` on clang, and
   `#pragma GCC optimize ("fp-contract=off")` inside `push_options` /
   `pop_options` on GCC. Clang ignores the standard pragma under
   `-ffp-contract=fast`, so such a build must also pass `-ffp-contract=off`.
3. **Golden hashes**: `0x6805FB0D` for the fixed-epoch recipe in
   `tests/audit.c` ("golden blob: [-1,+1] training path bit-pinned"), the
   starter kit's two recipes in `tests/starter_recipes.c`, and a committed
   format 7 file whose predictions `tests/load.c` compares bit for bit. These
   catch whatever the preprocessor cannot.
4. **`IRIS_FLUSH`**: a velocity, or a weight after the weight-decay step,
   smaller than 1e-30 in magnitude becomes exactly zero, so neither ever
   reaches the subnormals. 1e-30 is far below one unit in the last place of
   any weight that matters, so the flush discards nothing audible, and
   `tests/guards_ab.c` shows it changes no bit of a healthy run (it is
   compiled out with the other guards under `-DIRIS_NO_GUARDS`, and the two
   builds must agree).

`sh build.sh determinism` checks the golden hash and the starter hashes at
`-O0` to `-Os` with contraction off, on and at the default, and checks that
removing the pragmas loses the golden hash.

Host cost of contraction off: 2.3% to 4.0% more training time (recorded on the
Apple M4 Max, Apple clang 17, `-O2`; iris-studies S18).

The cross-platform half of the contract is met on one board: on an ES3C28P
(ESP32-S3) the golden recipe, the starter kit's `device_torture` test 1 and
`determinism_check` all give the laptop's hashes
([log](../board/2026-09-25-es3c28p.txt)). The contract is therefore stated
unconditionally, with that one board as its hardware evidence.

## Rejected alternatives

**Do nothing; the same-seed check passes.** Rejected: it compares a binary with
itself, so it passes whatever the compiler does.

**Document "use `-ffp-contract=off`" and skip the pragma.** Rejected: a flag in
a README is advice; a pragma in the header plus a hash in the tests is a
mechanism. Users compile single-header libraries with their own flags.

**One golden hash per flag class.** Rejected: three accepted hashes are three
instruments per seed, which is the defect itself.

**Flush subnormals with the processor's flush-to-zero mode.** Rejected: that is
a platform-specific control register, reached through the C library's
floating-point environment, and it changes *all* arithmetic, not just the
decaying tails the flush is known to be safe on.

## Consequences

- Moving a golden hash is a deliberate act that a commit must justify.
- A build that defeats the pragma (clang with `-ffp-contract=fast` and no
  `-ffp-contract=off`) fails the golden check loudly rather than drifting
  silently.
- Re-derive: `sh build.sh audit` and `sh build.sh determinism`.

Measurements: iris-studies S18.
