/* SPDX-License-Identifier: BSD-3-Clause
   Copyright (c) 2026 Kyle Smith */
/* ============================================================================
   export.c -- what iris computed, written out for a second implementation to
   check.

   tests/reference/reference.py is a model of iris written again, in NumPy's
   64-bit floating point, from the description of the model rather than from
   this library's code. This program is the other half of that comparison: it
   runs one recipe through iris and prints, as JSON (JavaScript Object
   Notation, a plain-text data format), every number the reference needs to
   replay the recipe and every number it will compare.

   It uses the public interface to do everything, and reads the instrument's
   structure (never writes it) to see what the interface does not report: the
   weights, the momentum velocities, the fitted ranges and the shuffle order
   each epoch used. Reading the structure is something a program using the
   library should not do; a test that exists to look inside may.

   THE RECIPE comes in on standard input as whitespace-separated tokens, with
   every floating-point number in C99 hexadecimal form (0x1.8p-1), so that it
   arrives as exactly the 32-bit value the generator meant:

     n_in n_hid n_out seed smoothing
     train              0 = do not train (the neighbour checks play an
                        unfitted instrument); 1 = iris_train_begin(k, ceiling)
                        then one-epoch slices until the run ends
     ceiling            the ceiling handed to iris_train_begin; 0 is
                        iris_train's own
     dump               1 = print the state after every epoch; 0 = only the
                        end of the run (the task-level comparison)
     warm               how many iris_continue(k, 1) calls to make after the
                        run, each printed like an epoch
     n_demos, then n_demos rows of n_in inputs and n_out outputs
     n_probes, then n_probes rows of n_in inputs
     n_k, then n_k neighbour counts for iris_knn_predict

   ONE-EPOCH SLICES ARE iris_train. iris_train_begin checks and reseeds
   exactly as iris_train does, and the shuffle buffer and the plateau
   reference carry across slices, so running the session one epoch at a time
   gives the blocking call's bits (the header's own claim, which this program
   re-checks on every recipe: it runs the same recipe through the blocking
   call on a second instrument and prints whether the two saved files are
   byte-identical). Between slices it prints order[], which then holds the
   permutation the epoch just used, because the shuffle permutes it in place
   at the top of each epoch.

   THE WARM EPOCHS are iris_continue(k, 1), one call each. Each call is a
   session of its own: it starts the shuffle again from the identity order,
   keeps the momentum velocities and the random state, and runs one epoch.
   So n such calls are not iris_continue(k, n): on the golden recipe, 800
   one-epoch calls end with weights up to 0.018 away from one 800-epoch call
   (the same random draws, permuting a different starting order).

   THE OUTPUT. Arrays of weights, velocities and demonstrations are printed
   as one hexadecimal string each, eight digits per 32-bit float giving its
   bit pattern, most significant digit first, so nothing is lost to decimal
   rounding and a 22,000-epoch run stays a file of 29 megabytes rather than
   hundreds. Weights are w1, b1, w2, b2 in the order the header stores
   them (w1[h*n_in + i], b1[h], w2[o*n_hid + h], b2[o]); velocities the same.

   THE HASH. For every trained recipe it prints the 32-bit Fowler-Noll-Vo
   hash (fnv1a) of the saved instrument's bytes, taken exactly as tests/audit.c takes its golden
   hash, so the reference's golden recipe can be shown to be the instrument
   that hash pins (0x6805FB0D).

   Build and run by tests/reference/run.py; by hand, from the repository root:

     cc -std=c99 -O2 -Wall -Wextra -o build/export tests/reference/export.c
     ./build/export < recipe.txt > run.json
   ========================================================================= */
#include "../../iris.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAP_MAX 1024
#define PROBE_MAX 8192

static unsigned char arena_a[IRIS_ARENA(IRIS_MAX_IN, IRIS_MAX_HID, IRIS_MAX_OUT, CAP_MAX)];
static unsigned char arena_b[IRIS_ARENA(IRIS_MAX_IN, IRIS_MAX_HID, IRIS_MAX_OUT, CAP_MAX)];
static unsigned char file_a[1 << 20], file_b[1 << 20];
static float demos[CAP_MAX * (IRIS_MAX_IN + IRIS_MAX_OUT)];
static float probes[PROBE_MAX * IRIS_MAX_IN];
static float scratch[PROBE_MAX * IRIS_MAX_OUT];

static void fail(const char *why) {
  fprintf(stderr, "export: %s\n", why);
  exit(2);
}

