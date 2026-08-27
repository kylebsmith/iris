# The Mathematical Audit of `iris`

**Target:** `iris.h`, v0.4.0, 2050 lines.
**Regime under audit throughout:** `n_in = 2`, `n_hid = 12`, `n_out = 3–8`, ~75 trainable parameters, `n_ex ≈ 5–50` (`cap` up to 4096), IEEE binary32, no libc, ESP32-S3 class target, and a hard bit-determinism requirement.

**Verification convention.** Every claim about the *code* below — line numbers, arithmetic, algebra — was checked against the file in this session. Every claim about the *literature* is from working knowledge and is **not** bibliographically verified here; anything going into a thesis needs the DOI checked against the publisher record. Where a prior review marked a citation "verified via Schmidhuber's priority page," treat it as unverified: that page is self-published advocacy, and you cannot disqualify it as a source in one paragraph and lean on it as your authority in the next.

---

## 1. What the algorithm is called

### In the terms a paper would use

> Mappings are learned by a **one-hidden-layer feedforward network** (an MLP) with **rational Padé approximants to `tanh` in the hidden layer and to the logistic function in the output layer**, trained by **per-example backpropagation with classical (heavy-ball) momentum**, η = 0.10, μ = 0.85, with **random reshuffling** each epoch. The per-example objective is **half the summed squared error** over output dimensions, computed on **min-max feature-scaled inputs** (to [−1, +1] for v3 instruments; [0, 1] for v1/v2 files) and **targets affinely rescaled to [0.1, 0.9]**. Weights are initialised **U(−1/√fan_in, +1/√fan_in)**, biases at zero. Optimisation terminates on a **windowed relative-improvement (`ftol`) criterion applied to the training objective** — less than 10% decrease per 2000-epoch window — under a 60,000-epoch ceiling, with an absolute error floor at 10⁻⁶. Outputs are **clamped to the demonstrated range at inference only**; the clamp is invisible to the gradient. Two alternative estimators ship alongside: the **1-nearest-neighbour rule under range-normalised Euclidean distance** with lowest-index tie-breaking, and a **regularised Extreme Learning Machine** — a frozen randomly-drawn `tanh` feature layer with a **ridge-regularised linear readout solved in closed form via Cholesky factorisation of the trace-scaled normal equations**, with targets mapped through the **inverse logistic link** and fitted by *unweighted* least squares in logit space.

**Do not write "tanh/logistic MLP" without qualification.** The file's own PART 1 comment refutes it: `iris_tanh` is `p(x) = x(27 + x²)/(27 + 9x²)` with a hard clamp at |x| > 4.9, and `p(4.9) = 1.02822`. The activation is neither `tanh` nor codomain-preserving nor continuous. §5 attack 4 covers what follows from that.

The one term of art you should adopt where the ELM is concerned is **SLFN — single-hidden-layer feedforward network** — because that is the phrase Huang's literature uses, and a reviewer reading your ELM section expects it. Using "SLFN" there and "one-hidden-layer MLP" in the backprop section is not an inconsistency; it is two literatures naming the same architecture.

### The same thing in plain English

You have a small network: two knobs in, twelve little units in the middle, a handful of synth parameters out. Each middle unit adds up its inputs and bends the total through an S-curve; each output does the same again. Training means: show it one demonstration, see how wrong it is, nudge every connection a little in the direction that would have been less wrong, and repeat — tens of thousands of times, in a fresh random order each pass. "Momentum" means the nudges have inertia: a consistent direction builds speed, a jittery one cancels out.

Three things about this description are not the textbook default, and a reviewer will assume the default unless you say otherwise:

1. **The S-curves are cheap polynomial fakes, not the real functions**, and the fake overshoots slightly before it is cut off. The nudge calculation uses the slope of the *real* function, not the slope of the fake one it actually ran. The two disagree by up to a factor of two in ordinary use.
2. **Training stops when training error stops improving**, not when performance on unseen input stops improving. Those are different events and the file never measures the second.
3. **The output squashing is the 1980s pairing** — S-curve output with squared error — which is known to kill the learning signal precisely when a unit is confidently wrong.

---

## 2. Where every piece came from

