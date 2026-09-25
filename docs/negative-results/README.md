# Negative results

Ideas that are not in the library. Each paragraph says what was tried, gives
its headline number, says why not to propose it again without new evidence,
and names where the number comes from. A few were argued instead of run, and
say so. Sources follow
[Where the numbers come from](../../README.md#where-the-numbers-come-from),
with [iris-studies](https://github.com/kylebsmith/iris-studies/tree/v1) cited
at its tag v1. New evidence
means a measurement that beats the recorded one, on the same question, from a
program anyone can run. Unless a paragraph says otherwise, every figure comes
from synthetic targets; none has been checked on recorded human gesture.

## Against the library's own default

**Training to a plateau on noisy demonstrations.** `iris_train` trains until
the error stops improving
([architecture decision record (ADR) 0017](../adr/0017-train-to-the-plateau-not-to-a-constant.md)).
With additive noise of standard deviation 0.02 to 0.10 on 20 demonstrations, a
fixed budget of 100 epochs beat the plateau on held-out error by 1.45 to 3.0
times (iris-studies S08). On six target shapes at noise of 0.05 and above, the
plateau at smoothing 0 is 1.2 to 2.2 times worse than 100 epochs (the table at
`iris_set_smoothing` in `iris.h`; from a re-run whose program is not yet
published; the original study is iris-studies S08). A learner that keeps going
learns the noise along with the mapping. The default stands only because
nothing has yet been measured on recorded human gesture, which is noisy in its
own way; that study decides it.

## Representations and model

**Dyadic positional encoding of the inputs** (sines and cosines of each input
at doubling frequencies). Recall of the demonstrations improved up to 20 times,
but held-out error did not: it was level with the raw inputs at two octaves
(0.0587 against 0.0596) and worse from three octaves up (0.0649 and 0.0794). It
memorises the demonstrations harder without learning the mapping between them,
and it widens the reroll spread on demonstrated ground. The code and full
tables are in [positional-encoding.md](positional-encoding.md) (from a study
whose program is not yet published).

**A more accurate activation function** (a [7/6] Padé approximant, a ratio of
a 7th- and a 6th-degree polynomial, or the C library's `tanhf`) in place of the
library's rational tanh. It gives 1.3% higher held-out error as a geometric
mean, and costs more time per epoch (from a re-run whose program is not yet
published; the original study is iris-studies S01, which recorded 3.8% higher
error and 43% more training time). What a saved instrument plays depends on
the activation, so changing it is a format change as well.

**The exact derivative of the rational tanh in the backward pass**, in place
of the true-tanh derivative. Contested: one run measured it 1.4% better and
another 1.1% worse (iris-studies S01), and a third found no difference, a
geometric mean ratio of 0.997 (from a re-run whose program is not yet
published; the original study is iris-studies S01). No run found a difference
worth changing training for, which would move the golden hashes of training
(`0x6805FB0D`, and the starter kit's `0xB7FC47A0` and `0x203834ED`); saved
instruments would play the same.

**Cross-entropy loss with the targets rescaled to [0, 1], or linear outputs,
in place of the sigmoid output with squared error.** Both were worse on
held-out error at every number of demonstrations tested (5, 10, 20 and 50): by
up to 2.28 times for cross-entropy and 1.77 times for linear outputs. A
cross-entropy gradient with the targets left in [0.1, 0.9] is the exception. It
wins 32 of 32 seeds at 20 demonstrations by about 5% and 24 of 32 at 50, and
by 10 to 12% at both with a tuned learning rate. It loses at 10 demonstrations
(1.094 times worse) and widens the reroll spread in the undemonstrated gaps by
1.6 times. Keeping squared error is therefore a judgement about the use case
made on top of the measurement; `iris.h` states the trade in its comment on
the output-layer error. New evidence here would be a measurement at 5 to 10
demonstrations, or of the reroll spread, that reverses those two costs.
(iris-studies S04)

**A Gaussian-process mapper.** Worse than radial basis function interpolation
at equal cost (held-out grid error 0.0239 against 0.0220 at 20
demonstrations), and it misses its own demonstrations (from a study whose
program is not yet published;
[ADR 0010](../adr/0010-knn-the-sampler-beside-the-morpher.md) records it).

**A Gaussian radial basis function kernel wider than about 1.6 times the median
nearest-neighbour distance.** Past that width, in single precision, the
interpolant stops reproducing its own demonstrations (an error of 4.7e-4 at
twice the distance and 0.18 at six times), and its largest weight grows from 102
at 1.5 times to 256,713 at six times, which the design document written before
the library judged a route to a not-a-number in the output. A Wendland C2
kernel, which is zero beyond its width, stayed within 2.9e-5 of its
demonstrations up to six times. (iris-studies S21)

## Choosing settings automatically

**Automatic smoothing by leave-one-out.** Rerolling the same demonstrations
changes its pick: across 16 rerolls of a fixed dataset it gave 2.36 distinct
picks on average (iris-studies S02). A re-run on 36 datasets gives 2.17 distinct
values per dataset, the figure `iris.h` states at `iris_suggest_smoothing` (from
a re-run whose program is not yet published; the original study is
iris-studies S02). An instrument whose smoothing changes when you reroll is not
predictable. The library offers the suggestion and leaves applying it to the
caller.

**Choosing the hidden width by leave-one-out.** At noise of standard deviation
0.05 it gave worse held-out error than a fixed 12 hidden units. Smoothing is
the axis leave-one-out can rank; width is not. (iris-studies S02)

## Finding the bad demonstration

**The residual at the end of training as a bad-take detector.** After training
to the plateau it ranked a take offset by +0.40 first in only 2 of 20 trials:
the optimiser bends the fit onto the bad take until it looks like every other.
The library integrates the residual over the whole run instead
([ADR 0019](../adr/0019-the-residual-ledger-integrates-it-does-not-sample.md),
which records the figure, from a study whose program is not yet published).

**A nearest-neighbour leave-one-out residual as a detector.** Free and
independent of training, and it found a take offset by +0.05 or +0.10 at most
2 times in 20: with twenty demonstrations in two dimensions, a small offset is
inside the natural spread between neighbours (from a study whose program is
not yet published; ADR 0019 records it).

## Training methods and precision

**8- and 16-bit integer weights.** The weights of a small instrument are a few
hundred bytes, and prediction was never the bottleneck. 8-bit weights erred by
up to 1.10% of a parameter's range, a zipper-noise risk on a resonant
parameter; 16-bit weights bought nothing. (iris-studies S19)

**Training in 16- or 8-bit fixed point (Q15, Q7).** Argued from arithmetic,
not run: late-training updates of about 1e-5 fall below one step of Q15 at the
±4.0 weight range training reaches (a resolution of 1.22e-4), so learning would
freeze in the last stretch of training; the usual fix, stochastic rounding,
puts randomness into every update and breaks bit-identity. A run would settle
it. (iris-studies S19)

**RMSProp** (root-mean-square propagation, a per-weight adaptive step size).
Up to 10.7 times slower than the library's momentum descent to reach the same
fit, at 200 demonstrations. (iris-studies S19)

**Lion** (an optimiser that keeps only the sign of its update). Argued from
arithmetic, not run: its one saving is a second moment buffer the library
does not have, and on a problem this small it discards the gradient
magnitudes that matter. A run would settle it. (iris-studies S19)

**Learning-rate schedules** (one-cycle, cosine, warm-up). The recorded gap
between any scheduled first-order method and the second-order trainers was 10 to
40 times, larger than the most optimistic gain from a schedule, and the record
reopens the question if the second-order trainers leave the library
(iris-studies S19). The library has no second-order trainer, so the question is
open: a measured schedule against the library's momentum descent would settle
it.

**Shrink-and-perturb** (shrinking the weights and adding noise before
continuing training). Worse than plain continued training at every small
budget: training root-mean-square error 0.0263 against 0.0190 at 30 epochs.
(iris-studies S19)

**Recency boosting** (presenting a new demonstration several extra times per
epoch). One extra presentation nearly doubled the epochs needed to reach the
same fit, 31 against 17; two or more never reached it within 60 epochs and
oscillated on contradictory corrections. (iris-studies S19)

**Model soups** (averaging the weights of separately trained networks).
Averaging two rerolls gave a grid error 9 times worse than either parent:
independently seeded networks number their hidden units differently.
(iris-studies S19)

**Levenberg–Marquardt** (a second-order least-squares method) **as the default
trainer.** Contested as recorded: the record gives its warm-start hybrid a time
of 0.58 to 0.69 times that of L-BFGS (the limited-memory
Broyden–Fletcher–Goldfarb–Shanno method) and in the same sentence calls L-BFGS
the faster, which the ratio contradicts. Its program is not published, so the
claim stands as recorded, with the contradiction. It also needs working memory
that grows with the square of the weight count. (iris-studies S19)

**An L-BFGS history deeper than 5.** A depth of 8 gave a seed that ran away;
L-BFGS is not in the library. (iris-studies S09)

**Compensated (Kahan) summation** in the closed-form solve. Argued, not run in
this solve: single-precision output weights miss a double-precision reference by
2e-4 to 6e-4 in weight space only, and the error on the probe grid is unchanged,
so compensated summation has at most that to recover (from a study whose program
is not yet published; [ADR 0008](../adr/0008-elm-same-network-better-math.md)
records it). Where it was run, in the radial basis function solve of the design
document written before the library, it changed nothing (iris-studies S21).

**No ridge (λ = 0) in the closed-form solve.** The Cholesky factorisation, the
linear solve inside the closed-form trainer, fails even on 20 well-spread
demonstrations ([ADR 0009](../adr/0009-ridge-is-mandatory.md), which records
it, from a study whose program is not yet published).

**Short plateau windows** (200 to 500 epochs). They mistake the noise of a
shuffled training trace for a plateau and stop at about a quarter of the
achievable fit ([ADR 0017](../adr/0017-train-to-the-plateau-not-to-a-constant.md),
which records it, from a study whose program is not yet published).
