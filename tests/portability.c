/* portability.c -- the parts of iris.h that stand in for the C library give
   the C library's answers.

   BUILD AND RUN (from the repository root):
       mkdir -p build && cc -std=c99 -O2 -Wall -Wextra -o build/portability \
         tests/portability.c -lm && ./build/portability
   The exit status is non-zero if any check fails.

   iris_sqrt is integer arithmetic (PART 1 of iris.h explains the method) so
   that no build needs the C library's sqrtf. It must still be the square
   root every processor computes, bit for bit, or an instrument would
   change when its square roots moved into the header. The reference is the
   host's sqrtf, which IEEE 754 requires to be correctly rounded.
   tools/sqrt_exhaustive.c compares all 2^32 inputs and takes about a minute;
   this is the sample that runs every time:
     - ten million non-negative bit patterns from a fixed seed;
     - every power of two, subnormal ones included;
     - the edges of every range: the subnormals, the normals, 1.0, the
       largest finite float;
     - every perfect square up to 4096 squared, whose root is exact;
     - the arguments iris.h itself passes (the layer widths it scales by);
     - the IEEE 754 special cases: -0, negative numbers, NaN and infinity.
   Infinity and NaN are built with __builtin_inff and __builtin_nanf, never
   by dividing by zero, so -fsanitize=float-divide-by-zero can watch this
   file too. */
#include "../iris.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
static void check(const char *name, int pass, const char *detail) {
  printf("  %s  %-54s %s\n", pass ? "PASS" : "FAIL", name, detail);
  if (!pass) failures++;
}

static uint32_t bits_of(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }
static float float_of(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }
static int is_nan_bits(uint32_t u) { return (u & 0x7FFFFFFFu) > 0x7F800000u; }

/* iris_sqrt against the host for one input; counts and remembers the first
   disagreement so the report can name it */
static unsigned long long compared, wrong;
static uint32_t first_wrong;
static void against_host(uint32_t u) {
  volatile float x = float_of(u);   /* computed at run time, on this input */
  const uint32_t hw = bits_of(sqrtf(x)), got = bits_of(iris_sqrt(x));
  if (hw != got && wrong++ == 0) first_wrong = u;
  compared++;
}
static void report(const char *name) {
  char detail[96];
  if (wrong)
    snprintf(detail, sizeof detail, "%llu of %llu differ, first at input 0x%08X",
             wrong, compared, (unsigned)first_wrong);
  else
    snprintf(detail, sizeof detail, "%llu inputs, all bit-identical", compared);
  check(name, wrong == 0, detail);
  compared = wrong = 0;
}

