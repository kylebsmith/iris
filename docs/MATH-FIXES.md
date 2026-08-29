# Prescription for `iris.h`

> **Historical record.** Describes fixes as applied on its date. Line numbers
> and counts refer to the file as it stood then.

Everything below was re-measured against the working tree at this repository (`iris.h` md5 unchanged, `./build.sh audit` = 40/40 PASS at baseline). Three candidate patches were applied to copies in scratch copies that were not retained and the full audit run on each; every hash and timing quoted in §4 and §7 is from those runs, not estimated.

---

## DEFECT 1 — the activation leaves its own codomain, and the backward pass is not the derivative of the forward pass

### 1. The defect

`iris_tanh` is the Padé rational `p(x) = x(27+x²)/(27+9x²)` clamped at `|x| > 4.9`, and since `p(x) − 1 = (x−3)³/(27+9x²)`, it returns magnitudes **above 1** on `3 < |x| ≤ 4.9` (peak 1.02822), which (a) puts a 0.0282 jump discontinuity into every hidden unit and a 0.0141 jump into every output unit at the clamp, and (b) drives the backward factor `1 − a²` **negative** — the gradient points the wrong way — in a band the guards cannot see.

### 2. The measured verdict

**The excursion is common, not theoretical.** At inference on trained nets, hidden activations with strict `|a| > 1` occur in **1.19–2.52 %** of evaluations on the smooth target and **2.38–8.34 %** on the localised target (worst cell: LOCAL N=50). At training time, **3.4–49.6 %** of weight updates involve at least one of 12 hidden units past `|s| = 3`. Moving the clamp to 3.0 removes **~99.8 %** of them (residual 0.004–0.018 %, which is the float32 1-ulp effect below).

**The sign error, weighted by magnitude, is negligible — and this is the number the header currently does not carry.** A gradient-*mass* census over all training evaluations (adversarial review, `hgrad.c`) gives wrong-signed contribution as a fraction of total hidden gradient mass: **0.0078 % (SMOOTH N=50)** to **0.0927 % (LOCAL N=50)**. The excursions cluster just past `|s| = 3` where `1 − a² ≈ −ε`. *"Half of all weight updates involve a wrong-signed unit" and "one part in a thousand of the gradient" are the same fact; the first must never be quoted without the second.* The real discrepancy between surrogate and exact is **scale**: `Σ|1−a²| / Σp' = 0.976 / 0.967`, a uniform **2.4–3.3 % under-scaling**.

**Fixing the forward is free. Fixing the backward is not.** Paired over 64 seeds on byte-identical data and byte-identical inits, 3 configs × 8 cells:

| arm | paired median Δ vs shipping, range over 24 cells | survives Holm over the 384 tests run |
|---|---|---|
| **B** clamp 3.0, backward still `1−a²` | **−0.70 % … +0.60 %**, ≤ 0 in 22/24 | yes at SMOOTH N=10 and N=50, both favouring B |
| **C** clamp 3.0, backward exact `p′` | −1.12 % … +1.97 % | yes at SMOOTH N=50: **+1.1 % of total, +4.3 % of controllable error**, 95 % CI on the paired median **[+0.65 %, +1.84 %]** |
| **D** clamp 4.9, backward exact `p′` | −0.94 % … +4.94 % | C vs D differ by ≤ 0.8 % with signs flipping across configs — **the clamp is not the mechanism** |
| **F** exact `p′` at both layers | −0.73 % … +3.54 % | tracks C; consistency is not what costs |

The exact-derivative penalty is **not an effective-learning-rate artefact**: across an lr sweep of 0.090–0.110 (22 % band, and separately at 0.30), `C − A` stays flat at **+1.14 % to +1.27 %**, while a 2.3 % lr change accounts for ~8 % of the gap *in the wrong direction*. It replicates across both split-halves (SMOOTH N=50: +1.27 % / +1.13 %) and at 20 000 epochs. A 1-ulp placebo arm (init weight moved one ulp) changes the result on only 1–5 of 64 seeds, max 0.13 % — the trainer is contractive at this budget, so these are mechanism, not trajectory chaos.

