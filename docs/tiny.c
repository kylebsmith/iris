/* ============================================================================
   tiny.c — iris_train, written again by hand in one short file, and checked
   against the library.

   It answers one question a reviewer or a student is entitled to ask: is
   iris.h doing anything mysterious, or is it this, plus guards, persistence,
   neighbours and documentation? This file is the whole algorithm -- scale
   the demonstrations, draw the starting weights, shuffle, forward pass,
   backward pass, momentum, stop at the plateau -- written from the model the
   header describes, not by calling it. Then it trains the library on the
   same demonstrations and compares the two instruments on a grid of
   gestures.

   THE AGREEMENT BOUND IS ZERO: every gesture must play bit-identically,
   and the two must stop after the same number of epochs. That is the right
   bound, not a fragile one. Both programs do the same operations on 32-bit
   floats in the same order, IEEE 754 rounds each one exactly one way, iris.h
   refuses to compile where a compiler would carry wider intermediate
   results, and both switch fused multiply-add contraction off. So the two
   agree on every C99 compiler (Apple clang, clang and gcc, at -O0 to -Os,
   measured) or one of them no longer computes the model the header
   describes, and then this program exits 1. Changing the momentum in iris.h
   from 0.85f to 0.8499f fails it, as does stretching iris's output scaling
   by one part in a million.

       sh build.sh tiny
   or  cc -std=c99 -O2 -o tiny docs/tiny.c -lm && ./tiny
   ============================================================================ */

#include "../iris.h"                 /* only to compare against */
/* iris.h switches contraction off for its own code; do the same here, or a
   compiler may fuse a*b+c in this file and not in the library. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif
#include <math.h>                    /* sqrtf: correctly rounded, like iris's */
#include <stdio.h>

#define NI 2                         /* inputs: a distance in mm, a tilt in g */
#define NH 12                        /* hidden units                          */
#define NO 1                         /* output: a pitch in hertz              */
#define NEX 16                       /* demonstrations                        */
#define SEED 1234u

/* ---- the entire model: two weight matrices and two bias vectors ---------- */
static float w1[NH][NI], b1[NH], w2[NO][NH], b2[NO];
static float v1[NH][NI], vb1[NH], v2[NO][NH], vb2[NO];   /* momentum velocity */
static float hid[NH], out[NO];
static float in_lo[NI], in_hi[NI], out_lo[NO], out_hi[NO];

/* The activation iris.h uses (PART 1): x(27+x^2)/(27+9x^2), held to [-1,1].
   The +-1e9 test only keeps x*(27+x^2) from overflowing. */
static float act(float x) {
  if (x >  1.0e9f) return  1.0f;
  if (x < -1.0e9f) return -1.0f;
  const float x2 = x * x, p = x * (27.0f + x2) / (27.0f + 9.0f * x2);
  return p > 1.0f ? 1.0f : (p < -1.0f ? -1.0f : p);
}
static float sig(float x) { return 0.5f * (act(0.5f * x) + 1.0f); }

/* xorshift32, as iris.h uses; a zero state becomes 0x9E3779B9. */
static uint32_t rs;
static uint32_t rnd_u32(void) {
  rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
  if (!rs) rs = 0x9E3779B9u;
  return rs;
}
static float rnd_sym(void) { return (float)(int32_t)rnd_u32() * (1.0f / 2147483648.0f); }

/* Inputs map to [-1,+1] over the demonstrated range; outputs to [0.1,0.9]. */
static float norm_in(int i, float v) { return 2.0f * ((v - in_lo[i]) / (in_hi[i] - in_lo[i])) - 1.0f; }
static float norm_out(int o, float v) {
  return 0.1f + (v - out_lo[o]) / (out_hi[o] - out_lo[o]) * (0.9f - 0.1f);
}
static float denorm_out(int o, float y) {
  float v = out_lo[o] + (y - 0.1f) / (0.9f - 0.1f) * (out_hi[o] - out_lo[o]);
  return v < out_lo[o] ? out_lo[o] : (v > out_hi[o] ? out_hi[o] : v);
}

static void forward(const float *x) {
  for (int h = 0; h < NH; ++h) {
    float s = b1[h];
    for (int i = 0; i < NI; ++i) s += w1[h][i] * x[i];
    hid[h] = act(s);
  }
  for (int o = 0; o < NO; ++o) {
    float s = b2[o];
    for (int h = 0; h < NH; ++h) s += w2[o][h] * hid[h];
    out[o] = sig(s);
  }
}

/* A velocity this small can never move a weight again; it is set to zero,
   as the ESP32-S3's floating-point unit would round it. */
static float flush(float v) { return (v < 1e-30f && v > -1e-30f) ? 0.0f : v; }

