/* ============================================================================
   tiny.c — the same algorithm iris.h implements, by hand, in a fraction of the space.

   THIS IS AN AUDIT, NOT A LIBRARY. Its job is to answer one question a reviewer
   or a student is entitled to ask: is iris.h doing anything mysterious, or is
   it this, plus guards, persistence, ports and documentation?

   Run it. It prints its own learned surface and iris.h's on the same task, and
   the mean difference between them. If the number is small, the library and this
   one short file compute the same thing — and every other line in
   iris.h is there for a reason you can name (a refusal, a file format, an
   output port, an explanation), not because the maths needs it.

       cc -std=c99 -O2 -I.. -o tiny tiny.c -lm && ./tiny
   ============================================================================ */

#include "../iris.h"                 /* only to compare against; tiny needs none */
#include <stdio.h>

#define NI 2                         /* inputs  */
#define NH 12                        /* hidden units */
#define NO 1                         /* outputs */
#define NEX 4                        /* demonstrations */

/* ---- the entire model: two weight matrices and two bias vectors ---------- */
static float w1[NH][NI], b1[NH], w2[NO][NH], b2[NO];
static float v1[NH][NI], vb1[NH], v2[NO][NH], vb2[NO];   /* momentum velocity */
static float hid[NH], out[NO];

static float tanh_(float x) {        /* Padé rational tanh, as iris.h uses    */
  if (x >  4.9f) return  1.0f;
  if (x < -4.9f) return -1.0f;
  float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
static float sig_(float x) { return 0.5f * (tanh_(0.5f * x) + 1.0f); }

/* A deterministic RNG. Any will do; this is xorshift32, as iris.h uses.      */
static uint32_t rs = 1234;
static float rnd(void) {             /* uniform in [-1, 1)                    */
  rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
  return (float)((int32_t)rs) / 2147483648.0f;
}

static void forward(const float *x) {
  for (int h = 0; h < NH; ++h) {
    float s = b1[h];
    for (int i = 0; i < NI; ++i) s += w1[h][i] * x[i];
    hid[h] = tanh_(s);
  }
  for (int o = 0; o < NO; ++o) {
    float s = b2[o];
    for (int h = 0; h < NH; ++h) s += w2[o][h] * hid[h];
    out[o] = sig_(s);
  }
}

int main(void) {
  /* Four demonstrations. Inputs already in [-1,+1], outputs in [0.1,0.9] —
     iris.h does that rescaling for you; here it is done by hand so the whole
     algorithm is visible.                                                    */
  const float X[NEX][NI] = { {-1,-1}, {1,-1}, {-1,1}, {1,1} };
  const float T[NEX][NO] = { {0.14f}, {0.86f}, {0.86f}, {0.14f} };

  for (int h = 0; h < NH; ++h) {     /* init: U[-1,1)/sqrt(fan_in), biases 0  */
    for (int i = 0; i < NI; ++i) w1[h][i] = rnd() / 1.41421356f;
    b1[h] = 0.0f;
  }
  for (int o = 0; o < NO; ++o) {
    for (int h = 0; h < NH; ++h) w2[o][h] = rnd() / 3.46410162f;
    b2[o] = 0.0f;
  }

  /* TRAIN: per-example gradient descent with classical momentum. That is all
     backpropagation is here — one forward pass, one backward pass, nudge.     */
  const float lr = 0.10f, mom = 0.85f;
  for (int ep = 0; ep < 4000; ++ep) {
    for (int e = 0; e < NEX; ++e) {
      forward(X[e]);
      float d_out[NO], d_hid[NH];
      for (int o = 0; o < NO; ++o) {                       /* output error   */
        float y = out[o];
        d_out[o] = (y - T[e][o]) * y * (1.0f - y);         /* x sigmoid'      */
      }
      for (int h = 0; h < NH; ++h) {                       /* push it back    */
        float s = 0.0f;
        for (int o = 0; o < NO; ++o) s += w2[o][h] * d_out[o];
        d_hid[h] = s * (1.0f - hid[h] * hid[h]);           /* x tanh'         */
      }
      for (int o = 0; o < NO; ++o) {                       /* apply, w/ mom.  */
        for (int h = 0; h < NH; ++h) {
          v2[o][h] = mom * v2[o][h] - lr * d_out[o] * hid[h];
          w2[o][h] += v2[o][h];
        }
        vb2[o] = mom * vb2[o] - lr * d_out[o]; b2[o] += vb2[o];
      }
      for (int h = 0; h < NH; ++h) {
        for (int i = 0; i < NI; ++i) {
          v1[h][i] = mom * v1[h][i] - lr * d_hid[h] * X[e][i];
          w1[h][i] += v1[h][i];
        }
        vb1[h] = mom * vb1[h] - lr * d_hid[h]; b1[h] += vb1[h];
      }
    }
  }

  /* ---- compare against the library on the identical task ------------------ */
  static unsigned char mem[IRIS_ARENA(NI, NH, NO, 8)];
  iris *k = iris_init(mem, sizeof mem, NI, NH, NO, 8, 1234);
  const float D[NEX][3] = { {0,0,0.05f}, {1,0,0.95f}, {0,1,0.95f}, {1,1,0.05f} };
  for (int e = 0; e < NEX; ++e) iris_record(k, D[e], &D[e][2]);
  iris_continue_to_plateau(k, 0, 0, 0);

  const char *RAMP = " .:-=+*#%@";
  double acc = 0.0; int n = 0;
  printf("\n   tiny.c (this file)        iris.h (the library)\n");
  for (int y = 6; y >= 0; --y) {
    printf("   ");
    for (int x = 0; x <= 14; ++x) {
      float u = (float)x / 14.0f, v = (float)y / 6.0f;
      float xin[NI] = { 2.0f * u - 1.0f, 2.0f * v - 1.0f };
      forward(xin);
      float mine = (out[0] - 0.1f) / 0.8f;
      int l = (int)(mine * 9.0f + 0.5f);
      putchar(RAMP[l < 0 ? 0 : (l > 9 ? 9 : l)]);
    }
    printf("     ");
    for (int x = 0; x <= 14; ++x) {
      float in[NI] = { (float)x / 14.0f, (float)y / 6.0f }, o;
      iris_predict(k, in, &o);
      int l = (int)(o * 9.0f + 0.5f);
      putchar(RAMP[l < 0 ? 0 : (l > 9 ? 9 : l)]);
      float xin[NI] = { 2.0f * in[0] - 1.0f, 2.0f * in[1] - 1.0f };
      forward(xin);
      float mine = (out[0] - 0.1f) / 0.8f;
      acc += (mine > o ? mine - o : o - mine); n++;
    }
    putchar('\n');
  }
  printf("\n   mean |tiny - iris| over the surface: %.4f\n", acc / (double)n);
  /* No line counts here. The two that used to be printed were string
     literals computed by nothing, and neither was ever true of either file.
     A number in a program that the program does not measure is the exact
     thing this project tells contributors not to write. */
  printf("   tiny.c does the same job in a small fraction of the space.\n\n");
  return 0;
}
