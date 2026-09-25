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
     6. A not-a-number met partway through a gradient run -- finite
        demonstrations whose width overflows -- makes the trainer return -1
        (iris_train 0) and leaves the seed's unfitted start, the ledger empty
        and the status IRIS_NAN_TRAPPED.
     7. iris_last_error means one thing: after iris_train, every slice,
        iris_continue and the closed-form solve it equals, to the bit, what a
        load of the saved instrument reports.
     8. Smoothing's weight decay never leaves a weight below IRIS_TINY and
        not zero, so no weight goes subnormal, where a board that flushes
        subnormal numbers and a laptop that does not would part ways.
     9. What the header says the set-up does: starting weights are each
        draw times the rounded reciprocal of the root of the fan-in; every
        blocking call starts its shuffle from the identity order; the
        learning-rate and momentum clamps pass Weka's pair.
    10. iris_clear empties the worst-demonstration ledger, so no take recorded
        after it is named for a miss made on a cleared one.
    11. A sliced run sees every edit made between two slices -- a record, a
        delete, and a delete followed by a record, which leaves the count as
        it was -- and restarts its plateau window, so the edit is not read as
        a plateau that ends the run at the next window test.
    12. iris_reseed ends a sliced run in flight: no later slice trains the
        new seed's weights inside the old run.
    13. The divergence guard reaches every weight and bias, the last of each
        array included: one past the limit is clamped to exactly the limit
        and the run reports IRIS_TRAINING_DIVERGED.
    14. After a record or a delete between two slices, the shuffle covers
        every take in the store exactly once.

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
       inside it is not: 15.9953 is the largest weight of the healthy default
       fits measured in iris.h's note on IRIS_W_LIMIT. */
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

  /* ---- 6. a not-a-number partway through a run ----------------------------
     Six demonstrations, trained, then two more at -2e38 and 2e38 on the
     first input: every value is finite, so every trainer accepts the store,
     but the input's width overflows to infinity and normalising either end
     gives a not-a-number in the first epoch. Each gradient trainer must
     return -1 (iris_train 0; a slice ends its run) and leave the instrument
     exactly where iris_reseed(k, iris_seed(k)) puts one, with an empty
     worst-demonstration ledger, the status IRIS_NAN_TRAPPED and nothing
     fitted. */
  {
    int right = 0; char wrong[160] = "";
    for (int t = 0; t < 4; ++t) {
      iris *k = iris_init(A, sizeof A, 2, 12, 1, 16, 99u);
      for (int i = 0; i < 6; ++i) {
        float in[2] = { (float)i, (float)(i % 3) }, out[1] = { (float)i };
        iris_record(k, in, out);
      }
      iris_train(k);
      { float in[2] = { -2e38f, 0.0f }, out[1] = { 0.0f }; iris_record(k, in, out); }
      { float in[2] = { 2e38f, 1.0f }, out[1] = { 1.0f }; iris_record(k, in, out); }
      int ret_ok;
      switch (t) {
        case 0:  ret_ok = iris_continue(k, 50) == -1.0f; break;
        case 1:  ret_ok = iris_continue_to_plateau(k, 0, 0, 0) == -1.0f; break;
        case 2:  ret_ok = iris_train(k) == 0; break;
        default: ret_ok = iris_train_begin(k, 0) == 1 && iris_train_slice(k, 50) == 0
                          && !iris_train_busy(k); break;
      }
      /* the weights must be the ones the seed draws: reseed a snapshot of
         the arena and compare, then put the arena back */
      static float w_after[12 * 2 + 12 + 1 * 12 + 1];
      memcpy(SNAP, A, sizeof A);
      memcpy(w_after, k->w1, sizeof w_after);
      iris_reseed(k, iris_seed(k));
      const int seed_weights = memcmp(w_after, k->w1, sizeof w_after) == 0;
      memcpy(A, SNAP, sizeof A);
      float stress = iris_example_stress(k, 0);
      const int ok = ret_ok && seed_weights && iris_get_status(k) == IRIS_NAN_TRAPPED
                  && !iris_is_trained(k) && !k->fitted && iris_last_error(k) == 1.0f
                  && stress == -1.0f && !iris_train_busy(k);
      right += ok;
      if (!ok && !wrong[0])
        snprintf(wrong, sizeof wrong, " -- trainer %d: return %d, seed weights %d, status %d, "
                 "trained %d, fitted %d, last error %g, stress %g",
                 t, ret_ok, seed_weights, (int)iris_get_status(k), iris_is_trained(k),
                 k->fitted, (double)iris_last_error(k), (double)stress);
    }
    snprintf(d, sizeof d, "%d of 4 gradient trainers returned -1 (iris_train 0) and left the "
             "seed's unfitted start%s", right, wrong);
    check("a not-a-number partway returns -1 and resets to the seed", right == 4, d);
  }

  /* ---- 7. iris_last_error has one meaning --------------------------------
     The training error of the weights the instrument holds, measured when
     the trainer finishes. So the figure a trainer leaves must be the figure
     a load of its save reports, to the bit -- after iris_train, after every
     slice of a sliced run, after iris_continue (whose return value it also
     is) and after the closed-form solve -- and it is not the last epoch's
     running error, which the plateau test reads. */
  {
    static unsigned char file[16384], LB[sizeof A];
    static unsigned char scr[IRIS_ELM_SCRATCH(12, 3)];
    int agree = 0, total = 0, differs_from_run = 0;
    char first[200] = "";
#define AGREES(label) do {                                                    \
      const size_t fn = iris_save(k, file, sizeof file);                      \
      iris *l = iris_init(LB, sizeof LB, 2, 12, 3, 32, 5u);                    \
      const int same = fn > 0 && iris_load(l, file, fn)                       \
                    && iris_last_error(l) == iris_last_error(k);              \
      total++; agree += same;                                                 \
      if (!same && !first[0])                                                 \
        snprintf(first, sizeof first, " -- %s: %.9g, loaded %.9g", label,     \
                 (double)iris_last_error(k), (double)iris_last_error(l));     \
    } while (0)
    iris *k = iris_init(A, sizeof A, 2, 12, 3, 32, 4242u);
    record_n(k, 20, 31337ul);
    iris_train(k);
    AGREES("iris_train");
    differs_from_run += iris_last_error(k) != k->tr_err;
    iris_train_begin(k, 0);
    for (int n = 0; n < 5 && iris_train_slice(k, 700); ++n) AGREES("a slice");
    const float ret = iris_continue(k, 300);
    AGREES("iris_continue");
    const int ret_is_it = ret == iris_last_error(k);
    const int elm = iris_train_elm(k, 1e-4f, scr, sizeof scr);
    AGREES("iris_train_elm");
    iris_clear(k);
    const int cleared = iris_last_error(k) == 1.0f;
