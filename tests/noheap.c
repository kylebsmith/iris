/* ============================================================================
   noheap.c — iris never calls the allocator, measured.

   iris.h promises no heap: the caller hands it one arena and it never asks
   the C library for memory. Reading the source says so; this program counts.
   It runs every public function in a window, and tests/noheap_interpose.c
   (loaded ahead of the C library) counts every call to malloc, calloc,
   realloc, free and the rest of the allocator made inside the window. Every
   window around an iris call must count zero.

   A count of zero is also what a counter that counts nothing reports, so
   the first windows are POSITIVE CONTROLS that must count: this program's
   own calls to malloc, calloc, realloc and posix_memalign, each followed by
   free. They go through volatile function pointers because a compiler may
   delete an allocation whose result is unused, and a control the compiler
   deleted would count zero and prove nothing. Without the interposer
   loaded the program cannot count at all, and it says so and exits 2.

   sh build.sh noheap builds both halves and runs this with the interposer
   (it must exit 0) and without it (it must exit 2). The arm also checks that
   every public function iris.h defines is named in this file.
   ========================================================================= */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif
#include "iris.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#define NI 2
#define NH 12
#define NO 3
#define CAP 64
static unsigned char A1[IRIS_ARENA(NI, NH, NO, CAP)];
static unsigned char A2[IRIS_ARENA(NI, NH, NO, CAP)];
static unsigned char SCRATCH[IRIS_ELM_SCRATCH(NH, NO) + IRIS_ARENA(NI, NH, NO, CAP)];
static unsigned char FILEBUF[8192];
static iris *K, *K2;
static volatile float SINK;
static size_t SAVED;

static void demos(iris *k, int n) {
  for (int e = 0; e < n; ++e) {
    float in[NI] = { (float)e / (float)(n - 1), (float)(e % 5) / 4.0f };
    float out[NO] = { 0.2f + 0.6f * in[0], 0.5f + 0.3f * in[1] - 0.2f * in[0],
                      0.9f - 0.7f * in[0] * in[1] };
    iris_record(k, in, out);
  }
}
static int progress(void *user, int done, int ceiling, float err) {
  (void)user; (void)done; (void)ceiling; SINK += err; return 1;
}

/* ---- the scenarios: every public function ------------------------------ */
static void s_setup(void) {
  SINK = (float)iris_size(NI, NH, NO, CAP);
  K = iris_init(A1, sizeof A1, NI, NH, NO, CAP, 1234u);
  iris_reseed(K, 99u);
  SINK += (float)iris_seed(K) + (float)iris_get_status(K);
}
static void s_record(void) {
  float in[NI], out[NO];
  demos(K, 40);
  SINK = (float)(iris_count(K) + iris_capacity(K) + iris_get(K, 3, in, out)
               + iris_index_of(K, 4) + iris_id_at(K, 5));
}
static void s_train(void)    { SINK = (float)iris_train(K) + (float)iris_is_trained(K) + iris_last_error(K); }
static void s_slices(void) {
  iris_train_begin(K, 6000);
  while (iris_train_slice(K, 500)) SINK = iris_train_progress(K) + (float)iris_train_busy(K);
  SINK += (float)iris_train_epochs_done(K);
}
static void s_smoothing(void) { iris_set_smoothing(K, 0.2f); SINK = iris_get_smoothing(K); iris_set_smoothing(K, 0.0f); }
static void s_continue(void) { SINK = iris_continue(K, 800); }
static void s_plateau(void)  { SINK = iris_continue_to_plateau(K, 4000, progress, 0); }
static void s_elm(void)      { SINK = (float)iris_train_elm(K, 1e-4f, SCRATCH, sizeof SCRATCH); }
static void s_predict(void) {
  float in[NI], out[NO];
  for (int i = 0; i < 100000; ++i) {
    in[0] = (float)(i % 101) / 100.0f; in[1] = (float)(i % 37) / 36.0f;
    iris_predict(K, in, out); SINK += out[0];
  }
}
static void s_neighbours(void) {
  float in[NI], out[NO];
  for (int i = 0; i < 10000; ++i) {
    in[0] = (float)(i % 101) / 100.0f; in[1] = 0.3f;
    iris_knn_predict(K, in, out, 3); SINK += out[1];
    SINK += (float)iris_classify_1nn(K, in, out);
  }
}
static void s_queries(void) {
  float in[NI] = { 0.4f, 0.6f }, m = 0.0f;
  SINK = iris_novelty(K, in) + iris_example_stress(K, 3);
  SINK += (float)iris_worst_example(K, &m) + (float)iris_worst_example_id(K, &m) + m;
}
static void s_loo(void)      { SINK = iris_loo_error(K, 50); }
static void s_suggest(void)  { SINK = iris_suggest_smoothing(K, SCRATCH, sizeof SCRATCH); }
static void s_save(void)     { SINK = (float)iris_save_size(K); SAVED = iris_save(K, FILEBUF, sizeof FILEBUF); }
static void s_load(void) {
  K2 = iris_init(A2, sizeof A2, NI, NH, NO, CAP, 5u);
  SINK = (float)iris_load(K2, FILEBUF, SAVED);
}
static void s_delete(void) {
  float in[NI] = { 0.5f, 0.5f };
  SINK = (float)(iris_delete_id(K, 3) + iris_delete_nearest(K, in)
               + iris_delete_last(K) + iris_delete_index(K, 0));
}
static void s_clear(void)    { iris_clear(K2); SINK = (float)iris_count(K2); }
static void s_empty(void)    { }

