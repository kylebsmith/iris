/* SPDX-License-Identifier: BSD-3-Clause
   Copyright (c) 2026 Kyle Smith */
/* ============================================================================
   starter_recipes.c -- the starter kit's two pinned recipes, on the host.

   Two sketches in the ESP32 starter kit compare what the board computes
   with a hash the host computed for the same recipe, so a board that trains
   or plays differently shows up as a different number:

     device_torture, test 1   iris_init(seed 1234), 20 demonstrations, 800
                              epochs continuing from the weights iris_init
                              drew, then 21 predictions hashed: 0xB7FC47A0
     determinism_check        iris_init(seed 1234), 20 demonstrations,
                              iris_reseed(1234) and 800 epochs, then 21
                              predictions hashed: 0x203834ED

   Both hashes must hold on the host for every change to iris.h, or the
   sketches' reference numbers would be wrong.

   CONTRACTION. The demonstrations are computed here, and device_torture's
   are a*b+c (1 - 0.03 i). iris.h switches fused multiply-add contraction off
   for its own code only, so a compiler whose default contracts would fuse
   this file's arithmetic into different demonstrations, and so a different
   instrument, before iris.h did anything: measured, device_torture's hash is
   0x60E31823 with Apple clang -O2 and with gcc-15 -std=gnu99 when this file
   leaves contraction on. So this file switches it off for its own code, after
   the #include, where iris.h says an includer's own pragma belongs, as
   tests/audit.c does and as the sketches must.

   Build and run from the repository root:

     mkdir -p build && cc -std=c99 -O2 -Wall -Wextra -I. -o build/starter_recipes \
       tests/starter_recipes.c && ./build/starter_recipes

   The exit status is non-zero if either hash differs.
   ========================================================================= */
#include "iris.h"
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif
#include <stdio.h>
#include <string.h>

#define NI 2
#define NH 12
#define NO 3

static int fails = 0;
static void check(const char *name, uint32_t got, uint32_t want) {
  printf("  [%s] %-40s 0x%08lX (want 0x%08lX)\n", got == want ? "PASS" : "FAIL", name,
         (unsigned long)got, (unsigned long)want);
  if (got != want) fails++;
}

/* The predictions' bytes, in memory order, through 32-bit FNV-1a: the hash
   device_torture computes. */
static uint32_t fnv_bytes(uint32_t h, const float *v, int n) {
  for (int o = 0; o < n; ++o) {
    unsigned char b[sizeof(float)];
    memcpy(b, &v[o], sizeof(float));
    for (unsigned j = 0; j < sizeof(float); ++j) { h ^= b[j]; h *= 16777619u; }
  }
  return h;
}

/* device_torture, test 1 */
static unsigned char arena20[IRIS_ARENA(NI, NH, NO, 20)];
static uint32_t device_torture(void) {
  iris *k = iris_init(arena20, sizeof arena20, NI, NH, NO, 20, 1234u);
  if (!k) return 0u;
  for (int i = 0; i < 20; ++i) {
    float in[NI], out[NO];
    in[0] = (float)i * 0.05f;
    in[1] = 1.0f - (float)i * 0.03f;
    out[0] = (float)((i * 7) % 11) / 11.0f;
    out[1] = (float)((i * 3) % 5) / 5.0f;
    out[2] = (float)((i * 5) % 7) / 7.0f;
    iris_record(k, in, out);
  }
  iris_train_epochs(k, 800);
  uint32_t h = 2166136261u;
  for (int i = 0; i <= 20; ++i) {
    float in[NI], out[NO];
    in[0] = (float)i * 0.05f;
    in[1] = 1.0f - (float)i * 0.03f;
    iris_predict(k, in, out);
    h = fnv_bytes(h, out, NO);
  }
  return h;
}

/* determinism_check. It hashes each prediction's bit pattern least
   significant byte first, by shifting, so its hash does not depend on the
   machine's byte order. */
static unsigned char arena64[IRIS_ARENA(NI, NH, NO, 64)];
static uint32_t determinism_check(void) {
  iris *k = iris_init(arena64, sizeof arena64, NI, NH, NO, 64, 1234u);
  if (!k) return 0u;
  for (int i = 0; i < 20; ++i) {
    const float u = (float)((i * 7919) % 97) / 97.0f;
    const float v = (float)((i * 6131) % 89) / 89.0f;
    float in[NI] = { u, v };
    float out[NO] = { 0.25f + 0.5f * u, 0.25f + 0.5f * v, 0.25f + 0.5f * (u * v) };
    iris_record(k, in, out);
  }
  iris_reseed(k, 1234u);
  iris_train_epochs(k, 800);
  uint32_t h = 2166136261u;
  for (int a = 0; a <= 20; ++a) {
    float in[NI] = { (float)a / 20.0f, 0.5f }, out[NO];
    iris_predict(k, in, out);
    for (int q = 0; q < NO; ++q) {
      uint32_t b;
      memcpy(&b, &out[q], sizeof b);
      for (int y = 0; y < 4; ++y) { h ^= (b >> (8 * y)) & 0xFFu; h *= 16777619u; }
    }
  }
  return h;
}

int main(void) {
  printf("\n  THE STARTER KIT'S PINNED RECIPES\n\n");
  check("device_torture, test 1", device_torture(), 0xB7FC47A0u);
  check("determinism_check", determinism_check(), 0x203834EDu);
  printf(fails ? "\n  %d FAILING\n\n" : "\n  all pass\n\n", fails);
  return fails ? 1 : 0;
}