static const char *next_token(void) {
  static char tok[128];
  if (scanf("%127s", tok) != 1) fail("the recipe ended early");
  return tok;
}
static long next_int(void) {
  char *end;
  const char *t = next_token();
  long v = strtol(t, &end, 10);
  if (*end) fail("expected a whole number in the recipe");
  return v;
}
static float next_float(void) {
  char *end;
  const char *t = next_token();
  float v = strtof(t, &end);
  if (*end) fail("expected a number in the recipe");
  return v;
}

static uint32_t bits(float f) {
  uint32_t u;
  memcpy(&u, &f, sizeof u);
  return u;
}
static void hex_array(const float *p, int n) {
  putchar('"');
  for (int i = 0; i < n; ++i) printf("%08x", (unsigned)bits(p[i]));
  putchar('"');
}

/* The weights, biases and velocities, each block read through its own
   pointer rather than assuming how the arena lays them out. */
static void print_state(const iris *k) {
  const int nh = k->n_hid, ni = k->n_in, no = k->n_out;
  printf("\"w\": \"");
  const float *blk[4] = { k->w1, k->b1, k->w2, k->b2 };
  const float *vel[4] = { k->v_w1, k->v_b1, k->v_w2, k->v_b2 };
  const int len[4] = { nh * ni, nh, no * nh, no };
  for (int b = 0; b < 4; ++b)
    for (int i = 0; i < len[b]; ++i) printf("%08x", (unsigned)bits(blk[b][i]));
  printf("\", \"v\": \"");
  for (int b = 0; b < 4; ++b)
    for (int i = 0; i < len[b]; ++i) printf("%08x", (unsigned)bits(vel[b][i]));
  printf("\"");
}

static void print_epoch(const iris *k, int first) {
  printf("%s\n  {\"order\": [", first ? "" : ",");
  for (int i = 0; i < k->n_ex; ++i) printf("%s%d", i ? "," : "", (int)k->order[i]);
  printf("], \"err\": \"%08x\", \"status\": %d, ", (unsigned)bits(k->last_error),
         (int)iris_get_status(k));
  print_state(k);
  printf("}");
}

/* tests/audit.c's instrument_bytes: the saved file without its plumbing
   (magic, version, header size, flags, random state, smoothing, checksum),
   behind the eight fixed bytes that hash was first taken with. */
static uint32_t instrument_hash(unsigned char *buf, size_t n) {
  static const unsigned char prefix[8] = { 'E', 'W', 'E', 'K', 1, 0, 0, 0 };
  uint32_t h = 2166136261u;
  if (n < 52) return 0;
  memmove(buf + 8, buf + 16, 24);
  memmove(buf + 32, buf + 48, n - 52);
  memcpy(buf, prefix, sizeof prefix);
  for (size_t i = 0; i < 32 + (n - 52); ++i) { h ^= buf[i]; h *= 16777619u; }
  return h;
}