**One new fact that is load-bearing.** Clamping the *domain* at 3.0 does not fix the codomain exactly in float32: 10 220 of the 2 097 153 floats in [2.5, 3.0] still give `p(x) > 1.0f`, peaking at **1.00000012 (1 ulp) at x ≈ 2.983256**, so `1 − a²` still reaches **−2.38e-7** instead of −0.0572. That is 5 orders of magnitude better and still not zero. A paper that says "the codomain is respected" needs either "to within one ulp" or a codomain clamp. Take the codomain clamp — it is cheaper than the domain clamp (below).

**Cost vs true `tanh` goes down, not up:** max `|p − tanh|` falls from **0.028327 (at 4.9)** to **0.023520 (at 1.566)**.

### 3. The prescription — **CHANGE the forward. KEEP the backward.**

Clamp the **codomain**, not the domain. This is exactly the same mathematical function as clamping the input at 3.0 (`p` is monotone with `p′ ≥ 0`, `p(±3) = ±1` exactly in float32, so `|x| > 3 ⟹ |p| > 1 ⟹ clamped`), plus it removes the 1-ulp escape, plus it is branchless on the hot path and therefore distribution-independent.

```c
/* Padé approximation of tanh, clamped to tanh's codomain.

   p(x) = x(27+x²)/(27+9x²) is monotone (p′(x) = ((x²−9)/(3(3+x²)))² ≥ 0) and
   p(3) = 1, p′(3) = 0, both EXACTLY, including in float32. So clamping the
   RETURN VALUE to [-1,+1] is identical to clamping the argument at |x| = 3,
   and it also removes the 1-ulp overshoot that an argument clamp leaves
   behind (10 220 floats in [2.5,3.0] give p(x) = 1.00000012).

   The result: continuous, C¹ (p′(3) = 0 makes the join smooth), never outside
   [-1,+1], and max |p − tanh| = 0.023520 at x = 1.566 — better than the 0.0283
   the old clamp at 4.9 carried. The old clamp put a jump of 0.0282 into every
   hidden unit and 0.0141 into every output unit, and let 1 − a² go negative
   as far as −0.0572. None of that survives here.

   The ±1e9 test exists only to keep x·(27+x²) finite: it overflows float32
   near |x| ≈ 1.9e12. It is not the saturation point and never fires in
   practice — pre-activations are bounded by IRIS_W_LIMIT. */
IRIS_API float iris_tanh(float x) {
  if (x >  1.0e9f) return  1.0f;
  if (x < -1.0e9f) return -1.0f;
  const float x2 = x * x;
  const float p  = x * (27.0f + x2) / (27.0f + 9.0f * x2);
  return p > 1.0f ? 1.0f : (p < -1.0f ? -1.0f : p);
}
```

`iris_sigmoid`, `iris_artanh` (which already clamps `y` to ±0.98, i.e. strictly inside the new saturation point), `iris_logit`, and the file format need **no change**.

**KEEP `d_hid = acc * (1.0f - a*a)`.** Justification is the measurement, not comfort: substituting the exact `p′` costs **+1.1 % of total held-out RMSE, +4.3 % of the error the network can actually control, CI [+0.65 %, +1.84 %]**, replicated in both split-halves, at 20 000 epochs, and across lr 0.09–0.30 — and it costs a division per hidden unit per example on a part with a slow divide. What is traded away: the trainer is still **not** doing backpropagation on the function it evaluates. After this patch the surrogate is no longer *sign*-wrong (at saturation `a = ±1` exactly, so `1 − a² = 0`, which is exactly `p′(3)`), but it remains **under-scaled by 2.4–3.3 % in aggregate** and by up to 2× pointwise in the ordinary operating range. That must stay in the comment, restated below.

**KEEP `d_out = e * y * (1 - y)`** — see Defect 3.

Comment surgery required in the same commit, because the current text will become false:

- `iris.h:248–261` — replace wholesale with the block above. Delete "maximum absolute error 0.0283" and the entire "IT ALSO LEAVES THE CODOMAIN … documented rather than fixed on purpose" paragraph.
- `iris.h:994–1017` (the ⚠️ surrogate note) — the ratio table stays (it is correct and reproduces digit for digit), but **"then it changes sign, reaching −1.25 by x=4.5" must go**: after this patch that band is unreachable. Replace with: *"exact only at 0, under-scaling monotonically to 0 at the saturation point; measured aggregate under-scaling of total hidden gradient mass 2.4–3.3 %."* Replace "substituting the mathematically exact Padé derivative makes recall and grid RMSE MEASURABLY WORSE" with the CI: *"+1.1 % of held-out RMSE (95 % CI +0.65 % … +1.84 %), 64 paired seeds, replicated at 20 000 epochs and across lr 0.09–0.30."*
- `iris.h:1031–1034` — same edit.
- `iris.h:827` — the epochs table (see §4).
- `docs/SYSTEM-technical.md:15`, `docs/MATH-AUDIT.md:16, 92, 185, 187, 260`, `docs/DESIGN.md:845–846` all quote 4.9 or the codomain escape.

