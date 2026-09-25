/* SPDX-License-Identifier: BSD-3-Clause
   Copyright (c) 2026 Kyle Smith */
/* ============================================================================
   train.c — the trainers' state logic, checked byte by byte.

   Every check here compares whole arenas with memcmp, so "changed nothing"
   and "bit-identical" mean every byte the instrument owns, not a few fields
   someone thought to look at.

     1. A refusal changes nothing. Every trainer and diagnostic is handed a
        store with a not-a-number or an infinity written straight into it, and
        an empty store, and must refuse without touching one byte.
     2. iris_train_begin + iris_train_slice is iris_train, bit for bit, over
        several shapes, seeds and slice sizes, starting from an instrument
        that has already been trained, warm-trained, edited and deleted from.
     3. iris_loo_error refuses with exactly -1, never a not-a-number.
     4. Once a divergence leaves a weight on the limit, every trainer that
        continues from the current weights refuses on every call -- no
        alternation, whatever touches the status in between -- and iris_train
        is the way out, to the instrument a cold start gives.

   Not-a-number and infinity are built with __builtin_nanf and __builtin_inff,
   never by dividing by zero, so -fsanitize=float-divide-by-zero can run over
   this file.

   Build and run, from the repository root:
     cc -std=c99 -O2 -Wall -Wextra -I. -o build/train tests/train.c && ./build/train
   The exit status is non-zero if any check fails.
   ========================================================================= */
#include "iris.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
static void check(const char *name, int ok, const char *detail) {
  printf("  [%s] %-52s %s\n", ok ? "PASS" : "FAIL", name, detail);
  if (!ok) fails++;
}

/* The largest shape used below, so one static arena serves every check. */
static unsigned char A[IRIS_ARENA(5, 20, 5, 48)];
static unsigned char SNAP[sizeof A], RA[sizeof A], RB[sizeof A];

/* A repeatable demonstration generator: inputs from a small linear
   congruential sequence, outputs a smooth function of the inputs built from
   iris_tanh, so every value is finite and every instrument is learnable. */
static unsigned long lcg = 1;
static float lcg01(void) {
  lcg = lcg * 1103515245ul + 12345ul;
  return (float)((lcg >> 8) & 0xFFFFu) / 65535.0f;
}
static void record_n(iris *k, int n, unsigned long seed) {
  float in[5], out[5];
  lcg = seed;
  for (int r = 0; r < n; ++r) {
    float s = 0.0f;
    for (int i = 0; i < k->n_in; ++i) { in[i] = lcg01() * 10.0f - 3.0f; s += in[i] * (float)(i + 1); }
    for (int o = 0; o < k->n_out; ++o)
      out[o] = 100.0f * (float)(o + 1) + 40.0f * iris_tanh(0.15f * s - 0.4f * (float)o);
    iris_record(k, in, out);
  }
}

/* An instrument with a history: trained, then warm-trained (so the momentum
   velocities and the random state are not where a reseed would put them),
   then edited -- two deletes and a fresh take -- so the fit is stale but
   still playing. */
static iris *lived_in(int ni, int nh, int no, int cap, int n, uint32_t seed) {
  iris *k = iris_init(A, sizeof A, ni, nh, no, cap, seed);
  if (!k) return 0;
  record_n(k, n, 1000ul + seed);
  iris_train(k);
  iris_train_epochs(k, 13);
  iris_delete_id(k, 3);
  iris_delete_index(k, 0);
  record_n(k, 1, 77ul + seed);
  return k;
}

static int same_arena(void) { return memcmp(A, SNAP, sizeof A) == 0; }
static int is_nan(float x) { return x != x; }

static int cb_calls = 0;
static int count_cb(void *user, int done, int ceiling, float err) {
  (void)user; (void)done; (void)ceiling; (void)err;
  cb_calls++;
  return 1;
}

/* Every trainer and diagnostic in PART 8 on one instrument whose store the
   caller has just made untrainable. Returns the number of calls that either
   answered wrongly or changed a byte. */
