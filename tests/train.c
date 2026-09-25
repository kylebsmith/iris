/* SPDX-License-Identifier: BSD-3-Clause
   Copyright (c) 2026 Kyle Smith */
/* ============================================================================
   train.c — the trainers' state logic, checked byte by byte.

   Every check here compares whole arenas with memcmp, so "changed nothing"
   and "bit-identical" mean every byte the instrument owns, not a few fields
   someone thought to look at.

     1. A refusal changes nothing but, for a poisoned store, the status.
        Every trainer and diagnostic is handed a store with a not-a-number or
        an infinity written straight into it, and must refuse with every byte
        but the status word unchanged and the status IRIS_NAN_TRAPPED; handed
        an empty store, it must refuse without touching one byte.
     2. iris_train_begin + iris_train_slice is iris_train, bit for bit, over
        several shapes, seeds and slice sizes, starting from an instrument
        that has already been trained, warm-trained, edited and deleted from.
     3. iris_loo_error refuses with exactly -1, never a not-a-number.
     4. Once a divergence leaves a weight on the limit, every trainer that
        continues from the current weights refuses on every call -- no
        alternation, whatever touches the status in between -- and iris_train
        is the way out, to the instrument a cold start gives.
     5. iris_suggest_smoothing puts every byte of the arena back, on a stale
        instrument that is still playing with live momentum, and its answer
        does not move when one output is recorded in units 1000 times larger.

   Not-a-number and infinity are built with __builtin_nanf and __builtin_inff,
   never by dividing by zero, so -fsanitize=float-divide-by-zero can run over
   this file.

   Build and run, from the repository root (build/ is not tracked, so a fresh
   checkout has to make it):
     mkdir -p build && cc -std=c99 -O2 -Wall -Wextra -I. -o build/train tests/train.c && ./build/train
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
   iris_internal_tanh, so every value is finite and every instrument is
   learnable. */
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
      out[o] = 100.0f * (float)(o + 1) + 40.0f * iris_internal_tanh(0.15f * s - 0.4f * (float)o);
    iris_record(k, in, out);
  }
}

/* Three outputs that want different amounts of smoothing: two smooth ones
   with take-to-take noise of about +/-amp, and one sharp, clean step. The
   step can be recorded in other units by scale2. */
static float noise(void) {       /* three draws, in an order C guarantees */
  const float a = lcg01(), b = lcg01(), c = lcg01();
  return a + b + c - 1.5f;
}
static void record_mixed(iris *k, int n, unsigned long seed, float amp, float scale2) {
  lcg = seed;
  for (int r = 0; r < n; ++r) {
    float in[2], out[3];
    in[0] = lcg01(); in[1] = lcg01();
    out[0] = 0.5f + 0.3f * iris_internal_tanh(2.0f * (in[0] - 0.5f)) + amp * noise();
    out[1] = 0.4f + 0.3f * in[1] + amp * noise();
    out[2] = (in[0] + in[1] > 1.0f ? 0.8f : 0.2f) + 0.2f * iris_internal_tanh(6.0f * (in[0] - in[1]));
    out[2] *= scale2;
    iris_record(k, in, out);
  }
}
static const float LADDER[5] = { 0.0f, 0.05f, 0.15f, 0.5f, 1.0f };   /* iris_suggest_smoothing's */
static int argmin5(const float *s) {
  int b = 0;
  for (int i = 1; i < 5; ++i) if (s[i] < s[b]) b = i;
  return b;
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
  iris_continue(k, 13);
  iris_delete_id(k, 3);
  iris_delete_index(k, 0);
  record_n(k, 1, 77ul + seed);
  return k;
}

static int same_arena(void) { return memcmp(A, SNAP, sizeof A) == 0; }
/* Every byte but the status word, which is compared on its own. */
static int same_but_status(const iris *k) {
  const size_t at = (size_t)((const unsigned char *)&k->status - A), n = sizeof k->status;
  return memcmp(A, SNAP, at) == 0 && memcmp(A + at + n, SNAP + at + n, sizeof A - at - n) == 0;
}
static int is_nan(float x) { return x != x; }

static int cb_calls = 0;
static int count_cb(void *user, int done, int ceiling, float err) {
  (void)user; (void)done; (void)ceiling; (void)err;
  cb_calls++;
  return 1;
}

/* Every trainer and diagnostic in PART 8 on one instrument whose store the
   caller has just made untrainable. Each call starts from the same snapshot,
   with the status set to IRIS_RIDGE_ESCALATED so that both a write and its
   absence show; afterwards every byte but the status must be as it was, and
   the status must be want (IRIS_RIDGE_ESCALATED for "left alone"). Returns
   the number of calls that answered wrongly or changed what they may not. */
