/* ============================================================================
   load.c — the save format, attacked as a parser.

   iris_load is the one function in the library that reads bytes somebody
   else chose. This file holds it to the format table in iris.h (PART 9):

     - round trips: a saved and reloaded instrument plays the same bits and
       saves the same bytes, for several shapes including 1-in/1-out and the
       maxima; an instrument that recorded a take after training still plays
       after a save and a load;
     - every truncation and every single-bit flip is refused;
     - for every rule in the table, a file that breaks that rule and no other,
       with its checksum recomputed so the checksum cannot be what refuses it,
       is refused -- and every byte of the receiving arena, its status
       included, is the same afterwards. Boundary values that obey the rules
       are loaded, so no rule can pass by refusing everything;
     - a file with the largest next_id leaves one identifier to hand out,
       and the record after it refuses instead of overflowing;
     - a buffer at any alignment works;
     - a translation unit that shrank IRIS_MAX_IN refuses an instrument too
       big for it instead of overflowing its own stack;
     - the committed fixture tests/golden/v7-instrument.bin loads and plays
       every prediction in tests/golden/v7-expected.txt to the bit.

   Build and run from the repository root (the fixture is read from
   tests/golden/; pass another directory as the first argument). The file is
   compiled twice: once as the translation unit with smaller maxima, once as
   the test itself.

     mkdir -p build
     cc -std=c99 -O2 -Wall -Wextra -DLOAD_SMALL_UNIT -c tests/load.c -o build/load_small.o
     cc -std=c99 -O2 -Wall -Wextra -c tests/load.c -o build/load_main.o
     cc build/load_main.o build/load_small.o -o build/load && ./build/load

   Under the sanitizers, add to all three lines:
     -g -fsanitize=address,undefined,alignment,float-divide-by-zero -fno-sanitize-recover=all

   Every arena, and every file handed to iris_load except the offset buffers
   of the alignment check, is a heap block of exactly its size, so a read or
   write past either is an AddressSanitizer report. Exits 1 if any check
   fails.
   ========================================================================= */
#ifdef LOAD_SMALL_UNIT

/* The unit that shrank its maxima, as iris.h documents for small boards. Its
   copy of iris_load has a working array of IRIS_MAX_IN = 4 floats. */
#define IRIS_MAX_IN  4
#define IRIS_MAX_OUT 4
#define IRIS_MAX_HID 12
#include "../iris.h"
int load_in_small_unit(iris *k, const void *file, size_t n);
int load_in_small_unit(iris *k, const void *file, size_t n) { return iris_load(k, file, n); }

#else

#include "../iris.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int load_in_small_unit(iris *k, const void *file, size_t n);

static int fails = 0, checks = 0;
static void check(const char *name, int ok, const char *detail) {
  printf("  [%s] %-58s %s\n", ok ? "PASS" : "FAIL", name, detail);
  checks++;
  if (!ok) fails++;
}

/* ---------------------------------------------------------------- bytes
   The test reads and writes the file with its own little-endian helpers, not
   the library's, so a mistake in one is not repeated in the other. */
static uint32_t rd32(const unsigned char *b, size_t off) {
  return (uint32_t)b[off] | (uint32_t)b[off + 1] << 8
       | (uint32_t)b[off + 2] << 16 | (uint32_t)b[off + 3] << 24;
}
static void wr32(unsigned char *b, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[off + (size_t)i] = (unsigned char)(v >> (8 * i));
}
static uint32_t fbits(float f) { union { float f; uint32_t u; } c; c.f = f; return c.u; }
static float bitsf(uint32_t u) { union { float f; uint32_t u; } c; c.u = u; return c.f; }
static void wrf(unsigned char *b, size_t off, float f) { wr32(b, off, fbits(f)); }
static void fixcrc(unsigned char *b, size_t n) { wr32(b, n - 4, iris_internal_crc32(b, n - 4)); }
static void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) { printf("out of memory\n"); exit(2); }
  return p;
}

/* Where each field of the table sits, computed here from the table itself. */
typedef struct { int ni, nh, no, cap; } shape;
typedef struct { size_t w1, b1, w2, b2, in_lo, in_hi, out_lo, out_hi, ex, ids, crc, total; } layout;
static layout lay(shape s, size_t n_ex) {
  layout L; size_t o = 48;
  L.w1 = o;     o += 4 * (size_t)s.nh * (size_t)s.ni;
  L.b1 = o;     o += 4 * (size_t)s.nh;
  L.w2 = o;     o += 4 * (size_t)s.no * (size_t)s.nh;
  L.b2 = o;     o += 4 * (size_t)s.no;
  L.in_lo = o;  o += 4 * (size_t)s.ni;
  L.in_hi = o;  o += 4 * (size_t)s.ni;
  L.out_lo = o; o += 4 * (size_t)s.no;
  L.out_hi = o; o += 4 * (size_t)s.no;
  L.ex = o;     o += 4 * n_ex * (size_t)(s.ni + s.no);
  L.ids = o;    o += 4 * n_ex;
  L.crc = o;    L.total = o + 4;
  return L;
}

/* ----------------------------------------------------------- instruments */
typedef struct { unsigned char *mem; size_t bytes; iris *k; } box;
static box make(shape s, uint32_t seed) {
  box b;
  b.bytes = iris_size(s.ni, s.nh, s.no, s.cap);
  b.mem = (unsigned char *)xmalloc(b.bytes);
  memset(b.mem, 0xA5, b.bytes);                /* a dirty arena, not a zeroed one */
  b.k = iris_init(b.mem, b.bytes, s.ni, s.nh, s.no, s.cap, seed);
  if (!b.k) { printf("iris_init refused a test shape\n"); exit(2); }
  return b;
}
static void drop(box *b) { free(b->mem); b->mem = 0; b->k = 0; }

static void demos(iris *k, shape s, int n, int salt) {
  float in[IRIS_MAX_IN], out[IRIS_MAX_OUT];
  for (int r = 0; r < n; ++r) {
    for (int i = 0; i < s.ni; ++i) in[i] = (float)((r * 7 + i * 3 + salt) % 11) / 10.0f - 0.3f;
    for (int o = 0; o < s.no; ++o) out[o] = 0.2f + 0.6f * (float)((r * 5 + o + salt) % 9) / 8.0f;
    iris_record(k, in, out);
  }
}

