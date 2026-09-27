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
     - a file one identifier short of the limit hands out that one, the
       record after it refuses with IRIS_STORE_FULL instead of overflowing,
       and the instrument, its identifiers spent, saves, loads and plays;
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
  { "next_id 2^31 - 1, every identifier spent", at_next, 0x7FFFFFFFu },
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
    wr32(f, 36, 0x80000000u); fixcrc(f, n);
    int top = refused_cleanly(S2, f, n);
    wr32(f, 36, 1u); fixcrc(f, n);
    int one = accepted(S2, f, n);
    snprintf(d, sizeof d, "next_id 0 %s, 2^31 %s, 1 %s", zero ? "refused" : "ACCEPTED",
             top ? "refused" : "ACCEPTED", one ? "loaded" : "REFUSED");
    check("with no demonstrations, next_id is still checked", zero && top && one, d);
    free(f); drop(&a); drop(&b); }
}

/* A file one short of IRIS_ID_LIMIT leaves exactly one identifier to hand
   out. The record after that one must refuse with IRIS_STORE_FULL rather
   than overflow next_id (UndefinedBehaviorSanitizer reports the overflow).
   The instrument, its identifiers spent, must still save, and the file must
   load into a fresh instrument that plays the same bits and records no
   more. */
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
  const int st = (int)iris_get_status(b.k);
  unsigned char *g = (unsigned char *)xmalloc(iris_save_size(b.k));
  const size_t saved = iris_save(b.k, g, iris_save_size(b.k));
  box c = make(S2, 5u);
  const int back = saved && iris_load(c.k, g, saved);
  float pb[3], pc[3];
  iris_predict(b.k, in, pb); iris_predict(c.k, in, pc);
  const int same = memcmp(pb, pc, sizeof pb) == 0;
  const int again = iris_record(c.k, in, out);
  snprintf(d, sizeof d, "load %d, last identifier %d, then %d (count %d -> %d, status %d), "
           "save %zu, reload %d plays the same %d, records %d (status %d)",
           ok, last, none, count, iris_count(b.k), st, saved, back, same, again,
           (int)iris_get_status(c.k));
  check("identifiers run out: the next record refuses, and it still saves",
        ok && last == 0x7FFFFFFE && none == 0 && count == 3 && iris_count(b.k) == 3
        && st == IRIS_STORE_FULL && saved == iris_save_size(b.k) && back && same
        && again == 0 && iris_get_status(c.k) == IRIS_STORE_FULL, d);
  free(g); free(f); drop(&a); drop(&b); drop(&c);
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

/* ------------------------------------------------------------ iris_copy
   iris_copy is defined as iris_save followed by iris_load with no buffer
   between, and is held to exactly that. Two receivers are built the same
   way; one is handed the source's file through iris_save and iris_load, the
   other the source itself through iris_copy. Both must give the same answer,
   and afterwards hold the same instrument (same_instrument): every field of
   the structure but the addresses of its arrays, and every byte of the
   arrays, the working ones included. A refusal must leave the receiver's
   arena as it was, every byte. */
static int same_instrument(const iris *a, const iris *b) {
#define SAME(f) (memcmp(&a->f, &b->f, sizeof a->f) == 0)
  if (!(SAME(n_in) && SAME(n_hid) && SAME(n_out) && SAME(cap) && SAME(n_ex) && SAME(next_id)
        && SAME(res_epochs) && SAME(lr) && SAME(momentum) && SAME(l2) && SAME(seed)
        && SAME(rng) && SAME(trained) && SAME(fitted) && SAME(last_error) && SAME(status)
        && SAME(tr_done) && SAME(tr_ceiling) && SAME(tr_running) && SAME(tr_n_ex)
        && SAME(tr_ref) && SAME(tr_err))) return 0;
#undef SAME
  /* iris_init carves the arrays in one run, from w1 to the shuffle buffer */
  const size_t len = (size_t)((const unsigned char *)(a->order + a->cap)
                            - (const unsigned char *)a->w1);
  return memcmp(a->w1, b->w1, len) == 0;
}

/* A receiver with a history, as recv_make's, and more: a sliced run in
   progress, a learning rate and momentum that are not the defaults. Made
   twice from the same calls, the two are the same instrument. */
static box copy_receiver(shape s) {
  box r = make(s, 77u);
  demos(r.k, s, s.cap < 3 ? s.cap : 3, 5);
  iris_continue(r.k, 5);
  iris_train_begin(r.k, 4000);
  iris_train_slice(r.k, 30);
  iris_internal_set_learning(r.k, 0.3f, 0.2f);
  r.k->status = IRIS_STORE_FULL;
  return r;
}

