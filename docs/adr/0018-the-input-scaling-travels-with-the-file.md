# 0018 — Inputs are scaled to [-1,+1], and the scaling travels with the file

**Status:** accepted in part, 2026-08-22. The scaling governs `iris.h` 0.2.0:
every input is scaled to [-1, +1] across its demonstrated range. The mechanism
that carried a choice of scaling in the file is superseded: 0.2.0 has one
scaling, reads and writes format 7 only, and the file has no field for the
scaling because there is nothing to choose. The rule below about versioning
the meaning of bytes still governs the save format.
**Affects:** `iris.h` PART 5 (`iris_internal_norm_in`) and PART 9
(`IRIS_FILE_VERSION`, `iris_save`, `iris_load`), `tests/audit.c` ("golden
blob: [-1,+1] training path bit-pinned"), `tests/load.c`, `tests/golden/`.

## Context

Inputs were once mapped to [0, 1]. Wekinator drives Weka's
`MultilayerPerceptron` with `normalizeAttributes` on, which scales
attributes to **[-1, +1]**: Weka's attribute normalisation (the brief is
archived with iris-studies S12). That is also what LeCun et al. 1998
("Efficient BackProp", §4.3) prescribe, for a concrete reason: with all-positive
inputs every weight into a hidden unit receives a gradient of the same sign, so
the descent has to zig-zag.

This is a correction towards Wekinator, not a deviation from it.

50 demonstrations, 8 outputs, 12 hidden units, mean of 9 seeds (recorded;
program in iris-studies S12):

| epochs | [0,1] training error | [-1,+1] training error | [0,1] grid error | [-1,+1] grid error |
|---|---|---|---|---|
| 600 | 5.94e-4 | 6.38e-5 (9.3×) | 0.0129 | 0.0084 (1.54×) |
| 20 000 | 8.19e-6 | 6.41e-6 (1.3×) | 0.0064 | 0.0063 (no difference) |

**Most of the gain is in how fast training converges, and
[0017](0017-train-to-the-plateau-not-to-a-constant.md) already buys that.** At
convergence the two scalings generalise the same. The reason to make the
change is fidelity to the model Wekinator runs, and that reason stands on its
own.

## The danger

A stored weight means nothing except against the scaling it was trained in.
Change the scaling under a saved instrument and it still loads, still reports
itself trained, and still plays, and every prediction is wrong. Every
first-layer pre-activation shifts by `-sum_i w1[h][i]`, an arbitrary constant
per hidden unit. The mapping is not degraded; it is a *different* mapping. The
musician loads the instrument they practised for a year, hears something else,
and has nothing to point at. That is the failure Fiebrink and Sonami describe
(NIME 2020, the conference on New Interfaces for Musical Expression). An
instrument saved on one scaling and read on the other played outputs that
differed by 0.427 of full scale over 441 probes (recorded; iris-studies S12).

## Decision

**What the bytes mean is fixed by the format version, and by nothing else.**
Not a build flag, not a global, not the caller.

When this record was accepted, two scalings coexisted and the version word
selected between them. In 0.2.0 there is one scaling and one format:
`iris_load` refuses any file whose version is not 7, and a format 7 file is
always on [-1, +1]. A future change to the scaling, or to anything else that
changes what stored weights mean, is a new format version.

**Specify the writer as explicitly as the reader.** A format whose meaning can
vary must say what `iris_save` writes, not only what `iris_load` reads. A
version word written from a constant ("the newest format this build knows")
rather than from the instrument's actual state labels old-meaning weights with
the new meaning, and the file round-trips bit-perfectly, reports itself
trained, and plays a different mapping. The reader alone is half a
specification.

## Rejected alternative: a build-time flag

An `#ifdef` choosing the scaling would have been three lines. It is exactly
wrong: the same file would load correctly or incorrectly depending on who
compiled the firmware, and nothing in the file would say which. Versioning the
format costs one number and makes the failure impossible rather than unlikely.

## Consequences

- A format has as many loaders as it has readers, and a version change has to
  visit every one of them.
- Moving an instrument to a new scaling is a new fit from its stored
  demonstrations, not a conversion of its weights, so it is never something
  `iris_load` does on the musician's behalf. Fiebrink and Sonami's users lost
  technique to retraining they did not ask for.
- Re-derive the pinned path: `sh build.sh audit` and `sh build.sh load`.

Measurements: iris-studies S12.
