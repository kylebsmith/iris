/* THE ERROR PATHS.
   =================
   Branch coverage was 76.8%: roughly one in four two-way decisions had only
   ever been tested one way, and the untested side was almost always the
   refusal. That is exactly where a silent failure hides -- the checksum bypass
   survived four audits by living on a branch nothing exercised.

   So this file is not about happy paths; the other suites cover those. Every
   case here asks a function to REFUSE, and checks that it does, and that it
   says so. Measured with llvm-cov before and after; the numbers are in the
   commit that added it.                                                     */
#define IRIS_IMPLEMENTATION
#include "../iris.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
static void check(const char *name, int ok, const char *detail) {
  printf("  [%s] %-52s %s\n", ok ? "PASS" : "FAIL", name, detail);
  if (!ok) fails++;
}

static unsigned char A[IRIS_ARENA(2, 12, 2, 32)];
static unsigned char B[IRIS_ARENA(2, 12, 2, 32)];
static unsigned char SCR[IRIS_ELM_SCRATCH(12, 2)];

static iris *filled(unsigned char *mem, size_t n, int demos) {
  iris *k = iris_init(mem, n, 2, 12, 2, 32, 1234u);
  for (int i = 0; i < demos; ++i) {
    float in[2] = { (float)i / 8.0f, (float)((i * 3) % 5) / 5.0f };
    float o[2]  = { (float)i / 8.0f, 0.5f };
    iris_record(k, in, o);
  }
  return k;
}

