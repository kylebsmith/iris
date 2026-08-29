# Which trainer iris recommends

> **Historical record.** Written against the pre-rename tree, where the
> header lived at `firmware/library/iris.h`. It is now `iris.h` at the repo
> root. Line numbers and counts describe the file on this record's date.

> ## ⚠️ PROVISIONAL — DO NOT TREAT AS SETTLED
>
> **Status: provisional, 2026-08-26.** The recommendation below (`iris_train_converge`,
> i.e. train until the error plateaus) is derived entirely from measurements on
> **smooth, noiseless, synthetic target functions.** A fair re-run with additive
> noise, reported in `research/prior-art/WHY-lbfgs-defaults-robustness.md` §1,
> found that training to a plateau **degrades badly once the data is noisy**,
> while a plain fixed epoch ceiling — the thing this ADR argues against — is the
> most robust arm:
>
> | held-out error, 20 examples | clean | σ=0.02 | σ=0.05 | σ=0.10 |
> |---|---|---|---|---|
> | SGD, 100-epoch ceiling | 0.01755 | 0.01899 | **0.02845** | **0.05009** |
> | SGD to plateau (**this ADR's recommendation**) | **0.00996** | 0.02760 | 0.08513 | **0.15002** |
>
> Real sensors are noisy and real performers are inconsistent, so the regime this
> ADR was measured in is not the regime it will be used in. **The blocking
> experiment is §7.4: one recorded human gesture set.** Until that runs, treat
> §1 as a hypothesis, not a decision, and do not propagate the recommendation
> into the README or the header usage block as settled.
>
> Also superseded within this document: §6's disposition of `iris_train_lbfgs`
> assumed the trainer was unrepaired. Both defects were repaired on 2026-08-26
> and the gap it cites (3.8x) is now 1.61x on that seed.


**Decision document — 2026-08-26.** All line numbers verified against `iris.h` as it stands today (2,087 lines; note several repo documents say 2,204 — that figure is stale).

---

## 1. The decision, in one paragraph

**The library recommends one trainer: `iris_train_converge`** (`iris.h:943-952`). It is ordinary backpropagation — the thing that has been in this library since v0.1 — with one change: instead of you guessing how long to train for, it keeps going until the error stops getting better, then stops. In plain terms: you demonstrate a few gestures, and it practises them over and over until practising stops helping. You don't pick a number of repetitions; there is no number to get wrong. It is the same code as the old fixed-length trainer — literally the same function underneath (`iris.h:762-909`, and the header says so at `:755-757`: "there is no second copy of the update rule to drift out of sync") — plus twelve extra lines that ask, every 2,000 repetitions, "did the last 2,000 buy me at least 10% of my remaining error?" (`iris.h:896-903`). It is chosen because it is the only option that is simultaneously (a) a real neural network learning, which is the point of the library, (b) safe by measurement, and (c) already what the shipping instrument runs. **But this recommendation is conditional on three small fixes landing first** — the function currently has a hidden fourth stopping rule that makes it stop early without telling you, it silently marks a network "trained" if you pass it zero repetitions, and its published speed figures for the ESP32-S3 are wrong by roughly a factor of eight. Details in §7 and §8. Until those land, the library should not recommend anything with a straight face.

---

## 2. What each of the four actually is

### `iris_train_epochs` — practise a fixed number of times
*(`iris.h:935-939`, engine at `:762-909`)*

You tell it "practise 600 times" and it practises exactly 600 times, then stops — whether it has finished learning or not. Like a music student who practises a piece for exactly one hour every day regardless of whether they've got it. The whole algorithm is five sentences: shuffle your demonstrations into a random order, take one, run it through the network, compare what came out to what you demonstrated, nudge every internal number a little in whichever direction would have narrowed that gap, repeat.

**Good at:** being understandable, and being predictable. Its worst-case time is arithmetic you can do on paper before you call it — 600 repetitions × your number of examples × a fixed amount of work each. Measured dead-linear across a 32× range in example count (`benchmark/results/iris.json:28999-29029`: 0.0626 ms per example, 0.0591 ms fixed cost) and dead-linear on the actual board too (`hardware/board/BRINGUP-LOG.md:207-209`: 16.03 ms per example per 600 repetitions).

**Bad at:** stopping at the right time. 600 repetitions is measurably nowhere near finished — the same code run to its plateau recalls your own demonstrations 5.9× more accurately (`iris.h:697-703`). But more is not simply better: at 50 examples a fixed 200,000 repetitions generalises *worse* than 60,000 (0.0065 vs 0.0063 held-out error, `iris.h:717-719`). No single constant is right for every example count, which is precisely why this trainer was demoted (`docs/adr/0017-train-to-the-plateau-not-to-a-constant.md`).

### `iris_train_converge` — practise until practising stops helping
*(`iris.h:943-952`, constants at `:748-750`)*

Identical to the above, except the stopping rule. Every 2,000 repetitions it checks whether the last 2,000 improved things by at least 10%. If yes, keep going. If no, the curve has flattened — stop. A hard ceiling of 60,000 repetitions means it can never run away. The student who practises until the piece stops improving, with a rule that they go home at midnight regardless.

**Good at:** the actual fit. It reaches an error floor that neither L-BFGS nor Levenberg-Marquardt can reach — 5.075e-06 against L-BFGS's 1.916e-05, a 3.8× gap, and this is pinned as a live test assertion, not a claim in a document (`tests/audit.c:1494-1508`, check 33). It removes the one hyperparameter a musician would otherwise have to guess.

**Bad at:** two things. It is slow — 26.1 ms at 20 examples on a laptop (`README.md:177`), which on the actual board is roughly seven seconds (derived; see §3 — **unmeasured on device**). And at very small example counts it is not really doing what its name says: measured in this review across 40 seeds, at 5 examples 36-39 of 40 runs stopped on an undocumented error floor rather than on the plateau test, and 22 of 40 finished before repetition 4,000, which is the earliest the plateau test can structurally fire (`iris.h:795`, `:898-902`).

### `iris_train_lbfgs` — a clever mathematician who remembers her last five steps
*(`iris.h:1456-1458`, driver at `:1319-1451`)*

Instead of feeling which way is downhill and taking one small step, it remembers the last five steps and how the slope changed across each of them, builds a cheap guess at the *shape* of the valley, and tries to jump straight to the bottom. Then the safety rule, which is the best-taught idea in the file: before jumping it photocopies its position; if the landing is worse (or produces nonsense), it throws the landing away, restores the photocopy exactly, halves the jump distance and tries again, up to 20 times (`iris.h:1413-1427`).

**Good at:** never making things worse. Because a step is only ever kept if it lowers the error, the error can only ever go down. This is verified rather than asserted — 299 steps on deliberately hostile data, every one an improvement on the last, zero nonsense values (`tests/audit.c:826-857`, check 18). It also has no learning-rate knob at all, so there is no setting a student can get wrong.

**Bad at:** finishing. It finds the nearest low point, correctly stops there, and cannot get out. 1,000 repetitions and 50,000 repetitions return **bit-for-bit identical** weights — 50× the work buys literally nothing (`README.md:202-203`). And there is no convergence test in the code: measured in this review, at 20 examples it reaches a frozen answer at step 691 and then spends the remaining 19,309 steps of a 20,000-step budget re-deriving it. That contradicts `docs/frontier/REPORT.md:116`, which claims "L-BFGS: gradient/step tolerance" — no such tolerance exists in the shipped source.

### `iris_train_elm` — roll dice for half the network, solve the other half exactly
*(`iris.h:1557-1708`, wrappers at `:1712`, `:1720`)*

The network has two halves: one turns your gesture into twelve internal "feature" numbers, the other mixes those into sound parameters. This trainer stops training the first half entirely — it rolls dice once, from the instrument's seed, and freezes them. Then the only unknowns enter the answer in a straight line, so you don't search for them, you *solve* for them, the way you fit a straight line through points, just in thirteen dimensions instead of two. One calculation, one right answer, no repetitions, nothing to overshoot.

**Good at:** speed and safety. 0.0029 ms at 20 examples on a laptop (`README.md:165`), 250-393× faster than 600 repetitions of backprop (audit cost table, run today). It cannot diverge because there is nothing to iterate. Ninety deliberately hostile solves — duplicates, conflicting examples, a dead sensor channel, a 1,000,000 outlier — produced zero unfixable failures and zero nonsense values (`tests/audit.c:979-992`, check 21).

**Bad at:** being a neural network. The interesting half never learns; what's left is ridge regression on random features, and the project's own decision record (`admin/DECISIONS.md`, D11) says a reviewer would be right to call it that. Also, at the width the device actually ships (12 hidden units) it is *worse* than 600-repetition backprop on both recall and held-out error at N=20 and N=50 (`docs/adr/0008:15`, `REPORT.md:87-88`); it only wins at 48 hidden units, which costs 10,388 B of scratch memory against 884 B (`docs/adr/0008:38`).

---

## 3. The comparison table

All host figures: Apple M4 Max, Apple clang 17.0.0, `-O2 -ffp-contract=off`. **"est."** marks a scaled estimate, never a stopwatch reading. **"unmeasured"** means exactly that.

| | `iris_train_epochs` (600) | `iris_train_converge` | `iris_train_lbfgs` | `iris_train_elm` |
|---|---|---|---|---|
| **Never fails** | Safe **by guard, not by construction.** Refuses bad examples before touching anything, weights and ranges bit-preserved (`iris.h:766-782`; checks 13a, 28). Clamps runaway weights at 16.0, reports `IRIS_TRAINING_DIVERGED` (`:677-687`, `:177`); measured healthy peak is 2.8, so 5.7× headroom, and guards are proven byte-inert on healthy runs (`build.sh` two-build compare). Hostile sweep: 0 NaN reaching predict over 961 probes per cell (check 13c). **But** it can diverge without the guard — measured at momentum 0.85/lr 5.0 and momentum 0.99/lr 1.0. **Two silent failures live today:** the `err < 1e-6f` stop at `:893` fires with status still OK, and `epochs <= 0` sets `trained = 1` with error 0.0 on an untrained network (`:788`, `:798`, `:906-908` — verified in source today). | Same guards, same engine, same two silent failures. **Additionally:** it is the only trainer that enters the library's own known algebraic defect — the fast tanh approximation exceeds its proper range for 3 < \|x\| ≤ 4.9, flipping a gradient sign; never entered on the fixed-epoch path (0 of 192,000), entered on **40 of 40 seeds** here at 2.5-4.2% of evaluations (`CORE-AUDIT:160-161`). Measured cost: nil. | **Cannot diverge, by construction** (`:1419` — a step is accepted only if the error fell and is not NaN; otherwise the weights are restored bit-exactly at `:1423`). Verified over 299 hostile steps (check 18). **But:** two of its three refusal paths return `-1.0` without setting a status code (`:1328`, `:1331-1333`), a direct violation of ADR 0004 — and no test exercises any refusal path. | **Cannot diverge** — no iteration. Solve is positive-definite by construction; the pivot test at `:1644` catches NaN too. 90 hostile solves, 0 failures (check 21). **But:** all four refusals return `-1` leaving status at OK (`:1559-1562`), which has already cost a live debugging session on hardware (`firmware/app/core/app.h:69-80`). Cannot raise `IRIS_TRAINING_DIVERGED` at all. |
| **Bounded time** | **Tightest.** Pure counter, `for (ep = 0; ep < epochs; ++ep)` (`:798`). Every exit path can only shorten a run. Measured worst/best across 7 data scenarios × 16 seeds: **1.1× within scenario.** | **Bounded by its ceiling, not its typical case.** 60,000 repetitions (`:750`). Measured epochs actually spent: 8,000-20,000 across 10-200 examples (`README.md:176-180`, `REPORT.md:302-306`), and in this review's 40-seed probe, **5,900-23,900 at 20 examples (4.0× spread)**. Cannot stop before repetition 4,000 — undocumented anywhere in prose. | **Loosest.** Each step costs 1 to 22 full passes over the data (`:1415-1428`). Measured within-scenario spread at a fixed 100-step cap on 50 examples: **8.1×** (0.360-2.923 ms), and the worst case *exceeds* the 600-repetition baseline it is nominally 4-5× faster than. No convergence test, so a large budget is spent in full. | **Fixed instruction count.** One pass over examples, at most 9 solve attempts (`:1634`). Measured worst/best across 7 scenarios × 16 seeds: **1.83×**, with zero escalations across all 112 solves. |
| **Deterministic** | **Strongest contract in the library.** Golden hash frozen across builds and dates: `0xFEFAEDF6` and `0x6805FB0D` (check 12, `audit.c:489-551`). Passes 16 of 20 optimisation/contraction flag combinations; the 4 failures are exactly `-ffp-contract=fast` (`CORE-AUDIT:185`). **But** it does not reset the RNG, so "same seed, same instrument" is true of `iris_retrain_new` and *false* here: 1×600 vs 3×200 repetitions gives max\|Δw\| 0.00990; 1×600 vs 300×2 gives 0.0600. The chunk schedule is part of the instrument's identity. | Same-binary sliced-vs-unsliced check passes by full memory compare (check 31, `audit.c:1401-1430`). **No golden hash exists for this trainer** — check 12 and `guards_ab.c:48` both exercise the fixed-epoch path. Also does not reseed; both production callers reseed first (`:2033`, `firmware/app/core/surface.c:287`). | **Cleanest.** Consumes zero randomness after seeding. Pinned twice: full memory compare (check 17) *and* a frozen hash `0x8260169D` / `0xD9AF8648` (check 12). | **Structurally clean** — draws its frozen layer from a *local* generator so the instrument's correction history is never touched (`:1583`). Proven memory-identical from different starting seeds at three widths (check 20). **But no golden hash.** Probe in this review: blob identical at `-O0/-O1/-O2/-O3`, contract off *and* on; differs under `-ffp-contract=fast`. |
| **Accuracy at N ≤ 20** *(held-out error on unseen points, 2-12-3, seed 4242, `README.md:176-177`)* | N=10: **0.0299**. N=20: **0.0124**. Recall of your own demos at N=20: 0.0066 (check 31). | N=10: **0.0302** — *worse than 600 repetitions*, and `README.md:182-188` flags this unprompted as "the first crack". N=20: **0.0100**, recall 0.0014. On a target with real local structure at N=20, going to convergence is a **1.93× regression** in generalisation and fails the project's own shipped reroll test by 2.7× (`research/notes/training-and-editing-design.md:44-69`). At N=5 convergence is a loss on both test targets (`:74-77`). | Measured in this review (2-12-3, median of 11 seeds): N=10 **0.0301**, N=20 **0.0105**, N=5 0.0575. Recall at N=5 is essentially exact (1.3e-09 training error). Floor reached by step ~691 and never improves. | nh=12: N=10 **0.0333**, N=20 **0.0142** (measured this review) — worse than backprop on both. nh=48: N=20 **0.0110**, recall 0.0025. Recall below N≈15 is far better than backprop (0.0001 at N=5) because 13 free parameters interpolate 5 points almost exactly. |
| **Speed** | Host, 20 ex: **0.9 ms** (audit cost table, run today; `README.md:165` says 1.0 ms median, `ADR 0007:16` says 0.903 ms, `REPORT.md:87` says 1.223 ms — same shape, unexplained). **ESP32-S3, MEASURED: 321.0 ms at 20 examples, 3,204.6 ms at 200** (`BRINGUP-LOG.md:198-204`) — the only on-device training measurement in the repository. | Host, 20 ex: **26.1 ms** (`README.md:177`) or **27 ms** (`ADR 0017:65`) or **34 ms** (`REPORT.md:317`, 8-output shape) or **41.3 ms** (`README.md:199`, 11 seeds). **ESP32-S3: unmeasured.** Every S3 figure in the repo is host × 32. At the *measured* ratio of ~270× (`BRINGUP-LOG.md:207-209`), 26.1 ms is **≈7 s** (derived, not measured), and the 60,000 ceiling is **≈40 s** (derived) — not `iris.h:750`'s "~4.8 s". | Host, 20 ex at 100 steps: **0.18 ms**. At the public entry's full 20,000-step budget: **236.8 ms** (measured this review). **ESP32-S3: unmeasured**, and `ADR 0007:47-49` gated its promotion on two board measurements that have never been taken. | Host, 20 ex: **0.0029 ms** (`README.md:165`). **ESP32-S3: unmeasured.** The repo publishes "~0.2 ms" (`iris.h:1464`) and "~1 ms measured" (`RELEASE-v1.0.md:48`) — **both are the same host measurement under two different scalings**; `BRINGUP-LOG.md:217` calls the second one a scaling, not a reading. |
| **Lines of code** | **5** lines of function (`:935-939`), on a shared 113-line engine (`:762-909`) used by four public entry points. | **10** lines of function (`:943-952`), plus **12** lines of plateau logic inside the shared engine, plus **32** lines for the sliced twin (`:959-998`). Same 113-line engine. | **175** code lines total across `:1197-1458` — a 111-line driver, a 45-line gradient kernel, 6 macro lines. Needs a caller-supplied scratch buffer: 4,248 B at the audit shape, **7,888 B at the shipped shape**, 176,560 B at maximum topology. | **147** code lines across `:1525-1725`, of which 71 are the numerical core. Scratch: 884 B at 12 hidden units, **10,388 B at 48**. |
| **How hard to explain** | **Easiest.** Three sentences and a counter. Derivatives reuse numbers already on the stack (`:823`, `:836`), verifiable in two lines of school calculus. **Caveats a student will catch you on:** the hidden `err < 1e-6f` stop, the fast-tanh approximation leaving its proper range, and `epochs <= 0` lying about being trained. ~25 minutes of honest footnotes. | **Easiest + one sentence.** "Every 2,000 repetitions, did the last 2,000 buy 10%? If not, stop." Same footnotes as above, plus: the invisible 4,000-repetition floor, and the fact that the plateau test is often *not* what stopped it. **The hard part is the justification** — why a criterion beats a bigger number rests on a 0.0002 difference in held-out error at 50 examples, which will not land with anyone who does not already believe in overfitting. | **Best story, worst mathematics.** The safety rule is one sentence and yields a guarantee a student can state and verify. The two-loop recursion (`:1369-1390`) is 22 lines of ring-buffer index arithmetic whose correctness needs the Sherman-Morrison-Woodbury identity — graduate material. The scaling factor `sy/yy` and the constant `c1 = 1e-4` are both folklore a beginner must simply accept. | **Ninety seconds to state, one lecture to teach.** "Roll dice for half, solve the other half" is genuinely intuitive. But an honest treatment needs: random feature maps (the header itself declines to justify these at `:1471-1473`), why the dice must be rolled at gain 2/√n and not the usual 1/√n, normal equations, why forming them squares the conditioning problem, ridge regularisation and why zero is not a valid setting, Cholesky factorisation with two back-substitutions, logit space, and Newton's method on the inverse of a Padé approximant. |

---

## 4. How this compares to the rest of the field

**Honest summary: the choice is recognisable, but it is not what the small-data machine-learning mainstream would pick — and we have measured evidence for why we depart.**

### The music lineage all does the same thing, and none of them says why

- **Wekinator** builds `new MultilayerPerceptron()` and never calls `setLearningRate`, `setMomentum` or `setTrainingTime` (`wekinator/src/wekimini/learning/NeuralNetModelBuilder.java:50-51`; grep across all 192 Java files returns zero hits for those setters). It therefore silently ships Weka's command-line defaults: learning rate 0.3, momentum 0.2, 500 repetitions. No comment in the learning package explains any of it.
- **RapidLib** — the C++/JavaScript successor for the same community — hardcodes **exactly 0.3, 0.2, 500** (`RapidLib/src/neuralNetwork.h:138-140`).
- **InteractML** links RapidLib as a compiled plugin and exposes no hyperparameters at all (`iml-unity/Assets/InteractML/Scripts/RapidLib/RapidlibLinkerDLL.cs:16-38`).

One set of general-purpose 1990s Weka defaults propagates untouched through three generations of interactive-ML music tooling, never restated and never chosen for the 5-50-example datasets these tools actually elicit. **Choosing a plateau criterion instead of a constant is the one place this library breaks that chain**, and that is defensible in review.

It also matters that these libraries fail in ways this one does not. RapidLib returns **NaN for a constant input dimension** — any sensor channel the musician didn't move, or one that came unplugged — because a guard commented "Prevent divide by zero later" iterates by value over a copy (`neuralNetwork.cpp:490-493`) and line 417 then computes 0/0, with nothing downstream to catch it. `memlp` seeds its shuffle from `std::random_device` (`MLP.cpp:39`), so training is not reproducible at all, and its `CheckAndFixWeights()` exists in five headers and is called from none. **Every one of those is a missing guard, not a wrong optimiser choice.**

### The general ML mainstream would pick something else

This is the part to be honest about. **scikit-learn's own documentation recommends L-BFGS for exactly our situation.** Verbatim from `MLPRegressor` (`_multilayer_perceptron.py:1439-1443`): *"For small datasets, however, 'lbfgs' can converge faster and perform better."* Its user guide is stronger: *"Empirically, we observed that L-BFGS converges faster and with better solutions on small datasets."* MATLAB's tabular neural-net entry point `fitrnet` ships L-BFGS as its **only** solver. Le et al. (ICML 2011) state it directly for our case: *"if we have a small dataset, it is better to use a batch method... because we do not have to tweak optimization parameters."* Bottou & Bousquet (NIPS 2007) supply the reason: at small scale computing time is not the constraint, so the optimisation error can be driven to nothing and generalisation is then determined purely by the statistics — the noise that makes stochastic methods worth using buys nothing when the "mini-batch" *is* the entire dataset.

**So why are we not choosing L-BFGS?** Because this library measured it and it lost, on the thing that matters. Audit check 33 is a live assertion, not a document: L-BFGS floors at 1.916e-05 while the same network carried to its plateau reaches 5.075e-06 — 3.8× lower — and ten times more L-BFGS steps move that by 1.00× (`tests/audit.c:1494-1508`). `REPORT.md:329` diagnoses it correctly: the per-example noise keeps escaping shallow minima, which is precisely the property a second-order method is designed to throw away. On a 75-parameter non-convex surface with 20 examples, "the nearest low point" is not the one you want.

That is a **publishable negative result**, and it is the honest justification for the departure: *the field's standard small-data advice was tested here and did not hold; we measured rather than inherited.* Two caveats must travel with that claim, though. First, scikit-learn and MATLAB recommend L-BFGS **with a ridge penalty** (`alpha`, `Lambda`); our L-BFGS has no penalty of any kind, so we tested half the recipe. Second, our L-BFGS was never given a convergence test, so part of its poor showing is an implementation gap, not a property of the method.

### Where others' defaults would be actively wrong here

- **GRT** defaults a 20% validation split **on** (`MLP.cpp:39-47`). At 10 demonstrations that silently throws away 2 of them and validates on those 2.
- **FluCoMa** defaults batch size 50 and validation 0.2 (`MLPRegressorClient.hpp:36-40`) — sized for corpora of thousands of analysed audio slices, not for a performer tapping "record example" fifteen times.
- Goodfellow et al. name the reason early stopping on a validation set is wrong at our scale: *"Early stopping requires a validation set, which means some training data is not fed to the model."* Cawley & Talbot (JMLR 2010) put it in one sentence for exactly our case: *"over-fitting in model selection is likely to be most severe when the sample of data is small and the number of hyper-parameters to be tuned is relatively large."*

`iris_train_converge` needs no held-out data and tunes nothing at the call site. **That is mainstream-defensible reasoning, arrived at from a different direction than the mainstream's own recommendation.**

---

## 5. The robustness-versus-teaching tension, resolved

**The tension is real. It is also not between the two options people assume.**

The genuinely robust choice is `iris_train_elm`: 1.83× worst-case time spread, a solve that cannot fail, no step size a student can set wrong, ninety hostile solves with zero failures. The genuinely educational choice is backpropagation. Those really do pull apart, and the honest statement of the cost is: **a student who trains an ELM never watches learning happen.** The network arrives fully formed and the interesting question is hidden inside a Cholesky factorisation. That is a real educational loss for a real engineering gain, and pretending otherwise would be dishonest.

**But `iris_train_lbfgs` is on neither horn.** It is less predictable in time than the closed form (8.1× spread vs 1.83×), harder to teach than backprop (Sherman-Morrison-Woodbury), *and* worse than backprop on the error floor (3.8×). It occupies the dominated corner. That removes one option from the tension entirely.

**And the tension does not arise between the two remaining backprop entry points.** `iris_train_converge` and `iris_train_epochs` are the same 45 lines of arithmetic on the same slide. The plateau test is eight additional lines. The marginal teaching cost of the recommended default over the most-teachable one is **one paragraph, not one lecture.**

### The resolution: separate "the function you read first" from "the function you call first"

The library does not have to choose. It has to stop conflating two different questions, which it currently does by having a single "recommended trainer" slot.

**Recommend `iris_train_converge`. Teach from `iris_train_epochs`. Say both, in the header, in two adjacent sentences.**

This costs nothing, because `iris_train_epochs` cannot be removed anyway (§6) and is already the reference the whole test suite is built on. It gains the thing that actually matters: a reader who wants to understand backpropagation reads a five-line function and a counter; a reader who wants a working instrument calls one function with zero tuning parameters; and neither of them is told a half-truth.

**What does *not* resolve the tension, and should be said plainly:** the reason the guarded backprop path is acceptable at all is the guards, not the optimiser. The refusal gate, the range floor, the weight clamp, the output NaN substitute. Those are ~40 lines of scaffolding, they are proven byte-inert on healthy runs, and every failure catalogued in the competing libraries above is a *missing* one of them. **The choice of algorithm determines how much scaffolding you need, not whether you need it.** That is the more valuable finding than the trainer choice itself, and it belongs in the ADR.

---

## 6. What happens to the other three

The PI's constraint is that optional extras must not ship before the core is validated. Applying it strictly:

### `iris_train_epochs` — **KEPT, reframed, not removed.** No lines change.

It is 5 lines of function on a shared engine (`:935-939`). It cannot be deleted without breaking the validation apparatus the core depends on:

- The frozen golden instrument files that checks 11, 12, 29 and 30 verify were produced by it (`tests/golden/make_golden.c:112`).
- The guards-are-inert proof uses it (`tests/guards_ab.c:48`).
- **Every published benchmark number in the repository** — six tasks, 32 replicates each, recall, generalisation, determinism, reroll spread — is measured through it (`benchmark/adapters/iris_adapter.c:114`).
- The **only on-device timing measurement** used it (`firmware/latency-rig/latency_rig.ino:176`).

Deleting it invalidates the fixture set. Its new documented role: **the reference implementation, and the one you read first.** Three documents already call it permanent (`iris.h:743-746`, `ADR 0007:45-50`, `ADR 0017:36-38`); that standing deserves a stated role rather than a footnote.

### `iris_train_elm` — **KEPT, and it is not an optional extra.** 147 code lines (`:1525-1725`).

It is not a trainer choice offered to library users. It is what the shipping instrument runs on every single demonstration, and in three fallback paths besides:

- `firmware/app/core/surface.c:204-205` — the per-demonstration retrain, inside the hot half's 5 ms model lock.
- `:289` — the fallback when the slow trainer refuses to start.
- `:314` — the restore when a finger on the glass abandons a long training run.
- `:358` — the restore when the divergence guard stops one.

At 0.0029 ms host it fits inside one audio buffer; the slow path, at ~7 s on the board (derived), does not. **Documenting it as a library-level trainer option is the mistake; documenting it as "the instant fit that plays while you decide whether to train properly" is correct.** It stays in the core, described as an implementation tier rather than a choice.

### `iris_train_lbfgs` — **MOVED OUT of the core header.** 262 lines removed (`:1197-1458`), of which 175 are code.

This is the one that meets the PI's description of an extra shipping before the core is validated:

- **Zero production callers.** Nine call sites in the entire tree, all in `tests/audit.c`.
- The header's own usage block (`iris.h:20-40`) does not name it.
- The shipping firmware does not reference it anywhere.
- `CORE-AUDIT:228` states it: *"It is also not on any shipping path, so the non-divergence guarantee protects nothing today."*
- Its promotion to the device was gated on two ESP32-S3 measurements (`ADR 0007:47-49`) that have never been taken.

**Move to `firmware/library/experimental/iris_lbfgs.h`**, included by the audit only. That takes `iris.h` from 2,087 to **1,825 lines, a 12.5% reduction**, and removes the caller-supplied scratch buffer — the only failure surface in the library where getting a size wrong turns every training call into a silent `-1` (which has already cost one live hardware debugging session, `firmware/app/core/app.h:69-80`).

**Keep audit check 33.** It is the single most valuable thing L-BFGS gives this project: a measured, reproducible demonstration that the method the small-data literature recommends stalls 3.8× above our floor. That result is more publishable than the trainer.

### `iris_knn_predict` — not in scope, but worth one line

Zero training, zero extra bytes, exact recall of every demonstration, cannot produce a value outside what the musician demonstrated (checks 23-26). It is the tier that plays when everything else refuses. It is not a trainer and does not compete here.

---

## 7. What must be measured before this decision is final

Listed in order of how much they could change the answer.

### 7.1 On-device wall clock for `iris_train_converge`. **Could change the answer.**

**Currently unmeasured.** Every ESP32-S3 figure for this trainer in the repository is host time × 32, and the board itself measured that scaling factor to be wrong by ~8× — the real ratio is ~260-270× (`BRINGUP-LOG.md:207-209`). `BRINGUP-LOG.md:221-222` asked for the correction to be propagated; it never was.

**Experiment:** the fixture already exists. `firmware/latency-rig/latency_rig.ino` runs `iris_retrain_new(K, 1234, 600)` on the board with `esp_timer`. Replace with `iris_train_converge(k, 0, 0, 0)` at 5, 10, 20, 50, 100 examples, 5 repetitions each, report epochs spent and milliseconds. One afternoon.

**Threshold that changes the decision:** if a 20-example run exceeds ~10 s on the board, the blocking form must be removed from the recommendation and the sliced form (`iris_train_begin`/`iris_train_slice`) made the only documented path — or the ceiling lowered. Derived arithmetic says ~7 s typical and ~40 s at ceiling, but derived is not measured.

### 7.2 An on-device bit-identity check. **Could change the answer on criterion 4.**

**Currently unmeasured for any trainer.** The `#pragma STDC FP_CONTRACT OFF` at `iris.h:83-85` is guarded to clang and is **ignored by GCC always, including the xtensa compiler for the ESP32-S3**. The Arduino sketches pass no contraction flag. So the shipped device build sits outside the tested determinism class, and "runs identically on a laptop and on an ESP32-S3" is an unverified claim.

**Experiment:** on the board, run `iris_retrain_new(k, 1234, 800)`, `iris_save`, FNV-1a hash the bytes, print over USB. Compare to `0xFEFAEDF6` / `0x6805FB0D`. Then repeat for a `iris_train_converge` run with a newly frozen hash. Compile with `-ffp-contract=off` explicitly in `platformio.ini` / the IDF component CMake.

**Threshold:** if the hashes differ, `ADR 0003:56-59` already anticipates the answer — the shipped contract becomes "bit-identical per platform" rather than unconditional, and the header must say so.

### 7.3 The N=5-12 crossover. **Could change the recommendation at small N.**

The repository has smooth-vs-structured target comparisons at N=5, 20 and 50 (`research/notes/training-and-editing-design.md:74-77`, `:44-50`) and knows the answer reverses at N=10 (`README.md:176`, `:182-188`). It does **not** have the 6-12 range where the crossover actually lives.

**Experiment:** N ∈ {5, 6, 8, 10, 12, 15}, both the SMOOTH and DETAIL truth functions, 40 seeds, comparing `iris_train_converge` against a fixed 600-repetition run on: held-out grid error, roughness, and the shipped reroll near-band test (`audit.c:375-383`).

**Threshold:** if convergence loses on held-out error below some N*, the recommendation gains one line — *below N* examples, use a fixed budget* — and the default should implement that fallback. Present evidence suggests N* is around 10-15, but it is **unmeasured** in that range.

### 7.4 Recorded human gesture. **The measurement most likely to overturn everything.**

Every accuracy number in this repository is on a smooth, noiseless, synthetic target function. The file says so itself (`iris.h:723-727`, `ADR 0017:69-75`): *"'More convergence never hurts' is exactly the conclusion most at risk from real sensor noise and human inconsistency, and none of this is verified on hardware or on recorded human gesture."*

**Experiment:** one recorded set — a real sensor, a real performer, 20 demonstrations with the ordinary inconsistency a human produces — held out against a second recorded set of the same intended gesture. Compare 600 repetitions, plateau, and ELM.

**Threshold:** if the plateau trainer overfits human noise where a fixed budget does not, the entire ADR 0017 argument is regime-limited and the default changes. This is not a nice-to-have; it is the load-bearing untested assumption.

### 7.5 The `err < 1e-6f` stop, after it is fixed.

Measured in this review across 40 seeds: at 5 examples, **36-39 of 40 runs stop on this floor, not on the plateau test**, and 22 of 40 finish before the plateau test can structurally fire. After the fix in §8, re-run to confirm the criterion actually operates at N=5-10. If it still almost never fires there, §7.3's fallback becomes mandatory rather than optional.

---

## 8. The exact text changes

### 8.1 Three code fixes, prerequisites for the recommendation

**Fix 1 — the hidden stopping rule.** `iris.h:893` currently reads:

```c
if (err < 1e-6f) { k->tr_running = 0; break; }  /* good enough; stop early */
```

It sits outside the `conv` guard and outside `#ifndef IRIS_NO_GUARDS`, so it fires on every entry point including the one the file calls the Wekinator fidelity path, with status still `IRIS_STATUS_OK` and `iris_train_progress` still returning 1.0. This is `CORE-AUDIT` priority fix #3 — the only one of four fixes to this file that was not applied. Either gate it behind `conv`, or set a distinguishable status so the caller can ask which stopping rule fired.

**Fix 2 — `epochs <= 0`.** `err` is initialised to `0.0f` at `:788`, the loop at `:798` does not execute, and `:906-908` unconditionally runs `k->trained = 1; k->last_error = err;`. So `iris_train_epochs(k, 0)` reports a freshly randomised network as trained with a perfect fit. Two live callers pass an unvalidated integer straight through: `ports/wasm/wasm_shim.c:110` (from JavaScript) and `benchmark/adapters/iris_adapter.c:114`. Refuse.

**Fix 3 — refusal return values.** A refused train returns the *previous* successful run's error (`:781`), so a caller reading only the return value cannot tell a refusal from a repeat. `iris_train_lbfgs` returns `-1.0f` and says so at `:1454`. Same library, same failure, two conventions. Pick one. (And while there: `iris_train_elm`'s four refusals at `:1559-1562` leave `k->status` at OK, which is a live ADR 0004 violation that has already cost a hardware debugging session.)