/* ---- positive controls: each MUST be counted --------------------------- */
static void *(*volatile v_malloc)(size_t) = malloc;
static void *(*volatile v_calloc)(size_t, size_t) = calloc;
static void *(*volatile v_realloc)(void *, size_t) = realloc;
static int   (*volatile v_posix_memalign)(void **, size_t, size_t) = posix_memalign;
static void  (*volatile v_free)(void *) = free;
static void p_malloc(void)  { void *p = v_malloc(16); SINK = (float)(p != 0); v_free(p); }
static void p_calloc(void)  { void *p = v_calloc(4, 4); SINK = (float)(p != 0); v_free(p); }
static void p_realloc(void) { void *p = v_realloc(0, 32); SINK = (float)(p != 0); v_free(p); }
static void p_posix(void)   { void *p = 0; SINK = (float)v_posix_memalign(&p, 64, 64); v_free(p); }

typedef struct { const char *name; void (*fn)(void); int must_count; } scenario;
static const scenario S[] = {
  { "control: an empty window",           s_empty,      0 },
  { "control: malloc + free",             p_malloc,     1 },
  { "control: calloc + free",             p_calloc,     1 },
  { "control: realloc + free",            p_realloc,    1 },
  { "control: posix_memalign + free",     p_posix,      1 },
  { "size, init, reseed, seed, status",   s_setup,      0 },
  { "record x40 and the store readers",   s_record,     0 },
  { "iris_train",                         s_train,      0 },
  { "train_begin and a slice loop",       s_slices,     0 },
  { "smoothing, set and get",             s_smoothing,  0 },
  { "iris_continue(800)",                 s_continue,   0 },
  { "iris_continue_to_plateau",           s_plateau,    0 },
  { "iris_train_elm",                     s_elm,        0 },
  { "100,000 predictions",                s_predict,    0 },
  { "10,000 k-NN and 1-NN reads",         s_neighbours, 0 },
  { "novelty, stress, worst example",     s_queries,    0 },
  { "iris_loo_error",                     s_loo,        0 },
  { "iris_suggest_smoothing",             s_suggest,    0 },
  { "save_size and save",                 s_save,       0 },
  { "init and load",                      s_load,       0 },
  { "the four deletes",                   s_delete,     0 },
  { "iris_clear",                         s_clear,      0 },
};
#define NS ((int)(sizeof S / sizeof S[0]))

int main(void) {
  void (*start)(void) = 0;
  long (*stop)(void) = 0;
  long counts[NS];
  int bad = 0;
  *(void **)&start = dlsym(RTLD_DEFAULT, "hc_start");
  *(void **)&stop  = dlsym(RTLD_DEFAULT, "hc_stop");
  if (!start || !stop) {
    printf("REFUSED: the allocator interposer is not loaded, so nothing can be counted.\n");
    return 2;
  }
  printf("counting allocator calls inside each window\n");   /* stdio's buffer, outside */
  for (int i = 0; i < NS; ++i) { start(); S[i].fn(); counts[i] = stop(); }
  for (int i = 0; i < NS; ++i) {
    const int ok = S[i].must_count ? counts[i] > 0 : counts[i] == 0;
    if (!ok) bad = 1;
    printf("  %s  %-38s allocator calls %ld\n", ok ? "PASS" : "FAIL", S[i].name, counts[i]);
  }
  printf("  (the scenarios did real work: trained %d, %d demonstrations, status %d)\n",
         iris_is_trained(K), iris_count(K), (int)iris_get_status(K));
  printf("%s\n", bad ? "FAIL  an allocator call was counted in an iris window, or a control was not counted"
                     : "PASS  every control counted; every iris window made zero allocator calls");
  return bad;
}
