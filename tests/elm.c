/* SPDX-License-Identifier: BSD-3-Clause
   Copyright (c) 2026 Kyle Smith */
/* ============================================================================
   elm.c — the closed-form trainer (iris.h PART 8d), checked from outside.

   From the repository root:

     cc -std=c99 -O2 -Wall -Wextra -I. -o build/elm tests/elm.c && ./build/elm

   and once more under the sanitizers, which decide the scratch-alignment case
   (the buffer is handed over at every byte offset from 0 to 7, with exactly
   IRIS_ELM_SCRATCH bytes behind it):

     /opt/homebrew/opt/llvm/bin/clang -std=c99 -O1 -g -I. \
       -fsanitize=address,undefined,alignment,float-divide-by-zero \
       -fno-sanitize-recover=all -o build/elm_san tests/elm.c && ./build/elm_san

   Exit status 0 means every check passed; anything else is a failure.

   `./build/elm measure` prints the two measurements quoted in PART 8d (what
   smoothing does to recall and to held-out error, and how well the refreshed
   ledger points at a bad take, beside the backprop ledger on the same
   sessions). It asserts nothing and takes a minute or two.

   What is checked:
     1. every refusal leaves every byte of the arena as it was; the status
        too, except that a poisoned demonstration sets it to IRIS_NAN_TRAPPED
     2. at smoothing 0 the solved weights match pinned hashes, bit for bit
        (fixed recipes, hashes below)
     3. recall error rises with smoothing, and held-out error on noisy
        demonstrations falls at a moderate setting; the output bias is never
        smoothed
     4. a solve refreshes the worst-example ledger
     5. every output constant, or every take at one gesture, is not reported
        as a collapse; a real collapse still is, as IRIS_SOLVE_COLLAPSED, and
        does not stop warm training
     6. the scratch works at every byte offset, with every byte of it in use
     7. a solve ends a sliced run that is still in flight, and leaves the
        progress fields describing the solve: no epochs, finished
     8. a solve whose output weights lie past IRIS_W_LIMIT saves, loads and
        plays; warm training from it reports the divergence, and iris_train
        starts over
   ========================================================================= */

#include "iris.h"
/* The pinned hashes are exact, so the demonstrations built below must be
   exact too: forbid fused multiply-add in this file's own arithmetic, as
   iris.h does in its own. iris.h's contraction setting ends with the header,
   so this comes after the #include, where the header says an includer's own
   pragma belongs. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
static void check(const char *name, int ok, const char *detail) {
  printf("  [%s] %-58s %s\n", ok ? "PASS" : "FAIL", name, detail);
  if (!ok) fails++;
}

static uint32_t fnv1a(const void *p, size_t n, uint32_t h) {
  const unsigned char *b = (const unsigned char *)p;
  for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
  return h;
}
static uint32_t weight_hash(const iris *k) {
  uint32_t h = 2166136261u;
  h = fnv1a(k->w1, sizeof(float) * (size_t)(k->n_hid * k->n_in), h);
  h = fnv1a(k->b1, sizeof(float) * (size_t)k->n_hid, h);
  h = fnv1a(k->w2, sizeof(float) * (size_t)(k->n_out * k->n_hid), h);
  h = fnv1a(k->b2, sizeof(float) * (size_t)k->n_out, h);
  return h;
}
static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }

static unsigned char arena[IRIS_ARENA(5, 48, 8, 128)];
static unsigned char before[sizeof arena];
static unsigned char scratch[IRIS_ELM_SCRATCH(48, 8)];
static unsigned char wide[IRIS_ARENA(IRIS_MAX_IN, 12, 3, 16)];

/* ---- 2. the pinned recipes ------------------------------------------------
   Seven fixed recipes: widths 8 to 48, one to five inputs, lam0 from 0 to
   1e-3, raw sensor units, explicit gains, and two that need ridge escalation.
   The hashes cover w1, b1, w2 and b2, beside the return value and the bits of
   iris_last_error. The same numbers come out of Apple clang 17, Homebrew
   clang 22 and gcc-15 at -O0, -O2 and -Os, in C and in C++. They pin the
   unsmoothed solve: at smoothing 0 nothing about it may move. */
static float frac(unsigned i, unsigned a, unsigned m) { return (float)((i * a) % m) / (float)m; }

struct recipe { int ni, nh, no, n, kind; uint32_t seed; float lam0, gw, gb;
                int ret; uint32_t weights, last_error; };
static const struct recipe R[] = {
  { 2, 12, 3, 20, 0,     7u, 1e-4f, 0.0f, 0.0f, 0, 0xDA8933A1u, 0x39384075u },
  { 3, 24, 2, 50, 1, 12345u, 1e-3f, 0.0f, 0.0f, 0, 0x30B9BD48u, 0x3B947D93u },
  { 5,  8, 1, 13, 0,    99u, 0.0f,  0.0f, 0.0f, 0, 0xEADC282Du, 0x3B8E6A10u },
  { 2, 48, 3, 50, 0,    42u, 1e-3f, 0.0f, 0.0f, 0, 0x0D768C2Cu, 0x38B51A68u },
  { 2,  8, 1, 32, 2,     3u, 1e-7f, 0.0f, 0.0f, 2, 0x6C2F23F3u, 0x2E133B06u },
  { 3, 12, 2, 24, 2,    11u, 0.0f,  0.0f, 0.0f, 6, 0x19E8DA4Cu, 0x26855555u },
  { 2, 16, 2, 30, 0,     5u, 1e-4f, 1.0f, 0.5f, 0, 0x71BDAFD2u, 0x38CF50FDu },
};