#undef AGREES
    snprintf(d, sizeof d, "%d of %d trainer results equal their load to the bit; iris_continue "
             "returned it %d; not the running error %d; solve %d; 1 after iris_clear %d%s",
             agree, total, ret_is_it, differs_from_run, elm, cleared, first);
    check("iris_last_error is the same after a trainer and a load", agree == total && ret_is_it
          && differs_from_run && elm >= 0 && cleared, d);
  }
  {
    /* A STALE instrument, as PART 9 and iris_last_error say: a take recorded
       after training is not measured until something measures it, and a
       load does -- the same weights over the demonstrations in the file, the
       bits iris_internal_recall_error gives on the unsaved instrument. */
    static unsigned char file[16384], LB[sizeof A];
    iris *k = iris_init(A, sizeof A, 2, 12, 3, 32, 4242u);
    record_n(k, 20, 31337ul);
    iris_train(k);
    { float in[2] = { 0.5f, 0.5f }, out[3] = { 3.0f, -2.0f, 7.0f }; iris_record(k, in, out); }
    const float before = iris_last_error(k);
    const size_t fn = iris_save(k, file, sizeof file);
    iris *l = iris_init(LB, sizeof LB, 2, 12, 3, 32, 5u);
    const int loaded = fn > 0 && iris_load(l, file, fn);
    float x[IRIS_MAX_IN];
    memcpy(SNAP, A, sizeof A);
    const float now = iris_internal_recall_error(k, x);
    memcpy(A, SNAP, sizeof A);
    snprintf(d, sizeof d, "before the save %.9g, after the load %.9g, the current demonstrations "
             "measure %.9g; trained %d and %d", (double)before, (double)iris_last_error(l),
             (double)now, iris_is_trained(k), iris_is_trained(l));
    check("a load measures a stale instrument's error afresh", loaded && !iris_is_trained(l)
          && iris_last_error(l) == now && now != before, d);
  }

  /* ---- 8. decayed weights are flushed like the velocities -----------------
     Smoothing shrinks every weight by a fraction of itself on every visit.
     A weight with no gradient -- here every weight from an input that never
     moved, which normalises to 0 -- then only shrinks, geometrically, and
     without a flush it passes through the subnormal numbers, which some
     processors flush to zero and others do not. At smoothing 1 on these
     six demonstrations the first weight falls below 1e-30 after 2,225
     epochs and goes subnormal after 2,832, so the run is taken in slices of
     25 epochs and every weight is looked at after each: none may be nonzero
     and smaller than IRIS_TINY (1e-30) in magnitude. */
  {
    iris *k = iris_init(A, sizeof A, 2, 12, 1, 16, 7u);
    for (int i = 0; i < 6; ++i) {
      float in[2] = { (float)i / 5.0f, 3.0f }, out[1] = { (float)(i * i) };
      iris_record(k, in, out);
    }
    iris_set_smoothing(k, 1.0f);
    const int nw = 12 * 2 + 12 + 1 * 12 + 1;
    int tiny = 0, slices = 0, exact_zero = 0;
    iris_train_begin(k, 8000);
    while (iris_train_slice(k, 25)) {
      slices++;
      for (int i = 0; i < nw; ++i) {
        const float w = k->w1[i];
        if (w != 0.0f && w < IRIS_TINY && w > -IRIS_TINY) tiny++;
      }
    }
    for (int h = 0; h < 12; ++h) exact_zero += k->w1[h * 2 + 1] == 0.0f;
    snprintf(d, sizeof d, "%d epochs in %d slices: %d weight readings below 1e-30 and not zero; "
             "%d of 12 weights from the still input decayed to exactly 0",
             iris_train_epochs_done(k), slices, tiny, exact_zero);
    check("smoothing never leaves a weight subnormal", tiny == 0 && exact_zero == 12
          && iris_train_epochs_done(k) >= 4000, d);
  }

  /* ---- 9. what the header says the trainers' set-up does -----------------
     (a) The starting weights are each draw times the reciprocal of the
     square root of the unit's fan-in, rounded once, not the draw divided by
     the root, which rounds differently (and the check requires that it
     would, somewhere, so it can tell the two apart). (b) Every blocking
     call starts its shuffle from the identity order: scrambling order[]
     before a warm call changes no byte of what it produces. (c) The
     learning-rate and momentum clamps let Weka's pair, 0.3 and 0.2, through
     exactly and hold 5 and 1.5 at 2 and 0.99. */
  {
    const int shapes[3][3] = { { 2, 12, 3 }, { 5, 20, 5 }, { 1, 8, 1 } };
    int wrong = 0, quotient_differs = 0;
    for (int sh = 0; sh < 3; ++sh)
      for (uint32_t seed = 1; seed <= 50; ++seed) {
        const int ni = shapes[sh][0], nh = shapes[sh][1], no = shapes[sh][2];
        iris *k = iris_init(A, sizeof A, ni, nh, no, 16, seed);
        iris_internal_rng r = { seed };
        const float r1 = iris_internal_sqrt((float)ni), r2 = iris_internal_sqrt((float)nh);
        const float s1 = 1.0f / r1, s2 = 1.0f / r2;
        for (int i = 0; i < nh * ni; ++i) {
          const float u = iris_internal_rand_sym(&r);
          wrong += k->w1[i] != u * s1;
          quotient_differs += u * s1 != u / r1;
        }
        for (int i = 0; i < no * nh; ++i) {
          const float u = iris_internal_rand_sym(&r);
          wrong += k->w2[i] != u * s2;
          quotient_differs += u * s2 != u / r2;
        }
      }

    iris *k = lived_in(2, 12, 3, 32, 14, 21u);
    memcpy(SNAP, A, sizeof A);
    iris_continue(k, 5);
    memcpy(RA, A, sizeof A);
    memcpy(A, SNAP, sizeof A);
    for (int i = 0; i < k->n_ex; ++i) k->order[i] = k->n_ex - 1 - i;   /* reversed */
    iris_continue(k, 5);
    const int restarts = memcmp(RA, A, sizeof A) == 0;

    iris_internal_set_learning(k, 0.3f, 0.2f);
    const int weka = k->lr == 0.3f && k->momentum == 0.2f;
    iris_internal_set_learning(k, 5.0f, 1.5f);
    const int clamped = k->lr == 2.0f && k->momentum == 0.99f;
    iris_internal_set_learning(k, 0.10f, 0.85f);

    snprintf(d, sizeof d, "starting weights: %d of every weight in 150 instruments not draw times "
             "rounded reciprocal (division would round %d differently); warm call ignores the "
             "order left behind %d; Weka's pair kept %d, clamps %d",
             wrong, quotient_differs, restarts, weka, clamped);
    check("reciprocal starting weights, fresh shuffle, clamps", wrong == 0 && quotient_differs > 0
          && restarts && weka && clamped, d);
  }

  /* ---- 10. iris_clear empties the worst-demonstration ledger -------------
     Twelve demonstrations, the sixth a bad take, fitted by iris_train and
     by the closed-form solve, so the ledger holds a sum for every take and
     names one (iris_train the bad take, the closed-form ledger its
     neighbour: the endpoint ledger can miss, PART 8f). After iris_clear and
     twelve clean takes recorded at the same positions, and before any
     training, nothing may be named: every stress -1, iris_worst_example and
     iris_worst_example_id -1 with a margin of 0, every slot of the ledger 0,
     and iris_train_progress 0.0. Training the clean takes then fills the
     ledger again, so the check tells an emptied ledger from a dead one. */
  {
    static unsigned char scr[IRIS_ELM_SCRATCH(12, 1)];
    int right = 0; char first[200] = "";
    for (int t = 0; t < 2; ++t) {
      iris *k = iris_init(A, sizeof A, 2, 12, 1, 16, 11u);
      for (int i = 0; i < 12; ++i) {
        float in[2] = { (float)(i % 4), (float)(i / 4) }, out[1] = { in[0] + in[1] };
        if (i == 5) out[0] = 40.0f;
        iris_record(k, in, out);
      }
      if (t == 0) iris_train(k); else iris_train_elm(k, 1e-4f, scr, sizeof scr);
      const int named = iris_worst_example(k, 0);
      iris_clear(k);
      for (int i = 0; i < 12; ++i) {
        float in[2] = { (float)(i % 4), (float)(i / 4) }, out[1] = { 0.1f * in[0] };
        iris_record(k, in, out);
      }
      float m = -1.0f;
      const int worst = iris_worst_example(k, &m), worst_id = iris_worst_example_id(k, 0);
      const float progress = iris_train_progress(k);
      int stressed = 0, slots = 0;
      for (int i = 0; i < 12; ++i) stressed += iris_example_stress(k, i) != -1.0f;
      for (int i = 0; i < k->cap; ++i) slots += k->ex_res[i] != 0.0f;
      const int trained = iris_train(k), again = iris_worst_example(k, 0);
      const int ok = named >= 0 && worst == -1 && worst_id == -1 && m == 0.0f && progress == 0.0f
                  && stressed == 0 && slots == 0 && trained && again >= 0;
      right += ok;
      if (!ok && !first[0])
        snprintf(first, sizeof first, " -- %s: named %d before the clear; after it worst %d "
                 "(margin %.2f), id %d, %d stresses and %d slots not empty, progress %g; "
                 "trained again, worst %d", t ? "closed form" : "iris_train", named, worst,
                 (double)m, worst_id, stressed, slots, (double)progress, again);
    }
    snprintf(d, sizeof d, "%d of 2 trainers' ledgers emptied by iris_clear%s", right, first);
    check("iris_clear empties the worst-demonstration ledger", right == 2, d);
  }

  /* ---- 11. a sliced run sees every edit between slices ------------------
     Twenty demonstrations, sliced to epoch 3,999, one epoch short of a
     plateau test. Then an edit: a take far off the mapping recorded, the
     fourth take deleted, or the fourth take deleted and the far take
     recorded in its place. The edit raises the error, so a plateau window
     that was not restarted would compare the risen error with the reference
     taken at epoch 2,000 and end the run at epoch 4,000. Restarted, the
     window test at 4,000 only takes a new reference, so the run must still
     be going after it. The same run with no edit goes on past 4,000 too,
     which shows the window test there would not have ended it anyway. */
  {
    int right = 0; char first[200] = "";
    const char *names[4] = { "no edit", "record", "delete", "delete then record" };
    for (int t = 0; t < 4; ++t) {
      iris *k = iris_init(A, sizeof A, 2, 12, 1, 64, 11u);
      for (int i = 0; i < 20; ++i) {
        const float u = (float)((i * 7919) % 97) / 97.0f, v = (float)((i * 6131) % 89) / 89.0f;
        float in[2] = { u, v }, out[1] = { 0.5f + 0.4f * iris_internal_tanh(3.0f * (u - 0.5f)) * v };
        iris_record(k, in, out);
      }
      iris_train_begin(k, 0);
      while (iris_train_busy(k) && iris_train_epochs_done(k) < 3999)
        iris_train_slice(k, 3999 - iris_train_epochs_done(k));
      const int at = iris_train_epochs_done(k);
      float in[2] = { 0.5f, 0.5f }, out[1] = { 5.0f };
      if (t == 2 || t == 3) iris_delete_index(k, 3);
      if (t == 1 || t == 3) iris_record(k, in, out);
      const int more = iris_train_slice(k, 1);          /* epoch 4,000: a window test */
      const int after = iris_train_epochs_done(k);
      while (iris_train_slice(k, 500)) {}
      const int ok = at == 3999 && after == 4000 && more && iris_train_epochs_done(k) > 4000;
      right += ok;
      if (!ok && !first[0])
        snprintf(first, sizeof first, " -- %s: sliced to %d, the window test at %d %s the run, "
                 "which ended at %d", names[t], at, after, more ? "kept" : "ended",
                 iris_train_epochs_done(k));
    }
    snprintf(d, sizeof d, "%d of 4 runs went on past the window test after the edit%s", right, first);
    check("a sliced run restarts its window after any edit", right == 4, d);
  }

  /* ---- 12. a reseed ends a sliced run -----------------------------------
     A reroll button pressed while a sliced run is going. After iris_reseed
     the run must be over: iris_train_busy 0, and a slice returns 0 having
     changed no byte, so the instrument holds exactly the new seed's
     starting weights until a trainer runs. iris_train then gives the
     instrument a fresh one with that seed and those demonstrations gives. */
  {
    iris *k = lived_in(2, 12, 3, 32, 14, 21u);
    iris_train_begin(k, 0);
    iris_train_slice(k, 300);
    iris_reseed(k, 42u);
    const int busy = iris_train_busy(k);
    memcpy(SNAP, A, sizeof A);
    const int more = iris_train_slice(k, 500);
    const int still = same_arena();
    iris_train(k);
    memcpy(RA, A, sizeof A);
    memcpy(A, SNAP, sizeof A);           /* the same demonstrations, seed 42 */
    iris_train(k);
    const int fresh = memcmp(RA, A, sizeof A) == 0;
    snprintf(d, sizeof d, "busy after the reseed %d, a slice then returned %d and changed "
             "nothing %d; iris_train gives the seed-42 instrument %d", busy, more, still, fresh);
    check("iris_reseed ends a sliced run in flight", !busy && !more && still && fresh, d);
  }

  /* ---- 13. the divergence guard reaches every weight ----------------------
     A weight of +-1e30 planted in the last element of w1, b1, w2 and b2 in
     turn -- the loader accepts any finite weight, so a file can bring one --
     then one warm epoch. The guard walks the four arrays as one block, so a
     walk cut short misses the end of it: the planted value must come back
     as exactly +-IRIS_W_LIMIT, with IRIS_TRAINING_DIVERGED. */
  {
    int right = 0; char first[160] = "";
    const char *names[4] = { "w1", "b1", "w2", "b2" };
    for (int t = 0; t < 8; ++t) {
      iris *k = lived_in(2, 12, 3, 32, 14, 77u);
      float *arr[4] = { k->w1, k->b1, k->w2, k->b2 };
      const int len[4] = { 12 * 2, 12, 3 * 12, 3 };
      const float plant = t < 4 ? 1e30f : -1e30f;
      float *w = arr[t % 4] + len[t % 4] - 1;
      *w = plant;
      iris_continue(k, 1);
      const int ok = *w == (t < 4 ? IRIS_W_LIMIT : -IRIS_W_LIMIT)
                  && iris_get_status(k) == IRIS_TRAINING_DIVERGED;
      right += ok;
      if (!ok && !first[0])
        snprintf(first, sizeof first, " -- %s[last] = %g came back %g, status %d",
                 names[t % 4], (double)plant, (double)*w, (int)iris_get_status(k));
    }
    snprintf(d, sizeof d, "%d of 8 planted weights clamped to the limit and reported%s", right, first);
    check("the divergence guard reaches the last of every array", right == 8, d);
  }

  /* ---- 14. after an edit between slices the shuffle covers the store -----
     The shuffle permutes positions 0 to n_ex - 1, and a sliced run keeps its
     permutation from one slice to the next. An edit between slices must
     rebuild it: after a delete the old permutation still holds the position
     one past the end, so a deleted take would be visited and a live one
     skipped; after a record the new slot holds whatever an earlier run left
     there. Each edit below is followed by one slice, and the first n_ex
     entries of the order must then be every position exactly once. The
     record case starts from 21 takes trained once and the last one deleted,
     so the slot the record reuses holds a stale entry from that run. */
  {
    int right = 0; char first[200] = "";
    const char *names[3] = { "delete", "record", "delete then record" };
    for (int t = 0; t < 3; ++t) {
      iris *k = iris_init(A, sizeof A, 2, 12, 1, 64, 5u);
      for (int i = 0; i < 21; ++i) {
        const float u = (float)((i * 7919) % 97) / 97.0f, v = (float)((i * 6131) % 89) / 89.0f;
        float in[2] = { u, v }, out[1] = { u * v };
        iris_record(k, in, out);
      }
      iris_continue(k, 50);
      iris_delete_last(k);
      iris_train_begin(k, 0);
      iris_train_slice(k, 10);
      float in[2] = { 0.3f, 0.7f }, out[1] = { 0.2f };
      if (t == 0 || t == 2) iris_delete_index(k, 3);
      if (t == 1 || t == 2) iris_record(k, in, out);
      iris_train_slice(k, 1);
      int seen[64] = { 0 }, perm = 1;
      for (int i = 0; i < k->n_ex; ++i) {
        const int o = k->order[i];
        if (o < 0 || o >= k->n_ex || seen[o]++) perm = 0;
      }
      right += perm;
      if (!perm && !first[0])
        snprintf(first, sizeof first, " -- after a %s the order is not a permutation of 0..%d",
                 names[t], k->n_ex - 1);
    }
    snprintf(d, sizeof d, "%d of 3 edits left an order covering every take once%s", right, first);
    check("an edit between slices rebuilds the shuffle over the store", right == 3, d);
  }

  if (fails) printf("\n  %d FAILING\n", fails);
  else       printf("\n  all pass\n");
  return fails ? 1 : 0;
}
