# 0012 — Explicit training is the hold-to-set act; the field reshaping is its consequence

**Status:** SUPERSEDED by D11 (2026-08-22) — batch training via an explicit TRAIN action
replaces hold-as-training. The hold still places a node; it no longer trains. Originally
accepted 2026-08-22 — PI-signed (D10); supersedes the TRAIN-button reading of "training is explicit"
critique that forced it is quoted verbatim below)
**Affects:** `app/core/` (the whole screen model is replaced), `app/host/`
(harness, tests, shots), the `test`/`shots` targets of `app/build.sh`.
`library/iris.h` is unchanged. The IDF shell is unchanged; its phase 3
wires this core through the extended vtable.

## Context

The project law says *training is explicit*. v1.1 of the app read that as "a
TRAIN button": the musician records examples, then presses TRAIN, and only
then does the model change. It also read "examples are listable, auditionable,
deletable" as "a chip lane with numbers on it", and "the synth's parameters"
as "three sliders with units". The result was two screens, three PLAY elements,
three TRAIN elements, a message line, a count chip, and a string budget table
of fifty-eight literals.

The PI's review of that build, verbatim:

> its so dense, the display is absolutely saturated with information and its
> entirely unintuitive, you need to totally re-evaluate how to implement this
> as a UI. why are there so many exposed parameters? Thats not what the device
> should expose. It should be a surface, a substrate. The training and shit
> needs to be more abstracted. Why are there three different play elements on
> the display and 3 different train? why are train and play on the same display
> and when playing it should be the entire screen no distractions, elegant
> visual feedback to show the space that youre navigating via our version of
> Wek.

Two things in that paragraph are design constraints, not taste:

1. **The device exposes a surface, not parameters.** The synth's parameters
   are the synth's business. The device's job is to learn a mapping from the
   glass to *whatever the synth does with three MPE dimensions*, and to show
   the musician the mapping it has learned.
2. **PLAY is the entire screen.** Nothing on it but the learned space and the
   finger moving through it.

Under those constraints a TRAIN button is impossible, because a button is a
parameter of the training process exposed on the surface. So the question
this ADR answers is: *where does "explicit training" live when there is no
button?*

The model in use is a closed-form ELM (ADR 0008): a retrain is a 1 ms solve,
deterministic in seed + dataset (ADR 0003). There is no longer any cost that
a TRAIN button was protecting the musician from. The button was a leftover of
the 600-epoch backprop era, when training took 50 ms and could diverge.

## Decision

**The explicit act is the HOLD.** In TRAIN the musician touches a point and
hears a candidate sound from their own synth. Lifting does nothing. Holding
for 600 ms — while a ring fills around the finger — sets a mark. The mark is
the example; the moment it sets, the model retrains (ELM, ~1 ms) and the
colour field on the glass reshapes around the new mark. The musician never
asks for training. They commit an example, and the reshaping is what a
commit looks like.

Stated as law:

- *Explicit* means: the model never changes because of something the musician
  did by accident. A tap is never a commit. A drag is never a commit. Only a
  still finger held through a visible, cancellable 600 ms fill commits, and
  only a still finger held through a visible, cancellable 900 ms drain
  deletes. Lifting during either fill cancels it with nothing changed.
- *Training* is the consequence of a commit, not a separate act. Every change
  to the example set — set, dissolve, reroll — is followed immediately by a
  retrain, and the retrain is shown (the field crossfades to its new shape
  over three frames). There is no stale state. There is no "trained" label
  because there is no untrained-but-has-examples state to label.
- *Listable, auditionable, deletable* are satisfied by the marks themselves:
  they are on the glass (listed), a tap on one plays it (auditioned), a hold
  on one dissolves it (deleted). No lane, no numbers, no chips.
- *Old models survive* is satisfied by determinism plus one retained seed: a
  commit adds one example, a dissolve removes one, a reroll changes the seed
  and keeps the previous one for a swipe-back undo. Nothing is ever replaced
  silently; every one of those changes is a full-screen reshape the musician
  watches happen.

