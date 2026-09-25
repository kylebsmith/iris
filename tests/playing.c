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

static unsigned char A[IRIS_ARENA(2, 12, 2, 64)];

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

  printf("\n  %s (%d failed)\n\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails);
  return fails ? 1 : 0;
}
