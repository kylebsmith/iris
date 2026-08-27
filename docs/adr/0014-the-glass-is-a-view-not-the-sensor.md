# ADR 0014 — the glass is a view of the input space, not the input space

Status: accepted (v3, the extension layer)
Depends on D10 / ADR 0012 (the hold is the explicit act), ADR 0013.

## Context

v2 computed the colour field from `in[0] = px/239, in[1] = py/319`, and
`hot.c` built the feature vector the same way. "The input space" and "the
pixels" were the same thing — true only because the only sensor was the panel,
and false the moment a student fits a glove.

Two claims the field was making at once:

* **(a)** *this point on the glass produces this colour* — a SPATIAL claim,
  true only when the glass IS the input;
* **(b)** *you are here and it sounds like this* — a STATE claim, always true.

With six input channels only (b) survives, and a display that keeps making (a)
is lying in the one way a performance surface must not.

## Decision

**`rig.h` names two inputs as the view axes.** Every lattice vertex is the
LIVE FEATURE VECTOR with those two overridden by the pixel position:

```c
for (j = 0; j < EWA_NI; ++j) in[j] = live[j];
in[EWX_VIEW_X] = px / (UI_W - 1);
in[EWX_VIEW_Y] = py / (UI_H - 1);
```

The picture is a **2-D slice through the N-D mapping, taken where the other
sensors are right now**. Tilt the board and the whole field reshapes live.
That is not a compromise visualisation; it is the honest one, and it cost one
function. `core/surface.c` did not change to make it work.

Four consequences, all of which the substrate already handled:

* **The ring** is `v[VIEW_X] * (UI_W-1)`, not the finger. On the touch rig that
  is bit-identical to v2; on a glove rig **the ring moves with no finger on the
  glass** and the instrument plays continuously. `surface.c` cannot tell.
* **Marks** are their recorded vectors projected onto the view axes.
* **The lattice fade** uses view-axis pixel distance (`glass_novelty`); the
  RING carries full-dimensional `iris_novelty`, so it dims when you are far from
  every demonstration *in a dimension the picture cannot show*.
* **Outputs** generalise for free: a 6-output rig colours from the first three
  and sounds all six.

## Two honesty channels, where the picture is a lie

A slice is a faithful picture only where the mapping does not depend much on
the frozen channels. So:

1. **Off-plane sensitivity → the stipple.** At each vertex the model is also
   evaluated with the non-view channels nudged +/-10 %, and the larger of that
   and the novelty is what the texture shows. Where the slice is faithful the
   glass is smooth; **where the picture is a lie it is visibly grainy.** Two
   extra `iris_predict` per vertex, on model change only.
2. **Off-plane distance → the mark's outer stroke.** A mark whose unseen
   channels are far from where the sensors are now gets a DASHED outer stroke.
   Solid means "this example lives on this slice"; dashed means "you are
   seeing its shadow".

Both compile to nothing at `EWA_NI == 2`.

## Rejected alternatives

* **A `plane()` callback on `iris_source`.** A source cannot know: the two axes a
  musician can sweep may come from two different sensors, and the choice is
  about the post-extraction INPUTS, not raw channels. It belongs in the rig.
* **Auto-selecting the two most-varying channels.** Chrome-free and adaptive,
  but the ranking flips as examples arrive, so marks would change position and
  colour on commit. Identity stability beats information density for an
  instrument you rehearse with.
* **PCA of the example set.** Same objection, plus it back-projects glass
  pixels along a plane, so two examples differing only off-plane collapse to
  one point and the field between them is fabricated with nothing to signal
  it. The declared-plane + stipple design signals it.

## What this does NOT show, stated plainly

Structure living entirely in the unseen channels; the stipple says THAT there
is such structure and roughly how much, never what it is. Nor the DIRECTION of
off-plane variation, only its magnitude. A rig with no plane a musician can
sweep declares no view axes, and then the glass stops claiming to be a map at
all — the absence of a gradient is itself the honest statement.