/* The source in one of nine states. */
#define COPY_STATES 9
static const char *const copy_state_name[COPY_STATES] = {
  "empty", "takes, never fitted", "trained", "trained then one more take",
  "closed-form solve", "in the middle of a sliced run",
  "a take recorded in the middle of a sliced run", "smoothing, gaps in the identifiers",
  "every take deleted after training" };
static void copy_source(iris *k, shape s, int state) {
  const int n = s.cap < 5 ? s.cap : 5;
  const int big = s.ni * s.nh > 256;               /* the maxima: keep it quick */
  if (state == 0) return;
  demos(k, s, state == 3 ? n - 1 : n, 1);
  switch (state) {
    case 2: case 3: case 8:
      if (big) iris_continue(k, 60); else iris_train(k);
      if (state == 3) demos(k, s, 1, 4);
      if (state == 8) while (iris_delete_last(k)) {}
      break;
    case 4: { const size_t sn = IRIS_ELM_SCRATCH(s.nh, s.no);
      unsigned char *scr = (unsigned char *)xmalloc(sn);
      iris_train_elm(k, 1e-3f, scr, sn);
      free(scr); break; }
    case 5: case 6:
      iris_train_begin(k, 4000);
      iris_train_slice(k, 40);
      if (state == 6 && iris_count(k) < s.cap) demos(k, s, 1, 7);
      iris_train_slice(k, 40);
      break;
    case 7:
      iris_set_smoothing(k, 0.37f);
      iris_continue(k, 40);
      iris_delete_id(k, 2);
      iris_delete_last(k);
      break;
    default: break;
  }
}

/* One source against two identical receivers: 1 when iris_copy answered as
   iris_save and iris_load did and left the same receiver, or refused and
   left its receiver's every byte. *copied gets iris_copy's answer.
   save_ok 0 says iris_save cannot write this source at all (a count of
   takes it would read past the store for), so the pair's answer is 0. */
static int copy_agrees(iris *src, shape sd, int save_ok, int *copied) {
  box d1 = copy_receiver(sd), d2 = copy_receiver(sd);
  unsigned char *snap = (unsigned char *)xmalloc(d2.bytes);
  int r1 = 0;
  if (save_ok) {
    const size_t need = iris_save_size(src);
    unsigned char *f = (unsigned char *)xmalloc(need);
    const size_t w = iris_save(src, f, need);
    if (w) r1 = iris_load(d1.k, f, w);
    free(f);
  }
  memcpy(snap, d2.mem, d2.bytes);
  const int r2 = iris_copy(d2.k, src);
  int ok = r1 == r2 && (r2 ? same_instrument(d1.k, d2.k) : memcmp(snap, d2.mem, d2.bytes) == 0);
  if (ok && r2) {                       /* the two play and save the same */
    const shape s = { d1.k->n_in, d1.k->n_hid, d1.k->n_out, d1.k->cap };
    float p1[NPROBE][IRIS_MAX_OUT], p2[NPROBE][IRIS_MAX_OUT];
    play(d1.k, s, p1); play(d2.k, s, p2);
    const size_t n1 = iris_save_size(d1.k), n2 = iris_save_size(d2.k);
    unsigned char *f1 = (unsigned char *)xmalloc(n1), *f2 = (unsigned char *)xmalloc(n2);
    ok = memcmp(p1, p2, sizeof p1) == 0 && n1 == n2 && iris_save(d1.k, f1, n1) == n1
      && iris_save(d2.k, f2, n2) == n2 && memcmp(f1, f2, n1) == 0;
    free(f1); free(f2);
  }
  *copied = r2;
  free(snap); drop(&d1); drop(&d2);
  return ok;
}

