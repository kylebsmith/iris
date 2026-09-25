/* ============================================================================
   threads.c — the threading contract, under ThreadSanitizer.

   iris keeps no global or static state: everything an instrument owns is in
   its own arena. So separate instruments may run on separate threads with
   no locking at all, and one instrument must never be used from two threads
   at once, not even two threads only playing it (iris_predict writes the
   activations and the status inside the instrument). This program checks
   both halves.

     ./threads separate   eight instruments on eight threads, released
                          together, each doing record, train, predict,
                          k-nearest-neighbour and nearest-neighbour reads,
                          the closed-form trainer, save, load, delete and a
                          sliced retrain. Each thread's saved bytes and its
                          stream of predictions are compared with the same
                          work done first on one thread. Exit 0 only if all
                          eight match to the bit.
     ./threads same       the positive control: two threads on ONE
                          instrument, one training, one playing. This is a
                          data race by design, and ThreadSanitizer must
                          report it. If it does not, the silence of
                          "separate" proves nothing.

   sh build.sh threads builds this file with -fsanitize=thread and requires
   "separate" to exit 0 with no report and "same" to produce a
   ThreadSanitizer data-race report.
   ========================================================================= */
/* pthread.h is POSIX, not C99; ask for it. */
#define _POSIX_C_SOURCE 200809L
#include "iris.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define NT 8
#define NI 3
#define NH 12
#define NO 2
#define CAP 32
#define PREDS 5000

typedef struct {
  unsigned char arena[2][IRIS_ARENA(NI, NH, NO, CAP)];
  unsigned char scratch[IRIS_ELM_SCRATCH(NH, NO)];
  unsigned char file[4096];
  float stream[PREDS / 50][3 * NO];
  size_t file_n;
} job;
static job SEQ[NT], PAR[NT];

static uint32_t fnv1a(const void *p, size_t n) {
  const unsigned char *b = (const unsigned char *)p;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
  return h;
}

/* The whole life of one instrument, touching nothing outside its job. */
static void work(job *j, int t) {
  iris *k = iris_init(j->arena[0], sizeof j->arena[0], NI, NH, NO, CAP, 1000u + (uint32_t)t);
  uint32_t s = 77u + (uint32_t)t * 13u;
  float in[NI], out[NO];
  j->file_n = 0;
  if (!k) return;
  for (int e = 0; e < 20; ++e) {
    for (int i = 0; i < NI; ++i) { s = s * 1664525u + 1013904223u; in[i] = (float)(s >> 8) / 16777216.0f; }
    out[0] = 0.2f + 0.5f * in[0] * in[1]; out[1] = 0.9f - 0.6f * in[2];
    iris_record(k, in, out);
  }
  iris_continue(k, 400);
  for (int p = 0; p < PREDS; ++p) {
    in[0] = (float)(p % 97) / 96.0f; in[1] = (float)(p % 31) / 30.0f; in[2] = 0.5f;
    iris_predict(k, in, out);
    if (p % 50 == 0) {
      float *row = j->stream[p / 50];
      row[0] = out[0]; row[1] = out[1];
      iris_knn_predict(k, in, row + 2, 3);
      (void)iris_classify_1nn(k, in, row + 4);
    }
  }
  iris_train_elm(k, 1e-4f, j->scratch, sizeof j->scratch);
  size_t n = iris_save(k, j->file, sizeof j->file);
  iris *k2 = iris_init(j->arena[1], sizeof j->arena[1], NI, NH, NO, CAP, 5u);
  if (!k2 || !iris_load(k2, j->file, n)) return;
  iris_delete_last(k2);
  iris_train_begin(k2, 1000);
  while (iris_train_slice(k2, 250)) {}
  j->file_n = iris_save(k2, j->file, sizeof j->file);
}

/* Every thread waits here until all have arrived, so their work overlaps.
   (macOS has no pthread_barrier_t, so it is a mutex and a condition.) */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;
static int waiting, needed;
static void barrier(void) {
  pthread_mutex_lock(&mu);
  if (++waiting == needed) pthread_cond_broadcast(&cv);
  else while (waiting < needed) pthread_cond_wait(&cv, &mu);
  pthread_mutex_unlock(&mu);
}

static int IDX[NT];
static void *separate_thread(void *arg) {
  int t = *(const int *)arg;
  barrier();
  work(&PAR[t], t);
  return 0;
}

/* the positive control: one instrument, two threads */
static unsigned char SHARED_ARENA[IRIS_ARENA(NI, NH, NO, CAP)];
static iris *SHARED;
static void *trainer(void *arg) {
  (void)arg; barrier();
  for (int r = 0; r < 20; ++r) iris_continue(SHARED, 50);
  return 0;
}
static void *player(void *arg) {
  float in[NI] = { 0.3f, 0.4f, 0.5f }, out[NO];
  (void)arg; barrier();
  for (int p = 0; p < 20000; ++p) iris_predict(SHARED, in, out);
  return 0;
}

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "separate";
  if (!strcmp(mode, "same")) {
    float in[NI], out[NO];
    SHARED = iris_init(SHARED_ARENA, sizeof SHARED_ARENA, NI, NH, NO, CAP, 1u);
    for (int e = 0; SHARED && e < 12; ++e) {
      in[0] = (float)e; in[1] = (float)(e % 3); in[2] = 1.0f;
      out[0] = (float)e / 11.0f; out[1] = 0.5f;
      iris_record(SHARED, in, out);
    }
    iris_continue(SHARED, 50);
    pthread_t a, b;
    needed = 2;
    pthread_create(&a, 0, trainer, 0); pthread_create(&b, 0, player, 0);
    pthread_join(a, 0); pthread_join(b, 0);
    printf("same-instrument run finished: a ThreadSanitizer data-race report "
           "above is the expected outcome\n");
    return 0;
  }
  if (strcmp(mode, "separate")) { printf("usage: threads [separate|same]\n"); return 2; }

  for (int t = 0; t < NT; ++t) work(&SEQ[t], t);          /* one thread first */
  pthread_t th[NT];
  needed = NT;
  for (int t = 0; t < NT; ++t) {
    IDX[t] = t;
    if (pthread_create(&th[t], 0, separate_thread, &IDX[t]) != 0) {
      printf("FAIL  pthread_create\n"); return 1;
    }
  }
  for (int t = 0; t < NT; ++t) pthread_join(th[t], 0);
  int same = 0;
  for (int t = 0; t < NT; ++t) {
    const int ok = SEQ[t].file_n > 0 && SEQ[t].file_n == PAR[t].file_n
                && memcmp(SEQ[t].file, PAR[t].file, SEQ[t].file_n) == 0
                && memcmp(SEQ[t].stream, PAR[t].stream, sizeof SEQ[t].stream) == 0;
    printf("  thread %d  one thread 0x%08X  eight threads 0x%08X  %s\n", t,
           fnv1a(SEQ[t].file, SEQ[t].file_n) ^ fnv1a(SEQ[t].stream, sizeof SEQ[t].stream),
           fnv1a(PAR[t].file, PAR[t].file_n) ^ fnv1a(PAR[t].stream, sizeof PAR[t].stream),
           ok ? "same" : "DIFFERENT");
    same += ok;
  }
  printf("%s  %d of %d instruments on their own threads reproduced the one-thread "
         "run bit for bit\n", same == NT ? "PASS" : "FAIL", same, NT);
  return same == NT ? 0 : 1;
}