static void record_recipe(iris *k, const struct recipe *r) {
  for (int i = 0; i < r->n; ++i) {
    float in[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }, out[3] = { 0.0f, 0.0f, 0.0f };
    for (int c = 0; c < r->ni; ++c)
      in[c] = frac((unsigned)i + 1u, 7919u + 104u * (unsigned)c, 97u + 4u * (unsigned)c);
    if (r->kind == 1) { in[0] *= 4095.0f; in[2] = in[2] * 4.0f - 2.0f; }
    if (r->kind == 2) for (int c = 0; c < r->ni; ++c) in[c] = (float)((i + c) % 3) / 2.0f;
    for (int o = 0; o < r->no; ++o) {
      float s = 0.0f;
      for (int c = 0; c < r->ni; ++c)
        s += (float)(o + c + 1) * (r->kind == 1 && c == 0 ? in[c] / 4095.0f : in[c]);
      out[o] = 0.5f + 0.4f * iris_internal_tanh(s - 0.5f * (float)(r->ni));
    }
    if (r->kind == 1) { out[0] = 20.0f + 19980.0f * out[0]; if (r->no > 1) out[1] *= 127.0f; }
    iris_record(k, in, out);
  }
}

static int solve_recipe(iris *k, const struct recipe *r) {
  return (r->gw > 0.0f) ? iris_internal_train_elm_ex(k, r->lam0, r->gw, r->gb, scratch, sizeof scratch)
                        : iris_train_elm(k, r->lam0, scratch, sizeof scratch);
}

/* ---- shared data for 3 and the measurement -------------------------------- */
static uint32_t rs = 1u;
static float rnd(void) { rs = rs * 1664525u + 1013904223u; return (float)((rs >> 8) & 0xFFFF) / 65535.0f; }
static float gauss(void) { float s = 0.0f; for (int i = 0; i < 12; ++i) s += rnd(); return s - 6.0f; }

/* four target shapes over two inputs: a soft step, a cliff, a quadratic and a
   bump, each with a second, simpler output */
static void shape_target(const float *in, float *out, int shape) {
  float a = in[0], b = in[1];
  switch (shape) {
    case 0:  out[0] = 0.5f + 0.4f * iris_internal_tanh(3.0f * (a - 0.5f));  out[1] = 0.5f + 0.3f * (a - b); break;
    case 1:  out[0] = (a > 0.5f) ? 0.85f : 0.15f;                   out[1] = 0.5f + 0.4f * b;       break;
    case 2:  out[0] = 0.1f + 0.8f * a * a;                          out[1] = 0.9f - 0.8f * b * b;   break;
    default: out[0] = 0.5f + 0.2f * (iris_internal_tanh(8.0f * (a - 0.3f)) - iris_internal_tanh(8.0f * (a - 0.7f)));
             out[1] = 0.2f + 0.6f * a * b;                                                          break;
  }
}

/* One noisy session of `nex` demonstrations, solved at nh 12 and lam0 1e-4.
   Returns the solve's result; *recall receives the mean squared miss at the
   demonstrations against what was recorded (noise included), *held the mean
   squared error on 300 fresh points against the clean target. */
static int smooth_trial(int nex, float sigma, float smooth, int shape, uint32_t seed,
                        double *recall, double *held) {
  iris *k = iris_init(arena, sizeof arena, 2, 12, 2, 128, seed);
  rs = seed;
  for (int i = 0; i < nex; ++i) {
    float in[2], o[2];
    in[0] = rnd(); in[1] = rnd();
    shape_target(in, o, shape);
    for (int j = 0; j < 2; ++j) o[j] += sigma * gauss();
    iris_record(k, in, o);
  }
  iris_set_smoothing(k, smooth);
  int ret = iris_train_elm(k, 1e-4f, scratch, sizeof scratch);
  double se = 0.0;
  for (int i = 0; i < nex; ++i) {
    float in[2], want[2], got[2];
    iris_get(k, i, in, want); iris_predict(k, in, got);
    for (int o = 0; o < 2; ++o) { double d = (double)got[o] - (double)want[o]; se += d * d; }
  }
  *recall = se / (2.0 * nex);
  se = 0.0;
  for (int t = 0; t < 300; ++t) {
    float in[2], want[2], got[2];
    in[0] = rnd(); in[1] = rnd();
    shape_target(in, want, shape); iris_predict(k, in, got);
    for (int o = 0; o < 2; ++o) { double d = (double)got[o] - (double)want[o]; se += d * d; }
  }
  *held = se / 600.0;
  return ret;
}

/* ---- 4 and the measurement: a session with one bad take -------------------
   Eight outputs over two inputs, the protocol of
   docs/adr/0019-the-residual-ledger-integrates-it-does-not-sample.md. */
static void ledger_truth(float x, float y, float *o) {
  for (int j = 0; j < 8; ++j) {
    float a = 0.7f + 0.3f * (float)j, b = 0.2f * (float)j;
    o[j] = 0.5f + 0.4f * iris_internal_tanh(a * (x - 0.5f) + (1.0f - 0.1f * (float)j) * (y - 0.5f) * (x + b));
  }
}
static uint32_t xs = 1u;
static float urand(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return (float)(xs >> 8) / 16777216.0f; }

/* records n demonstrations, the one at `bad` offset by `mag` on output `which`;
   returns that take's id (or 0 when mag is 0) */
static int ledger_session(iris *k, int n, int bad, int which, float mag) {
  int bad_id = 0;
  for (int i = 0; i < n; ++i) {
    float in[2], o[8];
    in[0] = urand(); in[1] = urand();
    ledger_truth(in[0], in[1], o);
    if (i == bad && mag != 0.0f) o[which] += mag;
    int id = iris_record(k, in, o);
    if (i == bad && mag != 0.0f) bad_id = id;
  }
  return bad_id;
}

