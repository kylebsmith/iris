/* audit.c — does the thing actually work?
   Each check prints PASS or FAIL and a number you can argue with. The
   timing tables at the end are a report of this machine's speed and never
   decide a pass or a fail.

   Build and run from the repository root:
     sh build.sh audit
   or by hand (the threads are for check 36, four instruments at once):
     cc -std=c99 -O2 -Wall -Wextra -pthread -o build/audit tests/audit.c && ./build/audit
   sh build.sh audit also runs the guards-are-inert comparison
   (tests/guards_ab.c), which needs two builds of the core and so lives
   outside this binary. */

/* pthread.h and clock_gettime are POSIX, not C99; ask for them. */
#define _POSIX_C_SOURCE 200809L
#include "../iris.h"
/* This file computes its own demonstrations (truth() below), and the golden
   blob check pins an instrument trained on them. iris.h switches fused
   multiply-add contraction off for its own code only, so this file switches
   it off for its own arithmetic too, for the same reason: a*b+c fused by one
   compiler and not by another would be different demonstrations, and so a
   different instrument, before iris.h did anything at all. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif

/* <math.h> for NAN only: a not-a-number is written NAN, never 0.0f/0.0f, so
   the float-divide-by-zero sanitizer can run over this whole file. */
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;
static void ok(const char *name, int pass, const char *fmt, ...) {
  va_list a; va_start(a, fmt);
  printf("%s  %-42s  ", pass ? "PASS" : "FAIL", name);
  vprintf(fmt, a); printf("\n"); va_end(a);
  if (!pass) failures++;
}
/* Wall clock, for the timing report only. */
static double now_ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

/* FNV-1a, 32 bits (the Fowler-Noll-Vo hash): a short, well-known hash that
   tells two byte sequences apart, used for every pinned value in this file. */
static uint32_t fnv1a(const void *p, size_t n) {
  const unsigned char *b = (const unsigned char *)p;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
  return h;
}

/* Reduce a saved file (iris.h, PART 9) to the bytes that describe the
   INSTRUMENT, discarding the file plumbing: magic, version, header size,
   flags, the random-number word, the smoothing word and the checksum. What is
   left is the shape, n_ex, seed and next_id (offsets 16 to 39), then the
   weights, ranges, demonstrations and identifiers (offset 48 up to the
   checksum), all little-endian -- nothing a change of file format can move,
   because a hash that moves for bookkeeping reasons teaches you to re-pin it
   without asking why.
   The record opens with eight fixed bytes, 'E' 'W' 'E' 'K' 1 0 0 0, which
   describe nothing: they are part of the byte sequence the pinned hash below
   was taken over, so they stay. */
static size_t instrument_bytes(unsigned char *buf, size_t n) {
  static const unsigned char prefix[8] = { 'E', 'W', 'E', 'K', 1, 0, 0, 0 };
  if (n < 52) return 0;
  memmove(buf + 8, buf + 16, 24);                /* shape, n_ex, seed, next_id */
  memmove(buf + 32, buf + 48, n - 52);           /* weights .. identifiers */
  memcpy(buf, prefix, sizeof prefix);
  return 32 + (n - 52);
}

#define NI 2
#define NH 12
#define NO 3
#define CAP 256

/* The reroll checks (6 and 7) run a deliberately under-constrained model:
   8 hidden units, the fewest iris_init accepts, and 5 examples, the corner
   where different random starts disagree most in the gaps. The 12-hidden /
   20-example model the rest of the audit uses sits at the other end, where
   every random start converges to nearly the same answer. */
#define RR_HID  8
#define RR_EX   5
#define RR_EP   800
#define RR_SEEDS 8
#define RR_GRID 21                       /* 21 x 21 = 441 probes */

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

/* a chain of ten corrections: scattered points, each asking +0.15 on a
   rotating output -- a musician reshaping the instrument bit by bit */
static void chain_correction(int i, float *nin, float *nout) {
  float x = 0.11f + 0.37f * (float)(i + 1);  x -= (float)(int)x;
  float y = 0.05f + 0.73f * (float)(i + 1);  y -= (float)(int)y;
  nin[0] = x; nin[1] = y;
  truth(x, y, nout);
  nout[i % NO] = iris_internal_clampf(nout[i % NO] + 0.15f, 0.0f, 1.0f);
}

/* A made-up but musically-shaped target: three sound parameters that vary
   smoothly and differently across a 2-D gesture space. */
static void truth(float x, float y, float *o) {
  o[0] = 0.5f + 0.45f * iris_internal_tanh(3.0f * (x - 0.5f));
  o[1] = 0.5f + 0.40f * iris_internal_tanh(2.5f * (y - 0.5f) * (x + 0.3f));
  o[2] = 0.2f + 0.6f  * (x * y);
}

/* A scattered but repeatable example set: example i sits at
   ((7919 i mod 97) / 97, (6131 i mod 89) / 89), so the points fill the square
   without clumping and every run sees the same ones. */
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
  return iris_internal_sqrt(se / (float)c);
}
static float grid_rmse_of(iris *k) {
  float se = 0.0f; int c = 0;
  for (int a = 0; a < 21; ++a) for (int b = 0; b < 21; ++b) {
    float in[NI] = { a / 20.0f, b / 20.0f }, want[NO], got[NO];
    truth(in[0], in[1], want);
    iris_predict(k, in, got);
    for (int o = 0; o < NO; ++o) { float e = got[o] - want[o]; se += e * e; c++; }
  }
  return iris_internal_sqrt(se / (float)c);
}

/* six hostile example sets, for check 21 --------------------------------------- */
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
static void load_outlier(iris *k) {         /* the 1e6 outliers of check 9 */
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
      if (iris_internal_isbad(got[o]) || got[o] > 1e30f || got[o] < -1e30f) bad++;
  }
  return bad;
}

/* --- how much does rerolling move the sound, and WHERE? --------------------
   Train the same examples from RR_SEEDS different random starts, then over a
   grid of probes measure the largest disagreement between any two rerolls.
   Report it separately for probes that sit on demonstrated ground and probes
   out in the gaps.

   "Near demos" means novelty below 0.15 and "in the gaps" novelty above
   0.35 (iris_novelty, PART 7); probes in between count as neither.        */
#define RR_NEAR_BAND 0.15f
#define RR_FAR_BAND  0.35f

static float rr_pred[RR_SEEDS][RR_GRID * RR_GRID][NO];

static void reroll_spread(iris *k, float *near_spread, int *near_n,
                          float *gap_spread,  int *gap_n) {
  for (int s = 0; s < RR_SEEDS; ++s) {
    load_examples(k, RR_EX);
    iris_reseed(k, 1000u + (uint32_t)s * 7919u); iris_continue(k, RR_EP);
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
        float d = iris_internal_absf(rr_pred[i][p][o] - rr_pred[j][p][o]);
        if (d > worst) worst = d;
      }
    if      (nov < RR_NEAR_BAND) { sn += worst; cn++; }
    else if (nov > RR_FAR_BAND)  { sf += worst; cf++; }
  }
  *near_n = cn; *gap_n = cf;
  *near_spread = cn ? sn / (float)cn : -1.0f;
  *gap_spread  = cf ? sf / (float)cf : -1.0f;
}