The PI signs this as a change to the law's wording: "explicit training"
becomes "explicit commitment; training is its visible consequence".

## Rejected alternatives

**1. Keep a TRAIN button, make it small.** Rejected by the critique directly
("why are there ... 3 different train"). A small button is still a parameter
of the training process exposed on the surface; and with a 1 ms ELM it
protects nothing. It also creates the stale state (examples recorded, model
not yet trained) that needed a label, a colour, and a message to explain.

**2. Train on every tap (no hold).** Rejected: violates explicitness. A tap is
how the musician *listens* — it must be free. A model that changes every
time you listen to a candidate would be a model you cannot audition.

**3. Train automatically on the 2-second idle timer (with the save).**
Rejected: the reshaping would happen when the finger is up and the musician
is not watching, severing the act from its consequence. The feedback must
land within the hold, under the finger.

**4. Separate "record" and "train" holds (hold once to record, hold again on
the same mark to train).** Rejected: two holds for one intention, and it
reintroduces the stale state.

**5. A confirmation step after the hold (a second tap to accept).** Rejected:
the hold *is* the confirmation. It is 600 ms of visible, cancellable intent.
Adding a second step makes the field reshape at the wrong moment.

## Consequences

- `app/core/` loses: sliders, the chip lane, the count chip, the message line,
  the strip, the switch button, LEARN/RECORD/TRAIN/REROLL buttons, the string
  budget table (fifty-eight literals become six error words). It gains: a
  field renderer, a candidate-sound model, the hold/drain rings, the chord,
  the swipe, and dirty-rect compose.
- The host tests that asserted chrome (T-TEXT-FIT over 58 strings, the slider
  band, the chip colours) are replaced by tests that assert the surface: the
  field is a pure function of the model; a tap never changes the model; the
  600 ms fill commits exactly one example and the field differs after; the
  900 ms drain removes exactly that example; a swipe changes the seed and a
  swipe back restores it bit-exactly.
- The only text the device can ever show is the error band (six words). If a
  future condition needs words, it is an error by definition, or it does not
  go on the glass.
- ELM is no longer optional for this app: the hold-to-set loop depends on a
  retrain that completes inside one frame. The backprop trainers stay in the
  library for other ports.

## How to verify

```sh
cd firmware/app
sh build.sh test    # T-BOOT, T-STATE (every transition), T-EXPLICIT (hold timing),
                    # T-SURFACE (no text unless a condition stands), T-FIELD,
                    # T-RECT, T-HOT (byte-exact), T-CAND, T-STORE — 139 assertions
sh build.sh shots   # host/shots/*.ppm + .png — the twelve scenes, and a 2x
                    # contact-sheet.png
```

## Review amendments (adversarial pass, 2026-08-22)

Four changes to the built core, each forced by a host test that failed
before the change and passes after it (`T-REVIEW` in `host/app_test.c`):

1. **The ring sits outside the fingertip.** The spec's 24 px ring (4.3 mm at
   0.178 mm/px) is entirely under a ~8 mm contact patch during the one act it
   exists for. The ring is now r 21..26 (56 px box, wider than the brief's
   44 px minimum target) and the act is an 8 px arc ON the ring: growing
   clockwise to set, a full arc erased anticlockwise to dissolve. On a mark
   the ring snaps to the mark's centre, so the drain is around the finger,
   not under it. Rect budgets: ring move <= 57x114, commit click <= 57x57.
2. **The sound you liked is the sound you set.** A DOWN within 22 px of the
   ghost re-hears the SAME candidate (no `cand_n` bump) and a hold commits
   it; a DOWN anywhere else on empty glass is a new candidate. Before, a
   tap-to-listen followed by a hold committed a different sound than the
   one auditioned, so the only way to set a sound was to decide within
   600 ms of first hearing it. "Tap again for a new one" now means "tap
   somewhere else".