### 8.2 The header usage block — replace `iris.h:20-40`

```
   USAGE
     static unsigned char mem[IRIS_ARENA(2, 12, 3, 64)];
     iris *k = iris_init(mem, sizeof mem, 2, 12, 3, 64, 12345);

     iris_record(k, gesture, sound);     // do this a few times
     iris_train_converge(k, 0, 0, 0);    // trains until the error stops improving
     iris_predict(k, gesture, sound);    // now play

   ONE TRAINER, AND ONE REFERENCE.

   iris_train_converge is the trainer. It is backpropagation — the same engine
   as everything else in PART 8 — with one rule on top: every 2,000 epochs it
   asks whether the last 2,000 bought at least 10% of the remaining error, and
   stops when they did not. There is nothing to tune. A criterion, not a
   constant, because no constant is right at every example count: at 50
   examples a fixed 200,000 epochs generalises WORSE than 60,000 (0.0065 vs
   0.0063 held-out; PART 8, ADR 0017).

   iris_train_epochs(k, n) is the reference, and it is the one to READ FIRST.
   Five lines, one counter, no stopping rule to explain. It is the byte-pinned
   fidelity path that audit check 12 hashes, that the golden files were made
   with, and that every published benchmark number was measured through. It is
   permanent and unchanged. Improvements go to iris_train_converge.

   ON TRAINING TIME. A converged run typically spends 8,000-20,000 epochs;
   measured on a laptop that is 14 ms at 10 examples and 26 ms at 20 (2-12-3,
   README table). ON THE ESP32-S3 THIS IS UNMEASURED. The one on-device
   training measurement this project has is 321 ms for 20 examples at 600
   epochs (hardware/board/BRINGUP-LOG.md), which makes the device roughly 270x
   slower than the host — so a converged run at 20 examples is of the order of
   SEVEN SECONDS, not the "~1 s" that older host-x32 estimates in this repo
   imply. Treat every "est. S3" number outside BRINGUP-LOG as optimistic by
   about 8x until the board measurement lands.

   If the UI must keep drawing across those seconds, take the same run in
   slices: iris_train_begin / iris_train_slice / iris_train_progress. Sliced and
   unsliced are bit-identical (audit check 31). This is what the firmware does.
```

