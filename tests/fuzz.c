/* SPDX-License-Identifier: BSD-3-Clause
   Copyright (c) 2026 Kyle Smith */
/* ============================================================================
   fuzz.c — random shapes and random call sequences, judged by the
   sanitizers.

   Every array this program hands the library -- the arena, the input and
   output buffers, the save buffer, the closed-form trainer's scratch -- is a
   heap block of EXACTLY the size the shape demands. It then makes random
   calls across the whole interface, with ordinary numbers, huge and tiny
   ones, zeros, not-a-number and infinity. Built with AddressSanitizer and
   UndefinedBehaviorSanitizer (sh build.sh fuzz does), any access outside
   those blocks, and any undefined arithmetic, is reported by the sanitizer,
   a tool with no opinion about what the right answer is.

   It asserts two things of its own, both documented promises: whatever
   goes in, iris_predict, iris_knn_predict and iris_classify_1nn write a
   finite number into every output (a not-a-number is replaced by the
   range centre and reported, iris.h PART 6 and PART 10); and a file
   iris_save wrote loads back into an instrument of the same shape, which
   then saves the very same bytes (PART 9).

   To see that it can fail, shorten any of the exact-size buffers by one
   element: AddressSanitizer reports a heap-buffer-overflow at once.

     sh build.sh fuzz          400 iterations
     sh build.sh fuzz 5000     a longer run

   Silence here is not proof of correctness. It is evidence that nothing
   went out of bounds on the paths this run happened to walk.
   ========================================================================= */