| Component | Standard name | Originator | Year | Citation (unverified — check before use) |
|---|---|---|---|---|
| Gradient computation through the layers | Reverse-mode automatic differentiation / backpropagation | **Linnainmaa** (algorithm); disputed downstream | 1970 | MSc thesis, Univ. Helsinki; published as *BIT* 16(2):146–160, 1976, DOI 10.1007/BF01931367. **For a peer-reviewed priority account use Griewank, "Who invented the reverse mode of differentiation?", *Documenta Mathematica*, 2012**, not Schmidhuber's web page. |
| — earlier adjoint precursors | Continuous adjoint / chain-rule recursion in staged systems | Kelley; Bryson; Dreyfus; Amari | 1960–67 | Kelley, *ARS J.* 30(10):947–954; Dreyfus, *JMAA* 5(1):30–45; Amari, *IEEE Trans. Elec. Comp.* EC-16(3):299–307 |
| — contested NN application | Backprop for neural networks | **Werbos** — *disputed* | 1974 / 1982 | *Beyond Regression*, PhD thesis, Harvard (1974); "Applications of advances in nonlinear sensitivity analysis," Springer LNCIS 38 (1982). See below. |
| — popularisation | Backprop, learning internal representations | Rumelhart, Hinton & Williams | 1986 | *Nature* 323:533–536, DOI 10.1038/323533a0; and PDP vol. 1, MIT Press, pp. 318–362 |
| Momentum term | Heavy-ball / classical / Polyak momentum | **Polyak** | 1964 | *USSR Comp. Math. Math. Phys.* 4(5):1–17, DOI 10.1016/0041-5553(64)90137-5 |
| — its arrival in NN practice | The "acceleration" term α·Δw(t−1) | Rumelhart, Hinton & Williams | 1986 | PDP vol. 1 (safer citation for the formula than the *Nature* paper) |
| — the identification of the two | NN momentum = Polyak heavy ball | Qian | 1999 | *Neural Networks* 12(1):145–151, DOI 10.1016/S0893-6080(98)00116-6. **This is the citation that closes the loop.** |
| — the method it is *not* | Nesterov accelerated gradient | Nesterov | 1983 | *Soviet Math. Dokl.* 27(2):372–376. Verified absent from the code. |
| — CM vs NAG in DL practice | — | Sutskever, Martens, Dahl & Hinton | 2013 | ICML, PMLR 28(3):1139–1147 |
| `tanh` hidden units; input centring; the Padé shortcut; target-band headroom | — | LeCun, Bottou, Orr & Müller, "Efficient BackProp" | 1998 | *Neural Networks: Tricks of the Trade*, LNCS 1524, pp. 9–50, DOI 10.1007/3-540-49430-8_2. §4.3, §4.4, §4.5, §4.6. **The file's only ML citation, and it is the right one.** |
| Weight init `U(±1/√fan_in)` | Fan-in-scaled uniform | — (folklore); **exactly PyTorch's `nn.Linear` default** | — | `reset_parameters` → `kaiming_uniform_(a=√5)` ⇒ gain √(1/3), bound = √(1/3)·√(3/fan_in) = 1/√fan_in. Identity verified algebraically. |
| — the schemes it is *not* | LeCun init; Glorot/Xavier; He | LeCun 1998 §4.6; Glorot & Bengio 2010; He et al. 2015 | | AISTATS PMLR 9:249–256; ICCV DOI 10.1109/ICCV.2015.123 |
| Sigmoid output + squared error | — | Rumelhart et al. convention | 1986 | Critique: Solla, Levin & Fleisher, *Complex Systems* 2:625–640 (1988); Bishop 1995 §6; Goodfellow et al. 2016 §6.2.2.2 |
| Shuffle | Fisher–Yates / Knuth shuffle | Fisher & Yates; Durstenfeld | 1938 / 1964 | *CACM* 7(7):420. Modulo-biased variant; bias O(n/2³²), immaterial. |
| RNG | xorshift32 | Marsaglia | 2003 | *J. Statistical Software* 8(14) |
| Stopping rule | Relative function tolerance (`ftol`) termination test | Optimisation folklore; codified | — | Nocedal & Wright, *Numerical Optimization*, 2nd edn. **Not** early stopping — see Prechelt, "Early Stopping — But When?", LNCS 1524 (1998), whose every criterion is defined on *validation* loss. |
| — the thing that *does* regularise here | Implicit regularisation by path truncation | Yao, Rosasco & Caponnetto; Raskutti, Wainwright & Yu | 2007 / 2014 | *Constr. Approx.* 26:289–315; *JMLR* 15:335–366 |
| Frozen random hidden layer + linear readout | **Extreme Learning Machine** — name contested | Huang, Zhu & Siew | 2006 | *Neurocomputing* 70:489–501 |
| — prior art | Feedforward NNs with random weights | **Schmidt, Kraaijveld & Duin** | 1992 | Proc. 11th IAPR ICPR, vol. II, pp. 1–4 |
| — prior art | RBF nets with fixed centres + LS readout | Broomhead & Lowe | 1988 | *Complex Systems* 2(3):321–355 |
| — cousin, *not* this | Random Vector Functional Link (RVFL) | Pao, Park & Sobajic | 1994 | RVFL requires input→output skip connections. **There are none here.** Not RVFL. |
| — the same object in another literature | Random-features ridge regression / "random kitchen sinks" | Rahimi & Recht | 2007–08 | NIPS. `tanh` features in place of random Fourier features. |
| — the priority dispute in print | — | Wang & Wan, "Comments on 'The Extreme Learning Machine'" | 2008 | *IEEE Trans. Neural Networks* 19(8):1494–1495, with Huang's reply |
| Ridge penalty | Ridge regression / Tikhonov regularisation | Hoerl & Kennard | 1970 | *Technometrics* 12(1):55–67 |
| — in the ELM literature | Regularised ELM | Huang, Zhou, Ding & Zhang | 2012 | *IEEE Trans. SMC-B* 42(2):513–529 |
| Cholesky with an added multiple of the identity, doubling on failure | — | Nocedal & Wright **Algorithm 3.3**; cousin of Gill–Murray modified Cholesky | — | *Numerical Optimization*. **Not** Marquardt scaling — Marquardt (1963) damps by `diag(JᵀJ)`, per-parameter and anisotropic; line 1570 is a single isotropic scalar `λ₀·tr(A)/K`. |
| Logit-space least squares | Linearised-link / transform-both-sides estimation; **Berkson's minimum-logit-χ²** | Berkson | 1944/1953 | *JASA*. The classical form is **weighted**; this one is not. |
| — principled modern alternative | Beta regression | Ferrari & Cribari-Neto | 2004 | *J. Applied Statistics* 31(7):799–815 |
| Nearest neighbour | 1-NN rule | Fix & Hodges; Cover & Hart | 1951 / 1967 | USAF Tech. Report 4; *IEEE Trans. IT* 13(1):21–27 |
| Inverse-distance-weighted regression, p = 2, ε-guarded | **Shepard's method** (local/modified Shepard, restricted to k neighbours) | Shepard | 1968 | ACM National Conference, pp. 517–524 |
| Min-max range-normalised Euclidean metric | Range normalisation | — | — | Matches Weka `EuclideanDistance` with `dontNormalize=false` under `IBk`. |
| Inverting the Padé approximant | Newton–Raphson, fixed 5 iterations | — | — | Correct choice: it inverts the *shipped* approximant, not true `tanh`, so the round trip is exact to float32. |