#define NPROBE 12
static void probe_in(shape s, int p, float *in) {
  for (int i = 0; i < s.ni; ++i) in[i] = -0.5f + 0.2f * (float)((p * 3 + i * 5) % 11);
}
/* Twelve predictions, including gestures outside the demonstrated range. */
static void play(iris *k, shape s, float out[NPROBE][IRIS_MAX_OUT]) {
  float in[IRIS_MAX_IN] = { 0 };
  memset(out, 0, sizeof(float) * NPROBE * IRIS_MAX_OUT);
  for (int p = 0; p < NPROBE; ++p) { probe_in(s, p, in); iris_predict(k, in, out[p]); }
}

/* A save into an exactly sized heap block, of an instrument the test built
   to be valid. If iris_save refuses one, nothing after it means anything. */
static unsigned char *save(iris *k, size_t *n) {
  size_t need = iris_save_size(k);
  unsigned char *f = (unsigned char *)xmalloc(need);
  *n = iris_save(k, f, need);
  if (*n != need) {
    printf("  [FAIL] iris_save refused a valid instrument (%zu of %zu bytes); stopping\n", *n, need);
    exit(1);
  }
  return f;
}

/* A receiver with a history: demonstrations, a little training, and a status
   that is not OK -- so a loader that wrote anything, even the status, shows. */
typedef struct { box r; unsigned char *snap; } receiver;
static receiver recv_make(shape s) {
  receiver v;
  v.r = make(s, 77u);
  demos(v.r.k, s, s.cap < 2 ? s.cap : 2, 5);
  iris_continue(v.r.k, 5);
  v.r.k->status = IRIS_TRAINING_DIVERGED;
  v.snap = (unsigned char *)xmalloc(v.r.bytes);
  memcpy(v.snap, v.r.mem, v.r.bytes);
  return v;
}
static void recv_drop(receiver *v) { free(v->snap); drop(&v->r); }
/* 1 when the load was refused and left every byte of the arena as it was.
   The file is copied into a block of exactly n bytes first. */
static int recv_refuses(receiver *v, const unsigned char *file, size_t n) {
  unsigned char *copy = (unsigned char *)xmalloc(n);
  if (n) memcpy(copy, file, n);
  int ok = iris_load(v->r.k, copy, n);
  int same = memcmp(v->snap, v->r.mem, v->r.bytes) == 0;
  free(copy);
  if (!same) memcpy(v->r.mem, v->snap, v->r.bytes);   /* for the next case */
  return !ok && same;
}
static int refused_cleanly(shape s, const unsigned char *file, size_t n) {
  receiver v = recv_make(s);
  int r = recv_refuses(&v, file, n);
  recv_drop(&v);
  return r;
}
static int accepted(shape s, const unsigned char *file, size_t n) {
  receiver v = recv_make(s);
  unsigned char *copy = (unsigned char *)xmalloc(n);
  memcpy(copy, file, n);
  int ok = iris_load(v.r.k, copy, n);
  free(copy);
  recv_drop(&v);
  return ok == 1;
}

/* --------------------------------------------------------- round trips */
static void round_trip(const char *name, shape s, int n_demos, float smoothing, int train) {
  char d[200];
  box a = make(s, 1234u);
  demos(a.k, s, n_demos, 0);
  if (smoothing > 0.0f) iris_set_smoothing(a.k, smoothing);
  if (train == 1) iris_train(a.k);
  if (train == 2) iris_continue(a.k, 40);
  float pa[NPROBE][IRIS_MAX_OUT], pb[NPROBE][IRIS_MAX_OUT];
  play(a.k, s, pa);
  const int st_a = (int)iris_get_status(a.k);
  size_t n = 0;
  unsigned char *f = save(a.k, &n);
  const layout L = lay(s, (size_t)n_demos);

  box b = make(s, 99u);
  demos(b.k, s, s.cap < 3 ? s.cap : 3, 9);
  iris_continue(b.k, 5);
  int ok = iris_load(b.k, f, n);
  play(b.k, s, pb);
  const int st_b = (int)iris_get_status(b.k);
  size_t n2 = 0;
  unsigned char *f2 = save(b.k, &n2);
  int same_play = memcmp(pa, pb, sizeof pa) == 0;
  int same_bytes = n2 == n && memcmp(f, f2, n) == 0;
  int same_state = iris_is_trained(a.k) == iris_is_trained(b.k)
                && iris_count(a.k) == iris_count(b.k) && iris_seed(a.k) == iris_seed(b.k)
                && a.k->rng.s == b.k->rng.s && a.k->next_id == b.k->next_id
                && fbits(a.k->l2) == fbits(b.k->l2) && a.k->fitted == b.k->fitted;
  for (int i = 0; i < n_demos && same_state; ++i)
    if (iris_id_at(a.k, i) != iris_id_at(b.k, i)) same_state = 0;
  snprintf(d, sizeof d, "%zu bytes (table says %zu), load %d, play %s, re-save %s, state %s, status %d/%d",
           n, L.total, ok, same_play ? "same" : "DIFFERS", same_bytes ? "same" : "DIFFERS",
           same_state ? "same" : "DIFFERS", st_a, st_b);
  check(name, n == L.total && ok && same_play && same_bytes && same_state && st_a == st_b, d);
  free(f); free(f2); drop(&a); drop(&b);
}

/* ------------------------------------------------ one rule, one crafted file
   Each case edits one field of a valid file and recomputes the checksum. */
static const shape S2 = { 2, 12, 3, 32 };
#define S2_DEMOS 8
typedef struct { const char *name; size_t (*where)(const layout *L); uint32_t bits; } edit;

