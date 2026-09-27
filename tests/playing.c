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
   take a non-const one, and so do iris_novelty and iris_nearest, which fit
   those ranges too. These five lines stop compiling if a signature takes
   const iris *. */
static void  (*const play_net)(iris *, const float *, float *) = iris_predict;
static void  (*const play_knn)(iris *, const float *, float *, int) = iris_knn_predict;
static int   (*const play_1nn)(iris *, const float *, float *) = iris_classify_1nn;
static float (*const play_nov)(iris *, const float *) = iris_novelty;
static int   (*const play_near)(iris *, const float *, int *, float *, int) = iris_nearest;

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

/* The scaling properties. iris fits its own ranges and does all its work in
   fractions of them, so the units you measure in cannot matter. Doubling a
   binary32 value is exact (it only changes the exponent), so the property can
   be held to the bit: double every input, in the demonstrations and in the
   query, and every playing path must give bit-identical answers; double every
   output and every answer must be exactly double. `path` picks what plays:
   0 unfitted, 1 iris_train, 2 iris_train_elm, 3 the k-nearest-neighbour
   blend (k-NN), 4 the single nearest (1-NN). Returns how
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

/* ---- random stores for iris_nearest -----------------------------------------
   Built to meet what the ranking must get right. Values sit on a coarse grid
   (one to four steps across 0..1), so exact ties between takes, and readings
   exactly on a take, are common. Sometimes one input never moves. Half the
   stores are fitted partway (the closed-form trainer, which fits the ranges)
   and then given more takes, some outside the fitted ranges and some at 1e20,
   so far outside that their squared distance overflows. Sometimes a take is
   deleted, so positions and identifiers part. With one_hot, output o of the
   take recorded o-th is 1 and every other output 0, so iris_knn_predict's
   answer shows which takes it blended: output o is non-zero exactly when the
   take with identifier o + 1 is among its neighbours. */
static unsigned char NB[IRIS_ARENA(4, 8, 16, 64)];
static unsigned char NB_SNAP[sizeof NB], NB_AFTER[sizeof NB];
static unsigned char NSCR[IRIS_ELM_SCRATCH(8, 16)];
static int nb_steps;
static float nb_grid(void) { return (float)(int)(lcg01() * (float)(nb_steps + 1)) / (float)nb_steps; }
static iris *nb_store(int n_in, int n_out, int n_ex, int one_hot) {
  iris *k = iris_init(NB, sizeof NB, n_in, 8, n_out, 64, 1u);
  nb_steps = 1 + (int)(lcg01() * 4.0f);
  const int still = lcg01() < 0.3f ? (int)(lcg01() * (float)n_in) : -1;
  const int fit_at = lcg01() < 0.5f ? 1 + (int)(lcg01() * (float)n_ex) : -1;
  for (int r = 0; r < n_ex; ++r) {
    if (r == fit_at) iris_train_elm(k, 1e-3f, NSCR, sizeof NSCR);
    float in[4], out[16];
    for (int i = 0; i < n_in; ++i) {
      in[i] = i == still ? 0.5f : nb_grid();
      if (fit_at >= 0 && r >= fit_at && lcg01() < 0.3f) in[i] = 3.0f * in[i] - 1.0f;
    }
    if (fit_at >= 0 && r >= fit_at && lcg01() < 0.15f) in[0] = 1e20f;
    for (int o = 0; o < n_out; ++o) out[o] = one_hot ? (o == r ? 1.0f : 0.0f) : nb_grid();
    iris_record(k, in, out);
  }
  if (n_ex > 1 && lcg01() < 0.3f) iris_delete_index(k, (int)(lcg01() * (float)(n_ex - 1)));
  return k;
}
/* A reading: on the grid, halfway between grid points, outside 0..1, or
   exactly on a stored take. */