Also correct `iris.h:729` ("A converged run at 50 examples is ~4 s on the S3") and `:750` ("~150 ms host / ~4.8 s S3 at 20 ex") — both are host × 32 and both are 2-8× off every other table in the tree. Mark them derived, or remove the S3 half until measured.

### 8.3 The README — replace the trainer paragraph at `README.md:92-98`

```markdown
## The trainer

There is one: `iris_train_converge`. It is backpropagation run until the
training error plateaus, with a hard ceiling of 60,000 epochs. You pass it
nothing — no learning rate, no epoch count, no tolerance.

It was chosen on where it STOPS, not on how fast it reaches a mediocre
baseline. Carrying the same code to its plateau instead of stopping at 600
epochs takes recall of your own demonstrations 0.0112 -> 0.0019 (5.9x) and
held-out error 0.0158 -> 0.0086 (1.8x) at 8 outputs and 20 examples
(PART 8 of iris.h; ADR 0017).

`iris_train_epochs` is retained verbatim as the reference implementation and
the fidelity path. It is what audit check 12 pins to a hash, what the golden
files were made with, what every number in `benchmark/results/` was measured
through, and the only trainer that has ever been run on an ESP32-S3. Read it
first; it is five lines and a counter.

`iris_train_elm` is not a trainer you choose — it is what the firmware runs on
every demonstration while you play (~0.003 ms on a laptop; on the S3
UNMEASURED). It freezes the hidden layer and solves the output layer in one
step. It cannot diverge, and it is not learning: the interesting half of the
network never moves.

`iris_train_lbfgs` has moved to `experimental/`. It has zero production callers,
it stalls 3.8x above the plateau trainer's error floor, and 50x more
iterations return bit-identical weights (audit checks 18, 33). The audit still
runs it, because "the method the small-data literature recommends was measured
here and lost" is a result worth keeping.

### Where this repo's own documents disagree

- **ESP32-S3 timings.** Most S3 columns in this repo are host time x 32. The
  board measured ~260-270x (BRINGUP-LOG.md:207-209). Every x32 column is
  optimistic by ~8x. Only RELEASE-v1.0.md and admin/DECISION-TREE.md have
  absorbed the correction.
- **Converge cost at 50 examples.** This README says 10,000 epochs / 39.8 ms.
  ADR 0017:65-67 and REPORT.md:299 say 12,000 epochs / 47 ms. Same shape,
  same seed. Unresolved.
- **Converge cost at 20 examples.** 26.1 ms here, 27 ms in ADR 0017, 34 ms in
  REPORT.md:317 (8 outputs, 9 seeds), 41.3 ms at README:199 (8 outputs, 11
  seeds).
- **Arena size.** This README:224 and RELEASE-v1.0.md:53 say 8,200 B;
  benchmark/README.md:185 says 8,192 B; the audit measures 9,256 B.
- **Header line count.** Several documents say 2,204 lines. Measured today:
  2,087.
- **Check count.** build.sh:3 says twenty-five checks; this README says
  thirty-three (36 assertions); the audit prints 38.
- **L-BFGS speedup.** RELEASE-v1.0.md:50 says 3.3-3.5x; ADR 0007:16-17 and
  REPORT.md:71-72 say 4.3-4.7x; the audit, run today, reports worst-seed
  7.6x. The 3.3-3.5x figure has no supporting table anywhere in the tree.
- **Reroll bands at 5 examples.** ADR 0001:83-84, DECISION-TREE.md:85,
  RESEARCH-POSITION.md:24 and two comments in audit.c all say 0.0763 gaps /
  0.0165 near-demos. Measured today on the shipped header: 0.0866 / 0.0144.
  The recorded pair predates the v0.3.0 input-scaling change.
- **ELM on-device cost.** iris.h:1464 says ~0.2 ms; RELEASE-v1.0.md:48 says
  "~1 ms measured". BOTH are the same host measurement under two different
  scalings. No stopwatch reading of iris_train_elm on an ESP32-S3 exists.

### What is not verified

Every accuracy number here comes from smooth, noiseless synthetic target
functions. Nothing is verified on recorded human gesture. Nothing about
`iris_train_converge` is verified on hardware. "More convergence never hurts"
is exactly the conclusion most at risk from real sensor noise, and at 10
examples it already reverses on our own test target (0.0302 converged vs
0.0299 at 600 epochs).
```

### 8.4 One new ADR

`docs/adr/0021-one-trainer.md`, recording: the decision, the three prerequisite fixes, the L-BFGS relocation with its line count, the honest note that the field's own small-data advice (scikit-learn, MATLAB) points at L-BFGS and we departed on measured evidence with the caveat that we tested it without a ridge penalty and without a convergence test, and the standing residual — **the plateau criterion measures fit, not generalisation, and the repository's own strongest counter-measurement (a 1.93× regression on a locally-structured target at 20 examples, 2.7× outside the project's own shipped reroll band) is a case where those two point in opposite directions and the criterion follows the wrong one.**