static size_t at_magic0(const layout *L)  { (void)L; return 0; }
static size_t at_version(const layout *L) { (void)L; return 4; }
static size_t at_header(const layout *L)  { (void)L; return 8; }
static size_t at_flags(const layout *L)   { (void)L; return 12; }
static size_t at_nin(const layout *L)     { (void)L; return 16; }
static size_t at_nhid(const layout *L)    { (void)L; return 20; }
static size_t at_nout(const layout *L)    { (void)L; return 24; }
static size_t at_seed(const layout *L)    { (void)L; return 32; }
static size_t at_next(const layout *L)    { (void)L; return 36; }
static size_t at_rng(const layout *L)     { (void)L; return 40; }
static size_t at_smooth(const layout *L)  { (void)L; return 44; }
static size_t at_w1_0(const layout *L)    { return L->w1; }
static size_t at_w1_end(const layout *L)  { return L->b1 - 4; }
static size_t at_b1_0(const layout *L)    { return L->b1; }
static size_t at_w2_0(const layout *L)    { return L->w2; }
static size_t at_w2_end(const layout *L)  { return L->b2 - 4; }
static size_t at_b2_0(const layout *L)    { return L->b2; }
static size_t at_b2_end(const layout *L)  { return L->in_lo - 4; }
static size_t at_inlo0(const layout *L)   { return L->in_lo; }
static size_t at_inhi1(const layout *L)   { return L->in_hi + 4; }
static size_t at_outlo0(const layout *L)  { return L->out_lo; }
static size_t at_outhi2(const layout *L)  { return L->out_hi + 8; }
static size_t at_ex0(const layout *L)     { return L->ex; }
static size_t at_ex_mid(const layout *L)  { return L->ex + 4 * 16; }
static size_t at_ex_end(const layout *L)  { return L->ids - 4; }
static size_t at_id0(const layout *L)     { return L->ids; }
static size_t at_id7(const layout *L)     { return L->ids + 28; }

#define NAN_BITS  0x7FC00000u
#define PINF_BITS 0x7F800000u
#define NINF_BITS 0xFF800000u

static const edit REFUSE[] = {
  { "magic 'I' -> 'J'",                         at_magic0, 0x5349524Au },
  { "magic 'S' -> 'T' (last byte)",             at_magic0, 0x54495249u },
  { "version 6",                                at_version, 6u },
  { "version 8",                                at_version, 8u },
  { "version 7 in the wrong byte order",        at_version, 0x07000000u },
  { "header bytes 44",                          at_header, 44u },
  { "header bytes 52",                          at_header, 52u },
  { "flags: trained without fitted",            at_flags, 2u },
  { "flags: unknown bit 2",                     at_flags, 5u },
  { "flags: unknown bit 31",                    at_flags, 0x80000003u },
  { "n_in 3 (receiver 2; length still agrees)", at_nin, 3u },
  { "n_hid 13 (receiver 12)",                   at_nhid, 13u },
  { "n_hid 0",                                  at_nhid, 0u },
  { "n_out 4 (receiver 3)",                     at_nout, 4u },
  { "n_out 2^32 - 1",                           at_nout, 0xFFFFFFFFu },
  { "seed 0",                                   at_seed, 0u },
  { "next_id 0",                                at_next, 0u },
  { "next_id equal to the largest id (8)",      at_next, 8u },
  { "next_id below stored ids (5)",             at_next, 5u },
  { "next_id 2^31 - 1",                         at_next, 0x7FFFFFFFu },
  { "next_id 2^31",                             at_next, 0x80000000u },
  { "next_id 2^32 - 1",                         at_next, 0xFFFFFFFFu },
  { "random-number state 0",                    at_rng, 0u },
  { "smoothing not-a-number",                   at_smooth, NAN_BITS },
  { "smoothing +infinity",                      at_smooth, PINF_BITS },
  { "smoothing -infinity",                      at_smooth, NINF_BITS },
  { "smoothing smallest negative (-1e-45)",     at_smooth, 0x80000001u },
  { "smoothing 1 + one step (1.0000001)",       at_smooth, 0x3F800001u },
  { "smoothing 2",                              at_smooth, 0x40000000u },
  { "w1[0] not-a-number",                       at_w1_0, NAN_BITS },
  { "w1[last] +infinity",                       at_w1_end, PINF_BITS },
  { "b1[0] -infinity",                          at_b1_0, NINF_BITS },
  { "in_lo[0] not-a-number",                    at_inlo0, NAN_BITS },
  { "in_hi[1] +infinity",                       at_inhi1, PINF_BITS },
  { "out_lo[0] not-a-number",                   at_outlo0, NAN_BITS },
  { "out_hi[2] -infinity",                      at_outhi2, NINF_BITS },
  { "demonstration 0, input 0 not-a-number",    at_ex0, NAN_BITS },
  { "demonstration 3, input 1 -infinity",       at_ex_mid, NINF_BITS },
  { "last demonstration, last output +infinity", at_ex_end, PINF_BITS },
  { "identifier 0",                             at_id0, 0u },
  { "identifier -1",                            at_id0, 0xFFFFFFFFu },
  { "last identifier equal to next_id (9)",     at_id7, 9u },
  { "last identifier repeats the one before (7)", at_id7, 7u },
};
static const edit ACCEPT[] = {
  { "flags 0: not fitted",                      at_flags, 0u },
  { "flags 1: fitted, not trained",             at_flags, 1u },
  { "seed 2^32 - 1",                            at_seed, 0xFFFFFFFFu },
  { "next_id one above the largest id (9)",     at_next, 9u },
  { "next_id 2^31 - 2",                         at_next, 0x7FFFFFFEu },
  { "random-number state 1",                    at_rng, 1u },
  { "smoothing 0",                              at_smooth, 0u },
  { "smoothing -0",                             at_smooth, 0x80000000u },
  { "smoothing 1",                              at_smooth, 0x3F800000u },
  { "smoothing smallest positive (1e-45)",      at_smooth, 1u },
  { "w1[0] exactly 16",                         at_w1_0, 0x41800000u },
  { "b2[0] exactly -16",                        at_b2_0, 0xC1800000u },
  { "w2[0] 16 + one step",                      at_w2_0, 0x41800001u },
  { "w2[last] 1e30",                            at_w2_end, 0x7149F2CAu },
  { "b2[last] -16 - one step",                  at_b2_end, 0xC1800001u },
  { "w1[last] the largest float",               at_w1_end, 0x7F7FFFFFu },
};

