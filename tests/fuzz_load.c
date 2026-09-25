/* ============================================================================
   fuzz_load.c — coverage-guided fuzzing of iris_load, past the checksum.

   A libFuzzer harness. Build with Homebrew clang and run from the repository
   root:

     mkdir -p build/fuzz_load_corpus
     /opt/homebrew/opt/llvm/bin/clang -std=c99 -g -O1 -I. \
       -fsanitize=fuzzer,address,undefined,float-divide-by-zero \
       -fno-sanitize-recover=all -o build/fuzz_load tests/fuzz_load.c
     ./build/fuzz_load -max_total_time=600 build/fuzz_load_corpus

   It exits non-zero, with the offending input written to a crash-* file, on
   the first failure. No corpus is committed: the harness makes its own seeds
   at start-up, a few genuine saves of each shape below (never trained,
   trained with smoothing, fitted with every demonstration deleted, full, and
   fitted after one more take), and the mutator hands them out.

   THE INPUT. Byte 0 picks the receiving instrument's shape; the rest is the
   file, copied into a heap block of exactly its size, so a read past it is an
   AddressSanitizer report.

   THE MUTATOR does what random byte edits almost never do on their own: it
   sets n_ex to agree with the length or the length to agree with n_ex,
   plants special floats (not-a-number, infinities, the largest float, just
   past IRIS_W_LIMIT, denormals, -0) in any field, sets header words to edge
   values, collapses or inverts a range, copies one identifier onto another,
   and repairs magic, version, header size and shape so a random blob gets
   past the door. Then it RECOMPUTES THE TRAILING CHECKSUM, fifteen times in
   sixteen: without that almost every mutant dies at the checksum and the run
   measures CRC-32 instead of the loader. The sixteenth keeps the refusal of
   a bad checksum covered.

   WHAT ABORTS, besides any sanitizer report:
     - a refused load that changed any byte of the receiving arena, status
       included;
     - after an accepted load: any rule of the format table (iris.h PART 9)
       broken by the loaded instrument, checked by this file's own copy of
       the rules, not the library's (the checksum and the smoothing field,
       which the loaded instrument does not show, are read from the file);
       velocities, the residual ledger or the training progress not reset; a
       status other than OK; a last_error that is not a finite number;
     - iris_save refusing the loaded instrument; a re-save that differs from
       the file anywhere except the smoothing field and the checksum; save,
       load, save not byte-identical;
     - iris_predict, iris_knn_predict or iris_classify_1nn writing a value
       that is not finite while the status says OK; iris_novelty outside
       [0,1];
     - iris_record handing out anything but the loaded next_id.
   ========================================================================= */
#include "iris.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int ni, nh, no, cap; } shape;
static const shape SH[] = {
  { 2, 12, 3, 16 },
  { 1,  8, 1,  4 },
  { 3,  8, 2,  8 },
  { 4, 16, 4,  4 },
  { 1,  8, 1,  1 },
  { 2,  9, 1, 32 },
};
#define NSHAPES ((int)(sizeof SH / sizeof SH[0]))

size_t LLVMFuzzerMutate(uint8_t *data, size_t size, size_t max_size);

/* ---------------------------------------------------------------- bytes */
static uint32_t rd32(const uint8_t *b, size_t off) {
  return (uint32_t)b[off] | (uint32_t)b[off + 1] << 8
       | (uint32_t)b[off + 2] << 16 | (uint32_t)b[off + 3] << 24;
}
static void wr32(uint8_t *b, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[off + (size_t)i] = (uint8_t)(v >> (8 * i));
}
static void fix_crc(uint8_t *file, size_t n) { if (n >= 4) wr32(file, n - 4, iris_crc32(file, n - 4)); }
static size_t nweights(shape s) { return (size_t)s.nh * s.ni + s.nh + (size_t)s.no * s.nh + s.no; }
static size_t file_bytes(shape s, uint32_t n_ex) {
  return 48 + 4 * (nweights(s) + 2 * (size_t)(s.ni + s.no) + (size_t)n_ex * (size_t)(s.ni + s.no + 1)) + 4;
}
static void fail(const char *why) { fprintf(stderr, "fuzz_load: %s\n", why); abort(); }

/* ----------------------------------------------------------- statistics */
static unsigned long loads = 0, accepted = 0;
static void report(void) {
  fprintf(stderr, "fuzz_load: %lu loads, %lu accepted, %lu refused\n", loads, accepted, loads - accepted);
}

