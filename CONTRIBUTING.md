# Contributing

This is how not to break iris. Nothing here is about the library's design —
that is in `README.md` and `docs/`. This is about the rules the project holds
itself to, and the specific ways it has been broken before.

## The first rule: behaviour is frozen

Two hashes are pinned as live assertions:

```
fnv1a 0xFEFAEDF6      the [0,1] training path
fnv1a 0x6805FB0D      the [-1,+1] training path
```

If a change moves either, a saved instrument now plays differently than it did.
That is the single thing this library promises never to do — a musician cannot
practise an instrument that gets restrung underneath them.

So: **never change the arithmetic.** Not the activation, not the update rule,
not the output scaling, not the save format's meaning. If a hash moves and you
did not deliberately intend to redefine what every existing instrument sounds
like, you have made a mistake — revert, do not re-pin.

Free to change: anything additive to the interface, diagnostics, error
messages, documentation, the output ports, and the microcontroller sketches.

## The second rule: every claim carries its evidence

A number in a comment names what produced it. A comparison names its baseline.
If something is estimated rather than measured, it says so. This project has
already been through an audit that found nine documentation claims that were
false where they stood — the discipline exists because we needed it.

## Commands

```sh
sh build.sh audit      # 43 correctness checks, including the frozen hashes
sh build.sh claims     # fails the build when a document contradicts the code
sh build.sh fuzz       # sanitizers decide, not our assertions. See below.
sh build.sh tiny       # an independent, much smaller reimplementation
sh build.sh mpe        # the polyphonic-expression encoder, byte level
sh build.sh sinks      # the control-change and network output ports
```

Run `audit` and `claims` after **every** change to `iris.h`. They are fast and
they have both caught real regressions.

`sh build.sh golden` is **destructive** — it rewrites the frozen baselines the
audit compares against. It refuses without an explicit environment variable. Do
not set that variable to make a failing test pass. A failing golden check means
behaviour changed, which is the thing it exists to tell you.

## Do not trust the tests, including when they are green

The test suite was written by the same author as the library. Code agreeing
with its own author's expectations is not evidence. This has already failed
four times, and each was found by someone tripping over it, not by the suite:

- A test compared two builds to prove a compile flag changed behaviour. The two
  builds were byte-identical; the flag never applied. It passed for months
  while testing nothing.
- The version check counted one macro and never noticed the file's own header
  comment stated a different version.
- `make_golden` regenerated the backward-compatibility fixture in the *current*
  file format, destroying the only artefact proving old files still load — on
  every run.
- The continuous-integration job guarding the two frozen hashes above piped the
  audit into `grep "golden blob"`. That text is printed on the failing line as
  well as the passing one, and the pipe handed the exit status to grep, so the
  check reported success while both hashes were wrong.

Practical consequence: when you add a check, **prove it can fail.** Break the
thing on purpose, watch the check go red, then restore. A check never observed
failing is decoration.

`sh build.sh fuzz` is the one test that encodes none of our expectations — it
allocates every caller array at exactly the shape's size and lets
AddressSanitizer decide. Prefer adding checks of that kind.

## Traps that have bitten before

- **The shape exists in three places** — `IRIS_ARENA(...)`, `iris_init(...)`,
  and the caller's array declarations. Every bug in this family is those three
  drifting apart, and C will not warn you. `iris_predict` writes one float per
  output into whatever you handed it.
- **Unsigned comparisons.** `bytes < iris_size(...)` was a memory-safety bug
  found during pre-release development: `iris_size` returns 0 for shapes it
  rejects, and `bytes < 0` is false forever on an unsigned type, so the bound
  silently ceased to exist.
- **`iris.h` is duplicated** into each sketch folder in the starter kit
  so students need no install step. After editing the header, run
  `sh sync-iris.sh` there or the copies go stale.
- **Structure sizes differ by architecture.** `IRIS_ARENA(2,12,1,8)` is 944
  bytes on the ESP32-S3 and 1,032 on a 64-bit laptop. Never quote one as the
  other; measure on the target. Both of those numbers were 8 bytes smaller in
  this file until 0.1.0, because `struct iris` grew and the prose did not
  follow — which is the trap, stated twice.
- **Comments have been wrong.** One stated a measurement off by a factor of
  eight. Verify a comment before relying on it, and fix it when it is wrong.

## What goes where

| Path | What |
|---|---|
| `iris.h` | The whole library. Start at the masthead. |
| `examples/` | `00_minimal.c` is the smallest one. `iris_smallest/` is the Arduino one. |
| `tests/` | `audit.c` is ours and self-referential. `fuzz.c` is not. |
| `extras/ports/` | Where sound goes out: control change, polyphonic expression, network. |
| `docs/` | Design notes and decision records. `tiny.c` is the independent check. |
| `experimental/` | Not in the core. Read its header before citing it. |

- **`iris.h` is the core.** It has no dependencies and no allocations, and those
  two properties are checked in continuous integration. A change that adds
  either will be rejected.
- **New output formats go in `extras/ports/`.** Copy `extras/ports/template/`
  and implement the sink interface in `extras/iris_sink.h`. Nothing above the
  port layer may know what your hardware is.
- **New sensors go behind `extras/iris_source.h`.** Same rule in the other
  direction: the core must not be able to tell what is producing the numbers.
- **Anything experimental goes in `experimental/`** and does not ship in the
  core until it has a production caller and a measurement.
- **Negative results go in `docs/negative-results/`.** Things that did not work
  are worth keeping. They are not worth keeping in the engine room.

## Writing anything a person will read

- **Never state a number or a claim you have not just verified.** `build.sh
  claims` polices some of them; it does not police prose in code comments.
- **No bare acronyms, ever.** Spell the term out and explain it in the same
  sentence. The audience includes musicians and first-year students. This is
  not a request to simplify the content.
- **No marketing voice, no platitudes, no hedging.** Say the thing.
- Do not claim the library is better than, faster than, or first at anything.
  `build.sh claims` will fail the build for several such phrasings.
- Zero dependencies is a promise about **`iris.h` only**. The Arduino sketches
  use Adafruit drivers and that is fine — a display driver is replaceable
  plumbing, whereas the code deciding how a gesture becomes sound is not.

Match the file you are editing. The comment density is deliberate: this is a
teaching artifact as much as a library, and the prose carries measurements and
reasoning that would otherwise be lost. Explain *why*, not *what*.

No dead code. No commented-out code. If it is worth keeping, it is worth a
negative-results note explaining what it measured.

## Before you open a pull request

1. `sh build.sh audit` — 43 checks, both hashes unmoved
2. `sh build.sh claims` — documents match the code
3. `sh build.sh fuzz` — no sanitizer report
4. `sh build.sh mpe` and `sh build.sh sinks` — zero failures
5. If `iris.h` changed: run `sh sync-iris.sh` in the starter kit
6. If a sketch changed: compile it, with `--warnings all`, and confirm zero
   diagnostics from our own files

`audit` includes the golden output hashes: if those change, you have altered
what the library computes. That is sometimes correct — but it is never
incidental, and the pull request must say so and re-pin them deliberately.

Report what actually happened. If a step was skipped, say so.

## Reporting a defect

Include the compiler, the optimisation level, and — if it is a behaviour
difference — the seed. Determinism is a contract here, so a reproduction should
be exactly reproducible.
