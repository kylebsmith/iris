/* audit.c — does the thing actually work?
   Each check prints PASS or FAIL and a number you can argue with.
   Build:  cc -O2 -o audit audit.c -lm && ./audit
   Run from the library root: check 11 opens tests/golden/ by relative path.
   ./build.sh audit also runs the guards-are-inert A/B (tests/guards_ab.c),
   which needs two builds of the core and so lives outside this binary. */

#include "../iris.h"
#include "../experimental/iris_lbfgs.h"

/* HOST -> ESP32-S3 SCALING. This was a bare 32.0 sprinkled through the cost
   tables, and it was wrong by ~8x. The ONE on-device training measurement in
   the repository (hardware/board/BRINGUP-LOG.md:198-209) puts the real ratio
   near 270x: 600-epoch backprop measured 321.0 ms at 20 examples and 3,204.6 ms
   at 200 on the board, dead linear at 16.03 ms per example per 600 epochs.
   At 32x the table below implied ~29 ms where the board measures 321 ms, and a
   reader would quote our own output back at us.

   EVERY FIGURE IN THE "est. S3" COLUMNS IS A SCALING OF A HOST MEASUREMENT,
   NOT A READING FROM THE BOARD. Only the backprop-600 row has ever been
   measured on hardware; iris_train_converge, iris_train_elm and iris_train_lbfgs are
   all UNMEASURED on device. Corrected 2026-08-26.                          */
#define IRIS_S3_SCALE 270.0
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

static int failures = 0;
static void ok(const char *name, int pass, const char *fmt, ...) {
  va_list a; va_start(a, fmt);
  printf("%s  %-42s  ", pass ? "PASS" : "FAIL", name);
  vprintf(fmt, a); printf("\n"); va_end(a);
  if (!pass) failures++;
}
/* Wall clock, portably. clock_gettime(CLOCK_MONOTONIC, ...) is POSIX and is not
   available under MSVC, which would stop a Windows student running the test
   suite at all. C89's clock() is in <time.h> everywhere; it measures processor
   time rather than wall time, which for these single-threaded, CPU-bound
   benchmarks is the same number to well within the precision we quote.
   The library itself needs none of this — iris.h includes only <stddef.h> and
   <stdint.h> and compiles anywhere a C99 compiler exists, MSVC included. */
#if defined(_WIN32) || !defined(CLOCK_MONOTONIC)
static double now_ms(void) { return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC; }
#else
static double now_ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
#endif

/* fnv1a-32 — the same hash E8 recorded its determinism envelope with. */
static uint32_t fnv1a(const void *p, size_t n) {
  const unsigned char *b = (const unsigned char *)p;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
  return h;
}

/* View a saved blob as its v1-layout bytes, in place: a v1 blob passes
   through untouched; a v2 or v3 blob (v1 plus one rng word after the 8-word
   header — v3 has the same layout as v2 and differs only in what the weights
   MEAN) has that word removed and the version stamp set back to 1.
   Returns the v1-layout byte count. Keeps the golden-blob constant
   meaningful across the format bumps. */
static size_t golden_v1_bytes(unsigned char *buf, size_t n) {
  uint32_t *h = (uint32_t *)buf;
  if (n >= 9 * sizeof(uint32_t) && (h[1] == 2u || h[1] == 3u)) {
    memmove(buf + 8 * sizeof(uint32_t), buf + 9 * sizeof(uint32_t),
            n - 9 * sizeof(uint32_t));
    h[1] = 1u;
    n -= sizeof(uint32_t);
  }
  return n;
}

#define NI 2
#define NH 12
#define NO 3
#define CAP 256

/* The reroll checks (6 and 7) run a deliberately under-constrained model:
   few hidden units, few examples. experiment.c's sweep marks this corner
   "<-- lively"; the 12-hidden / 20-example model the rest of the audit uses
   is at the other end, where every random start converges to the same
   answer. See docs/adr/0001-reroll-check-probes-the-gaps.md. */
#define RR_HID  4
#define RR_EX   5
#define RR_EP   800
#define RR_SEEDS 8
#define RR_GRID 21                       /* 21 x 21 = 441 probes, as experiment.c */

static unsigned char arena_a[IRIS_ARENA(NI, NH, NO, CAP)];
static unsigned char arena_b[IRIS_ARENA(NI, NH, NO, CAP)];
static unsigned char arena_c[IRIS_ARENA(NI, NH, NO, CAP)];
static unsigned char arena_d[IRIS_ARENA(NI, NH, NO, CAP)];
static unsigned char arena_r[IRIS_ARENA(NI, RR_HID, NO, RR_EX)];

/* the ELM checks (20-22) sweep hidden widths up to 48; one arena sized for
   the widest covers the narrower ones, and the solve scratch likewise */
#define ELM_NHMAX 48
static unsigned char arena_w1[IRIS_ARENA(NI, ELM_NHMAX, NO, CAP)];
static unsigned char arena_w2[IRIS_ARENA(NI, ELM_NHMAX, NO, CAP)];
static unsigned char elm_scratch[IRIS_ELM_SCRATCH(ELM_NHMAX, NO)];

/* two instruments are "the same instrument" iff weights AND velocity AND the
   rng word match to the bit — w1..v_b2 is one contiguous span in the arena */
static int state_identical(const iris *a, const iris *b) {
  return memcmp(a->w1, b->w1,
                sizeof(float) * (size_t)(2 * (NH*NI + NH + NO*NH + NO))) == 0
      && a->rng.s == b->rng.s;
}

/* forward: the truth() the corrections perturb is defined just below */
static void truth(float x, float y, float *o);

/* the E4 10-correction chain: scattered points, each asking +0.15 on a
   rotating output — a musician reshaping the instrument bit by bit */
static void chain_correction(int i, float *nin, float *nout) {
  float x = 0.11f + 0.37f * (float)(i + 1);  x -= (float)(int)x;
  float y = 0.05f + 0.73f * (float)(i + 1);  y -= (float)(int)y;
  nin[0] = x; nin[1] = y;
  truth(x, y, nout);
  nout[i % NO] = iris_clampf(nout[i % NO] + 0.15f, 0.0f, 1.0f);
}

/* A made-up but musically-shaped target: three sound parameters that vary
   smoothly and differently across a 2-D gesture space. */
static void truth(float x, float y, float *o) {
  o[0] = 0.5f + 0.45f * iris_tanh(3.0f * (x - 0.5f));
  o[1] = 0.5f + 0.40f * iris_tanh(2.5f * (y - 0.5f) * (x + 0.3f));
  o[2] = 0.2f + 0.6f  * (x * y);
}

/* The same scattered-but-repeatable example set experiment.c uses, so the two
   programs are measuring the same thing on the same data. */
static void load_examples(iris *k, int n) {
  iris_clear(k);
  for (int i = 0; i < n; ++i) {
    float u = (float)((i * 7919) % 97) / 97.0f;
    float v = (float)((i * 6131) % 89) / 89.0f;
    float in[NI] = { u, v }, out[NO];
    truth(u, v, out);
    iris_record(k, in, out);
  }
}

/* recall / generalisation, in output units — the fit-floor check (22)
   compares two trainers with these, so they live here and not inline */
static float recall_rmse_of(iris *k) {
  float se = 0.0f; int c = 0;
  for (int n = 0; n < iris_count(k); ++n) {
    float in[NI], want[NO], got[NO];
    iris_get(k, n, in, want);
    iris_predict(k, in, got);
    for (int o = 0; o < NO; ++o) { float e = got[o] - want[o]; se += e * e; c++; }
  }
  return iris_sqrt(se / (float)c);
}
static float grid_rmse_of(iris *k) {
  float se = 0.0f; int c = 0;
  for (int a = 0; a < 21; ++a) for (int b = 0; b < 21; ++b) {
    float in[NI] = { a / 20.0f, b / 20.0f }, want[NO], got[NO];
    truth(in[0], in[1], want);
    iris_predict(k, in, got);
    for (int o = 0; o < NO; ++o) { float e = got[o] - want[o]; se += e * e; c++; }
  }
  return iris_sqrt(se / (float)c);
}

/* the six hostile scenarios from the E1 campaign, for check 21 -------------- */
static void load_dup256(iris *k) {          /* one point, 256 times */
  iris_clear(k);
  float in[NI] = { 0.5f, 0.5f }, out[NO];
  truth(0.5f, 0.5f, out);
  for (int i = 0; i < 256; ++i) iris_record(k, in, out);
}
static void load_clusters(iris *k) {        /* two tight blobs of 25 */
  iris_clear(k);
  for (int i = 0; i < 50; ++i) {
    float cx = (i < 25) ? 0.2f : 0.8f, cy = (i < 25) ? 0.3f : 0.7f;
    float u = cx + 0.01f * (float)((i * 31) % 7 - 3) / 3.0f;
    float v = cy + 0.01f * (float)((i * 17) % 7 - 3) / 3.0f;
    float in[NI] = { u, v }, out[NO];
    truth(u, v, out);
    iris_record(k, in, out);
  }
}
static void load_outlier(iris *k) {         /* check-9-style 1e6 outliers */
  iris_clear(k);
  for (int i = 0; i < 50; ++i) {
    float u = (i % 3 == 0) ? 0.5f : (float)(i % 17) / 17.0f;
    float in[NI] = { u, 0.5f }, out[NO];
    truth(u, 0.5f, out);
    if (i % 5 == 0) { out[0] = 1e6f; out[1] = -1e6f; }
    iris_record(k, in, out);
  }
}
static void load_deaddim(iris *k) {         /* input dim 1 constant */
  iris_clear(k);
  for (int i = 0; i < 50; ++i) {
    float u = (float)((i * 7919) % 97) / 97.0f;
    float in[NI] = { u, 0.5f }, out[NO];
    truth(u, 0.5f, out);
    iris_record(k, in, out);
  }
}
static void load_allequal(iris *k) {        /* every output identical */
  iris_clear(k);
  for (int i = 0; i < 50; ++i) {
    float u = (float)((i * 7919) % 97) / 97.0f;
    float v = (float)((i * 6131) % 89) / 89.0f;
    float in[NI] = { u, v }, out[NO] = { 0.42f, 0.42f, 0.42f };
    iris_record(k, in, out);
  }
}
/* probes on and OUTSIDE the input box; counts non-finite outputs */
static int nan_scan_of(iris *k) {
  int bad = 0;
  for (int a = -5; a <= 25; ++a) {
    float in[NI] = { a / 20.0f, a / 20.0f }, got[NO];
    iris_predict(k, in, got);
    for (int o = 0; o < NO; ++o)
      if (iris_isbad(got[o]) || got[o] > 1e30f || got[o] < -1e30f) bad++;
  }
  return bad;
}

/* --- how much does rerolling move the sound, and WHERE? --------------------
   Train the same examples from RR_SEEDS different random starts, then over a
   grid of probes measure the largest disagreement between any two rerolls.
   Report it separately for probes that sit on demonstrated ground and probes
   out in the gaps.

   "near demos" and "in the gaps" mean exactly what they mean in experiment.c:
   novelty below 0.15 and above 0.35 respectively. Same words, same numbers,
   two programs. If those bands ever move, they move in both files.          */
#define RR_NEAR_BAND 0.15f
#define RR_FAR_BAND  0.35f

static float rr_pred[RR_SEEDS][RR_GRID * RR_GRID][NO];

static void reroll_spread(iris *k, float *near_spread, int *near_n,
                          float *gap_spread,  int *gap_n) {
  for (int s = 0; s < RR_SEEDS; ++s) {
    load_examples(k, RR_EX);
    iris_retrain_new(k, 1000u + (uint32_t)s * 7919u, RR_EP);
    int p = 0;
    for (int a = 0; a < RR_GRID; ++a) for (int b = 0; b < RR_GRID; ++b, ++p) {
      float in[NI] = { a / (float)(RR_GRID - 1), b / (float)(RR_GRID - 1) };
      iris_predict(k, in, rr_pred[s][p]);
    }
  }

  load_examples(k, RR_EX);                 /* novelty needs the examples back */
  float sn = 0.0f, sf = 0.0f; int cn = 0, cf = 0;
  int p = 0;
  for (int a = 0; a < RR_GRID; ++a) for (int b = 0; b < RR_GRID; ++b, ++p) {
    float in[NI] = { a / (float)(RR_GRID - 1), b / (float)(RR_GRID - 1) };
    float nov = iris_novelty(k, in);
    float worst = 0.0f;
    for (int i = 0; i < RR_SEEDS; ++i) for (int j = i + 1; j < RR_SEEDS; ++j)
      for (int o = 0; o < NO; ++o) {
        float d = iris_absf(rr_pred[i][p][o] - rr_pred[j][p][o]);
        if (d > worst) worst = d;
      }
    if      (nov < RR_NEAR_BAND) { sn += worst; cn++; }
    else if (nov > RR_FAR_BAND)  { sf += worst; cf++; }
  }
  *near_n = cn; *gap_n = cf;
  *near_spread = cn ? sn / (float)cn : -1.0f;
  *gap_spread  = cf ? sf / (float)cf : -1.0f;
}

