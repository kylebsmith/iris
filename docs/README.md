# docs

What each document here is. Three kinds live side by side:

- **Current**: describes iris 0.2.0, and is kept true with it.
- **Historical**: a record of how v0.1.0 was designed, measured and decided.
  Its line numbers, function names, counts and some figures describe the code
  of its date, not the current code; where a figure did not replicate, the
  header and the README say so and quote the replicated one.
- **Predecessor**: written inside the unpublished instrument application iris
  was extracted from. Its paths point into that application's tree and will
  not resolve here.

For what is true now, read [`../iris.h`](../iris.h) and
[`../README.md`](../README.md) first.

## Explanations and programs

| Document | Kind | What it contains |
|---|---|---|
| [SYSTEM-technical.md](SYSTEM-technical.md) | Current | The system, model, trainers, relation to Wekinator and Weka, costs and limitations, for a technical reader. |
| [SYSTEM-plain-english.md](SYSTEM-plain-english.md) | Current | The same, explained from scratch for someone who has never trained a network. |
| [DEGREES-OF-FREEDOM.md](DEGREES-OF-FREEDOM.md) | Current | How many inputs and outputs an instrument can afford; its tables reproduce with `degrees-of-freedom.c` on 0.2.0. |
| [degrees-of-freedom.c](degrees-of-freedom.c) | Current | The program behind DEGREES-OF-FREEDOM.md. |
| [gain-sweep.c](gain-sweep.c) | Current | The sweep behind the closed-form trainer's hidden-layer gain; prints the table in `iris.h` PART 8d. |
| [tiny.c](tiny.c) | Current | The network and update rule written out by hand, compared with the library on one task (`sh build.sh tiny`). |
| [negative-results/positional-encoding.md](negative-results/positional-encoding.md) | Historical | A positional-encoding option measured and removed before v0.1.0, with its code and numbers. |

## Studies and design records from v0.1.0

| Document | Kind | What it contains |
|---|---|---|
| [DESIGN.md](DESIGN.md) | Historical | The API and architecture planned before the library was written; the interface it specifies never shipped. |
| [FREEZE.md](FREEZE.md) | Historical | What a saved instrument's meaning depended on at v0.1.0, and the nonlinearity comparison; its 3.8% activation figure did not replicate (the header quotes 1.3%). |
| [KNOB-AUDIT.md](KNOB-AUDIT.md) | Historical | The study behind removing the learning rate and momentum from the interface and keeping smoothing; several of its figures did not replicate (the README quotes the replicated ones). |
| [MATH-AUDIT.md](MATH-AUDIT.md) | Historical | A review of the mathematics against the literature: names, provenance, and the objections a machine-learning reviewer would raise. |
| [MATH-FIXES.md](MATH-FIXES.md) | Historical | The fixes those objections led to, with the measurements taken when they were applied. |

## Architecture decision records

An architecture decision record, or ADR, notes one decision with a real
trade-off and the alternative it rejected. [adr/README.md](adr/README.md) says
how they are written.

| Record | Kind | What it decided |
|---|---|---|
| [0001](adr/0001-reroll-check-probes-the-gaps.md) | Historical | The reroll check measures the gaps between demonstrations, not the demonstrations. |
| [0002](adr/0002-the-sink-boundary-and-the-mpe-encoder.md) | Historical | The output-port boundary is three function pointers; the polyphonic-expression encoder holds no hardware. |
| [0003](adr/0003-bit-identity-is-a-contract-not-an-accident.md) | Historical | Bit-identical results are a contract, enforced by tripwires, a pragma and golden hashes. |
| [0004](adr/0004-guards-report-never-mutate.md) | Historical | Guards report through the status and never change anything silently. |
| [0005](adr/0005-warm-start-is-the-default-correction.md) | Predecessor | Warm start as the default correction; superseded, and 0.2.0 recommends `iris_train` after any delete. |
| [0006](adr/0006-format-v2-one-word-v1-loader-permanent.md) | Historical | Save format 2 as one added word; 0.2.0 reads format 7 only. |
| [0007](adr/0007-lbfgs-m5-is-the-fast-trainer.md) | Historical | L-BFGS (the limited-memory Broyden–Fletcher–Goldfarb–Shanno method) as the fast trainer; it has since left the library. |
| [0008](adr/0008-elm-same-network-better-math.md) | Historical | The closed-form trainer solves the same network in logit space. |
| [0009](adr/0009-ridge-is-mandatory.md) | Historical | The closed-form solve needs a ridge even on friendly data. |
| [0010](adr/0010-knn-the-sampler-beside-the-morpher.md) | Historical | Nearest-neighbour playing beside the network, and how the two differ. |
| [0011](adr/0011-the-graveyard-with-numbers.md) | Historical | Ideas measured and rejected, with their numbers. |
| [0012](adr/0012-the-hold-is-the-explicit-act.md) | Predecessor | Explicit training as a hold-to-set act in the application; superseded there. |
| [0013](adr/0013-the-rig-table-is-the-only-registration.md) | Predecessor | How the application registered its sensor rigs. |
| [0014](adr/0014-the-glass-is-a-view-not-the-sensor.md) | Predecessor | How the application's touch screen related to the input space. |
| [0015](adr/0015-ewp4-names-its-columns.md) | Predecessor | The application's save file naming its columns. |
| [0016](adr/0016-cc-is-the-default-sink.md) | Predecessor | MIDI (Musical Instrument Digital Interface) control change as the application's default output. |
| [0017](adr/0017-train-to-the-plateau-not-to-a-constant.md) | Historical | Training to a plateau instead of a fixed epoch count. |
| [0018](adr/0018-the-input-scaling-travels-with-the-file.md) | Predecessor | Inputs to [-1, +1] with the scaling carried by the file; 0.2.0 has one scaling and no flag for it. |
| [0019](adr/0019-the-residual-ledger-integrates-it-does-not-sample.md) | Predecessor | The worst-demonstration detector integrates the residual over training, as the current ledger still does. |
| [0020](adr/0020-settle-is-a-separate-act-from-train.md) | Predecessor | A separate SETTLE action in the application. |
| [0021](adr/0021-the-one-recommended-trainer.md) | Predecessor | Which trainer to recommend, marked provisional; 0.2.0 recommends `iris_train`. |
