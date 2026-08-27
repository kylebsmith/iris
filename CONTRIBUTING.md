# Contributing

## The one rule

**Every claim carries its evidence.** A number in a comment names what produced
it. A comparison names its baseline. If something is estimated rather than
measured, it says so. This project has already been through an audit that found
nine documentation claims that were false where they stood — the discipline
exists because we needed it.

## Before you open a pull request

```sh
sh build.sh audit && sh build.sh mpe && sh build.sh sinks
```

All three must print zero failures. `audit` includes the golden output hashes:
if those change, you have altered what the library computes. That is sometimes
correct — but it is never incidental, and the pull request must say so and
re-pin them deliberately.

## What goes where

- **`iris.h` is the core.** It has no dependencies and no allocations, and those
  two properties are checked in CI. A change that adds either will be rejected.
- **New output formats go in `ports/`.** Copy `ports/template/` and implement the
  sink interface in `iris_sink.h`. Nothing above the port layer may know what
  your hardware is.
- **New sensors go behind `iris_source.h`.** Same rule in the other direction:
  the core must not be able to tell what is producing the numbers.
- **Anything experimental goes in `experimental/`** and does not ship in the
  core until it has a production caller and a measurement.
- **Negative results go in `docs/negative-results/`.** Things that did not work
  are worth keeping. They are not worth keeping in the engine room.

## Style

Match the file you are editing. The comment density is deliberate: this is a
teaching artifact as much as a library, and the prose carries measurements and
reasoning that would otherwise be lost. Explain *why*, not *what*.

No dead code. No commented-out code. If it is worth keeping, it is worth a
negative-results note explaining what it measured.

## Reporting a defect

Include the compiler, the optimisation level, and — if it is a behaviour
difference — the seed. Determinism is a contract here, so a reproduction should
be exactly reproducible.
