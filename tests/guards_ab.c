/* guards_ab.c — are the guard rails provably inert on a healthy run?
 *
 * The guards' contract is "report, never mutate": on healthy data they must
 * not change one bit of anything the library computes. That can only be
 * proven by building the core twice -- guards compiled in and compiled out
 * (-DIRIS_NO_GUARDS) -- and comparing what the two builds compute. This
 * program is one half of that comparison; sh build.sh audit builds it both
 * ways and compares the output.
 *
 * It prints one line per guarded region, each an FNV-1a hash (the 32-bit
 * Fowler-Noll-Vo hash) of what that region computed on healthy data:
 * backpropagation for a fixed number of epochs, iris_train to the plateau,
 * the same run in slices, playing, the closed-form trainer, the neighbour
 * samplers, novelty, a save and a load, and the two diagnostics. Every one
 * of these lines must be identical in the two builds.
 *
 * Then it prints lines beginning "control". Identical output from the two
 * builds is also exactly what you would get if -DIRIS_NO_GUARDS reached
 * nothing at all -- rename the macro in the header and the equality still
 * holds -- so each control line is a value a guard MUST change: a
 * not-a-number refused at iris_record's door, an instrument that was never
 * fitted asked to play, and a not-a-number reading played. Every control
 * line must DIFFER between the two builds.
 *
 * Build:  cc -std=c99 -O2 -o build/guards_ab tests/guards_ab.c
 *         cc -std=c99 -O2 -DIRIS_NO_GUARDS -o build/guards_ab_ng tests/guards_ab.c
 */

#include "../iris.h"
/* The demonstrations are computed here; switch contraction off for this
   file's own arithmetic, as iris.h does for its own, so both builds are
   handed the same demonstrations on every compiler. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif
/* <math.h> for NAN only: a not-a-number is written NAN, never 0.0f/0.0f, so
   the float-divide-by-zero sanitizer can run over this file. */
#include <math.h>
#include <stdio.h>

#define NI 2
#define NH 12
#define NO 3
#define CAP 256

static unsigned char arena[IRIS_ARENA(NI, NH, NO, CAP)];
static unsigned char arena2[IRIS_ARENA(NI, NH, NO, CAP)];
static unsigned char file[64 * 1024];
static unsigned char scratch[IRIS_ELM_SCRATCH(NH, NO) + IRIS_ARENA(NI, NH, NO, CAP)];

static void truth(float x, float y, float *o) {
  o[0] = 0.5f + 0.45f * iris_internal_tanh(3.0f * (x - 0.5f));
  o[1] = 0.5f + 0.40f * iris_internal_tanh(2.5f * (y - 0.5f) * (x + 0.3f));
  o[2] = 0.2f + 0.6f  * (x * y);
}

static uint32_t fnv1a(uint32_t h, const void *p, size_t n) {
  const unsigned char *b = (const unsigned char *)p;
  for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
  return h;
}
#define FNV0 2166136261u

static iris *fresh(unsigned char *mem, size_t bytes, uint32_t seed, int n) {
  iris *k = iris_init(mem, bytes, NI, NH, NO, CAP, seed);
  for (int i = 0; k && i < n; ++i) {
    float u = (float)((i * 7919) % 97) / 97.0f;
    float v = (float)((i * 6131) % 89) / 89.0f;
    float in[NI] = { u, v }, out[NO];
    truth(u, v, out);
    iris_record(k, in, out);
  }
  return k;
}

static uint32_t saved(const iris *k) {
  size_t n = iris_save(k, file, sizeof file);
  return fnv1a(FNV0, file, n);
}

/* Play a 25 x 25 grid reaching a little outside the demonstrated square. */
static uint32_t played(iris *k, int how) {
  uint32_t h = FNV0;
  for (int a = -2; a <= 22; ++a) for (int b = -2; b <= 22; ++b) {
    float in[NI] = { (float)a / 20.0f, (float)b / 20.0f }, out[NO] = { 0 };
    int id = 0;
    float nov = 0.0f;
    switch (how) {
      case 0: iris_predict(k, in, out); break;
      case 1: iris_knn_predict(k, in, out, 3); break;
      case 2: id = iris_classify_1nn(k, in, out); break;
      default: nov = iris_novelty(k, in); break;
    }
    h = fnv1a(h, out, sizeof out);
    h = fnv1a(h, &id, sizeof id);
    h = fnv1a(h, &nov, sizeof nov);
  }
  return h;
}

int main(void) {
  iris *k = fresh(arena, sizeof arena, 1234, 20);
  if (!k) return 1;

  /* backpropagation for a fixed 800 epochs: tests/audit.c's golden recipe */
  iris_reseed(k, 1234);
  iris_continue(k, 800);
  printf("continue(800) saved  0x%08X\n", saved(k));
  printf("continue(800) played 0x%08X\n", played(k, 0));

  /* to the plateau, blocking and sliced */
  iris_train(k);
  printf("train saved          0x%08X\n", saved(k));
  { iris *s = fresh(arena2, sizeof arena2, 1234, 20);
    iris_train_begin(s, 0);
    while (iris_train_slice(s, 700)) {}
    printf("sliced saved         0x%08X\n", saved(s)); }

  /* the neighbour samplers and novelty, on the trained instrument */
  printf("knn played           0x%08X\n", played(k, 1));
  printf("1nn played           0x%08X\n", played(k, 2));
  printf("novelty              0x%08X\n", played(k, 3));

  /* a save and a load, then play what was loaded */
  { size_t n = iris_save(k, file, sizeof file);
    iris *r = iris_init(arena2, sizeof arena2, NI, NH, NO, CAP, 9);
    int ok = iris_load(r, file, n);
    printf("loaded played        0x%08X %d\n", played(r, 0), ok); }

  /* the closed-form trainer, then play it */
  { iris *e = fresh(arena2, sizeof arena2, 4242, 50);
    int esc = iris_train_elm(e, 1e-4f, scratch, sizeof scratch);
    printf("elm saved            0x%08X %d\n", saved(e), esc);
    printf("elm played           0x%08X\n", played(e, 0)); }

  /* the two diagnostics */
  { iris *d = fresh(arena2, sizeof arena2, 77, 12);
    float loo = iris_loo_error(d, 300);
    float sug = iris_suggest_smoothing(d, scratch, sizeof scratch);
    uint32_t h = fnv1a(fnv1a(FNV0, &loo, sizeof loo), &sug, sizeof sug);
    printf("loo, suggestion      0x%08X\n", h); }

  /* ---- controls: each value below is one a guard must change ---------- */
  { float bad_in[NI] = { NAN, 0.5f }, bad_out[NO];
    truth(0.5f, 0.5f, bad_out);
    iris_record(k, bad_in, bad_out);
    printf("control record: count %d status %d\n", iris_count(k), (int)iris_get_status(k)); }
  { iris *u = fresh(arena2, sizeof arena2, 5, 8);        /* never fitted */
    float in[NI] = { 0.25f, 0.75f }, out[NO];
    iris_predict(u, in, out);
    printf("control unfitted: %.9g %.9g %.9g status %d\n",
           (double)out[0], (double)out[1], (double)out[2], (int)iris_get_status(u)); }
  { iris *t = fresh(arena2, sizeof arena2, 1234, 20);
    iris_train(t);
    float in[NI] = { NAN, 0.5f }, out[NO];
    iris_predict(t, in, out);
    printf("control not-a-number reading: finite %d %d %d status %d\n",
           out[0] == out[0], out[1] == out[1], out[2] == out[2], (int)iris_get_status(t)); }
  return 0;
}
