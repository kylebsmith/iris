/* make_golden.c — freeze the v0.1 baseline instrument, forever.
 *
 * Trains the audit's standard 20-example model (seed 1234, 800 epochs — the
 * exact check-5 recipe: init, record 20, iris_retrain_new(1234, 800)), saves it
 * with iris_save, and writes:
 *
 *   tests/golden/v1-instrument.bin   the format-v1 bytes, byte-for-byte
 *   tests/golden/v1-expected.txt     prediction BITS (hex uint32 per float)
 *                                    at 21 fixed probe points {a/20, 0.4}
 *   tests/golden/v3-instrument.bin   the same recipe under the v3 input
 *   tests/golden/v3-expected.txt     scaling ([-1,+1]), frozen at v0.3.0
 *
 * TWO FILES, BECAUSE THERE ARE TWO INPUT SCALINGS AND BOTH ARE PERMANENT.
 * The v1 pair is regenerated with iris_internal_set_legacy_norm(k, 1) so that it stays
 * byte-for-byte what it has always been — regenerating it must be a no-op,
 * and if it is not, something in the v1/v2 path has moved and the audit is
 * about to say so.
 *
 * The audit's golden check loads the .bin and compares prediction bits
 * against the .txt. Old saved instruments must keep loading forever; this
 * file is the proof. History of the frozen bytes: generated against v0.1.0
 * under the then-canonical build (cc -O2, Apple clang default fp-contract),
 * then regenerated ONCE — a deliberate baseline-redefinition, in the same
 * change — when `#pragma STDC FP_CONTRACT OFF` landed and moved every
 * trained float to the contraction-off bit class (fnv1a 0xFEFAEDF6, the
 * hash E8 recorded for that class; see docs/adr/0003). The v1 LAYOUT never
 * changed; only the frozen float values re-froze, once.
 *
 * Modes (for the determinism-envelope measurement):
 *   make_golden                          write all four files to tests/golden/
 *   make_golden <out.bin> <out.txt>      write the v1 pair to given paths
 *   make_golden print                    train (v1 scaling), print 21 hex lines
 *   make_golden print3                   train (v3 scaling), print 21 hex lines
 *   make_golden loadpredict <file.bin>   load a blob, print the 21 hex lines
 *
 * Build:  cc -O2 -o build/make_golden tests/golden/make_golden.c -lm
 * Run it from the library root (paths are relative), or pass paths.        */

#include "../../iris.h"
#include <stdio.h>
#include <string.h>

#define NI 2
#define NH 12
#define NO 3
#define CAP 256
#define GOLD_SEED 1234u
#define GOLD_EPOCHS 800
#define GOLD_NEX 20
#define GOLD_PROBES 21

static unsigned char arena[IRIS_ARENA(NI, NH, NO, CAP)];

/* Identical to audit.c / experiment.c — same truth, same example set. */
static void truth(float x, float y, float *o) {
  o[0] = 0.5f + 0.45f * iris_tanh(3.0f * (x - 0.5f));
  o[1] = 0.5f + 0.40f * iris_tanh(2.5f * (y - 0.5f) * (x + 0.3f));
  o[2] = 0.2f + 0.6f  * (x * y);
}

static void load_examples(iris *k, int n) {
  iris_clear(k);
  for (int i = 0; i < n; ++i) {
    float u = (float)((i * 7919) % 97) / 97.0f;
    float v = (float)((i * 6131) % 89) / 89.0f;
    float in[NI] = { u, v }, out[NO];
    truth(u, v, out);
    iris_record(k, in, out);
  }
}

static uint32_t fbits(float f) {
  uint32_t u; memcpy(&u, &f, sizeof u); return u;
}

/* The golden file is a FORMAT V1 file, forever — it exists to prove old
   files keep loading. Since v0.2, iris_save writes format v2 (v1 plus one
   rng word after the 8-word header), so regeneration converts the blob
   back to the v1 layout: drop word 8, stamp the version back to 1. */
static size_t to_v1_bytes(unsigned char *buf, size_t n) {
  uint32_t *h = (uint32_t *)buf;
  if (n >= 9 * sizeof(uint32_t) && (h[1] == 2u || h[1] == 3u)) {
    memmove(buf + 8 * sizeof(uint32_t), buf + 9 * sizeof(uint32_t),
            n - 9 * sizeof(uint32_t));
    h[1] = 1u;
    n -= sizeof(uint32_t);
  }
  return n;
}