### 4. What it costs — measured, not estimated

| axis | measured |
|---|---|
| **Golden SGD hashes** | **UNCHANGED.** `0xFEFAEDF6` ([0,1]) and `0x6805FB0D` ([−1,+1]) both still match. The golden recipe (seed 1234, 20 examples, 800 epochs) never drives a pre-activation past 3.0, so the core training path is bit-identical. |
| **Golden files** | **No regeneration.** Checks 11 and 29 (`v1-instrument.bin` / `v3-instrument.bin` load and predict bit-identically) both still PASS. |
| **Guards A/B blob** | **UNCHANGED**, `0x0FEC913D`. |
| **What does move** | Check 12's pinned **L-BFGS** weight hash `0xFB5BE623 → 0x1648FA1E` (informational pass-1 value `0x1C8FB3B2 → 0x53E4CBE6`). Check 36 ("L-BFGS converges and stays converged, 10× budget buys nothing") **FAILS**: 1000 it 1.555e-06 → 10000 it 9.427e-07, a **1.65× move for 10× the work**. L-BFGS was previously converging into the discontinuity; on a C¹ activation it keeps improving. That check's *premise*, not just its threshold, needs re-examination. |
| **Fit quality** | Better. Check 5 at N=20: converged err **5.075e-06 → 4.169e-06**, recall **0.0014 → 0.0012**. |
| **On-device time, per epoch** | Host, N=50 at a fixed 12 000 epochs: **48.1 ms → 49.8 ms, +3.5 %** (n=5+4 runs, non-overlapping). Activation in isolation: 0.78 ns → 0.85 ns, flat across input distributions. The *domain*-clamp form is worse and distribution-dependent (1.05 ns at 0 % saturation, 1.40 ns at 14 %, from branch misprediction) and a branch-plus-clamp hybrid is worst end-to-end (+9.6 %). This is one reason to clamp the codomain rather than the domain. **Caveat: all of this is host (arm64, out-of-order). The S3 is in-order with cheap branches and an expensive divide, so the ordering could reverse there. Not measured on the part.** |
| **On-device time, to plateau** | Up at small N, because the net is still improving when it used to stop: N=10 goes **20 000 → 27 825 epochs** (it now reaches the `err < 1e-6` floor), ~4.4 s → ~6.2 s at the ×270 S3 estimate. N=20: 16 000 → 18 000. N=200: **10 000 → 6 000** (down). |
| **Code size** | +2 lines, one extra compare-and-select. Net flash change ≈ 0. |
| **Teaching complexity** | Lower. "The function is clamped so it cannot leave the range it promises" is one sentence, stated where it is enforced. It also deletes ~10 lines of comment apologising for the defect. |
| **Saved instruments** | A v0.4.0 file plays *identically* under the new code on the golden recipe (check 11/29 pass), but an instrument whose weights drive `|s| > 3` will move by up to 0.028 per hidden unit. This is a real behaviour change for existing users and belongs in CHANGELOG under a minor bump. |

---

## DEFECT 2 — a 75-parameter model trained to convergence on 5–50 demonstrations, with no capacity control but the epoch budget

### 1. The defect

The trainer has more parameters than it has training scalars in every configuration the library documents (75 parameters vs 3×N targets at N ≤ 25) and runs to a plateau with no penalty term, so with noisy demonstrations — which is what a human demonstrating a gesture produces — it fits the noise.

### 2. The measured verdict

Held-out RMSE on a fixed clean 41×41 grid, 32 paired seeds, identical data and identical initial weights across arms, L2 semantics matched to sklearn's penalty-to-data ratio via a per-example coefficient `alpha / n_ex`:

| target, N=20 | σ=0 | σ=0.02 | σ=0.05 | σ=0.10 |
|---|---|---|---|---|
| SMOOTH, best α | +9.3 % (1e-3) / **+34.9 % (1e-2)** harm | **−49.5 %** | **−65.2 %** | **−62.9 %** |
| STRUCTURED, best α | **−19.7 %** (1e-3) | **−28.8 %** | **−40.3 %** | **−46.8 %** |

