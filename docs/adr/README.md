# Architecture decision records

Measurements behind these records, and the records this project no longer
follows, are in [iris-studies](https://github.com/kylebsmith/iris-studies).

| Removed record | Where it is |
|---|---|
| 0001, the reroll check probes the gaps | iris-studies S07, `archive/docs/adr/0001-reroll-check-probes-the-gaps.md` |
| 0002, the output-port boundary | iris-studies `archive/docs/adr/0002-the-sink-boundary-and-the-mpe-encoder.md`, with the ports in `archive/extras/` |
| 0005, warm start as the default correction | iris-studies S13, `archive/docs/adr/0005-warm-start-is-the-default-correction.md` |
| 0006, save format 2 | iris-studies `archive/docs/adr/0006-format-v2-one-word-v1-loader-permanent.md` |
| 0007, L-BFGS (the limited-memory Broyden–Fletcher–Goldfarb–Shanno method) as the fast trainer | iris-studies S09, `archive/docs/adr/0007-lbfgs-m5-is-the-fast-trainer.md` |
| 0011, ideas measured and rejected | iris-studies S19, `archive/docs/adr/0011-the-graveyard-with-numbers.md` |
| 0012 to 0016 and 0020, the application iris was extracted from | iris-studies `archive/docs/adr/` |
| 0021, which trainer to recommend | iris-studies S08, `archive/docs/adr/0021-the-one-recommended-trainer.md` |

An architecture decision record (ADR) is a short note that records **one
decision that had a real trade-off**, written **before** the code that
implements it, and kept afterwards whether or not the decision turns out well.

It is not documentation. Documentation says what the code does; you can read the
code for that. An ADR says *why this and not the other thing*, and names the
other thing explicitly. Six months later the code will look obvious and the
rejected alternative will look tempting again. The ADR is what stops that
argument from being had twice.

## When one is required

Write an ADR when:

- a settled non-negotiable is being changed: this also needs the
  maintainer's sign-off, and the ADR is written and shown before the change
  lands;
- a test's *meaning* changes, not just its wiring (a threshold that moves, a
  property that is asserted differently, a check that is removed);
- a number that appears in the README or the header is being replaced;
- two parts of the project disagree and one of them has to give;
- the obvious cheap fix is being rejected in favour of a more expensive one.

You do not need an ADR to rename a variable, fix a typo, or add a check that
asserts something nobody disputes.

## Rules

1. **Before the code.** An ADR written afterwards is a press release.
2. **Record the rejected alternative.** An ADR with no rejected alternative is
   describing a decision that was never actually made.
3. **Measured, not estimated.** Numbers come from a program in this repository
   or a study in iris-studies that anyone can re-run. Say which program or
   study, and on what machine.
4. **Never reverse a decision in place.** If it turns out wrong, write the
   next-numbered ADR that supersedes it, and add a line at the top of the old
   one pointing at it. Where the code has moved on without a new record, the
   status line says which part no longer holds.

## Format

`NNNN-short-hyphenated-title.md`, numbered in the order written, never renumbered.
Sections: Status, Context, Decision, Rejected alternatives, Consequences, and a
last line naming the iris-studies study that holds the measurements.

## Index

| # | title | status |
|---|---|---|
| [0003](0003-bit-identity-is-a-contract-not-an-accident.md) | Bit-identity is a contract, not an accident | accepted |
| [0004](0004-guards-report-never-mutate.md) | Guards report, never silently mutate | accepted |
| [0008](0008-elm-same-network-better-math.md) | The closed-form trainer: same network, different mathematics | accepted |
| [0009](0009-ridge-is-mandatory.md) | The ridge is mandatory: the single-precision normal matrix is rank-deficient even on friendly data | accepted |
| [0010](0010-knn-the-sampler-beside-the-morpher.md) | Nearest neighbour: the sampler beside the morpher | accepted |
| [0017](0017-train-to-the-plateau-not-to-a-constant.md) | Train to the plateau, not to a constant | accepted; the default under noise is contested (iris-studies S08) |
| [0018](0018-the-input-scaling-travels-with-the-file.md) | Inputs are scaled to [-1, +1], and the scaling travels with the file | accepted in part: the scaling holds; the file no longer carries it |
| [0019](0019-the-residual-ledger-integrates-it-does-not-sample.md) | The bad-example detector integrates the residual; it does not sample the endpoint | accepted |