static int refusals_change_nothing(iris *k, char *why, size_t whylen) {
  static unsigned char scratch[sizeof A];
  int bad = 0;
  memcpy(SNAP, A, sizeof A);
  memset(scratch, 0xA5, sizeof scratch);
  const float prog = iris_train_progress(k);
  const int busy = iris_train_busy(k), done = iris_train_epochs_done(k);
  why[0] = 0;
#define REFUSES(expr, label) do { if (!(expr) || !same_arena()) {           \
      bad++; snprintf(why + strlen(why), whylen - strlen(why), " %s", label); \
      memcpy(A, SNAP, sizeof A); } } while (0)
  REFUSES(iris_train(k) == 0, "train");
  REFUSES(iris_train_epochs(k, 50) == -1.0f, "epochs");
  cb_calls = 0;
  REFUSES(iris_train_converge(k, 4000, count_cb, 0) == -1.0f && cb_calls == 0, "converge");
  REFUSES(iris_train_converge(k, 0, 0, 0) == -1.0f, "converge-default");
  REFUSES(iris_train_begin(k, 0) == 0, "begin");
  REFUSES(iris_train_slice(k, 50) == 0, "slice");
  REFUSES(iris_internal_train_run(k, 50, 0, 0, 0, 0) == -1.0f, "engine");
  REFUSES(iris_train_progress(k) == prog && iris_train_busy(k) == busy
          && iris_train_epochs_done(k) == done, "progress/busy/done");
  { float e = iris_loo_error(k, 30);
    REFUSES(e == -1.0f && !is_nan(e), "loo_error"); }
  { int untouched = 1;
    float s = iris_suggest_smoothing(k, scratch, sizeof scratch);
    for (size_t i = 0; i < sizeof scratch; ++i) if (scratch[i] != 0xA5) { untouched = 0; break; }
    REFUSES(s == -1.0f && untouched, "suggest_smoothing"); }
#undef REFUSES
  return bad;
}