#include "iris.h"
/* <math.h> for NAN and INFINITY only: they are never made by dividing by
   zero, so the float-divide-by-zero sanitizer judges the library, not this
   file. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long rng_s = 1;
static unsigned rnd(unsigned n) {
  rng_s = rng_s * 6364136223846793005UL + 1442695040888963407UL;
  return n ? (unsigned)((rng_s >> 33) % n) : 0;
}
static float rndf(void) {
  switch (rnd(12)) {
    case 0: return NAN;
    case 1: return INFINITY;
    case 2: return -INFINITY;
    case 3: return 1e30f;  case 4: return -1e30f;
    case 5: return 1e-30f; case 6: return 0.0f;
    default: return (float)((int)rnd(2000) - 1000) / 100.0f;
  }
}

static int finite(float x) { return x == x && x - x == 0.0f; }
static void must_be_finite(const char *who, const float *out, int n, int it) {
  for (int i = 0; i < n; ++i)
    if (!finite(out[i])) {
      printf("FAIL  %s wrote a non-finite output %d (iteration %d)\n", who, i, it);
      exit(1);
    }
}

int main(int argc, char **argv) {
  int iters = argc > 1 ? atoi(argv[1]) : 400;
  int refused = 0, built = 0;
  long calls = 0;
  for (int it = 0; it < iters; ++it) {
    rng_s = (unsigned long)it * 2654435761UL + 12345UL;
    int n_in  = 1 + (int)rnd(IRIS_MAX_IN);
    int n_hid = 8 + (int)rnd(IRIS_MAX_HID - 7);       /* 8 is iris's floor */
    int n_out = 1 + (int)rnd(IRIS_MAX_OUT);
    int cap   = 1 + (int)rnd(40);
    if (rnd(8) == 0) n_hid = 1 + (int)rnd(8);          /* now and then, refused */

    size_t need = iris_size(n_in, n_hid, n_out, cap);
    if (!need) { refused++; continue; }                 /* shape declined; fine */
    unsigned char *arena  = (unsigned char *)malloc(need);   /* EXACT size */
    unsigned char *arena2 = (unsigned char *)malloc(need);
    iris *k = iris_init(arena, need, n_in, n_hid, n_out, cap, rnd(100000));
    if (!k) { refused++; free(arena); free(arena2); continue; }
    built++;

    float *in  = (float *)malloc(sizeof(float) * (size_t)n_in);    /* exact */
    float *out = (float *)malloc(sizeof(float) * (size_t)n_out);   /* exact */
    const size_t scratch_n = IRIS_ELM_SCRATCH(n_hid, n_out);
    unsigned char *scratch = (unsigned char *)malloc(scratch_n);    /* exact */
    const size_t advice_n = need;                       /* IRIS_ARENA of the shape */
    unsigned char *advice = (unsigned char *)malloc(advice_n);

    for (int step = 0; step < 40; ++step, ++calls) {
      for (int i = 0; i < n_in; i++)  in[i]  = rndf();
      for (int i = 0; i < n_out; i++) out[i] = rndf();
      switch (rnd(22)) {
        case 0: case 1: case 2: iris_record(k, in, out); break;
        case 3: iris_predict(k, in, out); must_be_finite("iris_predict", out, n_out, it); break;
        case 4: iris_continue_to_plateau(k, (int)rnd(50), 0, 0); break;
        case 5: iris_clear(k); break;
        case 6: iris_get(k, (int)rnd((unsigned)cap + 3) - 1, in, out); break;
        case 7: (void)iris_novelty(k, in); break;
        case 8: iris_delete_nearest(k, in); break;
        case 9: (void)iris_loo_error(k, (int)rnd(20)); break;
        case 10: (void)iris_suggest_smoothing(k, advice, advice_n); break;
        case 11: iris_knn_predict(k, in, out, (int)rnd(6) - 1);
                 must_be_finite("iris_knn_predict", out, n_out, it); break;
        case 12: iris_classify_1nn(k, in, out);
                 must_be_finite("iris_classify_1nn", out, n_out, it); break;
        case 13: (void)iris_train_elm(k, rnd(4) ? 1e-4f : rndf(), scratch, scratch_n); break;
        case 14: if (iris_train_begin(k, (int)rnd(3000)))
                   while (iris_train_slice(k, 1 + (int)rnd(400))) {}
                 break;
        case 15: iris_continue(k, (int)rnd(30)); break;
        case 16: switch (rnd(3)) {
                   case 0: iris_delete_index(k, (int)rnd((unsigned)cap + 2) - 1); break;
                   case 1: iris_delete_id(k, (int)rnd(60)); break;
                   default: iris_delete_last(k); break;
                 }
                 break;
        case 17: iris_set_smoothing(k, rndf()); break;
        case 18: iris_train(k); break;
        case 19: { /* a save into a buffer of exactly iris_save_size, loaded
                      back, and saved again: the same bytes */
          size_t n = iris_save_size(k);
          unsigned char *file = (unsigned char *)malloc(n ? n : 1);
          unsigned char *again = (unsigned char *)malloc(n ? n : 1);
          size_t w = iris_save(k, file, n);
          iris *r = iris_init(arena2, need, n_in, n_hid, n_out, cap, 7u);
          if (w && r) {
            if (!iris_load(r, file, w)) {
              printf("FAIL  a file iris_save wrote did not load back (iteration %d)\n", it);
              exit(1);
            }
            if (iris_save(r, again, n) != w || memcmp(file, again, w) != 0) {
              printf("FAIL  save, load, save changed the bytes (iteration %d)\n", it);
              exit(1);
            }
          }
          free(file); free(again); break; }
        case 20: { /* a saved file with one byte changed, loaded as it stands */
          size_t n = iris_save_size(k);
          unsigned char *file = (unsigned char *)malloc(n ? n : 1);
          size_t w = iris_save(k, file, n);
          if (w) {
            file[rnd((unsigned)w)] ^= (unsigned char)(1u + rnd(255));
            iris *r = iris_init(arena2, need, n_in, n_hid, n_out, cap, 7u);
            if (r) (void)iris_load(r, file, w);
          }
          free(file); break; }
        default: { float m = 0.0f;
          (void)iris_example_stress(k, (int)rnd((unsigned)cap + 2) - 1);
          (void)iris_worst_example(k, &m); (void)iris_worst_example_id(k, &m); } break;
      }
    }
    free(in); free(out); free(scratch); free(advice); free(arena); free(arena2);
  }
  printf("iterations %d | shapes built %d | shapes refused %d | calls %ld\n",
         iters, built, refused, calls);
  printf("no sanitizer report above this line: no out-of-bounds access or undefined "
         "arithmetic was seen, and every output was finite.\n");
  return 0;
}
