# 0001 — The reroll check probes the gaps, not the demonstrations

**Status:** accepted, 2026-08-21
**Affects:** `tests/audit.c` checks 6 and 7. `iris.h` is unchanged.

## Context

`audit.c` check 6 claimed to prove that rerolling gives you a different
instrument. It trained the same 20 examples from 8 different random starts at
800 epochs, predicted at the single gesture `(0.37, 0.62)`, and asserted that the
8 answers differed by more than `0.01`. It measured **0.0092** and failed.

Two programs in this repository disagreed about the same property, and only one
of them had evidence.

`experiment.c` sweeps hidden units × examples × epochs, 12 random starts each,
and reports the spread twice: once over probes that sit **near demonstrations**
(`iris_novelty < 0.15`) and once over probes **in the gaps** (`iris_novelty > 0.35`).
Run on this machine **[measured]**, the gap spread shrinks monotonically as the
model becomes better constrained:

| hidden | examples | epochs | near demos | in the gaps |
|---|---|---|---|---|
| 4 | 5 | 400 | 0.0203 | 0.0649 |
| 4 | 5 | 1600 | 0.0183 | 0.0964 |
| 4 | 20 | 1600 | 0.0138 | 0.0071 |
| 12 | 5 | 400 | 0.0103 | 0.0241 |
| 12 | 20 | 1600 | 0.0082 | 0.0053 |
| 32 | 20 | 1600 | 0.0054 | 0.0026 |
| 12 | 40 | any | — | no gaps at all |

So check 6 was doing three things wrong at once:

1. **Wrong place.** It probed `(0.37, 0.62)`, an interior point that 20 scattered
   demonstrations pin down. Where the demonstrations constrain the model, every
   random start is pulled to the same answer — that is the model working, not
   failing. Rerolling is *supposed* to leave that point alone.
2. **Wrong configuration.** 12 hidden / 20 examples / 800 epochs is, by
   `experiment.c`'s own table, close to the least lively corner of the space.
   The check asked for liveliness at the setting chosen to suppress it.
3. **Wrong shape of evidence.** One gesture, one number. A single probe is a
   coin-flip about whether it happened to land in a gap.

The measured 0.0092 was not a bug in `iris.h`. It was the correct answer to a
badly asked question.

There is also a second property, which is the one the project actually promises
a musician and which nothing was checking at all: rerolling must **not** move the
sound where you demonstrated it. "Reroll" is only a usable control if pressing it
costs you nothing you taught. Liveliness alone is satisfied by a model that has
learned nothing.

## Decision

Replace check 6 with a two-sided pair of checks that reuse `experiment.c`'s own
definitions verbatim, so the two programs can no longer mean different things by
the same words:

- **near demos** = probe points with `iris_novelty(k, p) < 0.15`
- **in the gaps** = probe points with `iris_novelty(k, p) > 0.35`
- **spread** = for each probe, the largest disagreement between any two of the
  8 rerolls on any output; then the mean of that over the probes in the region.

Both checks run at **4 hidden / 5 examples / 800 epochs**, a configuration
`experiment.c` marks `<-- lively`, over the same 21×21 grid `experiment.c` uses.
The audit's other checks keep their 12-hidden / 20-example model; this pair gets
its own small arena.

**Check 6 — reroll is lively in the gaps.** `gap spread > 0.03`.
**Check 7 — reroll is steady at the demonstrations.** `near spread < 0.04`.

Neither number is worth anything on its own. Check 6 alone passes for a model
that has learned nothing; check 7 alone passes for a model that ignores its seed.
The pair is the claim: *steady where you taught it, lively where you didn't.*

### Where the thresholds come from

Measured on this machine at 4 hidden / 5 examples / 800 epochs, 8 seeds, 51 near
probes and 220 gap probes **[measured]**:

| | measured | threshold | headroom |
|---|---|---|---|
| in the gaps | 0.0763 | > 0.03 | 2.5× |
| near demos | 0.0165 | < 0.04 | 2.4× |

Across 100 / 200 / 400 / 800 epochs at this size the gap spread stays in
0.058–0.086 and the near spread in 0.0165–0.0399, so the thresholds hold over
the whole epoch range, not just the one we ship. The ratio between the two
regions is 4.6× as measured, against the 1.3× that the thresholds demand.

The thresholds are deliberately not tight. A test that passes at 1.05× its
threshold is a test that will fail on someone else's compiler for reasons that
have nothing to do with the code.

## Rejected alternatives

**1. Lower the `0.01` threshold until check 6 passes.** Rejected. It measured
0.0092 against `> 0.01`; `> 0.008` would have turned the suite green in one
character. This is the alternative that had to be written down, because it is
the one that will be proposed again.

It fails for a reason worth stating plainly: the check would then assert that at
a well-constrained interior point, 8 rerolls differ by *at least* 0.008 on a 0–1
parameter scale — a difference far below anything audible, at the one place we
want rerolling to change nothing. It would be a green check standing guard over a
property we do not want and cannot hear. And it would go on drifting: every
future improvement that makes training converge better — more units, more
examples, a better optimiser — would push the number down again and the threshold
would have to be lowered again. A test that has to be weakened each time the code
gets better is measuring the wrong thing.

**2. Delete check 6.** Rejected. Reroll is the feature that distinguishes this
from a deterministic mapping. It needs a check, just a truthful one.

**3. Keep the single-point probe and only move it into a gap.** Rejected. One
probe is one sample. Whether it lands in a gap depends on where the examples
happen to fall, which changes if anyone touches `load_examples`. Averaging over
whichever grid points the novelty measure calls a gap is stable against that.

**4. Assert only the liveliness half.** Rejected. It is the half that is easy to
satisfy and the half a musician does not care about. "Retraining did not destroy
what I taught you" is the promise; asserting only its complement would leave the
promise untested.

**5. Have `experiment.c` and `audit.c` share a header of common definitions.**
Rejected for now — the duplication is about fifteen lines and the tests are meant
to be readable end to end without chasing includes. Revisit if a third test needs
the same code.

## Consequences

- The audit is now **eleven** checks, not ten. `README.md`, the `audit.c` header
  comment and the layout table all say eleven.
- The reroll checks report region means over 271 probes instead of one number at
  one gesture, so a regression shows up as a trend rather than a coin flip.
- `audit.c` now runs 16 extra trainings (8 seeds × 2 regions share the same 8
  runs, plus the small model is cheap). Total audit runtime is unchanged to the
  nearest tenth of a second.
- The audit and the experiment now use the same two words — *near demos*, *in the
  gaps* — for the same two things. If `experiment.c`'s novelty bands ever move,
  the audit must move with them, and that coupling is intentional.
- We have given up the ability to say "reroll changes the sound at gesture X".
  We can now only say "reroll changes the sound in the region the model was not
  taught". That is the weaker sentence and the true one.

## How to re-derive every number here

```sh
cd firmware/library
./build.sh experiment   # the sweep table
./build.sh audit        # checks 6 and 7, and the timings
```

Measured on Apple M4 Max (arm64), Apple clang 17.0.0, `-O2`, 2026-08-21.