/* fnv1a-32 over the blob — comparable with E8's recorded hashes. */
static uint32_t fnv1a(const unsigned char *p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
  return h;
}

static void print_probes(const iris *k, FILE *f) {
  for (int a = 0; a < GOLD_PROBES; ++a) {
    float in[NI] = { (float)a / 20.0f, 0.4f }, out[NO];
    iris_predict(k, in, out);
    fprintf(f, "%08x %08x %08x\n", fbits(out[0]), fbits(out[1]), fbits(out[2]));
  }
}

/* Train the frozen recipe under one of the two input scalings. */
static iris *make(int legacy) {
  iris *k = iris_init(arena, sizeof arena, NI, NH, NO, CAP, GOLD_SEED);
  if (!k) return 0;
  iris_internal_set_legacy_norm(k, legacy);
  load_examples(k, GOLD_NEX);
  iris_retrain_new(k, GOLD_SEED, GOLD_EPOCHS);
  return k;
}

static int write_pair(int legacy, const char *binpath, const char *txtpath) {
  static unsigned char buf[64 * 1024];
  iris *k = make(legacy);
  if (!k) { fprintf(stderr, "init failed\n"); return 1; }
  size_t n = iris_save(k, buf, sizeof buf);
  if (!n) { fprintf(stderr, "iris_save failed\n"); return 1; }
  /* The v1 golden is stored in the v1 LAYOUT, forever (ADR 0006). The v3
     golden is stored exactly as iris_save writes it — that is the point of it. */
  if (legacy) n = to_v1_bytes(buf, n);
  FILE *fb = fopen(binpath, "wb");
  if (!fb || fwrite(buf, 1, n, fb) != n) { fprintf(stderr, "cannot write %s\n", binpath); return 1; }
  fclose(fb);
  FILE *ft = fopen(txtpath, "w");
  if (!ft) { fprintf(stderr, "cannot write %s\n", txtpath); return 1; }
  print_probes(k, ft);
  fclose(ft);
  printf("wrote %s  (%zu bytes, fnv1a 0x%08X, format v%u, %d examples, seed %u, %d epochs)\n",
         binpath, n, fnv1a(buf, n), (unsigned)((uint32_t *)buf)[1], iris_count(k), iris_seed(k), GOLD_EPOCHS);
  printf("wrote %s  (%d probes x %d outputs, hex float bits)\n", txtpath, GOLD_PROBES, NO);
  return 0;
}

int main(int argc, char **argv) {
  iris *k = iris_init(arena, sizeof arena, NI, NH, NO, CAP, GOLD_SEED);
  if (!k) { fprintf(stderr, "init failed\n"); return 1; }

  if (argc >= 3 && strcmp(argv[1], "loadpredict") == 0) {
    static unsigned char buf[64 * 1024];
    FILE *fb = fopen(argv[2], "rb");
    if (!fb) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
    size_t n = fread(buf, 1, sizeof buf, fb);
    fclose(fb);
    if (!iris_load(k, buf, n)) { fprintf(stderr, "iris_load refused %s\n", argv[2]); return 1; }
    print_probes(k, stdout);
    return 0;
  }

  if (argc >= 2 && strcmp(argv[1], "print") == 0)  { print_probes(make(1), stdout); return 0; }
  if (argc >= 2 && strcmp(argv[1], "print3") == 0) { print_probes(make(0), stdout); return 0; }

  if (argc >= 3)
    return write_pair(1, argv[1], argv[2]);

  {
    /* v1 regenerates faithfully: the legacy writer still exists, so
       write_pair(1, ...) really does produce a format-v1 file.

       v3 DOES NOT, and must not be regenerated here. write_pair(0, ...) writes
       whatever the CURRENT save format is, which is now v4 — so this call used
       to overwrite the v3 backward-compatibility fixture with a v4 file every
       single time it ran, silently destroying the only artifact that proves we
       can still load a v3 instrument. It did exactly that on 2026-08-27 and the
       audit's v3 check flipped to a failure that looked like a code defect.

       A fixture whose whole purpose is to be OLD cannot be regenerated by the
       new code. It is a historical artifact: preserve it, never rebuild it. If
       it is ever lost, it comes back from git history, not from here. */
    return write_pair(1, "tests/golden/v1-instrument.bin", "tests/golden/v1-expected.txt");
  }
}