static void rules(void) {
  char d[200];
  box a = make(S2, 1234u);
  demos(a.k, S2, S2_DEMOS, 0);
  iris_set_smoothing(a.k, 0.25f);
  iris_train(a.k);
  size_t n = 0;
  unsigned char *base = save(a.k, &n);
  const layout L = lay(S2, S2_DEMOS);
  unsigned char *f = (unsigned char *)xmalloc(n + 8);
  receiver v = recv_make(S2);

  /* the positive control first: the unedited file loads */
  { int ok = accepted(S2, base, n);
    snprintf(d, sizeof d, "load %d", ok);
    check("the unedited file loads", ok, d); }

  for (size_t c = 0; c < sizeof REFUSE / sizeof REFUSE[0]; ++c) {
    memcpy(f, base, n);
    wr32(f, REFUSE[c].where(&L), REFUSE[c].bits);
    fixcrc(f, n);
    char name[120]; snprintf(name, sizeof name, "refuses %s", REFUSE[c].name);
    int ok = recv_refuses(&v, f, n);
    check(name, ok, ok ? "arena identical" : "ACCEPTED or arena changed");
  }
  for (size_t c = 0; c < sizeof ACCEPT / sizeof ACCEPT[0]; ++c) {
    memcpy(f, base, n);
    wr32(f, ACCEPT[c].where(&L), ACCEPT[c].bits);
    fixcrc(f, n);
    char name[120]; snprintf(name, sizeof name, "accepts %s", ACCEPT[c].name);
    int ok = accepted(S2, f, n);
    check(name, ok, ok ? "loaded" : "REFUSED");
  }

  /* Ranges: each rule on its own, inputs and outputs. */
  { struct { const char *name; size_t lo, hi; float vlo, vhi; int ok; } R[] = {
      { "input range inverted (lo > hi)",           L.in_lo,      L.in_hi,      0.5f, 0.25f, 0 },
      { "input range of zero width at infinity",    L.in_lo,      L.in_hi,      __builtin_inff(), __builtin_inff(), 0 },
      { "input range whose width overflows",        L.in_lo + 4,  L.in_hi + 4, -3e38f, 3e38f, 0 },
      { "output range inverted (lo > hi)",          L.out_lo + 4, L.out_hi + 4, 0.8f, 0.2f,  0 },
      { "output range of zero width",               L.out_lo,     L.out_hi,     0.4f, 0.4f,  0 },
      { "output range whose width overflows",       L.out_lo + 8, L.out_hi + 8, -2e38f, 2e38f, 0 },
      { "input range of zero width (a still input)", L.in_lo,     L.in_hi,      0.5f, 0.5f,  1 },
      { "input range one step wide",                L.in_lo,      L.in_hi,      1.0f, 1.00000012f, 1 },
      { "input range -1.5e38 to 1.5e38",            L.in_lo + 4,  L.in_hi + 4, -1.5e38f, 1.5e38f, 1 },
      { "output range one step wide",               L.out_lo,     L.out_hi,     1.0f, 1.00000012f, 1 },
    };
    for (size_t c = 0; c < sizeof R / sizeof R[0]; ++c) {
      memcpy(f, base, n);
      wrf(f, R[c].lo, R[c].vlo); wrf(f, R[c].hi, R[c].vhi);
      fixcrc(f, n);
      char name[120];
      snprintf(name, sizeof name, "%s %s", R[c].ok ? "accepts" : "refuses", R[c].name);
      int ok = R[c].ok ? accepted(S2, f, n) : recv_refuses(&v, f, n);
      check(name, ok, ok ? (R[c].ok ? "loaded" : "arena identical")
                         : (R[c].ok ? "REFUSED" : "ACCEPTED or arena changed"));
    }
  }

  /* Identifiers out of order but all different: legal, and every one of them
     takes the search back through the others. */
  { memcpy(f, base, n);
    uint32_t id0 = rd32(f, L.ids), id7 = rd32(f, L.ids + 28);
    wr32(f, L.ids, id7); wr32(f, L.ids + 28, id0);
    fixcrc(f, n);
    int ok = accepted(S2, f, n);
    check("accepts identifiers out of order but all different", ok, ok ? "loaded" : "REFUSED");
    /* and the same order with one repeat far from its twin */
    wr32(f, L.ids + 16, id7);
    fixcrc(f, n);
    ok = recv_refuses(&v, f, n);
    check("refuses an out-of-order identifier that repeats", ok,
          ok ? "arena identical" : "ACCEPTED or arena changed"); }

  /* The length, each way, with the checksum moved to the new end. */
  { const size_t body = n - 4;
    memcpy(f, base, body); f[body] = 0; fixcrc(f, n + 1);
    int longer = recv_refuses(&v, f, n + 1);
    memcpy(f, base, body); memset(f + body, 0, 4); fixcrc(f, n + 4);
    int word = recv_refuses(&v, f, n + 4);
    memcpy(f, base, body - 1); fixcrc(f, n - 1);
    int shorter = recv_refuses(&v, f, n - 1);
    snprintf(d, sizeof d, "one byte more %s, one word more %s, one byte less %s",
             longer ? "refused" : "ACCEPTED", word ? "refused" : "ACCEPTED",
             shorter ? "refused" : "ACCEPTED");
    check("refuses a length the header does not describe", longer && word && shorter, d); }

  /* The checksum itself. */
  { memcpy(f, base, n); f[n - 1] ^= 0x01;
    int ok = recv_refuses(&v, f, n);
    check("refuses a wrong checksum", ok, ok ? "arena identical" : "ACCEPTED or arena changed"); }

  /* A demonstration far outside its stored range is legal (a take recorded
     after the fit), but normalising it overflows: the load-time error is then
     not a number and must read 1, with the status OK. */
  { memcpy(f, base, n); wrf(f, L.ex + 8, 3e38f); fixcrc(f, n);
    box r = make(S2, 5u);
    int ok = iris_load(r.k, f, n);
    snprintf(d, sizeof d, "load %d, last_error %g, status %d", ok,
             (double)iris_last_error(r.k), (int)iris_get_status(r.k));
    check("a demonstration past the float range loads; last_error 1",
          ok && iris_last_error(r.k) == 1.0f && iris_get_status(r.k) == IRIS_STATUS_OK, d);
    drop(&r); }

  recv_drop(&v); free(f); free(base); drop(&a);
}

