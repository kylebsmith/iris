/* SPDX-License-Identifier: BSD-3-Clause
   Copyright (c) 2026 Kyle Smith */
/* ============================================================================
   playing.c -- the playing paths and the two neighbour functions.

   Each check states the behaviour it holds and was watched to fail with that
   behaviour broken in a scratch copy of iris.h.

   Build and run from the repository root:

     mkdir -p build && cc -std=c99 -O2 -Wall -Wextra -I. -o build/playing tests/playing.c -lm && ./build/playing

   Exits 0 when every check passes and 1 when any fails.
   ========================================================================= */
#include "iris.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
static void check(const char *name, int ok, const char *detail) {
  printf("  [%s] %-58s %s\n", ok ? "PASS" : "FAIL", name, detail);
  if (!ok) fails++;
}

/* A small linear congruential generator, so every run sees the same data. */
static unsigned lcg_state = 1u;
static float lcg01(void) {
  lcg_state = lcg_state * 1664525u + 1013904223u;
  return (float)(lcg_state >> 8) / 16777216.0f;
}

/* The playing functions write into the instrument -- the status, the
   network's activations, the ranges of a never-fitted instrument -- so they
   take a non-const one, and so does iris_novelty, which fits those ranges
   too. These four lines stop compiling if a signature goes back to
   const iris *. */
static void  (*const play_net)(iris *, const float *, float *) = iris_predict;
static void  (*const play_knn)(iris *, const float *, float *, int) = iris_knn_predict;
static int   (*const play_1nn)(iris *, const float *, float *) = iris_classify_1nn;
static float (*const play_nov)(iris *, const float *) = iris_novelty;

static unsigned char A[IRIS_ARENA(2, 12, 2, 64)];
static unsigned char B[IRIS_ARENA(2, 12, 2, 64)];
static unsigned char SCR[IRIS_ELM_SCRATCH(12, 2)];
static unsigned char FILE_BUF[8192];

/* Six demonstrations in which input 0 moves across [0,1] and input 1 rests at
   `still` in every one: the shape of a switch left alone or a sensor against
   its rail. Two outputs, so both the network and the neighbours have
   something to blend. */
static iris *still_instrument(unsigned char *mem, size_t bytes, float still) {
  iris *k = iris_init(mem, bytes, 2, 12, 2, 64, 1234u);
  for (int i = 0; i < 6; ++i) {
    const float u = (float)i / 5.0f;
    float in[2] = { u, still }, out[2] = { 10.0f + 10.0f * u, 3.0f - 2.0f * u * u };
    iris_record(k, in, out);
  }
  return k;
}

/* Everything each playing function says across a sweep of input 0, with
   input 1 reading `reading`: the network, three nearest neighbours, the
   single nearest, and novelty. Compared whole with memcmp by the callers. */
typedef struct { float net[21][2], knn[21][2], nn[21][2], nov[21]; } sweep;
static void play_sweep(iris *k, float reading, sweep *s) {
  for (int q = 0; q <= 20; ++q) {
    float in[2] = { q / 20.0f, reading };
    iris_predict(k, in, s->net[q]);
    iris_knn_predict(k, in, s->knn[q], 3);
    iris_classify_1nn(k, in, s->nn[q]);
    s->nov[q] = iris_novelty(k, in);
  }
}
static float span_of(const sweep *s) {
  float lo = s->net[0][0], hi = s->net[0][0];
  for (int q = 1; q <= 20; ++q) {
    if (s->net[q][0] < lo) lo = s->net[q][0];
    if (s->net[q][0] > hi) hi = s->net[q][0];
  }
  return hi - lo;
}

/* THE SCALING PROPERTIES. iris fits its own ranges and does all its work in
   fractions of them, so the units you measure in cannot matter. Doubling a
   binary32 value is exact (it only changes the exponent), so the property can
   be held to the bit: double every input, in the demonstrations and in the
   query, and every playing path must give bit-identical answers; double every
   output and every answer must be exactly double. `path` picks what plays:
   0 unfitted, 1 iris_train, 2 iris_train_elm, 3 k-NN, 4 1-NN. Returns how
   many of the 15 x 15 probes broke the property.

   iris_novelty is held to the input property on every path: on an
   instrument never fitted it fits the demonstrated ranges, as the neighbour
   functions do, rather than measuring in the 0..1 ranges iris_init starts
   with, which are in raw units. */
static void scaled_store(iris *k, float in_scale, float out_scale) {
  lcg_state = 31337u;
  for (int r = 0; r < 12; ++r) {
    const float u = lcg01(), v = lcg01() * 4.0f - 2.0f;
    float in[2] = { u * in_scale, v * in_scale };
    float out[2] = { (10.0f + 10.0f * u - 3.0f * u * v) * out_scale,
                     (v * v - u) * out_scale };
    iris_record(k, in, out);
  }
}
static void play_path(iris *k, int path, const float *in, float *out) {
  if (path == 3) iris_knn_predict(k, in, out, 3);
  else if (path == 4) iris_classify_1nn(k, in, out);
  else iris_predict(k, in, out);
}
static int scaling_breaks(int path, int which) {
  iris *a = iris_init(A, sizeof A, 2, 12, 2, 64, 77u);
  iris *b = iris_init(B, sizeof B, 2, 12, 2, 64, 77u);
  scaled_store(a, 1.0f, 1.0f);
  scaled_store(b, which == 0 ? 2.0f : 1.0f, which == 1 ? 2.0f : 1.0f);
  if (path == 1) { iris_train(a); iris_train(b); }
  if (path == 2) { iris_train_elm(a, 1e-3f, SCR, sizeof SCR);
                   iris_train_elm(b, 1e-3f, SCR, sizeof SCR); }
  int broken = 0;
  for (int i = 0; i < 15; ++i) for (int j = 0; j < 15; ++j) {
    float q[2] = { -0.2f + 0.1f * (float)i, -2.5f + (5.0f / 14.0f) * (float)j };
    float q2[2] = { which == 0 ? 2.0f * q[0] : q[0], which == 0 ? 2.0f * q[1] : q[1] };
    float pa[2], pb[2];
    play_path(a, path, q, pa);
    play_path(b, path, q2, pb);
    if (which == 0) {
      if (memcmp(pa, pb, sizeof pa) != 0) broken++;
      if (iris_novelty(a, q) != iris_novelty(b, q2)) broken++;
    } else {
      if (pb[0] != 2.0f * pa[0] || pb[1] != 2.0f * pa[1]) broken++;
    }
  }
  return broken;
}