/* --- the arena, measured from the pointers iris_init leaves behind --------
   IRIS_ARENA is the size a caller declares; iris_init carves the structure
   and twenty arrays out of whatever it is handed. carve_check does not ask
   the library how big anything is. It takes each pointer iris_init stored,
   the number of elements that array must hold for this shape (the counts
   the structure's own comments give), and checks that every array lies
   wholly inside [mem, mem + bytes), is aligned for its type, and overlaps
   no other. The furthest byte any array reaches is what iris_init really
   needed, which *reach returns. */
typedef struct { const unsigned char *lo; size_t bytes, align; } carve;
static int carve_check(unsigned char *mem, size_t bytes, int ni, int nh,
                       int no, int cap, size_t *reach) {
  iris *k = iris_init(mem, bytes, ni, nh, no, cap, 1u);
  if (!k) return 0;
  const size_t F = sizeof(float), I = sizeof(int32_t);
  const size_t hi_ = (size_t)nh * (size_t)ni, ho_ = (size_t)no * (size_t)nh;
  const size_t n_in = (size_t)ni, n_hid = (size_t)nh, n_out = (size_t)no, c = (size_t)cap;
  const carve a[] = {
    { (const unsigned char *)k,         sizeof(iris), sizeof(void *) },
    { (const unsigned char *)k->w1,     F * hi_,   F }, { (const unsigned char *)k->b1,    F * n_hid, F },
    { (const unsigned char *)k->w2,     F * ho_,   F }, { (const unsigned char *)k->b2,    F * n_out, F },
    { (const unsigned char *)k->v_w1,   F * hi_,   F }, { (const unsigned char *)k->v_b1,  F * n_hid, F },
    { (const unsigned char *)k->v_w2,   F * ho_,   F }, { (const unsigned char *)k->v_b2,  F * n_out, F },
    { (const unsigned char *)k->hid,    F * n_hid, F }, { (const unsigned char *)k->out,   F * n_out, F },
    { (const unsigned char *)k->d_hid,  F * n_hid, F }, { (const unsigned char *)k->d_out, F * n_out, F },
    { (const unsigned char *)k->in_lo,  F * n_in,  F }, { (const unsigned char *)k->in_hi, F * n_in,  F },
    { (const unsigned char *)k->out_lo, F * n_out, F }, { (const unsigned char *)k->out_hi,F * n_out, F },
    { (const unsigned char *)k->ex_res, F * c,     F },
    { (const unsigned char *)k->ex,     F * c * (n_in + n_out), F },
    { (const unsigned char *)k->ex_id,  I * c,     I }, { (const unsigned char *)k->order, I * c,     I },
  };
  const size_t n = sizeof a / sizeof a[0];
  size_t far = 0;
  for (size_t i = 0; i < n; ++i) {
    if (a[i].lo < mem || a[i].lo + a[i].bytes > mem + bytes) return 0;
    if ((uintptr_t)a[i].lo % a[i].align) return 0;
    if ((size_t)(a[i].lo + a[i].bytes - mem) > far) far = (size_t)(a[i].lo + a[i].bytes - mem);
    for (size_t j = 0; j < i; ++j)
      if (a[i].lo < a[j].lo + a[j].bytes && a[j].lo < a[i].lo + a[i].bytes) return 0;
  }
  *reach = far;
  return 1;
}

/* --- four instruments at once (check 36) ----------------------------------
   One job: build an instrument in its own arena, record, train to the
   plateau, play XT_PREDS predictions into its own stream, save. A job
   touches nothing outside its own xt_job. The steps are separate functions
   so check 36 can also run four jobs interleaved step by step on one
   thread. */
#define XT_N 4
#define XT_PREDS 2000
typedef struct {
  unsigned char arena[IRIS_ARENA(NI, NH, NO, CAP)];
  unsigned char file[16 * 1024];
  float stream[XT_PREDS][NO];
  size_t file_n;
  uint32_t seed;
  int nex;
  iris *k;
} xt_job;
static xt_job xt_seq[XT_N], xt_rr[XT_N], xt_par[XT_N];

static void xt_start(xt_job *j) {
  j->k = iris_init(j->arena, sizeof j->arena, NI, NH, NO, CAP, j->seed);
  j->file_n = 0;
}
static void xt_record(xt_job *j, int e) {
  float u = (float)((e * 7919) % 97) / 97.0f, v = (float)((e * 6131) % 89) / 89.0f;
  float in[NI] = { u, v }, o[NO];
  truth(u, v, o);
  if (j->k && e < j->nex) iris_record(j->k, in, o);
}
static void xt_play(xt_job *j, int p) {
  float in[NI] = { (float)(p % 97) / 97.0f, (float)(p % 89) / 89.0f };
  if (j->k) iris_predict(j->k, in, j->stream[p]);
}
static void xt_finish(xt_job *j) {
  if (j->k) j->file_n = iris_save(j->k, j->file, sizeof j->file);
}
static void xt_work(xt_job *j) {
  xt_start(j);
  for (int e = 0; e < j->nex; ++e) xt_record(j, e);
  if (j->k) iris_train(j->k);
  for (int p = 0; p < XT_PREDS; ++p) xt_play(j, p);
  xt_finish(j);
}
static int xt_same(const xt_job *a, const xt_job *b) {
  return memcmp(a->stream, b->stream, sizeof a->stream) == 0 && a->file_n > 0
      && a->file_n == b->file_n && memcmp(a->file, b->file, a->file_n) == 0;
}

/* All four threads wait here until the last one arrives, so their work
   overlaps instead of running one after another. (macOS has no
   pthread_barrier_t, so it is a mutex and a condition variable.) */
static pthread_mutex_t xt_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  xt_cv = PTHREAD_COND_INITIALIZER;
static int xt_waiting;
static void *xt_thread(void *arg) {
  pthread_mutex_lock(&xt_mu);
  if (++xt_waiting == XT_N) pthread_cond_broadcast(&xt_cv);
  else while (xt_waiting < XT_N) pthread_cond_wait(&xt_cv, &xt_mu);
  pthread_mutex_unlock(&xt_mu);
  xt_work((xt_job *)arg);
  return 0;
}

