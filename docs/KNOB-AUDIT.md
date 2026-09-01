> **Historical record.** Measurements and line numbers refer to the file as it
> stood on its date.

---

# THE KNOB AUDIT — `iris.h` v0.4.0

Harnesses: built in a scratch directory that was not retained, with `cc -O2 -std=c99` against this repository's `iris.h`. Task: 2 sensor inputs → 3 sound parameters, N demonstrations drawn at random positions, two truth surfaces (smooth / structured-with-a-cliff), optional Gaussian take-to-take noise σ. Metrics: **recall** = RMSE on the musician's own demos ("does it play back what I showed it"), **grid** = RMSE against truth on a 21×21 held-out grid ("does it behave between demos"), **span** = mean output range over the grid (0 = dead instrument). 12–16 seeds/tasks per cell unless stated.

## 0. Full inventory — 22 runtime knobs, 14 compile-time

Runtime, user-reachable: `iris_init`(mem, bytes, n_in, n_hid, n_out, cap, seed) · `iris_reseed`(seed) · `iris_set_learning`(lr, momentum) · `iris_set_l2`(l2) · `iris_train_epochs`(epochs) · `iris_train_converge`(ceiling, cb, user) · `iris_train_begin`(ceiling) · `iris_train_slice`(epochs) · `iris_correct`(epochs) · `iris_loo_error`(epochs) · `iris_retrain_new`(seed, epochs) · `iris_train_elm`(lam0, scratch, scratch_bytes) · `iris_train_elm_ex`(lam0, **gain_w**, **gain_b**, …) · `iris_retrain_elm_new`(seed, lam0, …) · `iris_knn_predict`(kk) · `iris_internal_set_legacy_norm`(legacy) · `iris_migrate_scaling`().

Compile-time: `IRIS_MAX_{IN,OUT,HID,EX}`, `IRIS_API`, `IRIS_TINY`, `IRIS_NO_GUARDS`, `IRIS_W_LIMIT`, `IRIS_OUT_{LO,HI}`, `IRIS_CONV_{WINDOW,TOL,CEILING}`, `IRIS_STRESS_{MIN_EX,FLAG}`, `IRIS_KNN_{MAXK,GUARD}`.

## 1. THE CENTRAL FINDING: six knobs, one axis

`lr`, `momentum`, `l2`, the training ceiling, `n_hid` and ELM's `lam0` are **not six controls. They are six spellings of one control**: *how hard do you let it chase the demonstrations*. Measured directly — for each of 12 tasks × 3 noise levels, oracle-tune one family and leave the rest at defaults (20 ex, nh=12, held-out grid RMSE, `r.c` R1):

```
noise    default   +tune lr/mom   +tune l2 only   +tune ceiling only
0.00      0.0693       0.0590          0.0601           0.0601
0.05      0.1312       0.0728          0.0755           0.0753
0.10      0.2161       0.0880          0.0904           0.0909
```

Three unrelated-looking knob families land within 2–4% of each other. Tuning `lr` buys you nothing that `l2` alone does not buy. The library ships six ways to make the same adjustment, five of which are dangerous and one of which is safe.

**And it is auto-selectable.** `iris_loo_error` already exists and already gives a real held-out number. Sweep six `l2` candidates by LOO and keep the winner (`a.c` A1):

```
noise    default   auto-by-LOO   oracle   cost
0.00      0.0693      0.0682      0.0595   122 ms
0.05      0.1312      0.0836      0.0728   122 ms
0.10      0.2161      0.1009      0.0899   121 ms
```

81% of the oracle gain at σ=0.05, 78% at σ=0.10, for **zero user-facing knobs and 122 ms** (host; 6 × `iris_loo_error(600)`, `a.c` A3: 20.3 ms per LOO at n=20, 125 ms at n=50). Auto-selecting `n_hid` the same way does **not** work (A2: at σ=0.05 it is *worse* than fixed nh=12) — `l2` is the right axis, `n_hid` is not.

---

## 2. `iris_set_learning` — `lr`

**Does / range.** SGD step size. `iris_clampf(lr, 0.0001f, 2.0f)`, default 0.10.

**Extremes (`h.c`, 16 seeds, 20 ex, nh=12, converge):**

| lr | recall (smooth) | grid | diverged |
|---|---|---|---|
| **0.0001 (min)** | 0.0405 | 0.0582 | 0/16 |
| 0.10 (default) | 0.0024 | 0.0275 | 0/16 |
| 1.0 | 0.0079 | 0.0361 | 0/16 |
| 1.5 | 0.0454 | 0.0612 | 1/16 |
| **2.0 (max)** | **0.1743** | **0.1820** | **5/16** |

At the permitted maximum the instrument is **destroyed**: recall 73× worse than default, grid 6.6× worse, and a third of seeds trip `IRIS_TRAINING_DIVERGED`. At the permitted minimum it is **degraded**: 17× worse recall, plateau-stops at 7500 epochs having gone nowhere. Structured+noisy is worse still — 5/16 diverged at lr=2.0 with recall 0.2814.

**It bricks instruments.** `d.c` D2, 40 seeds, structured σ=0.05: at lr=2.0, **6/40 diverged and 6/6 of those were bricked** — the musician lowers lr back to 0.10, hits retrain, and `iris_internal_train_run` returns −1.0 with `IRIS_DIVERGED_STUCK` forever. Only `iris_retrain_new` recovers, which hands them a *different instrument*. At the defaults: 0/400 at σ≤0.05, 2/400 at σ=0.10 (`f.c` F2). So the bricking path is essentially only reachable by turning this knob.

**Wekinator.** `grep -rn "setLearningRate" src/` over the entire wekinator tree: **zero hits.** Confirmed. Weka's `MultilayerPerceptron` default 0.3 is used unmodified and is not settable anywhere in Wekinator's UI or code. Weka's own documented option range is `-L` "should be between 0 - 1" — **iris permits double Weka's documented maximum.**

**Could anyone choose it?** No, and worse than "no": the signal points backwards. The only number a UI can show is training error (`r.c` R2, σ=0.10, task 0):

```
lr=0.001   trainMSE 1.170e-02   grid 0.0693   <- best instrument, worst-looking number
lr=0.050   trainMSE 1.250e-03   grid 0.1472   <- best-looking number, near-worst instrument
```

An undergraduate tuning lr by watching the error readout will reliably pick the worst setting available.

**Cost of removal.** Zero. Section 1: `l2` recovers the same gain. There is one fidelity use — `iris_set_learning(k, 0.3f, 0.2f)` reproduces Weka's pair, which `tests/audit.c:662` exercises.

### **VERDICT: MAKE INTERNAL.** Rename to `iris_internal_set_learning` (double underscore, the file's existing private convention), keep it for `tests/audit.c`'s Weka-parity check and the golden hashes, and remove it from the public surface and README. If it must stay public: **CLAMP HARDER to [0.01, 0.5]** — 0.5 is the value the header's own surrogate-gradient note names as the point where the vanishing-gradient pathology starts firing, and everything above it is measured destructive here.

---

## 3. `iris_set_learning` — `momentum`

**Does / range.** `iris_clampf(momentum, 0.0f, 0.99f)`, default 0.85.