/* A genuine file breaking one rule: the shape, or the capacity. */
static void shape_and_capacity(void) {
  char d[200];
  const shape other[3] = { { 3, 12, 3, 32 }, { 2, 13, 3, 32 }, { 2, 12, 4, 32 } };
  const char *what[3] = { "n_in", "n_hid", "n_out" };
  for (int c = 0; c < 3; ++c) {
    box a = make(other[c], 1234u);
    demos(a.k, other[c], 6, 0);
    iris_continue(a.k, 20);
    size_t n = 0;
    unsigned char *f = save(a.k, &n);
    int ok = n > 0 && refused_cleanly(S2, f, n);
    char name[120]; snprintf(name, sizeof name, "refuses a file whose %s differs from the receiver's", what[c]);
    snprintf(d, sizeof d, "a genuine %d-%d-%d file into a 2-12-3: %s", other[c].ni, other[c].nh,
             other[c].no, ok ? "refused, arena identical" : "ACCEPTED or arena changed");
    check(name, ok, d);
    free(f); drop(&a);
  }
  { const shape big = { 2, 12, 3, 32 }, small = { 2, 12, 3, 8 };
    box a = make(big, 1234u);
    demos(a.k, big, 9, 0); iris_continue(a.k, 20);
    size_t n = 0; unsigned char *f = save(a.k, &n);
    int over = refused_cleanly(small, f, n);
    iris_delete_last(a.k);
    size_t n8 = 0; unsigned char *f8 = save(a.k, &n8);
    int full = accepted(small, f8, n8);
    snprintf(d, sizeof d, "9 demonstrations into capacity 8 %s; 8 into 8 %s",
             over ? "refused" : "ACCEPTED", full ? "loaded" : "REFUSED");
    check("refuses more demonstrations than the receiver holds", over && full, d);
    free(f); free(f8); drop(&a);
  }
}

/* --------------------------------------------------------------- sweeps */
static void truncations(const char *name, shape s, int n_demos, int train) {
  char d[200];
  box a = make(s, 1234u);
  demos(a.k, s, n_demos, 0);
  if (train) iris_continue(a.k, 20);
  size_t n = 0;
  unsigned char *f = save(a.k, &n);
  unsigned char *longer = (unsigned char *)xmalloc(n + 8);
  memcpy(longer, f, n); memset(longer + n, 0, 8);
  receiver v = recv_make(s);
  size_t bad = 0, tried = 0;
  for (size_t len = 0; len < n; ++len, ++tried) if (!recv_refuses(&v, f, len)) bad++;
  for (size_t len = n + 1; len <= n + 8; ++len, ++tried) if (!recv_refuses(&v, longer, len)) bad++;
  snprintf(d, sizeof d, "%zu-byte file: %zu lengths tried, %zu accepted or changed the arena", n, tried, bad);
  check(name, bad == 0, d);
  recv_drop(&v); free(longer); free(f); drop(&a);
}

/* every_bit: 1 flips every bit; 0 flips every bit of the fixed header and the
   checksum, and one bit of every other byte (bit number = offset mod 8). */
static void flips(const char *name, shape s, int n_demos, int every_bit) {
  char d[200];
  box a = make(s, 1234u);
  demos(a.k, s, n_demos, 0);
  iris_continue(a.k, 20);
  size_t n = 0;
  unsigned char *f = save(a.k, &n);
  unsigned char *t = (unsigned char *)xmalloc(n);
  receiver v = recv_make(s);
  size_t bad = 0, tried = 0;
  for (size_t byte = 0; byte < n; ++byte)
    for (int bit = 0; bit < 8; ++bit) {
      if (!every_bit && byte >= 48 && byte < n - 4 && bit != (int)(byte % 8)) continue;
      memcpy(t, f, n); t[byte] ^= (unsigned char)(1u << bit);
      tried++;
      if (!recv_refuses(&v, t, n)) bad++;
    }
  snprintf(d, sizeof d, "%zu-byte file: %zu flips, %zu accepted or changed the arena", n, tried, bad);
  check(name, bad == 0, d);
  recv_drop(&v); free(t); free(f); drop(&a);
}

/* ------------------------------------------------------- the other checks */
static void after_load_at_rest(void) {
  char d[320];
  box a = make(S2, 1234u);
  demos(a.k, S2, S2_DEMOS, 0); iris_train(a.k);
  size_t n = 0; unsigned char *f = save(a.k, &n);

  box b = make(S2, 99u);
  demos(b.k, S2, 6, 3);
  iris_continue(b.k, 50);                  /* velocities and ledger now set */
  iris_train_begin(b.k, 5000);
  iris_train_slice(b.k, 100);                  /* a sliced run in progress */
  iris_internal_set_learning(b.k, 0.3f, 0.2f); /* not the defaults */
  b.k->status = IRIS_STORE_FULL;
  const int nw = S2.nh * S2.ni + S2.nh + S2.no * S2.nh + S2.no;
  int dirty_v = 0, dirty_l = 0;
  for (int i = 0; i < S2.nh * S2.ni; ++i) if (b.k->v_w1[i] != 0.0f) dirty_v = 1;
  for (int i = 0; i < S2.cap; ++i) if (b.k->ex_res[i] != 0.0f) dirty_l = 1;
  const int dirty_t = b.k->tr_running && b.k->tr_done > 0;

  int ok = iris_load(b.k, f, n);
  int vel = 1, led = 1;
  for (int i = 0; i < S2.nh * S2.ni; ++i) if (fbits(b.k->v_w1[i]) != 0u) vel = 0;
  for (int i = 0; i < S2.nh; ++i)          if (fbits(b.k->v_b1[i]) != 0u) vel = 0;
  for (int i = 0; i < S2.no * S2.nh; ++i)  if (fbits(b.k->v_w2[i]) != 0u) vel = 0;
  for (int i = 0; i < S2.no; ++i)          if (fbits(b.k->v_b2[i]) != 0u) vel = 0;
  for (int i = 0; i < S2.cap; ++i)         if (fbits(b.k->ex_res[i]) != 0u) led = 0;
  const int prog = b.k->tr_done == 0 && b.k->tr_ceiling == 0 && b.k->tr_running == 0
                && b.k->tr_n_ex == 0 && b.k->tr_ref == 0.0f && b.k->tr_err == 0.0f
                && b.k->res_epochs == 0
                && !iris_train_busy(b.k) && iris_train_slice(b.k, 10) == 0;
  const int learn = b.k->lr == 0.10f && b.k->momentum == 0.85f;
  const float e = iris_last_error(b.k);
  snprintf(d, sizeof d, "dirty before %d/%d/%d; load %d, velocities zero %d, ledger zero %d, "
           "progress reset %d, learning rate and momentum the defaults %d, status %d, "
           "last_error %g, %d weights",
           dirty_v, dirty_l, dirty_t, ok, vel, led, prog, learn, (int)iris_get_status(b.k),
           (double)e, nw);
  check("after a load: velocities 0, ledger clear, progress reset, OK",
        dirty_v && dirty_l && dirty_t && ok && vel && led && prog && learn
        && iris_get_status(b.k) == IRIS_STATUS_OK && !iris_internal_isbad(e) && e >= 0.0f && e < 1.0f, d);
  free(f); drop(&a); drop(&b);
}