int main(void) {
  char d[256], why[160];

  /* ---- 1. a refusal changes nothing ------------------------------------ */
  {
    const int shapes[3][5] = { { 2, 12, 3, 32, 14 }, { 5, 16, 2, 40, 20 }, { 1, 8, 1, 16, 9 } };
    int bad = 0, cases = 0;
    char where[160] = "";
    for (int s = 0; s < 3; ++s) {
      const int *sh = shapes[s];
      for (int poison = 0; poison < 3; ++poison) {
        iris *k = lived_in(sh[0], sh[1], sh[2], sh[3], sh[4], 40u + (uint32_t)s);
        if (!k) { bad++; continue; }
        const int stride = k->n_in + k->n_out;
        /* A not-a-number in an output, an infinity in an input, and a negative
           infinity in the take recorded after training. */
        if (poison == 0) k->ex[(size_t)2 * stride + k->n_in] = __builtin_nanf("");
        if (poison == 1) k->ex[(size_t)1 * stride] = __builtin_inff();
        if (poison == 2) k->ex[(size_t)(k->n_ex - 1) * stride] = -__builtin_inff();
        int b = refusals_change_nothing(k, why, sizeof why);
        if (b && !where[0]) snprintf(where, sizeof where, "shape %d poison %d:%s", s, poison, why);
        bad += b; cases++;
      }
    }
    snprintf(d, sizeof d, "%d shape/poison cases x 10 calls: %d wrong%s%s",
             cases, bad, where[0] ? " -- " : "", where);
    check("a poisoned store is refused with every byte unchanged", bad == 0, d);
  }
  {
    /* An empty store, and two demonstrations (too few for leave-one-out). */
    iris *k = lived_in(2, 12, 3, 32, 14, 9u);
    iris_clear(k);
    int bad_empty = refusals_change_nothing(k, why, sizeof why);
    char w1[160]; snprintf(w1, sizeof w1, "%s", why);
    iris *k2 = iris_init(A, sizeof A, 2, 12, 3, 32, 5u);
    record_n(k2, 2, 3ul);
    iris_train(k2);
    memcpy(SNAP, A, sizeof A);
    float e = iris_loo_error(k2, 30);
    static unsigned char sc[sizeof A];
    float s = iris_suggest_smoothing(k2, sc, sizeof sc);
    int few_ok = e == -1.0f && s == -1.0f && same_arena();
    snprintf(d, sizeof d, "empty store: %d wrong%s; two demonstrations: loo %.1f suggest %.1f unchanged %d",
             bad_empty, w1, (double)e, (double)s, same_arena());
    check("an empty or too-small store is refused unchanged", bad_empty == 0 && few_ok, d);
  }
  {
    /* A run in flight whose store goes bad ends -- the one write a refused
       slice makes -- and nothing else moves. */
    iris *k = lived_in(2, 12, 3, 32, 14, 21u);
    iris_train_begin(k, 0);
    iris_train_slice(k, 300);
    k->ex[4] = __builtin_nanf("");
    memcpy(SNAP, A, sizeof A);
    { int32_t zero = 0;
      memcpy(SNAP + ((unsigned char *)&k->tr_running - A), &zero, sizeof zero); }
    int more = iris_train_slice(k, 300);
    snprintf(d, sizeof d, "slice returned %d, busy %d, every other byte unchanged %d",
             more, iris_train_busy(k), same_arena());
    check("a slice that finds a poisoned store ends the run only", more == 0 && !iris_train_busy(k) && same_arena(), d);
  }

  /* ---- 2. begin + slices == iris_train, every byte ---------------------- */
  {
    const int shapes[4][5] = { { 2, 12, 3, 32, 14 }, { 5, 20, 5, 48, 30 },
                               { 1, 8, 1, 16, 6 },   { 3, 16, 2, 40, 22 } };
    const uint32_t seeds[3] = { 1234u, 7u, 40507u };
    const int sizes[6] = { 1, 7, 500, 1999, 2000, 60000 };
    int cases = 0, differ = 0, unfinished = 0;
    char first[160] = "";
    for (int s = 0; s < 4; ++s)
      for (int e = 0; e < 3; ++e) {
        const int *sh = shapes[s];
        /* one-epoch slices on one shape and seed only: 60,000 calls apiece */
        for (int z = (s == 0 && e == 0) ? 0 : 1; z < 7; ++z) {
          iris *k = lived_in(sh[0], sh[1], sh[2], sh[3], sh[4], seeds[e]);
          memcpy(SNAP, A, sizeof A);
          int ra = iris_train(k);
          memcpy(RA, A, sizeof A);
          memcpy(A, SNAP, sizeof A);
          int rb = iris_train_begin(k, 0), step = 0;
          /* z == 6 is a mixed pattern of slice sizes */
          const int mix[5] = { 3, 500, 37, 2000, 1 };
          while (iris_train_slice(k, z < 6 ? sizes[z] : mix[step++ % 5])) {}
          memcpy(RB, A, sizeof A);
          cases++;
          if (!ra || !rb || memcmp(RA, RB, sizeof A) != 0) {
            differ++;
            if (!first[0]) snprintf(first, sizeof first, " (first: shape %d seed %u slice %d)",
                                    s, (unsigned)seeds[e], z < 6 ? sizes[z] : -1);
          }
          if (iris_train_busy(k) || iris_train_progress(k) != 1.0f) unfinished++;
        }
      }
    snprintf(d, sizeof d, "%d runs, %d differ from iris_train%s, %d left unfinished",
             cases, differ, first, unfinished);
    check("iris_train_begin + slices is iris_train, byte for byte", differ == 0 && unfinished == 0, d);
  }
  {
    /* With an explicit ceiling it is the run iris_train makes with that
       ceiling: reseed from the instrument's own seed, then converge. */
    iris *k = lived_in(2, 12, 3, 32, 14, 99u);
    memcpy(SNAP, A, sizeof A);
    iris_reseed(k, iris_seed(k));
    iris_train_converge(k, 3000, 0, 0);
    memcpy(RA, A, sizeof A);
    memcpy(A, SNAP, sizeof A);
    iris_train_begin(k, 3000);
    while (iris_train_slice(k, 250)) {}
    int same = memcmp(RA, A, sizeof A) == 0;
    snprintf(d, sizeof d, "ceiling 3000, 250-epoch slices: byte-identical %d, epochs %d",
             same, iris_train_epochs_done(k));
    check("a sliced run with a ceiling is the cold run with that ceiling", same, d);
  }

  /* ---- 3. leave-one-out never answers with a not-a-number --------------- */
  {
    iris *k = lived_in(2, 12, 3, 32, 14, 3u);
    k->ex[k->n_in] = __builtin_nanf("");
    float e = iris_loo_error(k, 30);
    k->ex[k->n_in] = 0.5f;
    float good = iris_loo_error(k, 30);
    snprintf(d, sizeof d, "poisoned store %.1f (not-a-number %d), clean store %.5f",
             (double)e, is_nan(e), (double)good);
    check("iris_loo_error refuses with -1, never a not-a-number", e == -1.0f && !is_nan(e) && good >= 0.0f, d);
  }

  /* ---- 4. the stuck refusal holds on every call -------------------------- */
  {
    /* The instrument a cold start from this seed should give, for comparison
       at the end: iris_init, the same demonstrations, iris_train. */
    iris *f = iris_init(A, sizeof A, 2, 12, 3, 32, 314u);
    record_n(f, 20, 2718ul);
    iris_train(f);
    memcpy(RA, A, sizeof A);

    iris *k = iris_init(A, sizeof A, 2, 12, 3, 32, 314u);
    record_n(k, 20, 2718ul);
    iris_internal_set_learning(k, 2.0f, 0.99f);   /* force a divergence */
    iris_train_converge(k, 0, 0, 0);
    const int diverged = iris_get_status(k) == IRIS_TRAINING_DIVERGED && iris_internal_pinned(k);
    iris_internal_set_learning(k, 0.10f, 0.85f);  /* back to the defaults */

    /* The first refusal writes the status and nothing else. */
    memcpy(SNAP, A, sizeof A);
    { int32_t st = IRIS_DIVERGED_STUCK;
      memcpy(SNAP + ((unsigned char *)&k->status - A), &st, sizeof st); }
    float r0 = iris_train_converge(k, 0, 0, 0);
    const int first_only_status = r0 == -1.0f && same_arena();

    /* Then every warm call refuses, and nothing moves: not a weight, not the
       epoch count. Zeroing the velocity first (iris_correct) changes the
       velocities, so only the weights and counters are compared there. */
    const size_t wbytes = sizeof(float) * (size_t)(12 * 2 + 12 + 3 * 12 + 3);
    static float w0[12 * 2 + 12 + 3 * 12 + 3];
    memcpy(w0, k->w1, wbytes);
    const int done0 = iris_train_epochs_done(k);
    int held = 0, calls = 0;
    for (int round = 0; round < 3; ++round) {
      float r[5];
      cb_calls = 0;
      r[0] = iris_train_converge(k, 0, 0, 0);
      r[1] = iris_train_epochs(k, 100);
      r[2] = iris_train_converge(k, 4000, count_cb, 0);
      r[3] = iris_correct(k, 0);
      r[4] = iris_train_slice(k, 100) == 0 ? -1.0f : 0.0f;   /* no run to continue */
      for (int c = 0; c < 5; ++c) {
        calls++;
        if (r[c] == -1.0f && iris_get_status(k) == IRIS_DIVERGED_STUCK && cb_calls == 0
            && memcmp(w0, k->w1, wbytes) == 0 && iris_train_epochs_done(k) == done0) held++;
      }
    }
    /* A status overwritten by an unrelated call does not let a warm run in. */
    { float bad[2] = { __builtin_nanf(""), 0.5f }, o[3] = { 100.0f, 200.0f, 300.0f };
      iris_record(k, bad, o); }
    const int st_nan = iris_get_status(k) == IRIS_NAN_TRAPPED;
    float r_after_nan = iris_train_converge(k, 0, 0, 0);
    const int still = st_nan && r_after_nan == -1.0f && iris_get_status(k) == IRIS_DIVERGED_STUCK
                   && memcmp(w0, k->w1, wbytes) == 0;

    /* iris_train is the way out, and it gives the cold-start instrument. */
    int trained = iris_train(k);
    const int way_out = trained == 1 && iris_get_status(k) == IRIS_STATUS_OK && !iris_internal_pinned(k)
                     && memcmp(RA, A, sizeof A) == 0;
    const int warm_again = iris_train_epochs(k, 50) >= 0.0f && iris_get_status(k) == IRIS_STATUS_OK;

    snprintf(d, sizeof d, "diverged %d; first refusal wrote only the status %d; %d of %d warm calls "
             "refused with nothing moved; after a refused record %d; iris_train %d, same as cold %d; warm again %d",
             diverged, first_only_status, held, calls, still, trained, way_out, warm_again);
    check("the stuck refusal holds on every warm call", diverged && first_only_status && held == calls
          && still && way_out && warm_again, d);
  }
  {
    /* Exactly on the limit is stuck, wherever it is -- a bias counts. Just
       inside it is not: 15.9953 is the largest weight measured in a healthy
       default fit. */
    iris *k = lived_in(2, 12, 3, 32, 14, 8u);
    iris_train(k);
    memcpy(SNAP, A, sizeof A);
    k->w2[5] = 15.9953f;
    float inside = iris_train_epochs(k, 20);
    int inside_ok = inside >= 0.0f && iris_get_status(k) != IRIS_DIVERGED_STUCK;
    memcpy(A, SNAP, sizeof A);
    k->b2[1] = -IRIS_W_LIMIT;
    float on = iris_train_epochs(k, 20);
    int on_ok = on == -1.0f && iris_get_status(k) == IRIS_DIVERGED_STUCK;
    snprintf(d, sizeof d, "weight at 15.9953 trained (%.2e), bias at -limit refused (%.1f, status %d)",
             (double)inside, (double)on, (int)iris_get_status(k));
    check("stuck means exactly on the limit, biases included", inside_ok && on_ok, d);
  }

  if (fails) printf("\n  %d FAILING\n", fails);
  else       printf("\n  all pass\n");
  return fails ? 1 : 0;
}
