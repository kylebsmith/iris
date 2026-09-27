# 0024 — Names for inputs and outputs in saved files

**Status:** proposed, 2026-09-27. No decision; nothing here is code.
**Affects, if adopted:** `iris.h` PART 9 (the file format and its
number), and the tool that takes an instrument off the board
(`tools/iris_dump.c`).

## Context

A saved instrument holds numbers only: the weights, the ranges, the takes
and their identifiers. What each input was (which sensor axis, in which
unit) and what each output drives lives in the sketch's source. For a
performer's own instrument that is enough. For a lab collecting what
performers taught, it is not: a file taken off a board months later says
"3 inputs, 2 outputs", and the columns of the table `tools/iris_dump.c`
writes are named by position only. Takes are first-class research data only
if they say what they measured.

## Options

**A. No names in the file.** The lab keeps a record beside each file: the
sketch and its commit, or a short text naming the columns, and
`tools/iris_dump.c` can be given the names on its command line. Contract:
unchanged. Cost: discipline; a file separated from its record is anonymous.

**B. Names in the file, format 8.** A fixed number of bytes per input and per
output (16, say: 16 × (n_in + n_out) bytes, 80 for a 2-in, 3-out instrument
against its 872-byte format 7 file with 20 takes), written after the takes and
covered by the checksum. Format 7 cannot carry them: its length rule refuses
any byte past the checksum (PART 9), so a longer file is a new format number.
Every reader from format 7 on is kept, so format 7 files still load; a
format 8 file does not load in 0.3.0 or earlier. The names need not live in
the arena: `iris_save` could take them from the caller and `iris_load` hand
them back into the caller's buffer, so an instrument costs no memory for
them. No heap; the stack is unchanged.

**C. One free-form text block in format 8** (the sketch's name, a date, the
columns), instead of fixed fields. Same contract as B; more useful to a lab,
harder to check (what is a valid block?).

## Decision

Not made. The maintainer decides; until then nothing here is code.

## The measurement that would decide it

Not a measurement but a trial: collect instruments from a class or a study
under A for one term, with `tools/iris_dump.c` and a naming record beside
every file, and count the files that can no longer be matched to what their
columns were. If none, A holds. Format 8 should be decided once, for every
reason it might be needed (this record, 0022 B2, 0023, 0025), not once per
reason.

The file sizes quoted are the format table's arithmetic (PART 9) and
the saved files `tests/load.c` checks.