static void save_after_record(void) {
  char d[200];
  box a = make(S2, 1234u);
  demos(a.k, S2, S2_DEMOS, 0);
  iris_train(a.k);
  { float in[2] = { 0.35f, 0.65f }, out[3] = { 0.3f, 0.6f, 0.4f };
    iris_record(a.k, in, out); }             /* fitted, and no longer trained */
  float pa[NPROBE][IRIS_MAX_OUT], pb[NPROBE][IRIS_MAX_OUT];
  play(a.k, S2, pa);
  size_t n = 0; unsigned char *f = save(a.k, &n);
  box b = make(S2, 99u);
  int ok = iris_load(b.k, f, n);
  play(b.k, S2, pb);
  const int plays = iris_get_status(b.k) == IRIS_STATUS_OK && memcmp(pa, pb, sizeof pa) == 0;
  snprintf(d, sizeof d, "flags %lu, load %d, plays the same bits %d, is_trained %d, status %d",
           (unsigned long)rd32(f, 12), ok, plays, iris_is_trained(b.k), (int)iris_get_status(b.k));
  check("a take recorded after training: still plays after save+load",
        rd32(f, 12) == 1u && ok && plays && !iris_is_trained(b.k) && iris_count(b.k) == S2_DEMOS + 1, d);
  free(f); drop(&a); drop(&b);
}

static void unfitted_and_emptied(void) {
  char d[200];
  /* never trained: flags 0, and it must not play random weights after a load */
  { box a = make(S2, 1234u);
    demos(a.k, S2, 4, 0);
    size_t n = 0; unsigned char *f = save(a.k, &n);
    box b = make(S2, 99u);
    int ok = iris_load(b.k, f, n);
    float in[2] = { 0.2f, 0.3f }, out[3];
    iris_predict(b.k, in, out);
    size_t n2 = 0; unsigned char *f2 = save(b.k, &n2);
    const int silent = iris_get_status(b.k) == IRIS_NOT_FITTED;
    snprintf(d, sizeof d, "flags %lu, load %d, status after predict %d, re-save %s",
             (unsigned long)rd32(f, 12), ok, (int)iris_get_status(b.k),
             (n2 == n && !memcmp(f, f2, n)) ? "same" : "DIFFERS");
    check("a never-trained instrument loads as never trained",
          rd32(f, 12) == 0u && ok && silent && n2 == n && !memcmp(f, f2, n), d);
    free(f); free(f2); drop(&a); drop(&b); }
  /* trained, then every demonstration deleted: fitted, nothing stored */
  { box a = make(S2, 1234u);
    demos(a.k, S2, 6, 0); iris_train(a.k);
    while (iris_count(a.k)) iris_delete_last(a.k);
    float pa[NPROBE][IRIS_MAX_OUT], pb[NPROBE][IRIS_MAX_OUT];
    play(a.k, S2, pa);
    size_t n = 0; unsigned char *f = save(a.k, &n);
    box b = make(S2, 99u);
    int ok = iris_load(b.k, f, n);
    play(b.k, S2, pb);
    snprintf(d, sizeof d, "%zu bytes, flags %lu, load %d, count %d, play %s", n,
             (unsigned long)rd32(f, 12), ok, iris_count(b.k), memcmp(pa, pb, sizeof pa) ? "DIFFERS" : "same");
    check("a fitted instrument with no demonstrations round-trips",
          n == lay(S2, 0).total && rd32(f, 12) == 1u && ok && iris_count(b.k) == 0
          && !memcmp(pa, pb, sizeof pa), d);
    /* With no identifiers stored, only the next_id rule itself can refuse. */
    wr32(f, 36, 0u); fixcrc(f, n);
    int zero = refused_cleanly(S2, f, n);
    wr32(f, 36, 0x7FFFFFFFu); fixcrc(f, n);
    int top = refused_cleanly(S2, f, n);
    wr32(f, 36, 1u); fixcrc(f, n);
    int one = accepted(S2, f, n);
    snprintf(d, sizeof d, "next_id 0 %s, 2^31 - 1 %s, 1 %s", zero ? "refused" : "ACCEPTED",
             top ? "refused" : "ACCEPTED", one ? "loaded" : "REFUSED");
    check("with no demonstrations, next_id is still checked", zero && top && one, d);
    free(f); drop(&a); drop(&b); }
}

/* The largest next_id a file may carry leaves exactly one identifier to hand
   out. The record after that one must refuse rather than overflow next_id
   (UndefinedBehaviorSanitizer reports the overflow), and the instrument, its
   identifiers spent, no longer saves. */