static void nb_reading(iris *k, float *in) {
  const float pick = lcg01();
  if (pick < 0.25f && iris_count(k) > 0) {
    float out[16];
    iris_get(k, (int)(lcg01() * (float)iris_count(k)), in, out);
    return;
  }
  for (int i = 0; i < k->n_in; ++i)
    in[i] = pick < 0.6f ? nb_grid() : pick < 0.85f ? nb_grid() + 0.5f / (float)nb_steps
                                                   : 3.0f * nb_grid() - 1.0f;
}
/* The distance iris_nearest documents, in double precision: the straight-line
   distance with every input counted in fractions of the width of the range the
   instrument measures in, an input of zero width counted not at all. Returns
   the square. */
static double nb_distance2(const iris *k, const float *row, const float *in) {
  double s = 0.0;
  for (int i = 0; i < k->n_in; ++i) {
    const float w = k->in_hi[i] - k->in_lo[i];
    if (w <= 0.0f) continue;
    const double t = ((double)row[i] - (double)in[i]) / (double)w;
    s += t * t;
  }
  return s;
}

/* One list from iris_nearest, every take asked for, held to what its comment
   promises. Each distance must equal the distance in the documented unit,
   computed in double precision from the ranges the instrument measures in
   (nb_distance2), to within 1e-5 of it; the distances must never fall; every
   identifier must come once; the count must be the takes at a finite
   distance, or every take when none is; and a distance may be infinity only
   in that case, for a take 10^15 ranges or more away. And the answer for
   every smaller n must be the start of this one,
   bit for bit: each pass of IRIS_KNN_MAXK takes carries on where the last
   stopped. c counts lists, values off, out of order, wrong counts, wrong
   prefixes and lists at no finite distance. */