int main(void) {
  printf("iris_sqrt is the host's correctly rounded square root\n");

  { /* ten million seeded bit patterns, sign bit clear: every exponent, every
       subnormal range and the positive NaNs, in proportion to their count */
    uint32_t s = 0x2545F491u;
    for (long i = 0; i < 10000000L; ++i) {
      s ^= s << 13; s ^= s >> 17; s ^= s << 5;
      against_host(s & 0x7FFFFFFFu);
    }
    report("ten million non-negative inputs from a fixed seed");
  }

  { /* every power of two from the smallest subnormal to the largest normal:
       even powers have exact roots, odd powers round */
    for (uint32_t b = 0; b < 23; ++b) against_host(1u << b);            /* subnormals */
    for (uint32_t e = 1; e < 255; ++e) against_host(e << 23);           /* normals */
    report("every power of two, 2^-149 to 2^127");
  }

  { /* the edges of each range, and their neighbours */
    static const uint32_t edge[] = {
      0x00000001u, 0x00000002u, 0x00000003u, 0x003FFFFFu, 0x00400000u,
      0x007FFFFEu, 0x007FFFFFu, 0x00800000u, 0x00800001u, 0x00FFFFFFu,
      0x01000000u, 0x3F7FFFFEu, 0x3F7FFFFFu, 0x3F800000u, 0x3F800001u,
      0x3FFFFFFFu, 0x40000000u, 0x407FFFFFu, 0x40800000u, 0x7F000000u,
      0x7F7FFFFEu, 0x7F7FFFFFu };
    for (size_t i = 0; i < sizeof edge / sizeof edge[0]; ++i) against_host(edge[i]);
    for (uint32_t u = 0x00000000u; u < 0x00010000u; ++u) against_host(u); /* the smallest subnormals */
    for (uint32_t u = 0x3F7F0000u; u < 0x3F810000u; ++u) against_host(u); /* either side of 1.0 */
    for (uint32_t u = 0x7F7F0000u; u < 0x7F800000u; ++u) against_host(u); /* up to the largest float */
    report("range edges: subnormal, normal, 1.0, largest");
  }

  { /* perfect squares: the remainder comes out zero, the root must be exact */
    int exact = 1;
    for (int n = 1; n <= 4096; ++n) {
      const float r = iris_sqrt((float)(n * n));
      if (r != (float)n) exact = 0;
      against_host(bits_of((float)(n * n)));
    }
    check("sqrt(n*n) == n for every n up to 4096", exact, "");
    report("perfect squares against the host");
  }

  { /* the arguments iris.h passes: layer widths, for 1/sqrt(n) and 2/sqrt(n) */
    int same = 1;
    for (int n = 1; n <= 4096; ++n) {
      volatile float x = (float)n;
      if (bits_of(1.0f / iris_sqrt(x)) != bits_of(1.0f / sqrtf(x))) same = 0;
      if (bits_of(2.0f / iris_sqrt(x)) != bits_of(2.0f / sqrtf(x))) same = 0;
      against_host(bits_of(x));
    }
    check("1/sqrt(n) and 2/sqrt(n), n = 1..4096, as the host", same, "");
    report("layer widths against the host");
  }

  { /* the IEEE 754 special cases */
    const float inf = __builtin_inff(), qnan = __builtin_nanf("");
    char detail[96];
    uint32_t r;

    r = bits_of(iris_sqrt(0.0f));
    snprintf(detail, sizeof detail, "0x%08X", (unsigned)r);
    check("sqrt(+0) = +0", r == 0x00000000u, detail);

    r = bits_of(iris_sqrt(-0.0f));
    snprintf(detail, sizeof detail, "0x%08X", (unsigned)r);
    check("sqrt(-0) = -0", r == 0x80000000u, detail);

    r = bits_of(iris_sqrt(inf));
    snprintf(detail, sizeof detail, "0x%08X", (unsigned)r);
    check("sqrt(+infinity) = +infinity", r == 0x7F800000u, detail);

    r = bits_of(iris_sqrt(-inf));
    snprintf(detail, sizeof detail, "0x%08X", (unsigned)r);
    check("sqrt(-infinity) is NaN", is_nan_bits(r), detail);

    {
      static const float neg[] = { -1.0f, -4.0f, -1e-45f, -3.4028235e38f, -0.5f };
      int all = 1;
      for (size_t i = 0; i < sizeof neg / sizeof neg[0]; ++i)
        if (!is_nan_bits(bits_of(iris_sqrt(neg[i])))) all = 0;
      check("sqrt(negative number) is NaN", all, "-1, -4, -1e-45, -largest, -0.5");
    }

    r = bits_of(iris_sqrt(qnan));
    snprintf(detail, sizeof detail, "0x%08X from 0x%08X", (unsigned)r, (unsigned)bits_of(qnan));
    check("sqrt(quiet NaN) is that NaN", r == bits_of(qnan), detail);

    { /* a signalling NaN comes back quieted, sign and payload kept, which is
         what the processor's own square root does */
      static const uint32_t in[] = { 0x7F800001u, 0x7FA00000u, 0xFF800001u, 0xFFC00123u };
      int all = 1;
      for (size_t i = 0; i < sizeof in / sizeof in[0]; ++i) {
        if (bits_of(iris_sqrt(float_of(in[i]))) != (in[i] | 0x00400000u)) all = 0;
        against_host(in[i]);
      }
      check("sqrt(NaN) returns it quieted, payload kept", all,
            "0x7F800001 0x7FA00000 0xFF800001 0xFFC00123");
      report("NaN inputs against the host");
    }
  }

  printf(failures ? "SOME CHECKS FAILED  (%d failed)\n" : "ALL CHECKS PASSED  (%d failed)\n",
         failures);
  return failures != 0;
}