static void identifiers_run_out(void) {
  char d[200];
  box a = make(S2, 1234u);
  demos(a.k, S2, 2, 0);
  size_t n = 0; unsigned char *f = save(a.k, &n);
  wr32(f, 36, 0x7FFFFFFEu); fixcrc(f, n);
  box b = make(S2, 99u);
  const int ok = iris_load(b.k, f, n);
  const float in[2] = { 0.1f, 0.2f }, out[3] = { 0.3f, 0.4f, 0.5f };
  const int last = iris_record(b.k, in, out);
  const int count = iris_count(b.k);
  const int none = iris_record(b.k, in, out);
  unsigned char *g = (unsigned char *)xmalloc(iris_save_size(b.k));
  const size_t saved = iris_save(b.k, g, iris_save_size(b.k));
  snprintf(d, sizeof d, "load %d, last identifier %d, then %d (count %d -> %d, status %d), save %zu",
           ok, last, none, count, iris_count(b.k), (int)iris_get_status(b.k), saved);
  check("identifiers run out: the next record refuses",
        ok && last == 0x7FFFFFFE && none == 0 && count == 3 && iris_count(b.k) == 3
        && iris_get_status(b.k) == IRIS_STATUS_OK && saved == 0, d);
  free(g); free(f); drop(&a); drop(&b);
}

static void save_side(void) {
  char d[200];
  box a = make(S2, 1234u);
  demos(a.k, S2, S2_DEMOS, 0); iris_train(a.k);
  const size_t need = iris_save_size(a.k);
  unsigned char *buf = (unsigned char *)xmalloc(need + 16);
  /* too small: refused, and not one byte written */
  memset(buf, 0x5A, need + 16);
  size_t small = iris_save(a.k, buf, need - 1);
  int untouched = 1;
  for (size_t i = 0; i < need + 16; ++i) if (buf[i] != 0x5A) untouched = 0;
  /* exact: written, and not one byte past the end */
  size_t exact = iris_save(a.k, buf, need);
  int inside = 1;
  for (size_t i = need; i < need + 16; ++i) if (buf[i] != 0x5A) inside = 0;
  snprintf(d, sizeof d, "need %zu: one short -> %zu (buffer untouched %d), exact -> %zu (nothing past it %d)",
           need, small, untouched, exact, inside);
  check("iris_save needs exactly iris_save_size bytes", small == 0 && untouched && exact == need && inside, d);

  /* a finite weight past IRIS_W_LIMIT is written and loads back to the bit;
     an instrument the loader would refuse is not written */
  { const float keep = a.k->w2[0];
    a.k->w2[0] = 20.0f;                                 /* past IRIS_W_LIMIT */
    size_t w = iris_save(a.k, buf, need + 16);
    receiver v = recv_make(S2);
    int back = w == need && iris_load(v.r.k, buf, w) && v.r.k->w2[0] == 20.0f;
    recv_drop(&v);
    a.k->w2[0] = keep;
    const float keep_ex = a.k->ex[1];
    a.k->ex[1] = __builtin_nanf("");                    /* a store only IRIS_NO_GUARDS reaches */
    memset(buf, 0x5A, need + 16);
    size_t x = iris_save(a.k, buf, need + 16);
    int cleared = 1;
    for (size_t i = 0; i < need; ++i) if (buf[i] != 0) cleared = 0;
    a.k->ex[1] = keep_ex;
    size_t again = iris_save(a.k, buf, need + 16);
    snprintf(d, sizeof d, "weight 20 -> %zu (loads back %d), stored not-a-number -> %zu (cleared %d), restored -> %zu",
             w, back, x, cleared, again);
    check("iris_save writes every finite instrument, and only those",
          back && x == 0 && cleared && again == need, d); }

  /* null arguments */
  { receiver v = recv_make(S2);
    size_t n = 0; unsigned char *f = save(a.k, &n);
    int r1 = iris_load(0, f, n), r2 = iris_load(v.r.k, 0, n);
    int same = memcmp(v.snap, v.r.mem, v.r.bytes) == 0;
    size_t s1 = iris_save(0, buf, need), s2 = iris_save(a.k, 0, need);
    snprintf(d, sizeof d, "load(NULL k) %d, load(NULL buf) %d (arena same %d), save(NULL k) %zu, save(NULL buf) %zu",
             r1, r2, same, s1, s2);
    check("null instrument or buffer is refused", !r1 && !r2 && same && !s1 && !s2, d);
    free(f); recv_drop(&v); }
  free(buf); drop(&a);
}

static void alignment(void) {
  char d[200];
  box a = make(S2, 1234u);
  demos(a.k, S2, S2_DEMOS, 0); iris_set_smoothing(a.k, 0.5f); iris_train(a.k);
  float pa[NPROBE][IRIS_MAX_OUT], pb[NPROBE][IRIS_MAX_OUT];
  play(a.k, S2, pa);
  size_t n = 0; unsigned char *ref = save(a.k, &n);
  unsigned char *big = (unsigned char *)xmalloc(n + 16);
  int good = 0;
  for (size_t off = 1; off <= 7; ++off) {
    memset(big, 0, n + 16);
    size_t w = iris_save(a.k, big + off, n);
    box b = make(S2, 99u);
    int ok = iris_load(b.k, big + off, n);
    play(b.k, S2, pb);
    if (w == n && !memcmp(big + off, ref, n) && ok && !memcmp(pa, pb, sizeof pa)) good++;
    drop(&b);
  }
  snprintf(d, sizeof d, "%d of 7 offsets saved the same bytes and loaded the same instrument", good);
  check("a buffer at offset 1..7 saves and loads", good == 7, d);
  free(big); free(ref); drop(&a);
}

static void standard_checksum(void) {
  char d[120];
  const uint32_t c = iris_internal_crc32("123456789", 9);
  snprintf(d, sizeof d, "CRC-32 of \"123456789\" = 0x%08lX (want 0xCBF43926)", (unsigned long)c);
  check("iris_internal_crc32 is the standard CRC-32", c == 0xCBF43926u, d);
}

static void small_unit(void) {
  char d[200];
  const shape s8 = { 8, 12, 2, 8 };
  box a = make(s8, 1234u);
  demos(a.k, s8, 6, 0); iris_continue(a.k, 50);
  size_t n = 0; unsigned char *f = save(a.k, &n);
  receiver v = recv_make(s8);
  unsigned char *copy = (unsigned char *)xmalloc(n);
  memcpy(copy, f, n);
  int small = load_in_small_unit(v.r.k, copy, n);
  int same = memcmp(v.snap, v.r.mem, v.r.bytes) == 0;
  int here = iris_load(v.r.k, copy, n);               /* the file itself is fine */
  snprintf(d, sizeof d, "8 inputs into a unit with IRIS_MAX_IN 4: load %d, arena same %d; "
           "this unit loads it: %d", small, same, here);
  check("a unit with smaller maxima refuses, no overflow", !small && same && here, d);
  free(copy); free(f); recv_drop(&v); drop(&a);
}

