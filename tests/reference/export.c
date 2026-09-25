/* ============================================================================
   export.c — what iris computes, written out for tests/reference/check.py.

   check.py is a second implementation of the iris model in 64-bit floating
   point, written from the model iris.h describes in its comments, not from
   its code. This program is the only part of the comparison that calls
   iris.h. It changes nothing in the library: it drives the public functions
   and reads the instrument's fields after each step.

     ./export traj FILE SMOOTHING   a training run: the demonstrations, the
                                    starting weights, then after every epoch
                                    the shuffle order, the epoch's error, the
                                    weights and the momentum velocities
     ./export save FILE             an instrument trained with iris_train,
                                    saved to FILE, and its predictions on a
                                    41 x 41 grid printed as hexadecimal floats
     ./export neighbours            40 demonstrations, then the k-nearest-
                                    neighbour (k = 3) and nearest-neighbour
                                    answers to 2,000 queries

   THE TRAINING RUN is iris_train_begin followed by iris_train_slice(k, 1)
   until the run ends, which iris.h promises is bit-identical to iris_train
   (tests/train.c checks that promise), so one epoch at a time is visible
   without touching the header. Every number is written in the host's byte
   order as raw 32-bit values; check.py reads them the same way on the same
   machine.
   ========================================================================= */
#include "iris.h"
/* The demonstrations are computed here; switch contraction off for this
   file's own arithmetic, as iris.h does for its own. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NI 2
#define NH 12
#define NO 3
#define CAP 64
static unsigned char arena[IRIS_ARENA(NI, NH, NO, CAP)];

static void truth(float x, float y, float *o) {
  o[0] = 0.5f + 0.45f * iris_internal_tanh(3.0f * (x - 0.5f));
  o[1] = 0.5f + 0.40f * iris_internal_tanh(2.5f * (y - 0.5f) * (x + 0.3f));
  o[2] = 0.2f + 0.6f  * (x * y);
}
static iris *build(uint32_t seed, int n) {
  iris *k = iris_init(arena, sizeof arena, NI, NH, NO, CAP, seed);
  for (int i = 0; k && i < n; ++i) {
    float in[NI] = { (float)((i * 7919) % 97) / 97.0f, (float)((i * 6131) % 89) / 89.0f }, o[NO];
    truth(in[0], in[1], o);
    iris_record(k, in, o);
  }
  return k;
}
static void put(FILE *f, const void *p, size_t n) {
  if (fwrite(p, 1, n, f) != n) { fprintf(stderr, "write failed\n"); exit(1); }
}
static void weights(FILE *f, const iris *k, int velocities) {
  if (!velocities) {
    put(f, k->w1, 4 * NH * NI); put(f, k->b1, 4 * NH); put(f, k->w2, 4 * NO * NH); put(f, k->b2, 4 * NO);
  } else {
    put(f, k->v_w1, 4 * NH * NI); put(f, k->v_b1, 4 * NH); put(f, k->v_w2, 4 * NO * NH); put(f, k->v_b2, 4 * NO);
  }
}

int main(int argc, char **argv) {
  if (argc >= 4 && !strcmp(argv[1], "traj")) {
    iris *k = build(1234u, 20);
    float sm = (float)atof(argv[3]);
    FILE *f = fopen(argv[2], "wb");
    if (!k || !f) return 1;
    iris_set_smoothing(k, sm);
    if (!iris_train_begin(k, 0)) return 1;
    int32_t hdr[5] = { NI, NH, NO, iris_count(k), (int32_t)iris_seed(k) };
    put(f, hdr, sizeof hdr);
    put(f, &sm, 4);
    put(f, k->ex, sizeof(float) * (size_t)iris_count(k) * (NI + NO));
    weights(f, k, 0);                               /* the starting weights */
    int32_t ep = 0;
    for (;;) {
      int more = iris_train_slice(k, 1);
      if (iris_train_epochs_done(k) == ep) break;   /* no epoch ran */
      ep = iris_train_epochs_done(k);
      if (ep == 1) {
        put(f, k->in_lo, 4 * NI); put(f, k->in_hi, 4 * NI);
        put(f, k->out_lo, 4 * NO); put(f, k->out_hi, 4 * NO);
      }
      put(f, &ep, 4);
      put(f, k->order, 4 * (size_t)iris_count(k));
      put(f, &k->last_error, 4);
      weights(f, k, 0);
      weights(f, k, 1);
      if (!more) break;
    }
    fclose(f);
    fprintf(stderr, "trajectory: smoothing %g, %d epochs, status %d\n",
            (double)sm, iris_train_epochs_done(k), (int)iris_get_status(k));
    return iris_get_status(k) == IRIS_STATUS_OK ? 0 : 1;
  }
  if (argc >= 3 && !strcmp(argv[1], "save")) {
    iris *k = build(1234u, 20);
    static unsigned char buf[65536];
    if (!k || !iris_train(k)) return 1;
    size_t n = iris_save(k, buf, sizeof buf);
    FILE *f = fopen(argv[2], "wb");
    if (!f || n == 0) return 1;
    put(f, buf, n);
    fclose(f);
    for (int a = 0; a <= 40; ++a) for (int b = 0; b <= 40; ++b) {
      float in[NI] = { (float)a / 40.0f, (float)b / 40.0f }, out[NO];
      iris_predict(k, in, out);
      printf("%a %a %a %a %a\n", (double)in[0], (double)in[1],
             (double)out[0], (double)out[1], (double)out[2]);
    }
    return 0;
  }
  if (argc >= 2 && !strcmp(argv[1], "neighbours")) {
    iris *k = build(1234u, 40);                     /* never fitted: ranges from the demonstrations */
    if (!k) return 1;
    for (int i = 0; i < iris_count(k); ++i) {
      float in[NI], out[NO];
      iris_get(k, i, in, out);
      printf("D %a %a %a %a %a\n", (double)in[0], (double)in[1], (double)out[0], (double)out[1], (double)out[2]);
    }
    uint32_t st = 99u;
    for (int q = 0; q < 2000; ++q) {
      st = st * 1664525u + 1013904223u; float x = (float)(st >> 8) / 16777216.0f * 1.4f - 0.2f;
      st = st * 1664525u + 1013904223u; float y = (float)(st >> 8) / 16777216.0f * 1.4f - 0.2f;
      float in[NI] = { x, y }, o3[NO], o1[NO];
      iris_knn_predict(k, in, o3, 3);
      int id = iris_classify_1nn(k, in, o1);
      printf("Q %a %a %a %a %a %d %a %a %a\n", (double)x, (double)y, (double)o3[0], (double)o3[1],
             (double)o3[2], id, (double)o1[0], (double)o1[1], (double)o1[2]);
    }
    return 0;
  }
  fprintf(stderr, "usage: export traj FILE SMOOTHING | save FILE | neighbours\n");
  return 2;
}
