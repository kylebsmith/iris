# 0011 — The graveyard, with numbers

**Status:** accepted, 2026-08-21 (kills recorded from the frontier campaign —
surveys plus experiments E1–E8)
**Affects:** nothing in `src/` — this ADR exists so eight dead ideas stay dead
until someone brings new numbers. `docs/frontier/REPORT.md` holds the full
context; this is the citable index of the kills.

## Context

The frontier campaign measured everything it shipped (ADRs 0003–0010) — and
everything it refused. The refusals are the more perishable knowledge: chosen
code defends itself by existing, but a killed idea comes back every time a new
contributor reads the same blog posts that proposed it the first time. Several
of the entries below are fashionable, well-cited, or shipped by projects this
one is compared against — which is exactly why each needs a number on the
headstone, from a program in this repository or the frontier scratch forks
that anyone can re-run.

## Decision

None of the following ships in v0.2. Each entry records the killing number,
the program that produced it, and the revival condition. Re-litigating any of
them without new numbers is out of order — cite this ADR and move on.

1. **int8/int16 weight quantization.** The entire weight set at 2-12-3 is 75
   floats = 300 bytes in an 8,192-byte arena that is mostly sacred-float32
   examples; inference is 0.8 µs and was never the bottleneck. Measured
   anyway (`quantnum.c`, real trained net, 41×41 grid): int8 per-tensor error
   1.10% of parameter range max — a real zipper risk on resonant parameters;
   int16 0.0064% — inaudible, buying nothing. There is nothing to quantize
   *for*. Revive: only as a wire format, if files (~1 KB) ever matter.

2. **Q15/Q7 fixed-point training.** Two independent fatal wounds. Gradient
   underflow: Q15 at the measured ±4.0 weight range has resolution 1.22e-4;
   late-training updates are ~1e-5 — below one quantum, so learning freezes
   in exactly the fine-convergence phase the musician is waiting for, and
   the known fix (stochastic rounding) injects RNG into every update, which
   breaks the determinism contract (ADR 0003). Silicon: the LX7 FPU issues
   float MACs at ~1-cycle throughput, so scalar int16 is not faster than
   scalar float. Revive: never on this class of FPU; the 30-minute
   quantum-rounding harness is specified in the survey for doubters.

3. **RMSProp** (memlp's optimizer, decay 0.9): 1,991–13,884 iterations to
   match the 600-epoch baseline — up to **10.7× slower than the shipping
   SGD** at 200 examples (`opt_bench.c`). The weakest optimizer in the
   field, and a documented reason not to inherit another library's judgment
   without measuring it. Revive: don't.

4. **Lion.** Its one selling point is dropping Adam's second-moment buffer;
   the baseline already runs on a single momentum buffer, so there is no
   memory to save. Sign-quantized updates discard exactly the
   gradient-magnitude information a 60–230-weight full-batch problem has;
   its reported wins are large-batch, large-model. Killed by arithmetic
   rather than a run — the 30-line resurrection experiment is specified in
   the survey if anyone objects.

5. **One-cycle / cosine / warmup LR schedules.** The measured gap between
   any scheduled first-order method and L-BFGS/LM is 10–40× — larger than
   the most optimistic schedule gain. Tuning schedules on SGD is dominated
   work. Revive: only if the second-order trainers somehow leave.

6. **Shrink-and-perturb.** Strictly worse than plain warm-start at every
   small budget measured (E4: train_rms 0.0263 vs 0.0190 at 30 epochs;
   drift 0.0158 vs 0.0035). The warm-start generalisation gap it exists to
   fix does not manifest at 5–50 examples — where users actively *prefer*
   overfit, easily-influenced models (Fiebrink, CHI 2011). Fashionable,
   cited, dead.

7. **Recency boosting** — presenting the new example k extra times per
   epoch, the intuition E4 was named to test. k=1 nearly doubles
   epochs-to-parity (31 vs 17); k≥2 never reaches parity in 60 epochs and
   oscillates on contradictory corrections (prediction swing 0.144 at k=2
   vs 0.022 at k=0). Strictly counterproductive on every metric except raw
   new-example error. Ships nowhere, not even as a knob (ADR 0005).

8. **Model soups.** Weight-averaging two rerolls with grid RMS 0.0126 and
   0.0134 produces grid RMS **0.1164 — 9× worse than either parent**
   (`soup_probe.c`). Independently seeded nets are permutations of each
   other's hidden units (Git Re-Basin); averaging across the mismatch is
   garbage, and same-basin averaging is a measured no-op (0.0131). Revive:
   only if 12-unit permutation alignment beats both the ensemble mean and
   warm-start on some measured axis; expected outcome null.

9. **Levenberg-Marquardt as the default trainer.** Killed by its own
   pre-registered criterion, and it died to *boring*: the warm-start+LM
   hybrid reaches baseline-equal fit in 0.58–0.69× the wall-clock **of
   L-BFGS** — i.e. L-BFGS is outright faster there — and LM needs a W×W
   arena slice (208 KB at 32 hidden; 39.7 MB refused at max topology).
   Buried with full honours: every other gate passed, and the regime
   inversion is real — LM's error floor is 2–5× lower than anything else
   measured (err ~1.3e-5 in ~2.3 ms S3 est. at 20 examples, where L-BFGS's
   float32 line search stalls at 3–9e-5). Revive: behind `#ifdef IRIS_OPT_LM`
   the day a "finalize instrument" gesture exists (code archived, E3). Also
   recorded so folklore dies with its evidence: pure LM's alleged stall on
   underdetermined configs did not reproduce — it was an artifact of the
   prototype's additive jitter, not intrinsic to LM.

## Rejected alternatives

**Scatter the kills across the ADRs that chose their competitors.** Rejected:
RMSProp's death arguably belongs with ADR 0007 and recency's with 0005, but
quantization, fixed-point and soups have no chooser ADR to live in — half the
graveyard would be homeless and citing a kill would need a treasure map. One
document, one number per headstone.

**Do not record kills at all; the code speaks.** Rejected: the code is silent
about what it does not contain. Absence reads as oversight, and oversights
get "fixed". Every entry above will be proposed again by someone arguing in
good faith from general knowledge; the whole value of the campaign is that
this project gets to answer with a measurement instead of an opinion.

**Record the kills without revival conditions.** Rejected: a kill without a
revival condition is dogma, and dogma ages worse than code. Each entry states
what new evidence would reopen it — for most, a measurement beating the
recorded number; for two (RMSProp, fixed-point-on-LX7), honestly, nothing.

## Consequences

- The bar for reopening any entry is written down: new numbers, measured
  against the recorded ones, or the proposal is answered by citation.
- LM is the one corpse kept warm: the archived E3 fork is a validated
  deep-polish trainer awaiting a product gesture, and this ADR is the record
  of why it waits instead of shipping.
- Two entries (Lion, fixed-point rounding) are killed by arithmetic with the
  resurrection experiment pre-specified — the cheapest possible courtesy to
  a future objector.
- Re-derive: `opt_bench.c`, `quantnum.c`, `soup_probe.c` and the E3/E4 forks
  under the frontier scratch directory (`ml-frontier/`); survey documents
  alongside. Measured on Apple M4 Max (arm64), Apple clang 17.0.0, `-O2`,
  2026-08-21.