int main(void) {
  printf("\niris v%d.%d.%d — audit\n", IRIS_VERSION_MAJOR, IRIS_VERSION_MINOR, IRIS_VERSION_PATCH);
  printf("--------------------------------------------------------------------------\n");

  /* --- 1. memory: IRIS_ARENA covers what iris_init actually carves --------
     For several shapes, from the smallest legal one to the largest, and at
     every starting offset 0 to 7 (a caller's unsigned char array has no
     alignment promise), an arena of exactly IRIS_ARENA bytes is allocated on
     the heap -- so under AddressSanitizer one byte past it is a report --
     and carve_check measures where iris_init put everything.              */
  {
    static const int shapes[5][4] = {
      { NI, NH, NO, CAP }, { 1, 8, 1, 1 }, { 3, 17, 2, 5 }, { 7, 33, 5, 999 },
      { IRIS_MAX_IN, IRIS_MAX_HID, IRIS_MAX_OUT, IRIS_MAX_EX } };
    int cases = 0, good = 0;
    size_t least_slack = (size_t)-1, worst_need = 0, worst_bytes = 0;
    for (int s5 = 0; s5 < 5; ++s5) {
      const int *sh = shapes[s5];
      const size_t bytes = IRIS_ARENA(sh[0], sh[1], sh[2], sh[3]);
      for (size_t off = 0; off < 8; ++off) {
        unsigned char *base = (unsigned char *)malloc(bytes + off);
        size_t reach = 0;
        cases++;
        if (base && carve_check(base + off, bytes, sh[0], sh[1], sh[2], sh[3], &reach)) {
          good++;
          if (bytes - reach < least_slack) {
            least_slack = bytes - reach; worst_need = reach; worst_bytes = bytes;
          }
        }
        free(base);
      }
    }
    ok("arena macro covers every array iris_init carves", good == cases,
       "%d of %d shape/offset cases inside, aligned, disjoint; tightest: "
       "reaches %zu of %zu B (slack %zu B)", good, cases, worst_need,
       worst_bytes, least_slack);
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
    float err = iris_continue(k, 800);

    /* recall: how close does it get to the sounds we actually demonstrated? */
    float worst = 0.0f, sum = 0.0f;
    for (int i = 0; i < iris_count(k); ++i) {
      float in[NI], want[NO], got[NO];
      iris_get(k, i, in, want);
      iris_predict(k, in, got);
      for (int o = 0; o < NO; ++o) {
        float e = iris_internal_absf(got[o] - want[o]);
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
        float e = iris_internal_absf(got[o] - want[o]);
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
    iris_reseed(k2, 1234); iris_continue(k2, 800);
    iris_reseed(k,  1234); iris_continue(k,  800);
    int identical = 1; float biggest = 0.0f;
    for (int a = 0; a <= 20; ++a) for (int b = 0; b <= 20; ++b) {
      float in[NI] = { a / 20.0f, b / 20.0f }, o1[NO], o2[NO];
      iris_predict(k, in, o1); iris_predict(k2, in, o2);
      for (int o = 0; o < NO; ++o) {
        float d = iris_internal_absf(o1[o] - o2[o]);
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
     steady everywhere. Measured at RR_HID hidden / RR_EX examples, the
     under-constrained corner described at RR_HID.                          */
  {
    iris *kr = iris_init(arena_r, sizeof arena_r, NI, RR_HID, NO, RR_EX, 1);
    float near_s = -1.0f, gap_s = -1.0f; int near_n = 0, gap_n = 0;
    if (kr) reroll_spread(kr, &near_s, &near_n, &gap_s, &gap_n);

    /* Measured 0.0399 with these seeds; the threshold 0.03 leaves 1.3x
       headroom. The bound belongs to this seed family: another family of
       eight seeds can land under it, so a change of seeds is a change of
       this bound, not a regression. */
    ok("reroll is lively in the gaps", kr && gap_n > 0 && gap_s > 0.03f,
       "%d hidden, %d examples, %d seeds: %d gap probes move by %.4f  (want > 0.0300)",
       RR_HID, RR_EX, RR_SEEDS, gap_n, gap_s);

    /* near demos: measured 0.0088 at this width. Threshold 0.04 leaves
       4.5x headroom. */
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
    iris_continue(k, 400);
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
    iris_continue(k, 400);
    float at[NI] = { 0.4f, 0.4f }, far[NI] = { 0.95f, 0.05f };
    float n_at = iris_novelty(k, at), n_far = iris_novelty(k, far);
    ok("novelty: low on home ground, high away", n_at < 0.05f && n_far > 0.5f,
       "on an example %.3f, far away %.3f", n_at, n_far);
  }

  /* --- 12. the golden blob: the TRAINING path, pinned to the bit ----------
     Run the check-5 recipe and hash the saved bytes. Any compiler-flag drift,
     contraction leak, or accidental math change in train/save fails this
     check loudly. The hash is taken over the instrument's bytes only (see
     instrument_bytes), so a change of file format cannot move it, and fnv1a
     is taken over exactly those bytes, so a wrong length gives a wrong hash:
     the hash is the check.

     0x6805FB0D is the [-1,+1] input scaling on the contraction-off bit class
     (docs/adr/0003). */
  {
    static unsigned char file[64 * 1024];
    /* fresh instrument: the blob carries example ids, so the recipe must
       start from iris_init */
    iris *kb = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 1234);
    for (int i = 0; i < 20; ++i) {
      float u = (float)((i * 7919) % 97) / 97.0f;
      float v = (float)((i * 6131) % 89) / 89.0f;
      float in[NI] = { u, v }, out[NO];
      truth(u, v, out);
      iris_record(kb, in, out);
    }
    iris_reseed(kb, 1234); iris_continue(kb, 800);
    size_t n = iris_save(kb, file, sizeof file);
    size_t n1 = instrument_bytes(file, n);
    uint32_t h = fnv1a(file, n1);
    const uint32_t want = 0x6805FB0Du;
    ok("golden blob: [-1,+1] training path bit-pinned", n1 > 0 && h == want,
       "%zu instrument bytes, fnv1a 0x%08X (want 0x%08X)", n1, h, want);
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
    iris_continue(k, 400);
    float probe[NI] = { 0.3f, 0.7f }, before[NO], after[NO];
    iris_predict(k, probe, before);
    /* THE DOOR REFUSES IT. iris_record rejects a not-a-number before it can
       enter the store: 0 back, status IRIS_NAN_TRAPPED, n_ex unmoved. */
    int door_refused, n_before = iris_count(k);
    { float in[NI] = { NAN, 0.5f }, out[NO] = { 0.5f, 0.5f, 0.5f };
      door_refused = (iris_record(k, in, out) == 0) && iris_count(k) == n_before
                   && iris_get_status(k) == IRIS_NAN_TRAPPED; }

    /* AND THE TRAINER'S BACKSTOP STILL WORKS. The door cannot be the only
       guard: examples also arrive through iris_load, which does not go through
       iris_record. Write the poison straight into the store to exercise the
       pre-scan the way a corrupt file would. */
    k->ex[0] = NAN;
    k->trained = 1;                            /* pretend the fit is current */
    iris_continue(k, 400);
    int refused = door_refused && (iris_get_status(k) == IRIS_NAN_TRAPPED);
    iris_predict(k, probe, after);
    int preserved = (before[0] == after[0] && before[1] == after[1]
                  && before[2] == after[2]);

    /* (b) NaN sensor input at play time: finite outputs + status */
    iris_delete_last(k);
    iris_continue(k, 400);
    float nan_in[NI] = { NAN, 0.4f }, got[NO];
    iris_predict(k, nan_in, got);
    int finite = 1;
    for (int o = 0; o < NO; ++o) if (iris_internal_isbad(got[o])) finite = 0;
    int reported = (iris_get_status(k) == IRIS_NAN_TRAPPED);

    /* (c) hostile lr/momentum sweep on check 9's data + a 1e6 INPUT outlier */
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
      iris_internal_set_learning(k, lrs[li], moms[mi]);
      int detect_call = 0;
      for (int call = 1; call <= 4; ++call) {
        iris_continue(k, 400);
        if (!detect_call && iris_get_status(k) != IRIS_STATUS_OK) detect_call = call;
      }
      for (int a = -5; a <= 25; ++a) for (int b = -5; b <= 25; ++b) {
        float in[NI] = { a / 20.0f, b / 20.0f }, o2[NO];
        iris_predict(k, in, o2);
        for (int o = 0; o < NO; ++o) if (iris_internal_isbad(o2[o])) sweep_nan++;
      }
      /* at the wild settings, if anything went wrong it must have been
         reported on the train call where it happened — never later */
      if (detect_call > 1) sweep_unreported++;
    }
    iris_internal_set_learning(k, 0.10f, 0.85f);

    ok("NaN never reaches output; guards report", refused && preserved
       && finite && reported && sweep_nan == 0 && sweep_unreported == 0,
       "poison refused %d, weights kept %d, sensor-NaN finite %d + status %d, "
       "sweep NaN %d, late reports %d",
       refused, preserved, finite, reported, sweep_nan, sweep_unreported);
  }

  /* --- 14. event-sourced determinism ---------------------------------------
     A warm run advances the rng past the seed, so the determinism promise
     for warm training is: the identical OPERATION HISTORY reproduces the
     instrument bit-exactly. Two instruments, same history of
     record/continue/delete, compared by memcmp over weights+velocity and
     the rng word, at every chain length 1..10 and after a delete.          */
  {
    iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 42);
    iris *c = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 42);
    load_examples(a, 20); iris_reseed(a, 42); iris_continue(a, 600);
    load_examples(c, 20); iris_reseed(c, 42); iris_continue(c, 600);
    int all_same = 1;
    for (int i = 0; i < 10; ++i) {
      float in[NI], out[NO];
      chain_correction(i, in, out);
      iris_record(a, in, out); iris_continue(a, 20);
      iris_record(c, in, out); iris_continue(c, 20);
      if (!state_identical(a, c)) all_same = 0;
    }
    iris_delete_last(a); iris_continue(a, 20);
    iris_delete_last(c); iris_continue(c, 20);
    int after_delete = state_identical(a, c);
    ok("event-sourced determinism (history replay)", all_same && after_delete,
       "10-step warm chain bit-identical %d, delete+continue bit-identical %d",
       all_same, after_delete);
  }

  /* --- 15. save and load carry the random state --------------------------
     The file stores the live rng word. The test that matters is not "the
     bytes come back" but "the FUTURE comes back": a warm run after
     save->load must be bit-identical to the warm run the in-memory
     instrument would have made from the same state. A load leaves the
     momentum velocities at zero (iris.h, PART 9), so the in-memory
     instrument is put at rest the same way before the two runs.            */
  {
    static unsigned char file[64 * 1024];
    iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 42);
    load_examples(a, 20); iris_reseed(a, 42); iris_continue(a, 600);
    for (int i = 0; i < 3; ++i) {
      float in[NI], out[NO];
      chain_correction(i, in, out);
      iris_record(a, in, out); iris_continue(a, 20);
    }
    size_t n2 = iris_save(a, file, sizeof file);
    iris *b = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 7);
    int loaded = iris_load(b, file, n2);
    int exact = loaded && a->rng.s == b->rng.s && a->seed == b->seed
             && iris_count(a) == iris_count(b)
             && memcmp(a->w1, b->w1,
                       sizeof(float) * (size_t)(NH*NI + NH + NO*NH + NO)) == 0;
    /* the future: at rest, one more warm run on both must match to the bit.
       v_w1, v_b1, v_w2 and v_b2 are one contiguous span in the arena. */
    memset(a->v_w1, 0, sizeof(float) * (size_t)(NH*NI + NH + NO*NH + NO));
    float in[NI], out[NO];
    chain_correction(3, in, out);
    iris_record(a, in, out); iris_continue(a, 20);
    iris_record(b, in, out); iris_continue(b, 20);
    int future = state_identical(a, b);
    ok("save round trip carries the random state", exact && future,
       "%zu B, state exact %d, post-reload warm run bit-identical %d",
       n2, exact, future);
  }

  /* --- 16. the correction reaches parity without wrecking the map ---------
     One corrective example on a practised 20-example instrument, then ONE
     warm run of 20 epochs (iris_continue). Measured here: it reaches train
     rms 0.0187 with far-field drift 0.0025; a cold 600-epoch retrain from
     the same seed reaches 0.0181 with 0.0033, on 30 times the epochs. The
     gates carry the warm run's numbers with a margin.                      */
  {
    static float snapA[21 * 21][NO], snapB[21 * 21][NO];
    iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 42);
    load_examples(a, 20); iris_reseed(a, 42); iris_continue(a, 600);
    int p = 0;
    for (int x = 0; x < 21; ++x) for (int y = 0; y < 21; ++y, ++p) {
      float in[NI] = { x / 20.0f, y / 20.0f };
      iris_predict(a, in, snapA[p]);
    }
    float cin[NI] = { 0.62f, 0.31f }, cout[NO];
    truth(cin[0], cin[1], cout);
    cout[0] = iris_internal_clampf(cout[0] + 0.15f, 0.0f, 1.0f);
    iris_record(a, cin, cout);
    iris_continue(a, 20);
    /* fit across all 21 examples */
    float acc = 0.0f;
    for (int i = 0; i < iris_count(a); ++i) {
      float in[NI], want[NO], got[NO];
      iris_get(a, i, in, want);
      iris_predict(a, in, got);
      for (int o = 0; o < NO; ++o) { float d = got[o] - want[o]; acc += d * d; }
    }
    float tr = iris_internal_sqrt(acc / (float)(iris_count(a) * NO));
    /* drift far from the correction: mean |delta| over probes > 0.25 away */
    p = 0;
    float sf = 0.0f; int cf = 0;
    for (int x = 0; x < 21; ++x) for (int y = 0; y < 21; ++y, ++p) {
      float in[NI] = { x / 20.0f, y / 20.0f };
      iris_predict(a, in, snapB[p]);
      float dx = in[0] - cin[0], dy = in[1] - cin[1];
      if (iris_internal_sqrt(dx * dx + dy * dy) < 0.25f) continue;
      float m = 0.0f;
      for (int o = 0; o < NO; ++o) m += iris_internal_absf(snapA[p][o] - snapB[p][o]);
      sf += m / NO; cf++;
    }
    float drift = cf ? sf / (float)cf : -1.0f;
    ok("correction: parity fit, surgical drift", tr <= 0.0195f && drift >= 0.0f && drift <= 0.0045f,
       "train rms %.4f (want <= 0.0195; cold-600 ref 0.0181), "
       "far-field drift %.4f (want <= 0.0045; cold-600 ref 0.0033), 20 epochs",
       tr, drift);
  }

  /* --- 20. ELM: same seed, same bits; reroll keeps both promises ----------
     ELM is the extreme learning machine, the closed-form trainer of PART 8d:
     it freezes the random first layer and solves the output layer in one
     step. Its determinism is structural (no random draw after the frozen
     layer, a fixed order of accumulation), observed here at three widths.
     Its reroll character is measured at the nh=12 / 5-example configuration
     the bands were set on: near <= 0.02, gap >= 0.06 (this program measures
     0.0106 / 0.1123 -- steadier at the demonstrations AND livelier in the
     gaps than backpropagation's 0.0088 / 0.0399 in checks 6 and 7).       */
  {
    const int nhs[3] = { 12, 24, 48 };
    int all_same = 1;
    for (int hi = 0; hi < 3; ++hi) {
      int nh = nhs[hi];
      iris *a = iris_init(arena_w1, sizeof arena_w1, NI, nh, NO, CAP, 7);
      iris *b = iris_init(arena_w2, sizeof arena_w2, NI, nh, NO, CAP, 99);
      load_examples(a, 50); load_examples(b, 50);
      iris_reseed(a, 4242u); iris_reseed(b, 4242u);
      int ra = iris_train_elm(a, 1e-4f, elm_scratch, sizeof elm_scratch);
      int rb = iris_train_elm(b, 1e-4f, elm_scratch, sizeof elm_scratch);
      if (ra < 0 || rb < 0) all_same = 0;
      if (memcmp(a->w1, b->w1,
                 sizeof(float) * (size_t)(nh*NI + nh + NO*nh + NO)) != 0) all_same = 0;
    }
    /* reroll bands, ELM flavour, at the lively config */
    iris *kr = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 1);
    for (int s = 0; s < RR_SEEDS; ++s) {
      load_examples(kr, RR_EX);
      iris_reseed(kr, 1000u + (uint32_t)s * 7919u);
      iris_train_elm(kr, 1e-4f, elm_scratch, sizeof elm_scratch);
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
            float d = iris_internal_absf(rr_pred[i][p][o] - rr_pred[j][p][o]);
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
     the normal matrix symmetric positive definite by construction, so a
     Cholesky factorisation always exists and the gate is absolute: zero
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
     At 48 hidden units and lam0 = 1e-3 the closed-form solve beats 600
     epochs of backpropagation on recall AND on the 441-probe grid at 50
     examples (this program measures recall 0.0030 against 0.0059, grid
     0.0090 against 0.0103).                                                */
  {
    iris *a = iris_init(arena_w1, sizeof arena_w1, NI, 48, NO, CAP, 42);
    load_examples(a, 50);
    iris_reseed(a, 42u); iris_continue(a, 600);
    float bp_rec = recall_rmse_of(a), bp_grid = grid_rmse_of(a);
    load_examples(a, 50);
    iris_reseed(a, 42u);
    int esc = iris_train_elm(a, 1e-3f, elm_scratch, sizeof elm_scratch);
    float el_rec = recall_rmse_of(a), el_grid = grid_rmse_of(a);
    ok("ELM: nh=48 fit floor beats backprop", esc >= 0
       && el_rec <= bp_rec && el_grid <= bp_grid,
       "recall %.4f vs backprop %.4f, grid %.4f vs %.4f (want both <=)",
       el_rec, bp_rec, el_grid, bp_grid);
  }

  /* --- 23. k-NN: exact recall at every demonstration ----------------------
     k-NN is the k-nearest-neighbour blend and 1-NN the single nearest
     neighbour (PART 10). The property backprop never delivers: standing on a
     demo returns the demo. Gate at float precision for the blended k=1/k=3 read, and
     bit-for-bit for the 1-NN snap (it returns the stored row verbatim).   */
  {
    const int counts[4] = { 5, 20, 50, 200 };
    float worst = 0.0f; int nn_exact = 1;
    iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 1);
    for (int c = 0; c < 4; ++c) {
      load_examples(a, counts[c]);
      iris_internal_fit_ranges(a);
      for (int n = 0; n < iris_count(a); ++n) {
        float in[NI], want[NO], got[NO];
        iris_get(a, n, in, want);
        iris_knn_predict(a, in, got, 1);
        for (int o = 0; o < NO; ++o) {
          float e = iris_internal_absf(got[o] - want[o]);
          if (e > worst) worst = e;
        }
        iris_knn_predict(a, in, got, 3);
        for (int o = 0; o < NO; ++o) {
          float e = iris_internal_absf(got[o] - want[o]);
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
    iris_internal_fit_ranges(a);
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
        if (iris_internal_isbad(g3[o]) || iris_internal_isbad(g1[o])) bad++;
        if (g3[o] < lo[o] || g3[o] > hi[o]) escaped++;
        if (g1[o] < lo[o] || g1[o] > hi[o]) escaped++;
      }
    }
    /* conflicting duplicates: same input, outputs 0 and 1 — finite average */
    iris_clear(a);
    float din[NI] = { 0.3f, 0.6f };
    float o0[NO] = { 0.0f, 0.2f, 0.2f }, o1[NO] = { 1.0f, 0.8f, 0.8f };
    iris_record(a, din, o0); iris_record(a, din, o1);
    iris_internal_fit_ranges(a);
    float avg[NO];
    iris_knn_predict(a, din, avg, 3);
    int avg_ok = !iris_internal_isbad(avg[0]) && avg[0] >= 0.0f && avg[0] <= 1.0f;
    /* seed independence: two instruments, different seeds, same examples */
    iris *b2 = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 987654);
    load_examples(a, 20); iris_internal_fit_ranges(a);
    load_examples(b2, 20); iris_internal_fit_ranges(b2);
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
    iris_internal_rng rr = { 20260821u };
    for (int cls = 0; cls < 3; ++cls) {
      for (int j = 0; j < 4; ++j) {
        float dx = iris_internal_rand_sym(&rr) * 0.12f, dy = iris_internal_rand_sym(&rr) * 0.12f;
        float in[NI] = { (float)cx[cls] + dx, (float)cy[cls] + dy };
        float out[NO] = { (float)cls, 0.0f, 0.0f };
        iris_record(a, in, out);
        ref_in[rn][0] = in[0]; ref_in[rn][1] = in[1];
        ref_cls[rn] = cls; rn++;
      }
    }
    iris_internal_fit_ranges(a);
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
    iris_internal_fit_ranges(t);
    float probe[NI] = { 0.5f, 0.5f }, pout[NO];
    int winner = iris_classify_1nn(t, probe, pout);
    ok("1-NN: agrees with the Weka-IBk reference", disagree == 0
       && agree > 0 && winner == id_first,
       "441 probes: %d agree, %d disagree, %d distance ties; "
       "exact tie -> earliest-recorded wins %d",
       agree, disagree, ties, winner == id_first);
  }

  /* --- 26. hostile queries and poisoned stores cannot corrupt the samplers -
     A not-a-number query matches no neighbour at all, and a not-a-number
     stored in an example's OUTPUTS would be copied straight into the answer
     by both samplers. Each must refuse visibly instead: the range-centre
     substitute, and IRIS_NAN_TRAPPED in the status.                        */
  {
    iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 7);
    iris_clear(a);
    for (int i = 0; i < 8; ++i) {
      float u = 0.1f + 0.1f * (float)i;
      float in[NI] = { u, 1.0f - u }, out[NO] = { u, 0.5f, 1.0f - u };
      iris_record(a, in, out);
    }
    iris_internal_fit_ranges(a);
    float nanq[NI] = { NAN, 0.5f }, out[NO];
    int bad = 0;
    iris_knn_predict(a, nanq, out, 3);
    for (int o = 0; o < NO; ++o) if (iris_internal_isbad(out[o])) bad++;
    int st_nanq = (a->status == 2);            /* IRIS_NAN_TRAPPED */
    float farq[NI] = { 1e38f, 1e38f };
    iris_knn_predict(a, farq, out, 3);
    for (int o = 0; o < NO; ++o) if (iris_internal_isbad(out[o])) bad++;
    /* poison one example's OUTPUT, then query sanely through both paths */
    a->status = 0;
    a->ex[2 * (NI + NO) + NI + 1] = NAN;
    float sane[NI] = { 0.3f, 0.7f };
    iris_knn_predict(a, sane, out, 8);           /* k=8: poisoned row included */
    for (int o = 0; o < NO; ++o) if (iris_internal_isbad(out[o])) bad++;
    int st_pois = (a->status == 2);
    float nearpois[NI] = { 0.3f, 0.7f };       /* 1nn onto the poisoned row */
    a->status = 0;
    iris_classify_1nn(a, nearpois, out);
    for (int o = 0; o < NO; ++o) if (iris_internal_isbad(out[o])) bad++;
    ok("samplers survive hostile queries and poisoned outputs",
       bad == 0 && st_nanq && st_pois,
       "NaN query, 1e38 query, NaN-output row via k-NN and 1-NN: "
       "%d bad values escaped, statuses reported %d/%d", bad, st_nanq, st_pois);
  }

  /* --- 27. the loader cannot be lied to about sizes ------------------------
     Two lies a file can tell about its own size, each told alone so that only
     the rule it breaks can refuse it: (a) more demonstrations than the
     receiving instrument has room for, in a file that is otherwise perfect --
     written by iris_save from an instrument one demonstration bigger; (b) a
     count one higher than the body holds, with the checksum recomputed so the
     checksum cannot be what refuses it. (c) is a file cut in half. Each must
     be refused, and every byte of the receiving arena must be the same
     afterwards.                                                             */
  {
    static unsigned char arena_big[IRIS_ARENA(NI, NH, NO, CAP + 1)];
    iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 7);
    iris *big = iris_init(arena_big, sizeof arena_big, NI, NH, NO, CAP + 1, 7);
    iris_clear(a);
    for (int i = 0; i < 6; ++i) {
      float u = 0.15f * (float)i + 0.05f;
      float in[NI] = { u, 1.0f - u }, out[NO] = { u, u, u };
      iris_record(a, in, out);
    }
    iris_reseed(a, 4321); iris_continue(a, 100);
    load_examples(big, CAP + 1);
    static unsigned char blob[8 * 1024], evil[64 * 1024];
    static unsigned char arena_before[sizeof arena_b];
    size_t n = iris_save(a, blob, sizeof blob);
    memcpy(arena_before, arena_b, sizeof arena_b);
    int refuse = 0, total = 0;
    /* (a) CAP + 1 demonstrations, every other rule obeyed */
    size_t nb = iris_save(big, evil, sizeof evil);
    total++; if (nb > 0 && !iris_load(a, evil, nb)) refuse++;
    /* (b) n_ex, the little-endian word at offset 28 (iris.h, PART 9), one
       higher than the body holds; checksum recomputed */
    memcpy(evil, blob, n); evil[28] = (unsigned char)(evil[28] + 1);
    { uint32_t c = iris_internal_crc32(evil, n - 4);
      for (int i = 0; i < 4; ++i) evil[n - 4 + i] = (unsigned char)(c >> (8 * i)); }
    total++; if (!iris_load(a, evil, n)) refuse++;
    /* (c) body physically cut short */
    memcpy(evil, blob, n);
    total++; if (!iris_load(a, evil, n / 2)) refuse++;
    int intact = memcmp(arena_before, arena_b, sizeof arena_b) == 0 && iris_count(a) == 6;
    ok("loader refuses a count over capacity or wrong for its body",
       nb > 0 && refuse == total && intact,
       "%d/%d lies refused; receiving arena bit-identical after them: %s",
       refuse, total, intact ? "yes" : "NO");
  }

  /* --- 28. a refused train changes nothing but the status -----------------
     Every trainer checks the demonstrations before it reseeds, fits ranges or
     touches a counter, so a refusal over a poisoned demonstration leaves every
     byte of the instrument as it was except the status, which reports the
     not-a-number it found (iris.h, PART 8). iris_record refuses a
     not-a-number at the door, so the poison is written into the store
     directly, the way a program writing into the store could.             */
  {
    iris *a = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 7);
    iris_clear(a);
    for (int i = 0; i < 6; ++i) {
      float u = 0.15f * (float)i + 0.05f;
      float in[NI] = { u, 1.0f - u }, out[NO] = { 0.2f + 0.1f * (float)i, 0.5f, 0.8f };
      iris_record(a, in, out);
    }
    iris_reseed(a, 99); iris_continue(a, 200);
    float pin[NI] = { 0.9f, 0.9f }, pout[NO] = { 0.5f, 5.0f, 0.5f };
    iris_record(a, pin, pout);                    /* clean row, accepted */
    a->ex[(size_t)(a->n_ex - 1) * (NI + NO) + NI] = NAN;  /* now poison it */
    static unsigned char arena_before[sizeof arena_b];
    memcpy(arena_before, arena_b, sizeof arena_b);
    const int32_t st0 = a->status;
    int refused = 0, untouched = 0;
    for (int t = 0; t < 4; ++t) {
      int r = 0;
      switch (t) {
        case 0: r = iris_continue(a, 100) == -1.0f; break;
        case 1: r = iris_continue_to_plateau(a, 0, 0, 0) == -1.0f; break;
        case 2: r = iris_train(a) == 0; break;
        default: r = iris_train_begin(a, 0) == 0; break;
      }
      if (r && a->status == IRIS_NAN_TRAPPED) refused++;
      a->status = st0;                            /* every byte but the status */
      if (memcmp(arena_before, arena_b, sizeof arena_b) == 0) untouched++;
    }
    iris_delete_last(a);                          /* musician removes the poison */
    ok("a refused train changes nothing but the status",
       refused == 4 && untouched == 4,
       "continue, continue_to_plateau, train, train_begin: %d/4 refused with "
       "IRIS_NAN_TRAPPED, %d/4 left every other byte unmoved", refused, untouched);
  }

  /* --- 31. training to convergence, and a progress bar that is not a lie --
     Three claims. (a) iris_train, which stops at the plateau, fits the
     reference task far better than a fixed 600-epoch run. (b) The same run
     sliced into chunks -- which is how a single-threaded UI keeps drawing --
     is BIT-IDENTICAL to iris_train taken in one blocking call: both check and
     reseed the same way, and the shuffle buffer is carried across the
     slices. (c) The reported progress never goes backwards and ends at
     exactly 1.0, which is the whole difference between a progress bar and
     an animation.                                                          */
  {
    load_examples(k, 20);
    iris_reseed(k, 4242u); iris_continue(k, 600);
    float e600 = iris_last_error(k), r600 = recall_rmse_of(k);

    iris *ka = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 4242u);
    load_examples(ka, 20);
    iris_train(ka);
    float econv = iris_last_error(ka);
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
        if (i == bad) out[i % NO] = iris_internal_clampf(out[i % NO] + 0.20f, 0.0f, 1.0f);
        iris_record(kd, in, out);
      }
      iris_train(kd);
      float m = 0.0f;
      if (iris_worst_example(kd, &m) == bad) hits++;
      if (m > worst_margin) worst_margin = m;

      /* the same seed, the same twenty points, nothing corrupted */
      iris *kc = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 1000u + (uint32_t)t * 7919u);
      load_examples(kc, 20);
      iris_train(kc);
      float mc = 0.0f;
      (void)iris_worst_example(kc, &mc);
      if (mc > loudest_clean) loudest_clean = mc;
      if (mc >= IRIS_STRESS_FLAG) quiet = 0;
    }
    /* and below IRIS_STRESS_MIN_EX it must refuse to have an opinion at all */
    iris *ks = iris_init(arena_d, sizeof arena_d, NI, NH, NO, CAP, 5);
    load_examples(ks, 6);
    iris_train(ks);
    float ms = 0.0f;
    int small_silent = iris_worst_example(ks, &ms) == -1;

    ok("finds the demonstration that fights, stays quiet otherwise",
       hits >= 9 && quiet && small_silent,
       "corrupted demo ranked first %d/12 (max margin %.2f); loudest clean margin "
       "%.2f (flag %.1f); silent under %d examples %d",
       hits, worst_margin, loudest_clean, (double)IRIS_STRESS_FLAG,
       IRIS_STRESS_MIN_EX, small_silent);
  }

  /* --- 33. the converged trainer reaches a usable fit ---------------------
     The shipping trainer, run to its plateau on the reference task, must end
     with a training error under 1e-3. A floor on the trainer actually
     fitting, not a comparison with any other method.                       */
  {
    iris *k3 = iris_init(arena_d, sizeof arena_d, NI, NH, NO, CAP, 4242u);
    load_examples(k3, 20);
    iris_train(k3);
    float sc = iris_last_error(k3);
    ok("the converged trainer reaches a usable fit", sc > 0.0f && sc < 1e-3f,
       "20 examples trained to the plateau: mean squared error %.3e "
       "(want < 1e-3)", sc);
  }

  /* --- 35. THE DIVERGENCE TRAP -------------------------------------------
     A contradictory demonstration drives the weights past IRIS_W_LIMIT; the
     guard clamps them onto the limit and stops the run. A warm run that
     continued from those pinned weights would trip the guard again on its
     first epoch and do nothing, for ever, so the warm trainers refuse with
     IRIS_DIVERGED_STUCK instead -- on every call, not just the first, because
     a refusal moves no weight. The demonstrations are fine and the weights
     are not, so iris_train, which starts over from the instrument's seed, is
     the way out: after the bad take is deleted it must play exactly what it
     played before the take, to the bit (the demonstrations are the same
     fourteen, in the same order, from the same seed).                     */
  {
    iris *d = iris_init(arena_b, sizeof arena_b, NI, NH, NO, CAP, 1234);
    for (int i = 0; i < 14; ++i) {
      float u = (float)i / 13.0f;
      float in[NI] = { u, 1.0f - u }, out[NO] = { u, 0.5f, 1.0f - u };
      iris_record(d, in, out);
    }
    iris_train(d);
    float pr[NI] = { 0.60f, 0.40f }, healthy[NO];
    iris_predict(d, pr, healthy);

    float bin[NI] = { 0.60f, 0.40f }, bout[NO] = { 0.05f, 0.95f, 0.05f };
    int bad_id = iris_record(d, bin, bout);
    iris_continue_to_plateau(d, 0, 0, 0);
    int diverged = (iris_get_status(d) == IRIS_TRAINING_DIVERGED);

    iris_delete_id(d, bad_id);
    float rc1 = iris_continue_to_plateau(d, 0, 0, 0);
    int st1 = iris_get_status(d) == IRIS_DIVERGED_STUCK;
    float rc2 = iris_continue(d, 20);
    int st2 = iris_get_status(d) == IRIS_DIVERGED_STUCK;
    int refused = rc1 < 0.0f && st1 && rc2 < 0.0f && st2;

    int trained = iris_train(d);
    float back[NO]; iris_predict(d, pr, back);
    float drift = 0.0f;
    for (int o = 0; o < NO; ++o) {
      float e = back[o] - healthy[o]; if (e < 0) e = -e;
      if (e > drift) drift = e;
    }
    int recovered = trained && iris_get_status(d) == IRIS_STATUS_OK
                 && memcmp(back, healthy, sizeof back) == 0;

    ok("divergence refuses loudly, and iris_train recovers",
       diverged && refused && recovered,
       "diverged %d, two warm runs refused with DIVERGED_STUCK %d, iris_train "
       "plays the pre-damage output to the bit %d (largest difference %.6f)",
       diverged, refused, recovered, drift);
  }


  /* --- 36. four instruments at once cannot touch each other --------------
     iris keeps no global or static state, so separate instruments may be
     alive together, and run on separate threads with no locking. Four jobs
     -- four arenas, four seeds, four example counts -- run three ways: one
     after another; interleaved step by step on this thread (every record,
     then every prediction, taken round-robin across the four); and on four
     threads released together. Each job's prediction stream and saved file
     must be the same, to the bit, all three ways. State carried from one call
     to the next, or shared between instruments, shows up as a difference in
     the interleaved run for certain, and in the threaded run whenever the
     scheduler overlaps the work; sh build.sh threads runs this file under
     ThreadSanitizer too, which reports any unsynchronised access whether or
     not it changed a bit. */
  {
    const uint32_t seed[XT_N] = { 1234u, 99u, 40507u, 7u };
    const int nex[XT_N] = { 12, 20, 31, 8 };
    pthread_t th[XT_N];
    for (int i = 0; i < XT_N; ++i) {
      xt_seq[i].seed = xt_rr[i].seed = xt_par[i].seed = seed[i];
      xt_seq[i].nex  = xt_rr[i].nex  = xt_par[i].nex  = nex[i];
    }
    for (int i = 0; i < XT_N; ++i) xt_work(&xt_seq[i]);
    for (int i = 0; i < XT_N; ++i) xt_start(&xt_rr[i]);
    for (int e = 0; e < 31; ++e) for (int i = 0; i < XT_N; ++i) xt_record(&xt_rr[i], e);
    for (int i = 0; i < XT_N; ++i) if (xt_rr[i].k) iris_train(xt_rr[i].k);
    for (int p = 0; p < XT_PREDS; ++p) for (int i = 0; i < XT_N; ++i) xt_play(&xt_rr[i], p);
    for (int i = 0; i < XT_N; ++i) xt_finish(&xt_rr[i]);
    xt_waiting = 0;
    for (int i = 0; i < XT_N; ++i)
      if (pthread_create(&th[i], 0, xt_thread, &xt_par[i]) != 0) {
        printf("FAIL  four instruments at once: pthread_create failed\n");
        return 1;
      }
    for (int i = 0; i < XT_N; ++i) pthread_join(th[i], 0);
    int rr = 0, par = 0;
    for (int i = 0; i < XT_N; ++i) {
      if (xt_same(&xt_seq[i], &xt_rr[i]))  rr++;
      if (xt_same(&xt_seq[i], &xt_par[i])) par++;
    }
    ok("four instruments at once cannot touch each other",
       rr == XT_N && par == XT_N,
       "%d predictions each; interleaved %d/4 and four threads %d/4 equal the "
       "one-at-a-time run; stream fnv1a 0x%08X 0x%08X 0x%08X 0x%08X",
       XT_PREDS, rr, par,
       fnv1a(xt_par[0].stream, sizeof xt_par[0].stream),
       fnv1a(xt_par[1].stream, sizeof xt_par[1].stream),
       fnv1a(xt_par[2].stream, sizeof xt_par[2].stream),
       fnv1a(xt_par[3].stream, sizeof xt_par[3].stream));
  }

  /* --- what it costs on this machine -------------------------------------
     A report, never a pass or a fail: these are wall-clock times on whatever
     computer runs the audit, and they move with its load. Timing on a
     microcontroller is measured on the microcontroller. */
  printf("--------------------------------------------------------------------------\n");
  printf("TRAINING COST on this machine (a report, not a check)\n\n");
  printf("  examples   backprop-600   |  ELM nh-12\n");
  const int exs[] = { 10, 20, 50, 100, 200 };
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
    iris_continue(k, 600);
    double dt = now_ms() - t0;
    /* the closed-form solve is under the timer's resolution; average 200 */
    double t2 = now_ms();
    for (int rep = 0; rep < 200; ++rep)
      iris_train_elm(k, 1e-4f, elm_scratch, sizeof elm_scratch);
    double de = (now_ms() - t2) / 200.0;
    printf("  %6d   %8.1f ms    |  %7.4f ms\n", exs[e], dt, de);
  }

  /* THE PLATEAU TRAINER, iris_train -- the default, and the one number a
     musician actually waits on. It stops when the error plateaus, so the
     epochs it spends are part of the measurement. */
  printf("\n  iris_train (plateau test, ceiling %d)\n", IRIS_CONV_CEILING);
  printf("  examples   epochs spent      time       train MSE   (600-epoch MSE)\n");
  for (int e = 0; e < 5; ++e) {
    iris *kk = iris_init(arena_c, sizeof arena_c, NI, NH, NO, CAP, 4242u);
    load_examples(kk, exs[e]);
    double t0 = now_ms();
    iris_train(kk);
    double dt = now_ms() - t0;
    float ec = iris_last_error(kk);
    int used = kk->tr_done;
    iris *k6 = iris_init(arena_d, sizeof arena_d, NI, NH, NO, CAP, 4242u);
    load_examples(k6, exs[e]);
    float e6 = iris_continue(k6, 600);
    printf("  %6d   %9d   %8.1f ms    %.3e   (%.3e)\n", exs[e], used, dt, ec, e6);
  }

  /* the correction path -- the felt latency of "record one more, fix it" */
  {
    const int cex[2] = { 20, 50 };
    printf("\n");
    for (int e = 0; e < 2; ++e) {
      load_examples(k, cex[e]);
      iris_reseed(k, 4242u); iris_continue(k, 600);
      float nin[NI], nout[NO];
      chain_correction(0, nin, nout);
      iris_record(k, nin, nout);
      double t0 = now_ms();
      float unused = 0.0f;
      for (int rep = 0; rep < 50; ++rep) unused += iris_continue(k, 20);
      double dc = (now_ms() - t0) / 50.0;
      (void)unused;
      printf("  one warm correction (iris_continue, 20 epochs) at %3d examples: %.3f ms\n",
             cex[e] + 1, dc);
    }
  }

  /* playing cost -- the one that must never be slow */
  {
    iris_clear(k);
    for (int i = 0; i < 20; ++i) {
      float u = (float)((i*7919)%97)/97.0f, v = (float)((i*6131)%89)/89.0f;
      float in[NI] = {u,v}, out[NO]; truth(u,v,out); iris_record(k,in,out);
    }
    iris_continue(k, 200);
    float in[NI] = { 0.3f, 0.7f }, out[NO];
    double t0 = now_ms();
    for (int i = 0; i < 1000000; ++i) { in[0] = (float)(i & 1023) / 1023.0f; iris_predict(k, in, out); }
    double per = (now_ms() - t0) * 1000.0 / 1e6;
    printf("\n  playing (one prediction): %.3f us\n", per);
    /* the k-NN read costs O(n_ex) per call; price it at both store sizes */
    const int kex[2] = { 64, 256 };
    for (int e = 0; e < 2; ++e) {
      load_examples(k, kex[e]);
      iris_internal_fit_ranges(k);
      double t1 = now_ms();
      for (int i = 0; i < 200000; ++i) {
        in[0] = (float)(i & 1023) / 1023.0f;
        iris_knn_predict(k, in, out, 3);
      }
      double pk = (now_ms() - t1) * 1000.0 / 2e5;
      printf("  k-NN prediction at %3d examples: %.3f us\n", kex[e], pk);
    }
  }

  printf("\n  memory for this instrument: %zu bytes (%d examples max)\n",
         sizeof arena_a, CAP);
  printf("--------------------------------------------------------------------------\n");
  printf("%s  (%d failed)\n\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", failures);
  return failures ? 1 : 0;
}
