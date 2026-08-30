/* SPDX-License-Identifier: BSD-3-Clause
   Copyright (c) 2026 Kyle Smith */
/* ============================================================================
   iris_lbfgs.h  —  the L-BFGS trainer   [EXPERIMENTAL, NOT IN THE CORE]

   Moved out of iris.h on 2026-08-26. It is not part of the validated core
   and nothing that ships calls it: all nine call sites in the tree are in
   tests/audit.c. Its promotion to the device was gated on two ESP32-S3
   measurements (adr/0007:47-49) that have never been taken.

   ⚠️ DO NOT CITE CHECK 33 AS A RESULT ABOUT L-BFGS. It is a result about THIS
   IMPLEMENTATION, and this implementation has two defects that account for
   most of the gap (measured 2026-08-26, research/prior-art/
   WHY-lbfgs-defaults-robustness.md section 1):

   DEFECT 1 — the curvature safeguard is absolutely scaled. The test
   `sTy <= 1e-12` is a fixed constant, but this problem's gradients run at
   ||g||inf ~ 2e-5, so the test fires on a MEDIAN OF ~680 OF 1000 ITERATIONS.
   Two thirds of iterations contribute no curvature pair, so with m = 5 the
   history is mostly empty and the two-loop recursion is decorative. The
   textbook safeguard is RELATIVE: skip when sTy <= eps*||s||*||y||
   (Nocedal & Wright, Numerical Optimization 2e, Ch. 6).

   DEFECT 2 — the line search resolves rounding noise. Loss is accumulated and
   compared in float32. Near the plateau f ~ 2e-5, where the smallest
   representable step in f is ~2.4e-12 — SMALLER than the Armijo decrease being
   tested for. Result: 2,001-13,730 halvings across ~1000 iterations, about ten
   per iteration, so the accepted step is ~2^-10 of the quasi-Newton step.
   (Control: making only the two-loop dot products double moves the result by
   1.1%. Float32 inside the recursion is NOT the cause.)

   BOTH DEFECTS ARE NOW REPAIRED IN THIS FILE (2026-08-26) — see REPAIR 1 and
   REPAIR 2 at their sites below. Measured effect: the training-MSE gap against
   SGD-to-plateau fell from 3.8x to 1.61x on audit seed 4242; across 8 seeds the
   repaired median is 1.28x and the per-seed comparison is a coin flip (4/8).
   The golden L-BFGS weight hash was deliberately re-pinned (0x8260169D ->
   0xFB5BE623); the CORE SGD hash is unchanged, which is the proof the repair
   touched nothing outside this file. Audit check 33 was re-expressed: it now
   asserts the PLATEAU property (10x budget buys nothing), which is stable, and
   REPORTS the comparative ratio instead of asserting it.

   ALSO: check 33 asserts on TRAINING error. scikit-learn's guidance is about
   HELD-OUT error on a REGULARISED objective (it applies alpha=1e-4 by
   default; we apply none). On held-out error at 16 seeds SGD wins 15/16 by
   about 6% at the mean — not 3.8x.

   AND: MATLAB's fitrnet ships L-BFGS as its ONLY solver, so it expresses no
   preference between L-BFGS and SGD and cannot be cited as agreeing with us
   or disagreeing with us. (feedforwardnet defaulting to trainlm is a separate,
   usable citation for "MATLAB prefers second-order methods at small scale".)

   WHAT MAY HONESTLY BE SAID: on this library's noiseless reference task,
   plain SGD to a plateau reaches lower held-out RMSE than our full-batch
   L-BFGS on 15/16 seeds by ~6%. That does NOT contradict scikit-learn.
   THE RESIDUAL METHOD GAP IS STILL UNMEASURED. 1.28x is an UPPER BOUND, not a
   measurement, because a strong-Wolfe line search is STILL NOT IMPLEMENTED —
   this file uses Armijo backtracking only. Armijo alone does not guarantee the
   curvature condition sTy > 0 that makes the BFGS update well-posed; the
   relative test in REPAIR 1 is a safeguard standing in for that guarantee.
   Until a Moré-Thuente or equivalent strong-Wolfe search lands, the arm
   labelled "L-BFGS" here is not L-BFGS as the literature means it, and no
   statement of the form "the field's advice was tested here" is available.
   That is the single remaining prerequisite for a fair comparison.

   Usage: include this INSTEAD of iris.h; it includes the core itself.
   ============================================================================ */