int main(void) {
  char d[256];
  printf("\n  PLAYING AND NEIGHBOURS\n\n");

  /* ---- k-nearest neighbours: a unanimous neighbourhood is exact ----------
     Twenty stores, ten random demonstrations each, every one labelled 3 on
     the first output and 0.1 on the second, queried over a 51 x 51 grid
     with every k from 2 to 8. Each answer must be the label, to the bit. */
  { long n = 0, wrong = 0;
    float example = 3.0f;
    lcg_state = 7u;
    for (unsigned s = 1; s <= 20; ++s) {
      iris *k = iris_init(A, sizeof A, 2, 12, 2, 64, s);
      for (int r = 0; r < 10; ++r) {
        float in[2] = { lcg01(), lcg01() }, out[2] = { 3.0f, 0.1f };
        iris_record(k, in, out);
      }
      for (int a = 0; a <= 50; ++a) for (int b = 0; b <= 50; ++b)
        for (int kk = 2; kk <= 8; ++kk) {
          float in[2] = { a / 50.0f, b / 50.0f }, o[2];
          iris_knn_predict(k, in, o, kk);
          n++;
          if (o[0] != 3.0f || o[1] != 0.1f) { wrong++; example = o[0]; }
        }
    }
    snprintf(d, sizeof d, "%ld of %ld answers not the label (e.g. %.9g)",
             wrong, n, (double)example);
    check("k-NN: a unanimous neighbourhood returns the label exactly", wrong == 0, d); }

  /* ---- k-nearest neighbours: never outside the demonstrated range --------
     Random stores and queries, then stores built from values at the very
     ends of their range (the smallest, the largest, and the floats next to
     them), where rounding would show first. */
  { long n = 0, out_of_range = 0;
    lcg_state = 12345u;
    for (int s = 0; s < 400; ++s) {
      iris *k = iris_init(A, sizeof A, 2, 12, 2, 64, 1);
      const int ne = 3 + s % 40;
      const float off = (s % 4 == 0) ? 0.0f : (s % 4 == 1) ? 1000.0f
                      : (s % 4 == 2) ? -3.0f : 1e6f;
      const float span = (s % 3 == 0) ? 1e-3f : 7.0f;
      float lo = 0.0f, hi = 0.0f;
      for (int r = 0; r < ne; ++r) {
        float in[2] = { lcg01(), lcg01() }, out[2];
        out[0] = off + lcg01() * span; out[1] = -out[0];
        if (r == 0 || out[0] < lo) lo = out[0];
        if (r == 0 || out[0] > hi) hi = out[0];
        iris_record(k, in, out);
      }
      for (int q = 0; q < 2000; ++q) {
        float in[2] = { lcg01() * 1.4f - 0.2f, lcg01() * 1.4f - 0.2f }, o[2];
        iris_knn_predict(k, in, o, 2 + q % 7);
        n++;
        if (o[0] < lo || o[0] > hi || -o[1] < lo || -o[1] > hi) out_of_range++;
      }
    }
    long n2 = 0, out2 = 0;
    const float tops[5] = { 1.0f, 3.0f, 1000.0f, 0.7f, 1e8f };
    for (int s = 0; s < 5000; ++s) {
      iris *k = iris_init(A, sizeof A, 2, 12, 2, 64, 1);
      const float top = tops[s % 5], bottom = (s & 8) ? -top : 0.0f;
      union { float f; uint32_t u; } below_top, above_bottom;
      below_top.f = top;       below_top.u -= 1u;
      above_bottom.f = bottom; above_bottom.u = bottom == 0.0f ? 1u : above_bottom.u - 1u;
      const float vals[4] = { bottom, top, below_top.f, above_bottom.f };
      float lo = 0.0f, hi = 0.0f;
      const int ne = 2 + s % 7;
      for (int r = 0; r < ne; ++r) {
        float in[2] = { lcg01(), lcg01() }, out[2];
        out[0] = vals[(int)(lcg01() * 4.0f) & 3]; out[1] = out[0];
        if (r == 0 || out[0] < lo) lo = out[0];
        if (r == 0 || out[0] > hi) hi = out[0];
        iris_record(k, in, out);
      }
      for (int q = 0; q < 200; ++q) {
        float in[2] = { lcg01(), lcg01() }, o[2];
        iris_knn_predict(k, in, o, 2 + q % 7);
        n2++;
        if (o[0] < lo || o[0] > hi) out2++;
      }
    }
    snprintf(d, sizeof d, "random: %ld of %ld outside; at the ends: %ld of %ld",
             out_of_range, n, out2, n2);
    check("k-NN: never outside the demonstrated range", out_of_range == 0 && out2 == 0, d); }

  /* ---- k-nearest neighbours: values near the largest float ---------------
     Two demonstrations whose outputs are 3e38 and -3e38: their difference
     overflows. The blend must still land inside the range, finite, with a
     healthy status. Then two demonstrations on one spot with outputs 1e30
     and 2e30, queried on that spot: each carries a weight of 1e9, and a
     weight times their difference overflows, but the answer must be their
     mean, 1.5e30, with a healthy status. Nothing here is broken; the numbers
     are just large. */
  { iris *k = iris_init(A, sizeof A, 2, 12, 2, 64, 1);
    float a_in[2] = { 0.0f, 0.0f }, a_out[2] = { 3e38f, 1.0f };
    float b_in[2] = { 1.0f, 1.0f }, b_out[2] = { -3e38f, 2.0f };
    iris_record(k, a_in, a_out); iris_record(k, b_in, b_out);
    int bad = 0;
    for (int q = 0; q <= 20; ++q) {
      float in[2] = { q / 20.0f, q / 20.0f }, o[2];
      iris_knn_predict(k, in, o, 2);
      if (iris_internal_isbad(o[0]) || o[0] > 3e38f || o[0] < -3e38f) bad++;
    }
    const int status_far = (int)iris_get_status(k);
    k = iris_init(A, sizeof A, 2, 12, 2, 64, 1);
    float spot[2] = { 0.25f, 0.5f }, corner[2] = { 1.0f, 1.0f };
    float y1[2] = { 1e30f, 1.0f }, y2[2] = { 2e30f, 1.0f }, y3[2] = { 1.5e30f, 1.0f };
    iris_record(k, spot, y1); iris_record(k, spot, y2); iris_record(k, corner, y3);
    float m[2];
    iris_knn_predict(k, spot, m, 2);
    const float mean = 0.5f * y1[0] + 0.5f * y2[0];
    const int mean_ok = iris_internal_absf(m[0] - mean) <= 1e-6f * mean
                     && iris_get_status(k) == IRIS_STATUS_OK;
    snprintf(d, sizeof d, "%d of 21 answers not finite or outside, status %d; "
             "1e30 and 2e30 blend to %g, status %d", bad, status_far,
             (double)m[0], (int)iris_get_status(k));
    check("k-NN: outputs near the largest float blend without overflow",
          bad == 0 && status_far == IRIS_STATUS_OK && mean_ok, d); }

  /* ---- takes recorded far outside the fitted ranges ----------------------
     Trained on two takes spanning 0..1 on two inputs, with a third input
     still at 7, then two takes recorded far outside those ranges and the
     first two deleted: B at (1e20, 1e20) recorded first, A at (1e21, 0.5)
     second, both with the still input at 3e38. A reading of (0.5, 0.5) is
     1e20 or more ranges from each, so every ordinary squared distance
     overflows, and the still input's difference from -3e38 overflows too.
     Counted in the far distance -- at most 10^15 ranges per input, still
     input skipped -- A is nearer (one input far, against two), so the
     classifier must name A although B is earlier, iris_delete_nearest must
     delete A, and the two-neighbour blend must weight A's 0.25 above B's
     0.75, inside them, with a healthy status. */
  { static unsigned char M[IRIS_ARENA(3, 8, 1, 8)];
    iris *k = iris_init(M, sizeof M, 3, 8, 1, 8, 1u);
    float a[3] = { 0.0f, 0.0f, 7.0f }, ya = 0.0f, b[3] = { 1.0f, 1.0f, 7.0f }, yb = 1.0f;
    float fb[3] = { 1e20f, 1e20f, 3e38f }, y_b = 0.75f;
    float fa[3] = { 1e21f, 0.5f, 3e38f }, y_a = 0.25f;
    iris_record(k, a, &ya); iris_record(k, b, &yb);
    iris_train(k);
    iris_record(k, fb, &y_b); iris_record(k, fa, &y_a);   /* identifiers 3 and 4 */
    iris_delete_index(k, 0); iris_delete_index(k, 0);
    const float q[3] = { 0.5f, 0.5f, -3e38f };
    float o = -1.0f, c = -1.0f;
    iris_knn_predict(k, q, &o, 2);
    const int knn_ok = o > 0.25f && o < 0.5f && iris_get_status(k) == IRIS_STATUS_OK;
    const int id = iris_classify_1nn(k, q, &c);
    const int one_ok = id == 4 && c == 0.25f && iris_get_status(k) == IRIS_STATUS_OK;
    const int deleted = iris_delete_nearest(k, q);
    const int del_ok = deleted == 1 && iris_count(k) == 1 && iris_id_at(k, 0) == 3;
    snprintf(d, sizeof d, "k-NN %g (status %d), 1-NN id %d playing %g, delete_nearest %d "
             "leaving id %d", (double)o, (int)iris_get_status(k), id, (double)c, deleted,
             iris_id_at(k, 0));
    check("neighbours answer takes far outside the fitted ranges", knn_ok && one_ok && del_ok, d); }

  /* ---- a store poisoned in memory has no nearest take ---------------------
     iris_record and iris_load refuse a value that is not finite, but the
     store is memory the caller can reach. With a not-a-number written into
     every take's moving input, no distance, ordinary or far, is a number,
     so neither neighbour function has a take to answer from: iris_knn_predict
     must write the substitute, the centre of the fitted output range, and
     iris_classify_1nn answer -1, both with IRIS_NAN_TRAPPED, and neither may
     read outside the store. */
  { static unsigned char M[IRIS_ARENA(2, 8, 1, 8)];
    iris *k = iris_init(M, sizeof M, 2, 8, 1, 8, 1u);
    float a[2] = { 0.0f, 0.0f }, ya = 2.0f, b[2] = { 1.0f, 1.0f }, yb = 4.0f;
    iris_record(k, a, &ya); iris_record(k, b, &yb);
    iris_train(k);
    k->ex[0] = __builtin_nanf("");
    k->ex[3] = __builtin_nanf("");
    const float q[2] = { 0.5f, 0.5f };
    float o = -1.0f, c = -1.0f;
    iris_knn_predict(k, q, &o, 2);
    const int knn_status = (int)iris_get_status(k);
    k->status = IRIS_STATUS_OK;
    const int id = iris_classify_1nn(k, q, &c);
    snprintf(d, sizeof d, "k-NN %g (status %d), 1-NN id %d playing %g (status %d)",
             (double)o, knn_status, id, (double)c, (int)iris_get_status(k));
    check("a store poisoned in memory gets the substitute, reported",
          o == 3.0f && knn_status == IRIS_NAN_TRAPPED && id == -1 && c == 3.0f
          && iris_get_status(k) == IRIS_NAN_TRAPPED, d); }

  /* ---- a still input is ignored: moving it changes nothing ---------------
     For inputs resting at 500 (a sensor mid-range), 4095 (a 12-bit rail), 0
     (a switch) and -2.5, trained both ways, every playing function must give
     bit-identical answers whether the still input reads its resting value,
     one count away, or something absurd. The instrument must also still
     respond to the input that did move. */
  { const float rests[4] = { 500.0f, 4095.0f, 0.0f, -2.5f };
    int moved = 0, dead = 0, runs = 0;
    float worst_span = 1e30f, worst_moved_span = 1e30f;
    for (int r = 0; r < 4; ++r) for (int trainer = 0; trainer < 2; ++trainer) {
      const float rest = rests[r];
      const float readings[5] = { rest + 1.0f, rest - 1.0f, rest + 0.001f, 1e6f, -1e6f };
      iris *k = still_instrument(A, sizeof A, rest);
      if (trainer == 0) iris_train(k); else iris_train_elm(k, 1e-3f, SCR, sizeof SCR);
      static sweep ref, got;
      play_sweep(k, rest, &ref);
      for (int j = 0; j < 5; ++j) {
        play_sweep(k, readings[j], &got);
        if (memcmp(&ref, &got, sizeof ref) != 0) moved++;
        if (span_of(&got) < worst_moved_span) worst_moved_span = span_of(&got);
      }
      const float sp = span_of(&ref);
      if (sp < worst_span) worst_span = sp;
      if (sp < 5.0f) dead++;
      runs++;
    }
    snprintf(d, sizeof d, "%d of %d sweeps changed; smallest output span %.2f of 10 "
             "at rest, %.2f moved", moved, runs * 5, (double)worst_span,
             (double)worst_moved_span);
    check("a still input does not change what any playing function says",
          moved == 0 && dead == 0, d); }

  /* ---- a still input produces no enormous numbers ------------------------
     Its normalised value is 0 whatever it reads, and training leaves its
     first-layer weights exactly where the reseed put them: with a value of
     0 no gradient reaches them. */
  { iris *k = still_instrument(A, sizeof A, 500.0f);
    float w0[12];
    for (int h = 0; h < 12; ++h) w0[h] = k->w1[h * 2 + 1];
    iris_train(k);
    int nonzero = 0, touched = 0, large = 0;
    const float probe[4] = { 500.0f, 501.0f, 499.999f, -1e30f };
    for (int j = 0; j < 4; ++j) if (iris_internal_norm_in(k, 1, probe[j]) != 0.0f) nonzero++;
    for (int h = 0; h < 12; ++h) if (k->w1[h * 2 + 1] != w0[h]) touched++;
    for (int i = 0; i < 24; ++i) if (!(iris_internal_absf(k->w1[i]) < 4.0f)) large++;
    snprintf(d, sizeof d, "normalised non-zero %d of 4, still weights moved %d of 12, "
             "|w1| >= 4: %d, width %g, status %d", nonzero, touched, large,
             (double)(k->in_hi[1] - k->in_lo[1]), (int)iris_get_status(k));
    check("a still input normalises to 0 and draws no gradient",
          nonzero == 0 && touched == 0 && large == 0
          && k->in_hi[1] == k->in_lo[1] && iris_get_status(k) == IRIS_STATUS_OK, d); }

  /* ---- ranges reach every finite demonstration ----------------------------
     The ranges are the smallest and largest value each column takes, however
     large: a search that started from a round number such as 1e30 would miss
     demonstrations beyond it. Both signs, inputs and outputs, and the largest
     finite float itself. */
  { iris *k = iris_init(A, sizeof A, 2, 12, 2, 64, 1u);
    float a[2] = { 2e30f, -3.4028235e38f }, b[2] = { 3e30f, -1e38f };
    float oa[2] = { -3e30f, 1e38f }, ob[2] = { -2e30f, 3.4028235e38f };
    iris_record(k, a, oa); iris_record(k, b, ob);
    iris_internal_fit_ranges(k);
    const int in_ok = k->in_lo[0] == 2e30f && k->in_hi[0] == 3e30f
                   && k->in_lo[1] == -3.4028235e38f && k->in_hi[1] == -1e38f;
    const int out_ok = k->out_lo[0] == -3e30f && k->out_hi[0] == -2e30f
                    && k->out_lo[1] == 1e38f && k->out_hi[1] == 3.4028235e38f;
    snprintf(d, sizeof d, "inputs [%g, %g] [%g, %g], outputs [%g, %g] [%g, %g]",
             (double)k->in_lo[0], (double)k->in_hi[0], (double)k->in_lo[1], (double)k->in_hi[1],
             (double)k->out_lo[0], (double)k->out_hi[0], (double)k->out_lo[1], (double)k->out_hi[1]);
    check("ranges reach demonstrations beyond 1e30", in_ok && out_ok, d); }

  /* ---- the rule's threshold, from both sides -----------------------------
     Still means a width of at most 1e-5 of the magnitude, or at most 1e-6.
     Five percent inside either threshold must be ignored and five percent
     outside must count, so a threshold moved by more than about 5% either
     way fails here. At 500 the float spacing is 3.05e-5, so the widths
     0.00475 and 0.00525 are stored as 0.0047607 and 0.0052490. */
  { struct { float lo, hi; int still; } c[10] = {
      { 500.0f, 500.004f, 1 },   /* width 0.004 <= 0.005: still        */
      { 500.0f, 500.01f,  0 },   /* width 0.01: moved                   */
      { 500.0f, 500.00475f, 1 }, /* 0.95 of 0.005: still                */
      { 500.0f, 500.00525f, 0 }, /* 1.05 of 0.005: moved                */
      { 0.0f,   5e-7f,    1 },   /* width 5e-7 <= 1e-6: still           */
      { 0.0f,   2e-6f,    0 },   /* width 2e-6: moved                   */
      { 0.0f,   9.5e-7f,  1 },   /* 0.95 of the 1e-6 floor: still       */
      { 0.0f,   1.05e-6f, 0 },   /* 1.05 of the 1e-6 floor: moved       */
      { -4095.0f, -4094.99f, 1 },/* width 0.01 <= 0.04095: still        */
      { 4095.0f, 4095.0f, 1 } }; /* exactly constant                    */
    int wrong = 0;
    for (int j = 0; j < 10; ++j) {
      iris *k = iris_init(A, sizeof A, 2, 12, 2, 64, 1u);
      float a[2] = { 0.0f, c[j].lo }, b[2] = { 1.0f, c[j].hi }, o[2] = { 0.0f, 1.0f };
      iris_record(k, a, o); iris_record(k, b, o);
      iris_internal_fit_ranges(k);
      const int is_still = (k->in_hi[1] == k->in_lo[1]);
      if (is_still != c[j].still) wrong++;
    }
    snprintf(d, sizeof d, "%d of 10 widths classified wrongly", wrong);
    check("a width 5% inside the threshold is still, 5% outside not", wrong == 0, d); }

  /* ---- the rule survives saving and loading -------------------------------
     A loaded instrument must play exactly as the one that was saved, still
     input included: a loader that widened the zero width would bring the
     enormous gain back. */
  { iris *k = still_instrument(A, sizeof A, 500.0f);
    iris_train(k);
    const size_t n = iris_save(k, FILE_BUF, sizeof FILE_BUF);
    iris *k2 = iris_init(B, sizeof B, 2, 12, 2, 64, 99u);
    const int loaded = n > 0 && iris_load(k2, FILE_BUF, n);
    static sweep before, after;
    play_sweep(k, 501.0f, &before);
    int same = 0;
    if (loaded) { play_sweep(k2, 501.0f, &after); same = memcmp(&before, &after, sizeof before) == 0; }
    snprintf(d, sizeof d, "load %d, loaded copy plays identically with the still "
             "input moved: %d", loaded, same);
    check("a still input stays ignored after save and load", loaded && same, d); }

  /* ---- a broken sensor on a still input is still reported ----------------
     Ignoring the input's value must not hide a not-a-number or an infinity
     coming from it: the answer is the substitute, and the status says so. */
  { iris *k = still_instrument(A, sizeof A, 500.0f);
    iris_train(k);
    const float broken[2] = { __builtin_nanf(""), __builtin_inff() };
    int reported = 0, finite = 1;
    for (int j = 0; j < 2; ++j) {
      float in[2] = { 0.5f, broken[j] }, o[2];
      k->status = IRIS_STATUS_OK;
      iris_predict(k, in, o);
      if (iris_get_status(k) == IRIS_NAN_TRAPPED) reported++;
      if (iris_internal_isbad(o[0]) || iris_internal_isbad(o[1])) finite = 0;
      k->status = IRIS_STATUS_OK;
      iris_knn_predict(k, in, o, 3);
      if (iris_get_status(k) == IRIS_NAN_TRAPPED) reported++;
      if (iris_internal_isbad(o[0]) || iris_internal_isbad(o[1])) finite = 0;
      k->status = IRIS_STATUS_OK;
      iris_classify_1nn(k, in, o);
      if (iris_get_status(k) == IRIS_NAN_TRAPPED) reported++;
      if (iris_internal_isbad(o[0]) || iris_internal_isbad(o[1])) finite = 0;
    }
    k->status = IRIS_STATUS_OK;
    snprintf(d, sizeof d, "%d of 6 reported, outputs finite %d", reported, finite);
    check("a not-a-number on a still input is still reported", reported == 6 && finite, d); }

  /* ---- every input still: one sound, and no fault -------------------------
     Six takes with both inputs resting and the outputs varying. No input
     moved, so the instrument cannot tell one gesture from another and the
     best it can play is one sound near the demonstrations' mean. Both
     trainers must fit that with a healthy status: the closed-form trainer's
     check for a mapping that collapsed to a constant must not call this a
     fault. */
  { int healthy = 0, fitted = 0, near_mean = 0;
    float got[2] = { 0.0f, 0.0f };
    for (int trainer = 0; trainer < 2; ++trainer) {
      iris *k = iris_init(A, sizeof A, 2, 12, 2, 64, 3u);
      float in[2] = { 500.0f, 0.0f };
      for (int r = 0; r < 6; ++r) {
        float out[2] = { (float)r, 1.0f - 0.5f * (float)r };
        iris_record(k, in, out);
      }
      if (trainer == 0) iris_train(k); else iris_train_elm(k, 1e-3f, SCR, sizeof SCR);
      if (iris_get_status(k) == IRIS_STATUS_OK) healthy++;
      if (iris_is_trained(k)) fitted++;
      float o[2];
      iris_predict(k, in, o);
      got[trainer] = o[0];
      if (iris_internal_absf(o[0] - 2.5f) < 0.25f && iris_internal_absf(o[1] + 0.25f) < 0.125f) near_mean++;
    }
    snprintf(d, sizeof d, "healthy %d of 2, trained %d of 2, near the mean %d of 2 "
             "(%.3f and %.3f, mean 2.5)", healthy, fitted, near_mean,
             (double)got[0], (double)got[1]);
    check("with every input still, both trainers fit without a fault",
          healthy == 2 && fitted == 2 && near_mean == 2, d); }

  /* ---- an unfitted instrument plays the centre of what it was shown -------
     Recorded but never trained: every output is the centre of the range its
     demonstrations covered, exactly, and the status says IRIS_NOT_FITTED.
     With nothing recorded it is 0. The answer must not depend on stale
     ranges: not on the 0..1 an instrument starts with, not on what a
     neighbour function fitted, not on what the store held before a clear. */
  { iris *k = iris_init(A, sizeof A, 2, 12, 2, 64, 5u);
    float q[2] = { 0.3f, 0.8f }, o[2] = { -7.0f, -7.0f };
    iris_predict(k, q, o);
    const int empty_ok = o[0] == 0.0f && o[1] == 0.0f
                      && iris_get_status(k) == IRIS_NOT_FITTED;
    const float outs[5][2] = { { 120, -3 }, { 100, 0 }, { 200, 7 }, { 150, 1 }, { 180, 2 } };
    for (int i = 0; i < 5; ++i) {
      float in[2] = { (float)i, (float)(i * i) }, out[2] = { outs[i][0], outs[i][1] };
      iris_record(k, in, out);
    }
    const size_t n0 = iris_save(k, FILE_BUF, sizeof FILE_BUF);
    static unsigned char before[8192];
    memcpy(before, FILE_BUF, n0);
    int centre = 0;
    for (int j = 0; j < 3; ++j) {
      float in[2] = { (float)j * 7.0f, -1.0f };
      iris_predict(k, in, o);
      if (o[0] == 150.0f && o[1] == 2.0f && iris_get_status(k) == IRIS_NOT_FITTED) centre++;
    }
    const size_t n1 = iris_save(k, FILE_BUF, sizeof FILE_BUF);
    const int bytes_same = n0 == n1 && memcmp(before, FILE_BUF, n0) == 0;
    float kn[2];
    iris_knn_predict(k, q, kn, 3);                  /* fits the ranges */
    iris_predict(k, q, o);
    const int after_knn = o[0] == 150.0f && o[1] == 2.0f;
    iris_clear(k);
    { float in[2] = { 1.0f, 1.0f }, out[2] = { 175.0f, 0.5f }; iris_record(k, in, out); }
    iris_predict(k, q, o);
    const int after_clear = o[0] == 175.0f && o[1] == 0.5f;
    iris_clear(k);
    for (int i = 0; i < 4; ++i) {
      float in[2] = { (float)i, 0.0f }, out[2] = { 100.0f, -0.25f };
      iris_record(k, in, out);
    }
    iris_predict(k, q, o);
    const int constant = o[0] == 100.0f && o[1] == -0.25f;
    snprintf(d, sizeof d, "empty %d, centre %d of 3, save unchanged %d, after k-NN %d, "
             "after clear %d, constant %d", empty_ok, centre, bytes_same, after_knn,
             after_clear, constant);
    check("an unfitted instrument plays the centre of its demonstrations",
          empty_ok && centre == 3 && bytes_same && after_knn && after_clear && constant, d); }

  /* ---- an edited instrument keeps playing ---------------------------------
     A take recorded after training makes the fit stale but must not silence
     the instrument: it keeps playing its network, status healthy. */
  { iris *k = still_instrument(A, sizeof A, 500.0f);
    iris_train(k);
    float q[2] = { 0.37f, 500.0f }, p0[2], p1[2];
    iris_predict(k, q, p0);
    { float in[2] = { 0.5f, 500.0f }, out[2] = { 30.0f, 0.0f }; iris_record(k, in, out); }
    iris_predict(k, q, p1);
    const int same = memcmp(p0, p1, sizeof p0) == 0;
    snprintf(d, sizeof d, "is_trained %d, prediction unchanged %d, status %d",
             iris_is_trained(k), same, (int)iris_get_status(k));
    check("a stale fit keeps playing its network",
          !iris_is_trained(k) && same && iris_get_status(k) == IRIS_STATUS_OK, d); }

  /* ---- the neighbour functions with nothing to compare against -----------
     No stale values left in `out`: 0, as iris_predict plays with nothing
     recorded, and -1 from the classifier. */
  { iris *k = iris_init(A, sizeof A, 2, 12, 2, 64, 5u);
    float q[2] = { 0.3f, 0.8f }, a[2] = { 7.0f, 7.0f }, b[2] = { 7.0f, 7.0f };
    iris_knn_predict(k, q, a, 3);
    const int id = iris_classify_1nn(k, q, b);
    snprintf(d, sizeof d, "k-NN %.1f %.1f, 1-NN %.1f %.1f id %d",
             (double)a[0], (double)a[1], (double)b[0], (double)b[1], id);
    check("an empty store gives 0 from both neighbour functions",
          a[0] == 0.0f && a[1] == 0.0f && b[0] == 0.0f && b[1] == 0.0f && id == -1, d); }

  /* ---- iris_delete_nearest measures in normalised units -------------------
     Two takes, (500 mm, -2 g) and (510 mm, +2 g), the hand at (506 mm, -2 g):
     the first take is nearest in fractions of each range (0.36 against
     1.16), though the second is nearer in raw units (32 against 36). */
  { iris *k = iris_init(A, sizeof A, 2, 12, 2, 64, 1u);
    float a_in[2] = { 500.0f, -2.0f }, b_in[2] = { 510.0f, 2.0f }, o[2] = { 0.0f, 0.0f };
    const int id_a = iris_record(k, a_in, o);
    iris_record(k, b_in, o);
    float hand[2] = { 506.0f, -2.0f };
    const int named = iris_classify_1nn(k, hand, 0);
    const int deleted = iris_delete_nearest(k, hand);
    const int a_gone = iris_index_of(k, id_a) < 0;
    snprintf(d, sizeof d, "1-NN names id %d, delete returned %d, id %d deleted %d",
             named, deleted, id_a, a_gone);
    check("delete_nearest deletes the take the classifier names",
          named == id_a && deleted == 1 && a_gone && iris_count(k) == 1, d); }

  /* The same property over random stores: scaling one input by 1000 (in the
     demonstrations and in the hand) must not change which take is deleted,
     and it must be the one iris_classify_1nn names. */
  { int differ = 0, not_named = 0, trials = 0;
    lcg_state = 4242u;
    for (int t = 0; t < 300; ++t) {
      float ins[12][2], outs[12][2];
      for (int r = 0; r < 12; ++r) {
        ins[r][0] = lcg01(); ins[r][1] = lcg01() * 4.0f - 2.0f;
        outs[r][0] = (float)r; outs[r][1] = 0.0f;
      }
      float hand[2] = { lcg01() * 1.2f - 0.1f, lcg01() * 4.4f - 2.2f };
      float hand_s[2] = { hand[0] * 1000.0f, hand[1] };
      iris *x = iris_init(A, sizeof A, 2, 12, 2, 64, 1u);
      iris *y = iris_init(B, sizeof B, 2, 12, 2, 64, 1u);
      for (int r = 0; r < 12; ++r) {
        float s_in[2] = { ins[r][0] * 1000.0f, ins[r][1] };
        iris_record(x, ins[r], outs[r]);
        iris_record(y, s_in, outs[r]);
      }
      const int named = iris_classify_1nn(x, hand, 0);
      iris_delete_nearest(x, hand);
      iris_delete_nearest(y, hand_s);
      int gone_x = -1, gone_y = -1;
      for (int id = 1; id <= 12; ++id) {
        if (iris_index_of(x, id) < 0) gone_x = id;
        if (iris_index_of(y, id) < 0) gone_y = id;
      }
      if (gone_x != gone_y) differ++;
      if (gone_x != named) not_named++;
      trials++;
    }
    snprintf(d, sizeof d, "%d trials: scaled store deleted a different take %d, "
             "deleted take not the classifier's %d", trials, differ, not_named);
    check("delete_nearest ignores the units an input is measured in",
          differ == 0 && not_named == 0, d); }

  /* ---- a finite query far outside still finds its neighbours --------------
     With ranges 1 wide, a hand 1e16 away has a squared distance near 1e32:
     finite, and far past any big round number a search might start from.
     All three neighbour paths must answer it normally, and a not-a-number
     must still find nothing. */
  { iris *k = iris_init(A, sizeof A, 2, 12, 2, 64, 1u);
    for (int r = 0; r < 4; ++r) {
      float in[2] = { (float)(r & 1), (float)(r >> 1) }, out[2] = { (float)r, 1.0f };
      iris_record(k, in, out);
    }
    float far[2] = { 1e16f, 0.0f }, o[2];
    k->status = IRIS_STATUS_OK;
    const int id = iris_classify_1nn(k, far, o);
    const int nn_ok = id > 0 && iris_get_status(k) == IRIS_STATUS_OK;
    k->status = IRIS_STATUS_OK;
    iris_knn_predict(k, far, o, 2);
    const int knn_ok = iris_get_status(k) == IRIS_STATUS_OK && !iris_internal_isbad(o[0]);
    const int del = iris_delete_nearest(k, far);
    float nanq[2] = { __builtin_nanf(""), 0.0f };
    const int del_nan = iris_delete_nearest(k, nanq);
    snprintf(d, sizeof d, "1-NN id %d ok %d, k-NN ok %d, delete far %d, delete NaN %d, "
             "count %d", id, nn_ok, knn_ok, del, del_nan, iris_count(k));
    check("a finite query far outside still finds its nearest",
          nn_ok && knn_ok && del == 1 && del_nan == 0 && iris_count(k) == 3, d); }

  /* ---- a reading that is not finite changes nothing but the status --------
     On an instrument that has never been fitted, the neighbour functions fit
     its ranges before they measure -- but a not-a-number or an infinity has
     no nearest demonstration, so it is refused first: every byte but the
     status stays as it was, and the status says IRIS_NAN_TRAPPED. k-NN and
     1-NN write the centre of the demonstrations; the delete deletes nothing. */
  { static unsigned char snap[sizeof A];
    iris *k = iris_init(A, sizeof A, 2, 12, 2, 64, 1u);
    for (int r = 0; r < 4; ++r) {
      float in[2] = { 10.0f * (float)(r & 1), 5.0f + (float)(r >> 1) }, out[2] = { (float)r, 1.0f + (float)r };
      iris_record(k, in, out);
    }
    const size_t at = (size_t)((unsigned char *)&k->status - A), n = sizeof k->status;
    const float bad[3][2] = { { __builtin_nanf(""), 5.0f }, { 3.0f, __builtin_inff() },
                              { -__builtin_inff(), 5.5f } };
    int wrong = 0;
    for (int q = 0; q < 3; ++q)
      for (int path = 0; path < 3; ++path) {
        float o[2] = { 7.0f, 7.0f };
        k->status = IRIS_STATUS_OK;
        memcpy(snap, A, sizeof A);
        int ret = 0;
        if (path == 0) iris_knn_predict(k, bad[q], o, 3);
        if (path == 1) ret = iris_classify_1nn(k, bad[q], o) != -1;
        if (path == 2) ret = iris_delete_nearest(k, bad[q]) != 0;
        const int same = memcmp(A, snap, at) == 0
                      && memcmp(A + at + n, snap + at + n, sizeof A - at - n) == 0;
        const int centre = path == 2 || (o[0] == 1.5f && o[1] == 2.5f);
        if (ret || !same || !centre || iris_get_status(k) != IRIS_NAN_TRAPPED) wrong++;
        memcpy(A, snap, sizeof A);
      }
    snprintf(d, sizeof d, "%d of 9 refusals wrong; still never fitted %d", wrong, !k->fitted);
    check("a reading that is not finite changes only the status", wrong == 0 && !k->fitted, d); }

  /* ---- scaling every input or every output by two -------------------------
     See scaled_store above. Five playing paths, both properties. */
  { const char *names[5] = { "unfitted", "iris_train", "iris_train_elm", "k-NN", "1-NN" };
    int in_broken = 0, out_broken = 0;
    char which_in[80] = "", which_out[80] = "";
    for (int path = 0; path < 5; ++path) {
      const int bi = scaling_breaks(path, 0), bo = scaling_breaks(path, 1);
      in_broken += bi; out_broken += bo;
      if (bi) { strncat(which_in, " ", sizeof which_in - strlen(which_in) - 1);
                strncat(which_in, names[path], sizeof which_in - strlen(which_in) - 1); }
      if (bo) { strncat(which_out, " ", sizeof which_out - strlen(which_out) - 1);
                strncat(which_out, names[path], sizeof which_out - strlen(which_out) - 1); }
    }
    snprintf(d, sizeof d, "inputs x2: %d probes differ%s; outputs x2: %d not doubled%s",
             in_broken, which_in, out_broken, which_out);
    check("doubling inputs changes nothing, doubling outputs doubles",
          in_broken == 0 && out_broken == 0, d); }

  /* ---- novelty before the first fit: the demonstrated ranges ------------
     One input demonstrated at 0 and 1000. A reading of 10 is 0.02 of the
     way across the normalised range [-1,+1] from the nearest take, which
     novelty divides by sqrt(1)/2: 0.04, before training and after it, to the
     bit. Measured in the 0..1 ranges iris_init starts with it would read 1.
     A reading that is not finite reads 1 and is answered before any
     fitting, so it leaves every byte of the arena as it was. */
  { static unsigned char M[IRIS_ARENA(1, 8, 1, 8)], snap[sizeof M];
    iris *k = iris_init(M, sizeof M, 1, 8, 1, 8, 1u);
    const float a = 0.0f, b = 1000.0f, o0 = 0.0f, o1 = 1.0f, q = 10.0f;
    const float nan_q = __builtin_nanf("");
    iris_record(k, &a, &o0);
    iris_record(k, &b, &o1);
    memcpy(snap, M, sizeof M);
    const float n_bad = iris_novelty(k, &nan_q);
    const int untouched = memcmp(snap, M, sizeof M) == 0;
    const float before = iris_novelty(k, &q);
    const int ranges = k->in_lo[0] == 0.0f && k->in_hi[0] == 1000.0f;
    iris_train(k);
    const float after = iris_novelty(k, &q);
    snprintf(d, sizeof d, "never fitted %.4f, trained %.4f; ranges fitted %d; "
             "not finite %.1f, arena untouched %d",
             (double)before, (double)after, ranges, (double)n_bad, untouched);
    check("novelty before the first fit uses the demonstrated ranges",
          before == after && before > 0.039f && before < 0.041f && ranges
          && n_bad == 1.0f && untouched, d); }

  /* ---- the output floor: max(1e-5 * |lo|, 1e-6) -------------------------
     An output shown one value keeps a width, relative to that value with an
     absolute floor near zero, exactly as PART 5 states it. The ranges are
     fitted by the first neighbour call on a never-fitted instrument. At the
     largest float the width goes below the value instead, since above it
     would be infinity. */
  { const float los[6] = { 500.0f, 0.0f, -2e7f, 3e-3f, 1e30f, 3.4028235e38f };
    int wrong = 0; char first[120] = "";
    for (int c = 0; c < 6; ++c) {
      static unsigned char M[IRIS_ARENA(1, 8, 1, 8)];
      iris *k = iris_init(M, sizeof M, 1, 8, 1, 8, 3u);
      for (int i = 0; i < 3; ++i) { const float in = (float)i, out = los[c]; iris_record(k, &in, &out); }
      float o, q = 1.0f;
      iris_knn_predict(k, &q, &o, 2);
      float w = (los[c] < 0.0f ? -los[c] : los[c]) * 1e-5f;
      if (w < 1e-6f) w = 1e-6f;
      const float want_lo = c == 5 ? los[c] - w : los[c], want_hi = c == 5 ? los[c] : los[c] + w;
      if (k->out_lo[0] != want_lo || k->out_hi[0] != want_hi) {
        wrong++;
        if (!first[0]) snprintf(first, sizeof first, " -- at %g: %.9g to %.9g, want %.9g to %.9g",
                                (double)los[c], (double)k->out_lo[0], (double)k->out_hi[0],
                                (double)want_lo, (double)want_hi);
      }
    }
    snprintf(d, sizeof d, "%d of 6 constant outputs (500, 0, -2e7, 3e-3, 1e30, the largest "
             "float) off the floor%s", wrong, first);
    check("an output that never moved gets max(1e-5*|lo|, 1e-6)", wrong == 0, d); }

  /* ---- an output held at the largest float plays and saves ---------------
     Four demonstrations whose second output is always the largest float,
     fitted by each trainer. Its range must stay finite, so that a sweep of
     readings plays only finite numbers inside the range, and the instrument
     must save and load and play the same bits. With the range widened
     upward, hi is infinity: the sweep plays infinity and iris_save refuses
     the instrument. */
  { int wrong = 0; char first[200] = "";
    for (int t = 0; t < 2; ++t) {
      static unsigned char M[IRIS_ARENA(1, 12, 2, 8)], L[IRIS_ARENA(1, 12, 2, 8)];
      static unsigned char S[IRIS_ELM_SCRATCH(12, 2)], F[1024];
      iris *k = iris_init(M, sizeof M, 1, 12, 2, 8, 5u);
      for (int i = 0; i < 4; ++i) {
        const float in = (float)i, out[2] = { (float)(i * i), 3.4028235e38f };
        iris_record(k, &in, out);
      }
      const int fit = t == 0 ? iris_train(k) == 1 : iris_train_elm(k, 1e-4f, S, sizeof S) >= 0;
      int bad = 0;
      for (int i = -200; i <= 500; ++i) {
        const float q = (float)i / 100.0f;
        float o[2];
        iris_predict(k, &q, o);
        if (!(o[1] >= k->out_lo[1] && o[1] <= k->out_hi[1] && o[1] <= 3.4028235e38f)) bad++;
      }
      const size_t n = iris_save(k, F, sizeof F);
      iris *l = iris_init(L, sizeof L, 1, 12, 2, 8, 9u);
      const int loaded = n > 0 && iris_load(l, F, n);
      int same = loaded;
      for (int i = -20; loaded && i <= 50; ++i) {
        const float q = (float)i / 10.0f;
        float a[2], b[2];
        iris_predict(k, &q, a);
        iris_predict(l, &q, b);
        same &= memcmp(a, b, sizeof a) == 0;
      }
      const int ok = fit && bad == 0 && n == iris_save_size(k) && same;
      wrong += !ok;
      if (!ok && !first[0])
        snprintf(first, sizeof first, " -- %s: fitted %d, range %g to %g, %d of 701 readings "
                 "not finite or outside it, saved %d bytes, loads and plays the same %d",
                 t ? "closed form" : "iris_train", fit, (double)k->out_lo[1],
                 (double)k->out_hi[1], bad, (int)n, same);
    }
    snprintf(d, sizeof d, "%d of 2 trainers left an instrument that plays infinity or will "
             "not save%s", wrong, first);
    check("an output at the largest float plays finite numbers and saves", wrong == 0, d); }

  /* ---- the playing functions take a non-const instrument -----------------
     The pointers above only compile against the non-const signatures; this
     plays once through each so the check is also exercised at run time. */
  { iris *k = still_instrument(A, sizeof A, 500.0f);
    float in[2] = { 0.5f, 500.0f }, o[2];
    iris_train(k);
    play_net(k, in, o);
    play_knn(k, in, o, 3);
    const int id = play_1nn(k, in, o);
    const float nov = play_nov(k, in);
    snprintf(d, sizeof d, "compiled against iris *; 1-NN answered id %d, novelty %.3f",
             id, (double)nov);
    check("the playing functions and iris_novelty take iris *",
          id > 0 && nov >= 0.0f && nov <= 1.0f, d); }

  printf("\n  %s (%d failed)\n\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails);
  return fails ? 1 : 0;
}