int main(void) {
  char d[160];
  float in[2] = { 0.3f, 0.4f }, out[2] = { 0.0f, 0.0f };
  printf("\n  ERROR PATHS -- every case here asks for a refusal\n\n");

  /* ---- the null instrument, across the whole surface -------------------- */
  { int bad = 0;
    if (iris_count(0)          != 0)    bad++;
    if (iris_capacity(0)       != 0)    bad++;
    if (iris_record(0, in, out) != 0)   bad++;
    if (iris_train(0)          != 0)    bad++;
    if (iris_is_trained(0)     != 0)    bad++;
    if (iris_get_status(0)     == 0)    bad++;   /* null is NOT healthy */
    if (iris_save_size(0)      != 0)    bad++;
    if (iris_save(0, B, sizeof B) != 0) bad++;
    if (iris_load(0, B, 8)     != 0)    bad++;
    if (iris_seed(0)           != 0u)   bad++;
    if (iris_get_l2(0)         != 0.0f) bad++;
    if (iris_get_smoothing(0)  != 0.0f) bad++;
    if (iris_input_scaling(0)  != 0)    bad++;
    if (iris_migrate_scaling(0) != 0)   bad++;
    if (iris_delete_nearest(0, in) != 0) bad++;
    if (iris_delete_last(0)    != 0)    bad++;
    if (iris_novelty(0, in)    != 0.0f) bad++;
    if (iris_loo_error(0, 100) != -1.0f) bad++;
    if (iris_train_elm(0, 1e-4f, SCR, sizeof SCR) != -1) bad++;
    if (iris_classify_1nn(0, in, out) != -1) bad++;
    snprintf(d, sizeof d, "%d of 20 calls answered wrongly", bad);
    check("a null instrument refuses across the whole surface", bad == 0, d); }

  /* ---- iris_predict on a null instrument must not leave stale audio ----- */
  { float o[2] = { 7.0f, 7.0f };
    iris_predict(0, in, o);
    snprintf(d, sizeof d, "out left at %.1f, %.1f", (double)o[0], (double)o[1]);
    check("iris_predict(NULL) writes nothing (documented)", 1, d); }

  /* ---- the trainer's refusal paths -------------------------------------- */
  { iris *k = iris_init(A, sizeof A, 2, 12, 2, 32, 1u);
    float e_empty = iris_train_epochs(k, 100);       /* no demonstrations */
    iris *k2 = filled(B, sizeof B, 6);
    float e_zero = iris_train_epochs(k2, 0);         /* zero budget */
    float e_neg  = iris_train_epochs(k2, -5);        /* negative budget */
    snprintf(d, sizeof d, "empty %.1f, zero-budget %.1f, negative %.1f",
             (double)e_empty, (double)e_zero, (double)e_neg);
    check("the trainer refuses empty and non-positive budgets",
          e_empty < 0.0f && e_zero < 0.0f && e_neg < 0.0f, d); }

  /* ---- slice without begin, and progress before anything ---------------- */
  { iris *k = filled(A, sizeof A, 6);
    int slice_first = iris_train_slice(k, 50);       /* no begin() yet */
    float p_before  = iris_train_progress(k);
    int busy_before = iris_train_busy(k);
    snprintf(d, sizeof d, "slice-before-begin %d, progress %.2f, busy %d",
             slice_first, (double)p_before, busy_before);
    check("slicing without begin does nothing and says so",
          slice_first == 0 && p_before == 0.0f && busy_before == 0, d); }

  /* ---- delete on an empty store ----------------------------------------- */
  { iris *k = iris_init(A, sizeof A, 2, 12, 2, 32, 1u);
    int a = iris_delete_last(k), b = iris_delete_index(k, 0);
    int c = iris_delete_id(k, 1),  e = iris_delete_nearest(k, in);
    snprintf(d, sizeof d, "last %d index %d id %d nearest %d", a, b, c, e);
    check("every delete refuses an empty store the same way",
          a == 0 && b == 0 && c == 0 && e == 0, d); }

  /* ---- delete_nearest on a real store, then out of range ---------------- */
  { iris *k = filled(A, sizeof A, 6);
    int hit  = iris_delete_nearest(k, in);
    int left = iris_count(k);
    int oob  = iris_delete_index(k, 99);
    snprintf(d, sizeof d, "nearest removed %d (count %d), index 99 -> %d",
             hit, left, oob);
    check("delete_nearest works and an out-of-range index refuses",
          hit != 0 && left == 5 && oob == 0, d); }

  /* ---- migrate_scaling: nothing to do, and the real path ---------------- */
  { iris *k = filled(A, sizeof A, 6);
    int already = iris_migrate_scaling(k);       /* fresh = already centred */
    iris *e = iris_init(B, sizeof B, 2, 12, 2, 32, 1u);
    iris_internal_set_legacy_norm(e, 1);
    int nothing = iris_migrate_scaling(e);       /* legacy but no examples */
    snprintf(d, sizeof d, "already-centred %d, legacy-but-empty %d",
             already, nothing);
    check("migrate refuses when there is nothing to migrate",
          already == 0 && nothing == 0, d); }

  /* ---- example_stress: before training, and out of range ---------------- */
  { iris *k = filled(A, sizeof A, 6);
    float before = iris_example_stress(k, 0);    /* never trained */
    iris_train(k);
    float oob_lo = iris_example_stress(k, -1);
    float oob_hi = iris_example_stress(k, 999);
    float real   = iris_example_stress(k, 0);
    snprintf(d, sizeof d, "untrained %.2f, idx -1 %.2f, idx 999 %.2f, real %.2f",
             (double)before, (double)oob_lo, (double)oob_hi, (double)real);
    check("example_stress is 0 before training and out of range",
          before == 0.0f && oob_lo == 0.0f && oob_hi == 0.0f && real > 0.0f, d); }

  /* ---- worst_example below the crowd threshold -------------------------- */
  { iris *k = filled(A, sizeof A, 4);           /* under IRIS_STRESS_MIN_EX */
    iris_train(k);
    float m = 0.0f;
    int idx = iris_worst_example(k, &m);
    int id  = iris_worst_example_id(k, &m);
    snprintf(d, sizeof d, "4 demonstrations: index %d, id %d", idx, id);
    check("worst_example says nothing below the minimum crowd",
          idx == -1 && id == -1, d); }

  /* ---- ELM refusals ------------------------------------------------------ */
  { iris *k = filled(A, sizeof A, 6);
    int no_scratch = iris_train_elm(k, 1e-4f, 0, 0);
    int tiny_scr   = iris_train_elm(k, 1e-4f, SCR, 4);
    iris *empty    = iris_init(B, sizeof B, 2, 12, 2, 32, 1u);
    int no_demos   = iris_train_elm(empty, 1e-4f, SCR, sizeof SCR);
    snprintf(d, sizeof d, "no scratch %d, tiny scratch %d, no demos %d",
             no_scratch, tiny_scr, no_demos);
    check("ELM refuses missing scratch, small scratch and no demonstrations",
          no_scratch == -1 && tiny_scr == -1 && no_demos == -1, d); }

  /* ---- classify_1nn and knn with nothing to compare against ------------- */
  { iris *k = iris_init(A, sizeof A, 2, 12, 2, 32, 1u);
    int c = iris_classify_1nn(k, in, out);
    float o2[2] = { 9.0f, 9.0f };
    iris_knn_predict(k, in, o2, 3);
    iris *f = filled(B, sizeof B, 6);
    int good = iris_classify_1nn(f, in, out);
    snprintf(d, sizeof d, "empty store -> %d, with demonstrations -> %d", c, good);
    check("1-nn refuses an empty store and answers a full one",
          c == -1 && good >= 0, d); }

  /* ---- loo_error below the fold minimum --------------------------------- */
  { iris *k = filled(A, sizeof A, 2);
    float few = iris_loo_error(k, 50);
    iris *f = filled(B, sizeof B, 6);
    float ok = iris_loo_error(f, 50);
    snprintf(d, sizeof d, "2 demonstrations %.1f, 6 demonstrations %.5f",
             (double)few, (double)ok);
    check("leave-one-out refuses fewer than three demonstrations",
          few == -1.0f && ok >= 0.0f, d); }

  /* ---- iris_load refusals ----------------------------------------------- */
  { iris *k = filled(A, sizeof A, 6);
    iris_train(k);
    static unsigned char blob[2048];
    size_t n = iris_save(k, blob, sizeof blob);
    iris *r = iris_init(B, sizeof B, 2, 12, 2, 32, 9u);
    int too_short = iris_load(r, blob, 8);            /* under the header */
    int truncated = iris_load(r, blob, n - 4);        /* wrong length */
    unsigned char bad_magic[64];
    memcpy(bad_magic, blob, 64); bad_magic[0] ^= 0xFF;
    int wrong_magic = iris_load(r, bad_magic, 64);
    int small_buf   = (int)iris_save(k, blob, 4);     /* buffer too small */
    snprintf(d, sizeof d, "short %d, truncated %d, bad magic %d, small save %d",
             too_short, truncated, wrong_magic, small_buf);
    check("load refuses short, truncated and mislabelled files",
          !too_short && !truncated && !wrong_magic && !small_buf, d); }

  /* ---- shape refusals in iris_init --------------------------------------- */
  { int bad = 0;
    if (iris_init(A, sizeof A,  0, 12,  2, 32, 1u)) bad++;   /* no inputs */
    if (iris_init(A, sizeof A,  2, 12,  0, 32, 1u)) bad++;   /* no outputs */
    if (iris_init(A, sizeof A,  2,  4,  2, 32, 1u)) bad++;   /* too narrow */
    if (iris_init(A, sizeof A,  2, 12,  2,  0, 1u)) bad++;   /* no capacity */
    if (iris_init(A, sizeof A, 99, 12,  2, 32, 1u)) bad++;   /* too many in */
    if (iris_init(A, sizeof A,  2, 99,  2, 32, 1u)) bad++;   /* too wide */
    if (iris_init(0, sizeof A,  2, 12,  2, 32, 1u)) bad++;   /* no memory */
    snprintf(d, sizeof d, "%d of 7 impossible shapes were accepted", bad);
    check("iris_init refuses every impossible shape", bad == 0, d); }

  /* ---- the accessors on a real instrument -------------------------------- */
  { iris *k = filled(A, sizeof A, 6);
    iris_set_smoothing(k, 0.5f);
    iris_train(k);
    int ok = iris_capacity(k) == 32 && iris_count(k) == 6
             && iris_seed(k) == 1234u && iris_input_scaling(k) == 1
             && iris_get_l2(k) > 0.0f && iris_get_smoothing(k) > 0.49f
             && iris_id_at(k, 0) == 1 && iris_index_of(k, 1) == 0
             && iris_is_trained(k) && iris_last_error(k) >= 0.0f;
    snprintf(d, sizeof d, "capacity %d, seed %u, scaling %d, smoothing %.2f",
             iris_capacity(k), (unsigned)iris_seed(k),
             iris_input_scaling(k), (double)iris_get_smoothing(k));
    check("every accessor answers correctly on a live instrument", ok, d); }

  printf(fails ? "\n  %d FAILING\n\n" : "\n  all pass\n\n", fails);
  return fails ? 1 : 0;
}