/* ---------------------------------------------------------------- seeds */
#define NSEEDS_MAX 64
static uint8_t *seed_data[NSEEDS_MAX];
static size_t seed_size[NSEEDS_MAX];
static int nseeds = 0;

static void record_some(iris *k, shape s, int n, int salt) {
  float in[IRIS_MAX_IN], out[IRIS_MAX_OUT];
  for (int r = 0; r < n; ++r) {
    for (int i = 0; i < s.ni; ++i) in[i] = (float)((r * 7 + i * 3 + salt) % 11) / 10.0f;
    for (int o = 0; o < s.no; ++o) out[o] = 0.2f + 0.6f * (float)((r * 5 + o + salt) % 9) / 8.0f;
    iris_record(k, in, out);
  }
}
static void add_seed(int sel, iris *k) {
  if (nseeds >= NSEEDS_MAX) return;
  size_t n = iris_save_size(k);
  uint8_t *b = (uint8_t *)malloc(n + 1);
  b[0] = (uint8_t)sel;
  if (iris_save(k, b + 1, n) != n) fail("a seed would not save");
  seed_data[nseeds] = b; seed_size[nseeds] = n + 1; nseeds++;
}
int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void)argc; (void)argv;
  atexit(report);
  for (int si = 0; si < NSHAPES; ++si) {
    const shape s = SH[si];
    const size_t ab = iris_size(s.ni, s.nh, s.no, s.cap);
    unsigned char *a = (unsigned char *)malloc(ab);
    const int some = s.cap < 5 ? s.cap : 5;
    iris *k;
    k = iris_init(a, ab, s.ni, s.nh, s.no, s.cap, 1234u);           /* never trained */
    record_some(k, s, some, 0); add_seed(si, k);
    k = iris_init(a, ab, s.ni, s.nh, s.no, s.cap, 99u);             /* trained, smoothed */
    record_some(k, s, some, 1); iris_set_smoothing(k, 0.3f); iris_train_epochs(k, 200); add_seed(si, k);
    while (iris_count(k)) iris_delete_last(k);                      /* fitted, emptied */
    add_seed(si, k);
    k = iris_init(a, ab, s.ni, s.nh, s.no, s.cap, 7u);              /* full */
    record_some(k, s, s.cap, 2); iris_train_epochs(k, 100); add_seed(si, k);
    if (s.cap > some) {                                             /* fitted, one more take */
      k = iris_init(a, ab, s.ni, s.nh, s.no, s.cap, 5u);
      record_some(k, s, some, 3); iris_train_epochs(k, 100); record_some(k, s, 1, 4); add_seed(si, k);
    }
    free(a);
  }
  return 0;
}

/* ------------------------------------------------ this file's copy of the rules
   Checked against the LOADED instrument, so a rule dropped from the library's
   validator (which iris_save shares) is still caught here. */
static int bad(float x) { return iris_isbad(x); }
static const char *rules_broken(const iris *k) {
  if (k->trained && !k->fitted) return "trained without fitted";
  if (k->seed == 0u) return "seed 0";
  if (k->rng.s == 0u) return "random state 0";
  if (k->next_id < 1 || k->next_id >= 0x7FFFFFFF) return "next_id out of range";
  if (bad(k->l2) || k->l2 < 0.0f || k->l2 > 0.3f) return "smoothing out of range";
  if (k->n_ex < 0 || k->n_ex > k->cap) return "count out of range";
  { const float *arr[4] = { k->w1, k->b1, k->w2, k->b2 };
    const size_t len[4] = { (size_t)k->n_hid * k->n_in, (size_t)k->n_hid,
                            (size_t)k->n_out * k->n_hid, (size_t)k->n_out };
    for (int a = 0; a < 4; ++a)
      for (size_t i = 0; i < len[a]; ++i)
        if (bad(arr[a][i]) || arr[a][i] > IRIS_W_LIMIT || arr[a][i] < -IRIS_W_LIMIT) return "weight"; }
  for (int i = 0; i < k->n_in; ++i)
    if (bad(k->in_lo[i]) || bad(k->in_hi[i]) || !(k->in_lo[i] <= k->in_hi[i]) || bad(k->in_hi[i] - k->in_lo[i])) return "input range";
  for (int o = 0; o < k->n_out; ++o)
    if (bad(k->out_lo[o]) || bad(k->out_hi[o]) || !(k->out_lo[o] < k->out_hi[o]) || bad(k->out_hi[o] - k->out_lo[o])) return "output range";
  for (size_t i = 0; i < (size_t)k->n_ex * (size_t)(k->n_in + k->n_out); ++i) if (bad(k->ex[i])) return "demonstration";
  for (int i = 0; i < k->n_ex; ++i) {
    if (k->ex_id[i] < 1 || k->ex_id[i] >= k->next_id) return "identifier out of range";
    for (int j = 0; j < i; ++j) if (k->ex_id[j] == k->ex_id[i]) return "identifier repeated";
  }
  return 0;
}