**Extremes (`h.c`, 16 seeds):**

| momentum | recall | grid | epochs | diverged |
|---|---|---|---|---|
| **0.00 (min)** | 0.0039 | **0.0257** | 20250 | 0/16 |
| 0.85 (default) | 0.0024 | 0.0275 | 12125 | 0/16 |
| 0.95 | 0.0022 | 0.0290 | 8250 | 0/16 |
| **0.99 (max)** | **0.0179** | 0.0501 | 3807 | **3/16** |

Momentum 0 → 0.95 is **flat**. The whole usable range is a *speed* knob: momentum 0 gets the same instrument in 1.7× the epochs. Momentum 0.99 is the only setting that changes anything, and it changes it for the worse.

**0.99 is the single most destructive knob position in the library.** `d.c` D2, 40 seeds, structured σ=0.05, lr left at default 0.10: **21/40 diverged, 20/40 bricked.** Half the instruments. Combined with lr=0.5 (`h.c`): **16/16 diverged, stopping after 10 epochs.** The permitted maximum of a knob whose *default is 0.85* — one nudge away on any slider — has a coin-flip chance of permanently destroying the instrument.

**Wekinator.** Zero `setMomentum` calls. Weka default 0.2, unmodified, unexposed. Weka's documented range is 0–1; iris's default of 0.85 is 4× Weka's.

**Could anyone choose it?** No. The measurable difference over 0.0–0.95 is training *time*, which converge already hides. And there is nothing to choose: it does not affect the instrument.

**Cost of removal.** Measured: **nothing**, except ~1.7× more epochs at momentum 0 (12k → 20k, i.e. ~17 ms → ~28 ms host, ~4 s → ~6.5 s S3 at 20 ex). Hard-coding at 0.85 costs literally nothing.

### **VERDICT: MAKE INTERNAL** (hard-code at 0.85). This is the clearest delete in the audit: the safe range does nothing and the unsafe end bricks half of all instruments. If the parameter must remain in the signature for Weka parity, **CLAMP HARDER to [0.0, 0.9]** — 0.9 was clean 0/16 in every cell, 0.95 was 2/16 at lr=0.5, 0.99 was catastrophic everywhere.

---

## 4. `iris_set_l2` — `l2`

**Does / range.** Decoupled weight decay on weights only, scaled `l2 * lr / n_ex`. `iris_clampf(l2, 0.0f, 1.0f)`, default 0.0.

**Extremes (`h.c`, 16 seeds, 20 ex):**

```
                 clean smooth        structured σ=.05      smooth σ=.10
l2        recall / grid          recall / grid          recall / grid
0.0       0.0024 / 0.0275        0.0144 / 0.1453        0.0163 / 0.1805
1e-3      0.0029 / 0.0260        0.0195 / 0.1240        0.0194 / 0.1733
1e-2      0.0065 / 0.0259        0.0362 / 0.1002        0.0470 / 0.1207
0.1       0.0209 / 0.0412        0.0616 / 0.0890        0.0802 / 0.0716
1.0 (max) 0.0524 / 0.0687        0.1401 / 0.1626        0.1089 / 0.0771
```

