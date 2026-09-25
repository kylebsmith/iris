/* SPDX-License-Identifier: BSD-3-Clause
   Copyright (c) 2026 Kyle Smith */
/* ============================================================================
   playing.c -- the playing paths and the two neighbour functions.

   Each check states the behaviour it holds and was watched to fail with that
   behaviour broken in a scratch copy of iris.h.

   Build and run from the repository root:

     mkdir -p build && cc -std=c99 -O2 -Wall -Wextra -I. -o build/playing tests/playing.c && ./build/playing

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
   take a non-const one. These three lines stop compiling if a signature
   goes back to const iris *. */
static void (*const play_net)(iris *, const float *, float *) = iris_predict;
static void (*const play_knn)(iris *, const float *, float *, int) = iris_knn_predict;
static int  (*const play_1nn)(iris *, const float *, float *) = iris_classify_1nn;

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

int main(void) {
  char d[200];
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
     healthy status -- nothing here is broken, the numbers are just large. */
  { iris *k = iris_init(A, sizeof A, 2, 12, 2, 64, 1);
    float a_in[2] = { 0.0f, 0.0f }, a_out[2] = { 3e38f, 1.0f };
    float b_in[2] = { 1.0f, 1.0f }, b_out[2] = { -3e38f, 2.0f };
    iris_record(k, a_in, a_out); iris_record(k, b_in, b_out);
    int bad = 0;
    for (int q = 0; q <= 20; ++q) {
      float in[2] = { q / 20.0f, q / 20.0f }, o[2];
      iris_knn_predict(k, in, o, 2);
      if (iris_isbad(o[0]) || o[0] > 3e38f || o[0] < -3e38f) bad++;
    }
    snprintf(d, sizeof d, "%d of 21 answers not finite or outside, status %d",
             bad, (int)iris_get_status(k));
    check("k-NN: outputs near the largest float blend without overflow",
          bad == 0 && iris_get_status(k) == IRIS_STATUS_OK, d); }

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
    for (int j = 0; j < 4; ++j) if (iris_norm_in(k, 1, probe[j]) != 0.0f) nonzero++;
    for (int h = 0; h < 12; ++h) if (k->w1[h * 2 + 1] != w0[h]) touched++;
    for (int i = 0; i < 24; ++i) if (!(iris_absf(k->w1[i]) < 4.0f)) large++;
    snprintf(d, sizeof d, "normalised non-zero %d of 4, still weights moved %d of 12, "
             "|w1| >= 4: %d, width %g, status %d", nonzero, touched, large,
             (double)(k->in_hi[1] - k->in_lo[1]), (int)iris_get_status(k));
    check("a still input normalises to 0 and draws no gradient",
          nonzero == 0 && touched == 0 && large == 0
          && k->in_hi[1] == k->in_lo[1] && iris_get_status(k) == IRIS_STATUS_OK, d); }

  /* ---- the rule's threshold, from both sides -----------------------------
     Still means a width of at most 1e-5 of the magnitude, or at most 1e-6.
     Just inside that must be ignored; twice as wide must count. */
  { struct { float lo, hi; int still; } c[6] = {
      { 500.0f, 500.004f, 1 },   /* width 0.004 <= 0.005: still        */
      { 500.0f, 500.01f,  0 },   /* width 0.01: moved                   */
      { 0.0f,   5e-7f,    1 },   /* width 5e-7 <= 1e-6: still           */
      { 0.0f,   2e-6f,    0 },   /* width 2e-6: moved                   */
      { -4095.0f, -4094.99f, 1 },/* width 0.01 <= 0.04095: still        */
      { 4095.0f, 4095.0f, 1 } }; /* exactly constant                    */
    int wrong = 0;
    for (int j = 0; j < 6; ++j) {
      iris *k = iris_init(A, sizeof A, 2, 12, 2, 64, 1u);
      float a[2] = { 0.0f, c[j].lo }, b[2] = { 1.0f, c[j].hi }, o[2] = { 0.0f, 1.0f };
      iris_record(k, a, o); iris_record(k, b, o);
      iris_fit_ranges(k);
      const int is_still = (k->in_hi[1] == k->in_lo[1]);
      if (is_still != c[j].still) wrong++;
    }
    snprintf(d, sizeof d, "%d of 6 widths classified wrongly", wrong);
    check("a width inside the threshold is still, twice it is not", wrong == 0, d); }

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
      if (iris_isbad(o[0]) || iris_isbad(o[1])) finite = 0;
      k->status = IRIS_STATUS_OK;
      iris_knn_predict(k, in, o, 3);
      if (iris_get_status(k) == IRIS_NAN_TRAPPED) reported++;
      if (iris_isbad(o[0]) || iris_isbad(o[1])) finite = 0;
      k->status = IRIS_STATUS_OK;
      iris_classify_1nn(k, in, o);
      if (iris_get_status(k) == IRIS_NAN_TRAPPED) reported++;
      if (iris_isbad(o[0]) || iris_isbad(o[1])) finite = 0;
    }
    k->status = IRIS_STATUS_OK;
    snprintf(d, sizeof d, "%d of 6 reported, outputs finite %d", reported, finite);
    check("a not-a-number on a still input is still reported", reported == 6 && finite, d); }

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

  /* ---- the playing functions take a non-const instrument -----------------
     The pointers above only compile against the non-const signatures; this
     plays once through each so the check is also exercised at run time. */
  { iris *k = still_instrument(A, sizeof A, 500.0f);
    float in[2] = { 0.5f, 500.0f }, o[2];
    iris_train(k);
    play_net(k, in, o);
    play_knn(k, in, o, 3);
    const int id = play_1nn(k, in, o);
    snprintf(d, sizeof d, "compiled against iris *; 1-NN answered id %d", id);
    check("iris_predict, iris_knn_predict, iris_classify_1nn take iris *", id > 0, d); }

  printf("\n  %s (%d failed)\n\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails);
  return fails ? 1 : 0;
}