static int at_rest(const iris *k) {
  const float *arr[4] = { k->v_w1, k->v_b1, k->v_w2, k->v_b2 };
  const size_t len[4] = { (size_t)k->n_hid * k->n_in, (size_t)k->n_hid,
                          (size_t)k->n_out * k->n_hid, (size_t)k->n_out };
  for (int a = 0; a < 4; ++a)
    for (size_t i = 0; i < len[a]; ++i) if (arr[a][i] != 0.0f) return 0;
  for (int i = 0; i < k->cap; ++i) if (k->ex_res[i] != 0.0f) return 0;
  return k->res_epochs == 0 && k->tr_done == 0 && k->tr_ceiling == 0 && k->tr_running == 0
      && k->tr_n_ex == 0 && k->tr_ref == 0.0f;
}

/* Every output finite, or the status says something is wrong. */
static void finite_or_reported(const iris *k, const float *out, const char *who) {
  for (int o = 0; o < k->n_out; ++o)
    if (bad(out[o]) && iris_get_status(k) == IRIS_STATUS_OK) fail(who);
}

/* ------------------------------------------------------------ the target */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 1) return 0;
  const shape s = SH[data[0] % NSHAPES];
  const size_t n = size - 1;
  uint8_t *file = (uint8_t *)malloc(n ? n : 1);
  if (n) memcpy(file, data + 1, n);

  const size_t ab = iris_size(s.ni, s.nh, s.no, s.cap);
  unsigned char *arena = (unsigned char *)malloc(ab), *before = (unsigned char *)malloc(ab);
  memset(arena, 0x5A, ab);
  iris *k = iris_init(arena, ab, s.ni, s.nh, s.no, s.cap, 7u);
  if (!k) fail("iris_init refused a harness shape");
  record_some(k, s, s.cap < 2 ? s.cap : 2, 6);    /* a receiver with a history */
  iris_train_epochs(k, 2);
  k->status = IRIS_STORE_FULL;                     /* not OK, so a written status shows */
  memcpy(before, arena, ab);

  loads++;
  if (!iris_load(k, file, n)) {
    if (memcmp(before, arena, ab) != 0) fail("a refused load changed the arena");
    free(file); free(arena); free(before);
    return 0;
  }
  accepted++;

  { const char *why = rules_broken(k);
    if (why) { fprintf(stderr, "fuzz_load: accepted a file that breaks a rule: %s\n", why); abort(); } }
  /* Two rules the loaded instrument cannot show: the checksum is not kept,
     and iris_set_smoothing clamps whatever the smoothing field held, or
     ignores it if it is not a number. So these two are read from the file. */
  if (iris_crc32(file, n - 4) != rd32(file, n - 4)) fail("accepted a file with a wrong checksum");
  { union { uint32_t u; float f; } sm; sm.u = rd32(file, 44);
    if (bad(sm.f) || sm.f < 0.0f || sm.f > 1.0f) fail("accepted a file that breaks a rule: smoothing field"); }
  if (!at_rest(k)) fail("velocities, ledger or training progress survived the load");
  if (iris_get_status(k) != IRIS_STATUS_OK) fail("status not OK after a load");
  if (bad(iris_last_error(k)) || iris_last_error(k) < 0.0f) fail("last_error not a finite measurement");
  if ((uint32_t)k->fitted != (rd32(file, 12) & 1u) || (uint32_t)k->trained != ((rd32(file, 12) >> 1) & 1u))
    fail("fitted or trained differs from the flags");

  /* save, load, save */
  { const size_t need = iris_save_size(k);
    uint8_t *s1 = (uint8_t *)malloc(need), *s2 = (uint8_t *)malloc(need);
    if (need != n || iris_save(k, s1, need) != need) fail("iris_save refused a loaded instrument");
    for (size_t i = 0; i < n - 4; ++i)
      if ((i < 44 || i >= 48) && s1[i] != file[i]) fail("re-save differs outside the smoothing field");
    unsigned char *a2 = (unsigned char *)malloc(ab);
    iris *k2 = iris_init(a2, ab, s.ni, s.nh, s.no, s.cap, 9u);
    if (!iris_load(k2, s1, need)) fail("could not reload its own save");
    if (iris_save(k2, s2, need) != need || memcmp(s1, s2, need) != 0) fail("save, load, save not byte-identical");
    free(a2); free(s1); free(s2); }

  /* play it every way there is */
  { static const float P[5][4] = { { 0, 0, 0, 0 }, { 0.5f, 0.5f, 0.5f, 0.5f }, { 1, -1, 1, -1 },
                                   { 1e3f, -1e3f, 2, 3 }, { 3e38f, -3e38f, 1e-40f, -0.0f } };
    float out[IRIS_MAX_OUT];
    for (int p = 0; p < 5; ++p) {
      k->status = IRIS_STATUS_OK; iris_predict(k, P[p], out);          finite_or_reported(k, out, "predict: not finite, status OK");
      k->status = IRIS_STATUS_OK; iris_knn_predict(k, P[p], out, 1 + p); finite_or_reported(k, out, "knn: not finite, status OK");
      k->status = IRIS_STATUS_OK;
      if (iris_classify_1nn(k, P[p], out) >= 0) finite_or_reported(k, out, "1nn: not finite, status OK");
      const float nv = iris_novelty(k, P[p]);
      if (!(nv >= 0.0f && nv <= 1.0f)) fail("novelty outside [0,1]");
    } }

  /* the identifiers the file handed over are the ones the store goes on with */
  if (iris_count(k) < iris_capacity(k)) {
    float in[IRIS_MAX_IN], out[IRIS_MAX_OUT];
    for (int i = 0; i < s.ni; ++i) in[i] = 0.25f;
    for (int o = 0; o < s.no; ++o) out[o] = 0.75f;
    const int32_t expect = k->next_id;
    const int id = iris_record(k, in, out);
    if (id != expect || iris_index_of(k, id) != iris_count(k) - 1) fail("iris_record did not hand out next_id");
    if (!iris_delete_id(k, id)) fail("could not delete the take just recorded");
  }
  free(file); free(arena); free(before);
  return 0;
}