**Neither extreme destroys anything. Zero divergences at any value, any task, any noise level** — l2 ≥ 1e-3 was the only setting in the whole audit that *reduced* divergence (matching the header's own claim). At the maximum the instrument is degraded but alive and finite: recall 22× worse on clean data, meaning it stops reproducing your demos — a musically legible, immediately audible failure, not a silent one.

**It is monotone and legible.** recall goes 0.0024 → 0.0024 → 0.0024 → 0.0029 → 0.0065 → 0.0209 → 0.0421 → 0.0524 across the range: strictly monotone with no reversals. This knob answers one question a musician can actually hold in their head — *"stick tightly to my demos, or smooth between them?"* — and it answers it monotonically in both directions.

**It carries the real gain.** At σ=0.10 it takes grid RMSE 0.1805 → 0.0716, a 2.5× improvement, which is the entire oracle gain of the lr/momentum sweep (Section 1).

**Wekinator.** No equivalent for the MLP path (Weka MLP has no regularisation parameter at all). Wekinator's SVM panel exposes a complexity `C` text field (`SVMEditorPanel.java:147`, default "1.0") and an RBF gamma (default ".01"), which is the nearest thing in the codebase to a capacity control the user can touch.

**Could anyone choose it?** Yes, *if* the units are hidden. The raw scale is unchooseable — the header itself warns it "is NOT directly comparable to scikit-learn's alpha", and the good value moves from 1e-3 to 0.1 with noise the musician cannot measure. But the *direction* is choosable, and the good value is auto-selectable by LOO (Section 1).

**Cost of removal.** Large — this is the one knob whose removal costs something real: 2.5× held-out error on noisy demonstrations.

### **VERDICT: KEEP EXPOSED — but reskinned as the only knob, and auto-set by default.** Concretely: (a) keep `iris_set_l2` as the internal mechanism; (b) add `iris_set_smoothing(iris *k, float t)` taking 0..1, mapping to l2 ∈ {0 … 0.3} — a named musical control, not an ML hyperparameter; (c) add `iris_auto_smoothing(k)` doing the LOO sweep measured in A1 (122 ms at 20 ex) and make it what the train path calls unless the user has set it by hand. **Clamp the raw setter to [0.0, 0.3]**: 0.3–1.0 buys nothing on any task measured (σ=0.10: 0.0688 at 0.5 vs 0.0716 at 0.1) while costing 2× more recall.

---

## 5. `iris_init` — `n_hid`

**Does / range.** Hidden units. `[1, 64]` hard-refused outside (`iris_init` returns NULL — verified, `e.c` E6). ELM additionally refuses `nh < 8` (verified: nh=1..7 return −1 and leave the instrument unfitted; nh=8 is the first that solves).

**Extremes (`h.c`, 16 seeds, 20 ex smooth):** nh=1 → recall 0.0823, grid 0.1034 (34× / 3.8× worse than nh=12) — **degraded to useless**. nh=64 (max) → recall 0.0015, grid 0.0255 — **marginally better than the default and completely safe**. The entire range 8–64 is flat: grid 0.0279 / 0.0275 / 0.0269 / 0.0277 / 0.0290 / 0.0276 / 0.0255. Same at 5 ex and 50 ex. **The top half of the permitted range does nothing at all.**

**But on noisy data it is a capacity knob in disguise (`r.c` R4):**

```
σ=0.10 n=20:  nh=12 → 0.2161   oracle-nh → 0.1253  (42% gain)   best nh: 4:9  6:3
σ=0.10 n=35:  nh=12 → 0.1465   oracle-nh → 0.0839  (43% gain)   best nh: 4:12
σ=0.05 n=50:  nh=12 → 0.0582   oracle-nh → 0.0393  (33% gain)   best nh: 4:9  8:2  24:1
```

The oracle is nh=4 in 39 of 48 noisy cells — i.e. it is *underfitting on purpose*, the same axis as l2, reached by a different lever. And nh=4 is **below the ELM floor of 8**, so a musician who tunes nh for the backprop path silently loses the ELM trainer.

**Wekinator.** This is the **only** MLP knob Wekinator exposes: `NeuralNetEditorPanel` gives a combo box `{0,1,2,3}` hidden *layers* and a second combo `{"Same as number of inputs", "Custom number:"}`. Default: 1 layer, width = number of inputs. So Wekinator's musician-facing MLP surface is two dropdowns and one optional integer — and the width default is *derived from the rig*, never guessed.

**Could anyone choose it?** No. Clean data says "bigger is fine, doesn't matter"; noisy data says "4". Nothing observable distinguishes the two situations, and getting it right on the noisy side breaks ELM.

**Cost of removal (hard-code 12).** 2–5% on clean data, 33–43% on noisy data — but that 33–43% is the *same* gain `l2` already buys safely (Section 1), and unlike nh, l2 does not break ELM and is LOO-selectable (A2 shows LOO-selecting nh does **not** work).

### **VERDICT: CLAMP HARDER to [8, 64], and default it.** Add `iris_init_default(mem, bytes, n_in, n_out, cap, seed)` that picks nh internally. Keep the explicit form for arena-budget reasons — nh is a *memory* decision on an MCU, which is legitimate — but the floor of 8 must be enforced in `iris_init` itself, not only inside `iris_train_elm`, so that "I picked nh=4 and now the instant trainer silently refuses" cannot happen. Do **not** advertise nh as a quality control; measured, it is not one.

---

## 6. Epochs / ceiling — all seven entry points

**`iris_train_converge(k, ceiling, …)` and `iris_train_begin(k, ceiling)`** — `ceiling ≤ 0` → `IRIS_CONV_CEILING` (60000).

Measured (`h.c`, 8 seeds, 20 ex): ceiling 20000, 60000, 200000, 0, −1 and `INT_MAX` **all produce byte-for-byte the same run** (11750 epochs, recall 0.0024, grid 0.0276) — the plateau test stops first every time. Below the plateau it is strictly a damage knob: ceiling=1 → recall 0.0900 (37× worse), ceiling=100 → 0.0228, ceiling=600 → 0.0077.

**So the ceiling has exactly two settings: "the default" and "worse than the default."** Its only honest use is a hard wall-clock budget on a slow device.

**`iris_train_epochs(k, epochs)`** — the Wekinator-fidelity fixed-epoch path. Measured: monotone improvement in recall out to 200000 (0.0900 → 0.0007) but grid RMSE *turns around* after 6000 (0.0268 → 0.0314 at 60000 → 0.0347 at 200000). Over-convergence is real and this knob is how you reach it. `epochs ≤ 0` returns −1.0 and does nothing (correct refusal, verified).

**`iris_train_slice(k, epochs)`** — verified **genuinely inert**: slice sizes 1, 10, 500, 2000, 100000, 0 and −1 all produce weights **bit-identical** to `iris_train_converge(20000)` (`e.c` E4). This is a UI frame-budget argument and nothing else. It is the one budget knob in the file that cannot hurt you.

**`iris_correct(k, epochs)`** — default 20. Measured (`e.c` E2 — record one new demo, correct, measure how far the mapping moved *away* from the correction, i.e. technique lost):

```
epochs      recall    grid    drift
20 (def)    0.1138  0.0550   0.0258
600         0.1303  0.0703   0.0753   (2.9× more technique destroyed)
60000       0.1443  0.0793   0.0765
```

**Every value above the default is strictly worse on all three metrics.** The knob's only reachable effect is to undo the feature. (Note also: the header says `iris_correct` has no production caller — D11.3 dropped it from the firmware in Aug 2026.)

**`iris_loo_error(k, epochs)`** — default 600. Measured non-monotone and unreliable as an *absolute* number (`e.c` E3, true grid RMSE 0.1162): 600 → 0.0750, 100 → 0.0667, 2000 → 0.1038, 20000 → 0.1305. The default *underestimates held-out error by 35%*. Its *ranking* is sound (that is what A1 uses), but nobody can choose this budget and the number it returns should not be shown as an error estimate.

**`iris_retrain_new(k, seed, epochs)` — this one destroys instruments (`e.c` E5).** `epochs = 0` or `−1`: reseeds first, *then* `iris_train_epochs` refuses. Result: grid 0.0465 → **0.1522**, `fitted = 0`, status `IRIS_NOT_FITTED`, return −1.0. **A zero in the epochs argument replaces a working instrument with random weights.** This violates the file's own stated refusal convention ("a train call that did no training … leaves `trained` alone") — the refusal happens *after* the reseed.

**Wekinator.** Zero `setTrainingTime` calls. Weka default 500 epochs, unmodified, unexposed. Wekinator's user has no training-budget control of any kind.

### **VERDICTS:**
- `iris_train_converge` **ceiling → MAKE INTERNAL.** Drop the parameter; keep `IRIS_CONV_CEILING`. Every in-range value ≥ the plateau is identical, every value below is damage. Callers who need a wall-clock wall have `iris_train_begin`/`iris_train_slice` and the progress callback, both of which abort cleanly.
- `iris_train_epochs` **epochs → KEEP EXPOSED.** It is the Weka-fidelity reference path and audit check 12 pins it; it earns its place as a *reference implementation*, not as a musician's control. Move it out of the README's front matter.
- `iris_train_slice` **epochs → KEEP EXPOSED.** Proven inert, honest frame-budget knob.
- `iris_correct` **epochs → DELETE the parameter.** Hard-code 20. Every other value measured is worse at everything.
- `iris_loo_error` **epochs → DELETE the parameter.** Hard-code 600. Unchooseable, and the value chosen changes the answer non-monotonically.
- `iris_retrain_new` **epochs → DELETE the parameter** (hard-code the converge path), **and fix the reseed-before-refuse ordering regardless.**

---

## 7. `iris_train_elm` — `lam0`  ⚠ THE WORST KNOB IN THE FILE

**Does / range.** Relative ridge: `lam = lam0 * trace/(nh+1) + 1e-7`. **There is no clamp anywhere.** No `iris_clampf`, no bounds check, no documented range in the signature. The header names 1e-4 (nh=12) and 1e-3 (nh=48) as defaults.

**Extremes (`h.c`, 16 seeds, 20 ex smooth, nh=12):**

| lam0 | ret | recall | grid | status |
|---|---|---|---|---|
| **−1.0** | −1 | 0.1346 | 0.1455 | 2 (NAN_TRAPPED) — **16/16** |
| 0.0 | 0 | 0.0059 | 0.0458 | 3 (ridge escalated) 1/16 |
| 1e-4 (default) | 0 | 0.0089 | 0.0366 | 0 |
| 1.0 | 0 | 0.0541 | 0.0765 | 0 |
| **1e6** | **0** | **0.1346** | **0.1455** | **0 — reported HEALTHY** |

**Defect A — negative `lam0` destroys a working instrument, and the header's guarantee is false.** `d.c` D3, on an instrument that had just solved cleanly:

```
GOOD instrument      pred(.25,.75)=[0.5015 0.3021 0.4984]  pred(.8,.2)=[0.8254 0.6115 0.5020]
lam0=-1.0 returned -1, status 2
w1[0] 0.001246 -> 0.000623   w2[0] 0.139358 -> 0.136080   CHANGED: w1=1 w2=1
after 'refusal'      pred(.25,.75)=[0.6179 0.4091 0.5191]  pred(.8,.2)=[0.6179 0.4091 0.5191]  status=4
```

The header at `iris_train_elm_ex` says: *"or −1 refusing: nh < 8, no examples, scratch too small, or a poisoned (NaN/Inf) example — **weights untouched on every refusal**."* Both weight arrays changed. The instrument is flat and unfitted. The frozen layer is overwritten *before* the factorisation loop, and the `doublings < 0` branch then calls `iris_reseed`, so the previous instrument is unrecoverable. Same for `lam0 = NaN` and `−inf`.

**Defect B — `lam0 = +inf` and `lam0 = 1e6` return SUCCESS and hand back a dead instrument.** `d.c` D3b/D3c: return value **0** ("solved on the first try"), `iris_get_status` **0** (healthy), `iris_is_trained` **1**, `last_error` 0.0580 (a plausible-looking number) — and the forward pass returns `[0.6179 0.4091 0.5191]` **for every input on the grid, span 0.000**. Every gesture produces the same sound. Nothing in the API says so. This is the exact "no status, no return code, no silence" failure the header's `IRIS_NOT_FITTED` guard was written to prevent, arriving through the front door instead. 1e6 is a value any text field or slider produces.

**Cost of removal (`r.c` R5).** Hard-coding 1e-3 loses 7.3–8.3% at nh=12, 7.6% at nh=48 clean, rising to 21.5–35.1% on noisy nh=48 — and the oracle drifts to 0.01/0.1 with noise, i.e. **the same fit-vs-smooth axis again**, reached by a third lever.

**Wekinator.** No equivalent — Wekinator has no ELM, and none of its nine algorithm families expose a ridge parameter. Closest is the SVM `C` box.

**Could anyone choose it?** No. It is a log-scale relative ridge whose good value depends on `nh`, the data's noise, and the trace of a Gram matrix the user never sees. And unlike lr, a wrong value here is *silent*.

### **VERDICT: MAKE INTERNAL.** Replace `iris_train_elm(k, lam0, scratch, bytes)` with `iris_train_elm(k, scratch, bytes)` choosing lam0 from `n_hid` internally (1e-4 at nh<24, 1e-3 above), and route musician-facing smoothing through the same single `iris_set_smoothing` control as the backprop path. **If it stays public, both defects must be fixed regardless of the verdict:** (a) `lam0 = iris_clampf(lam0, 0.0f, 1.0f)` *plus* an explicit `iris_isbad` reject at entry; (b) after the solve, check the recall error and the output span and set `IRIS_NAN_TRAPPED` rather than returning 0/healthy on a dead instrument.

---

## 8. `iris_train_elm_ex` — `gain_w`, `gain_b`

**Does / range.** Scale of the frozen hidden layer. **Public (`IRIS_API`), unclamped, no default in the signature.** `iris_train_elm` passes `2/sqrt(n_in)` for both.

**Measured (`e.c` E1, nh=48, 20 ex smooth, default gain = 1.4142):**

```
gain      recall    grid
0.0       0.1321   0.1433   <- DEAD (flat), returns 0, status 0
0.1       0.0350   0.0650
1.0       0.0018   0.0443   <- BETTER than the shipped default
1.414     0.0014   0.0456   <- the "measured default"
4.0       0.0003   0.0589
100.0     0.0029   0.1037
-1.414    0.0014   0.0456   <- identical to +1.414 (sign is meaningless)
NaN       ret -1, instrument left unfitted (status 4)
```

Three findings. (1) **gain = 0 returns 0/healthy and produces a dead instrument** — same silent-destroy class as `lam0 = 1e6`. (2) A negative gain is *silently equivalent* to its absolute value, because `iris_rand_sym` is symmetric — a sign a caller might reasonably think means something. (3) **gain = 1.0 beats the shipped 1.4142 on held-out grid error on the header's own reference task** (0.0443 vs 0.0456), which is consistent with the header's own ⚠ admitting the 2/sqrt(n_in) provenance "is NOT currently backed by a reproducible measurement."

**One header claim is wrong.** It says the asymmetric split "cannot be reproduced through the shipped API — `iris_train_elm_ex` takes separate gain_w and gain_b, but its only caller passes the same value for both." `iris_train_elm_ex` is itself `IRIS_API` and public; any caller can pass the split. Measured directly: `gw=1.414 gb=10` → grid 0.0564; `gw=4 gb=0.1` → 0.0612; `gw=1.414 gb=0` → 0.0681. The split is reachable, unmeasured, and unclamped.

**Wekinator.** No equivalent, at any level.

**Could anyone choose it?** Absolutely not. Two unlabelled floats controlling the conditioning of a Gram matrix, with a silent-death value at 0 and no documented range.

**Cost of removal.** Nothing measured is lost. The best value found (1.0) is not the shipped one, and the difference is 3%.

### **VERDICT: DELETE.** Make `iris_train_elm_ex` static/private (`iris_internal_train_elm_ex`) or fold it into `iris_train_elm` outright. This is a two-parameter research hook with a silent-kill value at 0, a meaningless sign, a documented-as-unreachable feature that is in fact reachable, and a default the file itself admits is unmeasured. It is precisely the PI's "random under the hood knob with massive implications."

---

## 9. `iris_knn_predict` — `k`

**Does / range.** `kk < 1 → 1`; `kk > IRIS_KNN_MAXK(8) → 8`; `kk > n_ex → n_ex`. Fully clamped in both directions.

**Extremes (`h.c`, 16 seeds):** `k = −5`, `0` and `1` are **identical** (grid 0.0557); `k = 9`, `100` and `INT_MAX` are **identical to k=8** (0.0589). Total spread across the entire permitted range: 0.0511 (k=3) to 0.0589 (k=8) — **15%, and nothing is ever destroyed.** Recall is exactly 0.0000 at every k, at every noise level, at every N: the zero-distance guard makes exact recall structural.

**Cost of removal (hard-code k=3, `f.c` F1):** 1.6–3.1% on clean data, 5.7–9.9% at σ=0.10 with n≥20 (where the oracle drifts to k=8). Small and bounded.

**Wekinator.** **This is the one knob Wekinator genuinely exposes** — `KNNEditorPanel` has a "Number of neighbors (k):" text field, `KNNModelBuilder` default k=1, and it is the shipping default algorithm for discrete outputs. iris's `iris_classify_1nn` is fixed at k=1 and matches that default exactly.

**Could anyone choose it?** Yes — uniquely in this audit. "How many demonstrations blend together" is a sentence a musician understands, the range is 1–8, the effect is audible and monotone (k=1 snaps, k=8 smears), it cannot diverge, cannot NaN, and cannot leave the demonstrated output range.

### **VERDICT: KEEP EXPOSED.** The only knob in the file that passes every test: clamped at both ends, bounded consequences at the extremes, an effect a musician can hear and name, precedent in Wekinator's own UI, and a real cost to removal. This is what a good knob looks like — measure the others against it.

---

## 10. `iris_init` — `seed` (and `iris_reseed` / `iris_retrain_new` / `iris_retrain_elm_new`)

**Range.** Full `uint32_t`; 0 is silently mapped to 1 (verified: seed 0 and seed 1 produce the identical first weight, 0.000089025).

**Measured (`f.c` F3, same examples, 32 rerolls, clean smooth):** grid RMSE spread max/min = 1.20× at n=5, 1.28× at n=20, 1.14× at n=35, 1.26× at n=50. Recall is essentially unaffected. So the reroll changes the *gaps between demos* while leaving the demos themselves intact — exactly the advertised behaviour. **No seed anywhere in 400 trials produced a broken instrument at the defaults.**

**Wekinator.** No equivalent — Weka's MLP seeds from a fixed default and Wekinator never varies it. This is a genuine iris capability with no Wekinator counterpart.

**Could anyone choose it?** There is nothing to choose. It is a *dice roll*, not a setting, and the API is right to present it as `iris_retrain_new(k, seed, …)` — "give me a different one."

### **VERDICT: KEEP EXPOSED.** Bounded (max/min 1.26×), harmless at every value tested, and it is the library's distinguishing feature. One fix: `iris_retrain_new`'s **epochs** argument must go (Section 6) — `iris_retrain_new(k, seed, 0)` currently reseeds and then refuses to train, destroying the instrument.

---

## 11. `iris_init` — `n_in`, `n_out`, `cap`, `mem`/`bytes`

Not tuning knobs — they are the *rig*. All correctly hard-refused: `n_in ∈ [1,32]`, `n_out ∈ [1,16]`, `cap ∈ [1,4096]`, all out-of-range values return NULL (verified for 0/33, 0/17, 0/4097/−1). `cap` is a pure memory decision, verified linear: 1104 B at cap=1, 1712 B at cap=20, 132144 B at cap=4096 — 32 B/example asymptotically. Recording past cap returns −1 loudly (verified).

### **VERDICT: KEEP EXPOSED** (all four). They describe the instrument, not the learning. `cap` is the one number a musician's UI can ask for honestly ("how many gestures do you want room for"). Consider adding `IRIS_ARENA` convenience defaults for common rigs.

---

## 12. `iris_internal_set_legacy_norm` / `iris_migrate_scaling`

`iris_internal_set_legacy_norm` already carries the private `__` prefix but is `IRIS_API` and reachable. Its own comment says "WHO ACTUALLY CALLS THIS: `tests/audit.c` only" and "Nothing else should call it: changing the scaling under trained weights changes what those weights mean."

### **VERDICT: MAKE INTERNAL** (`#ifdef IRIS_TESTING` or move to a test-only header). A function documented as "nothing should call this" should not be in the public surface. `iris_migrate_scaling` and `iris_input_scaling` are fine — they are a UI's honest answer to "why did my old instrument load worse."

---

## 13. DEFECTS FOUND (independent of any verdict)

| # | Defect | Evidence |
|---|---|---|
| **1** | **`iris_clampf` does not trap NaN.** `iris_clampf(NaN, 1e-4, 2.0) = NaN` — the ternary `v<lo ? lo : (v>hi ? hi : v)` falls through on unordered comparisons. So `iris_set_learning(k, NaN, …)`, `(…, NaN)` and `iris_set_l2(k, NaN)` all store NaN. Each then NaN-traps on epoch 1, reseeds, and leaves the instrument unfitted. `iris_get_l2` returns `nan`. | `d.c` D1/D1b/D1c |
| **2** | **`lam0 = +inf` / `1e6` / `gain = 0` return SUCCESS on a dead instrument.** ret 0, status `IRIS_STATUS_OK`, `is_trained` 1, `fitted` 1, plausible `last_error` — and constant output for every input (span 0.000). No signal of any kind. | `d.c` D3b/D3c, `e.c` E1 |
| **3** | **`iris_train_elm_ex`'s "weights untouched on every refusal" is false.** Negative/NaN `lam0` overwrites both `w1` and `w2` and reverts a working instrument to unfitted. | `d.c` D3 |
| **4** | **`iris_retrain_new(k, seed, 0)` destroys the instrument.** Reseeds, then the epochs≤0 guard refuses — grid 0.0465 → 0.1522, `fitted = 0`, returns −1.0. Violates the file's own refusal convention. | `e.c` E5 |
| **5** | **The permitted maximum of `momentum` bricks 20/40 instruments** into `IRIS_DIVERGED_STUCK` at the *default* lr. `lr = 2.0` bricks 6/40. Defaults brick 0/400 at σ≤0.05, 2/400 at σ=0.10. | `d.c` D2, `f.c` F2 |
| **6** | **Header claim wrong:** "the split the sweep explored is unreachable [through the shipped API]" — `iris_train_elm_ex` is public and takes `gain_w`/`gain_b` separately. Measured reachable. | `e.c` E1 |
| **7** | **`gain_w`/`gain_b` sign is silently meaningless** (`iris_rand_sym` is symmetric): −1.414 is bit-identical to +1.414. | `e.c` E1 |
| **8** | **`iris_loo_error` at its default 600 underestimates true held-out error by 35%** (0.0750 reported vs 0.1162 actual) and is non-monotone in its epochs argument. Safe as a *ranking*, unsafe as a number shown to a user. | `e.c` E3 |

## 14. SUMMARY TABLE

| Knob | Range | At the extremes | Wekinator | Choosable? | Cost of removal | **Verdict** |
|---|---|---|---|---|---|---|
| `lr` | [1e-4, 2.0] | max: **destroyed**, 5/16 diverge, 6/40 bricked. min: 17× worse recall | **none** (Weka 0.3 fixed) | No — train error points backwards | 0 (l2 covers it) | **MAKE INTERNAL** (else clamp [0.01, 0.5]) |
| `momentum` | [0, 0.99] | max: **20/40 BRICKED**. min: identical, 1.7× slower | **none** (Weka 0.2 fixed) | No — nothing to choose | 0 | **MAKE INTERNAL** (else clamp [0, 0.9]) |
| `l2` | [0, 1.0] | both ends safe; max degrades recall 22×, 0 divergences ever | none (SVM `C` is nearest) | Direction yes, value no | **2.5× held-out on noisy demos** | **KEEP EXPOSED**, reskin as `iris_set_smoothing`, auto-set by LOO, clamp [0, 0.3] |
| `n_hid` | [1, 64] | 1–2 useless; 8–64 flat; ELM refuses <8 | **only MLP knob Wekinator exposes** | No (clean says 64, noisy says 4) | 2–5% clean, 33–43% noisy — same axis as l2 | **CLAMP HARDER [8,64]** + `iris_init_default` |
| `ceiling` | any int | ≥20000, ≤0, INT_MAX all identical; below = damage | none | Nothing to choose | 0 | **MAKE INTERNAL** |
| `epochs` (fixed) | any int | 1 → 37× worse; 200k → grid *worsens* | none (Weka 500 fixed) | No | Weka-parity reference | **KEEP EXPOSED** as reference only |
| `epochs` (slice) | any int | **proven bit-identical** at 1…100000, 0, −1 | n/a | n/a — frame budget | 0 | **KEEP EXPOSED** |
| `epochs` (correct) | any int | every value >20 strictly worse (drift 2.9×) | n/a | No | 0 | **DELETE param** |
| `epochs` (loo) | any int | non-monotone; default underestimates 35% | n/a | No | 0 | **DELETE param** |
| `epochs` (retrain_new) | any int | **0/−1 destroys the instrument** | n/a | No | 0 | **DELETE param** + fix ordering |
| `lam0` | **UNCLAMPED** | neg: destroys + false guarantee. 1e6/inf: **dead, reported healthy** | none | No — silent when wrong | 7–35%, same axis as l2 | **MAKE INTERNAL** (+ fix both defects) |
| `gain_w`,`gain_b` | **UNCLAMPED** | 0 = dead+healthy; sign meaningless; default beaten by 1.0 | none | No | 0 | **DELETE** |
| `k` (knn) | [1, min(8,n)] | fully clamped both ends; 15% total spread; recall always exact | **yes — a text field, default 1** | **Yes** | 1.6–9.9% | **KEEP EXPOSED** |
| `seed` | uint32 | 1.14–1.28× spread; 0 broken instrument in 400 | none | Nothing to choose — it's a die | the reroll itself | **KEEP EXPOSED** |
| `n_in`/`n_out`/`cap` | [1,32]/[1,16]/[1,4096] | out-of-range → NULL, correctly | n/a | Yes — it's the rig | the rig | **KEEP EXPOSED** |
| `iris_internal_set_legacy_norm` | 0/1 | "nothing else should call it" — its own comment | n/a | No | 0 | **MAKE INTERNAL** |

**Net: 22 runtime knobs → 8.** What survives: `n_in`, `n_out`, `cap`, `seed`, `n_hid` (as a memory decision, floored at 8), one smoothing control, k-NN's `k`, and the slice size. Everything removed is either provably inert across its safe range, provably a second spelling of the smoothing control, or provably capable of destroying an instrument — and in four cases, of destroying it while reporting `IRIS_STATUS_OK`.

---

# Yes. Twelve routes, all reachable, none requiring anything stupid.

Harnesses: built in a scratch directory that was not retained, with `cc -O2 -std=c99` against this repository's `iris.h`. All output below is real.

**Healthy reference** (defaults, 20 demos, seed 7): grid RMSE 0.0170, recall 0.0011, status OK.

---

## Tier 1 — broken and completely silent

### R1. `iris_set_learning` inside its own permitted box (`a_lr.c`, `a3.c`)

```
 2.00  0.85 |     0.123  OK    4000  recall 0.1226  grid 0.1207  max|w| 9.91  is_trained=1
```

Recall is **111x** worse than baseline, grid **7x**, and `iris_get_status()` returns `IRIS_STATUS_OK`. The divergence guard never fires because `max|w| = 9.91 < IRIS_W_LIMIT`, and the plateau test happily certifies the oscillating error as converged.

Census over the whole permitted rectangle, 9 lr x 6 momentum x 10 seeds = 540 runs, "broken" = grid > 5x baseline:

```
      mom:   0.00     0.50     0.85     0.90     0.95     0.99     (silentBad/reportedBad of 10)
lr  0.10 :    0/0      0/0      0/0      0/0      0/0      4/1
lr  0.75 :    0/0      0/0      0/0      0/0      8/2      0/10
lr  1.25 :    0/0      0/0      0/0      9/1      2/8      0/10
lr  1.75 :    0/0      0/0      4/0      7/3      1/9      1/9
lr  2.00 :    0/0      0/0     10/0      6/4      3/7      0/10

TOTAL runs 540   BROKEN-AND-SILENT 75   broken-and-reported 125
```

**lr=2.0 / momentum=0.85 is silently broken on 10 seeds out of 10.** Also note `lr=0.10, mom=0.99` — a conservative-looking learning rate — is silently broken 4/10.

**Reported?** No. **Actionable?** No: `iris_last_error` returns 0.123 vs 5e-6 healthy, but nothing in the API says what a bad number looks like.

**Can it be closed?** Yes, two ways. (a) The permitted box is wrong: the measured safe region is roughly `lr*momentum_amplification <= ~1`, and everything at momentum >= 0.90 with lr >= 0.75 should be refused, not clamped-and-accepted. (b) Cheaper and exact: targets live in `[0.1, 0.9]`, so a final MSE above ~0.01 is *arithmetically* a garbage fit regardless of task. Add a status (`IRIS_FIT_FAILED`) when `last_error` exceeds a fraction of the target-band variance. That closes R1, R2 and R10 at once with one comparison.

### R2. `iris_set_l2(1.0)` — the decay coefficient is unbounded above 1 (`b3.c`, `b4.c`)

`iris.h:1143` applies `w[h] -= wd * w[h]` with `wd = l2 * lr / n_ex`. Nothing bounds `wd < 1`. At the two permitted maxima and 3 demonstrations, `wd = 2.0/3 = 0.667`, i.e. **`w *= 0.333` on every single update**:

```
  ret=0.171144 status=OK is_trained=1
  w2[0]=-3.999e-31   played output span over the whole gesture plane = 0.000000000
```

Every hidden→output weight is annihilated to ~1e-31. The biases survive — because the header deliberately never decays biases — so the network becomes a *constant function with a plausible mid-range value*, not zeros:

```
  gesture (0.0,0.0) -> played 0.374598 0.365982 0.663907
  gesture (1.0,1.0) -> played 0.374598 0.365982 0.663907
```

Control, same seed and data with `l2=0`: span 0.369458. Status OK in both. At `n_ex=2`, `wd = 1.0` exactly — every weight is set to zero every update. At `n_ex=1`, `wd = 2.0` — every weight is sign-flipped every update.

`l2=1.0` alone at default lr is merely poor (recall 0.0011 → 0.0275), not fatal. It is the *permitted-max x permitted-max* corner that kills it.

**Reported?** No. **Closable?** Yes, trivially — clamp the product: `wd = min(wd, 0.5f)`, or bound `iris_set_l2`'s max by `n_ex/lr`. The route then does not exist.

### R8. Save before train, then load — defeats the `IRIS_NOT_FITTED` guard (`g_order.c`)

```
  source (never trained): out 0.5000 0.5000 0.5000 status=NOT_FITTED   <- guard works
  after save+load:        out 0.5302 0.5203 0.5034 status=OK is_trained=1
  truth here is           0.4533 0.4719 0.5700 ; grid RMSE 0.1272
```

`iris_save` will serialise an instrument with `fitted == 0` without a word, and `iris_load` (`iris.h:2046-2048`) unconditionally stamps `trained = 1; fitted = 1; status = IRIS_STATUS_OK`. The reloaded object plays the forward pass over `iris_reseed`'s random weights, in range, plausible, with no symptom anywhere — **exactly the failure the header calls "the highest on-stage risk in the library"**, restored by two public calls.

**Closable?** Yes, and it should be closed on the write side: `iris_save` should refuse (return 0) when `!k->fitted`. There is nothing to save.

---

## Tier 2 — the divergence-trap family: four independent laundering paths

Setup for all four: `iris_set_learning(k, 1.5f, 0.99f)` (permitted), train, then put the knobs back to the defaults and retrain — what any musician does.

### R4. The `DIVERGED_STUCK` refusal alternates refuse / accept (`l_stuck2.c`, `REPRO.c`)

```
  attempt 1: ret=-1        status=DIVERGED_STUCK    epochs=5     pinned=3
  attempt 2: ret=0.11195   status=TRAINING_DIVERGED epochs=1     pinned=3
  attempt 3: ret=-1        status=DIVERGED_STUCK    epochs=1     pinned=3
  attempt 4: ret=0.11195   status=TRAINING_DIVERGED epochs=1     pinned=3
  ...
   10 | 0.11195 | OK                |   4000 |  <- ACCEPTED, reported healthy
```

The guard at `iris.h:979` reads `if (!resume && k->status == IRIS_TRAINING_DIVERGED)` and then *writes* `IRIS_DIVERGED_STUCK` into that same field. On the next call the condition is false, so the guard skips itself. Every second attempt runs one epoch, re-diverges, sets `trained = 1` (the `break` falls straight through to `iris.h:1212`), and returns a float. By attempt 10 it reports `status=OK` after a full-looking 4000-epoch run with three weights still pinned at ±16.

**Closable?** Yes: test `k->status == IRIS_TRAINING_DIVERGED || k->status == IRIS_DIVERGED_STUCK`. One-line fix.

### R4b. The stuck-detector never scans the biases (`l_stuck2.c`)

`iris_internal_check_weights` clamps `w1, b1, w2, b2`, but the pinned-scan at `iris.h:980-985` walks only `w1` and `w2`. When divergence pins a *bias*, `DIVERGED_STUCK` is unreachable — permanently:

```
    seed 5: diverged with pinned weights=0 pinned biases=1
      retrain 1: ret=0.27611  status=TRAINING_DIVERGED epochs=1 grid=0.2273
      retrain 2: ret=0.27611  status=TRAINING_DIVERGED epochs=1 grid=0.2273
      retrain 3: ret=0.27611  status=TRAINING_DIVERGED epochs=1 grid=0.2273
      retrain 4: ret=0.27611  status=TRAINING_DIVERGED epochs=1 grid=0.2273
```

This is the exact "frozen at its damaged output, retrain does nothing, no message" scenario the header's long comment says it fixed — and the fix only covers the case where the pinned float happens to be a weight. `TRAINING_DIVERGED`'s documented remedy is "check lr/momentum, or reseed"; the musician *already* put lr/momentum back. Only `DIVERGED_STUCK` names `iris_retrain_new`. Seed 1 is worse still: three refusals, then `status=OK, epochs=4000, grid=0.1944`.

**Closable?** Yes: extend the scan to `b1`/`b2`. The arrays are contiguous — walk the same `nw` floats `iris_internal_check_weights` already walks.

### R5. The sliced trainer has no refusal at all (`e2.c`, `REPRO.c`)

```
  attempt 1: begin=1 epochs=1 status=TRAINING_DIVERGED progress=1.00 is_trained=1
  attempt 2: begin=1 epochs=1 status=TRAINING_DIVERGED progress=1.00 is_trained=1
  attempt 3: begin=1 epochs=1 status=TRAINING_DIVERGED progress=1.00 is_trained=1
```

`iris_train_begin` (`iris.h:1266`) never inspects `status`, and `iris_train_slice` calls `iris_internal_train_run(..., resume=1)`, which skips the guard by construction. `iris_train_progress()` returns **1.00** — a completed bar. This is the path the header tells UI authors to use and claims is "bit-identical to the blocking call"; on this input the blocking call returns `-1.0f` / `DIVERGED_STUCK` and the sliced one returns a finished, "trained" instrument. **The claim is false on exactly the input where it matters.**

**Closable?** Yes: run the pinned-check in `iris_train_begin` and return 0.

### R6. `iris_correct` clears the diverged state to OK (`REPRO.c`)

```
  iris_correct 1: ret=-1        status=DIVERGED_STUCK    pinned=3 grid=0.1230
  iris_correct 2: ret=0.11195   status=OK                pinned=3 grid=0.1230
  iris_correct 3: ret=0.11195   status=OK                pinned=3 grid=0.1230
```

Two presses of the "fix it now" button turn a diverged instrument into a **`IRIS_STATUS_OK`, `is_trained=1`** instrument with three weights still pinned at ±16 and grid error 7x baseline. The pre-scan sets `status = IRIS_STATUS_OK` unconditionally, and the per-epoch guard only fires on `> IRIS_W_LIMIT` — weights sitting *exactly at* the clamp never re-trip it.

### R7. Save/load launders divergence into a healthy-looking file (`REPRO.c`)

```
  saved 856 bytes while status=TRAINING_DIVERGED, 3 weights pinned at +/-16
  loaded: status=OK is_trained=1 pinned=3 grid=0.1230
```

`iris.h:2048` — `k->status = IRIS_STATUS_OK; /* a freshly loaded instrument carries no stale error */`. But the error is not stale: it is *in the file*, in the weights. The saved artefact is a permanently laundered broken instrument, and the `DIVERGED_STUCK` trap is disarmed for it forever.

**Closable?** Yes for R6 and R7 together: derive the diverged state from the weights, not from a status word — a pinned-weight scan on load and at the top of every trainer. The scan already exists.

---

## Tier 3 — silent wrong answers from correct-looking calls

### R9. `iris_classify_1nn` is missing the guard `iris_knn_predict` has 30 lines above it (`k_knn.c`)

```
  NaN on sensor 0        | 1nn: id=1 class 1.0  status=OK   | knn: status=NAN_TRAPPED
  +Inf on sensor 0       | 1nn: id=1 class 1.0  status=OK   | knn: status=NAN_TRAPPED
  1e30 both (overflow)   | 1nn: id=1 class 1.0  status=OK   | knn: status=NAN_TRAPPED
  -1e30 sensor 0         | 1nn: id=1 class 1.0  status=OK   | knn: status=NAN_TRAPPED
```

`iris_classify_1nn` initialises `int best = 0` (`iris.h:2237`). A non-finite or overflowing query makes every `d < best_d` false, so `best` stays 0 and the function returns the **first demonstration ever recorded**, verbatim, as a confident class label, with `iris_get_status()` reporting OK. `iris_knn_predict` initialises `bi[n] = -1` and has an explicit refusal for precisely this case, with a comment explaining it. The reasoning was never carried across. A stuck or unplugged sensor makes the classifier answer class 1 forever.

**Closable?** Yes — initialise `best = -1`, add the same three-line backstop. Not a design trade-off, just an omission.

### R3. One sensor the musician never moved during recording (`c_const.c`, `REPRO.c`)

`iris_fit_ranges` floors a zero-range channel at 1e-6. `iris_norm_in` then divides by that floor:

```
  s1 = 0.500000 (as recorded) -> 0.4502 0.3801 0.6799  status=OK
  s1 = 0.500001 (ONE ULP)     -> 0.3268 0.2321 0.7786  status=OK
  s1 = 0.6                    -> 0.4534 0.4140 0.7604  status=OK
  s1 = 0.4                    -> 0.6083 0.5448 0.4601  status=OK
```

A one-ULP change on a channel the musician never touched swings the normalised value from −1.0 to +1.0 and moves out1 by 0.148 (19% of the demonstrated range). Any real movement drives the pre-activation to ±197380, saturating every hidden unit — the instrument collapses into a **two-state switch driven by noise on an unused channel**. The 1e-6 floor prevents the NaN but converts it into a 1e6 gain.

`iris_novelty` does honestly return 1.0 here — that is the one working signal — but 1.0 also legitimately means "far from anything demonstrated", so it cannot distinguish the two.

**Closable?** Yes, and it is the highest-value fix in the list because it needs no numeric judgement: a channel with zero observed range carries **zero information**, so `iris_norm_in` should return a constant (0) for it and `iris_fit_ranges` should record the fact. Add a status (`IRIS_DEAD_INPUT`) naming the channel index. Then the route stops existing *and* the musician learns their sensor is unplugged.

### R10. The fighting-demonstration detector cannot see a contradiction (`h5.c`)

18 clean demos plus two literally contradictory takes at the identical gesture:

```
idx  id   stress
 17  18    0.009
 18  19   10.853  <-- contradictory take
 19  20    8.815  <-- contradictory take

worst id=19 margin=1.2312  -> UI condition (margin >= 2.5) is FALSE, nothing is said.
recall 0.1439 (misses its own demos by 14% of full scale), status OK
```

The two offenders score **10.9x and 8.8x** an ordinary example (~0.02). But `iris_worst_example_id` reports worst-over-*second-worst*, and a contradiction by definition produces at least two equally-stressed examples, so the margin is 1.23 and the feature stays silent on the cleanest possible instance of the problem it was built for. 20 examples, well above `IRIS_STRESS_MIN_EX`.

**Closable?** Yes: worst-over-*median* instead of worst-over-second-worst would read ~680 here. The header's own calibration data (clean-data worst-of-n rising with n) is what motivated second-worst; the median is equally drift-free and is not defeated by a pair.

### R12. k-NN and novelty read ranges only a train ever writes (`k2.c`, `g_order.c`)

```
  demos (600mm,-1.9g)=class1 (700mm,+1.9g)=class2 ; query (640mm,+1.85g)
  before iris_fit_ranges: class 1   novelty 1.0000
  after  iris_fit_ranges: class 2   novelty 1.0000
```

Same instrument, same data, **opposite classification**, decided entirely by an invisible call. `iris_record` does not refit ranges; `in_lo/in_hi` sit at `iris_init`'s defaults of 0 and 1, so a millimetre channel drowns an accelerometer channel. Status OK both ways. The header does document the requirement — but the selling point of the k-NN path is "ZERO training, ZERO seed", which *is* the workflow that skips `iris_fit_ranges`.

**Closable?** Yes: mark ranges dirty in `iris_record`/`iris_delete_*` and have the three range-consuming readers refit lazily. It is already O(n_ex) work in a function that is O(n_ex).

### R11. NaN passes straight through both setters (`j_nan.c`)

```
  iris_clampf(NaN, 0.0001, 2.0) is bad? 1
  before: grid=0.0170 fitted=1
  iris_set_learning(k, NaN, 0.85) -> k->lr is NaN
  next iris_correct: ret=1 status=NAN_TRAPPED fitted=0  -> weights reseeded, instrument lost
```

`iris_clampf` passes NaN — every comparison with NaN is false, a fact the header states explicitly at `iris_predict` and then does not apply at `iris.h:504` / `iris.h:548`. A NaN from a UI slider (empty text field, divide-by-zero in the app's own scaling) silently poisons `lr`, and the next train reseeds the practised instrument away. This one **is** reported (`IRIS_NAN_TRAPPED`, and `fitted` drops to 0 so playback degrades to silence rather than noise) — but the destruction is not recoverable, and the header's promise of "previous weights preserved" covers only poisoned *examples*, not poisoned hyperparameters.

`+Inf` clamps correctly (to 2.0 / 0.99) and then lands you in R1.

**Closable?** Yes: one `iris_isbad` test in each setter. Two lines.

---

## Routes I tried that are genuinely closed

Worth stating, because they show the guards that *were* written are solid:

- **Dimension bounds** (`i_bounds.c`). `n_in`, `n_hid`, `n_out`, `cap` at 0, at max, at max+1, and negative — `iris_init` refuses all 8 out-of-range shapes and `iris_size` returns its 0 sentinel. The smallest legal instrument (1/1/1/1) and the largest (32/64/16/4096, 841.9 KB) both train and play correctly. The `IRIS_MAX_EX` overflow the header worries about is genuinely unreachable.
- **`n_hid` vs `n_ex` at extremes** (`d_nhid.c`). 387 parameters against 2 examples: recall 0.0000, grid 0.1701 — but that is "two examples is not enough data", not a library defect, and the LOO section documents it. `n_hid=1` with 64 examples is merely poor (grid 0.0285). No divergence, no NaN, no crash across the full 5x8 grid.
- **Load into a differently-shaped instrument** (`g_order.c`). `n_in`, `n_hid`, `n_out` mismatches and `cap < n_ex` are all refused; `cap >= n_ex` correctly accepted. Payload-length and unsigned-`n_ex` checks work.
- **Predict / train before record** (`g_order.c`). `IRIS_NOT_FITTED` fires correctly, outputs 0.0 with no demos and the range centre with demos; `iris_train_converge` on an empty store returns −1.0f; `iris_train_begin` returns 0; `iris_train_epochs(k,0)` returns −1.0f.
- **Duplicates and degenerate data** (`h_data.c`). 20 identical demos, all-identical outputs, and all-identical inputs all train to sane constants with correct recall; ELM needed zero ridge doublings even in the all-identical-input case the header flags as the trace≈0 corner.
- **NaN at the record door and at play time** — refused / trapped correctly in `iris_record`, `iris_predict`, `iris_knn_predict`. Only `iris_classify_1nn` (R9) leaks.

---

## Verdict against your standard

> *A student should not be able to break their instrument by setting a number the API offered them.*

That standard is currently not met, and R1 is the clean violation: **75 of 540 `(lr, momentum)` pairs the API accepts leave a broken instrument reporting `IRIS_STATUS_OK`**, with an entire cell (`lr=2.0, momentum=0.85`) failing on every seed tested. R2 is the same violation with two setters instead of one.

The cheapest single change that retires the most routes is **a fit-quality status derived from `last_error` against the target band** — targets are always in `[0.1, 0.9]`, so the threshold needs no per-task tuning. That covers R1, R2 and R10's silence. After that, in order of cost-to-benefit: `iris_save` refusing an unfitted instrument (R8, two lines), the `DIVERGED_STUCK` condition including its own status value and scanning biases (R4, R4b, two lines), `iris_train_begin` checking pinned weights (R5, three lines), `iris_isbad` in both setters (R11, two lines), `best = -1` in `iris_classify_1nn` (R9, four lines), and clamping `wd` (R2, one line). R3 and R12 need slightly more design but are both mechanical.