Sign p down to 4.7e-10 (32/32 seeds), IQRs disjoint in the large cells, surviving Benjamini–Hochberg across all 224 arm×cell tests (160/224 significant at q = 0.05). It survives both confounds the adversarial pass threw at it: with the output clamp removed the gain grows (−65.2 % → −67.0 %), and restricted to grid points inside the training input hull it grows again (−68.0 %).

Three things that did **not** survive, and that constrain the prescription:

- **On SMOOTH, training less is as good or better.** An oracle over epoch budget {plateau, 600, 100} beats an oracle over α on **10 of 32 cells**, every one of them SMOOTH-under-noise, by 3–17 % — at 1/40th the compute. The distinctive, irreplaceable L2 result is **STRUCTURED at N ≥ 10**, where α=1e-3 beats the best of {plateau, 600 ep, 100 ep} by **19–29 %**. And there is **no control arm** that spends the same selection budget choosing epochs instead of α, so "L2 is what helps" is not fully separated from "capacity control by any means helps."
- **No fixed α is safe everywhere, and the claimed harm for the small α's is not established.** Exact distribution-free CIs on the SMOOTH N=20 σ=0 harm cells include zero for α=1e-5 (+0.00002, CI [−0.00000, +0.00008]) and α=1e-4 (+0.00015, CI [−0.00003, +0.00033]); the reported bootstrap CIs were too narrow on a 32-point median. α=1e-2 and 1e-3 have large unambiguous harm cells at σ=0 (+43 % and +9.3 %; **+31.6 % for 1e-3 once the output clamp is removed** — the clamp was *flattering* L2 in the noiseless cells).
- **α is not a fixed regulariser in this library.** `iris_fit_ranges` derives the input and output normalisation from the training data's own observed range, and noise inflates that range by **0.583× to 1.385×** across the (N, σ) grid. The `alpha / n_ex` construction removes the N-dependence of the step ratio and the normalisation puts an N- and σ-dependence straight back in. The "comparable to sklearn's alpha" claim holds only under matched preprocessing, which no sklearn user has.
- **All N=5 conclusions are withdrawn.** 57 % of the test grid is outside the training input box there; two N=5 conclusions reverse sign when that region is excluded.

One unreported result in L2's favour: `iris_get_status` was never consulted by the harness. Re-run with it printed, plain plateau training produced **4 `IRIS_TRAINING_DIVERGED` events** at σ=0.10 (weights hit ±16, run aborted mid-window). **α ≥ 1e-3 produced zero, anywhere.**

### 3. The prescription — **CHANGE: add the mechanism. Default it to zero.**

Ship the knob; do not ship the default. The measurement says the win is large where it exists and that a single always-on α is not safe — those are both findings, and shipping α=0 by default is the one setting consistent with both.

```c
/* in struct iris, next to lr/momentum: */
  float   l2;   /* weight decay, 0 = off (the default and the shipped bits) */
```

```c
/* in iris_init, beside the other learning defaults: */
  k->lr = 0.10f; k->momentum = 0.85f; k->l2 = 0.0f;
```

```c
/* beside iris_set_learning.

   L2 weight decay, in sklearn's units. alpha is divided by n_ex because iris
   does per-example SGD with no 1/n averaging, where sklearn averages once per
   full-batch step; dividing here makes one iris EPOCH apply a total decay of
   lr*alpha*W, so alpha means the same thing at every N.

   Biases are never decayed (sklearn decays coefs_ only). The decay enters the
   momentum velocity — classic L2, not decoupled/AdamW.

   MEASURED, 32 paired seeds, held-out grid RMSE:
     alpha = 0       the default, and bit-identical to every version before this
     alpha = 1e-4    largest value with no measured harm cell above 1.3e-4 RMSE
     alpha = 1e-3    the recommendation for demonstrated-from-human data:
                     -20% to -47% held-out error on structured targets, and it
                     eliminated every IRIS_TRAINING_DIVERGED event we saw
     alpha = 1e-2    biggest win under heavy noise, but +35% to +43% HARM on
                     clean, smooth data. Do not default to it.
   Caveat that matters: iris normalises inputs and outputs from the training
   data's own observed range, so the effective penalty strength moves with N
   and with how noisy the demonstrations are. alpha is a dial, not a constant. */
IRIS_API void iris_set_l2(iris *k, float alpha) {
  k->l2 = iris_clampf(alpha, 0.0f, 1.0f);
}
```

