# 0025 — Takes that carry time

**Status:** proposed, 2026-09-27. No decision; nothing here is code.
**Affects, if adopted:** a new PART of `iris.h` beside PART 10, PART 3 (the
arena), PART 9 (the file format), and `IRIS_ARENA`.

## Context

`iris_predict` is a function of one frame: what the sensor reads now. A
gesture that is a movement in time (a strike, a flick, a twist, a shake) has
to be turned into one frame before iris sees it, and that work is all the
sketch's. The first instrument built outside the project did it with an onset
detector on a motion level, a buffer of the samples before the onset, a window
after it, hand-made features of the window (force, energy, duration, the
direction of the push, the rotation, the spin) and a refractory time so one
strike is not two. That was most of the instrument's code, written before iris
was asked anything, and the choice of features decides what the player can
teach. It was built but not played: no measurement on real strikes exists yet.

The smallest addition that would let such gestures be demonstrated rather
than engineered is a take that is a short sequence of frames, and a call that
reads a live window against the stored sequences, nearest by dynamic time
warping (DTW: an alignment of two sequences that lets one run faster or
slower than the other), returning the take and its distance as
`iris_nearest` does. Onsets, and how hard the strike was, can stay in the
sketch.

## Options

**A. Keep time in the sketch** and give the starter kit a helper: an onset
detector and a window, with the features left to the player. Contract:
unchanged.

**B. Flattened windows through the library as it is.** The sketch resamples
each window to a fixed number of frames and hands iris the frames side by
side as one take: 8 frames of 6 inputs is 48 inputs, which the maxima allow
once raised (`IRIS_MAX_IN 64`; the note on the maxima near the top of
`iris.h` gives the stack that costs). Every trainer and neighbour function
works unchanged, and `iris_nearest` gives the distance. Contract: unchanged.
What it cannot do is let a gesture be performed faster or slower than it was
taught: frame 3 is compared with frame 3.

**C. Sequences and a DTW neighbour call in the library.** Each take holds up
to T frames. Memory, from the arithmetic (an estimate, not a measurement):
at 100 frames a second, half a second is T = 50; with 6 inputs a take is
50 × 6 × 4 = 1,200 bytes, 20 takes 24,000 bytes, 64 takes 76,800 bytes,
against the ESP32-S3's 512 KB of internal memory (the note at
`IRIS_MAX_EX`). It stays one header with no heap: the sequences live in the
arena, sized by a new macro beside `IRIS_ARENA`, and the alignment needs two
rows of T floats on the stack (400 bytes at T = 50), bounded by a new maximum
like the others. Time per query: T² × n_in multiply-adds per take, 15,000 at
T = 50 and 6 inputs, 300,000 for 20 takes; a band that limits how far the
alignment may stray (a Sakoe-Chiba band of width w) cuts that to
T × (2w + 1) × n_in. How long that takes on the board is not measured.
Contract: sequences are part of what a file holds, so format 8; the network
and its trainers are untouched, since the sequences would be read by the new
call only.

## Decision

Not made. The maintainer decides; until then nothing here is code.

## The measurement that would decide it

First, the baseline: record strikes on the board with the instrument whose
chooser already exists (hand features, then `iris_classify_1nn`,
`iris_knn_predict`, the closed-form trainer and `iris_train`, chosen by a
leave-one-out harness that is written and has not been run on real strikes).
Then, on the same recordings, with several players: held-out accuracy and
false triggers for (1) the hand features, (2) B's flattened windows at a few
frame counts, and (3) a DTW prototype on the laptop, and the time per decision
of (2) and (3) on the ES3C28P. C is worth format 8 and a new PART only if it
beats B by more than the variation between players, at a time per decision
that leaves the sound running.

The memory and operation counts are arithmetic on the sizes stated; nothing
in this record is measured yet.