static int refusals_change_nothing(iris *k, int32_t want, char *why, size_t whylen) {
  static unsigned char scratch[sizeof A];
  int bad = 0;
  k->status = IRIS_RIDGE_ESCALATED;
  memcpy(SNAP, A, sizeof A);
  memset(scratch, 0xA5, sizeof scratch);
  const float prog = iris_train_progress(k);
  const int busy = iris_train_busy(k), done = iris_train_epochs_done(k);
  why[0] = 0;
#define REFUSES_AS(expr, label, st) do {                                    \
      if (!(expr) || !same_but_status(k) || k->status != (st)) {              \
        bad++; snprintf(why + strlen(why), whylen - strlen(why), " %s", label); } \
      memcpy(A, SNAP, sizeof A); } while (0)
#define REFUSES(expr, label) REFUSES_AS(expr, label, want)
  REFUSES(iris_train(k) == 0, "train");
  REFUSES(iris_continue(k, 50) == -1.0f, "continue");
  cb_calls = 0;
  REFUSES(iris_continue_to_plateau(k, 4000, count_cb, 0) == -1.0f && cb_calls == 0, "plateau");
  REFUSES(iris_continue_to_plateau(k, 0, 0, 0) == -1.0f, "plateau-default");
  REFUSES(iris_train_begin(k, 0) == 0, "begin");
  /* no run is in flight, so a slice has nothing to continue and asks nothing */
  REFUSES_AS(iris_train_slice(k, 50) == 0, "slice", IRIS_RIDGE_ESCALATED);
  REFUSES(iris_internal_train_run(k, 50, 0, 0, 0, 0) == -1.0f, "engine");
  REFUSES_AS(iris_train_progress(k) == prog && iris_train_busy(k) == busy
             && iris_train_epochs_done(k) == done, "progress/busy/done", IRIS_RIDGE_ESCALATED);
  { float e = iris_loo_error(k, 30);
    REFUSES(e == -1.0f && !is_nan(e), "loo_error"); }
  { int untouched = 1;
    float s = iris_suggest_smoothing(k, scratch, sizeof scratch);
    for (size_t i = 0; i < sizeof scratch; ++i) if (scratch[i] != 0xA5) { untouched = 0; break; }
    REFUSES(s == -1.0f && untouched, "suggest_smoothing"); }
  /* too little scratch is a mistake in the call, which leaves the status
     alone even when the store is poisoned too */
  REFUSES_AS(iris_suggest_smoothing(k, scratch, 16) == -1.0f, "suggest-small-scratch",
             IRIS_RIDGE_ESCALATED);
#undef REFUSES
#undef REFUSES_AS
  return bad;
}