#ifndef IRIS_LBFGS_H
#define IRIS_LBFGS_H

#include "../iris.h"

/* ==========================================================================
   PART 8c — THE FAST TRAINER  (L-BFGS, m = 5, Armijo line search)

   Full-batch limited-memory quasi-Newton. Where backprop nudges every weight
   a little and repeats thousands of times, this builds a cheap running model
   of the error surface's curvature from the last 5 steps and jumps. The line
   search only ever ACCEPTS a step that reduces the loss — otherwise the
   weights are restored bit-exactly and the step size is halved — so
   divergence is structurally impossible, not just unobserved.

   Measured against the 600-epoch backprop baseline (16 seeds): reaches the
   same error 4.3x faster at 20 examples, 4.7x at 50 (worst seeds 3.2x /
   2.5x), and at equal wall-clock the loss is 2.5-2.7x LOWER with better
   generalisation at every example count. The reroll character shifts in the
   direction musicians like: ~1.7x livelier in the gaps, slightly steadier
   at the demos.

   m = 5 is hard-coded, not a knob. Measured: m=8 has a runaway seed, m=12
   HALVES the number of seeds that reach target. More history means staler
   curvature pairs poisoning the inverse-Hessian model on this nonconvex
   surface — a double-precision diagnostic ruled out float32 rounding.

   Zero RNG after seeding: same weights + same examples => bit-identical
   result, and the same seed under the SAME trainer is still bit-identical.
   But an old seed retrained under L-BFGS is a DIFFERENT (equally valid)
   instrument than under backprop — anyone who wants their exact old
   instrument uses the saved file, or iris_train_epochs, which is retained
   verbatim and forever.

   The work buffer is transient scratch the caller owns and is NEVER
   serialized: IRIS_LBFGS_WORK(NI,NH,NO,5) bytes — 6,544 B at 2-16-4, and
   176,560 B at the 32-64-16 maximum (fits the S3's 512 KB but is a third of
   it: size it consciously). Too small a buffer refuses with -1, weights
   untouched.

   Budgets, for callers migrating from epochs: iters ~ epochs/6, and do not
   go below ~70 iterations (~400 epoch-equivalents) for interactive use —
   short-budget L-BFGS is measurably less steady near the demos.
   ========================================================================== */

#define IRIS_LBFGS_M 5           /* history depth — measured optimum, not a knob */
#define IRIS_LBFGS_NW(NI, NH, NO) ((NI)*(NH) + (NH) + (NH)*(NO) + (NO))
#define IRIS_LBFGS_WORK_FLOATS(NI, NH, NO, M) \
  ((size_t)(2*(M) + 4) * (size_t)IRIS_LBFGS_NW(NI, NH, NO) + 2*(size_t)(M))
#define IRIS_LBFGS_WORK(NI, NH, NO, M) \
  (sizeof(float) * IRIS_LBFGS_WORK_FLOATS(NI, NH, NO, M) + 8 /* align pad */)

/* one full-batch pass: loss always; gradient too when g != NULL.
   Normalisation via precomputed reciprocals; identical arithmetic on the
   loss whether or not the gradient is requested, so line-search decisions
   and the accepted-loss trace are consistent to the last bit. */