### The two contested priorities, honestly

**Backpropagation.** The algorithm is Linnainmaa 1970: reverse-mode AD on a sparse computation graph *is* backpropagation, and `iris.h`'s `d_out → d_hid →` weight-gradient recursion is a hand-specialised instance of it. **Werbos 1974 is genuinely disputed** and you must not assert it flatly. The defensible phrasing is: *"Werbos (1974) applied the method to general nonlinear statistical models; whether the 1974 thesis contains the neural-network-specific formulation is disputed (cf. Werbos 1982)."* Rumelhart–Hinton–Williams 1986 get popularisation and the hidden-representation demonstration, not the method. Griewank (2012) is the peer-reviewed place to hang all of this, and it is more careful than the advocacy sources about whether reverse-mode AD and NN backprop are one lineage or two.

**ELM.** The name is contested and there is a published objection with a published reply (Wang & Wan 2008). The file's own posture — *"a 20-year argument about whether it deserves one"* (line 1416) — is correct. Cite Huang 2006 **and** Schmidt et al. 1992 together, and note the random-features framing (Rahimi & Recht), which sidesteps the fight entirely and connects the method to a literature no one disputes.

### The provenance gap

`iris.h` contains **two** literature citations in 2050 lines: LeCun et al. 1998 (in the `in_center` struct comment) and *Fiebrink & Sonami, NIME 2020* (line 1352, carrying the file's central design argument for the correction path). Backpropagation, momentum, ridge, Cholesky, Shepard, nearest-neighbour, Fisher–Yates and xorshift are all uncited.

For a PhD artifact **that** is the exposure, not any individual attribution error. And one sentence makes it worse: line 775, *"PART 8 — TRAINING (backpropagation) / The only genuinely new idea in this file, and it is one idea."* Read charitably it means "the only non-trivial idea here." Read as written, in the one section with no citation at all, it claims a 1970 method as new. Fix the sentence and add the citation.

---

## 3. Is it contemporary?

Verdicts: **old-and-correct** (nothing better exists) · **old-and-fine-here** (superseded in general, right for this regime) · **old-and-a-liability** (fix it).

| Component | Verdict | What a 2026 practitioner would use — and whether it wins at N=20, on an MCU, in float32, deterministically |
|---|---|---|
| Backprop | **old-and-correct** | Every framework is Linnainmaa's method. Nothing to change. |
| Heavy-ball momentum | **old-and-correct here** | Adam. At 75 parameters, 20 examples and a bit-determinism requirement, Adam buys nothing: its advantage is per-parameter scaling across heterogeneous gradient magnitudes in deep, wide models, and it costs two extra state arrays and a bias-correction division. **Heavy ball wins here.** One caveat you must state: Polyak's acceleration guarantee is for the *deterministic full-gradient* case. This applies it per example. "Momentum makes training roughly three times faster" (line 790) is an empirical claim about this file, not a theorem. |
| `tanh` hidden units | **old-and-correct here** | ReLU/GELU/SiLU. Their advantage is gradient flow through *depth*; at depth 1 there is none. Worse, ReLU makes the output piecewise-linear, so a smooth gesture produces audible kinks in a synthesis parameter — against this library's headline feature. **`tanh` is the right call and you can say so confidently.** |
| Padé approximant with clamp at 4.9 | **old-and-a-liability** | The approximation is fine and LeCun explicitly endorses the rational shortcut (§4.4). The **clamp point** is the liability: it makes both activations discontinuous. See §5, attack 4. One-constant fix. |
| Backward pass using `1−a²` and `y(1−y)` | **old-and-a-liability (of naming, at minimum)** | These are the derivatives of the *true* functions, not of the approximants that ran. See §5, attack 1. |
| Weight init `U(±1/√fan_in)` | **old-and-fine** | Glorot uniform for a `tanh` net. Ours has **one third the variance** of LeCun's or Glorot's prescription (a uniform draw on ±a has σ = a/√3, so σ = 0.577·fan_in^{−1/2} against LeCun's fan_in^{−1/2} — 42% low). It is nevertheless **exactly PyTorch's `nn.Linear` default**, which makes it defensible rather than arbitrary. State the PyTorch identity and move on. |
| ELM gain `2/√n_in` | **flagged in-file as unmeasured — and rescuable** | The same uniform-vs-Gaussian correction gives σ = 1.155·fan_in^{−1/2}, i.e. **within 15% of LeCun 1998 eq. (16)**. That converts a self-declared provenance hole (lines 1424–1434) into a principled derivation from a cited source. Two caveats to state: LeCun derives it for the `1.7159 tanh(2x/3)` sigmoid with unit-variance inputs; iris uses plain Padé `tanh` on inputs of variance ⅓. The transfer is approximate. |
| Targets at 0.1 / 0.9 | **old-and-fine, wrong constant** | LeCun §4.5's principled point is the maximum of the sigmoid's second derivative: σ = (3 ± √3)/6 = **0.2113 / 0.7887**. At y = 0.1, σ′ = 0.09; at y = 0.2113, σ′ = 1/6. **The shipped band delivers 1.85× less gradient at the targets.** It is the *older and more aggressive* rule of thumb, not the conservative one. It also strengthens rather than weakens the saturation objection. Cheap fix; changes every golden hash. |
| Sigmoid output + squared error | **old-and-a-liability, but the head is not the problem** | See §5, attack 3. Keep the head; change the loss. |
| Output clamp at inference only | **a real train/test mismatch** | The hypothesis class evaluated is not the one optimised. Standard practice is to fold the clamp into the forward pass or drop it. Here it is also load-bearing safety (it hides the codomain escape), so it stays — but name it. |
| Stopping on training error | **old-and-a-liability** | Not a 2026-vs-1990 question; it was never a model-selection procedure. See §5, attack 2. |
| SGD with random reshuffling | **old-and-correct** | RR is what everyone does. Correct name: **SGD-RR**. Do not cite with-replacement SGD convergence rates for it (Gürbüzbalaban et al.; Mishchenko et al. 2020 give the RR analysis). At n_ex = 20 and 60,000 epochs no rate from either literature is meaningful anyway — keep the naming correction, drop any implication that a rate is at stake. |
| Normal equations + Cholesky (K = 13) | **old-and-fine-here** | QR of `[H; √λI]` or SVD, which work at κ(H) rather than κ(H)². See §5, attack 6 for why the file's framing of this is wrong in both directions. At K = 13, Cholesky is right. |
| Fixed relative ridge λ₀ | **old-and-fine-here, and deliberately so** | GCV (Golub, Heath & Wahba 1979) or LOOCV. **Both lose here**: selection needs a second pass over examples the streaming Gram deliberately does not retain; LOOCV at N = 20 is high-variance noise; and a data-selected λ makes the fitted weights a *discontinuous function of the example set* — add one demonstration, λ jumps a grid point, every weight moves. That is exactly the retraining-drift failure the file cites Fiebrink & Sonami about and that `iris_correct` exists to prevent. **State fixed λ as a determinism/locality trade, not as a gap.** |
| Unweighted logit-space LS | **old-and-a-minor-liability** | Weight the Gram accumulation by `(t(1−t))²` (one IRLS step), or use a plain linear head. Bounded impact — see §5, attack 7. Two-line fix. |
| 1-NN, brute-force scan | **old-and-correct** | k-d tree / ball tree / LSH. At n ≤ a few hundred, brute force is genuinely faster, and Weka's `LinearNNSearch` is the same thing. |
| Shepard IDW k-NN | **old-and-correct** | Nothing better for k ≤ 8 at this cost. |
| Fisher–Yates, xorshift32 | **old-and-correct** | PCG/xoshiro for statistical quality; irrelevant at this scale. The `% (i+1)` modulo bias is O(n/2³²) — mention only in an RNG audit. |

---

## 4. Why a neural network at all at N = 20

Gaussian process regression and kernel ridge are closed-form, have no epochs, no divergence, no seeds, and are the textbook answer for twenty points. A reviewer *will* ask. Here is the honest set of answers, strongest first.

**(a) You already ship kernel ridge. You ship it at fixed rank.** This is the strongest true answer and the file does not make it. `iris_train_elm_ex` is random-features ridge regression: a randomly drawn `tanh` feature map, a Gram matrix, a Tikhonov penalty, a closed-form solve. That *is* a finite-rank approximation to kernel ridge regression — the Rahimi–Recht construction, with `tanh` features instead of random Fourier ones. The difference from exact KRR is that the rank is fixed at `nh + 1 = 13` rather than growing to N. So the correct framing is not "we chose a neural network over kernel ridge"; it is **"we ship kernel ridge at rank 13, and rank 13 is a memory decision."** Say that and the question largely dissolves.

**(b) The memory contract is sized for `cap`, not for `n_ex` — and that is not negotiable.** The public macro `IRIS_ELM_SCRATCH(NH, NO)` is O(K²), independent of the number of examples. That independence *is* its contract: a caller allocates once, at init, for the worst case. Exact KRR needs an N×N Gram sized for `cap`. At `cap = 4096` that is 67 MB in float32 on a part with roughly 512 KB of internal SRAM. **Concede the contingency honestly:** if you capped the learner at 64 examples, a 64×64 Gram is 16 KB and a 64³ solve is a quarter-megaflop, both perfectly comfortable. So this argument is contingent on the cap being large, and the cap is a design choice. It is real but it is not a proof.

**(c) Prediction cost does not depend on the cap, and this argument does not have an escape hatch.** The MLP forward pass is `nh(n_in + n_out)` multiply-adds — 60 of them — **independent of N**, and runs at control rate on every frame forever. A GP or KRR predictor evaluates N kernel functions per query. At N = 200 that is 200 transcendental evaluations per frame on an S3, growing with every demonstration the musician records. **An instrument that gets slower the more you teach it is a broken instrument.** This is the argument that survives every reframing, and it is the one to lead with in a defence.

**(d) "Closed form" does not mean "numerically safe" in float32.** The intuition that a closed-form solve avoids the messiness of iterative training is wrong at this precision. RBF Gram matrices at N = 20 with any reasonable length-scale are notoriously near-singular; you would need jitter, and quite possibly *more* of it than the ridge here. The file's own experience is the evidence: the unridged 13×13 normal matrix failed Cholesky in every realistic scenario measured. Scaling that matrix to N×N with a smooth kernel does not improve it.

**(e) A GP has hyperparameters, and fitting them reintroduces the exact failure the design exists to prevent.** A GP needs a kernel family, a length-scale and a noise level. Marginal-likelihood optimisation is an inner optimisation loop with its own convergence, determinism and float32 problems — you have not removed training, you have moved it. And a data-fitted length-scale makes the mapping a discontinuous function of the example set, which is the retraining-drift failure again. A *fixed* length-scale avoids that, but is then exactly as arbitrary as `n_hid = 12`, and you have traded one unjustified constant for another.

**(f) Warm correction has no clean closed-form analogue — but state this one carefully.** `iris_correct` keeps the trained weights, zeroes the momentum, and runs a short burst: measured equal fit to a cold 600-epoch retrain with 9× less collateral change and 27× faster. A closed-form solve has no "keep the instrument you practised" mode; it recomputes. **Concede the counter-argument:** for a narrow kernel, adding one point to a KRR fit changes the prediction surface locally too, because the influence decays with the kernel. So the locality difference is smaller than it first looks. What warm-start SGD actually gives you that closed form does not is a **tunable dial** — the epoch budget directly trades correction strength against collateral change, and that dial is measurable and exposed. That is the honest version of this argument.

**(g) Lineage — say it plainly, it is a legitimate reason.** The project's stated claim is Wekinator-compatible semantics on an embedded target. Weka's `MultilayerPerceptron` is the reference implementation, `tests/audit.c` holds the training path against a frozen hash, and "the same instrument the musician learned on desktop Wekinator" is a product requirement, not a modelling one. A GP would be a *different instrument*. Reviewers accept lineage arguments when they are stated as design constraints rather than smuggled in as modelling claims. State it.

**(h) The concession you should volunteer, because it is the strongest thing an examiner could say.** What a GP genuinely buys is a **calibrated predictive variance** — a principled, per-query answer to "how far am I from anything I was taught?" That is precisely what `iris_novelty` hand-rolls in four lines. And it hand-rolls it **wrong**: the scale constant `√n_in · 0.5` (line 769) is applied to distances computed through `iris_norm_in`, which depends on `in_center`, so the same instrument's novelty curve saturates twice as fast on a v3 file as on a v1/v2 file, and the documented semantics ("1 means as far away as the examples are from each other") hold under neither branch exactly. **The file already wants the GP's headline feature and approximates it badly.** Volunteering this converts a weakness into evidence that you understand what you gave up — and the fix is one scaling-aware constant.

---

## 5. What a hostile ML reviewer would attack, ranked

### 1. "Your backward pass is not the gradient of your forward pass."

**This is the most serious finding in the audit, and it is not in the file's own comments.**

The forward pass uses `iris_tanh(x) = x(27 + x²)/(27 + 9x²)`. Its exact derivative is

```
p′(x) = ( (x² − 9) / (3(3 + x²)) )²
```

The backward pass uses `1 − a²` (line 1010) and `y(1 − y)` (line 997) — the derivatives of the **true** `tanh` and logistic. Ratio of the derivative used to the exact derivative of the function that actually ran:

| x | 0 | 0.5 | 1.0 | 1.5 | 2.0 | 2.5 | 3.5 | 4.5 |
|---|---|---|---|---|---|---|---|---|
| used / exact | 1.000 | 0.972 | **0.889** | **0.750** | **0.556** | **0.306** | −0.361 | −1.250 |

`1 − a²` is the correct derivative **only at x = 0**, and under-scales by up to 2× across the ordinary operating range long before it flips sign. The file documents the sign flip (lines 247–261) and treats it as the whole story; it is a subcase. The update is a **surrogate gradient everywhere**: a descent direction for an idealised network, applied to a different one.

Note also that the file's comment conflates two distinct mechanisms. The hidden factor `1 − a²` goes negative at |s| > 3. The **output** factor `y(1 − y)` goes negative at pre-activation **|z| > 6**, since `iris_sigmoid(z) = ½(p(z/2) + 1)`. The output-layer flip inverts the descent direction for a saturated output unit and is the consequential one.

**The answer.** Do not defend this as correct — name it. *"Per-example SGD with classical momentum on a surrogate gradient."* Then give the empirical defence the file has already earned: over 100 seeds, substituting the mathematically exact Padé derivative makes recall and grid RMSE **measurably worse**, and clamping the derivative to ≥ 0 moves them only in the fourth decimal. That is a legitimate, measured, publishable finding — surrogate gradients that outperform exact ones are a known and respectable phenomenon (the straight-through-estimator literature). It stops being a defence the moment it is concealed behind line 989's *"sigmoid'(z) is conveniently y*(1−y)"*, which is false of this implementation and contradicts PART 1 twenty lines up.

### 2. "You stop on training error and call it convergence."

Every 2000 epochs the run halts if the relative decrease over that window is ≤ 10%. That is a **relative function tolerance (`ftol`) termination test** — Nocedal & Wright's `|f_k − f_{k−1}| ≤ ε|f_{k−1}|` generalised to a window — and the `err < 1e-6` floor is its `atol` companion.

It is **not early stopping**. Early stopping (Prechelt 1998) is a *regularisation* technique defined on **held-out** error; every criterion in Prechelt's taxonomy (GL_α, PQ_α, UP_s) reads validation loss. The training error is the quantity being optimised, so it carries no signal about the generalisation gap **by construction** — regardless of whether its trajectory is monotone, which under momentum SGD at effective step 0.67 it is not.

The file claims (lines 848–853) that a 200,000-epoch budget is worse on held-out grid RMSE than 60,000 at 50 examples (0.0065 vs 0.0063), then asserts *"a plateau criterion stops before that on its own."* **The assertion is unsupported and must be marked so.** But so is the premise: the table's 20-example rows state "mean of 9 seeds"; the 50-example figures state no seed count and no variance. A 3% gap in one unreplicated pair of numbers establishes nothing in either direction. Saying that is the stronger objection.

Three implementation details a careful reviewer will also find:
- **A single noisy sample decides.** The test compares two individual epoch errors from a shuffled batch-size-1 trace. The window is long, which lowers the *frequency* of false positives, but not the variance of the *test*. Standard practice compares window means or an EMA, and/or requires **patience** (s consecutive failures — Prechelt's UP_s). Neither is done. One unlucky epoch ends the run.
- **`tr_ref` is initialised to 0**, so the first boundary can only record. Minimum plateau-path run length is 4000 epochs. Undocumented floor.
- **The error floor is what actually fires.** At 5 examples, 36–39 of 40 seeds stop at `err < 1e-6`, not at the plateau. So for small example counts the plateau criterion is largely decorative, and the operative rule is an absolute tolerance on a normalised training MSE in a heavily over-parameterised interpolation regime, where reaching 10⁻⁶ says nothing about fit. Credit where due: the file says this out loud.

**The answer.** Rename it — *"a convergence criterion, not early stopping."* Then be precise about regularisation: truncation of the optimisation path **does** regularise implicitly (Yao/Rosasco/Caponnetto; Raskutti/Wainwright/Yu), and that result attaches to the **60,000-epoch ceiling**, not to the plateau test. What is missing is that the stopping point is never selected against, or even measured against, held-out error.

### 3. "Sigmoid output with squared error is a known error."

Correctly scoped, the standard critique is a *classification* argument: the output gradient carries `σ′(z) = y(1−y)`, which vanishes when a unit is confidently wrong; cross-entropy cancels it exactly (Solla, Levin & Fleisher 1988).

The mitigation the file has — targets at 0.1/0.9 — is **LeCun §4.5 verbatim**, and worst-case attenuation *at the targets* is 0.25/0.09 = 2.8×, not catastrophic. But the file justifies it with LeCun's *weaker* argument (asymptotes are unreachable) rather than his primary one (weights are driven to ever-larger values where σ′ ≈ 0, and *may become stuck*). Right constant, incomplete reason, and — per §3 — the constant is the older, more aggressive one.

The exposure is on the **prediction** side, not the target side: it is `y`, not `t`, that enters σ′. A unit transiently at y ≈ 0.999 against t = 0.1 has σ′ ≈ 10⁻³ and no escape. `IRIS_W_LIMIT` and the measured `IRIS_DIVERGED_STUCK` case (14 good demos + 1 contradictory take → one weight of 60 pinned → training stops after 1 epoch forever) live in exactly this family. The file's own diagnosis of that case is more precise than "the sigmoid did it" and I will not claim otherwise — but the file's answer to the exposure is a **guard**, not a loss function.

**The answer, and it is not the obvious one.** Cross-entropy **is** available for bounded regression: for continuous t ∈ (0,1), `−t log y − (1−t) log(1−y)` is minimised at y = t and cancels σ′ exactly (this is the standard VAE reconstruction loss on [0,1] pixels). So "ours is regression, therefore MSE" is not a complete argument.

**Do not drop to a linear head.** With the sigmoid, the raw network output is algebraically bounded before the clamp, so a diverging weight yields a saturated but in-range synthesis parameter; with a linear head the clamp is the *only* thing between an unbounded pre-activation and an audio parameter on an unsupervised device, and the per-example loss becomes unbounded, so the divergence trap fires *more* often. The train/test clamp mismatch is unchanged either way. **Keep the head, change the loss** — and in this implementation that is a *deletion*: with BCE, `d_out[o] = e` exactly, so line 997 loses `* y * (1.0f - y)`. The competing argument, which must be stated rather than left implicit, is Wekinator fidelity: Weka's `MultilayerPerceptron` minimises squared error, and `tests/audit.c` pins this path to the bit.

### 4. "Your model function is not continuous, and it leaves its stated codomain."

`iris_tanh` returns ±1.0 for |x| > 4.9, but `p(4.9) = 1.02822`. That is a **jump discontinuity of 0.0282 in every hidden unit** at |s| = 4.9, and — since `iris_sigmoid(z) = ½(p(z/2)+1)`, `iris_sigmoid(9.79) = 1.01402` — a **jump of 0.0141 in every output unit** at |z| = 9.8, which is 1.4% of the demonstrated output range. For a library whose headline feature is smooth morphing, that is a step in a synthesis parameter. The file's accuracy comment measures the clamp's error against *true* `tanh`, where the clamp is an improvement, and never notices that it introduces a discontinuity into its own function.

**The answer is a one-constant fix and it is the best change available.** Note that `p(3) = 3·36/108 = 1` **exactly**, and `p′(3) = 0` **exactly**. So moving the clamp from 4.9 to 3.0 simultaneously:

- makes the activation **C¹ continuous** at the join (value and slope both match);
- eliminates the codomain escape entirely (|p| ≤ 1 everywhere);
- eliminates the negative-derivative band in *both* `1 − a²` and `y(1 − y)`;
- **reduces** maximum absolute error against true `tanh` from 0.0283 to ≈ 0.0234 (the residual peak sits near x ≈ 1.5, not at the join);
- costs nothing at runtime.

This is not a trade-off. It is strictly better on every axis except one: **every frozen determinism hash and golden audit vector changes.** That is the real cost, and it is a day of work, not a design decision.

### 5. "N = 20, 75 parameters, and no validation set anywhere."

Substantively true. The generalisation evidence is grid RMSE against a smooth, noiseless truth function — the regime *most* favourable to "more convergence never hurts" and *least* informative about real sensor data, as the file itself concedes. The answer is not to add a validation split at N = 20 (it would be four examples and pure noise) but to report **leave-one-out** error, which at 75 parameters and 20 examples costs 20 refits of a 12-hidden-unit net — milliseconds — and is the statistically appropriate instrument at this N. For the ELM path LOO is nearly free via the hat matrix, though it requires a second pass the streaming Gram does not currently keep.

### 6. "You formed the normal equations in float32 and then blamed the data."

Line 1416 says the closed-form solve has *"no possibility of divergence (the ridged normal matrix is symmetric positive definite BY CONSTRUCTION)."* Lines 1435–1440 and the nine-attempt escalation loop at 1574–1594 exist because that is false in float32. Both statements are in the same comment block, twenty lines apart. **Fix the phrasing:** *SPD in exact arithmetic for any λ > 0; in float32 a sufficient ridge is required, and the escalation loop supplies it (measured: at most 2 doublings across the campaign).* (Also: the loop is `for (att = 0; att <= 8; ++att)` — **nine attempts, eight doublings**. The file's prose is about doublings; do not transcribe "8" onto attempts.)

**But do not over-correct in the other direction.** It is tempting to say "the mandatory ridge is purely an artifact of squaring the condition number; a well-posed problem was destroyed by the normal equations." That is unsupported and probably wrong here. At `n_in = 2`, the 12 hidden features are `tanh(w·x + b)` for random (w, b) over a **two-dimensional** input: twelve smooth monotone ridge functions of a 2-D variable, sampled at 20 points, span an effectively low-dimensional space by construction, and their singular values decay fast. κ(H) is plausibly large *before* any squaring — which is precisely what the file's own first finding says in other words (*"at that scale tanh barely bends — the random features are nearly collinear"*). And statistically: **13 parameters fit to 20 examples per output.** The ridge is required at that ratio whatever the factorisation.

**The defensible claim:** forming `A = HᵀH` costs roughly half the available binary32 digits (κ(HᵀH) = κ(H)²); QR of `[H; √λ I]` or an SVD would work at κ(H). But the file's evidence does not separate that loss from genuine ill-conditioning, and the normal-equations route makes the necessary ridge **larger and the failure louder** — it did not manufacture the need for one. The cost defence stands independently: QR needs O(cap·K) scratch — ~800 KB at cap = 4096, nh = 48 — against ~10 KB now, on a part with ~512 KB SRAM. The streaming Gram is architecturally load-bearing, not a lazy default.

### 7. "Your ELM minimises the wrong objective."

Solving least squares on `z = σ⁻¹(t)` and applying σ at inference minimises error **in logit space**, not output space. The classical form of this trick — Berkson's minimum-logit-χ² — weights each observation by the Jacobian of the transform; unweighted, the estimator is not the least-squares solution of the actual problem and is not the MLE of anything.

**Scope it correctly, because the obvious phrasing overstates it.** Targets are guaranteed to lie in exactly [0.1, 0.9] by `iris_norm_out`, and `iris_artanh` clamps at ±0.98. So `t(1−t) ∈ [0.09, 0.25]` and the reweighting has a **bounded dynamic range of 7.7×** across the entire dataset. Nothing blows up. The deviation is a fixed, mild, systematic tilt over-weighting band-edge targets — real, nameable, and cheap to remove: weight the Gram and `B` accumulation by `(t(1−t))²`, which is one IRLS step, two lines.

The file's framing (*"a bounded-output VARIANT of the backprop head, not an equivalent"*) is honest but does not name the statistical issue. Name it: Berkson; the principled modern alternative is beta regression (Ferrari & Cribari-Neto 2004).

### 8. "You have three different distance metrics and they disagree."

The file claims one normalised input space. It has three:

| Function | Line | Metric |
|---|---|---|
| `iris_novelty` | 764 | `iris_norm_in` — **depends on `in_center`**: [0,1] on v1/v2, [−1,+1] on v3 |
| `iris_knn_predict`, `iris_classify_1nn` | 1953, 2018 | `(row[i] − in[i]) * inv[i]` — always the [0,1] scale, never touches `in_center` |
| `iris_delete_nearest` | 586 | `row[i] − in[i]` — **raw sensor units, no normalisation at all** |

Consequences:
- The k-NN comment (line 1917) claims *"Distances live in the min-max normalised input space — the same space `iris_novelty` uses."* **False for every v3 instrument** — the two are a factor of 2 apart.
- For 1-NN and k-NN the offset cancels in the difference and a uniform factor cannot change an `argmin`, so those paths are *correct*, just misdescribed. `iris_novelty` uses the value as a **magnitude**, so it does not cancel — hence the 2× saturation bug in §4(h).
- `iris_delete_nearest` — "delete whichever example is closest to where you are standing" — is metrically incommensurable with the k-NN it is paired with, and reintroduces exactly the failure the file's own comment says normalisation fixed (*"a millimetre sensor and a g-force sensor count equally"*).

### 9. "Your divergence refusal has a hole in it, and your residual ledger goes stale."

Two live bugs, neither in any comment:

**The bias path.** `iris__check_weights` walks `w1, b1, w2, b2` as one contiguous block (line 806) and clamps all of them. The `IRIS_DIVERGED_STUCK` refusal scan (lines 923–927) checks **only `w1` and `w2`**. A run that diverges through a *bias* leaves `b1` or `b2` pinned at ±16, passes the refusal scan, runs one epoch, re-trips the guard, and stops — forever, silently. That is verbatim the failure the refusal was written to prevent, still live through the bias path. And biases are the *more* likely divergence route: the bias update carries no `x[i]` factor, and |x| ≤ 1 damps weight updates while nothing damps bias updates.

**The residual ledger.** `k->ex_res` is a per-example accumulated squared error (PART 8f), consumed by `iris_worst_example`. `iris_delete_index` (lines 561–572) shifts `ex` and `ex_id` and decrements `n_ex` but **does not shift `ex_res`**. After any deletion, every stored residual is attributed to the wrong example until the next non-resume run clears the array. "Which demonstration is fighting the others?" answers with a different demonstration.

Also worth correcting: §2's tempting claim that the weight clamp is *"a divergence trap, not a regulariser — it fires and stops rather than shaping the solution."* It does shape the solution, destructively: line 811 writes `w[i] = IRIS_W_LIMIT` **before** stopping, and that pinned weight is the entire subject of the `DIVERGED_STUCK` apparatus.

### 10. "One citation in 2050 lines."

Two, actually — but see §2. This is the cheapest attack to defuse and the most embarrassing to leave standing.

### 11. Minor, and to be *demoted* rather than defended

**The ridge penalises the intercept.** Line 1577 adds `lam` to all K diagonal entries including the bias column, and textbook ridge does not shrink the intercept. But the bias column is `h[NH_] = 1.0` for every example, so its diagonal entry is exactly `n_ex` — the largest in A — while λ = λ₀·tr(A)/K is order 10⁻³ at the shipped λ₀ = 1e-4, nh = 12, n_ex = 20. Relative shrinkage of the intercept is **order 10⁻⁴**, rising to a percent or so after eight doublings. It is a real convention violation with no measurable consequence at any λ the code can reach. Fix it in one line; do not list it among things that "must be stated."

**`IRIS_FLUSH` is not part of any standard SGD.** It flushes velocity components below 10⁻³⁰ to exactly zero, for ESP32-S3 denormal parity. Legitimate, but it means the update rule is `v ← flush(μv − ηg)`, and any bit-exactness claim is a claim about *that* rule.

**`last_error` is not the objective, and the two paths measure it differently.** The gradient implies a per-example objective of **half** the summed squared error (Rumelhart's `E_p`; the ½ cancels the 2 from differentiation). Line 1035 divides by `n_ex * NO` and carries **no ½**, so the reported figure is `(2/(N·M)) × Σ_n E_n`. Harmless — a constant only rescales η — but you cannot write "the network minimises the MSE reported by `iris_train_epochs`." Worse, backprop's `err` is a **running** epoch total accumulated along the descent trajectory, while the ELM's (lines 1645–1660) is a clean forward-only pass at fixed final weights. The comment claims they are "in the same units" — true — but the *protocols* differ, systematically against backprop while the loss is still falling. Do not compare the two numbers directly.

---

## 6. What to actually change, ranked by value

| # | Change | Value | Cost |
|---|---|---|---|
| **1** | **`iris_tanh`: move the clamp from 4.9 to 3.0.** `p(3) = 1` and `p′(3) = 0` exactly, so this makes both activations C¹, removes the codomain escape, removes the negative-derivative band in `1−a²` *and* `y(1−y)`, and **lowers** max error vs true `tanh` from 0.0283 to ≈0.0234. Strictly better on every axis. | Removes a real discontinuity in a synthesis parameter and closes attack 4 and half of attack 1 with one constant. | One constant. **But every frozen determinism hash and golden audit vector changes** — re-freeze and re-run the 100-seed comparison. ~1 day. |
| **2** | **Add `b1`/`b2` to the `DIVERGED_STUCK` refusal scan** (lines 923–927). | Closes a live, silent, permanent-failure path — the exact bug the refusal exists to prevent. | 4 lines. No behaviour change on healthy runs. |
| **3** | **Shift `ex_res` in `iris_delete_index`.** | `iris_worst_example` currently returns the wrong example after any deletion. | 1 line. |
| **4** | **Unify the three distance metrics on `iris_norm_in`** (`iris_delete_nearest`, `iris_knn_predict`, `iris_classify_1nn`), and make `iris_novelty`'s `0.5` constant scaling-aware. | Removes attack 8 and the v1/v2-vs-v3 novelty inconsistency; makes the file's own comment at 1917 true. | ~6 lines. Changes k-NN behaviour only where ranges differ per-dimension in the offset — verify against the Weka-parity vectors. Novelty change is a documented behaviour change. |
| **5** | **Rename the stopping rule and report a held-out or LOO number.** "Windowed relative-improvement convergence criterion." Add leave-one-out RMSE to the training report; add patience (require 2 consecutive failed windows) and compare window *means* rather than single epochs. | Closes attack 2, which is the second-most-damaging finding and the one a supervisor will ask about first. | LOO: 20 refits, milliseconds, ~30 lines. Patience: ~5 lines and one new hash. Renaming: free. |
| **6** | **Add the citations, and rewrite line 775.** Linnainmaa 1970 (via Griewank 2012), Polyak 1964 + Qian 1999, Huang 2006 + Schmidt 1992 + Rahimi & Recht, Hoerl & Kennard 1970, Nocedal & Wright Alg. 3.3, Cover & Hart 1967, Shepard 1968, Fisher–Yates, Marsaglia 2003. Add LeCun §4.4's endorsement of the rational approximation. | Removes the cheapest attack. For a PhD artifact this is not optional. | An afternoon. Zero code risk. |
| **7** | **Name the surrogate gradient in the comments, and state the empirical defence as a defence.** Replace line 989's *"sigmoid'(z) is conveniently y*(1−y)"* with the truth, and put the 100-seed measurement next to it. Fix line 1416's SPD claim. Fix the "at most 8 times" / nine-attempts wording. | Converts three internal contradictions into three defensible findings. | An afternoon. Zero code risk. Do this even if you never change a line of code. |
| **8** | **Ship continuous-target cross-entropy behind a flag.** `d_out[o] = e;` — delete `* y * (1.0f - y)`. | Cancels σ′ exactly, is minimised at y = t, and removes the confidently-wrong dead zone while *keeping* the sigmoid head's algebraic range safety. | 1 line + a flag. Breaks Weka parity, so it must be opt-in and the parity path must remain the default. |
| **9** | **Weight the ELM Gram accumulation by `(t(1−t))²`.** | Makes the logit-space solve the correctly-weighted (Berkson/IRLS) estimator rather than the biased unweighted one. Bounded 7.7× effect, so expect a small improvement, not a large one. | 2 lines. New golden vectors for the ELM path. |
| **10** | **Move the target band from 0.1/0.9 to 0.2113/0.7887.** | 1.85× more gradient at the targets; replaces a folklore constant with LeCun §4.5's derived one. | 2 constants — and every hash, every saved file's implied output mapping, and the Weka comparison. **High cost for a modest gain. Do it only if you are already re-freezing hashes for change 1.** |
| **11** | **Do not penalise the intercept in the ELM ridge** (line 1577, skip `i == NH_`). | Restores a near-universal convention. Effect is order 10⁻⁴ and unmeasurable. | 1 line. Listed for completeness; it is a footnote, not a finding. |

**If you do only three things:** #1, #2, and #6. The first fixes the mathematics, the second fixes a bug that silently bricks a musician's instrument, and the third removes the objection that costs nothing to raise and everything to leave standing.