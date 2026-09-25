/* sqrt_exhaustive.c -- iris_internal_sqrt against the host's square root,
   for every one of the 2^32 float bit patterns.

   Build and run from the repository root (about a minute on an Apple M4):
       mkdir -p build && cc -O2 -o build/sqrt_exhaustive tools/sqrt_exhaustive.c -lm \
         && ./build/sqrt_exhaustive
   The exit status is non-zero if any input disagrees.

   What "agrees" means. IEEE 754, the floating-point standard, requires a
   square root to be correctly rounded, so the host's sqrtf is the
   reference, bit for bit, for:
     - every pattern with the sign bit clear: +0, every subnormal, every
       normal number, +infinity, and every NaN (not-a-number) with the sign
       bit clear;
     - -0, whose root is -0;
     - every NaN with the sign bit set (both return the input NaN, quieted).
   A negative number has no real root and IEEE 754 asks only for a NaN, which
   processors spell differently (64-bit ARM returns 0x7FC00000, x86 returns
   0xFFC00000), so for those patterns the check is that iris_internal_sqrt
   returns a NaN.

   tests/portability.c runs a ten-million-input sample of this on every
   build; this program is the proof that the sample stands for the whole. */
#include "../iris.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint32_t bits_of(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }
static float float_of(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }

int main(void) {
  unsigned long long checked = 0, exact = 0, nan_only = 0, bad = 0;
  const clock_t start = clock();
  uint32_t u = 0;
  do {
    /* volatile: the reference must be computed at run time, on this input */
    volatile float x = float_of(u);
    const uint32_t hw = bits_of(sqrtf(x));
    const uint32_t got = bits_of(iris_internal_sqrt(x));
    const int negative_number = (u & 0x80000000u) && u != 0x80000000u
                             && (u & 0x7FFFFFFFu) <= 0x7F800000u;
    int agrees;
    if (negative_number) {
      agrees = (got & 0x7FFFFFFFu) > 0x7F800000u;
      ++nan_only;
    } else {
      agrees = got == hw;
      ++exact;
    }
    if (!agrees) {
      if (bad < 10)
        printf("  input 0x%08X: host 0x%08X, iris_internal_sqrt 0x%08X\n",
               (unsigned)u, (unsigned)hw, (unsigned)got);
      ++bad;
    }
    ++checked;
    if ((u & 0x0FFFFFFFu) == 0x0FFFFFFFu) {
      printf("  ... 0x%08X done\n", (unsigned)u);
      fflush(stdout);
    }
  } while (++u != 0u);
  printf("%llu inputs: %llu compared bit for bit, %llu negative numbers "
         "checked for NaN; %llu disagree. %.1f s\n",
         checked, exact, nan_only, bad,
         (double)(clock() - start) / (double)CLOCKS_PER_SEC);
  printf(bad ? "SQUARE ROOT DISAGREES\n" : "iris_internal_sqrt is the host's square root, everywhere\n");
  return bad != 0;
}