IRIS_API float iris_internal_lbfgs_pass(iris *k, float *g) {
  const int stride = k->n_in + k->n_out;
  const int NI = k->n_in, NH = k->n_hid, NO = k->n_out;
  const int ob1 = NH * NI, ow2 = ob1 + NH, ob2 = ow2 + NO * NH;
  const float cs = 2.0f / (float)(k->n_ex * NO);
  float x[IRIS_MAX_IN], tg[IRIS_MAX_OUT];
  float inv_i[IRIS_MAX_IN], inv_o[IRIS_MAX_OUT];
  /* same two scalings as iris_norm_in, folded into the reciprocal so the inner
     loop stays one multiply. cen == -1 reproduces 2*t-1 with one add. */
  const float cen = k->in_center ? -1.0f : 0.0f;
  for (int i = 0; i < NI; ++i)
    inv_i[i] = (k->in_center ? 2.0f : 1.0f) / (k->in_hi[i] - k->in_lo[i]);
  for (int o = 0; o < NO; ++o)
    inv_o[o] = (IRIS_OUT_HI - IRIS_OUT_LO) / (k->out_hi[o] - k->out_lo[o]);
  /* REPAIR 2 (2026-08-26) — accumulate the loss in DOUBLE. In float32 the
     loss near the plateau is ~2e-5, where the smallest representable step is
     ~2.4e-12 — LARGER than the Armijo sufficient-decrease being tested for.
     The line search was resolving rounding noise and halving ~10x per
     iteration, so the accepted step was ~2^-10 of the quasi-Newton step.
     Only the accumulator is double; the weights, the gradient and the two-loop
     recursion stay float32 (measured: making the recursion double moves the
     result by 1.1%, so it is not the cause). */
  double loss = 0.0;
  if (g) { const int W = ob2 + NO; for (int i = 0; i < W; ++i) g[i] = 0.0f; }
  for (int r = 0; r < k->n_ex; ++r) {
    const float *row = k->ex + (size_t)r * stride;
    for (int i = 0; i < NI; ++i) x[i] = (row[i] - k->in_lo[i]) * inv_i[i] + cen;
    for (int o = 0; o < NO; ++o)
      tg[o] = IRIS_OUT_LO + (row[NI + o] - k->out_lo[o]) * inv_o[o];
    iris_forward_norm(k, x);
    for (int o = 0; o < NO; ++o) {
      float y = k->out[o];
      float e = y - tg[o];
      loss += (double)e * (double)e;
      if (g) k->d_out[o] = cs * e * y * (1.0f - y);
    }
    if (!g) continue;
    for (int o = 0; o < NO; ++o) {
      float go = k->d_out[o];
      float *gw = g + ow2 + (size_t)o * NH;
      for (int h = 0; h < NH; ++h) gw[h] += go * k->hid[h];
      g[ob2 + o] += go;
    }
    for (int h = 0; h < NH; ++h) {
      float acc = 0.0f;
      for (int o = 0; o < NO; ++o) acc += k->w2[(size_t)o * NH + h] * k->d_out[o];
      float a = k->hid[h];
      float gh = acc * (1.0f - a * a);
      float *gw = g + (size_t)h * NI;
      for (int i = 0; i < NI; ++i) gw[i] += gh * x[i];
      g[ob1 + h] += gh;
    }
  }
  return (float)(loss / (double)(k->n_ex * NO));
}

/* Full-batch mean-squared loss at the current weights, same units as the
   value iris_train_epochs returns. Fits ranges first. */
IRIS_API float iris_eval_loss(iris *k) {
  if (k->n_ex == 0) return 1.0f;
  iris_fit_ranges(k);
  return iris_internal_lbfgs_pass(k, 0);
}

IRIS_API float iris_internal_dot(const float *a, const float *b, int n) {
  float s = 0.0f;
  for (int i = 0; i < n; ++i) s += a[i] * b[i];
  return s;
}

/* The full-argument trainer. tests/audit.c calls this directly for the
   monotone-descent trace; everyone else wants iris_train_lbfgs below.
     max_iters : cap on accepted iterations
     target    : stop when loss <= target
     work      : caller memory, >= IRIS_LBFGS_WORK(n_in, n_hid, n_out, 5)
     trace     : optional (NULL ok) — accepted losses, trace[0] = initial
     out_iters : optional — number of accepted iterations performed
   Returns final full-batch loss, or -1.0f refusing (bad work buffer, or a
   poisoned example — status says which; weights untouched either way). */
