# extras

**Nothing in the library calls anything in here.** That is the whole reason it
is a separate directory.

`iris.h` is the library. These are things built around it that have not earned
their way into it:

- `ports/` — where sound goes out: MIDI control change, polyphonic expression,
  Open Sound Control, a null port and a template to copy. 2,042 lines. The
  control-change port is the one a sketch would realistically use; the rest are
  demonstrations that the boundary is real.
- `iris_sink.h`, `iris_source.h` — the two port interfaces those implement.
- `bench/` — the same core compiled to WebAssembly, running in a browser.
- `tests/` — the tests for the above. Run them with `sh build.sh mpe` and
  `sh build.sh sinks` from the repository root.

An independent reviewer put it plainly: a person landing on this repository saw
a single-header library, a 1,600-line output layer nothing called, an
experimental optimiser and a browser benchmark, and could not tell what to
read. Moving these out does not delete anything and does not change the
library by one byte. It answers the question "what am I looking at?" at the
top level instead of three directories down.