/* ------------------------------------------------------------- mutator */
static uint32_t rs;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs ? rs : (rs = 0x9E3779B9u); }

static size_t structural(uint8_t *d, size_t size, size_t max) {
  if (size < 1 + 52) return size;
  const shape s = SH[d[0] % NSHAPES];
  uint8_t *b = d + 1;
  size_t n = size - 1;
  const size_t base = file_bytes(s, 0), per = 4 * (size_t)(s.ni + s.no + 1);
  const size_t body = 48, ranges = body + 4 * nweights(s), ex = ranges + 8 * (size_t)(s.ni + s.no);
  switch (rnd() % 8) {
    case 0:                                          /* n_ex from the length */
      if (n >= base) wr32(b, 28, (uint32_t)((n - base) / per));
      break;
    case 1: {                                        /* the length from n_ex */
      uint32_t e = rd32(b, 28);
      if (e > (uint32_t)s.cap + 1) { e = rnd() % ((uint32_t)s.cap + 2); wr32(b, 28, e); }
      const size_t want = file_bytes(s, e);
      if (1 + want <= max) { if (want > n) memset(b + n, 0, want - n); n = want; }
      break; }
    case 2: {                                        /* a special float, anywhere past offset 44 */
      static const uint32_t SP[] = { 0x7FC00000u, 0x7F800000u, 0xFF800000u, 0x7F7FFFFFu, 0xFF7FFFFFu,
                                     0x41800000u, 0x41800001u, 0xC1800001u, 0x00000001u, 0x80000000u,
                                     0x3F800001u, 0x3F800000u, 0x7149F2CAu, 0x00000000u };
      if (n < 52) break;
      const size_t slot = 44 + 4 * (rnd() % ((n - 48) / 4));
      wr32(b, slot, SP[rnd() % (sizeof SP / sizeof SP[0])]);
      break; }
    case 3: {                                        /* header words to edge values */
      static const uint32_t EV[] = { 0u, 1u, 2u, 3u, 4u, 0x7FFFFFFEu, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu };
      static const size_t OFF[] = { 12, 28, 32, 36, 40 };
      wr32(b, OFF[rnd() % 5], EV[rnd() % (sizeof EV / sizeof EV[0])]);
      break; }
    case 4:                                          /* past the door */
      b[0] = 'I'; b[1] = 'R'; b[2] = 'I'; b[3] = 'S';
      wr32(b, 4, 7u); wr32(b, 8, 48u);
      wr32(b, 16, (uint32_t)s.ni); wr32(b, 20, (uint32_t)s.nh); wr32(b, 24, (uint32_t)s.no);
      break;
    case 5: {                                        /* collapse or invert a range */
      const int ch = (int)(rnd() % (uint32_t)(s.ni + s.no));
      const size_t lo = ch < s.ni ? ranges + 4 * (size_t)ch : ranges + 8 * (size_t)s.ni + 4 * (size_t)(ch - s.ni);
      const size_t hi = lo + 4 * (size_t)(ch < s.ni ? s.ni : s.no);
      if (hi + 4 > n) break;
      const uint32_t l = rd32(b, lo), h = rd32(b, hi);
      if (rnd() % 2) wr32(b, hi, l); else { wr32(b, lo, h); wr32(b, hi, l); }
      break; }
    case 6: {                                        /* one identifier onto another, or to the edge */
      const uint32_t e = rd32(b, 28);
      if (e == 0 || e > (uint32_t)s.cap) break;
      const size_t ids = ex + 4 * (size_t)e * (size_t)(s.ni + s.no);
      if (ids + 4 * (size_t)e > n) break;
      const size_t i = rnd() % e, j = rnd() % e;
      switch (rnd() % 3) {
        case 0: wr32(b, ids + 4 * i, rd32(b, ids + 4 * j)); break;
        case 1: wr32(b, ids + 4 * i, rd32(b, 36) - 1u); break;
        default: wr32(b, 36, rd32(b, ids + 4 * i)); break;
      }
      break; }
    default: {                                       /* a whole seed, the right shape for this selector */
      const int pick = (int)(rnd() % (uint32_t)nseeds);
      if (seed_size[pick] <= max) { memcpy(d, seed_data[pick], seed_size[pick]); return seed_size[pick]; }
      break; }
  }
  return 1 + n;
}