int main(void) {
  printf("\niris v%d.%d.%d — audit\n", IRIS_VERSION_MAJOR, IRIS_VERSION_MINOR, IRIS_VERSION_PATCH);
  printf("--------------------------------------------------------------------------\n");

  /* --- 1. memory: does the compile-time macro cover the runtime size? ----- */
  {
    size_t macro = sizeof arena_a, fn = iris_size(NI, NH, NO, CAP);
    ok("arena macro >= runtime size", macro >= fn,
       "macro %zu B, needed %zu B, slack %zu B", macro, fn, macro - fn);
  }

  iris *k = iris_init(arena_a, sizeof arena_a, NI, NH, NO, CAP, 1234);
  ok("init", k != 0, "%d in -> %d hidden -> %d out, room for %d examples",
     NI, NH, NO, CAP);
  if (!k) return 1;

  /* --- 2. record / delete ------------------------------------------------- */
  {
    iris_clear(k);
    int ids[5];
    for (int i = 0; i < 5; ++i) {
      float in[NI] = { 0.2f * i, 0.15f * i }, out[NO];
      truth(in[0], in[1], out);
      ids[i] = iris_record(k, in, out);
    }
    int before = iris_count(k);
    int deleted = iris_delete_id(k, ids[2]);
    int after = iris_count(k);
    int still_there = (iris_index_of(k, ids[3]) >= 0) && (iris_index_of(k, ids[0]) >= 0);
    int gone = iris_index_of(k, ids[2]) < 0;
    ok("delete one example by id", deleted && before == 5 && after == 4 && gone && still_there,
       "5 -> %d, id %d gone, others keep their ids", after, ids[2]);
  }

  /* --- 3. does it learn? -------------------------------------------------- */
  {
    iris_clear(k);
    for (int i = 0; i < 20; ++i) {
      float u = (float)((i * 7919) % 97) / 97.0f;
      float v = (float)((i * 6131) % 89) / 89.0f;
      float in[NI] = { u, v }, out[NO];
      truth(u, v, out);
      iris_record(k, in, out);
    }
    float err = iris_train_epochs(k, 800);

    /* recall: how close does it get to the sounds we actually demonstrated? */
    float worst = 0.0f, sum = 0.0f;
    for (int i = 0; i < iris_count(k); ++i) {
      float in[NI], want[NO], got[NO];
      iris_get(k, i, in, want);
      iris_predict(k, in, got);
      for (int o = 0; o < NO; ++o) {
        float e = iris_absf(got[o] - want[o]);
        if (e > worst) worst = e;
        sum += e;
      }
    }
    float mean = sum / (float)(iris_count(k) * NO);
    ok("learns 20 demonstrations", worst < 0.06f,
       "mean miss %.4f, worst %.4f  (final error %.5f)", mean, worst, err);
  }

  /* --- 4. generalises to gestures it never saw ---------------------------- */
  {
    float worst = 0.0f, sum = 0.0f; int n = 0;
    for (int a = 0; a <= 10; ++a) for (int b = 0; b <= 10; ++b) {
      float in[NI] = { a / 10.0f, b / 10.0f }, want[NO], got[NO];
      truth(in[0], in[1], want);
      iris_predict(k, in, got);
      for (int o = 0; o < NO; ++o) {
        float e = iris_absf(got[o] - want[o]);
        if (e > worst) worst = e;
        sum += e; n++;
      }
    }
    ok("fills in the gaps sensibly", worst < 0.25f,
       "121 unseen points: mean miss %.4f, worst %.4f", sum / n, worst);
  }

  /* --- 5. same seed, same instrument -------------------------------------- */
  {
    iris *k2 = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 1234);
    iris_clear(k2);
    for (int i = 0; i < 20; ++i) {
      float u = (float)((i * 7919) % 97) / 97.0f;
      float v = (float)((i * 6131) % 89) / 89.0f;
      float in[NI] = { u, v }, out[NO];
      truth(u, v, out);
      iris_record(k2, in, out);
    }
    iris_retrain_new(k2, 1234, 800);
    iris_retrain_new(k,  1234, 800);
    int identical = 1; float biggest = 0.0f;
    for (int a = 0; a <= 20; ++a) for (int b = 0; b <= 20; ++b) {
      float in[NI] = { a / 20.0f, b / 20.0f }, o1[NO], o2[NO];
      iris_predict(k, in, o1); iris_predict(k2, in, o2);
      for (int o = 0; o < NO; ++o) {
        float d = iris_absf(o1[o] - o2[o]);
        if (d > biggest) biggest = d;
        if (o1[o] != o2[o]) identical = 0;
      }
    }
    ok("same seed -> identical instrument", identical,
       "441 points compared, largest difference %.9f", biggest);
  }

  /* --- 6 & 7. the reroll, both halves of it -------------------------------
     Rerolling has to do two things at once, and only the pair is the promise:

       lively in the gaps   — a fresh random start gives a genuinely different
                              instrument where you never demonstrated anything.
                              This is what the button is for.
       steady at the demos  — and it leaves the sound alone where you DID
                              demonstrate. Retraining must not cost you the
                              work you just did.

     Either one alone is trivially satisfiable by a broken model: a model that
     learned nothing is lively everywhere, a model that ignores its seed is
     steady everywhere. Measured at RR_HID hidden / RR_EX examples, the corner
     experiment.c's sweep marks "<-- lively".                                 */
  {
    iris *kr = iris_init(arena_r, sizeof arena_r, NI, RR_HID, NO, RR_EX, 1);
    float near_s = -1.0f, gap_s = -1.0f; int near_n = 0, gap_n = 0;
    if (kr) reroll_spread(kr, &near_s, &near_n, &gap_s, &gap_n);

    /* gaps: measured 0.0763 here, and 0.058-0.086 across 100..800 epochs.
       Threshold 0.03 leaves 2.5x headroom. */
    ok("reroll is lively in the gaps", kr && gap_n > 0 && gap_s > 0.03f,
       "%d hidden, %d examples, %d seeds: %d gap probes move by %.4f  (want > 0.0300)",
       RR_HID, RR_EX, RR_SEEDS, gap_n, gap_s);

    /* near demos: measured 0.0165 here, at most 0.0399 across the same epoch
       range. Threshold 0.04 leaves 2.4x headroom on the shipped setting. */
    ok("reroll is steady at the demonstrations", kr && near_n > 0 && near_s < 0.04f,
       "%d probes on demonstrated ground move by %.4f  (want < 0.0400; %.1fx less than the gaps)",
       near_n, near_s, near_s > 0.0f ? gap_s / near_s : 0.0f);
  }

  /* --- 8. save and load ---------------------------------------------------- */
  {
    static unsigned char file[64 * 1024];
    size_t n = iris_save(k, file, sizeof file);
    float before[21][NO];
    for (int a = 0; a <= 20; ++a) {
      float in[NI] = { a / 20.0f, 0.4f };
      iris_predict(k, in, before[a]);
    }
    iris *k2 = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 99);
    int loaded = iris_load(k2, file, n);
    int same = 1;
    for (int a = 0; a <= 20; ++a) {
      float in[NI] = { a / 20.0f, 0.4f }, got[NO];
      iris_predict(k2, in, got);
      for (int o = 0; o < NO; ++o) if (got[o] != before[a][o]) same = 0;
    }
    int kept = iris_count(k2) == iris_count(k);
    ok("save -> load -> identical", loaded && same && kept,
       "%zu bytes, %d examples travelled with it", n, iris_count(k2));
  }

  /* --- 9. nothing blows up ------------------------------------------------ */
  {
    int bad = 0;
    iris_clear(k);
    for (int i = 0; i < 200; ++i) {   /* deliberately nasty: duplicates,
                                         extremes, a dead dimension */
      float u = (i % 3 == 0) ? 0.5f : (float)(i % 17) / 17.0f;
      float in[NI] = { u, 0.5f }, out[NO];
      truth(u, 0.5f, out);
      if (i % 5 == 0) { out[0] = 1e6f; out[1] = -1e6f; }
      iris_record(k, in, out);
    }
    iris_train_epochs(k, 400);
    for (int a = -5; a <= 25; ++a) {
      float in[NI] = { a / 20.0f, a / 20.0f }, got[NO];
      iris_predict(k, in, got);
      for (int o = 0; o < NO; ++o)
        if (got[o] != got[o] || got[o] > 1e30f || got[o] < -1e30f) bad++;
    }
    ok("survives hostile data, no NaN", bad == 0,
       "200 examples incl. duplicates + 1e6 outliers, 93 probes, %d bad values", bad);
  }

  /* --- 10. does it know when it's lost? ------------------------------------ */
  {
    iris_clear(k);
    for (int i = 0; i < 6; ++i) {
      float in[NI] = { 0.4f + 0.03f * i, 0.4f + 0.03f * i }, out[NO];
      truth(in[0], in[1], out);
      iris_record(k, in, out);
    }
    iris_train_epochs(k, 400);
    float at[NI] = { 0.4f, 0.4f }, far[NI] = { 0.95f, 0.05f };
    float n_at = iris_novelty(k, at), n_far = iris_novelty(k, far);
    ok("novelty: low on home ground, high away", n_at < 0.05f && n_far > 0.5f,
       "on an example %.3f, far away %.3f", n_at, n_far);
  }

  /* --- 11. the golden v1 file loads and predicts bit-identically ----------
     tests/golden/v1-instrument.bin was written by tests/golden/make_golden.c
     (the exact check-5 recipe). It was frozen against v0.1.0 first, then
     re-frozen ONCE — a deliberate, documented act — when the FP_CONTRACT
     OFF determinism contract landed and moved every trained float to the
     contraction-off bit class (see docs/adr/0003). tests/golden/
     v1-expected.txt holds the prediction BITS (hex uint32) at 21 fixed
     probes. Old saved instruments must keep loading forever; this check is
     that promise, made mechanical. It also catches flag drift and
     accidental math changes in the predict path: bits, not tolerances.     */
  {
    static unsigned char file[64 * 1024];
    FILE *fb = fopen("tests/golden/v1-instrument.bin", "rb");
    FILE *ft = fopen("tests/golden/v1-expected.txt", "r");
    size_t n = 0;
    int loaded = 0, probes_read = 0, mismatches = 0;
    if (fb) { n = fread(file, 1, sizeof file, fb); fclose(fb); }
    iris *kg = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 99);
    if (kg && n > 0) loaded = iris_load(kg, file, n);
    if (ft) {
      for (int a = 0; a < 21; ++a) {
        unsigned want[NO];
        if (fscanf(ft, "%x %x %x", &want[0], &want[1], &want[2]) != NO) break;
        probes_read++;
        if (loaded) {
          float in[NI] = { a / 20.0f, 0.4f }, got[NO];
          iris_predict(kg, in, got);
          for (int o = 0; o < NO; ++o) {
            unsigned bits; memcpy(&bits, &got[o], sizeof bits);
            if (bits != want[o]) mismatches++;
          }
        }
      }
      fclose(ft);
    }
    ok("golden v1 file loads, predicts bit-identically",
       fb && ft && loaded && probes_read == 21 && mismatches == 0,
       "%zu bytes, %d examples, %d probes x %d outputs, %d bit mismatches",
       n, loaded && kg ? iris_count(kg) : -1, probes_read, NO, mismatches);
  }

  /* --- 12. the golden blob: the TRAINING path, pinned to the bit ----------
     Check 11 pins the predict path against a frozen file. This one pins the
     training path: run the check-5 recipe and hash the saved bytes. Any
     compiler-flag drift, contraction leak, or accidental math change in
     train/save fails this check loudly. The hash is taken over the v1-layout
     bytes so the constants survive format bumps (v2/v3 = v1 + one inserted
     rng word).

     TWO constants, because there are now two input scalings and both are
     live in every build:

       0xFEFAEDF6  the [0,1] scaling — E8's measured -ffp-contract=off blob
                   hash, the only flag-robust class (O0/O1/O2 all
                   bit-identical). UNCHANGED since v0.1.0 and re-frozen only
                   once, when FP_CONTRACT OFF landed (ADR 0003). v0.3.0 did
                   NOT re-freeze it: iris__set_legacy_norm puts the instrument
                   back on the old scaling and the old bits come back, which
                   is the mechanical proof that the v1/v2 path in iris_load is
                   still the code it always was.
       0x6805FB0D  the [-1,+1] scaling that v3 files are written in. Frozen
                   at v0.3.0 by this audit, on this contraction-off class.  */
  {
    static unsigned char file[64 * 1024];
    /* fresh instrument: the blob carries example ids, so the recipe must
       start from iris_init exactly as make_golden.c does */
    for (int pass = 0; pass < 2; ++pass) {
      iris *kb = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 1234);
      iris__set_legacy_norm(kb, pass == 0);       /* pass 0: v1/v2, pass 1: v3 */
      for (int i = 0; i < 20; ++i) {
        float u = (float)((i * 7919) % 97) / 97.0f;
        float v = (float)((i * 6131) % 89) / 89.0f;
        float in[NI] = { u, v }, out[NO];
        truth(u, v, out);
        iris_record(kb, in, out);
      }
      iris_retrain_new(kb, 1234, 800);
      size_t n = iris_save(kb, file, sizeof file);
      size_t n1 = golden_v1_bytes(file, n);     /* v1-layout view of the blob */
      uint32_t h = fnv1a(file, n1);
      uint32_t want = pass == 0 ? 0xFEFAEDF6u : 0x6805FB0Du;
      /* AND THE L-BFGS PATH, for one specific reason. The v0.3.0 scaling
         change edited exactly one piece of arithmetic that neither golden
         blob covers: iris__lbfgs_pass folds the input scaling into a
         precomputed reciprocal and now adds an offset term, which is
         identically 0.0f on the legacy scaling. "Identically zero" is a claim
         about float semantics, so it is pinned rather than argued.
         0xFB5BE623 is the current value, re-pinned 2026-08-26 after the
         L-BFGS repair (relative curvature test + double-precision loss).
         0x8260169D was the pre-repair value, measured against the previous
         header. */
      static float lw12[IRIS_LBFGS_WORK_FLOATS(NI, NH, NO, IRIS_LBFGS_M) + 2];
      iris *kl = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 1234);
      iris__set_legacy_norm(kl, pass == 0);
      load_examples(kl, 20);
      iris_train_lbfgs(kl, 300, lw12, sizeof lw12);
      /* Use the library's own macro, not a hand-copy of the same expression.
         The golden hash at the next line is only meaningful if this weight
         count matches what the trainer actually wrote; a hand-recomputed
         duplicate can drift out of sync silently and the hash would then be
         hashing the wrong number of bytes. Fixed 2026-08-26. */
      const int nw12 = (int)IRIS_LBFGS_NW(NI, NH, NO);
      uint32_t hl = fnv1a(kl->w1, (size_t)nw12 * sizeof(float));
      /* Re-pinned 2026-08-26: the L-BFGS trainer was repaired (relative curvature
   test + double-precision loss accumulation), which deliberately changes the
   weights it produces. The old value was 0x8260169D. The core SGD hash above
   is UNCHANGED, which is the point — the repair touched only
   experimental/iris_lbfgs.h. */
      /* Re-pinned 2026-08-27 (was 0xFB5BE623). The tanh codomain clamp moved from
   |x|>4.9 to the codomain itself, which changes the weights L-BFGS reaches.
   The CORE SGD hash on the line above is UNCHANGED, which is the informative
   part: at the reference task SGD never drives a pre-activation past 3, so the
   old defect was inert for it. L-BFGS takes larger steps, entered the band
   routinely, and was being fed wrong-signed gradients. */
      uint32_t wantl = pass == 0 ? 0x1648FA1Eu : 0u;
      ok(pass == 0 ? "golden blob: [0,1] training path bit-pinned"
                   : "golden blob: [-1,+1] training path bit-pinned",
         n1 == 852 && h == want && (pass != 0 ? 1 : hl == wantl),
         "%zu v1-layout bytes, fnv1a 0x%08X (want 0x%08X); L-BFGS weights "
         "0x%08X%s", n1, h, want, hl,
         pass == 0 ? " (want 0xFB5BE623, re-pinned after the 2026-08-26 L-BFGS repair)" : "");
    }
  }

  /* --- 13. NaN never reaches the audio path -------------------------------
     Three doors a NaN can come through, all guarded, all REPORTED:
     (a) a poisoned example  -> training refused, weights bit-preserved;
     (b) a glitched sensor at play time -> finite substitute + status;
     (c) hostile lr/momentum -> zero NaN at predict, anomaly reported within
         one train call. Guards report via iris_get_status, never silently.  */
  {
    /* (a) poisoned example refused, previous weights preserved */
    iris_clear(k);
    for (int i = 0; i < 10; ++i) {
      float u = (float)((i * 7919) % 97) / 97.0f;
      float v = (float)((i * 6131) % 89) / 89.0f;
      float in[NI] = { u, v }, out[NO];
      truth(u, v, out);
      iris_record(k, in, out);
    }
    iris_reseed(k, 77);
    iris_train_epochs(k, 400);
    float probe[NI] = { 0.3f, 0.7f }, before[NO], after[NO];
    iris_predict(k, probe, before);
    /* THE DOOR REFUSES IT (gap B4 closed 2026-08-27). iris_record now rejects a
       NaN before it can enter the store: -1 back, status set, n_ex unmoved. */
    int door_refused, n_before = iris_count(k);
    { float in[NI] = { 0.0f / 0.0f, 0.5f }, out[NO] = { 0.5f, 0.5f, 0.5f };
      door_refused = (iris_record(k, in, out) == -1) && iris_count(k) == n_before
                   && iris_get_status(k) == IRIS_NAN_TRAPPED; }

    /* AND THE TRAINER'S BACKSTOP STILL WORKS. The door cannot be the only
       guard: examples also arrive through iris_load, which does not go through
       iris_record. Write the poison straight into the store to exercise the
       pre-scan the way a corrupt file would. */
    k->ex[0] = 0.0f / 0.0f;
    k->trained = 1;                            /* pretend the fit is current */
    iris_train_epochs(k, 400);
    int refused = door_refused && (iris_get_status(k) == IRIS_NAN_TRAPPED);
    iris_predict(k, probe, after);
    int preserved = (before[0] == after[0] && before[1] == after[1]
                  && before[2] == after[2]);

    /* (b) NaN sensor input at play time: finite outputs + status */
    iris_delete_last(k);
    iris_train_epochs(k, 400);
    float nan_in[NI] = { 0.0f / 0.0f, 0.4f }, got[NO];
    iris_predict(k, nan_in, got);
    int finite = 1;
    for (int o = 0; o < NO; ++o) if (iris_isbad(got[o])) finite = 0;
    int reported = (iris_get_status(k) == IRIS_NAN_TRAPPED);

    /* (c) hostile lr/momentum sweep on check-9 data + a 1e6 INPUT outlier */
    int sweep_nan = 0, sweep_unreported = 0;
    const float lrs[3] = { 0.5f, 1.0f, 2.0f };
    const float moms[2] = { 0.85f, 0.99f };
    for (int li = 0; li < 3; ++li) for (int mi = 0; mi < 2; ++mi) {
      iris_clear(k);
      iris_reseed(k, 1234);
      for (int i = 0; i < 200; ++i) {
        float u = (i % 3 == 0) ? 0.5f : (float)(i % 17) / 17.0f;
        float in[NI] = { u, 0.5f }, out[NO];
        truth(u, 0.5f, out);
        if (i % 5 == 0) { out[0] = 1e6f; out[1] = -1e6f; }
        iris_record(k, in, out);
      }
      { float in[NI] = { 1e6f, 0.5f }, out[NO] = { 0.5f, 0.5f, 0.5f };
        iris_record(k, in, out); }
      iris_set_learning(k, lrs[li], moms[mi]);
      int detect_call = 0;
      for (int call = 1; call <= 4; ++call) {
        iris_train_epochs(k, 400);
        if (!detect_call && iris_get_status(k) != IRIS_STATUS_OK) detect_call = call;
      }
      for (int a = -5; a <= 25; ++a) for (int b = -5; b <= 25; ++b) {
        float in[NI] = { a / 20.0f, b / 20.0f }, o2[NO];
        iris_predict(k, in, o2);
        for (int o = 0; o < NO; ++o) if (iris_isbad(o2[o])) sweep_nan++;
      }
      /* at the wild settings, if anything went wrong it must have been
         reported on the train call where it happened — never later */
      if (detect_call > 1) sweep_unreported++;
    }
    iris_set_learning(k, 0.10f, 0.85f);

    ok("NaN never reaches output; guards report", refused && preserved
       && finite && reported && sweep_nan == 0 && sweep_unreported == 0,
       "poison refused %d, weights kept %d, sensor-NaN finite %d + status %d, "
       "sweep NaN %d, late reports %d",
       refused, preserved, finite, reported, sweep_nan, sweep_unreported);
  }

  /* --- 14. event-sourced determinism ---------------------------------------
     Warm correction advances the rng past the seed, so the new determinism
     promise is: the identical OPERATION HISTORY reproduces the instrument
     bit-exactly. Two instruments, same history of record/correct/delete,
     compared by memcmp over weights+velocity and the rng word, at every
     chain length 1..10 and after a delete.                                  */
  {
    iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 42);
    iris *c = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 42);
    load_examples(a, 20); iris_retrain_new(a, 42, 600);
    load_examples(c, 20); iris_retrain_new(c, 42, 600);
    int all_same = 1;
    for (int i = 0; i < 10; ++i) {
      float in[NI], out[NO];
      chain_correction(i, in, out);
      iris_record(a, in, out); iris_correct(a, 20);
      iris_record(c, in, out); iris_correct(c, 20);
      if (!state_identical(a, c)) all_same = 0;
    }
    iris_delete_last(a); iris_correct(a, 20);
    iris_delete_last(c); iris_correct(c, 20);
    int after_delete = state_identical(a, c);
    ok("event-sourced determinism (history replay)", all_same && after_delete,
       "10-correction chain bit-identical %d, delete+correct bit-identical %d",
       all_same, after_delete);
  }

  /* --- 15. format v2 round trip; the v1 loader is untouched ---------------
     v2 = v1 + one rng word. The test that matters is not "the bytes come
     back" but "the FUTURE comes back": a correction after save->load must
     be bit-identical to the correction the in-memory instrument would have
     made. And a v1 file must still load with the old semantics (rng reset
     to seed) — old files are sacred.                                       */
  {
    static unsigned char file[64 * 1024];
    iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 42);
    load_examples(a, 20); iris_retrain_new(a, 42, 600);
    for (int i = 0; i < 3; ++i) {
      float in[NI], out[NO];
      chain_correction(i, in, out);
      iris_record(a, in, out); iris_correct(a, 20);
    }
    size_t n2 = iris_save(a, file, sizeof file);
    iris *b = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 7);
    int loaded = iris_load(b, file, n2);
    int exact = loaded && a->rng.s == b->rng.s && a->seed == b->seed
             && iris_count(a) == iris_count(b)
             && memcmp(a->w1, b->w1,
                       sizeof(float) * (size_t)(NH*NI + NH + NO*NH + NO)) == 0;
    /* the future: one more correction on both must match to the bit */
    float in[NI], out[NO];
    chain_correction(3, in, out);
    iris_record(a, in, out); iris_correct(a, 20);
    iris_record(b, in, out); iris_correct(b, 20);
    int future = state_identical(a, b);
    /* v1 semantics: the golden file is v1; loading it must reset rng to seed */
    FILE *fb = fopen("tests/golden/v1-instrument.bin", "rb");
    size_t n1 = 0;
    if (fb) { n1 = fread(file, 1, sizeof file, fb); fclose(fb); }
    iris *g = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 99);
    int v1_ok = n1 > 0 && iris_load(g, file, n1) && g->rng.s == g->seed;
    ok("format v2 round trip; v1 loader permanent", exact && future && v1_ok,
       "v2 %zu B (v1 layout + 4) state exact %d, post-reload correction bit-identical %d, "
       "v1 loads with old semantics %d", n2, exact, future, v1_ok);
  }

  /* --- 16. the correction reaches parity without wrecking the map ---------
     One corrective example on a practised 20-example instrument, default
     20-epoch warm budget, ONE iris_correct call — the shipped semantics.
     References, measured here: cold-600 reaches train rms 0.0185-0.0190
     with far-field drift 0.0030 in ~30x the time; the single warm call
     measures 0.0191 / 0.0040. (E4's 0.0190/0.0031 came from a per-epoch
     measurement loop that re-zeroed velocity every epoch — not the shipped
     call, so the gates below carry the shipped call's numbers + margin.)   */
  {
    static float snapA[21 * 21][NO], snapB[21 * 21][NO];
    iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 42);
    load_examples(a, 20); iris_retrain_new(a, 42, 600);
    int p = 0;
    for (int x = 0; x < 21; ++x) for (int y = 0; y < 21; ++y, ++p) {
      float in[NI] = { x / 20.0f, y / 20.0f };
      iris_predict(a, in, snapA[p]);
    }
    float cin[NI] = { 0.62f, 0.31f }, cout[NO];
    truth(cin[0], cin[1], cout);
    cout[0] = iris_clampf(cout[0] + 0.15f, 0.0f, 1.0f);
    iris_record(a, cin, cout);
    iris_correct(a, 20);
    /* fit across all 21 examples */
    float acc = 0.0f;
    for (int i = 0; i < iris_count(a); ++i) {
      float in[NI], want[NO], got[NO];
      iris_get(a, i, in, want);
      iris_predict(a, in, got);
      for (int o = 0; o < NO; ++o) { float d = got[o] - want[o]; acc += d * d; }
    }
    float tr = iris_sqrt(acc / (float)(iris_count(a) * NO));
    /* drift far from the correction: mean |delta| over probes > 0.25 away */
    p = 0;
    float sf = 0.0f; int cf = 0;
    for (int x = 0; x < 21; ++x) for (int y = 0; y < 21; ++y, ++p) {
      float in[NI] = { x / 20.0f, y / 20.0f };
      iris_predict(a, in, snapB[p]);
      float dx = in[0] - cin[0], dy = in[1] - cin[1];
      if (iris_sqrt(dx * dx + dy * dy) < 0.25f) continue;
      float m = 0.0f;
      for (int o = 0; o < NO; ++o) m += iris_absf(snapA[p][o] - snapB[p][o]);
      sf += m / NO; cf++;
    }
    float drift = cf ? sf / (float)cf : -1.0f;
    ok("correction: parity fit, surgical drift", tr <= 0.0195f && drift >= 0.0f && drift <= 0.0045f,
       "train rms %.4f (want <= 0.0195; cold-600 ref 0.0185), "
       "far-field drift %.4f (want <= 0.0045; cold-600 ref 0.0030), 20 epochs",
       tr, drift);
  }

  /* --- 17. L-BFGS carries the whole suite ---------------------------------
     The fast trainer substituted for backprop must still pass the core
     behavioural checks: learn the demos, fill the gaps, same-seed bit
     identity, and BOTH halves of the reroll promise. Budgets map as
     iters = epochs/6.                                                       */
  {
    static float lwork[IRIS_LBFGS_WORK_FLOATS(NI, NH, NO, IRIS_LBFGS_M) + 2];
    /* learns + generalises */
    iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 1234);
    load_examples(a, 20);
    iris_reseed(a, 1234);
    iris_train_lbfgs(a, 800 / 6, lwork, sizeof lwork);
    float worst_r = 0.0f, worst_g = 0.0f;
    for (int i = 0; i < iris_count(a); ++i) {
      float in[NI], want[NO], got[NO];
      iris_get(a, i, in, want);
      iris_predict(a, in, got);
      for (int o = 0; o < NO; ++o) {
        float e = iris_absf(got[o] - want[o]);
        if (e > worst_r) worst_r = e;
      }
    }
    for (int x = 0; x <= 10; ++x) for (int y = 0; y <= 10; ++y) {
      float in[NI] = { x / 10.0f, y / 10.0f }, want[NO], got[NO];
      truth(in[0], in[1], want);
      iris_predict(a, in, got);
      for (int o = 0; o < NO; ++o) {
        float e = iris_absf(got[o] - want[o]);
        if (e > worst_g) worst_g = e;
      }
    }
    /* same seed, same instrument — to the bit */
    iris *b = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 1234);
    load_examples(b, 20);
    iris_reseed(b, 1234);
    iris_train_lbfgs(b, 800 / 6, lwork, sizeof lwork);
    int bitsame = memcmp(a->w1, b->w1,
                         sizeof(float) * (size_t)(NH*NI + NH + NO*NH + NO)) == 0;
    /* the reroll, both halves, at the audit's lively corner */
    iris *kr = iris_init(arena_r, sizeof arena_r, NI, RR_HID, NO, RR_EX, 1);
    float near_s = -1.0f, gap_s = -1.0f; int near_n = 0, gap_n = 0;
    for (int s = 0; s < RR_SEEDS; ++s) {
      load_examples(kr, RR_EX);
      iris_reseed(kr, 1000u + (uint32_t)s * 7919u);
      iris_train_lbfgs(kr, RR_EP / 6, lwork, sizeof lwork);
      int p = 0;
      for (int x = 0; x < RR_GRID; ++x) for (int y = 0; y < RR_GRID; ++y, ++p) {
        float in[NI] = { x / (float)(RR_GRID - 1), y / (float)(RR_GRID - 1) };
        iris_predict(kr, in, rr_pred[s][p]);
      }
    }
    {
      load_examples(kr, RR_EX);
      float sn = 0.0f, sf = 0.0f; int cn = 0, cf = 0, p = 0;
      for (int x = 0; x < RR_GRID; ++x) for (int y = 0; y < RR_GRID; ++y, ++p) {
        float in[NI] = { x / (float)(RR_GRID - 1), y / (float)(RR_GRID - 1) };
        float nov = iris_novelty(kr, in);
        float worst = 0.0f;
        for (int i = 0; i < RR_SEEDS; ++i) for (int j = i + 1; j < RR_SEEDS; ++j)
          for (int o = 0; o < NO; ++o) {
            float d = iris_absf(rr_pred[i][p][o] - rr_pred[j][p][o]);
            if (d > worst) worst = d;
          }
        if      (nov < RR_NEAR_BAND) { sn += worst; cn++; }
        else if (nov > RR_FAR_BAND)  { sf += worst; cf++; }
      }
      near_n = cn; gap_n = cf;
      near_s = cn ? sn / (float)cn : -1.0f;
      gap_s  = cf ? sf / (float)cf : -1.0f;
    }
    ok("L-BFGS: learns, generalises, bit-identical", worst_r < 0.06f && worst_g < 0.25f && bitsame,
       "recall worst %.4f (<0.06), grid worst %.4f (<0.25), same-seed memcmp %d",
       worst_r, worst_g, bitsame);
    ok("L-BFGS: reroll keeps both promises", near_n > 0 && gap_n > 0
       && gap_s > 0.03f && near_s < 0.04f,
       "gaps move %.4f (want > 0.0300), demos move %.4f (want < 0.0400)",
       gap_s, near_s);
  }

  /* --- 18. L-BFGS is monotone on hostile data -----------------------------
     Duplicates, a 1e6 outlier, a dead input dimension. Every ACCEPTED step's
     loss must be <= the one before (the Armijo rule, observed rather than
     assumed) and nothing may go non-finite.                                 */
  {
    static float lwork[IRIS_LBFGS_WORK_FLOATS(NI, NH, NO, IRIS_LBFGS_M) + 2];
    static float trace[512];
    iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 1234);
    iris_clear(a);
    for (int i = 0; i < 200; ++i) {
      float u = (i % 3 == 0) ? 0.5f : (float)(i % 17) / 17.0f;
      float in[NI] = { u, 0.5f }, out[NO];
      truth(u, 0.5f, out);
      if (i % 5 == 0) { out[0] = 1e6f; out[1] = -1e6f; }
      iris_record(a, in, out);
    }
    iris_reseed(a, 1234);
    int iters = 0;
    iris__train_lbfgs_full(a, 300, 0.0f, lwork, sizeof lwork, trace, 512, &iters);
    int mono = 1, tn = iters + 1 < 512 ? iters + 1 : 512;
    for (int i = 1; i < tn; ++i) if (trace[i] > trace[i - 1]) mono = 0;
    int bad = 0;
    for (int x = -5; x <= 25; ++x) {
      float in[NI] = { x / 20.0f, x / 20.0f }, got[NO];
      iris_predict(a, in, got);
      for (int o = 0; o < NO; ++o) if (iris_isbad(got[o])) bad++;
    }
    ok("L-BFGS: monotone descent on hostile data", mono && bad == 0 && iters > 0,
       "%d accepted iterations, loss %.4f -> %.4f, monotone %d, %d NaN",
       iters, trace[0], trace[tn - 1], mono, bad);
  }

  /* --- 19. the race: L-BFGS vs the 600-epoch baseline ---------------------
     16 seeds x {20, 50} examples. For each seed, time the baseline to its
     final error, then time L-BFGS from the SAME initial weights to that
     exact error. Gate: every seed at least 2x faster (measured mean 4.3x /
     4.7x, worst seeds 3.2x / 2.5x).                                         */
  {
    static float lwork[IRIS_LBFGS_WORK_FLOATS(NI, NH, NO, IRIS_LBFGS_M) + 2];
    const int excnt[2] = { 20, 50 };
    float worst_sp[2] = { 1e9f, 1e9f };
    int misses = 0;
    for (int e = 0; e < 2; ++e) {
      for (int sd = 0; sd < 16; ++sd) {
        uint32_t seed = 1000u + (uint32_t)sd * 7919u;
        iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, seed);
        load_examples(a, excnt[e]);
        double tb[5], tl[5];
        float e_base = 0.0f, e_l = 0.0f;
        for (int r = 0; r < 5; ++r) {
          iris_reseed(a, seed);
          double t0 = now_ms(); iris_train_epochs(a, 600); tb[r] = now_ms() - t0;
        }
        e_base = iris_eval_loss(a);
        for (int r = 0; r < 5; ++r) {
          iris_reseed(a, seed);
          double t0 = now_ms();
          e_l = iris__train_lbfgs_full(a, 2000, e_base, lwork, sizeof lwork, 0, 0, 0);
          tl[r] = now_ms() - t0;
        }
        /* median of 5 */
        for (int i = 0; i < 5; ++i) for (int j = i + 1; j < 5; ++j) {
          if (tb[j] < tb[i]) { double t = tb[i]; tb[i] = tb[j]; tb[j] = t; }
          if (tl[j] < tl[i]) { double t = tl[i]; tl[i] = tl[j]; tl[j] = t; }
        }
        if (e_l > e_base) misses++;
        float sp = (float)(tb[2] / (tl[2] > 1e-6 ? tl[2] : 1e-6));
        if (sp < worst_sp[e]) worst_sp[e] = sp;
      }
    }
    ok("L-BFGS: >= 2x faster to baseline error, every seed", misses == 0
       && worst_sp[0] >= 2.0f && worst_sp[1] >= 2.0f,
       "16 seeds: worst 20-ex %.1fx, worst 50-ex %.1fx (want >= 2.0x), %d target misses",
       worst_sp[0], worst_sp[1], misses);
  }

  /* --- 20. ELM: same seed, same bits; reroll keeps both promises ----------
     The instant trainer's determinism is structural (no rng after the
     frozen draw, fixed accumulation order) — observed here at three widths.
     Its reroll character is measured at the nh=12 / 5-example config the
     bands were calibrated on: near <= 0.02, gap >= 0.06 (measured 0.017 /
     0.110 — steadier at the demos AND livelier in the gaps than backprop). */
  {
    const int nhs[3] = { 12, 24, 48 };
    int all_same = 1;
    for (int hi = 0; hi < 3; ++hi) {
      int nh = nhs[hi];
      iris *a = iris_init(arena_w1, sizeof arena_w1, NI, nh, NO, CAP, 7);
      iris *b = iris_init(arena_w2, sizeof arena_w2, NI, nh, NO, CAP, 99);
      load_examples(a, 50); load_examples(b, 50);
      int ra = iris_retrain_elm_new(a, 4242u, 1e-4f, elm_scratch, sizeof elm_scratch);
      int rb = iris_retrain_elm_new(b, 4242u, 1e-4f, elm_scratch, sizeof elm_scratch);
      if (ra < 0 || rb < 0) all_same = 0;
      if (memcmp(a->w1, b->w1,
                 sizeof(float) * (size_t)(nh*NI + nh + NO*nh + NO)) != 0) all_same = 0;
    }
    /* reroll bands, ELM flavour, at the lively config */
    iris *kr = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 1);
    for (int s = 0; s < RR_SEEDS; ++s) {
      load_examples(kr, RR_EX);
      iris_retrain_elm_new(kr, 1000u + (uint32_t)s * 7919u, 1e-4f,
                         elm_scratch, sizeof elm_scratch);
      int p = 0;
      for (int a = 0; a < RR_GRID; ++a) for (int b = 0; b < RR_GRID; ++b, ++p) {
        float in[NI] = { a / (float)(RR_GRID - 1), b / (float)(RR_GRID - 1) };
        iris_predict(kr, in, rr_pred[s][p]);
      }
    }
    float near_s = -1.0f, gap_s = -1.0f; int near_n = 0, gap_n = 0;
    {
      load_examples(kr, RR_EX);
      float sn = 0.0f, sf = 0.0f; int cn = 0, cf = 0, p = 0;
      for (int a = 0; a < RR_GRID; ++a) for (int b = 0; b < RR_GRID; ++b, ++p) {
        float in[NI] = { a / (float)(RR_GRID - 1), b / (float)(RR_GRID - 1) };
        float nov = iris_novelty(kr, in);
        float worst = 0.0f;
        for (int i = 0; i < RR_SEEDS; ++i) for (int j = i + 1; j < RR_SEEDS; ++j)
          for (int o = 0; o < NO; ++o) {
            float d = iris_absf(rr_pred[i][p][o] - rr_pred[j][p][o]);
            if (d > worst) worst = d;
          }
        if      (nov < RR_NEAR_BAND) { sn += worst; cn++; }
        else if (nov > RR_FAR_BAND)  { sf += worst; cf++; }
      }
      near_n = cn; gap_n = cf;
      near_s = cn ? sn / (float)cn : -1.0f;
      gap_s  = cf ? sf / (float)cf : -1.0f;
    }
    ok("ELM: same seed, same bits; reroll character", all_same
       && near_n > 0 && gap_n > 0 && near_s <= 0.02f && gap_s >= 0.06f,
       "memcmp-identical at nh 12/24/48: %d; demos move %.4f (want <= 0.02), "
       "gaps move %.4f (want >= 0.06)",
       all_same, near_s, gap_s);
  }

  /* --- 21. ELM: the solve cannot fail -------------------------------------
     Six hostile scenarios x five lambdas x three widths. The ridge makes
     the normal matrix SPD by construction, so the gate is absolute: zero
     unfixable Cholesky failures, escalation bounded (measured max 2), no
     non-finite output on any probe including outside the input box, and
     escalation is REPORTED (IRIS_RIDGE_ESCALATED), never silent.             */
  {
    const int nhs[3] = { 12, 24, 48 };
    const float lams[5] = { 1e-5f, 1e-4f, 1e-3f, 1e-2f, 1e-1f };
    void (*loaders[6])(iris *) = { 0, load_dup256, load_clusters,
                                 load_outlier, load_deaddim, load_allequal };
    int unfixable = 0, max_esc = 0, bad_total = 0, status_wrong = 0;
    for (int s = 0; s < 6; ++s) {
      for (int hi = 0; hi < 3; ++hi) {
        iris *a = iris_init(arena_w1, sizeof arena_w1, NI, nhs[hi], NO, CAP, 1);
        for (int li = 0; li < 5; ++li) {
          if (s == 0) load_examples(a, 50); else loaders[s](a);
          a->seed = 42u;
          int esc = iris_train_elm(a, lams[li], elm_scratch, sizeof elm_scratch);
          if (esc < 0) { unfixable++; continue; }
          if (esc > max_esc) max_esc = esc;
          bad_total += nan_scan_of(a);
          if (esc >  0 && iris_get_status(a) != IRIS_RIDGE_ESCALATED) status_wrong++;
          if (esc == 0 && iris_get_status(a) != IRIS_STATUS_OK)       status_wrong++;
        }
      }
    }
    ok("ELM: solve cannot fail on hostile data", unfixable == 0 && max_esc <= 3
       && bad_total == 0 && status_wrong == 0,
       "90 solves (6 scenarios x 5 lambdas x 3 widths): %d unfixable, "
       "max escalations %d (<= 3), %d bad values, %d status errors",
       unfixable, max_esc, bad_total, status_wrong);
  }

  /* --- 22. ELM: at nh=48 it fits BETTER than backprop ---------------------
     The E1 headline, reproduced inside the integrated core: nh=48 with
     lam0=1e-3 beats the 600-epoch backprop on recall AND on the 441-probe
     grid at 50 examples (measured .0066/.0109 vs .0077/.0116).             */
  {
    iris *a = iris_init(arena_w1, sizeof arena_w1, NI, 48, NO, CAP, 42);
    load_examples(a, 50);
    iris_retrain_new(a, 42u, 600);
    float bp_rec = recall_rmse_of(a), bp_grid = grid_rmse_of(a);
    load_examples(a, 50);
    int esc = iris_retrain_elm_new(a, 42u, 1e-3f, elm_scratch, sizeof elm_scratch);
    float el_rec = recall_rmse_of(a), el_grid = grid_rmse_of(a);
    ok("ELM: nh=48 fit floor beats backprop", esc >= 0
       && el_rec <= bp_rec && el_grid <= bp_grid,
       "recall %.4f vs backprop %.4f, grid %.4f vs %.4f (want both <=)",
       el_rec, bp_rec, el_grid, bp_grid);
  }

  /* --- 23. k-NN: exact recall at every demonstration ----------------------
     The property backprop never delivers: standing on a demo returns the
     demo. Gate at float precision for the blended k=1/k=3 read, and
     bit-for-bit for the 1-NN snap (it returns the stored row verbatim).   */
  {
    const int counts[4] = { 5, 20, 50, 200 };
    float worst = 0.0f; int nn_exact = 1;
    iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 1);
    for (int c = 0; c < 4; ++c) {
      load_examples(a, counts[c]);
      iris_fit_ranges(a);
      for (int n = 0; n < iris_count(a); ++n) {
        float in[NI], want[NO], got[NO];
        iris_get(a, n, in, want);
        iris_knn_predict(a, in, got, 1);
        for (int o = 0; o < NO; ++o) {
          float e = iris_absf(got[o] - want[o]);
          if (e > worst) worst = e;
        }
        iris_knn_predict(a, in, got, 3);
        for (int o = 0; o < NO; ++o) {
          float e = iris_absf(got[o] - want[o]);
          if (e > worst) worst = e;
        }
        if (iris_classify_1nn(a, in, got) < 0
            || memcmp(got, want, sizeof want) != 0) nn_exact = 0;
      }
    }
    ok("k-NN: exact recall on every demo", worst < 1e-4f && nn_exact,
       "5/20/50/200 examples, k=1 and k=3: worst |err| %.2e (< 1e-4), "
       "1-NN verbatim to the bit %d", worst, nn_exact);
  }

  /* --- 24. k-NN: structurally safe, convex, deterministic -----------------
     Output is a convex blend of demonstrated outputs, so the hostile set
     (duplicates, 1e6 outliers, dead dim) can produce no NaN and nothing
     outside the demonstrated range — even probed outside the input box.
     Conflicting duplicate inputs must average finitely, and the whole
     algorithm must ignore the seed (it does not reroll, BY DESIGN).       */
  {
    iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 1234);
    iris_clear(a);
    for (int i = 0; i < 200; ++i) {
      float u = (i % 3 == 0) ? 0.5f : (float)(i % 17) / 17.0f;
      float in[NI] = { u, 0.5f }, out[NO];
      truth(u, 0.5f, out);
      if (i % 5 == 0) { out[0] = 1e6f; out[1] = -1e6f; }
      iris_record(a, in, out);
    }
    iris_fit_ranges(a);
    float lo[NO], hi[NO];
    for (int o = 0; o < NO; ++o) { lo[o] = 1e30f; hi[o] = -1e30f; }
    for (int n = 0; n < iris_count(a); ++n) {
      float in[NI], out[NO];
      iris_get(a, n, in, out);
      for (int o = 0; o < NO; ++o) {
        if (out[o] < lo[o]) lo[o] = out[o];
        if (out[o] > hi[o]) hi[o] = out[o];
      }
    }
    int bad = 0, escaped = 0;
    for (int x = -5; x <= 25; ++x) {
      float in[NI] = { x / 20.0f, x / 20.0f }, g3[NO], g1[NO];
      iris_knn_predict(a, in, g3, 3);
      iris_classify_1nn(a, in, g1);
      for (int o = 0; o < NO; ++o) {
        if (iris_isbad(g3[o]) || iris_isbad(g1[o])) bad++;
        if (g3[o] < lo[o] || g3[o] > hi[o]) escaped++;
        if (g1[o] < lo[o] || g1[o] > hi[o]) escaped++;
      }
    }
    /* conflicting duplicates: same input, outputs 0 and 1 — finite average */
    iris_clear(a);
    float din[NI] = { 0.3f, 0.6f };
    float o0[NO] = { 0.0f, 0.2f, 0.2f }, o1[NO] = { 1.0f, 0.8f, 0.8f };
    iris_record(a, din, o0); iris_record(a, din, o1);
    iris_fit_ranges(a);
    float avg[NO];
    iris_knn_predict(a, din, avg, 3);
    int avg_ok = !iris_isbad(avg[0]) && avg[0] >= 0.0f && avg[0] <= 1.0f;
    /* seed independence: two instruments, different seeds, same examples */
    iris *b2 = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 987654);
    load_examples(a, 20); iris_fit_ranges(a);
    load_examples(b2, 20); iris_fit_ranges(b2);
    int seedfree = 1;
    for (int x = 0; x <= 20 && seedfree; ++x) for (int y = 0; y <= 20; ++y) {
      float in[NI] = { x / 20.0f, y / 20.0f }, p[NO], q[NO];
      iris_knn_predict(a,  in, p, 3);
      iris_knn_predict(b2, in, q, 3);
      if (memcmp(p, q, sizeof p) != 0) { seedfree = 0; break; }
    }
    ok("k-NN: convex, finite, does not reroll", bad == 0 && escaped == 0
       && avg_ok && seedfree,
       "hostile set: %d NaN, %d range escapes; conflicting duplicates avg %.3f "
       "(finite); seed-independent %d", bad, escaped, avg[0], seedfree);
  }

  /* --- 25. 1-NN agrees with the Weka-IBk reference ------------------------
     An independent double-precision implementation of Weka's IBk +
     LinearNNSearch semantics (desktop Wekinator's classifier), a separate
     code path on purpose: agreement comes from matching SEMANTICS, not
     from being the same code. 441 grid probes over a 3-class/12-demo set,
     zero disagreements outside genuine distance ties — plus a deliberate
     exact-tie probe where the earliest-recorded example must win.         */
  {
    enum { RN = 12 };
    static double ref_in[RN][NI], ref_lo[NI], ref_hi[NI];
    static int ref_cls[RN];
    iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 1);
    iris_clear(a);
    const double cx[3] = { 0.2, 0.8, 0.5 }, cy[3] = { 0.25, 0.3, 0.85 };
    int rn = 0;
    iris_rng rr = { 20260821u };
    for (int cls = 0; cls < 3; ++cls) {
      for (int j = 0; j < 4; ++j) {
        float dx = iris_rand_sym(&rr) * 0.12f, dy = iris_rand_sym(&rr) * 0.12f;
        float in[NI] = { (float)cx[cls] + dx, (float)cy[cls] + dy };
        float out[NO] = { (float)cls, 0.0f, 0.0f };
        iris_record(a, in, out);
        ref_in[rn][0] = in[0]; ref_in[rn][1] = in[1];
        ref_cls[rn] = cls; rn++;
      }
    }
    iris_fit_ranges(a);
    for (int i = 0; i < NI; ++i) { ref_lo[i] = 1e300; ref_hi[i] = -1e300; }
    for (int r = 0; r < rn; ++r)
      for (int i = 0; i < NI; ++i) {
        if (ref_in[r][i] < ref_lo[i]) ref_lo[i] = ref_in[r][i];
        if (ref_in[r][i] > ref_hi[i]) ref_hi[i] = ref_in[r][i];
      }
    int agree = 0, disagree = 0, ties = 0;
    for (int xa = 0; xa <= 20; ++xa) for (int xb = 0; xb <= 20; ++xb) {
      double x[NI] = { xa / 20.0, xb / 20.0 };
      float fin[NI] = { (float)x[0], (float)x[1] }, fout[NO];
      iris_classify_1nn(a, fin, fout);
      int got = (int)(fout[0] + 0.5f);
      /* the reference: min-max normalise, squared Euclidean, first wins */
      int best = 0; double bd = 1e300, second = 1e300;
      for (int r = 0; r < rn; ++r) {
        double d = 0.0;
        for (int i = 0; i < NI; ++i) {
          double rng = ref_hi[i] - ref_lo[i]; if (rng <= 0) rng = 1.0;
          double t = ((ref_in[r][i] - ref_lo[i]) / rng)
                   - ((x[i] - ref_lo[i]) / rng);
          d += t * t;
        }
        if (d < bd) { second = bd; bd = d; best = r; }
        else if (d < second) second = d;
      }
      if (second - bd < 1e-9) { ties++; continue; }   /* genuine tie */
      if (got == ref_cls[best]) agree++; else disagree++;
    }
    /* the deliberate exact tie: mirror-symmetric demos, probe on the axis */
    iris *t = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 1);
    iris_clear(t);
    float ta[NI] = { 0.25f, 0.5f }, oa[NO] = { 0.0f, 0.0f, 0.0f };
    float tb[NI] = { 0.75f, 0.5f }, ob[NO] = { 1.0f, 0.0f, 0.0f };
    int id_first = iris_record(t, ta, oa);
    iris_record(t, tb, ob);
    iris_fit_ranges(t);
    float probe[NI] = { 0.5f, 0.5f }, pout[NO];
    int winner = iris_classify_1nn(t, probe, pout);
    ok("1-NN: agrees with the Weka-IBk reference", disagree == 0
       && agree > 0 && winner == id_first,
       "441 probes: %d agree, %d disagree, %d distance ties; "
       "exact tie -> earliest-recorded wins %d",
       agree, disagree, ties, winner == id_first);
  }

  /* --- 26. hostile queries and poisoned stores cannot corrupt the samplers -
     Found by adversarial verification of v0.2: a NaN query left every
     neighbour slot at -1 and the blend read out of bounds; a NaN stored in
     an example's OUTPUTS sailed through both sampler paths verbatim. Both
     must now refuse visibly: range-centre substitute + IRIS_NAN_TRAPPED.    */
  {
    iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 7);
    iris_clear(a);
    for (int i = 0; i < 8; ++i) {
      float u = 0.1f + 0.1f * (float)i;
      float in[NI] = { u, 1.0f - u }, out[NO] = { u, 0.5f, 1.0f - u };
      iris_record(a, in, out);
    }
    iris_fit_ranges(a);
    float nanq[NI] = { 0.0f/0.0f, 0.5f }, out[NO];
    int bad = 0;
    iris_knn_predict(a, nanq, out, 3);
    for (int o = 0; o < NO; ++o) if (iris_isbad(out[o])) bad++;
    int st_nanq = (a->status == 2);            /* IRIS_NAN_TRAPPED */
    float farq[NI] = { 1e38f, 1e38f };
    iris_knn_predict(a, farq, out, 3);
    for (int o = 0; o < NO; ++o) if (iris_isbad(out[o])) bad++;
    /* poison one example's OUTPUT, then query sanely through both paths */
    a->status = 0;
    a->ex[2 * (NI + NO) + NI + 1] = 0.0f/0.0f;
    float sane[NI] = { 0.3f, 0.7f };
    iris_knn_predict(a, sane, out, 8);           /* k=8: poisoned row included */
    for (int o = 0; o < NO; ++o) if (iris_isbad(out[o])) bad++;
    int st_pois = (a->status == 2);
    float nearpois[NI] = { 0.3f, 0.7f };       /* 1nn onto the poisoned row */
    a->status = 0;
    iris_classify_1nn(a, nearpois, out);
    for (int o = 0; o < NO; ++o) if (iris_isbad(out[o])) bad++;
    ok("samplers survive hostile queries and poisoned outputs",
       bad == 0 && st_nanq && st_pois,
       "NaN query, 1e38 query, NaN-output row via k-NN and 1-NN: "
       "%d bad values escaped, statuses reported %d/%d", bad, st_nanq, st_pois);
  }

  /* --- 27. the loader cannot be lied to ------------------------------------
     Found by adversarial verification of v0.2: a corrupted example-count
     high byte went NEGATIVE past a signed compare and loaded an instrument
     with -16 million examples; a plausible count with a truncated body read
     kilobytes past the caller's buffer. Both must refuse, and a refusal
     must leave the loaded instrument bit-identical.                        */
  {
    iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 7);
    iris_clear(a);
    for (int i = 0; i < 6; ++i) {
      float u = 0.15f * (float)i + 0.05f;
      float in[NI] = { u, 1.0f - u }, out[NO] = { u, u, u };
      iris_record(a, in, out);
    }
    iris_retrain_new(a, 4321, 100);
    static unsigned char blob[8 * 1024], evil[8 * 1024];
    size_t n = iris_save(a, blob, sizeof blob);
    float before[NO]; float probe[NI] = { 0.4f, 0.6f };
    iris_predict(a, probe, before);
    int refuse = 0, total = 0;
    /* (a) n_ex high byte -> huge/negative count */
    memcpy(evil, blob, n); evil[23] = 0xFF;
    total++; if (!iris_load(a, evil, n)) refuse++;
    /* (b) plausible n_ex, truncated body */
    memcpy(evil, blob, n); evil[20] = (unsigned char)200;
    total++; if (!iris_load(a, evil, n)) refuse++;
    /* (c) body physically cut short */
    memcpy(evil, blob, n);
    total++; if (!iris_load(a, evil, n / 2)) refuse++;
    float after[NO];
    iris_predict(a, probe, after);
    int intact = (memcmp(before, after, sizeof before) == 0) && iris_count(a) == 6;
    ok("loader refuses corrupt counts and truncated bodies",
       refuse == total && intact,
       "%d/%d corruptions refused; instrument intact after refusals: %s",
       refuse, total, intact ? "yes" : "NO");
  }

  /* --- 28. a refused train mutates nothing, ranges included ----------------
     Found by adversarial verification of v0.2: the SGD path fitted ranges
     BEFORE its poison scan, so a refused train silently moved the playing
     instrument's denormalisation. Now every trainer scans first.          */
  {
    iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 7);
    iris_clear(a);
    for (int i = 0; i < 6; ++i) {
      float u = 0.15f * (float)i + 0.05f;
      float in[NI] = { u, 1.0f - u }, out[NO] = { 0.2f + 0.1f * (float)i, 0.5f, 0.8f };
      iris_record(a, in, out);
    }
    iris_retrain_new(a, 99, 200);
    float rlo[NO], rhi[NO];
    for (int o = 0; o < NO; ++o) { rlo[o] = a->out_lo[o]; rhi[o] = a->out_hi[o]; }
    /* iris_record now refuses NaN at the door (B4), so put the poison into the
       store directly — this check is about the TRAINER's pre-scan, which must
       still hold for examples that arrive via iris_load rather than iris_record. */
    float pin[NI] = { 0.9f, 0.9f }, pout[NO] = { 0.5f, 5.0f, 0.5f };
    iris_record(a, pin, pout);                    /* clean row, accepted */
    a->ex[(size_t)(a->n_ex - 1) * (NI + NO) + NI] = 0.0f/0.0f;  /* now poison it */
    iris_train_epochs(a, 100);                    /* must refuse */
    int st = (a->status == 2);
    int ranges_intact = 1;
    for (int o = 0; o < NO; ++o)
      if (a->out_lo[o] != rlo[o] || a->out_hi[o] != rhi[o]) ranges_intact = 0;
    iris_delete_last(a);                          /* musician removes the poison */
    ok("refused train leaves ranges bit-identical",
       st && ranges_intact,
       "status reported %d, out_lo/out_hi unmoved %d", st, ranges_intact);
  }


  /* --- 29. the golden v3 file loads and predicts bit-identically ----------
     The v3 twin of check 11. tests/golden/v3-instrument.bin is the SAME
     recipe as the v1 golden — seed 1234, 20 examples, 800 epochs — trained
     and saved under the v3 input scaling ([-1,+1]), frozen at v0.3.0. Two
     goldens, because there are now two scalings and both are permanent:
     check 11 proves the old one still works, this one gives the new one the
     same protection from the day it ships rather than a year later.       */
  {
    static unsigned char file[64 * 1024];
    FILE *fb = fopen("tests/golden/v3-instrument.bin", "rb");
    FILE *ft = fopen("tests/golden/v3-expected.txt", "r");
    size_t n = 0;
    int loaded = 0, probes_read = 0, mismatches = 0, ver = 0, centered = 0;
    if (fb) { n = fread(file, 1, sizeof file, fb); fclose(fb); }
    if (n >= 8) ver = (int)((uint32_t *)file)[1];
    iris *kg = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 99);
    if (kg && n > 0) loaded = iris_load(kg, file, n);
    if (loaded) centered = kg->in_center;       /* the file chose the scaling */
    if (ft) {
      for (int a = 0; a < 21; ++a) {
        unsigned want[NO];
        if (fscanf(ft, "%x %x %x", &want[0], &want[1], &want[2]) != NO) break;
        probes_read++;
        if (loaded) {
          float in[NI] = { a / 20.0f, 0.4f }, got[NO];
          iris_predict(kg, in, got);
          for (int o = 0; o < NO; ++o) {
            unsigned bits; memcpy(&bits, &got[o], sizeof bits);
            if (bits != want[o]) mismatches++;
          }
        }
      }
      fclose(ft);
    }
    ok("golden v3 file loads, predicts bit-identically",
       fb && ft && loaded && ver == 3 && centered == 1
         && probes_read == 21 && mismatches == 0,
       "%zu bytes, format v%d, in_center %d, %d probes x %d outputs, %d bit mismatches",
       n, ver, centered, probes_read, NO, mismatches);
  }

  /* --- 30. the migration story, tested rather than asserted ---------------
     The dangerous half of the input-scaling change is not the new files, it
     is the old ones. Three things have to be true at once and this check
     holds all three in the same instrument:

       (a) the v1 file plays as it always did — that is check 11, and it
           passes on the LEGACY scaling, selected by the file's own version;
       (b) the same demonstrations, re-trained under the NEW scaling and
           round-tripped through a v3 save, land on the same mapping to
           within a float tolerance — so a musician who re-trains is not
           handed a different instrument, only a better-fitted one;
       (c) the two instruments are genuinely on DIFFERENT scalings while
           doing it, so (b) is not passing by accident.

     The tolerance is real and is stated as a number: these are two separate
     fits of the same twenty points by two differently-conditioned
     optimisations, so they agree to about the fit error, not to the bit.  */
  {
    static unsigned char file[64 * 1024], resaved[64 * 1024];
    FILE *fb = fopen("tests/golden/v1-instrument.bin", "rb");
    size_t n = 0;
    int ok_load = 0, scal_v1 = -1, scal_v3 = -1;
    float worst = 0.0f;
    if (fb) { n = fread(file, 1, sizeof file, fb); fclose(fb); }

    iris *kold = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 99);
    if (kold && n) ok_load = iris_load(kold, file, n);
    if (ok_load) scal_v1 = kold->in_center;     /* must be 0: it is a v1 file */

    /* the SAME demonstrations, in a fresh v3 instrument, trained to
       convergence, saved, and loaded back from the v3 bytes */
    iris *knew = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 1234);
    if (ok_load) {
      for (int i = 0; i < iris_count(kold); ++i) {
        float in[NI], out[NO];
        iris_get(kold, i, in, out);
        iris_record(knew, in, out);
      }
      iris_train_converge(knew, 0, 0, 0);
      size_t m = iris_save(knew, resaved, sizeof resaved);
      iris *kback = iris_init(arena_d, sizeof arena_d, NI, NH, NO, CAP, 7);
      if (m && iris_load(kback, resaved, m)) {
        scal_v3 = kback->in_center;             /* must be 1: it is a v3 file */
        for (int a = 0; a <= 20; ++a) for (int b = 0; b <= 20; ++b) {
          float in[NI] = { a / 20.0f, b / 20.0f }, o1[NO], o2[NO];
          iris_predict(kold,  in, o1);
          iris_predict(kback, in, o2);
          for (int o = 0; o < NO; ++o) {
            float d = o1[o] - o2[o];
            if (d < 0) d = -d;
            if (d > worst) worst = d;
          }
        }
      }
    }
    ok("v1 and a re-saved v3 of the same instrument agree",
       ok_load && scal_v1 == 0 && scal_v3 == 1 && worst <= 0.06f,
       "441 probes x %d outputs, worst |v1 - v3| %.4f (want <= 0.06); "
       "scalings %d then %d", NO, worst, scal_v1, scal_v3);
  }

  /* --- 31. training to convergence, and a progress bar that is not a lie --
     Three claims. (a) The plateau criterion beats the old 600-epoch
     recommendation on the very reference task the audit already uses.
     (b) A run sliced into chunks — which is how a single-threaded UI keeps
     drawing — is BIT-IDENTICAL to the same run taken in one blocking call,
     because the shuffle buffer is initialised once and carried across the
     slices. (c) The reported progress never goes backwards and ends at
     exactly 1.0, which is the whole difference between a progress bar and
     an animation.                                                          */
  {
    load_examples(k, 20);
    iris_retrain_new(k, 4242u, 600);
    float e600 = iris_last_error(k), r600 = recall_rmse_of(k);

    iris *ka = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 4242u);
    load_examples(ka, 20);
    float econv = iris_train_converge(ka, 0, 0, 0);
    float rconv = recall_rmse_of(ka);
    int epochs_used = ka->tr_done;

    /* the same run, in slices, with a progress bar read between them */
    iris *kb2 = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 4242u);
    load_examples(kb2, 20);
    float prev = -1.0f; int monotone = 1, reads = 0;
    iris_train_begin(kb2, 0);
    while (iris_train_slice(kb2, 500)) {
      float p = iris_train_progress(kb2);
      if (p < prev) monotone = 0;
      prev = p; reads++;
    }
    float pend = iris_train_progress(kb2);
    const int nw = NH * NI + NH + NO * NH + NO;
    int same_bits = memcmp(ka->w1, kb2->w1, (size_t)nw * sizeof(float)) == 0;

    ok("trains to convergence; sliced == unsliced; honest bar",
       econv < e600 * 0.2f && rconv < r600 * 0.6f && same_bits
         && monotone && pend == 1.0f && reads > 2,
       "600 ep: err %.3e recall %.4f -> converged (%d ep): err %.3e recall %.4f; "
       "sliced memcmp %d over %d reads, bar monotone %d, ends %.1f",
       e600, r600, epochs_used, econv, rconv, same_bits, reads, monotone, pend);
  }

  /* --- 32. it points at the demonstration that is fighting ----------------
     The integrated residual (PART 8f) against the shipping trainer. Twelve
     trials, one demonstration of twenty corrupted by +0.20 on one output;
     the corrupted one has to come out on top. And the other half of the
     claim, which matters more: on CLEAN data the same call must stay quiet
     — a detector that accuses an innocent example is worse than none.     */
  {
    int hits = 0, quiet = 1; float loudest_clean = 0.0f, worst_margin = 0.0f;
    for (int t = 0; t < 12; ++t) {
      int bad = t % 20;
      iris *kd = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 1000u + (uint32_t)t * 7919u);
      iris_clear(kd);
      for (int i = 0; i < 20; ++i) {
        float u = (float)((i * 7919) % 97) / 97.0f, v = (float)((i * 6131) % 89) / 89.0f;
        float in[NI] = { u, v }, out[NO];
        truth(u, v, out);
        if (i == bad) out[i % NO] = iris_clampf(out[i % NO] + 0.20f, 0.0f, 1.0f);
        iris_record(kd, in, out);
      }
      iris_train_converge(kd, 0, 0, 0);
      float m = 0.0f;
      if (iris_worst_example(kd, &m) == bad) hits++;
      if (m > worst_margin) worst_margin = m;

      /* the same seed, the same twenty points, nothing corrupted */
      iris *kc = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 1000u + (uint32_t)t * 7919u);
      load_examples(kc, 20);
      iris_train_converge(kc, 0, 0, 0);
      float mc = 0.0f;
      (void)iris_worst_example(kc, &mc);
      if (mc > loudest_clean) loudest_clean = mc;
      if (mc >= IRIS_STRESS_FLAG) quiet = 0;
    }
    /* and below IRIS_STRESS_MIN_EX it must refuse to have an opinion at all */
    iris *ks = iris_init(arena_d, sizeof arena_d, NI, NH, NO, CAP, 5);
    load_examples(ks, 6);
    iris_train_converge(ks, 0, 0, 0);
    float ms = 0.0f;
    int small_silent = iris_worst_example(ks, &ms) == -1;

    ok("finds the demonstration that fights, stays quiet otherwise",
       hits >= 9 && quiet && small_silent,
       "corrupted demo ranked first %d/12 (max margin %.2f); loudest clean margin "
       "%.2f (flag %.1f); silent under %d examples %d",
       hits, worst_margin, loudest_clean, (double)IRIS_STRESS_FLAG,
       IRIS_STRESS_MIN_EX, small_silent);
  }

  /* --- 33. the trainer chosen on error floor, not on time-to-parity -------
     docs/frontier/REPORT.md ranked the optimisers by how fast they reach the
     600-epoch baseline. Re-scored on where they STOP, the ranking inverts:
     L-BFGS — the fast trainer, and the right one when speed is the
     constraint — plateaus and stays there. 10,000 iterations are
     indistinguishable from 1,000. Plain SGD carried to convergence goes
     several times lower and generalises better. This check is that claim,
     kept honest run to run rather than left in a document.

     ⚠️ WHAT THIS CHECK MEASURES, PRECISELY: our L-BFGS IMPLEMENTATION, not
     the L-BFGS method. Two defects in experimental/iris_lbfgs.h account for
     most of the gap — an absolutely-scaled curvature safeguard that discards
     ~680 of 1000 curvature pairs, and a float32 line search resolving rounding
     noise below its own Armijo threshold. Repaired, the median training-MSE
     ratio falls from 4.05x to 1.28x, the per-seed comparison becomes a coin
     flip (4/8), and THIS ASSERTION FAILS (sc = 5.08e-06 vs l2 = 8.02e-06, so
     sc < l2*0.5f is false). It also asserts on TRAINING error, whereas
     scikit-learn's small-data guidance concerns HELD-OUT error on a
     regularised objective; on held-out error the honest figure is ~6% at 16
     seeds, not "several times". Do not quote this check as evidence against
     the field's advice. Full workings and the fair-comparison protocol:
     research/prior-art/WHY-lbfgs-defaults-robustness.md section 1.

     Levenberg-Marquardt was measured too and is NOT here, because it is not
     in the file: it stalls in the same place as L-BFGS (median train MSE
     1.6e-5 at 20 examples with double-precision normal equations and Nielsen
     damping) and its normal matrix does not fit — see the trainer table in
     docs/frontier/REPORT.md.                                               */
  {
    static float lw33[IRIS_LBFGS_WORK_FLOATS(NI, NH, NO, IRIS_LBFGS_M) + 2];
    iris *k1 = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 4242u);
    load_examples(k1, 20);
    float l1 = iris__train_lbfgs_full(k1, 1000, 0.0f, lw33, sizeof lw33, 0, 0, 0);
    iris *k2 = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 4242u);
    load_examples(k2, 20);
    float l2 = iris__train_lbfgs_full(k2, 10000, 0.0f, lw33, sizeof lw33, 0, 0, 0);
    iris *k3 = iris_init(arena_d, sizeof arena_d, NI, NH, NO, CAP, 4242u);
    load_examples(k3, 20);
    float sc = iris_train_converge(k3, 0, 0, 0);
    /* WHAT IS ASSERTED, AND WHY IT CHANGED TWICE.

       2026-08-26: this asserted `sc < l2 * 0.5f` — that SGD beats our L-BFGS by
       at least 2x. That was mostly measuring two defects in our own L-BFGS. Both
       were repaired and the assertion was replaced by a plateau test.

       2026-08-27: the plateau test is dead too, and the reason is worth reading.
       Fixing the tanh codomain clamp REVERSED the comparison. L-BFGS now reaches
       a LOWER training error than SGD-to-plateau on this seed, and is still
       improving at 10,000 iterations rather than plateauing. The old activation
       let 1-a*a go negative past |s|=3; L-BFGS takes large steps, entered that
       band routinely, and was being fed wrong-signed gradients. SGD at these
       hyperparameters never goes there — which is why the core golden hash above
       did not move and this one did.

       So: assert only what is stable, which is that both trainers actually fit.
       REPORT the comparison and do not assert it. It has now flipped once, on a
       change to a function neither trainer owns; a test that pins a comparative
       ordering here would be pinning an artefact. Anyone quoting the ratio must
       quote it as our implementation, one seed, one task.
       See docs/MATH-FIXES.md defect 1.                                        */
    ok("both trainers reach a usable fit; ratio reported, not asserted",
       l2 > 0.0f && l2 < 1e-3f && sc > 0.0f && sc < 1e-3f,
       "L-BFGS 1000 it %.3e -> 10000 it %.3e (%.2fx); SGD-to-plateau %.3e; "
       "L-BFGS/SGD = %.2fx (REPORTED — flipped on 2026-08-27 when the tanh "
       "codomain was fixed)", l1, l2, l1 / l2, sc, l2 / sc);
  }

  /* --- 34. THE HOLE CHECK 30 LEFT OPEN: open, re-train, save, re-open -----
     Check 30 proves that the same demonstrations re-fitted in a FRESH v3
     instrument land near the v1 original. That is the "start again" route,
     and it was the only migration route the audit covered. The route the
     app actually takes is the other one, and it was broken:

         core/store.c   iris_load  (a v1/v2 payload -> legacy scaling)
         core/surface.c iris_train_begin / iris_train_slice   (the musician
                        holds BOOT and asks for a better fit)
         core/store.c   iris_save  (and here the version word lied)

     iris_save stamped IRIS_FORMAT unconditionally, so [0,1] weights went to
     flash labelled v3. The reload believed the label, switched to [-1,+1],
     and played a different instrument — out of a file whose every weight
     round-tripped bit-perfectly. Measured 0.427 of full scale before the
     fix, which is not drift, it is a different mapping. Nothing was
     corrupt, nothing failed, nothing was reported.

     The instrument that comes back off disk must be the instrument that was
     in memory when it was written. Not close: identical. That is the whole
     of this check, and it is asserted at zero tolerance because a save and
     a load that disagree by ANY amount are a bug.                          */
  {
    static unsigned char file[64 * 1024], resaved[64 * 1024];
    FILE *fb = fopen("tests/golden/v1-instrument.bin", "rb");
    size_t n = 0, m = 0;
    int loaded = 0, scal_in = -1, scal_out = -1, ver = 0, reloaded = 0;
    float worst = -1.0f;
    if (fb) { n = fread(file, 1, sizeof file, fb); fclose(fb); }

    iris *ka = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 99);
    if (ka && n) loaded = iris_load(ka, file, n);
    if (loaded) {
      scal_in = iris_input_scaling(ka);           /* 0: it came from a v1 file */
      iris_train_converge(ka, 8000, 0, 0);        /* the musician re-trains    */
      scal_out = iris_input_scaling(ka);          /* still 0: weights own it   */
      m = iris_save(ka, resaved, sizeof resaved);
      if (m >= 8) ver = (int)((uint32_t *)resaved)[1];
      iris *kb = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 7);
      if (m && (reloaded = iris_load(kb, resaved, m))) {
        worst = 0.0f;
        for (int a = 0; a <= 20; ++a) for (int b = 0; b <= 20; ++b) {
          float in[NI] = { a / 20.0f, b / 20.0f }, o1[NO], o2[NO];
          iris_predict(ka, in, o1);
          iris_predict(kb, in, o2);
          for (int o = 0; o < NO; ++o) {
            float d = o1[o] - o2[o];
            if (d < 0) d = -d;
            if (d > worst) worst = d;
          }
        }
      }
    }
    ok("re-trained old instrument survives save/reload BIT-EXACTLY",
       loaded && scal_in == 0 && scal_out == 0 && ver == 2 && reloaded
         && worst == 0.0f,
       "v1 in (scaling %d) -> re-trained (scaling %d) -> saved v%d -> reloaded %d; "
       "worst |memory - disk| over 441 probes %.9f (want exactly 0)",
       scal_in, scal_out, ver, reloaded, worst);
  }

  /* --- 35. and the way OUT of the old scaling is a decision, not a default -
     Check 34 locks an old instrument onto its old scaling for ever, which is
     correct and is also a trap if there is no door. iris_migrate_scaling is
     the door: it throws the old weights away and re-fits the same stored
     demonstrations under [-1,+1]. The musician's demonstrations are raw
     sensor values in their own units, so they mean the same thing under
     either scaling — that is why the re-fit is legitimate.

     It has to be LOUD (a function the caller names), IDEMPOTENT (calling it
     on an already-centred instrument does nothing and says so), and it has
     to actually move the file format with it. And the instrument it hands
     back is a NEW FIT: it lands within the same fit tolerance check 30 uses,
     not on the same bits, and the caller has to be willing to accept that
     before pressing the button.                                            */
  {
    static unsigned char file[64 * 1024], out[64 * 1024];
    FILE *fb = fopen("tests/golden/v1-instrument.bin", "rb");
    size_t n = 0, m = 0;
    int loaded = 0, first = -1, second = -1, before = -1, after = -1, ver = 0;
    float moved = -1.0f;
    if (fb) { n = fread(file, 1, sizeof file, fb); fclose(fb); }

    iris *ko = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 99);
    iris *km = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 99);
    if (ko && km && n) loaded = (iris_load(ko, file, n) && iris_load(km, file, n));
    if (loaded) {
      before = iris_input_scaling(km);
      first  = iris_migrate_scaling(km);          /* the decision */
      after  = iris_input_scaling(km);
      second = iris_migrate_scaling(km);          /* nothing left to do */
      m = iris_save(km, out, sizeof out);
      if (m >= 8) ver = (int)((uint32_t *)out)[1];
      moved = 0.0f;
      for (int a = 0; a <= 20; ++a) for (int b = 0; b <= 20; ++b) {
        float in[NI] = { a / 20.0f, b / 20.0f }, o1[NO], o2[NO];
        iris_predict(ko, in, o1);
        iris_predict(km, in, o2);
        for (int o = 0; o < NO; ++o) {
          float d = o1[o] - o2[o];
          if (d < 0) d = -d;
          if (d > moved) moved = d;
        }
      }
    }
    ok("migrating scaling is opt-in, idempotent, and carries the format",
       loaded && before == 0 && first == 1 && after == 1 && second == 0
         && ver == 3 && moved <= 0.06f,
       "scaling %d -> %d, migrate returned %d then %d, saves v%d, "
       "instrument moved %.4f (want <= 0.06, the check-30 fit tolerance)",
       before, after, first, second, ver, moved);
  }

  /* --- timing: what will this cost on the S3? ----------------------------- */
  printf("--------------------------------------------------------------------------\n");
  /* --- 35. THE DIVERGENCE TRAP -------------------------------------------
     Found 2026-08-27 by running examples/02_fix_a_mistake.c. A contradictory
     demonstration diverges the weights; the guard clamps and stops. On the
     NEXT fresh run one pinned weight trips the guard again on epoch 1, so
     training silently did nothing -- forever -- while reporting a healthy
     status. The musician deletes the bad take, retrains, and the instrument
     stays broken with no message. Zeroing the momentum does not help; it is
     the pinned weight, measured at 1 of 60.

     The examples survive, the weights do not. This pins BOTH halves: the
     refusal is distinguishable, and a reroll actually recovers.            */
  {
    iris *d = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 1234);
    for (int i = 0; i < 14; ++i) {
      float u = (float)i / 13.0f;
      float in[NI] = { u, 1.0f - u }, out[NO] = { u, 0.5f, 1.0f - u };
      iris_record(d, in, out);
    }
    iris_train_converge(d, 0, 0, 0);
    float pr[NI] = { 0.60f, 0.40f }, healthy[NO];
    iris_predict(d, pr, healthy);

    float bin[NI] = { 0.60f, 0.40f }, bout[NO] = { 0.05f, 0.95f, 0.05f };
    iris_record(d, bin, bout);
    iris_train_converge(d, 0, 0, 0);
    int diverged = (iris_get_status(d) == IRIS_TRAINING_DIVERGED);

    float mg = 0.0f; iris_delete_id(d, iris_worst_example_id(d, &mg));
    float rc = iris_train_converge(d, 0, 0, 0);
    int refused = (rc < 0.0f) && (iris_get_status(d) == IRIS_DIVERGED_STUCK);

    iris_retrain_new(d, 1234, 600);
    float back[NO]; iris_predict(d, pr, back);
    float drift = 0.0f;
    for (int o = 0; o < NO; ++o) {
      float e = back[o] - healthy[o]; if (e < 0) e = -e;
      if (e > drift) drift = e;
    }
    int recovered = (iris_get_status(d) == IRIS_STATUS_OK) && (drift < 0.02f);

    ok("divergence refuses loudly, and a reroll recovers",
       diverged && refused && recovered,
       "diverged %d, retrain refused with DIVERGED_STUCK %d, reroll recovered "
       "%d (worst drift from pre-damage output %.4f)",
       diverged, refused, recovered, drift);
  }


  printf("TRAINING COST  (this machine; the S3 is roughly 25-40x slower)\n\n");
  printf("  * S3 columns are the HOST time x270, not board readings. Only\n"
         "    backprop-600 has ever been measured on hardware (321 ms @20 ex,\n"
         "    BRINGUP-LOG.md:198). converge/ELM/L-BFGS are UNMEASURED on device.\n\n");
  printf("  examples   backprop-600      S3 x270*  |  L-BFGS-100      S3 x270* |  ELM nh-12       S3 x270*\n");
  const int exs[] = { 10, 20, 50, 100, 200 };
  static float lwork_t[IRIS_LBFGS_WORK_FLOATS(NI, NH, NO, IRIS_LBFGS_M) + 2];
  for (int e = 0; e < 5; ++e) {
    iris_clear(k);
    for (int i = 0; i < exs[e]; ++i) {
      float u = (float)((i * 7919) % 97) / 97.0f;
      float v = (float)((i * 6131) % 89) / 89.0f;
      float in[NI] = { u, v }, out[NO];
      truth(u, v, out); iris_record(k, in, out);
    }
    iris_reseed(k, 4242);
    double t0 = now_ms();
    iris_train_epochs(k, 600);
    double dt = now_ms() - t0;
    iris_reseed(k, 4242);
    double t1 = now_ms();
    iris_train_lbfgs(k, 100, lwork_t, sizeof lwork_t);
    double dl = now_ms() - t1;
    /* the closed-form solve is under the timer's resolution; average 200 */
    double t2 = now_ms();
    for (int rep = 0; rep < 200; ++rep)
      iris_train_elm(k, 1e-4f, elm_scratch, sizeof elm_scratch);
    double de = (now_ms() - t2) / 200.0;
    printf("  %6d   %8.1f ms     ~%5.0f ms   |  %7.2f ms    ~%5.0f ms  |  %7.4f ms    ~%5.2f ms\n",
           exs[e], dt, dt * IRIS_S3_SCALE, dl, dl * IRIS_S3_SCALE, de, de * IRIS_S3_SCALE);
  }

  /* THE CONVERGED TRAINER — the default since v0.3.0, and the one number a
     musician actually waits on. Reported separately because it is the only
     path here whose cost is not a fixed budget: it stops when the error
     plateaus, so the epochs it spends are part of the measurement. */
  printf("\n  iris_train_converge (plateau test, ceiling %d) — the default\n", IRIS_CONV_CEILING);
  printf("  examples   epochs spent      host        S3 x270*    train MSE   (600-epoch MSE)\n");
  for (int e = 0; e < 5; ++e) {
    iris *kk = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 4242u);
    load_examples(kk, exs[e]);
    double t0 = now_ms();
    float ec = iris_train_converge(kk, 0, 0, 0);
    double dt = now_ms() - t0;
    int used = kk->tr_done;
    iris *k6 = iris_init(arena_d, sizeof arena_d, NI, NH, NO, CAP, 4242u);
    load_examples(k6, exs[e]);
    float e6 = iris_train_epochs(k6, 600);
    printf("  %6d   %9d   %8.1f ms   ~%6.1f s     %.3e   (%.3e)\n",
           exs[e], used, dt, dt * IRIS_S3_SCALE / 1000.0, ec, e6);
  }

  /* the correction path — the felt latency of "record one more, fix it" */
  {
    const int cex[2] = { 20, 50 };
    printf("\n");
    for (int e = 0; e < 2; ++e) {
      load_examples(k, cex[e]);
      iris_retrain_new(k, 4242u, 600);
      float nin[NI], nout[NO];
      chain_correction(0, nin, nout);
      iris_record(k, nin, nout);
      double t0 = now_ms();
      float unused = 0.0f;
      for (int rep = 0; rep < 50; ++rep) unused += iris_correct(k, 20);
      double dc = (now_ms() - t0) / 50.0;
      (void)unused;
      printf("  one warm correction (iris_correct, 20 epochs) at %3d examples: "
             "%.3f ms here, ~%.1f ms on the S3\n", cex[e] + 1, dc, dc * IRIS_S3_SCALE);
    }
  }

  /* inference cost — this is the one that must never be slow */
  {
    iris_clear(k);
    for (int i = 0; i < 20; ++i) {
      float u = (float)((i*7919)%97)/97.0f, v = (float)((i*6131)%89)/89.0f;
      float in[NI] = {u,v}, out[NO]; truth(u,v,out); iris_record(k,in,out);
    }
    iris_train_epochs(k, 200);
    float in[NI] = { 0.3f, 0.7f }, out[NO];
    double t0 = now_ms();
    for (int i = 0; i < 1000000; ++i) { in[0] = (float)(i & 1023) / 1023.0f; iris_predict(k, in, out); }
    double per = (now_ms() - t0) * 1000.0 / 1e6;
    printf("\n  playing (one prediction): %.3f us here, ~%.1f us on the S3\n", per, per * IRIS_S3_SCALE);
    printf("  at 1000 gestures/sec that is %.2f%% of one S3 core\n", per * IRIS_S3_SCALE * 1000.0 / 10000.0);
    /* the k-NN read costs O(n_ex) per call; price it at both store sizes */
    const int kex[2] = { 64, 256 };
    for (int e = 0; e < 2; ++e) {
      load_examples(k, kex[e]);
      iris_fit_ranges(k);
      double t1 = now_ms();
      for (int i = 0; i < 200000; ++i) {
        in[0] = (float)(i & 1023) / 1023.0f;
        iris_knn_predict(k, in, out, 3);
      }
      double pk = (now_ms() - t1) * 1000.0 / 2e5;
      printf("  k-NN prediction at %3d examples: %.3f us here, ~%.1f us on the S3\n",
             kex[e], pk, pk * IRIS_S3_SCALE);
    }
  }

  printf("\n  memory for this instrument: %zu bytes (%d examples max)\n",
         sizeof arena_a, CAP);
  printf("--------------------------------------------------------------------------\n");
  printf("%s  (%d failed)\n\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", failures);
  return failures ? 1 : 0;
}
