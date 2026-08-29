/* guards_ab.c — are the guard rails provably inert on a healthy run?
 *
 * The guards' contract is "report, never mutate": on healthy training they
 * must not change one bit. That can only be proven by building the core
 * twice — guards compiled in vs compiled out (-DIRIS_NO_GUARDS) — and
 * comparing the trained blobs. This program is one half of that A/B;
 * ./build.sh audit builds it both ways and diffs the output.
 *
 * Prints one line: the fnv1a-32 hash of the check-5 recipe's saved blob.
 *
 * Build:  cc -O2 -o guards_ab tests/guards_ab.c -lm
 *         cc -O2 -DIRIS_NO_GUARDS -o guards_ab_ng tests/guards_ab.c -lm
 */

#include "../iris.h"
#include <stdio.h>

#define NI 2
#define NH 12
#define NO 3
#define CAP 256

static unsigned char arena[IRIS_ARENA(NI, NH, NO, CAP)];
static unsigned char file[64 * 1024];

static void truth(float x, float y, float *o) {
  o[0] = 0.5f + 0.45f * iris_tanh(3.0f * (x - 0.5f));
  o[1] = 0.5f + 0.40f * iris_tanh(2.5f * (y - 0.5f) * (x + 0.3f));
  o[2] = 0.2f + 0.6f  * (x * y);
}

static uint32_t fnv1a(const unsigned char *p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
  return h;
}

int main(void) {
  iris *k = iris_init(arena, sizeof arena, NI, NH, NO, CAP, 1234);
  if (!k) return 1;
  for (int i = 0; i < 20; ++i) {
    float u = (float)((i * 7919) % 97) / 97.0f;
    float v = (float)((i * 6131) % 89) / 89.0f;
    float in[NI] = { u, v }, out[NO];
    truth(u, v, out);
    iris_record(k, in, out);
  }
  iris_retrain_new(k, 1234, 800);
  size_t n = iris_save(k, file, sizeof file);
  printf("blob %zu B fnv1a 0x%08X\n", n, fnv1a(file, n));

  /* THE POSITIVE CONTROL.
     Asserting the two builds are EQUAL proves the guards are inert -- and it
     is also exactly what you get if -DIRIS_NO_GUARDS does nothing at all.
     Rename the macro in the header and the equality check still passes, which
     means it cannot detect its own defeat. So print one more line: a value the
     guards MUST change. With guards, a poisoned demonstration is refused and
     the count stays 20; without them it is accepted and the count becomes 21.
     build.sh asserts these two lines DIFFER. */
  { float bad_in[NI], bad_out[NO];
    bad_in[0] = 0.0f / 0.0f; bad_in[1] = 0.5f;
    truth(0.5f, 0.5f, bad_out);
    iris_record(k, bad_in, bad_out);
    printf("poison-probe count %d status %d\n", iris_count(k), (int)iris_get_status(k)); }
  return 0;
}