3. **`pad_live` is 1 only while the core holds no finger.** `set_mode` used
   to re-arm the hot task while the surface was still SETTLE-ing; the hot
   could then take platform id 0, swallow the UP the core was waiting for,
   and strand the surface in SETTLE for good (reproduced by a 3000-step
   random storm at step 1599, and by a scripted id1-first chord followed by
   a PLAY chord). A declined PLAY gesture also posts `pad_live = 0` until
   its UP. The adopted hot finger is platform id 0 (`fmap[0] = 0`).
4. **A lost UP never strands anything.** The core tracks which platform ids
   it holds; a DOWN on an id still held lifts the old finger first. The hot
   task does the same for its gesture (note-off, then the new gesture).
   A deferred retrain (model busy at the commit) now starts the crossfade
   when it lands; before, the field stayed on the old shape indefinitely.

Observed, not changed (outside `app/core`): with one mark, or with marks
sharing an input coordinate, `iris_fit_ranges` floors the input range to
1e-6, so `iris_novelty` is 1 everywhere and `iris_predict` is a hard step along
the degenerate axis. The field renders this honestly (a flat, fully faded
first mark), but the first commit's "reshaping" is flat until the second
mark lands off-axis. That is a library decision (`library/iris.h`).

## Review amendments, second pass (adversarial, 2026-08-22)

Measured on the rendered PPMs (mean HSV saturation/value per region) and
by scripted hostile sequences; each change is forced by a `T-REVIEW2`
assertion in `host/app_test.c` that failed before it:

5. **The first mark glows.** The field's novelty is now measured in GLASS
   units (pixel distance to the nearest mark / 200 px, `EWA_NOV_PX`), not
   with `iris_novelty`. With one mark, or two marks sharing a column, the
   library floors the marks' spread to 1e-6 and reports novelty 1 on every
   pixel but the mark's own: the first commit painted a flat, fully faded
   field (near S .32 V .15 vs far S .32 V .14 — indistinguishable). The
   glass's range is known, so its scale is fixed; for marks spanning the
   glass the two measures agree (the 6-mark PLAY field and every bit-exact
   test were unchanged). Now: near S .83 V .38, far S .32 V .14. The hot
   task still reports `iris_novelty` in `live.nov` (the library's semantics).
6. **SAVE FAILED heals itself.** A failed write re-arms the idle save
   10 s later (`EWA_SAVE_RETRY_US`); before, the band stood until the next
   commit, and a transient FFat failure after the LAST act of a session
   would have lost that act without a retry ever happening.
7. **A refused MODE post is retried every tick.** `hot_post` returning
   "queue full" during the chord into PLAY left `pad_live = 0` with no
   band and no retry: a PLAY that made no sound. AUDITION/RELEASE were
   already retried; MODE now is (`mode_pending`). MODEL_READY needs none:
   the hot re-reads `iris_is_trained` on the shared model at each DOWN.
8. **A reroll while the model is busy changes nothing, including later.**
   The refused retrain left `retrain_due` set, so the next tick retrained
   with the SAME seed and crossfaded the identical field back in (3 full
   frames of nothing). The pulse is the whole answer now.

Verified without change: PLAY calls the text primitive zero times and the
PPM holds no glyph; the ring box is 56 px (> 44), the mark target 44 px;
the band is 2x type; zero malloc, zero libc in `core/`; every TU passes
`clang --target=wasm32 -fsyntax-only -Wall -Wextra -Wpedantic -Wshadow`;
reload reproduces the 825-vertex field and the PLAY glass bit-exactly;
the field is recomputed only on model change (generation stable across
12 000 touch events in both modes); second finger during a dissolve,
button mid-chord, a 5 s stuck button, 300 commits, 1000 CC/s, and the
3000-step storm all end in IDLE with everything up.

Noted, not changed: "tap again for a new candidate" means "tap off the
ghost (>= 22 px)" — amendment 2 above; the PI should sign this one. The
two-finger sideways swipe (reroll/undo) is only discoverable by drifting
during a chord; the chord itself is discovered by accident within the
first minute, the swipe probably within the first session. `TOUCH ERRORS`
(a platform id outside 0..1) never clears: that is a shell bug the
musician should keep seeing until the shell is fixed.