int main(void) {
  char d[512], why[160];

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
        int b = refusals_change_nothing(k, IRIS_NAN_TRAPPED, why, sizeof why);
        if (b && !where[0]) snprintf(where, sizeof where, "shape %d poison %d:%.100s", s, poison, why);
        bad += b; cases++;
      }
    }
    snprintf(d, sizeof d, "%d shape/poison cases x 11 calls: %d wrong%s%s",
             cases, bad, where[0] ? " -- " : "", where);
    check("a poisoned store is refused, only the status set", bad == 0, d);
  }
  {
    /* An empty store, and two demonstrations (too few for leave-one-out). */
    iris *k = lived_in(2, 12, 3, 32, 14, 9u);
    iris_clear(k);
    int bad_empty = refusals_change_nothing(k, IRIS_RIDGE_ESCALATED, why, sizeof why);
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
    /* A run in flight whose store goes bad ends and reports IRIS_NAN_TRAPPED
       -- the two writes a slice refused over a poisoned store makes -- and
       nothing else moves. */
    iris *k = lived_in(2, 12, 3, 32, 14, 21u);
    iris_train_begin(k, 0);
    iris_train_slice(k, 300);
    k->ex[4] = __builtin_nanf("");
    memcpy(SNAP, A, sizeof A);
    { int32_t zero = 0, trapped = IRIS_NAN_TRAPPED;
      memcpy(SNAP + ((unsigned char *)&k->tr_running - A), &zero, sizeof zero);
      memcpy(SNAP + ((unsigned char *)&k->status - A), &trapped, sizeof trapped); }
    int more = iris_train_slice(k, 300);
    snprintf(d, sizeof d, "slice returned %d, busy %d, status %d, every other byte unchanged %d",
             more, iris_train_busy(k), (int)iris_get_status(k), same_arena());
    check("a slice over a poisoned store ends the run, reports", more == 0 && !iris_train_busy(k) && same_arena(), d);
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
       ceiling: reseed from the instrument's own seed, then continue to the
       plateau. */
    iris *k = lived_in(2, 12, 3, 32, 14, 99u);
    memcpy(SNAP, A, sizeof A);
    iris_reseed(k, iris_seed(k));
    iris_continue_to_plateau(k, 3000, 0, 0);
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
    iris_continue_to_plateau(k, 0, 0, 0);
    const int diverged = iris_get_status(k) == IRIS_TRAINING_DIVERGED && iris_internal_pinned(k);
    iris_internal_set_learning(k, 0.10f, 0.85f);  /* back to the defaults */

    /* The first refusal writes the status and nothing else. */
    memcpy(SNAP, A, sizeof A);
    { int32_t st = IRIS_DIVERGED_STUCK;
      memcpy(SNAP + ((unsigned char *)&k->status - A), &st, sizeof st); }
    float r0 = iris_continue_to_plateau(k, 0, 0, 0);
    const int first_only_status = r0 == -1.0f && same_arena();

    /* Then every warm call refuses, and nothing moves: not a weight, not the
       epoch count. */
    const size_t wbytes = sizeof(float) * (size_t)(12 * 2 + 12 + 3 * 12 + 3);
    static float w0[12 * 2 + 12 + 3 * 12 + 3];
    memcpy(w0, k->w1, wbytes);
    const int done0 = iris_train_epochs_done(k);
    int held = 0, calls = 0;
    for (int round = 0; round < 3; ++round) {
      float r[4];
      cb_calls = 0;
      r[0] = iris_continue_to_plateau(k, 0, 0, 0);
      r[1] = iris_continue(k, 100);
      r[2] = iris_continue_to_plateau(k, 4000, count_cb, 0);
      r[3] = iris_train_slice(k, 100) == 0 ? -1.0f : 0.0f;   /* no run to continue */
      for (int c = 0; c < 4; ++c) {
        calls++;
        if (r[c] == -1.0f && iris_get_status(k) == IRIS_DIVERGED_STUCK && cb_calls == 0
            && memcmp(w0, k->w1, wbytes) == 0 && iris_train_epochs_done(k) == done0) held++;
      }
    }
    /* A status overwritten by an unrelated call does not let a warm run in. */
    { float bad[2] = { __builtin_nanf(""), 0.5f }, o[3] = { 100.0f, 200.0f, 300.0f };
      iris_record(k, bad, o); }
    const int st_nan = iris_get_status(k) == IRIS_NAN_TRAPPED;
    float r_after_nan = iris_continue_to_plateau(k, 0, 0, 0);
    const int still = st_nan && r_after_nan == -1.0f && iris_get_status(k) == IRIS_DIVERGED_STUCK
                   && memcmp(w0, k->w1, wbytes) == 0;

    /* iris_train is the way out, and it gives the cold-start instrument. */
    int trained = iris_train(k);
    const int way_out = trained == 1 && iris_get_status(k) == IRIS_STATUS_OK && !iris_internal_pinned(k)
                     && memcmp(RA, A, sizeof A) == 0;
    const int warm_again = iris_continue(k, 50) >= 0.0f && iris_get_status(k) == IRIS_STATUS_OK;

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
    float inside = iris_continue(k, 20);
    int inside_ok = inside >= 0.0f && iris_get_status(k) != IRIS_DIVERGED_STUCK;
    memcpy(A, SNAP, sizeof A);
    k->b2[1] = -IRIS_W_LIMIT;
    float on = iris_continue(k, 20);
    int on_ok = on == -1.0f && iris_get_status(k) == IRIS_DIVERGED_STUCK;
    snprintf(d, sizeof d, "weight at 15.9953 trained (%.2e), bias at -limit refused (%.1f, status %d)",
             (double)inside, (double)on, (int)iris_get_status(k));
    check("stuck means exactly on the limit, biases included", inside_ok && on_ok, d);
  }

  /* ---- 5. asking for a smoothing value leaves the arena exactly -------- */
  {
    /* An instrument in the state most likely to be damaged by a save and load
       round trip: trained, warm-trained (live velocities), then a take
       recorded after training (stale but playing), and a status set by a
       refused reading. The scratch is exactly iris_size bytes, from the heap,
       so a sanitizer sees any access past it. */
    const int shapes[3][5] = { { 2, 12, 3, 32, 14 }, { 5, 16, 2, 40, 20 }, { 1, 8, 1, 16, 9 } };
    int ok_all = 1; char first[200] = "";
    for (int s = 0; s < 3; ++s) {
      const int *sh = shapes[s];
      iris *k = lived_in(sh[0], sh[1], sh[2], sh[3], sh[4], 60u + (uint32_t)s);
      { float bad[5] = { __builtin_nanf(""), 0, 0, 0, 0 }, o[5] = { 0, 0, 0, 0, 0 };
        iris_record(k, bad, o); }
      const size_t need = iris_size(sh[0], sh[1], sh[2], sh[3]);
      unsigned char *scratch = (unsigned char *)malloc(need);
      if (!scratch) return 2;
      const int stale = !iris_is_trained(k) && k->fitted && iris_get_status(k) == IRIS_NAN_TRAPPED;
      memcpy(SNAP, A, sizeof A);
      float v = iris_suggest_smoothing(k, scratch, need);
      const int on_ladder = v == 0.0f || v == 0.05f || v == 0.15f || v == 0.5f || v == 1.0f;
      const int same = same_arena();
      /* and a warm run afterwards is the warm run that would have happened */
      iris_continue(k, 20);
      memcpy(RA, A, sizeof A);
      memcpy(A, SNAP, sizeof A);
      iris_continue(k, 20);
      const int continues = memcmp(RA, A, sizeof A) == 0;
      /* one byte short, or overlapping the instrument itself, is refused */
      memcpy(A, SNAP, sizeof A);
      const float short1 = iris_suggest_smoothing(k, scratch, need - 1);
      const float overlap = iris_suggest_smoothing(k, A, sizeof A);
      const int refusals = short1 == -1.0f && overlap == -1.0f && same_arena();
      free(scratch);
      if (!(stale && on_ladder && same && continues && refusals)) {
        ok_all = 0;
        if (!first[0]) snprintf(first, sizeof first, " -- shape %d: stale %d, pick %.2f, unchanged %d, continues %d, refusals %d",
                                s, stale, (double)v, same, continues, refusals);
      }
    }
    snprintf(d, sizeof d, "3 shapes, stale-but-playing with live velocities: every byte restored, warm run unchanged, "
             "short and overlapping scratch refused%s", first);
    check("iris_suggest_smoothing restores the arena exactly", ok_all, d);
  }
  {
    /* Scale one of three outputs by 1000 and the suggestion must not move.
       Two datasets where it matters: scored in raw units the picks differ
       (checked below, so the data cannot quietly stop testing anything). */
    const unsigned long ds[2] = { 19ul * 7919ul, 13ul * 7919ul };
    const float amp[2] = { 0.03f, 0.06f };
    int ok_all = 1; char msg[200] = "";
    for (int t = 0; t < 2; ++t) {
      float pick[2], norm[2][5], raw[2][5];
      for (int sc = 0; sc < 2; ++sc) {
        iris *k = iris_init(A, sizeof A, 2, 12, 3, 32, 4242u);
        record_mixed(k, 16, ds[t], amp[t], sc ? 1000.0f : 1.0f);
        iris_train(k);
        static unsigned char scratch[sizeof A];
        memcpy(SNAP, A, sizeof A);
        for (int i = 0; i < 5; ++i) {
          iris_set_smoothing(k, LADDER[i]);
          norm[sc][i] = iris_internal_loo(k, 0, 1); memcpy(A, SNAP, sizeof A);
          iris_set_smoothing(k, LADDER[i]);
          raw[sc][i]  = iris_internal_loo(k, 0, 0); memcpy(A, SNAP, sizeof A);
        }
        pick[sc] = iris_suggest_smoothing(k, scratch, sizeof scratch);
      }
      float worst = 0.0f;
      for (int i = 0; i < 5; ++i) {
        float r = (norm[1][i] - norm[0][i]) / norm[0][i];
        if (r < 0.0f) r = -r;
        if (r > worst) worst = r;
      }
      const int raw_differs = argmin5(raw[0]) != argmin5(raw[1]);
      const int ok = pick[0] == pick[1] && pick[0] == LADDER[argmin5(norm[0])] && worst < 1e-4f && raw_differs;
      if (!ok) ok_all = 0;
      snprintf(msg + strlen(msg), sizeof msg - strlen(msg), "%spick %.2f/%.2f (raw units would pick %.2f/%.2f), score moved %.1e",
               t ? "; " : "", (double)pick[0], (double)pick[1],
               (double)LADDER[argmin5(raw[0])], (double)LADDER[argmin5(raw[1])], (double)worst);
    }
    check("the suggestion ignores the units an output is recorded in", ok_all, msg);
  }

  if (fails) printf("\n  %d FAILING\n", fails);
  else       printf("\n  all pass\n");
  return fails ? 1 : 0;
}