static double nb_worst = 0.0;
static void nb_list_check(iris *k, const float *in, long *c) {
  int ids[64], ids2[64]; float dists[64], dists2[64];
  const int stride = k->n_in + k->n_out;
  const int got = iris_nearest(k, in, ids, dists, 64);
  int finite = 0;
  c[0]++;
  for (int r = 0; r < k->n_ex; ++r)
    if (nb_distance2(k, k->ex + (size_t)r * stride, in) < 3.4e38) finite++;
  if (got != (finite > 0 ? finite : k->n_ex)) c[3]++;
  if (finite == 0) c[5]++;
  for (int j = 0; j < got; ++j) {
    const int at = iris_index_of(k, ids[j]);
    const double ref2 = at < 0 ? -1.0 : nb_distance2(k, k->ex + (size_t)at * stride, in);
    if (dists[j] > 3.4e38f) {
      if (finite > 0 || ref2 < 0.99e30) c[1]++;
    } else {
      const double ref = ref2 >= 0.0 ? __builtin_sqrt(ref2) : -1.0;
      const double err = __builtin_fabs((double)dists[j] - ref);
      if (err > nb_worst) nb_worst = err;
      if (ref < 0.0 || err > 1e-5 * ref + 1e-12) c[1]++;
    }
    if (j > 0 && dists[j] < dists[j - 1]) c[2]++;
    for (int m = 0; m < j; ++m) if (ids[m] == ids[j]) c[2]++;
  }
  for (int m = 1; m <= got; ++m)
    if (iris_nearest(k, in, ids2, dists2, m) != m
        || memcmp(ids, ids2, sizeof(int) * (size_t)m)
        || memcmp(dists, dists2, sizeof(float) * (size_t)m)) c[4]++;
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

  /* ---- the same, with the nearest far take in the first row ---------------
     The takes of the check above in the other order: A recorded first, so
     after the deletes it is row 0. The rescan with the far distance must
     start at row 0, or it never sees the nearest take and names B. */
  { static unsigned char M[IRIS_ARENA(3, 8, 1, 8)];
    iris *k = iris_init(M, sizeof M, 3, 8, 1, 8, 1u);
    float a[3] = { 0.0f, 0.0f, 7.0f }, ya = 0.0f, b[3] = { 1.0f, 1.0f, 7.0f }, yb = 1.0f;
    float fb[3] = { 1e20f, 1e20f, 3e38f }, y_b = 0.75f;
    float fa[3] = { 1e21f, 0.5f, 3e38f }, y_a = 0.25f;
    iris_record(k, a, &ya); iris_record(k, b, &yb);
    iris_train(k);
    iris_record(k, fa, &y_a); iris_record(k, fb, &y_b);   /* identifiers 3 and 4 */
    iris_delete_index(k, 0); iris_delete_index(k, 0);
    const float q[3] = { 0.5f, 0.5f, -3e38f };
    float o = -1.0f, c = -1.0f;
    iris_knn_predict(k, q, &o, 2);
    const int knn_ok = o > 0.25f && o < 0.5f && iris_get_status(k) == IRIS_STATUS_OK;
    const int id = iris_classify_1nn(k, q, &c);
    const int one_ok = id == 3 && c == 0.25f && iris_get_status(k) == IRIS_STATUS_OK;
    const int deleted = iris_delete_nearest(k, q);
    const int del_ok = deleted == 1 && iris_count(k) == 1 && iris_id_at(k, 0) == 4;
    snprintf(d, sizeof d, "k-NN %g (status %d), 1-NN id %d playing %g, delete_nearest %d "
             "leaving id %d", (double)o, (int)iris_get_status(k), id, (double)c, deleted,
             iris_id_at(k, 0));
    check("the far rescan starts at the first row", knn_ok && one_ok && del_ok, d); }

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

  /* ---- a poisoned store on an instrument never fitted -------------------
     Never trained, so the substitute a refusal plays is the centre of the
     demonstrated output range, read from the store. With a not-a-number
     written into one take's output, that centre is not a number, and the
     substitute must be 0 instead, from iris_predict, iris_knn_predict and
     iris_classify_1nn alike (the neighbour functions refuse because their
     own fitting of the ranges meets the poison). */
  { static unsigned char M[IRIS_ARENA(2, 8, 1, 8)];
    iris *k = iris_init(M, sizeof M, 2, 8, 1, 8, 1u);
    float a[2] = { 0.0f, 0.0f }, ya = 2.0f, b[2] = { 1.0f, 1.0f }, yb = 4.0f;
    iris_record(k, a, &ya); iris_record(k, b, &yb);
    k->ex[2] = __builtin_nanf("");
    const float q[2] = { 0.5f, 0.5f };
    float p = -1.0f, o = -1.0f, c = -1.0f;
    iris_predict(k, q, &p);
    iris_knn_predict(k, q, &o, 2);
    iris_classify_1nn(k, q, &c);
    snprintf(d, sizeof d, "predict %g, k-NN %g, 1-NN %g", (double)p, (double)o, (double)c);
    check("a poisoned store never fitted plays 0, not the poison",
          p == 0.0f && o == o && c == c && (o == 0.0f || (o >= 2.0f && o <= 4.0f))
          && (c == 0.0f || c == 2.0f || c == 4.0f), d); }

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

  /* ---- iris_nearest: the classifier's take, and nothing else written ------
     Over random stores (see nb_store), some empty, some with a not-a-number
     written into one take or into every take, and random readings, some not
     finite: iris_nearest must find a take exactly when iris_classify_1nn
     does, its first must be the one the classifier names, and the arena
     after it must equal, byte for byte, the arena after iris_classify_1nn
     with a null `out`: the same ranges fitted, the same status, nothing
     else. */
  { long trials = 0, wrong_id = 0, wrong_mem = 0, refused = 0;
    lcg_state = 4242u;
    for (int s = 0; s < 400; ++s) {
      iris *k = nb_store(1 + (int)(lcg01() * 4.0f), 2, (int)(lcg01() * 40.0f), 0);
      if (s % 10 == 3 && k->n_ex > 0) k->ex[0] = __builtin_nanf("");
      if (s % 10 == 7)
        for (int r = 0; r < k->n_ex; ++r) k->ex[(size_t)r * (k->n_in + k->n_out)] = __builtin_nanf("");
      for (int q = 0; q < 20; ++q) {
        float in[4];
        nb_reading(k, in);
        if (q == 7) in[(int)(lcg01() * (float)k->n_in)] = q & 1 ? __builtin_inff() : __builtin_nanf("");
        int ids[24]; float dists[24];
        const int n = 1 + (int)(lcg01() * 20.0f);
        k->status = IRIS_STATUS_OK;
        memcpy(NB_SNAP, NB, sizeof NB);
        const int named = iris_classify_1nn(k, in, 0);
        memcpy(NB_AFTER, NB, sizeof NB);
        memcpy(NB, NB_SNAP, sizeof NB);
        const int got = iris_nearest(k, in, ids, dists, n);
        trials++;
        if (got == 0) refused++;
        if ((named < 0) != (got == 0) || (got > 0 && ids[0] != named)) wrong_id++;
        if (memcmp(NB, NB_AFTER, sizeof NB) != 0) wrong_mem++;
      }
    }
    snprintf(d, sizeof d, "%ld queries (%ld found nothing): %ld named another take, "
             "%ld left another arena", trials, refused, wrong_id, wrong_mem);
    check("iris_nearest names the classifier's take and writes what it writes",
          wrong_id == 0 && wrong_mem == 0 && refused > 0 && refused < trials, d); }

  /* ---- iris_nearest: the first kk are the takes k-NN blends -----------------
     One-hot stores (see nb_store), so iris_knn_predict's answer names the
     takes it blended. For every kk from 1 to IRIS_KNN_MAXK, the first kk
     identifiers iris_nearest returns must be exactly those takes, ties at the
     edge of the neighbourhood included, and when k-NN blends fewer than kk
     (takes whose distance overflows are left out) iris_nearest must return
     that many. */
  { long trials = 0, wrong = 0, short_lists = 0;
    lcg_state = 777u;
    for (int s = 0; s < 300; ++s) {
      iris *k = nb_store(1 + (int)(lcg01() * 4.0f), 16, 1 + (int)(lcg01() * 16.0f), 1);
      for (int q = 0; q < 12; ++q) {
        float in[4];
        nb_reading(k, in);
        for (int kk = 1; kk <= IRIS_KNN_MAXK; ++kk) {
          float o[16]; int ids[IRIS_KNN_MAXK];
          unsigned want = 0u, have = 0u; int blended = 0;
          iris_knn_predict(k, in, o, kk);
          for (int j = 0; j < 16; ++j) if (o[j] != 0.0f) { want |= 1u << j; blended++; }
          const int got = iris_nearest(k, in, ids, 0, kk);
          for (int j = 0; j < got; ++j) have |= 1u << (ids[j] - 1);
          trials++;
          if (got < kk && got < k->n_ex) short_lists++;
          if (want != have || got != blended) wrong++;
        }
      }
    }
    snprintf(d, sizeof d, "%ld of %ld neighbourhoods differ (%ld shorter than asked "
             "for, as k-NN's)", wrong, trials, short_lists);
    check("iris_nearest's first kk are the takes iris_knn_predict blends",
          wrong == 0 && short_lists > 0, d); }

  /* ---- iris_nearest: ascending, in fractions of each range -----------------
     Every take of random stores of up to 60, asked for in one call (see
     nb_list_check for what each list must satisfy), and then a store every
     take of which is at no finite distance: fitted on takes across 0..1,
     given 20 takes past 1e20 and its first takes deleted. */
  { long c[6] = { 0, 0, 0, 0, 0, 0 };
    lcg_state = 99u;
    for (int s = 0; s < 200; ++s) {
      iris *k = nb_store(1 + (int)(lcg01() * 4.0f), 2, 1 + (int)(lcg01() * 60.0f), 0);
      for (int q = 0; q < 10; ++q) {
        float in[4];
        nb_reading(k, in);
        nb_list_check(k, in, c);
      }
    }
    iris *k = iris_init(NB, sizeof NB, 2, 8, 1, 64, 1u);
    for (int r = 0; r < 4; ++r) { float in[2] = { (float)(r & 1), (float)(r >> 1) }, y = 0.0f; iris_record(k, in, &y); }
    iris_train_elm(k, 1e-3f, NSCR, sizeof NSCR);
    for (int r = 0; r < 20; ++r) {
      float in[2] = { 1e20f * (float)(1 + r % 3), (float)(r % 5) }, y = (float)r;
      iris_record(k, in, &y);
    }
    for (int r = 0; r < 4; ++r) iris_delete_index(k, 0);
    for (int q = 0; q < 10; ++q) { float in[2] = { lcg01(), lcg01() }; nb_list_check(k, in, c); }
    snprintf(d, sizeof d, "%ld lists (%ld at no finite distance): %ld values off (worst "
             "%.2g), %ld out of order, %ld counts, %ld prefixes wrong", c[0], c[5], c[1],
             nb_worst, c[2], c[3], c[4]);
    check("iris_nearest's distances ascend, in fractions of each range",
          c[1] == 0 && c[2] == 0 && c[3] == 0 && c[4] == 0 && c[5] == 10, d); }

  /* ---- iris_nearest: a tie comes back in the order it was recorded ----------
     Thirty takes at three points, recorded in turn (A B C A B C ...), so each
     point holds ten takes at exactly one distance from any reading: more than
     one pass of IRIS_KNN_MAXK. A reading nearest A, then B, then C must get
     A's ten in the order they were recorded, then B's, then C's. */
  { static unsigned char M[IRIS_ARENA(2, 8, 1, 32)];
    iris *k = iris_init(M, sizeof M, 2, 8, 1, 32, 1u);
    const float pts[3][2] = { { 0.1f, 0.1f }, { 0.5f, 0.6f }, { 0.9f, 1.0f } };
    for (int r = 0; r < 30; ++r) { float y = (float)r; iris_record(k, pts[r % 3], &y); }
    const float q[2] = { 0.0f, 0.0f };
    int ids[30]; float dists[30];
    const int got = iris_nearest(k, q, ids, dists, 30);
    int wrong = 0;
    for (int j = 0; j < 30; ++j) {
      const int want = (j / 10) + 3 * (j % 10) + 1;
      if (ids[j] != want) wrong++;
    }
    snprintf(d, sizeof d, "%d takes back, %d out of recording order; first %d %d %d, "
             "eleventh %d", got, wrong, ids[0], ids[1], ids[2], ids[10]);
    check("iris_nearest returns tied takes in recording order", got == 30 && wrong == 0, d); }

  /* ---- iris_nearest: takes at no finite distance ---------------------------
     The two far takes of "neighbours answer takes far outside the fitted
     ranges" above: every ordinary distance overflows, so both are ranked by
     the far distance, identifier 4 before 3 as the classifier names 4, and
     both distances are infinity, with a healthy status. Then a take inside
     the ranges added: it is the only take at a finite distance, so asking
     for two gets one, the take k-NN plays alone. */
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
    int ids[2] = { 0, 0 }; float dists[2] = { 0.0f, 0.0f };
    const int got = iris_nearest(k, q, ids, dists, 2);
    const int far_ok = got == 2 && ids[0] == 4 && ids[1] == 3 && dists[0] > 3.4e38f
                    && dists[1] > 3.4e38f && iris_get_status(k) == IRIS_STATUS_OK;
    float c[3] = { 0.5f, 0.5f, 7.0f }, yc = 0.5f;
    const int id_c = iris_record(k, c, &yc);
    int ids2[2] = { 0, 0 }; float dists2[2] = { -1.0f, -1.0f }, o = -1.0f;
    const int got2 = iris_nearest(k, q, ids2, dists2, 2);
    iris_knn_predict(k, q, &o, 2);
    const int mixed_ok = got2 == 1 && ids2[0] == id_c && dists2[0] == 0.0f
                      && dists2[1] == -1.0f && o == 0.5f;
    snprintf(d, sizeof d, "far: %d back, ids %d %d, distances %g %g; one near: %d back, "
             "id %d at %g, k-NN %g", got, ids[0], ids[1], (double)dists[0], (double)dists[1],
             got2, ids2[0], (double)dists2[0], (double)o);
    check("iris_nearest: takes at no finite distance", far_ok && mixed_ok, d); }

  /* ---- iris_nearest at the ends of the float range -------------------------
     Two takes whose first input never moved, at 3e38, and a reading of
     -3e38 there: the difference overflows, times the still input's zero
     scale that is not a number, so no ordinary distance is a number and
     the far distance ranks the takes. It skips the still input and no cap
     acts, so the distances reported are the unit's own, 0.25 and 0.75, and
     the first take is the classifier's. Then a range narrower than the
     reciprocal of the largest float, 1e-39, which the fit never makes,
     with a reading exactly on its takes: every distance is 0 times infinity,
     and iris_nearest must refuse exactly as iris_classify_1nn does, the
     arena after each the same. */
  { static unsigned char M[IRIS_ARENA(2, 8, 1, 8)];
    iris *k = iris_init(M, sizeof M, 2, 8, 1, 8, 1u);
    float t1[2] = { 3e38f, 0.0f }, t2[2] = { 3e38f, 1.0f }, y = 0.5f;
    iris_record(k, t1, &y); iris_record(k, t2, &y);
    const float q[2] = { -3e38f, 0.25f };
    int ids[2] = { 0, 0 }; float dists[2] = { -1.0f, -1.0f };
    const int named = iris_classify_1nn(k, q, 0);
    const int got = iris_nearest(k, q, ids, dists, 2);
    const int far_ok = got == 2 && ids[0] == 1 && ids[1] == 2 && named == 1
                    && dists[0] == 0.25f && dists[1] == 0.75f;
    k = iris_init(M, sizeof M, 2, 8, 1, 8, 1u);
    for (int r = 0; r < 3; ++r) { float in[2] = { 0.0f, (float)r }, o = (float)r; iris_record(k, in, &o); }
    iris_train_elm(k, 1e-3f, NSCR, sizeof NSCR);
    k->in_hi[0] = 1e-39f;
    const float on[2] = { 0.0f, 0.5f };
    k->status = IRIS_STATUS_OK;
    memcpy(NB_SNAP, M, sizeof M);
    const int c1 = iris_classify_1nn(k, on, 0);
    memcpy(NB_AFTER, M, sizeof M);
    memcpy(M, NB_SNAP, sizeof M);
    const int g1 = iris_nearest(k, on, ids, dists, 2);
    const int narrow_ok = c1 == -1 && g1 == 0 && iris_get_status(k) == IRIS_NAN_TRAPPED
                       && memcmp(M, NB_AFTER, sizeof M) == 0;
    snprintf(d, sizeof d, "overflowed still input: %d back, ids %d %d at %g %g (classifier %d); "
             "range 1e-39: nearest %d, classifier %d, status %d", got, ids[0], ids[1],
             (double)dists[0], (double)dists[1], named, g1, c1, (int)iris_get_status(k));
    check("iris_nearest at the ends of the float range", far_ok && narrow_ok, d); }

  /* ---- iris_nearest: the reading may share memory with dists ----------------
     Twelve takes on one input at 0 to 11 and a reading of 5.2 kept in the
     first slot of the array dists is written into: every pass after the
     first reads the reading again, so it must read a copy taken before
     anything was written. The answer must equal the one with separate
     arrays, identifiers and distances. */
  { static unsigned char M[IRIS_ARENA(1, 8, 1, 16)];
    iris *k = iris_init(M, sizeof M, 1, 8, 1, 16, 1u);
    for (int r = 0; r < 12; ++r) { float in = (float)r, o = (float)r; iris_record(k, &in, &o); }
    const float q = 5.2f;
    int ids[12], ids2[12]; float dists[12], buf[12];
    const int got = iris_nearest(k, &q, ids, dists, 12);
    buf[0] = q;
    const int got2 = iris_nearest(k, buf, ids2, buf, 12);
    const int same = got == 12 && got2 == 12 && memcmp(ids, ids2, sizeof ids) == 0
                  && memcmp(dists, buf, sizeof dists) == 0;
    snprintf(d, sizeof d, "separate: %d back, ninth id %d; sharing: %d back, ninth id %d; same %d",
             got, ids[8], got2, ids2[8], same);
    check("iris_nearest: the reading may share memory with dists", same, d); }

  /* ---- iris_nearest: either buffer may be null ----------------------------
     With ids, dists or both null the count is the same and the buffer given
     holds the same values, and nothing past the count is written: canaries
     after it survive. n below 1 asks for nothing: 0, and not a byte of the
     arena written, even on an instrument never fitted with a reading that is
     not finite. */
  { long wrong = 0, trials = 0;
    lcg_state = 5150u;
    for (int s = 0; s < 100; ++s) {
      iris *k = nb_store(1 + (int)(lcg01() * 4.0f), 2, 1 + (int)(lcg01() * 30.0f), 0);
      for (int q = 0; q < 10; ++q) {
        float in[4];
        nb_reading(k, in);
        const int n = 1 + (int)(lcg01() * 20.0f);
        int i0[24], i1[24]; float d0[24], d2[24];
        for (int j = 0; j < 24; ++j) { i0[j] = i1[j] = -7; d0[j] = d2[j] = -7.0f; }
        const int g0 = iris_nearest(k, in, i0, d0, n);
        const int g1 = iris_nearest(k, in, i1, 0, n);
        const int g2 = iris_nearest(k, in, 0, d2, n);
        const int g3 = iris_nearest(k, in, 0, 0, n);
        trials++;
        if (g1 != g0 || g2 != g0 || g3 != g0 || memcmp(i0, i1, sizeof i0)
            || memcmp(d0, d2, sizeof d0)) wrong++;
        for (int j = g0; j < 24; ++j) if (i0[j] != -7 || d0[j] != -7.0f) { wrong++; break; }
      }
    }
    iris *k = iris_init(NB, sizeof NB, 2, 8, 1, 64, 1u);
    for (int r = 0; r < 4; ++r) { float in[2] = { (float)r, 10.0f * (float)r }, y = 1.0f; iris_record(k, in, &y); }
    const float nan_in[2] = { __builtin_nanf(""), 0.0f };
    int ids[1] = { -7 }; float dists[1] = { -7.0f };
    memcpy(NB_SNAP, NB, sizeof NB);
    const int z0 = iris_nearest(k, nan_in, ids, dists, 0);
    const int z1 = iris_nearest(k, nan_in, ids, dists, -3);
    const int untouched = memcmp(NB, NB_SNAP, sizeof NB) == 0 && ids[0] == -7 && dists[0] == -7.0f;
    snprintf(d, sizeof d, "%ld of %ld calls with a null buffer differ; n 0 and -3 return %d %d, "
             "arena untouched %d", wrong, trials, z0, z1, untouched);
    check("iris_nearest: either buffer may be null, n below 1 asks nothing",
          wrong == 0 && z0 == 0 && z1 == 0 && untouched, d); }

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
    int near_id = 0; float near_d = -1.0f;
    const int got = play_near(k, in, &near_id, &near_d, 1);
    snprintf(d, sizeof d, "compiled against iris *; 1-NN answered id %d, novelty %.3f, "
             "nearest id %d at %.3f", id, (double)nov, near_id, (double)near_d);
    check("the playing functions, iris_novelty and iris_nearest take iris *",
          id > 0 && nov >= 0.0f && nov <= 1.0f && got == 1 && near_id == id && near_d >= 0.0f, d); }

  printf("\n  %s (%d failed)\n\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails);
  return fails ? 1 : 0;
}