/* ------------------------------------------------------------- the fixture
   tests/golden/v7-instrument.bin was generated once, on purpose, from the
   golden recipe of tests/audit.c (seed 1234, its 20 demonstrations,
   iris_reseed(k, 1234) and iris_continue(k, 800), a 2-12-3 instrument of
   capacity 256), and tests/golden/v7-expected.txt holds what that instrument
   played before it was saved. Loading the file must play every one of those
   bits, and saving it again must give the file back byte for byte. This pins
   both the bytes of format 7 and what they mean. Never regenerate it to make
   this pass. */
static unsigned char *slurp(const char *path, size_t *n) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return 0;
  unsigned char *b = (unsigned char *)xmalloc(1 << 16);
  *n = fread(b, 1, 1 << 16, fp);
  fclose(fp);
  return b;
}
static void fixture(const char *dir) {
  char d[240], path[512];
  const shape sg = { 2, 12, 3, 256 };
  snprintf(path, sizeof path, "%s/v7-instrument.bin", dir);
  size_t n = 0;
  unsigned char *raw = slurp(path, &n);
  if (!raw) { check("the committed fixture plays every bit", 0, "cannot open tests/golden/v7-instrument.bin"); return; }
  unsigned char *f = (unsigned char *)xmalloc(n);
  memcpy(f, raw, n); free(raw);
  box b = make(sg, 5u);
  int ok = iris_load(b.k, f, n);
  snprintf(path, sizeof path, "%s/v7-expected.txt", dir);
  FILE *fp = fopen(path, "r");
  int lines = 0, grid_ok = 1, bits_ok = 1;
  char line[256];
  while (fp && fgets(line, sizeof line, fp)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    unsigned long w[5];
    if (sscanf(line, "%lx %lx %lx %lx %lx", &w[0], &w[1], &w[2], &w[3], &w[4]) != 5) { grid_ok = 0; break; }
    const int a = lines / 25, c = lines % 25;
    float in[2] = { (float)(a - 4) / 16.0f, (float)(c - 4) / 16.0f }, out[3];
    if (fbits(in[0]) != (uint32_t)w[0] || fbits(in[1]) != (uint32_t)w[1]) grid_ok = 0;
    in[0] = bitsf((uint32_t)w[0]); in[1] = bitsf((uint32_t)w[1]);
    iris_predict(b.k, in, out);
    for (int o = 0; o < 3; ++o) if (fbits(out[o]) != (uint32_t)w[2 + o]) bits_ok = 0;
    lines++;
  }
  if (fp) fclose(fp);
  size_t n2 = 0; unsigned char *f2 = save(b.k, &n2);
  const int resave = n2 == n && !memcmp(f, f2, n);
  snprintf(d, sizeof d, "%zu bytes, version %lu, flags %lu, load %d, %d predictions %s, grid %s, re-save %s",
           n, n >= 16 ? (unsigned long)rd32(f, 4) : 0ul, n >= 16 ? (unsigned long)rd32(f, 12) : 0ul, ok,
           lines, bits_ok ? "bit-identical" : "DIFFER", grid_ok ? "as documented" : "WRONG",
           resave ? "byte-identical" : "DIFFERS");
  check("the committed fixture plays every bit", ok && lines == 625 && bits_ok && grid_ok && resave, d);
  free(f); free(f2); drop(&b);
}

int main(int argc, char **argv) {
  const char *golden = argc > 1 ? argv[1] : "tests/golden";
  printf("\n  THE SAVE FORMAT, AS A PARSER (iris.h PART 9)\n\n");

  standard_checksum();
  round_trip("round trip, 1-in/1-out (1-8-1, 5 demonstrations)",  (shape){ 1, 8, 1, 16 }, 5, 0.0f, 1);
  round_trip("round trip, 2-12-3 with smoothing 0.25",            (shape){ 2, 12, 3, 32 }, 8, 0.25f, 1);
  round_trip("round trip, 3-16-2 at full capacity",               (shape){ 3, 16, 2, 8 }, 8, 0.7f, 1);
  round_trip("round trip, the maxima (32-64-16)",
             (shape){ IRIS_MAX_IN, IRIS_MAX_HID, IRIS_MAX_OUT, 6 }, 6, 1.0f, 2);
  round_trip("round trip, one demonstration (inputs still, outputs floored)", (shape){ 2, 12, 3, 4 }, 1, 0.0f, 1);
  save_after_record();
  unfitted_and_emptied();
  after_load_at_rest();
  identifiers_run_out();
  save_side();
  alignment();
  rules();
  shape_and_capacity();
  small_unit();
  truncations("every truncation refused, 1-8-1",   (shape){ 1, 8, 1, 16 }, 5, 1);
  truncations("every truncation refused, 2-12-3",  (shape){ 2, 12, 3, 32 }, 8, 1);
  truncations("every truncation refused, the maxima",
              (shape){ IRIS_MAX_IN, IRIS_MAX_HID, IRIS_MAX_OUT, 3 }, 3, 0);
  flips("every single-bit flip refused, 1-8-1",    (shape){ 1, 8, 1, 16 }, 5, 1);
  flips("every single-bit flip refused, 2-12-3",   (shape){ 2, 12, 3, 32 }, 8, 1);
  flips("every single-bit flip refused, 3-16-2",   (shape){ 3, 16, 2, 8 }, 8, 1);
  flips("flips refused at the maxima (a bit in every byte)",
        (shape){ IRIS_MAX_IN, IRIS_MAX_HID, IRIS_MAX_OUT, 3 }, 3, 0);
  fixture(golden);

  if (fails) printf("\n  %d of %d FAILING\n\n", fails, checks);
  else       printf("\n  all %d pass\n\n", checks);
  return fails ? 1 : 0;
}

#endif