static void copies(void) {
  char d[320];
  /* ---- every state, several shapes: the same instrument either way ---- */
  { static const shape from[4] = { { 2, 12, 3, 32 }, { 1, 8, 1, 4 }, { 3, 9, 2, 10 },
                                   { IRIS_MAX_IN, IRIS_MAX_HID, IRIS_MAX_OUT, 6 } };
    static const shape to[4]   = { { 2, 12, 3, 32 }, { 1, 8, 1, 16 }, { 3, 9, 2, 10 },
                                   { IRIS_MAX_IN, IRIS_MAX_HID, IRIS_MAX_OUT, 6 } };
    int pairs = 0, wrong = 0, refused = 0;
    char which[160] = "";
    for (int sh = 0; sh < 4; ++sh)
      for (int st = 0; st < COPY_STATES; ++st) {
        box a = make(from[sh], 1234u);
        copy_source(a.k, from[sh], st);
        int copied = 0;
        pairs++;
        if (!copy_agrees(a.k, to[sh], 1, &copied)) {
          wrong++;
          snprintf(which, sizeof which, "; e.g. %d-%d-%d, %s", from[sh].ni, from[sh].nh,
                   from[sh].no, copy_state_name[st]);
        }
        if (!copied) refused++;
        drop(&a);
      }
    snprintf(d, sizeof d, "%d sources (4 shapes, %d states): %d differ from save and load, "
             "%d refused%s", pairs, COPY_STATES, wrong, refused, which);
    check("iris_copy leaves what iris_save and iris_load leave", wrong == 0 && refused == 0, d); }

  /* ---- every rule of the format, broken in the source ----------------
     A trained 2-12-3 source with eight takes (identifiers 1 to 8), one
     field changed. accept says what save and load answer, so that neither
     outcome can pass by the other; a shape change is made in the
     receiver instead. The last case is a weight decay iris_set_smoothing
     cannot produce: stored as the setting, weight decay / 0.3, and set
     again, it comes back one step of float resolution lower, and the copy
     must come back the same. */
  { typedef struct { const char *what; int accept; } rule;
    static const rule R[] = {
      { "unchanged", 1 }, { "a not-a-number weight", 0 }, { "an infinite hidden bias", 0 },
      { "a weight of minus infinity", 0 }, { "a not-a-number output bias", 0 },
      { "a weight past IRIS_W_LIMIT", 1 }, { "a not-a-number input in a take", 0 },
      { "an infinite output in a take", 0 }, { "an input range with lo above hi", 0 },
      { "an input range of zero width", 1 }, { "an infinite input range end", 0 },
      { "an output range of zero width", 0 }, { "a not-a-number output range end", 0 },
      { "an input range whose width overflows", 0 }, { "seed 0", 0 }, { "random state 0", 0 },
      { "next identifier 0", 0 }, { "next identifier -1", 0 },
      { "an identifier at the next identifier", 0 }, { "next identifier at IRIS_ID_LIMIT", 1 },
      { "an identifier repeated", 0 }, { "an identifier 0", 0 }, { "an identifier -3", 0 },
      { "identifiers out of order, all different", 1 }, { "more takes than the capacity", 0 },
      { "a count of takes of -1", 0 }, { "trained but never fitted", 0 },
      { "fitted 7 and trained -2 (true, as 1)", 1 }, { "never fitted, never trained", 1 },
      { "smoothing not a number", 0 }, { "smoothing above 1", 0 }, { "smoothing exactly 1", 1 },
      { "smoothing -0", 1 }, { "smoothing below 0", 0 },
      { "a receiver with another n_in", 0 }, { "a receiver with another n_hid", 0 },
      { "a receiver with another n_out", 0 }, { "a receiver holding 7 takes", 0 },
      { "a receiver holding exactly 8", 1 },
      { "more takes than the source holds, into a larger receiver", 0 },
      { "a weight decay the setting does not give back exactly", 1 } };
    const int nr = (int)(sizeof R / sizeof R[0]);
    int wrong = 0;
    char which[200] = "";
    for (int c = 0; c < nr; ++c) {
      box a = make(S2, 1234u);
      demos(a.k, S2, S2_DEMOS, 0);
      iris_train(a.k);
      iris *k = a.k;
      shape sd = S2;
      int save_ok = 1;
      switch (c) {
        case 1: k->w1[0] = bitsf(0x7FC00000u); break;
        case 2: k->b1[S2.nh - 1] = bitsf(0x7F800000u); break;
        case 3: k->w2[5] = bitsf(0xFF800000u); break;
        case 4: k->b2[0] = bitsf(0x7FC00000u); break;
        case 5: k->w2[0] = 20.0f; break;
        case 6: k->ex[0] = bitsf(0x7FC00000u); break;
        case 7: k->ex[S2_DEMOS * (S2.ni + S2.no) - 1] = bitsf(0x7F800000u); break;
        case 8: k->in_lo[0] = k->in_hi[0] + 1.0f; break;
        case 9: k->in_lo[1] = k->in_hi[1]; break;
        case 10: k->in_hi[0] = bitsf(0x7F800000u); break;
        case 11: k->out_lo[0] = k->out_hi[0]; break;
        case 12: k->out_hi[2] = bitsf(0x7FC00000u); break;
        case 13: k->in_lo[0] = -3e38f; k->in_hi[0] = 3e38f; break;
        case 14: k->seed = 0u; break;
        case 15: k->rng.s = 0u; break;
        case 16: k->next_id = 0; break;
        case 17: k->next_id = -1; break;
        case 18: k->next_id = 8; break;
        case 19: k->next_id = (int32_t)IRIS_ID_LIMIT; break;
        case 20: k->ex_id[3] = k->ex_id[1]; break;
        case 21: k->ex_id[0] = 0; break;
        case 22: k->ex_id[0] = -3; break;
        case 23: k->ex_id[0] = 2; k->ex_id[1] = 1; break;
        case 24: k->n_ex = S2.cap + 1; break;
        case 25: k->n_ex = -1; save_ok = 0; break;
        case 26: k->fitted = 0; k->trained = 1; break;
        case 27: k->fitted = 7; k->trained = -2; break;
        case 28: k->fitted = 0; k->trained = 0; break;
        case 29: k->l2 = bitsf(0x7FC00000u); break;
        case 30: k->l2 = 0.31f; break;
        case 31: k->l2 = 0.3f; break;
        case 32: k->l2 = -0.0f; break;
        case 33: k->l2 = -1e-9f; break;
        case 34: sd.ni = 3; break;
        case 35: sd.nh = 13; break;
        case 36: sd.no = 2; break;
        case 37: sd.cap = 7; break;
        case 38: sd.cap = 8; break;
        case 39:   /* only the source's capacity refuses: the row and the
                      identifier read past its store are valid ones */
          demos(k, S2, S2.cap - S2_DEMOS, 3);
          for (int i = 0; i < S2.cap; ++i) k->ex_id[i] = 100 + i;
          k->next_id = 200; k->order[0] = 7; k->n_ex = S2.cap + 1; sd.cap = 64;
          break;
        case 40: iris_internal_set_l2(k, bitsf(0x3A9A33E7u)); break;   /* 0.00117647357 */
        default: break;
      }
      int copied = -1;
      const int agree = copy_agrees(k, sd, save_ok, &copied);
      if (!agree || copied != R[c].accept) {
        wrong++;
        snprintf(which, sizeof which, "; e.g. %s: copy %d, agrees %d", R[c].what, copied, agree);
      }
      drop(&a);
    }
    snprintf(d, sizeof d, "%d cases: %d answered unlike save and load, or unlike the rule%s",
             nr, wrong, which);
    check("iris_copy refuses what save and load refuse, dst untouched", wrong == 0, d); }

  /* ---- random breakage ------------------------------------------------
     3,000 sources with one to three fields set to values near the edges of
     the rules, drawn at random: the answer and the receiver must match
     save and load's every time. */
  { unsigned st = 99u;
    int trials = 0, wrong = 0, accepted_n = 0;
    static const float V[] = { 0.0f, -0.0f, 1.0f, -1.0f, 20.0f, 1e-40f, 3e38f, -3e38f, 0.3f, 0.31f };
    for (int t = 0; t < 3000; ++t) {
      box a = make(S2, 1234u);
      demos(a.k, S2, S2_DEMOS, 0);
      if (t % 3) iris_train(a.k);
      iris *k = a.k;
      const int edits = 1 + (int)(t % 3);
      for (int e = 0; e < edits; ++e) {
        st = st * 1664525u + 1013904223u;
        const unsigned f = (st >> 8) % 12u, w = (st >> 16) % 13u, at = (st >> 20) % 8u;
        const float v = w < 10 ? V[w] : w == 10 ? bitsf(0x7FC00000u)
                      : w == 11 ? bitsf(0x7F800000u) : bitsf(0xFF800000u);
        const int iv = (int)(st >> 24) % 11 - 2;
        switch (f) {
          case 0: k->w1[at] = v; break;
          case 1: k->w2[at % (unsigned)(S2.no * S2.nh)] = v; break;
          case 2: k->in_lo[at % 2u] = v; break;
          case 3: k->in_hi[at % 2u] = v; break;
          case 4: k->out_lo[at % 3u] = v; break;
          case 5: k->out_hi[at % 3u] = v; break;
          case 6: k->ex[at * 5u + at % 5u] = v; break;
          case 7: k->l2 = v; break;
          case 8: k->ex_id[at] = iv; break;
          case 9: k->next_id = iv + 7; break;
          case 10: k->fitted = iv; break;
          default: k->trained = iv; break;
        }
      }
      int copied = 0;
      trials++;
      if (!copy_agrees(k, S2, 1, &copied)) wrong++;
      accepted_n += copied;
      drop(&a);
    }
    snprintf(d, sizeof d, "%d random sources (%d copied, %d refused): %d unlike save and load",
             trials, accepted_n, trials - accepted_n, wrong);
    check("iris_copy agrees with save and load on random breakage",
          wrong == 0 && accepted_n > 0 && accepted_n < trials, d); }

  /* ---- dst == src ----------------------------------------------------
     iris_copy(k, k) is iris_save and iris_load of k into itself: two
     instruments with one history, in the middle of a sliced run, one saved
     and loaded into itself, the other copied onto itself, must end as the
     same instrument, at rest. With a not-a-number weight, both refuse and
     the copy leaves every byte. */
  { box a = copy_receiver(S2), b = copy_receiver(S2);
    const int busy = iris_train_busy(a.k);
    const size_t n = iris_save_size(b.k);
    unsigned char *f = (unsigned char *)xmalloc(n);
    const int r1 = iris_save(b.k, f, n) == n && iris_load(b.k, f, n);
    const int r2 = iris_copy(a.k, a.k);
    const int same = same_instrument(a.k, b.k);
    const int rest = !iris_train_busy(a.k) && iris_get_status(a.k) == IRIS_STATUS_OK
                  && a.k->lr == 0.10f && a.k->v_w1[0] == 0.0f;
    a.k->w1[3] = bitsf(0x7FC00000u);
    unsigned char *snap = (unsigned char *)xmalloc(a.bytes);
    memcpy(snap, a.mem, a.bytes);
    const int r3 = iris_copy(a.k, a.k);
    const int kept = memcmp(snap, a.mem, a.bytes) == 0;
    const int s3 = iris_save(a.k, f, n) != 0;
    snprintf(d, sizeof d, "busy before %d; save+load %d, copy %d, same %d, at rest %d; "
             "poisoned: copy %d, arena kept %d, save %d", busy, r1, r2, same, rest, r3, kept, s3);
    check("iris_copy(k, k) is a save and a load of k into itself",
          busy && r1 && r2 && same && rest && r3 == 0 && kept && s3 == 0, d);
    free(f); free(snap); drop(&a); drop(&b); }

  /* ---- null arguments ------------------------------------------------ */
  { box a = copy_receiver(S2);
    unsigned char *snap = (unsigned char *)xmalloc(a.bytes);
    memcpy(snap, a.mem, a.bytes);
    const int r1 = iris_copy(0, a.k), r2 = iris_copy(a.k, 0), r3 = iris_copy(0, 0);
    const int same = memcmp(snap, a.mem, a.bytes) == 0;
    snprintf(d, sizeof d, "copy(NULL, src) %d, copy(dst, NULL) %d, copy(NULL, NULL) %d, "
             "arena same %d", r1, r2, r3, same);
    check("iris_copy refuses a null instrument", !r1 && !r2 && !r3 && same, d);
    free(snap); drop(&a); }
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
  copies();
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

  /* iris_shape reports what iris_init took, skips a null pointer, and
     refuses a null instrument without writing; the shape it reports is the
     one a saved file carries at bytes 16 to 24. */
  { char d[160];
    box a = make(S2, 3u);
    demos(a.k, S2, 4, 0);
    size_t n = 0; unsigned char *f = save(a.k, &n);
    int ni = -1, nh = -1, no = -1, cap = -1, x = -7;
    const int r = iris_shape(a.k, &ni, &nh, &no, &cap);
    const int r_skip = iris_shape(a.k, 0, &x, 0, 0);
    int y = -7; const int r_null = iris_shape(0, &y, &y, &y, &y);
    snprintf(d, sizeof d, "returned %d: %d-%d-%d cap %d; with nulls %d (hidden %d); "
             "null instrument %d, untouched %d", r, ni, nh, no, cap, r_skip, x, r_null, y == -7);
    check("iris_shape reports the shape the file carries",
          r == 1 && ni == S2.ni && nh == S2.nh && no == S2.no && cap == S2.cap
          && (uint32_t)ni == rd32(f, 16) && (uint32_t)nh == rd32(f, 20)
          && (uint32_t)no == rd32(f, 24)
          && r_skip == 1 && x == S2.nh && r_null == 0 && y == -7, d);
    free(f); drop(&a); }

  if (fails) printf("\n  %d of %d FAILING\n\n", fails, checks);
  else       printf("\n  all %d pass\n\n", checks);
  return fails ? 1 : 0;
}

#endif