int main(void) {
  const int ni = (int)next_int(), nh = (int)next_int(), no = (int)next_int();
  const uint32_t seed = (uint32_t)next_int();
  const float smoothing = next_float();
  const int train = (int)next_int(), ceiling = (int)next_int();
  const int dump = (int)next_int(), warm = (int)next_int();
  const int n_demos = (int)next_int();
  if (ni < 1 || ni > IRIS_MAX_IN || no < 1 || no > IRIS_MAX_OUT
      || nh < 8 || nh > IRIS_MAX_HID) fail("shape out of range");
  if (n_demos < 1 || n_demos > CAP_MAX) fail("demonstration count out of range");
  const int stride = ni + no;
  for (int i = 0; i < n_demos * stride; ++i) demos[i] = next_float();
  const int n_probes = (int)next_int();
  if (n_probes < 0 || n_probes > PROBE_MAX) fail("probe count out of range");
  for (int i = 0; i < n_probes * ni; ++i) probes[i] = next_float();
  const int n_k = (int)next_int();
  int ks[16];
  if (n_k < 0 || n_k > 16) fail("too many neighbour counts");
  for (int i = 0; i < n_k; ++i) ks[i] = (int)next_int();

  iris *k = iris_init(arena_a, sizeof arena_a, ni, nh, no, n_demos, seed);
  iris *kb = iris_init(arena_b, sizeof arena_b, ni, nh, no, n_demos, seed);
  if (!k || !kb) fail("iris_init refused the shape");
  iris_set_smoothing(k, smoothing);
  iris_set_smoothing(kb, smoothing);
  for (int r = 0; r < n_demos; ++r) {
    if (!iris_record(k, demos + r * stride, demos + r * stride + ni)
        || !iris_record(kb, demos + r * stride, demos + r * stride + ni))
      fail("iris_record refused a demonstration");
  }

  printf("{\"shape\": [%d, %d, %d], \"seed\": %u, \"n_demos\": %d,\n", ni, nh, no,
         (unsigned)seed, n_demos);
  printf("\"lr\": \"%08x\", \"momentum\": \"%08x\", \"l2\": \"%08x\",\n",
         (unsigned)bits(k->lr), (unsigned)bits(k->momentum), (unsigned)bits(k->l2));
  printf("\"demos\": ");
  hex_array(demos, n_demos * stride);
  printf(",\n");

  if (train) {
    if (!iris_train_begin(k, ceiling)) fail("iris_train_begin refused");
    printf("\"ceiling\": %d, \"rng_after_reseed\": %u,\n\"initial\": {",
           (int)k->tr_ceiling, (unsigned)k->rng.s);
    print_state(k);
    printf("},\n\"epochs\": [");
    int first = 1, more = 1;
    while (more) {
      const int before = iris_train_epochs_done(k);
      more = iris_train_slice(k, 1);
      if (iris_train_epochs_done(k) == before) break;
      if (dump) { print_epoch(k, first); first = 0; }
    }
    printf("\n],\n");

    /* the same recipe through the blocking call, on a second instrument */
    if (ceiling > 0) {
      iris_reseed(kb, seed);
      iris_continue_to_plateau(kb, ceiling, 0, 0);
    } else {
      iris_train(kb);
    }
    size_t na = iris_save(k, file_a, sizeof file_a);
    size_t nb = iris_save(kb, file_b, sizeof file_b);
    const int same = na > 0 && na == nb && memcmp(file_a, file_b, na) == 0;
    printf("\"end\": {\"epochs_done\": %d, \"status\": %d, \"trained\": %d, "
           "\"last_error\": \"%08x\", \"blocking_identical\": %s, ",
           iris_train_epochs_done(k), (int)iris_get_status(k), iris_is_trained(k),
           (unsigned)bits(iris_last_error(k)), same ? "true" : "false");
    printf("\"hash\": \"0x%08X\", ", (unsigned)instrument_hash(file_a, na));
    print_state(k);
    printf("},\n");

    printf("\"warm\": [");
    for (int w = 0; w < warm; ++w) {
      if (iris_continue(k, 1) < 0.0f) fail("iris_continue refused");
      print_epoch(k, w == 0);
    }
    printf("\n],\n\"final\": {");
    print_state(k);
    printf("},\n");
  }

  /* The ranges the instrument holds now: fitted by the run, or, on an
     untrained instrument, fitted by the first neighbour call below (so they
     are printed after it). */
  if (n_probes > 0 && train) {
    for (int p = 0; p < n_probes; ++p) iris_predict(k, probes + p * ni, scratch + p * no);
    printf("\"predict\": ");
    hex_array(scratch, n_probes * no);
    printf(",\n");
  }
  printf("\"probes\": ");
  hex_array(probes, n_probes * ni);
  printf(",\n\"knn\": {");
  for (int j = 0; j < n_k; ++j) {
    for (int p = 0; p < n_probes; ++p)
      iris_knn_predict(k, probes + p * ni, scratch + p * no, ks[j]);
    printf("%s\"%d\": ", j ? ", " : "", ks[j]);
    hex_array(scratch, n_probes * no);
  }
  printf("},\n\"nn1_id\": [");
  for (int p = 0; p < n_probes; ++p)
    printf("%s%d", p ? "," : "", iris_classify_1nn(k, probes + p * ni, scratch + p * no));
  printf("],\n\"nn1_out\": ");
  hex_array(scratch, n_probes * no);
  printf(",\n\"final_status\": %d, \"fitted\": %d,\n", (int)iris_get_status(k), k->fitted);
  printf("\"ranges\": {\"in_lo\": ");
  hex_array(k->in_lo, ni);
  printf(", \"in_hi\": ");
  hex_array(k->in_hi, ni);
  printf(", \"out_lo\": ");
  hex_array(k->out_lo, no);
  printf(", \"out_hi\": ");
  hex_array(k->out_hi, no);
  printf("}}\n");
  return 0;
}
