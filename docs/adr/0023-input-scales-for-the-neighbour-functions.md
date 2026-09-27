# 0023 — Input scales or groups for the neighbour functions

**Status:** proposed, 2026-09-27. No decision; nothing here is code.
**Affects, if adopted:** `iris.h` PART 10 (`iris_internal_neighbour_scale`
and every function that measures with it: `iris_knn_predict`,
`iris_classify_1nn`, `iris_delete_nearest`, `iris_nearest`), PART 7
(`iris_novelty`), and PART 9 if the setting is saved.

## Context

The neighbour functions count each input in fractions of its demonstrated
range, so a millimetre sensor and a g-force sensor count alike, and an input
that never moved counts not at all (PART 10; the worked example above
`iris_delete_nearest`). For inputs in different units that is right. For
inputs that are one geometric thing it is not: a direction given as three
numbers, such as the gravity vector the starter kit's BNO055 sketches read,
is stretched axis by axis. An axis the teacher barely moved is magnified to
the same weight as one swept end to end, so two readings a few degrees apart
can be far apart in the distance and two readings far apart can be near.
There is no way to say "these three share one scale" or "measure these
raw".

No measurement yet says how much this costs in practice. The instrument that
raised it had takes spanning every axis, where the stretch is mild. The
concern is sharpest for a few takes clustered around one pose, where one axis
spans a small range and dominates every distance.

## Options

**A. Nothing in the library.** The sketch can equalise the ranges itself by
recording two extra takes at the ends of a common range on every axis of the
group (for a unit vector, -1 and +1), which widens each axis's range to the
same span; but those takes also train the network and appear in every
neighbour answer, so this is a workaround with side effects. Contract:
unchanged.

**B. A per-input scale for the neighbour functions**, a multiplier on the
scale PART 10 derives, 1 by default, set at run time. With every scale 1 the
multiplication is exact (`x * 1.0f == x`), so every neighbour function
returns what it returns now, bit for bit, and the frozen nearest-neighbour
paths are untouched until a caller sets a scale. Held at run time only, it
fits a minor release; saved, it is part of what a file plays and needs
format 8. Arena: one float per input. Stack and heap: none. The network is
unaffected.

**C. Input groups.** Inputs in one group share one scale: the widest range
in the group, so the geometry inside the group is preserved. Contract as B
(a group index per input, default "alone", which reproduces today's scale
exactly). It answers the direction case directly and needs no numbers from
the caller, only which inputs belong together.

## Decision

Not made. The maintainer decides; until then nothing here is code.

## The measurement that would decide it

Record, with several players, takes of poses given as the BNO055's gravity
vector (and as the heading directions of the starter kit's helper, which are
unit vectors too), clustered as a player would teach a small instrument.
Compare, on held-out takes, how often `iris_classify_1nn` names the taught
pose under today's per-input scale and under a shared scale for the three
axes of each vector. If the difference is within the variation between
players, the record should end at A with a paragraph in PART 10; if it is
not, C is the smaller change for a caller to get right.

No figure in this record comes from a measurement yet.
