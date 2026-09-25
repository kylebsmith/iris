# Negative results

Ideas that were measured and are not in the library. Each paragraph says what
was tried, gives one headline number, says why not to propose it again without
new evidence, and names the study in
[iris-studies](https://github.com/kylebsmith/iris-studies) that holds the
measurement. New evidence means a measurement that beats the recorded one, on
the same question, from a program anyone can run. Unless a paragraph says
otherwise, every figure comes from smooth synthetic targets; none has been
checked on recorded human gesture.

## Against the library's own default

**Training to a plateau on noisy demonstrations.** `iris_train` trains until
the error stops improving
([ADR 0017](../adr/0017-train-to-the-plateau-not-to-a-constant.md)). With
noise added to the demonstrated outputs (standard deviation 0.02 or more), a
fixed budget of 100 epochs beat the plateau default on held-out error by 1.2
to 3.7 times, in 14 to 16 of 16 trials. A learner that keeps going learns the
noise along with the mapping. The default stands only because nothing has yet
been measured on recorded human gesture, which is noisy in its own way; that
study decides it. (iris-studies S08)

## Representations and model

**Dyadic positional encoding of the inputs** (sines and cosines of each input
at doubling frequencies). Recall of the demonstrations improved up to 20 times,
but held-out error got worse, not better: it memorises the demonstrations
harder without learning the mapping between them, and it widens the reroll
spread on demonstrated ground. The code and full tables are in
[positional-encoding.md](positional-encoding.md). (iris-studies S20)

**A more accurate activation function** (a [7/6] Padé approximant, a ratio of
a 7th- and a 6th-degree polynomial, or the C library's `tanhf`) in place of the
library's rational tanh. It gives 1.3% higher held-out error as a geometric
mean, and costs more time per epoch. An earlier, larger figure did not
replicate. The saved-instrument format depends on the
activation, so changing it is a format change as well. (iris-studies S01)

**The exact derivative of the rational tanh in the backward pass**, in place
of the true-tanh derivative. Contested: separate runs measured it slightly
better, slightly worse, and no different, the most recent giving a geometric
mean ratio of 0.997. No run found a difference
worth a format change. (iris-studies S01)

**Cross-entropy loss, or linear outputs, in place of the sigmoid output with
squared error.** Both were worse on held-out error at every number of
demonstrations tested, by up to 2.28 times for cross-entropy.
(iris-studies S04)

**A Gaussian-process mapper.** Worse than radial basis function interpolation
at equal cost (held-out grid error 0.0239 against 0.0220 at 20
demonstrations), and it misses its own demonstrations. (iris-studies S14)

**A radial basis function kernel wider than about 1.6 times the median
nearest-neighbour distance.** Past that width, in single precision, the
interpolant stops reproducing its own demonstrations and its weights grow
towards values that overflow into a not-a-number. (iris-studies S21)

## Choosing settings automatically

**Automatic smoothing by leave-one-out.** Rerolling the same demonstrations
changes its pick: 2.17 distinct values per dataset on average, in the figures
`iris.h` gives at `iris_suggest_smoothing`. An instrument whose smoothing
changes when you reroll is not predictable. The size of the instability is
contested between runs; its presence is not. The library offers the
suggestion and leaves applying it to the caller. (iris-studies S02)

**Choosing the hidden width by leave-one-out.** At noise of standard deviation
0.05 it gave worse held-out error than a fixed 12 hidden units. Smoothing is
the axis leave-one-out can rank; width is not. (iris-studies S02)

## Finding the bad demonstration

**The residual at the end of training as a bad-take detector.** After training
to the plateau it ranked a take offset by +0.40 first in only 2 of 20 trials:
the optimiser bends the fit onto the bad take until it looks like every other.
The library integrates the residual over the whole run instead
([ADR 0019](../adr/0019-the-residual-ledger-integrates-it-does-not-sample.md)).
(iris-studies S10)

**A nearest-neighbour leave-one-out residual as a detector.** Free and
independent of training, and it found a take offset by +0.05 or +0.10 at most
2 times in 20: with twenty demonstrations in two dimensions, a small offset is
inside the natural spread between neighbours. (iris-studies S10)

## Training methods and precision

**8- and 16-bit integer weights.** The weights of a small instrument are a few
hundred bytes, and prediction was never the bottleneck. 8-bit weights erred by
up to 1.10% of a parameter's range, audible as zipper noise on a resonant
parameter; 16-bit weights bought nothing. (iris-studies S19)

**Training in 16- or 8-bit fixed point (Q15, Q7).** Late-training updates
fall below one step of Q15 at the weight range training reaches (a resolution
of 1.22e-4), so learning freezes in the last stretch of training; the usual fix,
stochastic rounding, puts randomness into every update and breaks
bit-identity. (iris-studies S19)

**RMSProp** (root-mean-square propagation, a per-weight adaptive step size).
Up to 10.7 times slower than the library's momentum descent to reach the same
fit, at 200 demonstrations. (iris-studies S19)

**Lion** (an optimiser that keeps only the sign of its update). Argued from
arithmetic, not run: its one saving is a second moment buffer the library
does not have, and on a problem this small it discards the gradient
magnitudes that matter. A run would settle it. (iris-studies S19)

**Learning-rate schedules** (one-cycle, cosine, warm-up). The recorded gap
between any scheduled first-order method and the second-order trainers was 10
to 40 times, larger than the most optimistic gain from a schedule.
(iris-studies S19)

**Shrink-and-perturb** (shrinking the weights and adding noise before
continuing training). Worse than plain continued training at every small
budget: training error 0.0263 against 0.0190 at 30 epochs. (iris-studies S19)

**Recency boosting** (presenting a new demonstration several extra times per
epoch). One extra presentation nearly doubled the epochs needed to reach the
same fit, 31 against 17, and more oscillated on contradictory corrections.
(iris-studies S19)

**Model soups** (averaging the weights of separately trained networks).
Averaging two rerolls gave a grid error 9 times worse than either parent:
independently seeded networks number their hidden units differently.
(iris-studies S19)

**Levenberg–Marquardt** (a second-order least-squares method) **as the default
trainer.** Contested as recorded: the record gives its warm-start hybrid a time
of 0.58 to 0.69 times that of L-BFGS (the limited-memory
Broyden–Fletcher–Goldfarb–Shanno method) and in the same sentence calls L-BFGS
the faster, which the ratio contradicts. Its program is not preserved, so the claim
stands as recorded, with the contradiction. It also needs working memory that
grows with the square of the weight count. (iris-studies S19)

**An L-BFGS history deeper than 5.** A depth of 8 gave a seed that ran away;
L-BFGS is not in the library. (iris-studies S09)

**Compensated (Kahan) summation** in the closed-form solve. It changes the
output weights by 2e-4 to 6e-4 and the error on the probe grid not at all.
(iris-studies S15, S16)

**No ridge (λ = 0) in the closed-form solve.** The Cholesky factorisation, the
linear solve inside the closed-form trainer, fails even on 20 well-spread
demonstrations ([ADR 0009](../adr/0009-ridge-is-mandatory.md)). (iris-studies S16)

**Short plateau windows** (200 to 500 epochs). They mistake the noise of a
shuffled training trace for a plateau and stop at about a quarter of the
achievable fit ([ADR 0017](../adr/0017-train-to-the-plateau-not-to-a-constant.md)).
(iris-studies S11)