```c
/* in iris__train_run, hoisted out of the example loop, just before `err = 0.0f;`
   — one divide and one multiply per EPOCH, not per weight: */
    const float wdlr = k->lr * (k->l2 / (float)k->n_ex);
```

```c
/* the two weight-update lines. NOTE THE ASSOCIATION: the decay is a separate
   subtracted term, NOT folded inside the existing product. Folding it
   (`- k->lr * (g * k->hid[h] + wd * w[h])`) re-associates the shipping
   arithmetic and breaks both golden hashes at alpha = 0 — measured, it gives
   0xAA888031 / 0x9C827EF9. The form below leaves every bit alone when
   k->l2 == 0, because x - (±0.0f) == x exactly for every finite x. */
          v[h] = IRIS_FLUSH(k->momentum * v[h] - k->lr * g * k->hid[h] - wdlr * w[h]);
...
          v[i] = IRIS_FLUSH(k->momentum * v[i] - k->lr * g * x[i] - wdlr * w[i]);
```

Do **not** ship the LOO-CV auto-selection (arm E). It costs **64–68× arm A** (5 α's × N folds + a refit), projecting to **~50 s on the S3 at N=20** for a library that sells retraining between two notes; it is worse than plain plateau stopping on **15 % of individual datasets** and by >25 % on 1.5 %, which is what a musician who trains once actually experiences; and its regret against an oracle that also ranges over the epoch budget is **+4.3 % median, +19.1 % max**.

### 4. What it costs

| axis | measured |
|---|---|
| **Golden hashes** | **NOTHING CHANGES.** The full 40-check audit on the patched copy (`iris6/`) is byte-identical to baseline except one wall-clock timing readout. `0xFEFAEDF6`, `0x6805FB0D`, `0xFB5BE623`, `0x0FEC913D` all match. |
| **File format** | Untouched — `lr`/`momentum` are not serialised, and neither is `l2`. |
| **On-device time, α = 0** | Host: N=50 48.1 → 48.9 ms (**+1.7 %**); N=200 158.9 → 161.8 ms (**+1.8 %**). Two float ops per weight per example, always paid. Predict path: zero. |
| **On-device time, α > 0** | Same +1.8 %. Training *time* can go **down**: α=1e-2 stops at the earliest plateau check (4000 epochs) in every N=20 cell. |
| **Code size** | One float in the struct (+4 B RAM per instrument), one setter, one hoisted expression, two edited lines. |
| **Teaching complexity** | Real and worth naming: this is a **second** capacity control alongside the plateau rule, they overlap on smooth data (§2), and its effective strength is not constant across N and σ. That is why it is off by default and why the comment says "a dial, not a constant." |

---

## DEFECT 3 — logistic output units trained with squared error, on a surrogate derivative

### 1. The defect

The output layer is a logistic squashed to `[0.1, 0.9]` trained under squared error with `d_out = (y − t)·y(1 − y)`, which is the textbook vanishing-gradient pathology — a saturated unit that is confidently wrong produces almost no gradient — and `y(1−y)` is in any case the derivative of the *true* logistic, not of `iris_sigmoid`.

### 2. The measured verdict

**The pathology does not fire.** A counter defined as `y(1−y) < 0.01 AND |y − t| > 0.3` (saturated *and* badly wrong), on the shipping reference task at the shipping `lr = 0.10, momentum = 0.85`:

**0 fires in 48 960 000 output-unit updates**, across N ∈ {5, 10, 20, 50}, all 32 seeds, all 6000 epochs. Zero. Decile breakdown across the run: all zero.

It takes deliberate abuse to make it fire at all. At the shipping lr on four stress tasks (reference, cliff, contradictory, xor-extremes) the worst cell is **0.013 %** (contradictory, N=50). It needs **lr ≥ 0.5** to appear (cliff N=50: 0.735 %) and lr = 1.0 to become common. At lr = 2.0 / momentum = 0.99 it fires 25–40 % of the time — and the weight-divergence guard has already killed 32/32 runs by then, which is the guard doing its job.

**The proposed fixes are worse, measured paired per seed on the same data and the same inits:**

| arm | N=5 | N=10 | N=20 | N=50 |
|---|---|---|---|---|
| **B** BCE, targets rescaled to [0,1] | ×1.59 | ×2.28 | ×2.08 | ×1.21 (wins **0/32** at N ≤ 20) |
| **C** linear output, squared error | ×1.06 | ×1.06 | ×1.65 | ×1.77 (wins 0/32 at N ≥ 20) |
| **D** floor `max(y(1−y), ε)` | ×1.000 | ×1.000 | ×1.000 | ×1.000 |

Arm D is a **mathematical no-op**: targets live in [0.1, 0.9] so `y(1−y) ≥ 0.09`, and the ε sweep confirms nothing below 0.09 can bind (ε = 0.00/0.01/0.05 give identical medians to 5 decimals). Arm C also triples-to-quintuples the pre-clamp overshoot (×3.2–×4.7 vs A), which is roughness in a synthesis parameter.

**The one arm that beats it, stated plainly.** Arm **B9** — BCE gradient `d_out = (y − t)` with the targets left in [0.1, 0.9], i.e. BCE isolated from the target rescale — wins **32/32 seeds at N=20 (×0.947)** and 24/32 at N=50, and at a tuned lr = 0.025 it wins 32/32 at both N=20 and N=50 by **10–12 %**. It also **loses** at N=10 (2/32 wins, **×1.094**) and ties at N=5 (×1.002).

### 3. The prescription — **KEEP, unchanged.**

`d_out = e * y * (1.0f - y)` stays exactly as written, including the `(e*y)*(1-y)` association, which is load-bearing at the ulp level.

The justification is the counter: **0 fires in 48.96 million output updates at the shipped hyperparameters**, and the arms that remove the factor are 1.2×–2.3× worse on held-out error at every N. This is not "no evidence of a problem"; it is a direct measurement of the specific event the objection names, at a resolution where 1 fire in 10⁷ would have been visible.

**What is being traded away, plainly:**

1. **A measured 5.3 % (untuned) to 11.9 % (tuned lr) improvement at N=20–50 is being left on the table**, because B9 costs ×1.094 at N=10 — and N=5–10 is the regime a musician actually demonstrates in — and because B9 widens the reroll spread in the undemonstrated gaps by **1.6×** (0.04133 vs 0.02625 at N=10 far-field). "Reroll is a real control rather than a shrug" is a stated design promise of this library, and `y(1−y)` is the brake that keeps it. That is the trade, and it is a judgement about the use case sitting on top of a measurement, not a measurement by itself. It should be written down as such.
2. The backward pass is still not the derivative of the forward pass at the output layer either. `iris_sigmoid` is built from the Padé, so `y(1−y)` under-scales by the same 2.4–3.3 %. Arm F (exact `p′` at both layers) tracks arm C to within noise, so **consistency is not what is worth having** — it costs the same +1.1 % as fixing the hidden layer alone.
3. `d_out` is *not* saturation-safe if a user sets `lr ≥ 0.5`. `iris_set_learning` permits up to 2.0. The honest response is documentation, not code: the header should say the pathology is measured absent at the defaults and measured present above lr 0.5.

### 4. What it costs

Nothing: no code change, no hash change, no time, no size. The cost is one paragraph of comment (replacing the current "WHY IT STAYS" text, which cites a 100-seed recall comparison on a different task) with the counter, the stress table, and the B9 concession named. The teaching-complexity cost of *not* changing it is that a reader who knows the vanishing-gradient story will ask; the comment must answer with the number, not with a claim.

---

## 5. The combined patch — do the changes interact?

**Two changes land: the codomain clamp (Defect 1) and the L2 knob (Defect 2). Defect 3 is a comment-only change.**

**Interaction with each other: not measured, and I will not pretend otherwise.** Experiment 1's arm B ran with `l2 = 0`; Experiment 2 ran on the stock clamp at 4.9. They were never crossed.

What can be said mechanically, as a hypothesis to be tested rather than a result:

- Both act on the same quantity — the size of the hidden pre-activations. L2 shrinks weights, which lowers the rate of `|s| ≥ 3`, which makes the clamp change **more nearly a no-op**. They should be **sub-additive**, not antagonistic.
- The one place they touch the same failure is divergence: L2 at α ≥ 1e-3 eliminated all four `IRIS_TRAINING_DIVERGED` events, and the clamp change removes the negative-derivative band that can feed runaway. Neither result establishes the other.

**Interaction with the golden hashes: measured, and they compose cleanly.** L2 at α=0 is bit-identical on its own (`iris6/`: full audit unchanged). The clamp change on its own leaves both SGD golden hashes unchanged (`iris4/`: `0xFEFAEDF6`, `0x6805FB0D` still match) and moves only the L-BFGS pin. So the combined patch's effect on the pinned core-SGD constants is **nil**, and the L2 half contributes nothing to the re-pin.

**Required before the combined patch ships:**

1. Re-run Experiment 1's arm A vs arm B at `l2 = 1e-3` and `l2 = 0`, 64 paired seeds, both targets, N ∈ {20, 50} only (the equal-budget cells). Hypothesis to falsify: the arm-B effect shrinks toward zero under L2. Cost: hours.
2. Re-run Experiment 2's σ-sweep on the patched activation at N ∈ {10, 20, 50} on STRUCTURED — the cell that carries L2's load-bearing claim. Hypothesis: the 19–29 % margin over the best epoch budget is unchanged. Cost: the same 4 min 39 s on 16 cores.
3. Add the missing control from the adversarial pass: an arm that spends the identical LOO budget selecting the **epoch budget** at α=0. Without it, "L2 is what helps" remains unseparated from "capacity control helps," and that is the single most likely reviewer objection to the L2 half.

---

## 6. What a reviewer could still say afterwards — honestly

**Standing, and correctly so:**

1. **"Your trainer still isn't doing backpropagation."** Correct. Both surrogates stay. After the patch the sign error is gone and the *scale* error remains: `1 − a²` under-scales total hidden gradient mass by **2.4–3.3 %**. Keeping it is defended by a confidence interval — the exact derivative costs +1.1 % of held-out error, CI [+0.65 %, +1.84 %] — not by convenience. The correct description of the trainer is "per-example SGD with classical momentum on a surrogate gradient," and it should appear in the abstract, not a footnote.
2. **"Your evidence for that comes from two hand-written target functions."** Correct, and the headline conclusions **differ by target**: the exact-derivative penalty is a SMOOTH-only finding, and on the localised target nothing survives multiplicity correction in either direction — at lr 0.30 there is a *significant reversal* (C better by 1.12 % at LOCAL N=20). Two targets, one architecture, one `nh`, no pre-registration. Say "on these targets," not "in general."
3. **"The Padé forward beating libm `tanhf` is unexplained."** Yes, and it should be stated at its true strength: real `tanhf` loses the *paired* comparison by 3–5 % of total error reproducibly, but at 20 000 epochs / N=50 it has the **lowest median of all six arms**. The effect is a heavier bad tail, not a worse typical fit. Phrase it as "libm tanh fails badly more often here," never as "libm tanh is worse," and say there is no mechanism.
4. **"Your α isn't sklearn's α."** Correct under the library's data-derived normalisation, which moves the effective penalty by 40–57 % between σ=0 and σ=0.10. The comment says so. The L2 branch also has **no correctness check of its own** — the bit-identity proof covers only the α=0 path; a finite-difference gradient check on the decay term is a gap and should be added as audit check 41.
5. **"On smooth data you could have just trained less."** Correct — and the paper should concede it in exactly those words. The claim L2 is entitled to is the STRUCTURED one.
6. **"You left a known improvement on the table at the output layer."** Correct: BCE-with-[0.1,0.9]-targets beats the shipping rule by 5.3 % at N=20 untuned and 11.9 % tuned. **Keeping the shipping rule is right anyway**, and the reason is not that the improvement is fake — it is that it reverses at N=10 (×1.094), which is the regime a musician demonstrates in, and it widens the reroll spread in the gaps by 1.6×, which degrades a control this library sells. State the number, state the trade, do not bury it.
7. **Every ESP32-S3 figure is host×270.** Nothing in this prescription has been run on the part. The clamp change's per-epoch cost in particular could go the other way there (in-order pipeline, cheap branches, expensive divide). Say "estimated," always.

**Closed by the patch, and safe to claim without qualification:**

- The activation is continuous, C¹, monotone, and **never returns a value outside [−1, +1]** — exactly, in float32, not to within an ulp. (This is why the codomain clamp is prescribed over the one-constant domain clamp; a reviewer who evaluates `iris_tanh(2.983256f) > 1.0f` on the one-constant version finds `true`.)
- The 0.0282 jump in every hidden unit and the 0.0141 jump in every output unit are gone.
- `1 − a²` and `y(1−y)` can no longer go negative.
- Max error vs true `tanh` **fell**, 0.028327 → 0.023520.
- The library has a capacity control other than "stop early," and it is off by default.

---

## 7. Re-pinning plan

**The good news, measured: the two core-SGD golden constants do not move.** `0xFEFAEDF6` and `0x6805FB0D` survive both changes, because the golden recipe (seed 1234, 20 examples, 800 epochs, `truth()`) never drives a pre-activation past `|s| = 3`. Neither golden `.bin` needs regeneration and neither `expected.txt` changes — checks 11 and 29 pass untouched. **`tests/golden/make_golden.c` is not run.**

**What must be re-pinned, one constant:**

| location | old | new | why |
|---|---|---|---|
| `tests/audit.c` check 12, `wantl` | `0xFB5BE623` | **`0x1648FA1E`** | L-BFGS folds the activation into its own loss and hits the [2.9, 3.0] band. Nothing to do with the SGD core — which is the point, and should be said in the re-pin comment exactly as the 2026-08-26 L-BFGS repair note says it. |

**What must be re-derived rather than re-pinned:**

| location | change | action |
|---|---|---|
| `tests/audit.c` check 36 ("L-BFGS converges and stays converged, 10× budget buys nothing") | now 1000 it 1.555e-06 → 10000 it 9.427e-07, a **1.65× move**; the check asserts ≈1.00× | **Do not just widen the tolerance.** The assertion's premise is that L-BFGS has converged at 1000 iterations. On a C¹ activation it has not. Either re-derive the iteration count at which it does converge, or restate the check as a reported ratio like the SGD comparison beside it. Decide deliberately; this is the one place the patch changes a *claim*, not a number. |
| `iris.h:827` (the epochs/error docs table) and the audit's perf table | N=10: 20 000 → 27 825 ep (now hits the `err<1e-6` floor); N=20: 16 000 → 18 000, err 5.075e-06 → 4.169e-06, recall 0.0014 → 0.0012; N=100: 8 000 → 10 000; N=200: 10 000 → 6 000 | Regenerate from `./build.sh audit`. Note in the table that the epoch counts moved because the net keeps improving, not because epochs got slower — per-epoch cost is +3.5 % on host. |
| checks 30 / 38 tolerance readouts | 0.0447 → 0.0442 | Inside tolerance, no edit. Recorded so the diff isn't mistaken for drift. |

**Must be re-run, in this order:**

1. `./build.sh audit` — expect 40/40 after the single re-pin and the check-36 decision. Confirm `0xFEFAEDF6` / `0x6805FB0D` / `0x0FEC913D` are still printed as *matching*, and say so in the commit message: **the SGD training path is bit-identical on the golden recipe.**
2. `./build.sh sinks`, `./build.sh mpe`, `./build.sh wasm` — the wasm freestanding build is the check that no libc snuck in with the clamp/decay edit.
3. `./build.sh experiment` and `./build.sh bench` — regenerate every timing quoted in `README.md`, `docs/SYSTEM-technical.md`, `docs/DESIGN.md`.
4. **New audit check 41:** finite-difference gradient check on the L2 term (`k->l2 > 0`), because nothing in the existing suite exercises the non-zero branch. Pin it.
5. **New audit check 42:** `iris_tanh` codomain and continuity — assert `iris_tanh(x) <= 1.0f && iris_tanh(x) >= -1.0f` over every float in [0, 8] (2⁵ M values, ~1 s), assert `iris_tanh(3.0f) == 1.0f` bitwise, and assert `iris_tanh(nextafterf(3.0f, 0.0f)) == 1.0f`. This is the check whose absence let the defect ship for four minor versions.
6. Re-run the three cross-experiment measurements listed in §5 before the combined patch is tagged.

**Version and changelog.** This is a **minor** bump (0.4.0 → 0.5.0), not a patch: a saved instrument whose weights drive `|s| > 3` predicts differently. The CHANGELOG entry must say that in one line, with the magnitude (up to 0.028 per hidden unit, up to 0.0141 per output), and must say that the golden SGD hashes did **not** move — because a reader who sees "we changed the activation" will assume they did, and the fact that they did not is the mechanical proof that the core training path is the code it always was.