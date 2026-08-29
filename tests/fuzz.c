/* SPDX-License-Identifier: BSD-3-Clause
   Copyright (c) 2026 Kyle Smith */
/* ============================================================================
   fuzz.c — the check that does NOT encode my expectations.

   Every other test in this repo compares the library against assertions I
   wrote. That is circular, and it has failed in exactly the way you would
   predict: the guards A/B test once compared two byte-identical builds, the
   claims check verified the version macro while the file's own masthead
   disagreed, and make_golden destroyed the fixture it existed to protect.
   Code I wrote agreeing with expectations I wrote is not evidence.

   This file asserts almost nothing. It allocates every caller array on the
   heap at EXACTLY the size the shape demands, then hammers the API with random
   shapes and random call sequences, including not-a-number and infinity. Under
   AddressSanitizer and UndefinedBehaviorSanitizer, any access outside those
   arrays is reported by the sanitizer — a tool that has never heard of this
   project and has no opinion about what the right answer is.

   A test that cannot fail is worthless, so verify this one can: shorten the
   output array by one float and it reports a heap-buffer-overflow immediately.

     sh build.sh fuzz          400 iterations, roughly 14,000 calls
     sh build.sh fuzz 5000     longer run

   Silence here is not proof of correctness. It is only evidence that nothing
   went out of bounds on the paths it happened to walk.
   ========================================================================= */
#include "iris.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static unsigned long rng_s = 1;
static unsigned rnd(unsigned n){ rng_s = rng_s*6364136223846793005UL + 1442695040888963407UL;
                                 return n ? (unsigned)((rng_s>>33) % n) : 0; }
static float rndf(void){
  switch(rnd(12)){
    case 0: return 0.0f/0.0f;                 /* not-a-number */
    case 1: return 1.0f/0.0f;                 /* infinity */
    case 2: return -1.0f/0.0f;
    case 3: return 1e30f;  case 4: return -1e30f;
    case 5: return 1e-30f; case 6: return 0.0f;
    default: return (float)((int)rnd(2000)-1000)/100.0f;
  }
}

int main(int argc,char**argv){
  int iters = argc>1 ? atoi(argv[1]) : 300;
  int refused=0, built=0, crashes_are_reported_by_the_sanitizer=0;
  (void)crashes_are_reported_by_the_sanitizer;
  for(int it=0; it<iters; ++it){
    rng_s = (unsigned long)it*2654435761UL + 12345UL;
    int n_in  = 1 + (int)rnd(IRIS_MAX_IN);
    int n_hid = 1 + (int)rnd(IRIS_MAX_HID);
    int n_out = 1 + (int)rnd(IRIS_MAX_OUT);
    int cap   = 1 + (int)rnd(40);

    size_t need = iris_size(n_in,n_hid,n_out,cap);
    if(!need){ refused++; continue; }               /* shape declined; fine */
    unsigned char *arena = malloc(need);            /* EXACT size, no slack */
    iris *k = iris_init(arena, need, n_in,n_hid,n_out,cap, rnd(100000));
    if(!k){ refused++; free(arena); continue; }
    built++;

    float *in  = malloc(sizeof(float)*(size_t)n_in);   /* exact */
    float *out = malloc(sizeof(float)*(size_t)n_out);  /* exact */

    for(int step=0; step<40; ++step){
      for(int i=0;i<n_in;i++)  in[i]=rndf();
      for(int i=0;i<n_out;i++) out[i]=rndf();
      switch(rnd(9)){
        case 0: iris_record(k,in,out); break;
        case 1: iris_predict(k,in,out); break;        /* may be untrained */
        case 2: iris_train_converge(k, (int)rnd(50), 0,0); break;
        case 3: iris_clear(k); break;
        case 4: iris_get(k,(int)rnd(cap+3),in,out); break;
        case 5: iris_novelty(k,in); break;
        case 6: iris_delete_nearest(k,in); break;
        case 7: iris_loo_error(k,(int)rnd(20)); break;
        case 8: { static unsigned char sm[16384];
                 iris_suggest_smoothing(k, sm, sizeof sm); } break;
      }
    }
    free(in); free(out); free(arena);
  }
  printf("iterations %d | shapes built %d | shapes refused %d\n", iters, built, refused);
  printf("no sanitizer report above this line means no out-of-bounds access was seen.\n");
  return 0;
}