int main(void) {
  /* Sixteen demonstrations on a 4 x 4 grid of distance (mm) and tilt (g).
     The pitch rises with distance, and tilt bends it more the further away
     the hand is. */
  float D[NEX][NI + NO];
  for (int e = 0; e < NEX; ++e) {
    const float d = 100.0f + 300.0f * (float)(e % 4), t = -1.5f + (float)(e / 4);
    D[e][0] = d; D[e][1] = t;
    D[e][2] = 220.0f + 0.4f * d + 0.1f * d * t;
  }

  /* SCALE: the smallest and largest value of every column. (These spans are
     wide, so iris.h's rules for an input or output that never moved do not
     arise here.) */
  for (int c = 0; c < NI + NO; ++c) {
    float lo = D[0][c], hi = D[0][c];
    for (int e = 1; e < NEX; ++e) { if (D[e][c] < lo) lo = D[e][c]; if (D[e][c] > hi) hi = D[e][c]; }
    if (c < NI) { in_lo[c] = lo; in_hi[c] = hi; } else { out_lo[c - NI] = lo; out_hi[c - NI] = hi; }
  }

  /* START: uniform in [-1,1) over the square root of the fan-in; biases 0. */
  rs = SEED;
  for (int h = 0; h < NH; ++h) for (int i = 0; i < NI; ++i) w1[h][i] = rnd_sym() * (1.0f / sqrtf((float)NI));
  for (int o = 0; o < NO; ++o) for (int h = 0; h < NH; ++h) w2[o][h] = rnd_sym() * (1.0f / sqrtf((float)NH));

  /* TRAIN: per-demonstration gradient descent with classical momentum, the
     order shuffled every epoch. Stop when 2,000 epochs buy less than 10% of
     the error, when the error falls under 1e-6, or at 60,000 epochs. */
  const float lr = 0.10f, mom = 0.85f;
  int order[NEX], done = 0;
  float ref = 0.0f;
  for (int e = 0; e < NEX; ++e) order[e] = e;
  while (done < 60000) {
    for (int i = NEX - 1; i > 0; --i) {                  /* Fisher-Yates shuffle */
      int j = (int)(rnd_u32() % (uint32_t)(i + 1)), t = order[i];
      order[i] = order[j]; order[j] = t;
    }
    float err = 0.0f;
    for (int s = 0; s < NEX; ++s) {
      const float *row = D[order[s]];
      float x[NI], t[NO], d_out[NO], d_hid[NH];
      for (int i = 0; i < NI; ++i) x[i] = norm_in(i, row[i]);
      for (int o = 0; o < NO; ++o) t[o] = norm_out(o, row[NI + o]);
      forward(x);
      for (int o = 0; o < NO; ++o) {                     /* output error        */
        float y = out[o], d = y - t[o];
        err += d * d;
        d_out[o] = d * y * (1.0f - y);                   /* x the logistic slope */
      }
      for (int h = 0; h < NH; ++h) {                     /* blame flows back     */
        float a = 0.0f;
        for (int o = 0; o < NO; ++o) a += w2[o][h] * d_out[o];
        d_hid[h] = a * (1.0f - hid[h] * hid[h]);         /* x the tanh slope     */
      }
      for (int o = 0; o < NO; ++o) {                     /* nudge, with momentum */
        for (int h = 0; h < NH; ++h) {
          v2[o][h] = flush(mom * v2[o][h] - lr * d_out[o] * hid[h]);
          w2[o][h] += v2[o][h];
        }
        vb2[o] = flush(mom * vb2[o] - lr * d_out[o]); b2[o] += vb2[o];
      }
      for (int h = 0; h < NH; ++h) {
        for (int i = 0; i < NI; ++i) {
          v1[h][i] = flush(mom * v1[h][i] - lr * d_hid[h] * x[i]);
          w1[h][i] += v1[h][i];
        }
        vb1[h] = flush(mom * vb1[h] - lr * d_hid[h]); b1[h] += vb1[h];
      }
    }
    err /= (float)(NEX * NO);
    ++done;
    if (err < 1e-6f) break;
    if (done % 2000 == 0) {
      if (ref > 0.0f && (ref - err) <= 0.10f * ref) break;
      ref = err;
    }
  }

  /* ---- the library, on the identical task -------------------------------- */
  static unsigned char mem[IRIS_ARENA(NI, NH, NO, NEX)];
  iris *k = iris_init(mem, sizeof mem, NI, NH, NO, NEX, SEED);
  for (int e = 0; k && e < NEX; ++e) iris_record(k, D[e], &D[e][NI]);
  if (!k || !iris_train(k)) { printf("   FAIL  iris could not be trained\n"); return 1; }

  const char *RAMP = " .:-=+*#%@";
  float worst = 0.0f;
  int same = 0, n = 0;
  printf("\n   tiny.c (this file)        iris.h (the library)\n");
  for (int y = 6; y >= 0; --y) {
    char a[16], b[16];
    for (int x = 0; x <= 14; ++x, ++n) {
      float in[NI] = { in_lo[0] + (in_hi[0] - in_lo[0]) * (float)x / 14.0f,
                       in_lo[1] + (in_hi[1] - in_lo[1]) * (float)y / 6.0f };
      float xn[NI] = { norm_in(0, in[0]), norm_in(1, in[1]) }, lib;
      forward(xn);
      float mine = denorm_out(0, out[0]);
      iris_predict(k, in, &lib);
      float d = mine > lib ? mine - lib : lib - mine;
      if (!(d <= worst)) worst = d;                      /* a NaN counts as worst */
      same += mine == lib;
      int lm = (int)((mine - out_lo[0]) / (out_hi[0] - out_lo[0]) * 9.0f + 0.5f);
      int ll = (int)((lib  - out_lo[0]) / (out_hi[0] - out_lo[0]) * 9.0f + 0.5f);
      a[x] = RAMP[lm < 0 ? 0 : (lm > 9 ? 9 : lm)];
      b[x] = RAMP[ll < 0 ? 0 : (ll > 9 ? 9 : ll)];
    }
    a[15] = b[15] = '\0';
    printf("   %s          %s\n", a, b);
  }
  const int ok = same == n && done == iris_train_epochs_done(k);
  printf("\n   epochs: tiny %d, iris %d\n", done, iris_train_epochs_done(k));
  printf("   %d of %d gestures bit-identical (all must be); largest difference %.3g Hz\n",
         same, n, (double)worst);
  printf("   %s\n\n", ok ? "PASS  tiny.c and iris.h compute the same instrument"
                          : "FAIL  tiny.c and iris.h disagree");
  return ok ? 0 : 1;
}
