# ADR 0015 — the save file names its columns, so a rig change costs no
# demonstrations

Status: accepted (v3, the extension layer)
Extends ADR 0006 (format v2, the one-word v1 loader is permanent).

## Context

`iris_load()` refuses a shape change:

```c
if ((int)h[2] != k->n_in || (int)h[3] != k->n_hid || (int)h[4] != k->n_out) return 0;
```

It is right to. Weights for a 2-input network are meaningless in a 5-input
one. But v2's `store.c` mapped that refusal straight onto **SAVE FAILED and an
empty instrument** — so *a student who adds one sensor loses every
demonstration they have made*. For a platform whose entire purpose is students
adding sensors, that was the single worst thing in the design.

The demonstrations are not weights. They are what the musician DID, and they
are the expensive part: a hold takes 600 ms and an afternoon's work is a
hundred of them. Weights take one millisecond to recompute.

## Decision

**EWP4: a 64-byte header, then a SCHEMA BLOCK that names the columns, then the
`iris_save()` payload byte for byte.**

```
[0..3] 'EWP4' [4] ver=4 [5] ni [6] nh [7] no [8] cap [9] nsrc
[12..15] payload len   [16..19] crc32 over (schema + payload)
[20..23] cand_n  [24..27] prev_seed  [28] has_prev  [32..35] schema len
schema:  ni rows { char name[8]; u32 fp; }  then  no rows { char name[8]; u32 num; }
```

`fp` is FNV-1a over the column's **derivation** — `"1:5;SMOOTH,500,0,0;VAR,40000,0,16"`.
The NAME is what the musician demonstrated; the FINGERPRINT is how it was
computed. Both are needed and they are treated differently.

| the file says | what happens | the band |
|---|---|---|
| schema identical | `iris_load`, byte-identical, ADR 0003 intact | nothing |
| name matches, fp matches | carried verbatim | — |
| **name matches, fp differs** | **carried anyway**, flagged | MIGRATED |
| a column is gone | dropped to the new slot's declared default | MIGRATED |
| a column is new | filled with its declared default | MIGRATED |
| `nh` / `no` changed | re-record the examples, `iris_retrain_elm_new` (~1 ms) | MIGRATED |
| `n_ex > cap` | keep the first `cap` | MIGRATED |
| ver = 3 | columns are positional **by definition** | (silent if clean) |
| bad crc / magic / truncated | SAVE FAILED, empty instrument, as before | SAVE FAILED |

## Why "name matches, fp differs" CARRIES rather than discards

Changing `SMOOTH(0.15)` to `SMOOTH(0.5)` barely moves a recorded value.
Changing `RAW` to `VAR(32)` makes it meaningless. **No fingerprint can tell
those apart** — it is a hash, not a semantics.

Carrying-and-saying-so is right because the examples are auditionable and
individually deletable: the musician hears a wrong one in ONE TOUCH and
dissolves it. Discarding gives them nothing to hear and nothing to judge.

## Why v3 maps POSITIONALLY

v2/v3 shipped exactly one rig — two glass axes, three outputs. Position IS the
identity there; there is nothing else it could be. That arm is hard-coded,
commented, and permanent, in the same spirit as ADR 0006's vow about the v1
loader. A migrated instrument is written back as EWP4 at the next auto-save,
so migration happens exactly once.

## Constraints honoured

* **`library/iris.h` is not modified** and `iris_load` is not bypassed on
  the matching path: an unchanged rig loads exactly as it always did.
* Migration reads examples out of `iris_save`'s DOCUMENTED layout — the header
  words say how many of what, and the example block sits at a computable
  offset. No private field is touched.
* `core/schema.c` is platform-free and host-tested against a real v3 blob, a
  re-chained column, a renamed column and a truncated file.
* MIGRATED is a WARNING band, not an error, and it clears at the first act
  that changes the dataset — by then the musician has seen it.

## Consequence

The vow in ADR 0006 is extended one level up: **a save file now outlives a
change to the rig.**
