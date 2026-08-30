Everything below is measured on this host (Apple clang, `-O2 -std=c99`). Harnesses: a scratch directory that was not retained — `sweep.c` (the activation arms), `speed.c`, `fmt.c`, `verify.c`, `task.h`, variant headers `act_{A,B,C,D,E}.h`, raw CSV `out_{A,B,C,D,E}.csv`. `act_A.h` is byte-identical to `iris.h`, and `sh build.sh audit` passes on the untouched tree (0 failed).

---

# THE STRUCTURAL FACT THAT DECIDES EVERYTHING ELSE

**Only the forward pass is baked into a saved instrument.** A file carries `w1,b1,w2,b2`, the four range vectors, the examples, ids, the rng word, and a version word. What turns those bytes back into sound is exactly: `iris_norm_in` (+ `in_center` from the version word) → `iris_tanh` → `iris_sigmoid` → `iris_denorm_out` (+ `IRIS_OUT_LO/HI`) → `iris_clampf`, over a fixed 1-hidden-layer topology whose three dimensions are in the header. I audited the whole path: **nothing in it is caller-settable or overridable from a build flag.** `IRIS_OUT_LO/HI` are unguarded `#define`s (no `#ifndef`), and `-DIRIS_NO_GUARDS` is provably inert on healthy predictions — same 41×41×3 grid hash `32778ccffb525716` with and without.

So the freeze line is sharp:

| Baked into what a saved instrument MEANS — decide NOW | Free forever after | 
|---|---|
| `iris_tanh`, `iris_sigmoid` | the backward pass / `d_hid` |
| `IRIS_OUT_LO/HI = 0.1/0.9` | `lr`, `momentum`, `l2`, epochs, ceiling |
| the two input scalings + version-word dispatch | ELM, `lam0`, gains, LOO, k-NN |
| the header field list and payload order | the status enum (not in the file) |
| the topology (one hidden layer, `n_hid` in the header) | every function name (source, not format) |

The backward pass being on the right-hand side is the single most useful thing in this report: **the largest remaining mathematical objection is not a freeze decision at all.** It can be fixed in 2029 without touching one saved instrument.

---

# 1. THE ACTIVATION — freeze [3/2]. Do not take [7/6].

