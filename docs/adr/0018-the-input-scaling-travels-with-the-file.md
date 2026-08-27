# 0018 — Inputs are scaled to [-1,+1], and the scaling travels with the file

**Status:** accepted, 2026-08-22
**Supersedes nothing; extends** [`0006`](0006-format-v2-one-word-v1-loader-permanent.md),
whose promise it is written to keep.
**Affects:** `iris.h` (`iris_norm_in`, `iris__set_legacy_norm`, `iris_save`,
`iris_load`, `IRIS_FORMAT`), `tests/audit.c` checks 11, 12, 29, 30,
`tests/golden/`, `firmware/app/core/schema.c`.

## Context

`iris_norm_in` mapped inputs to [0,1]. Wekinator drives Weka's
`MultilayerPerceptron` with `normalizeAttributes` on
(`research/briefs/01-wekinator-internals.md:54`), which scales
attributes to **[-1,+1]**. That is also what LeCun et al. 1998 ("Efficient
BackProp", §4.3) prescribes, and for a concrete reason: with all-positive
inputs every weight into a hidden unit receives a gradient of the same sign, so
the descent has to zig-zag.

**This is a fix, not a deviation.** We were less faithful to Wekinator than the
README said we were.

50 examples, 8 outputs, nh=12, mean of 9 seeds:

| epochs | [0,1] train | [-1,+1] train | [0,1] grid | [-1,+1] grid |
|---|---|---|---|---|
| 600 | 5.94e-4 | 6.38e-5 (9.3×) | 0.0129 | 0.0084 (1.54×) |
| 20 000 | 8.19e-6 | 6.41e-6 (1.3×) | 0.0064 | 0.0063 (wash) |

Stated honestly: **most of the win is a convergence-speed win, and
[ADR 0017](0017-train-to-the-plateau-not-to-a-constant.md) already buys it.**
At convergence the two scalings generalise the same. The reason to make the
change is fidelity to the model Wekinator actually runs, and that reason
stands on its own.

## The danger, which is the whole reason this ADR exists

A stored weight means nothing except against the scaling it was trained in.
Change the scaling under a saved instrument and it still loads, still reports
itself trained, and still plays — and every prediction is wrong. Every
first-layer pre-activation shifts by `-sum_i w1[h][i]`, an arbitrary
per-hidden-unit constant. The mapping is not degraded; it is a *different*
mapping. The musician loads the instrument they practised for a year, hears
something else, and has nothing to point at. That is the exact failure
Fiebrink & Sonami (NIME 2020) describe and the exact thing ADR 0006 promised
would never happen here.

## Decision

**The file version selects the scaling.** Not a build flag, not a global, not
the caller.

- `IRIS_FORMAT` becomes **3**. A v3 file has the v2 layout exactly — same nine
  header words, same payload, same length. Nothing about the bytes changes;
  what changes is what they mean.
- `iris_load` sets `k->in_center` from `h[1]`: v1 and v2 → [0,1], **for ever**;
  v3 → [-1,+1].
- **`iris_save` writes `h[1]` from `k->in_center`, never from a constant.** The
  version word describes the scaling the weights were fitted under; it is not
  a build stamp and not "the newest format we know". A legacy-scaled
  instrument saves as **v2** however new the build is. *This clause was
  missing from the first draft of this ADR and its absence was a live
  silent-corruption bug — see "The clause that was missing" below.*
- `iris_init` starts every fresh instrument at v3.
- `iris__set_legacy_norm` exists for exactly two callers: `iris_load`, and the
  audit, which uses it to hold the pre-v3 training path against its frozen
  hash.

Both branches are live in every build. There is no configuration in which the
[0,1] path is compiled out, and `iris.h` PART 9 states in a comment what
silently breaks if someone deletes it.

## How it is held

| check | what it pins |
|---|---|
| 11 (unmodified) | the frozen golden **v1** file loads and predicts **bit-identically** |
| 12 | **both** training paths by hash. `0xFEFAEDF6`, the [0,1] constant, is unchanged since v0.1.0 — it was *not* re-frozen for v0.3.0, which is the mechanical proof that the legacy path is still the code it always was. `0x6805FB0D` is the new [-1,+1] constant, verified identical at `-O0`/`-O1`/`-O2`/`-ffp-contract=off`. |
| 29 | a frozen golden **v3** file, given the same protection on the day it ships rather than a year later |
| 30 | the migration story: a v1 file and a re-trained, re-saved v3 of the same demonstrations agree to 0.045 over 441 probes, *while provably being on different scalings* |
| **34** | an old-scaling instrument **re-trained in place, saved, and re-loaded** is bit-identical to what was in memory. Zero tolerance. This is the route check 30 does not take. |
| **35** | `iris_migrate_scaling` is opt-in, idempotent, and carries the format word with it |

`./build.sh golden` regenerates the v1 pair with the legacy scaling forced, so
regenerating it must be a byte-for-byte no-op. It is: same MD5 before and after.

## The rejected alternative: a build-time flag

An `#ifdef IRIS_CENTER_IN` would have been three lines. It is exactly wrong: the
same file would then load correctly or incorrectly depending on who compiled
the firmware, and nothing in the file would say which. Versioning the format
costs one number and makes the failure impossible rather than unlikely.

## The one that got away, recorded

A second, independent parser of this format exists in the app —
`firmware/app/core/schema.c:old_open`, which salvages examples across rig
changes. It refused v3 and turned every migration into a silently empty
instrument. It was caught by the app's own migration assertions within minutes,
but it is the lesson: **a format has as many loaders as it has readers**, and
a version bump has to visit all of them.

## The clause that was missing

The first implementation of this ADR specified what `iris_load` *reads* and said
nothing about what `iris_save` *writes*. `iris_save` therefore kept doing what it
had always done — stamp `IRIS_FORMAT`, the newest format the build knows.

That is wrong the moment an instrument can be on a scaling older than the
build, which is precisely what this ADR introduces. The sequence:

1. `iris_load` on a v1/v2 payload → `in_center = 0`. **Correct.**
2. The musician re-trains. The instrument stays on `[0,1]`. **Correct** — its
   weights mean nothing else.
3. `iris_save` stamps **v3**. **Wrong.** `[0,1]` weights, labelled `[-1,+1]`.
4. The next `iris_load` believes the label and switches scaling.

The file is not corrupt. Every weight round-trips bit-perfectly, every field
arrives, the instrument reports itself trained and plays. It simply plays a
different mapping: each first-layer pre-activation shifts by
`-sum_i w1[h][i]`, an arbitrary per-hidden-unit constant. **Measured 0.427 of
full scale** over 441 probes — this ADR's own opening paragraph, arrived at
from the inside.

Reachable through `core/store.c` → `core/surface.c` → `core/store.c`, which is
the ordinary open / train / save the instrument app performs every session, and
the *only* route for a rig that has not changed (a changed rig goes down the
schema-migration path, which rebuilds from the demonstrations and is unaffected).

**Why the audit missed it.** Check 30 tests migration by copying the
demonstrations into a *fresh* `iris_init` instrument. A fresh instrument is
already `in_center = 1`, so the save is legitimately v3 and the defect is
invisible. It was the one migration route that cannot reach the bug. The
31-check audit passed green with the corruption live; checks 34 and 35 now take
the other route, and both were mutation-tested by reintroducing the constant
and confirming they fail.

**The general lesson, worth more than the fix:** an ADR that versions the
*meaning* of bytes must specify the writer as explicitly as the reader. Stating
how a value is interpreted, and leaving how it is produced to whatever the code
did before, is not a specification — it is half of one, and the half that was
left out is where the silence lives.

## The way out, and why it is a function and not a default

`iris_migrate_scaling(k)` throws the old weights away and re-fits the same stored
demonstrations under `[-1,+1]`. This is legitimate because the demonstrations
are raw sensor values in the musician's own units and mean the same thing under
either scaling — the same reason the app's schema-migration salvage is correct.

It is **a new fit, not a conversion**: it moves the instrument by 0.0447, the
same fit tolerance check 30 measures. So it cannot be something `iris_load` does
on the musician's behalf. Fiebrink & Sonami's users lost technique to
retraining they did not ask for; an old instrument stays exactly as it is until
someone names the function. `iris_input_scaling(k)` reports which scaling an
instrument is on, so a UI can offer the choice rather than hide it.
