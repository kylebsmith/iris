# docs

What each document here is. Everything in this folder describes iris 0.2.0 and
is kept true with it; where a document and [`../iris.h`](../iris.h) disagree,
the header is right and the document is wrong. Read the header and
[`../README.md`](../README.md) first.

The measurements behind these documents, and the design records the library no
longer follows, are in
[iris-studies](https://github.com/kylebsmith/iris-studies), cited as
"(iris-studies Snn)".

## Explanations and programs

| Document | What it contains |
|---|---|
| [SYSTEM-technical.md](SYSTEM-technical.md) | The system, model, trainers, relation to Wekinator and Weka, costs and limitations, for a technical reader. |
| [SYSTEM-plain-english.md](SYSTEM-plain-english.md) | The same, explained from scratch for someone who has never trained a network. |
| [tiny.c](tiny.c) | `iris_train` written out again by hand, which must agree with the library to the bit on one task (`sh build.sh tiny`). |
| [board/](board/) | Logs of the starter kit's sketches run on hardware. Every board figure in the library cites one. |
| [negative-results/](negative-results/README.md) | Ideas that were measured and are not in the library, one paragraph each, with the study that holds the numbers. |

## Architecture decision records

An architecture decision record (ADR) notes one decision with a real
trade-off and the alternative it rejected. [adr/README.md](adr/README.md) says
how they are written and where the removed ones went.

| Record | What it decided |
|---|---|
| [0003](adr/0003-bit-identity-is-a-contract-not-an-accident.md) | Bit-identical results are a contract, enforced by compiler tripwires, contraction pragmas and golden hashes. |
| [0004](adr/0004-guards-report-never-mutate.md) | Guards report through the status and never change anything silently. |
| [0008](adr/0008-elm-same-network-better-math.md) | The closed-form trainer solves the same network's output layer in logit space. |
| [0009](adr/0009-ridge-is-mandatory.md) | The closed-form solve needs a ridge even on friendly data. |
| [0010](adr/0010-knn-the-sampler-beside-the-morpher.md) | Nearest-neighbour playing beside the network, and how the two differ. |
| [0017](adr/0017-train-to-the-plateau-not-to-a-constant.md) | Training to a plateau instead of a fixed epoch count; contested on noisy data. |
| [0018](adr/0018-the-input-scaling-travels-with-the-file.md) | Inputs scaled to [-1, +1]; what stored bytes mean is fixed by the format version. |
| [0019](adr/0019-the-residual-ledger-integrates-it-does-not-sample.md) | The worst-demonstration detector integrates the residual over training. |