static float median(float *v, int n) {
  for (int a = 0; a < n; ++a) for (int b = a + 1; b < n; ++b)
    if (v[b] < v[a]) { float t = v[a]; v[a] = v[b]; v[b] = t; }
  return (n & 1) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

static int measure(void) {
  const float sm[5] = { 0.0f, 0.1f, 0.3f, 0.5f, 1.0f };
  const float sig[3] = { 0.0f, 0.05f, 0.10f };
  const int exs[3] = { 8, 20, 50 };
  printf("SMOOTHING under iris_train_elm: nh 12, lam0 1e-4, 4 shapes x 16 seeds.\n"
         "Root-mean-square error: recall at the demonstrations | held out on 300\n"
         "fresh points against the clean target. Outputs span about 0..1.\n\n");
  printf("  demos noise ");
  for (int s = 0; s < 5; ++s) printf("   smoothing %.1f ", (double)sm[s]);
  printf("\n");
  for (int e = 0; e < 3; ++e)
    for (int g = 0; g < 3; ++g) {
      printf("  %5d %5.2f ", exs[e], (double)sig[g]);
      for (int s = 0; s < 5; ++s) {
        double rc = 0.0, ho = 0.0; int n = 0;
        for (int shape = 0; shape < 4; ++shape)
          for (uint32_t sd = 1; sd <= 16; ++sd) {
            double r, h;
            if (smooth_trial(exs[e], sig[g], sm[s], shape, sd * 7919u, &r, &h) >= 0) { rc += r; ho += h; n++; }
          }
        printf("  %.4f | %.4f", (double)iris_internal_sqrt((float)(rc / n)), (double)iris_internal_sqrt((float)(ho / n)));
      }
      printf("\n");
    }

  printf("\nLEDGER: nh 12, 8 outputs, one take offset on one output. Hits: the\n"
         "offset take ranked first, of 20 sessions. Clean: the margin (worst /\n"
         "second worst) with nothing wrong, over 200 sessions, and how many of\n"
         "those reach IRIS_STRESS_FLAG (%.1f). The iris_train rows are the\n"
         "integrated ledger of PART 8f on the same sessions, for comparison;\n"
         "they take a minute or two.\n\n", (double)IRIS_STRESS_FLAG);
  printf("  trainer          demos   +0.05   +0.10   +0.20   +0.40   clean median   max   >= flag\n");
  const int ns[3] = { 20, 50, 100 };
  const float mags[4] = { 0.05f, 0.10f, 0.20f, 0.40f };
  static float margins[200];
  for (int tr = 0; tr < 2; ++tr)
    for (int ni = 0; ni < 3; ++ni) {
      int n = ns[ni];
      printf("  %-15s %5d ", tr ? "iris_train" : "iris_train_elm", n);
      for (int mi = 0; mi < 4; ++mi) {
        int hits = 0;
        for (int t = 0; t < 20; ++t) {
          xs = 5000u + (uint32_t)t * 13u + (uint32_t)n;
          iris *k = iris_init(arena, sizeof arena, 2, 12, 8, 128, 1u + (uint32_t)t);
          int bad_id = ledger_session(k, n, t % n, t % 8, mags[mi]);
          if (tr) iris_train(k); else iris_train_elm(k, 1e-4f, scratch, sizeof scratch);
          if (iris_worst_example_id(k, 0) == bad_id) hits++;
        }
        printf("  %2d/20 ", hits);
      }
      float mx = 0.0f; int over = 0;
      for (int t = 0; t < 200; ++t) {
        xs = 1000u + (uint32_t)t * 7u + (uint32_t)n;
        iris *k = iris_init(arena, sizeof arena, 2, 12, 8, 128, 1u + (uint32_t)t);
        ledger_session(k, n, 0, 0, 0.0f);
        if (tr) iris_train(k); else iris_train_elm(k, 1e-4f, scratch, sizeof scratch);
        float m = 0.0f;
        iris_worst_example(k, &m);
        margins[t] = m;
        if (m > mx) mx = m;
        if (m >= IRIS_STRESS_FLAG) over++;
      }
      printf("     %5.2f     %6.2f   %3d/200\n", (double)median(margins, 200), (double)mx, over);
      fflush(stdout);
    }
  return 0;
}

/* ---- 1. refusals ---------------------------------------------------------- */
/* snap() sets the status, keeps a copy of the arena and expects the status
   to be left alone; a caller that expects a write sets want_status after it.
   refused_cleanly() compares every byte but the status word, and the status
   on its own. */
static iris *snapped;
static int want_status;
static int refused_cleanly(int ret) {
  const size_t at = (size_t)((unsigned char *)&snapped->status - arena), n = sizeof snapped->status;
  return ret == -1 && memcmp(before, arena, at) == 0
      && memcmp(before + at + n, arena + at + n, sizeof arena - at - n) == 0
      && snapped->status == want_status;
}
static void snap(iris *k, int status) {
  k->status = status; memcpy(before, arena, sizeof arena); snapped = k; want_status = status;
}

int main(int argc, char **argv) {
  char d[200];
  if (argc > 1 && strcmp(argv[1], "measure") == 0) return measure();

  const float nan_f = __builtin_nanf(""), inf_f = __builtin_inff();
  printf("\n  THE CLOSED-FORM TRAINER (iris.h PART 8d)\n\n");

  /* ---- 1. every refusal changes nothing --------------------------------------
     The instrument is fitted, then given one more take far outside its ranges,
     so a refit of the ranges would move them; and its status is set to a value
     no refusal path writes. Any byte that moves fails the check, and so does
     the status, except after a poisoned demonstration, which must set it to
     IRIS_NAN_TRAPPED: that is numerical health, which the status reports,
     where every other refusal here is a mistake in the arguments. */
  {
    int bad = 0, cases = 0;
    const size_t need = IRIS_ELM_SCRATCH(16, 2);
    iris *k = iris_init(arena, sizeof arena, 3, 16, 2, 128, 21u);
    for (int i = 0; i < 24; ++i) {
      float in[3] = { frac((unsigned)i, 37u, 101u), frac((unsigned)i, 53u, 89u), (float)(i % 4) };
      float o[2] = { in[0] * in[1], 0.3f + 0.1f * in[2] };
      iris_record(k, in, o);
    }
    iris_train(k);
    { float in[3] = { 9.0f, -9.0f, 90.0f }, o[2] = { 5.0f, -5.0f }; iris_record(k, in, o); }

    struct { float lam0, gw, gb; size_t bytes; int use_null; const char *what; } arg[] = {
      { 1e-4f, 1.0f, 1.0f, 0,        1, "no scratch" },
      { 1e-4f, 1.0f, 1.0f, 0,        0, "zero bytes of scratch" },
      { 1e-4f, 1.0f, 1.0f, need - 1, 0, "one byte short" },
      { nan_f, 1.0f, 1.0f, need,     0, "lam0 NaN" },
      { inf_f, 1.0f, 1.0f, need,     0, "lam0 +inf" },
      { -inf_f,1.0f, 1.0f, need,     0, "lam0 -inf" },
      { -1.0f, 1.0f, 1.0f, need,     0, "lam0 -1" },
      { -1e-30f,1.0f,1.0f, need,     0, "lam0 -1e-30" },
      { 1e-4f, nan_f, 1.0f, need,    0, "gain_w NaN" },
      { 1e-4f, inf_f, 1.0f, need,    0, "gain_w +inf" },
      { 1e-4f, -0.5f, 1.0f, need,    0, "gain_w -0.5" },
      { 1e-4f, 1.0f, nan_f, need,    0, "gain_b NaN" },
      { 1e-4f, 1.0f, inf_f, need,    0, "gain_b +inf" },
      { 1e-4f, 1.0f, -0.5f, need,    0, "gain_b -0.5" },
    };
    char which[64] = "";
    for (size_t a = 0; a < sizeof arg / sizeof arg[0]; ++a) {
      snap(k, IRIS_RIDGE_ESCALATED);
      int r = iris_internal_train_elm_ex(k, arg[a].lam0, arg[a].gw, arg[a].gb,
                                arg[a].use_null ? 0 : scratch, arg[a].bytes);
      cases++;
      if (!refused_cleanly(r)) { bad++; snprintf(which, sizeof which, "%s", arg[a].what); }
      if (arg[a].gw == 1.0f && arg[a].gb == 1.0f) {        /* and the public door */
        snap(k, IRIS_RIDGE_ESCALATED);
        r = iris_train_elm(k, arg[a].lam0, arg[a].use_null ? 0 : scratch, arg[a].bytes);
        cases++;
        if (!refused_cleanly(r)) { bad++; snprintf(which, sizeof which, "%s (iris_train_elm)", arg[a].what); }
      }
    }

    /* a poisoned demonstration, input side and output side */
    { const int stride = k->n_in + k->n_out;
      const int where[2] = { 5 * stride + 1, 9 * stride + 3 + 1 };
      const float poison[2] = { nan_f, inf_f };
      for (int p = 0; p < 2; ++p) {
        float keep = k->ex[where[p]];
        k->ex[where[p]] = poison[p];
        snap(k, IRIS_RIDGE_ESCALATED);
        want_status = IRIS_NAN_TRAPPED;
        int r = iris_train_elm(k, 1e-4f, scratch, sizeof scratch);
        cases++;
        if (!refused_cleanly(r)) { bad++; snprintf(which, sizeof which, "poisoned demonstration %d", p); }
        /* a mistake in the call as well -- a bad lam0, too little scratch,
           no scratch -- is found first, and leaves the status alone */
        snap(k, IRIS_RIDGE_ESCALATED);
        r = iris_train_elm(k, -1.0f, scratch, sizeof scratch);
        cases++;
        if (!refused_cleanly(r)) { bad++; snprintf(which, sizeof which, "poisoned demonstration %d, lam0 -1", p); }
        snap(k, IRIS_RIDGE_ESCALATED);
        r = iris_train_elm(k, 1e-4f, scratch, need - 1);
        cases++;
        if (!refused_cleanly(r)) { bad++; snprintf(which, sizeof which, "poisoned demonstration %d, one byte short", p); }
        snap(k, IRIS_RIDGE_ESCALATED);
        r = iris_train_elm(k, 1e-4f, 0, sizeof scratch);
        cases++;
        if (!refused_cleanly(r)) { bad++; snprintf(which, sizeof which, "poisoned demonstration %d, no scratch", p); }
        /* the reroll door sets a new seed before it solves, and must put the
           old one back when the solve refuses */
        snap(k, IRIS_RIDGE_ESCALATED);
        want_status = IRIS_NAN_TRAPPED;
        r = iris_retrain_elm_new(k, iris_seed(k) + 17u, 1e-4f, scratch, sizeof scratch);
        cases++;
        if (!refused_cleanly(r)) { bad++; snprintf(which, sizeof which, "poisoned demonstration %d (iris_retrain_elm_new)", p); }
        snap(k, IRIS_RIDGE_ESCALATED);
        r = iris_retrain_elm_new(k, iris_seed(k) + 17u, -1.0f, scratch, sizeof scratch);
        cases++;
        if (!refused_cleanly(r)) { bad++; snprintf(which, sizeof which, "poisoned demonstration %d, lam0 -1 (iris_retrain_elm_new)", p); }
        k->ex[where[p]] = keep;
      } }

    /* a shape this translation unit cannot hold, and a width under 8 */
    { int keep = k->n_in; k->n_in = IRIS_MAX_IN + 1;
      snap(k, IRIS_RIDGE_ESCALATED);
      cases++;
      if (!refused_cleanly(iris_train_elm(k, 1e-4f, scratch, sizeof scratch))) { bad++; snprintf(which, sizeof which, "shape too big"); }
      k->n_in = keep; }
    { int keep = k->n_hid; k->n_hid = 7;
      snap(k, IRIS_RIDGE_ESCALATED);
      cases++;
      if (!refused_cleanly(iris_train_elm(k, 1e-4f, scratch, sizeof scratch))) { bad++; snprintf(which, sizeof which, "nh 7"); }
      k->n_hid = keep; }

    /* nothing to fit */
    { iris *e = iris_init(arena, sizeof arena, 3, 16, 2, 128, 21u);
      snap(e, IRIS_RIDGE_ESCALATED);
      cases++;
      if (!refused_cleanly(iris_train_elm(e, 1e-4f, scratch, sizeof scratch))) { bad++; snprintf(which, sizeof which, "no demonstrations"); } }

    /* a factorisation that fails even after escalation: lam0 = 0 on 128
       demonstrations all made at one gesture, recorded after a fit on other
       data so the ranges the failed solve fitted differ from the ones it must
       put back. Both inputs are still, so every demonstration normalises to
       the same point and the normal matrix has rank one */
    int fact_ret = 0;
    { iris *f = iris_init(arena, sizeof arena, 2, 8, 1, 128, 3u);
      for (int i = 0; i < 10; ++i) { float in[2] = { (float)i, (float)(i % 3) }, o = (float)i; iris_record(f, in, &o); }
      iris_train_elm(f, 1e-4f, scratch, sizeof scratch);
      iris_clear(f);
      for (int i = 0; i < 128; ++i) { float in[2] = { 0.5f, 0.25f }, o = (float)(i % 3); iris_record(f, in, &o); }
      snap(f, IRIS_RIDGE_ESCALATED);
      fact_ret = iris_train_elm(f, 0.0f, scratch, sizeof scratch);
      cases++;
      if (!refused_cleanly(fact_ret)) { bad++; snprintf(which, sizeof which, "factorisation failure"); } }

    /* finite demonstrations whose span overflows a float: the input side
       poisons the normal matrix, the output side poisons only the solution */
    for (int side = 0; side < 2; ++side) {
      iris *f = iris_init(arena, sizeof arena, 2, 12, 1, 128, 5u);
      for (int i = 0; i < 12; ++i) { float in[2] = { (float)i, (float)(i % 4) }, o = (float)i; iris_record(f, in, &o); }
      iris_train_elm(f, 1e-4f, scratch, sizeof scratch);
      { float in[2] = { side ? 1.0f : 3e38f, 1.0f }, o = side ? 3e38f : 1.0f; iris_record(f, in, &o); }
      { float in[2] = { side ? 2.0f : -3e38f, 2.0f }, o = side ? -3e38f : 2.0f; iris_record(f, in, &o); }
      snap(f, IRIS_RIDGE_ESCALATED);
      cases++;
      if (!refused_cleanly(iris_train_elm(f, 1e-4f, scratch, sizeof scratch))) {
        bad++; snprintf(which, sizeof which, "span overflow, %s side", side ? "output" : "input"); }
    }

    snprintf(d, sizeof d, "%d of %d refusals moved a byte%s%s (the factorisation case returned %d)",
             bad, cases, bad ? "; last: " : "", which, fact_ret);
    check("every refusal leaves the arena, and the status, as it should", bad == 0, d);
  }

  /* ---- 2. smoothing 0 is the unsmoothed solve, to the bit -------------------- */
  {
    int moved = 0; char first[120] = "";
    for (size_t i = 0; i < sizeof R / sizeof R[0]; ++i) {
      const struct recipe *r = &R[i];
      iris *k = iris_init(arena, sizeof arena, r->ni, r->nh, r->no, 64, r->seed);
      record_recipe(k, r);
      int ret = solve_recipe(k, r);
      uint32_t h = weight_hash(k), le = bits(k->last_error);
      if (ret != r->ret || h != r->weights || le != r->last_error) {
        if (!moved) snprintf(first, sizeof first, "; recipe %zu: ret %d weights 0x%08X last_error 0x%08X",
                             i, ret, h, le);
        moved++;
      }
    }
    snprintf(d, sizeof d, "%d of %zu recipes moved%s", moved, sizeof R / sizeof R[0], first);
    check("smoothing 0 leaves the solved weights bit-identical", moved == 0, d);
  }

  /* ---- 3. smoothing does what it says ----------------------------------------
     Recall error is summed over 4 shapes x 16 seeds x {8, 20, 50} noisy
     demonstrations and must rise at every step of the ladder. Held-out error
     on 20 demonstrations at noise 0.10 must fall by at least a tenth between
     smoothing 0 and 0.3 (measured: 0.1239 to 0.0954 root-mean-square). And the
     setting must reach the solve at all: smoothing 1 moves the weights. */
  {
    const float ladder[6] = { 0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f };
    double rec[6] = { 0 };
    for (int s = 0; s < 6; ++s)
      for (int shape = 0; shape < 4; ++shape)
        for (uint32_t sd = 1; sd <= 16; ++sd)
          for (int nex = 8; nex <= 50; nex += 21) {
            double r, h;
            smooth_trial(nex, 0.05f, ladder[s], shape, sd * 7919u, &r, &h);
            rec[s] += r;
          }
    int rising = 1;
    for (int s = 1; s < 6; ++s) if (!(rec[s] > rec[s - 1])) rising = 0;
    snprintf(d, sizeof d, "summed recall mean squared error %.4f %.4f %.4f %.4f %.4f %.4f",
             rec[0], rec[1], rec[2], rec[3], rec[4], rec[5]);
    check("recall error rises at every smoothing step", rising, d);

    double h0 = 0.0, h3 = 0.0;
    for (int shape = 0; shape < 4; ++shape)
      for (uint32_t sd = 1; sd <= 16; ++sd) {
        double r, h;
        smooth_trial(20, 0.10f, 0.0f, shape, sd * 7919u, &r, &h); h0 += h;
        smooth_trial(20, 0.10f, 0.3f, shape, sd * 7919u, &r, &h); h3 += h;
      }
    snprintf(d, sizeof d, "held-out root-mean-square error %.4f at smoothing 0, %.4f at 0.3",
             (double)iris_internal_sqrt((float)(h0 / 64.0)), (double)iris_internal_sqrt((float)(h3 / 64.0)));
    check("moderate smoothing helps noisy demonstrations", h3 < 0.81 * h0, d);   /* -10% root-mean-square */

    const struct recipe *r = &R[0];
    iris *k = iris_init(arena, sizeof arena, r->ni, r->nh, r->no, 64, r->seed);
    record_recipe(k, r);
    iris_set_smoothing(k, 1.0f);
    int ret = solve_recipe(k, r);
    uint32_t h = weight_hash(k);
    snprintf(d, sizeof d, "recipe 0 at smoothing 1: ret %d, weights 0x%08X (0x%08X at 0), status %d",
             ret, h, r->weights, iris_get_status(k));
    check("smoothing reaches the solve", ret >= 0 && h != r->weights
          && iris_get_status(k) == IRIS_STATUS_OK, d);

    /* ...and never reaches the output bias. The bias row of the normal
       equations then carries only the small ridge relative to the data, so
       even at smoothing 1 the fitted pre-activations average to the
       demonstrated targets (in logit units). Measured: the two averages
       differ by under 4e-5; with the bias smoothed too, by 1e-2 to 2e-2. */
    iris *b = iris_init(arena, sizeof arena, 2, 12, 2, 128, 31u);
    for (int i = 0; i < 20; ++i) {
      float a = (float)((i * 7) % 20) / 19.0f, c = (float)((i * 3) % 20) / 19.0f;
      float in[2] = { a, c }, o[2] = { 0.5f + 0.4f * iris_internal_tanh(3.0f * (a - 0.2f)), 0.3f + 0.2f * a * c };
      iris_record(b, in, o);
    }
    iris_set_smoothing(b, 1.0f);
    int rb = iris_train_elm(b, 1e-4f, scratch, sizeof scratch);
    double gap[2] = { 0.0, 0.0 };
    for (int n = 0; n < b->n_ex; ++n) {
      float ti[2], to[2], x[2];
      iris_get(b, n, ti, to);
      for (int i = 0; i < 2; ++i) x[i] = iris_internal_norm_in(b, i, ti[i]);
      iris_internal_forward_norm(b, x);
      for (int o = 0; o < 2; ++o) {
        float z = b->b2[o];
        for (int j = 0; j < 12; ++j) z += b->w2[o * 12 + j] * b->hid[j];
        gap[o] += ((double)z - (double)iris_internal_logit(iris_internal_norm_out(b, o, to[o]))) / (double)b->n_ex;
      }
    }
    snprintf(d, sizeof d, "smoothing 1: ret %d, fitted minus demonstrated mean logit %.2e %.2e",
             rb, gap[0], gap[1]);
    check("smoothing leaves the output bias alone", rb >= 0 && gap[0] < 1e-3 && gap[0] > -1e-3
          && gap[1] < 1e-3 && gap[1] > -1e-3, d);
  }

  /* ---- 4. the ledger is the solve's, not a leftover ------------------------- */
  {
    iris *k = iris_init(arena, sizeof arena, 2, 12, 8, 128, 9u);
    xs = 4242u;
    int bad_id = ledger_session(k, 20, 7, 3, 0.4f);
    iris_train_elm(k, 1e-4f, scratch, sizeof scratch);
    int elm_only = iris_worst_example_id(k, 0);

    /* now a backprop fit fills the ledger, the bad take is deleted and a new
       bad take arrives at a different position; the solve must describe the
       data it just fitted */
    iris_train(k);
    iris_delete_id(k, bad_id);
    float in[2] = { 0.5f, 0.5f }, o[8];
    ledger_truth(in[0], in[1], o);
    o[5] += 0.4f;
    int new_bad = iris_record(k, in, o);
    int ret = iris_train_elm(k, 1e-4f, scratch, sizeof scratch);
    int after = iris_worst_example_id(k, 0);

    /* and every stored value is exactly that demonstration's squared miss
       under the solve, in normalised units; every slot past the last is 0,
       even after a backprop run over more demonstrations than the solve saw
       (iris_clear and iris_record leave the ledger alone, so a slot left
       holding an old sum would be read again by the next take recorded) */
    int exact = (k->res_epochs == 1);
    iris_train(k);
    iris_clear(k);
    xs = 99u;
    ledger_session(k, 13, 0, 0, 0.0f);
    int ret2 = iris_train_elm(k, 1e-4f, scratch, sizeof scratch);
    if (ret2 < 0 || k->res_epochs != 1) exact = 0;
    for (int n = 0; n < k->cap; ++n) {
      float want = 0.0f;
      if (n < k->n_ex) {
        float x[2], ti[2], to[8];
        iris_get(k, n, ti, to);
        for (int i = 0; i < 2; ++i) x[i] = iris_internal_norm_in(k, i, ti[i]);
        iris_internal_forward_norm(k, x);
        for (int j = 0; j < 8; ++j) { float e = k->out[j] - iris_internal_norm_out(k, j, to[j]); want += e * e; }
      }
      if (bits(k->ex_res[n]) != bits(want)) exact = 0;
    }
    snprintf(d, sizeof d, "solve only: worst id %d (bad %d); after backprop, delete and a new "
             "bad take: worst id %d (bad %d), ret %d, entries exact %d",
             elm_only, bad_id, after, new_bad, ret, exact);
    check("a solve refreshes the worst-example ledger",
          elm_only == bad_id && after == new_bad && ret >= 0 && exact, d);
  }

  /* ---- 5. a constant is not a collapse when a constant is the answer --------- */
  {
    const float cv[7] = { 0.0f, 0.5f, 1.0f, 1.5f, 7.0f, 100.0f, -100.0f };
    int wrong = 0; char first[80] = "";
    for (int c = 0; c < 7; ++c) {
      iris *k = iris_init(arena, sizeof arena, 2, 12, 2, 128, 1234u);
      for (int r = 0; r < 6; ++r) { float in[2] = { (float)(r % 3), (float)(r / 3) }, o[2] = { cv[c], -cv[c] };
                                    iris_record(k, in, o); }
      int ret = iris_train_elm(k, 1e-4f, scratch, sizeof scratch);
      if (ret < 0 || iris_get_status(k) == IRIS_SOLVE_COLLAPSED) {
        if (!wrong) snprintf(first, sizeof first, "; first at %g: ret %d status %d", (double)cv[c], ret, iris_get_status(k));
        wrong++; }
    }
    /* two contradictory takes at one gesture: the mean is the right answer */
    iris *k = iris_init(arena, sizeof arena, 2, 12, 1, 128, 1234u);
    for (int r = 0; r < 6; ++r) { float in[2] = { 3.0f, 4.0f }, o = (r & 1) ? 10.0f : 20.0f; iris_record(k, in, &o); }
    int ret = iris_train_elm(k, 1e-4f, scratch, sizeof scratch);
    int same_gesture = (ret >= 0 && iris_get_status(k) != IRIS_SOLVE_COLLAPSED);
    snprintf(d, sizeof d, "%d of 7 constant outputs flagged%s; one gesture, two sounds: ret %d status %d",
             wrong, first, ret, iris_get_status(k));
    check("constant outputs and one-gesture sessions are not collapses", wrong == 0 && same_gesture, d);

    /* the check still fires: an enormous ridge, and a frozen layer that ignores
       its inputs, both map every gesture to one sound. The report is a status
       of its own, IRIS_SOLVE_COLLAPSED, whose value 7 a sketch may print, and
       it must never lock the warm trainers, which refuse only a diverged
       gradient run: one warm epoch from the collapsed solve trains. */
    const struct recipe *r = &R[0];
    iris *c1 = iris_init(arena, sizeof arena, r->ni, r->nh, r->no, 64, r->seed);
    record_recipe(c1, r);
    int r1 = iris_train_elm(c1, 1e6f, scratch, sizeof scratch);
    int s1 = iris_get_status(c1);
    float warm1 = iris_continue(c1, 1);
    iris_clear(c1); record_recipe(c1, r);
    int r2 = iris_internal_train_elm_ex(c1, 1e-4f, 0.0f, 1.0f, scratch, sizeof scratch);
    int s2 = iris_get_status(c1);
    float warm2 = iris_continue(c1, 1);
    int s2w = iris_get_status(c1);
    snprintf(d, sizeof d, "lam0 1e6: ret %d status %d, then a warm epoch %g; "
             "gain_w 0: ret %d status %d, then a warm epoch %g status %d",
             r1, s1, (double)warm1, r2, s2, (double)warm2, s2w);
    check("a real collapse is reported, and warm training still runs",
          (int)IRIS_SOLVE_COLLAPSED == 7
          && r1 >= 0 && s1 == IRIS_SOLVE_COLLAPSED && warm1 >= 0.0f
          && r2 >= 0 && s2 == IRIS_SOLVE_COLLAPSED && warm2 >= 0.0f
          && s2w != IRIS_DIVERGED_STUCK, d);
  }

  /* ---- 6. the scratch at every byte offset ------------------------------------
     The instrument has IRIS_MAX_IN inputs, so the solve uses every byte of
     IRIS_ELM_SCRATCH, and exactly that many bytes are allocated behind each
     offset: the address sanitizer sees any access past the end, and the
     alignment sanitizer any access the solve did not align. Same weights at
     every offset. */
  {
    const size_t need = IRIS_ELM_SCRATCH(12, 3);
    int all = 1, ret0 = -2; uint32_t h0 = 0;
    for (int off = 0; off < 8; ++off) {
      unsigned char *block = (unsigned char *)malloc(need + (size_t)off);
      if (!block) { all = 0; break; }
      iris *k = iris_init(wide, sizeof wide, IRIS_MAX_IN, 12, 3, 16, 77u);
      for (int n = 0; n < 16; ++n) {
        float in[IRIS_MAX_IN], o[3] = { 0.0f, 0.0f, 0.0f };
        for (int i = 0; i < IRIS_MAX_IN; ++i) {
          in[i] = frac((unsigned)(n * IRIS_MAX_IN + i) + 1u, 7919u, 97u);
          o[i % 3] += in[i] / (float)IRIS_MAX_IN;
        }
        iris_record(k, in, o);
      }
      int ret = iris_train_elm(k, 1e-4f, block + off, need);
      uint32_t h = weight_hash(k);
      if (off == 0) { h0 = h; ret0 = ret; }
      if (ret < 0 || ret != ret0 || h != h0) all = 0;
      free(block);
    }
    snprintf(d, sizeof d, "%d inputs, offsets 0..7 with exactly %zu bytes: %s", IRIS_MAX_IN, need,
             all ? "identical weights" : "DIFFERENT or refused");
    check("the scratch works at any alignment", all, d);
  }

  /* ---- 7. a solve ends a sliced run ------------------------------------------
     A run begun with iris_train_begin and still in flight would otherwise go
     on training from the solved weights at its next slice, and quietly
     replace the solve. A refused solve leaves the run going, like every other
     byte. The solve itself does not depend on what the run left in the
     weights: it gives recipe 0's pinned weights. */
  {
    const struct recipe *r = &R[0];
    iris *k = iris_init(arena, sizeof arena, r->ni, r->nh, r->no, 64, r->seed);
    record_recipe(k, r);
    iris_train_begin(k, 4000);
    iris_train_slice(k, 50);
    snap(k, (int)iris_get_status(k));
    int kept = refused_cleanly(iris_train_elm(k, -1.0f, scratch, sizeof scratch))
            && iris_train_busy(k);
    int ret = solve_recipe(k, r);
    uint32_t h = weight_hash(k);
    int more = iris_train_slice(k, 50);
    snprintf(d, sizeof d, "refused mid-run: run %s; solve ret %d weights 0x%08X; after: busy %d, slice %d, weights %s",
             kept ? "kept" : "DISTURBED", ret, h, iris_train_busy(k), more, weight_hash(k) == h ? "kept" : "MOVED");
    check("a solve ends a sliced run in flight", kept && ret == r->ret && h == r->weights
          && !iris_train_busy(k) && !more && weight_hash(k) == h, d);

    /* and afterwards the progress fields describe the solve, not the run
       before it: no epochs, and a finished fit. The same on an instrument
       that never trained any other way. */
    iris_train(k);
    const int before_epochs = iris_train_epochs_done(k);
    solve_recipe(k, r);
    const int ep = iris_train_epochs_done(k);
    const float pr = iris_train_progress(k);
    iris *f = iris_init(arena, sizeof arena, r->ni, r->nh, r->no, 64, r->seed);
    record_recipe(f, r);
    const float pr_fresh = iris_train_progress(f);
    solve_recipe(f, r);
    snprintf(d, sizeof d, "iris_train ran %d epochs; after a solve %d, progress %g; "
             "fresh: progress %g, after a solve %d epochs, progress %g",
             before_epochs, ep, (double)pr, (double)pr_fresh, iris_train_epochs_done(f),
             (double)iris_train_progress(f));
    check("after a solve, the progress fields describe the solve", before_epochs > 0
          && ep == 0 && pr == 1.0f && pr_fresh == 0.0f
          && iris_train_epochs_done(f) == 0 && iris_train_progress(f) == 1.0f, d);
  }

  /* ---- 8. weights past IRIS_W_LIMIT make an ordinary instrument ------------
     The limit detects a runaway gradient run; the closed-form solve is not
     one, and its output weights can legitimately lie beyond it. This recipe
     (a cubic in one input times a line in the other, on a 5 x 5 grid, nh 12,
     lam0 1e-4) solves to output weights past 16. The instrument must save,
     load and play the same bits. A warm trainer that continues from those
     weights is gradient training again: its first epoch clamps them to the
     limit and reports IRIS_TRAINING_DIVERGED, the next warm call refuses with
     IRIS_DIVERGED_STUCK, and iris_train, starting over from the seed, is the
     way from one trainer to the other. */
  {
    static unsigned char file[4096], other[IRIS_ARENA(2, 12, 1, 64)];
    iris *k = iris_init(arena, sizeof arena, 2, 12, 1, 64, 1u);
    for (int a = 0; a < 5; ++a) for (int b = 0; b < 5; ++b) {
      const float x = (float)a / 4.0f, y = (float)b / 4.0f, t = 2.0f * x - 1.0f;
      float in[2] = { x, y };
      float o = 0.5f + 0.4f * (4.0f * t * t * t - 3.0f * t) * (2.0f * y - 1.0f);
      iris_record(k, in, &o);
    }
    const int ret = iris_train_elm(k, 1e-4f, scratch, sizeof scratch);
    float big = 0.0f;
    for (int i = 0; i < 12 + 1; ++i) {
      const float w = i < 12 ? k->w2[i] : k->b2[0], m = w < 0.0f ? -w : w;
      if (m > big) big = m;
    }
    const size_t n = iris_save(k, file, sizeof file);
    iris *c = iris_init(other, sizeof other, 2, 12, 1, 64, 9u);
    const int loaded = n > 0 && iris_load(c, file, n);
    int same = loaded;
    for (int p = 0; p < 9 && same; ++p) {
      float in[2] = { (float)(p % 3) / 2.0f, (float)(p / 3) / 2.0f }, ya, yb;
      iris_predict(k, in, &ya); iris_predict(c, in, &yb);
      if (bits(ya) != bits(yb)) same = 0;
    }
    const float e1 = iris_continue(k, 1);
    const int st1 = (int)iris_get_status(k);
    const float e2 = iris_continue(k, 1);
    const int st2 = (int)iris_get_status(k);
    const int cold = iris_train(k);
    snprintf(d, sizeof d, "ret %d, largest output weight %.1f; save %zu, load %d, plays the same %d; "
             "warm: %g status %d, then %g status %d; iris_train %d",
             ret, (double)big, n, loaded, same, (double)e1, st1, (double)e2, st2, cold);
    check("a solve past IRIS_W_LIMIT saves, loads and plays", ret >= 0 && big > IRIS_W_LIMIT
          && loaded && same && e1 >= 0.0f && st1 == IRIS_TRAINING_DIVERGED
          && e2 == -1.0f && st2 == IRIS_DIVERGED_STUCK && cold == 1, d);
  }

  printf("\n  %s\n\n", fails ? "FAILURES ABOVE" : "all pass");
  return fails ? 1 : 0;
}
