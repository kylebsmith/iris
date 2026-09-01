# Architecture Decision Records

> **Where these decisions were made.** iris was extracted from a larger
> instrument-firmware project that is not published. Ten of the records below —
> 0005, 0012, 0013, 0014, 0015, 0016, 0018, 0019, 0020 and 0021 — were written
> inside that tree, and their file references (`firmware/`, `host/`, `core/`,
> `app.c`, `rig.h`, `admin/DECISIONS.md`) are to it, not to this repository.
> The same goes for two sources they cite, `docs/frontier/REPORT.md` and
> `BRINGUP-LOG.md`, which are not here either — including the on-device
> training measurement in 0021, which you therefore cannot re-run from this
> repository. The *decisions* are accurate and still govern `iris.h`; the
> *paths* will not resolve. Rule 3 below asks that numbers come from a program
> anyone can re-run, and for those records this repository cannot keep that
> promise, so it is said here rather than left to be discovered.

An ADR is a short note that records **one decision that had a real trade-off**,
written **before** the code that implements it, and kept afterwards whether or
not the decision turns out well.

It is not documentation. Documentation says what the code does; you can read the
code for that. An ADR says *why this and not the other thing*, and names the
other thing explicitly. Six months later the code will look obvious and the
rejected alternative will look tempting again. The ADR is what stops that
argument from being had twice.

## When one is required

Write an ADR when:

- a settled non-negotiable is being changed — this **also requires the PI's
  sign-off**, and the ADR must be written and shown before the change lands;
- a test's *meaning* changes, not just its wiring (a threshold that moves, a
  property that is asserted differently, a check that is removed);
- a number that appears in the README or the design doc is being replaced;
- two parts of the project disagree and one of them has to give;
- the obvious cheap fix is being rejected in favour of a more expensive one.

You do not need an ADR to rename a variable, fix a typo, or add a check that
asserts something nobody disputes.

## Rules

1. **Before the code.** An ADR written afterwards is a press release.
2. **Record the rejected alternative.** An ADR with no rejected alternative is
   describing a decision that was never actually made.
3. **Measured, not estimated.** Numbers come from a program in this repository
   that anyone can re-run. Say which program, and on what machine.
4. **Never edit a decision.** If it turns out wrong, write the next-numbered ADR
   that supersedes it, and add a line at the top of the old one pointing at it.

## Format

`NNNN-short-hyphenated-title.md`, numbered in the order written, never renumbered.
Sections: Status, Context, Decision, Rejected alternatives, Consequences.

## Index

| # | title | status |
|---|---|---|
| [0001](0001-reroll-check-probes-the-gaps.md) | The reroll check probes the gaps, not the demonstrations | accepted |
| [0002](0002-the-sink-boundary-and-the-mpe-encoder.md) | The sink boundary is three function pointers, and the MPE encoder holds no hardware | accepted |
| [0003](0003-bit-identity-is-a-contract-not-an-accident.md) | Bit-identity is a contract, not an accident | accepted |
| [0004](0004-guards-report-never-mutate.md) | Guards report, never silently mutate | accepted |
| [0005](0005-warm-start-is-the-default-correction.md) | Warm-start is the default correction; reroll is the only reseed | accepted |
| [0006](0006-format-v2-one-word-v1-loader-permanent.md) | Save format v2 is one added word; the v1 loader is permanent | accepted |
| [0007](0007-lbfgs-m5-is-the-fast-trainer.md) | L-BFGS(m=5) is the fast trainer; SGD+momentum retained verbatim | accepted |
| [0008](0008-elm-same-network-better-math.md) | ELM: same network, better math | accepted |
| [0009](0009-ridge-is-mandatory.md) | Ridge is mandatory: the float32 Gram is rank-deficient even on friendly data | accepted |
| [0010](0010-knn-the-sampler-beside-the-morpher.md) | k-NN/1-NN: the sampler beside the morpher | accepted |
| [0011](0011-the-graveyard-with-numbers.md) | The graveyard, with numbers | accepted |
| [0012](0012-the-hold-is-the-explicit-act.md) | Explicit training is the hold-to-set act; the field reshaping is its consequence | accepted |
| [0013](0013-the-rig-table-is-the-only-registration.md) | The rig table is the only registration | accepted |
| [0014](0014-the-glass-is-a-view-not-the-sensor.md) | The glass is a view of the input space, not the input space | accepted |
| [0015](0015-ewp4-names-its-columns.md) | The save file names its columns, so a rig change costs no demonstrations | accepted |
| [0016](0016-cc-is-the-default-sink.md) | MIDI CC is the default sink; nothing is privileged | accepted |
| [0017](0017-train-to-the-plateau-not-to-a-constant.md) | Train to the plateau, not to a constant | accepted |
| [0018](0018-the-input-scaling-travels-with-the-file.md) | Inputs are scaled to [-1,+1], and the scaling travels with the file | accepted |
| [0019](0019-the-residual-ledger-integrates-it-does-not-sample.md) | The bad-example detector integrates the residual; it does not sample the endpoint | accepted |
| [0020](0020-settle-is-a-separate-act-from-train.md) | SETTLE is a separate act from TRAIN, and it reseeds | accepted |
