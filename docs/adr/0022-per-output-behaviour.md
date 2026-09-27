# 0022 — Per-output behaviour: snap or interpolate, listening, partial takes

**Status:** proposed, 2026-09-27. No decision; nothing here is code.
**Affects, if adopted:** `iris.h` PART 6 (`iris_predict`), PART 8 and 8d (both
trainers), PART 9 (the file format), PART 10.

## Context

iris trains one network for every output. Every output listens to every
input, every take sets every output, and every output interpolates between
the takes. Desktop Wekinator decides each of those per output. The first
instrument built outside the project met all three:

- **Snap or interpolate.** A category stored as a number is interpolated as a
  number. `examples/04_categories.c` shows it with four takes teaching a tilt
  two chords, C major and A minor (labels 0 and 5): between the two places
  the network plays 1.09, 2.62 and 4.05 at 40, 45 and 50 degrees, which are
  Dm, F and G, none of them taught. For timbre, interpolation is the point;
  for harmony, it is the problem.
- **Which inputs an output listens to.** A move meant to change the colour
  also changes the note.
- **Which outputs a take teaches.** A take recorded to correct one output
  resets every other output at that gesture too.

What 0.3.0 already gives, with no change to the contract: the same example
reads the category from the nearest take (`iris_nearest`, then `iris_get`)
and holds it with hysteresis from the distances, so only taught chords sound,
and the chord changes at 50 degrees going up and 40 going down. The
continuous outputs still come from `iris_predict`. Two instruments over the
same takes, one per group of outputs, already give an output group its own
inputs.

## Options

**A. Nothing in the library.** Read an instrument twice, as
`examples/04_categories.c` does; split outputs that must not listen to some
inputs into a second instrument. Contract: unchanged. Cost: the sketch keeps
the list of snapped outputs, and a snapped reading costs one neighbour scan,
O(takes × inputs), beside the prediction. A second instrument costs its arena
and its training time.

**B. A per-output snap flag, applied by `iris_predict`.** Output o, when
flagged, is the nearest take's value (the ranking of `iris_nearest`), and the
network's value otherwise.
- *B1, held at run time only* (a setter; not in the file). Contract: nothing
  an existing function returns changes while no flag is set, so it fits a
  minor release; format 7 is untouched; a saved instrument plays as before
  and the sketch sets the flag again after `iris_load`. Stack: the neighbour
  scan's `IRIS_MAX_IN` floats and slots inside `iris_predict`. No heap.
- *B2, saved.* The flag becomes part of what a file plays, so it needs
  format 8; 0.3.0 and earlier refuse such a file, and every later reader
  keeps reading format 7. One bit per output.
Cost of either: `iris_predict` goes from 60 multiply-adds at 2-12-3 to that
plus a scan of every take whenever a flag is set: on the development laptop
a k-nearest-neighbour call over 256 takes is 1.2 µs, against 0.036 µs for a
prediction (the README's timing table, from `sh build.sh audit`). The board
is not measured.

**C. Per-output input masks.** Output o sees only the inputs its mask names.
One network cannot do this without training changes: masked weights must be
held at zero by both trainers, which changes how every masked instrument
trains, and the mask is part of what a file plays (format 8). The closed-form
solve would need one normal matrix per distinct mask. Option A's second
instrument gives the same separation now at the price of a second arena.

**D. Takes that teach some outputs.** A per-take, per-output "not taught"
mark. Backpropagation skips the marked terms of the loss; the closed-form
solve needs a separate set of rows for each output, one factorisation per
output instead of one for all. The marks are `cap × n_out` bits in the arena
and in the file (format 8). The neighbour functions would need a rule for a
nearest take that does not teach the output asked for.

## Decision

Not made. The maintainer decides; until then nothing here is code.

## The measurement that would decide it

Option A exists. The question is whether B1 adds enough to justify a second
path in the playing call. Measure, with the starter kit's BNO055 and a
harmony instrument built both ways (A in the sketch; B1 in a branch of the
header): the time per played reading on the ES3C28P at 20 and 50 takes, and,
with several players, how often a player returns to a taught chord within a
tolerance of the taught tilt, and how often an untaught chord sounds on the
way. If B1 buys nothing A does not, the record should end at A plus
documentation. C and D are larger than any measured need so far; they wait
for an instrument that cannot be built with two instruments over one set of
takes.

The measurements quoted are printed by `examples/04_categories.c` and
`sh build.sh audit`.