IRIS_API float iris_internal_train_lbfgs_full(iris *k, int max_iters, float target,
                                  void *work, size_t work_bytes,
                                  float *trace, int trace_cap, int *out_iters) {
  if (out_iters) *out_iters = 0;
  if (k->n_ex == 0) return 1.0f;
  const int m = IRIS_LBFGS_M;

  const int NI = k->n_in, NH = k->n_hid, NO = k->n_out;
  const int W = IRIS_LBFGS_NW(NI, NH, NO);
  if (!work || work_bytes < IRIS_LBFGS_WORK(NI, NH, NO, m)) return -1.0f;

  /* the parameter block [w1 | b1 | w2 | b2] is contiguous by arena layout */
  float *xp = k->w1;
  if (k->b1 != xp + NH * NI || k->w2 != k->b1 + NH ||
      k->b2 != k->w2 + NO * NH) return -1.0f;

#ifndef IRIS_NO_GUARDS
  /* same door as iris_train_epochs: refuse poisoned examples up front */
  {
    const int st = NI + NO;
    for (int i = 0; i < k->n_ex * st; ++i)
      if (iris_isbad(k->ex[i])) { k->status = IRIS_NAN_TRAPPED; return -1.0f; }
  }
  k->status = IRIS_STATUS_OK;
#endif

  unsigned char *pb = (unsigned char *)work;
  pb += ((uintptr_t)pb & 7u) ? (8u - ((uintptr_t)pb & 7u)) : 0u;
  float *S    = (float *)pb;              /* m * W */
  float *Y    = S + (size_t)m * W;        /* m * W */
  float *g    = Y + (size_t)m * W;
  float *gt   = g + W;
  float *d    = gt + W;
  float *xbak = d + W;
  float *rho  = xbak + W;                 /* m */
  float *al   = rho + m;                  /* m */

  iris_fit_ranges(k);
  float f = iris_internal_lbfgs_pass(k, g);
  if (trace && trace_cap > 0) trace[0] = f;
  int tn = 1;

  const float c1 = 1e-4f;
  int count = 0, head = 0;                /* ring buffer of (s,y) pairs */
  int iters = 0;

  for (int it = 0; it < max_iters; ++it) {
    if (f <= target) break;

    /* --- two-loop recursion: d = -H*g ----------------------------------- */
    for (int i = 0; i < W; ++i) d[i] = g[i];
    for (int j = 0; j < count; ++j) {
      int i = (head - 1 - j + 2 * m) % m;          /* newest -> oldest */
      al[i] = rho[i] * iris_internal_dot(S + (size_t)i * W, d, W);
      const float *yy = Y + (size_t)i * W;
      for (int t = 0; t < W; ++t) d[t] -= al[i] * yy[t];
    }
    if (count > 0) {
      int nw = (head - 1 + m) % m;
      float yy = iris_internal_dot(Y + (size_t)nw * W, Y + (size_t)nw * W, W);
      float sy = iris_internal_dot(S + (size_t)nw * W, Y + (size_t)nw * W, W);
      float gamma = (yy > 1e-30f) ? sy / yy : 1.0f;
      for (int t = 0; t < W; ++t) d[t] *= gamma;
    }
    for (int j = count - 1; j >= 0; --j) {
      int i = (head - count + j + 2 * m) % m;       /* oldest -> newest */
      float beta = rho[i] * iris_internal_dot(Y + (size_t)i * W, d, W);
      const float *ss = S + (size_t)i * W;
      for (int t = 0; t < W; ++t) d[t] += (al[i] - beta) * ss[t];
    }
    for (int t = 0; t < W; ++t) d[t] = -d[t];

    float gd = iris_internal_dot(g, d, W);
    int was_sd = (count == 0);
    if (gd >= 0.0f) {                    /* not a descent direction: reset */
      count = 0; head = 0; was_sd = 1;
      for (int t = 0; t < W; ++t) d[t] = -g[t];
      gd = -iris_internal_dot(g, g, W);
      if (gd >= 0.0f) break;             /* zero gradient */
    }

    /* first-ever step: scale so the step is O(1) in weight space */
    float alpha = 1.0f;
    if (was_sd) {
      float ginf = 0.0f;
      for (int t = 0; t < W; ++t) { float a = iris_absf(g[t]); if (a > ginf) ginf = a; }
      if (ginf > 1.0f) alpha = 1.0f / ginf;
    }

    /* --- Armijo backtracking, at most 20 halvings ------------------------
       Trial 0 computes loss AND gradient in one fused pass; if accepted
       (the common case) no separate gradient pass is needed. The fn == fn
       test rejects NaN trials, so a blown-up step can never be accepted.  */
    for (int t = 0; t < W; ++t) xbak[t] = xp[t];
    float fn = f;
    int accepted = 0, have_grad = 0;
    for (int ls = 0; ls < 20; ++ls) {
      for (int t = 0; t < W; ++t) xp[t] = xbak[t] + alpha * d[t];
      if (ls == 0) { fn = iris_internal_lbfgs_pass(k, gt); have_grad = 1; }
      else         { fn = iris_internal_lbfgs_pass(k, 0);  have_grad = 0; }
      if (fn == fn && fn <= f + c1 * alpha * gd) { accepted = 1; break; }
      alpha *= 0.5f;
    }
    if (!accepted) {
      for (int t = 0; t < W; ++t) xp[t] = xbak[t];  /* bit-exact restore */
      if (was_sd) break;                 /* stuck even on steepest descent */
      count = 0; head = 0;               /* drop history, retry with -g */
      continue;
    }
    if (!have_grad) fn = iris_internal_lbfgs_pass(k, gt);     /* backtracked: 1 extra pass */

    /* --- curvature pair -------------------------------------------------- */
    float *ss = S + (size_t)head * W;
    float *yv = Y + (size_t)head * W;
    for (int t = 0; t < W; ++t) ss[t] = alpha * d[t];
    for (int t = 0; t < W; ++t) yv[t] = gt[t] - g[t];
    /* REPAIR 1 (2026-08-26) — RELATIVE curvature test. This was
       `if (sy > 1e-12f)`, an ABSOLUTELY scaled threshold. At this problem's
       gradient scale (||g||inf ~ 2e-5) it discarded a median of ~680 of 1000
       curvature pairs, leaving the m=5 history mostly empty and the two-loop
       recursion decorative. The textbook safeguard scales with the operands
       (Nocedal & Wright, Numerical Optimization 2e, Ch. 6): accept the pair
       only when sTy exceeds a small multiple of ||s||*||y||. */
    float sy = iris_internal_dot(ss, yv, W);
    {
    const float ss_n = iris_sqrt(iris_internal_dot(ss, ss, W));
    const float yy_n = iris_sqrt(iris_internal_dot(yv, yv, W));
    const float rel  = 1e-8f * ss_n * yy_n;
    if (sy > rel && sy > 0.0f) {
      rho[head] = 1.0f / sy;
      head = (head + 1) % m;
      if (count < m) count++;
    }
    }
    for (int t = 0; t < W; ++t) g[t] = gt[t];
    f = fn;
    iters++;
    if (trace && tn < trace_cap) trace[tn++] = f;
  }

  if (out_iters) *out_iters = iters;
  k->trained = 1;
  k->fitted  = 1;
  k->last_error = f;
  return f;
}

/* Train with L-BFGS — the fast trainer. Returns final full-batch loss (same
   units as iris_train_epochs), or -1.0f on refusal (see above). The 1e-6f
   target matches iris_train_epochs' early-stop threshold. */
IRIS_API float iris_train_lbfgs(iris *k, int max_iters, void *work, size_t work_bytes) {
  return iris_internal_train_lbfgs_full(k, max_iters, 1e-6f, work, work_bytes, 0, 0, 0);
}


#endif  /* IRIS_LBFGS_H */