**Design.** 5 arms × 6 target surfaces × N∈{5,10,20,50} × σ∈{0,.05,.10} × 32 paired seeds × 2 trainers = **23,040 fits**, 2,304 paired datasets per arm per trainer. Data *and* initial weights identical across arms. Scored as MSE against clean truth on a 41×41 held-out grid. Targets: SMOOTH (the repo's own reference from `tests/experiment.c`), STRUCTURED (ridge+bump+cliff), SATURATING (targets pinned near the ends), PERIODIC, NEAR-LINEAR, CLIFF.

Arms: **A** = `[3/2]` as shipped · **B** = `[7/6]` Padé `x(135135+17325x²+378x⁴+x⁶)/(135135+62370x²+3150x⁴+28x⁶)` · **C** = `[3/2]` forward with its *exact* derivative `((s²−9)/(3(3+s²)))²` in the backward pass · **D** = true `tanhf` from libm · **E** = `[7/6]` rescaled by 1/tanh(3) so it saturates exactly at `|s|=3`.

**First result: [7/6] *is* true tanh.** Max `|p−tanh|` falls from 0.023520 to 0.000096 (245×), and the backward factor becomes exact — `(1−a²)/p'` is 1.0000 at x = 0.5, 1.0, 1.5, 2.0, 2.5 where `[3/2]` reads 0.972, 0.889, 0.750, 0.556, 0.306. Empirically B and D are the same instrument: **median B/D grid-MSE ratio 0.999999, geometric mean 0.999859.** So this experiment is not "which approximant"; it is "does the shipped approximant's error help or hurt".

**It helps. Backprop, paired vs A, 2,304 datasets:**

| arm | median | **geomean** | 95% CI (2k bootstrap) | % worse than A | sign test |
|---|---|---|---|---|---|
| **B `[7/6]`** | 1.0088 | **1.0384** | **[1.0265, 1.0506]** | 57.2% | z = **+6.92** |
| **D true `tanhf`** | 1.0089 | 1.0385 | [1.0272, 1.0516] | 57.2% | z = +6.92 |
| **E `[7/6]` sat@\|s\|=3** | 1.0079 | 1.0353 | [1.0241, 1.0474] | 57.0% | — |
| **C `[3/2]`+exact deriv** | 0.9984 | **0.9860** | [0.9773, 0.9955] | 47.8% | z = −2.12 |

ELM (`iris_train_elm`, lam0=1e-4): B = 1.0105 geomean, C = **1.0000 exactly** — the sanity check, since ELM never runs the backward pass.

Per target (backprop geomean vs A): SMOOTH 1.0005 · STRUCT 1.0063 · **SATUR 1.1634** · PERIOD 1.0436 · LINEAR 1.0013 · CLIFF 1.0243.

**Mechanism, isolated.** Arm E moves the saturation point of an *accurate* tanh to `|s|=3`, matching `[3/2]`'s. It is 1.0353 — indistinguishable from plain `[7/6]`. **The saturation point is not the mechanism.** The win comes from the mid-range shape: `p32` overshoots tanh by up to +0.0235 at x=1.566, i.e. it is a steeper sigmoid with a hard floor on gradient flow past `|s|=3`. That is a capacity control — the same axis the L2 study and the `n_hid` audit each rediscovered by a different lever.

**Cost of taking it anyway:** +14.6% per epoch (10.28 → 11.78 ms for 6000 epochs × 20 ex), **+25% epochs to plateau** (median 6400 → 8000), so **+43% training wall clock**; and **+23% on the on-stage hot path** (predict 32.8 → 40.4 ns). Divergences unchanged: 7/2304 in every arm.

### VERDICT — FREEZE `iris_tanh` AS `x(27+x²)/(27+9x²)`, OUTPUT-CLAMPED, PERMANENTLY.

The earlier 6.3%-worse single-task result replicates in direction across six targets at a smaller magnitude (3.8%). The objection "the approximant is 245× less accurate than it could be" is answered: **accuracy is not the objective, and buying it costs 3.8% held-out and 43% of the training budget.** Rewrite the PART 1 comment to say this — that the approximant is a chosen nonlinearity with a measured advantage over true tanh, not a compromise we regret — and cite the 2,304 pairs and the CI. The `[7/6]` sweep goes in `docs/negative-results/`.

**And note what this makes true:** the backward pass's `1−a²` is *not* the derivative of the forward pass, and that is now permanent as a *description of the forward pass*, not as a defect. Arm C says fixing it is worth **1.4%** (geomean 0.9860, CI [0.977, 0.996]) — real but marginal, and my implementation via `iris_artanh` costs 2.4× per epoch. **Do not do it now.** It is on the free-forever side of the line; a cached-preactivation version can land any time it earns its 1.4%.

---

# 2. THE MINIMAL PUBLIC KNOB SET — eight

Adopt the audit's verdicts. What stays, and what a musician does with each:

| Survivor | What the musician is actually deciding |
|---|---|
| `n_in`, `n_out` | the rig: how many sensors, how many sound parameters. Not tuning. |
| `cap` | "room for how many gestures." Pure memory, verified linear at 32 B/example. |
| `n_hid` | **a memory decision on an MCU, and now also a permanent one** — see below. Never advertised as a quality control: 8–64 is flat (grid 0.0279/0.0275/0.0269/0.0277/0.0290/0.0276/0.0255). |
| `seed` / `iris_retrain_new(k, seed)` | "give me a different one." A die, not a setting. Bounded: max/min grid RMSE 1.14–1.28× over 32 rerolls, 0 broken instruments in 400. |
| `iris_set_smoothing(k, 0..1)` → l2 ∈ [0, 0.3] | "stick tightly to my demos, or smooth between them." The only knob that carries real gain (2.5× held-out at σ=0.10), monotone in both directions across its whole range, 0 divergences at any value. Raw `iris_set_l2` stays as the mechanism, clamped to [0, 0.3]. |
| `iris_tune_smoothing(k)` → returns the chosen value | one explicit, occasional act that runs the 5-rung LOO sweep and **hands back the number** so it is visible, pinnable and overridable. Not automatic: 37 s on the S3 at N=20, 202 s at N=50, worse than l2=0 on 16.7% of datasets, and 2.36 distinct picks from one fixed dataset across 16 rerolls. Ship it beside `iris_set_l2`, never instead of it. |
| `iris_knn_predict(k, …, kk)` | "how many demonstrations blend together." k=1 snaps, k=8 smears, audible, clamped both ends, 15% total spread, recall exactly 0.0000 at every k. Precedent: Wekinator's own text field. |
| `iris_train_slice(k, epochs)` | frame budget. Proven bit-identical at 1, 10, 500, 2000, 100000, 0 and −1. The one budget knob that cannot hurt you. |

Removed: `lr` and `momentum` → `iris_internal_` (momentum 0.99 bricks 20/40 instruments at the *default* lr; lr 2.0 bricks 6/40; the safe range of momentum is a pure speed knob and the safe range of lr is covered by l2 — and the only number a UI can show points backwards: lr=0.001 → grid 0.0693 with trainMSE 1.17e-2, lr=0.05 → grid 0.1472 with trainMSE 1.25e-3). Ceiling → internal. `lam0`, `gain_w`, `gain_b` → internal. Epochs parameters on `iris_correct`, `iris_loo_error` and `iris_retrain_new` → deleted. `iris_internal_set_legacy_norm` → `#ifdef IRIS_TESTING`. `iris_train_epochs` stays public as the Weka-parity reference only, out of the README front matter.

---

# 3. WHAT ELSE MUST CHANGE BEFORE THE FREEZE

### 3a. `n_hid` is permanent per instrument — so raise the floor to 8 in `iris_init`. **NOW.**

`n_hid` is header word `h[3]`, and `iris_load` refuses a mismatch (verified: loading a nh=12 file into a nh=16 arena returns 0). So the width a student picks on day one is the width that file has forever. And measured:

```
  nh= 4  iris_init OK    iris_train_elm -> -1
  nh= 7  iris_init OK    iris_train_elm -> -1
  nh= 8  iris_init OK    iris_train_elm ->  0
```

A student can create — and save — an instrument that the instant trainer will refuse for the rest of its life, with no warning at the point of the decision. **Move the `nh >= 8` check from inside `iris_train_elm` into `iris_init`.** Also add `iris_init_default(mem, bytes, n_in, n_out, cap, seed)` picking nh internally; nh=1–2 is useless (recall 0.0823, 34× worse) and nh>12 buys 2–5%, so nobody should be choosing this by hand.

### 3b. Put a CRC32 in the file. **NOW — this is the only real format change I recommend.**

Measured: **354 single-bit flips across the payload, 354 accepted silently, worst prediction shift 0.282 of full scale.** The round trip is otherwise perfect (playback diff 0.000000000 over 41×41×3). A flipped bit in `w1` passes the magic word, the version word, the dimension check, the unsigned `n_ex` check and the length check, and then plays a different instrument out of a file that looks fine. That is precisely the silent-failure class this format's own comments are written against. Cost: **+4 bytes, and 0.012 ms host / ~3.2 ms scaled to the S3** for a table-free bitwise CRC32 over a 1912-byte file. Adding it after the first student saves is impossible; adding it today is ten lines. Call it v4, keep v1/v2/v3 loading forever.

### 3c. Carry the training recipe in the file. **NOW.**

The v2 rng word exists because a corrected instrument saved and reloaded would "take a different shuffle path on its next correction — same weights, quietly diverging futures." That exact bug is still open one level up. Measured: train with `l2 = 0.1`, save, load into a fresh arena (which silently starts at `l2 = 0`), then have both "keep working" by retraining the same demos —

```
  playback after load:      0.000000000     (bit-identical, as promised)
  after both retrain:       >0.02 of full scale in 34/48 cases, worst 0.1205
```

The file carries the examples *so the recipient can keep working*. Keeping working means retraining, and today retraining a loaded instrument gives a different instrument for a reason nothing in the file records. **Add one float word for the smoothing setting** to v4, alongside the CRC. Four more bytes.

### 3d. `iris_peek`. Free later, but do it now to prove the format is sufficient.

`iris_load` requires the arena to already match `n_in/n_hid/n_out`, and there is no way to ask a file its shape first — even though the header states it (`n_in=2 n_hid=12 n_out=3 n_ex=10`). A host application cannot size its arena from an arbitrary instrument file. `iris_peek(buf, bytes, &n_in, &n_hid, &n_out, &n_ex)` is additive, needs no format change, and its absence is the one thing that would make a v4 header look incomplete in hindsight.

### 3e. Fix the four destroy-while-reporting-healthy defects. Not format, but do them in the same pass.

All reproduced this session:

- `iris_retrain_new(k, 999, 0)`: **grid RMSE 0.0107 → 0.2308, trained 1 → 0, status 4, returns −1.0.** Reseeds *then* refuses. Violates the file's own stated refusal convention. Deleting the `epochs` parameter fixes it by construction.
- `iris_train_elm(k, 1e6, …)`: **ret 0, status 0, is_trained 1, last_error 0.0698** — and `pred(.2,.8)` = `pred(.9,.1)` = `[0.5060 0.5413 0.4841]`. Every gesture, one sound, reported healthy. Same for `+inf` and for `gain = 0`. Making `lam0`/gains internal removes the door; add a post-solve output-span check regardless.
- `iris_set_l2(k, NaN)`: `iris_get_l2` returns `nan`, then training traps and leaves `trained=0`. **`iris_clampf`'s ternary falls through on unordered comparisons** — one `iris_isbad` at the top of the setters fixes all three.
- `iris_train_elm_ex` with negative/NaN `lam0` overwrites both weight arrays, contradicting the header's "weights untouched on every refusal."

### 3f. Status vocabulary. Free later — but it costs nothing to do now.

`IRIS_DIVERGED_STUCK = 5` sits before `IRIS_NOT_FITTED = 4`; renumber to contiguous order while nothing has serialised them (nothing does — status is not in the file). There is no status for "alive but flat," which is what 3e's dead-instrument check needs; add `IRIS_OUTPUT_COLLAPSED`. Both are pure source changes.

---

# 4. THE FREEZE STATEMENT — for the README

> ## What is permanent
>
> From v1.0, a saved instrument is a permanent object. The file format, and everything that turns its bytes back into sound, is frozen: the magic word and header layout, the payload order, the two input scalings and the version word that selects between them, the output band `[0.1, 0.9]`, the one-hidden-layer topology, and the activation — `tanh(x) ≈ x(27+x²)/(27+9x²)`, clamped to `[-1,1]`. That activation is a deliberate choice, not an approximation we intend to improve: a Padé [7/6] is 245× more accurate and is empirically identical to true `tanh`, and across 2,304 paired datasets on six target surfaces it is **3.8% worse held-out** (95% CI [1.027, 1.051], worse on 57.2% of pairs) while costing 43% more training time and 23% more per prediction. It will not change. Every older file version loads bit-identically, forever.
>
> ## What may still change
>
> Everything that produces weights, as opposed to everything that reads them. The trainer, the backward pass, the stopping rule, the learning rate and momentum, regularisation, the ELM solver and its ridge, the leave-one-out estimator, k-NN, the guards and the status codes are all implementation, and we will keep improving them. A change to any of them means that **retraining** your demonstrations may give you a different instrument — which is why retraining is always an explicit act you take, never something a load does behind your back. It does not mean the instrument you already saved will play differently. That one is a vow: your file is your instrument, the forward pass is frozen, and the golden hashes in `tests/audit.c` fail the build if anyone — including us — changes it.

---

**Do all of §3 in one commit, then cut v1.0.** The activation question is closed with the widest evidence available; the only genuinely irreversible items left are the CRC word, the smoothing word, and the `n_hid >= 8` floor, and all three are cheap today and impossible tomorrow.