size_t LLVMFuzzerCustomMutator(uint8_t *d, size_t size, size_t max, unsigned int seed);
size_t LLVMFuzzerCustomMutator(uint8_t *d, size_t size, size_t max, unsigned int seed) {
  rs = seed ? seed : 1u;
  if (nseeds > 0 && (size < 1 + 52 || rnd() % 64 == 0)) {
    const int pick = (int)(rnd() % (uint32_t)nseeds);
    if (seed_size[pick] <= max) { memcpy(d, seed_data[pick], seed_size[pick]); size = seed_size[pick]; }
    if (rnd() % 4 == 0) return size;                 /* sometimes the genuine file, untouched */
  }
  size = LLVMFuzzerMutate(d, size, max);
  if (rnd() % 2) size = structural(d, size, max);
  if (rnd() % 4 == 0) size = structural(d, size, max);
  if (size >= 1 + 52 && rnd() % 16 != 0) fix_crc(d + 1, size - 1);
  return size;
}

size_t LLVMFuzzerCustomCrossOver(const uint8_t *d1, size_t s1, const uint8_t *d2, size_t s2,
                                 uint8_t *out, size_t max, unsigned int seed);
size_t LLVMFuzzerCustomCrossOver(const uint8_t *d1, size_t s1, const uint8_t *d2, size_t s2,
                                 uint8_t *out, size_t max, unsigned int seed) {
  rs = seed ? seed : 1u;
  if (s1 < 1 || s2 < 1) return 0;
  /* the selector and fixed header from one parent, the rest from the other */
  size_t h = s1 < 49 ? s1 : 49;
  size_t rest = s2 > 49 ? s2 - 49 : 0;
  if (h + rest > max) rest = max - h;
  memcpy(out, d1, h);
  memcpy(out + h, d2 + (s2 - rest), rest);
  size_t size = structural(out, h + rest, max);
  if (size >= 1 + 52) fix_crc(out + 1, size - 1);
  return size;
}
