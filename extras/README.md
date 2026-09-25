# extras

**Nothing in the library calls anything in here.** That is the whole reason it
is a separate directory.

`iris.h` is the library. These are things built around it that have not earned
their way into it:

- `ports/` — where sound goes out: control change messages in MIDI (the
  Musical Instrument Digital Interface, the standard way synthesisers talk),
  polyphonic expression, Open Sound Control, a null port and a template to
  copy. The control-change port is the one a sketch would realistically use;
  the rest show that the boundary is real.
- `iris_sink.h`, `iris_source.h` — the two port interfaces those implement.
- `bench/` — the same core compiled to WebAssembly, running in a browser.
  The page comes from the application iris was extracted from and keeps its
  vocabulary ("the glass" is the page's drawing canvas). Its file loader
  still expects that application's files (the magic word `EWEK`, formats 1
  and 2), so it refuses the format 7 files the page itself saves, under the
  name `.ewk`. Training and playing in the page do not touch files.
- `tests/` — the tests for the above. Run them with `sh build.sh mpe` and
  `sh build.sh sinks` from the repository root.

Keeping them here means a person landing on the repository sees one header at
the top level and knows what to read first. None of it changes the library by
one byte.
