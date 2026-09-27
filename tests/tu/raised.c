/* Translation unit that RAISED the maxima, as iris.h allows for a synthesiser
   with more than sixteen parameters: IRIS_MAX_IN 64, IRIS_MAX_HID 96 and
   IRIS_MAX_OUT 32. It builds an instrument of 3 inputs, 16 hidden units and
   31 outputs, more outputs than the default maxima allow, and holds it to
   what an instrument at the defaults does: it records, trains by both
   trainers, plays through every playing function, saves, loads into a second
   instrument that saves the same bytes and plays the same bits, and copies.
   The unit with the default maxima must then refuse it. Built under
   AddressSanitizer by sh build.sh tu, so a working array that did not grow
   with its maximum is a report. */
#define IRIS_MAX_IN  64
#define IRIS_MAX_HID 96
#define IRIS_MAX_OUT 32
#include "../../iris.h"
#include <stdio.h>
#include <string.h>

#define NI 3
#define NH 16
#define NO 31
#define CAP 16
static unsigned char A[IRIS_ARENA(NI, NH, NO, CAP)], B[IRIS_ARENA(NI, NH, NO, CAP)];
static unsigned char FILE31[IRIS_ARENA(NI, NH, NO, CAP)], AGAIN[IRIS_ARENA(NI, NH, NO, CAP)];
static unsigned char SCR[IRIS_ELM_SCRATCH(NH, NO)];
extern int tu_default_refuses(iris *k, int n_out);
int tu_raised(void);

static int fails = 0;
static void check(const char *name, int ok) {
  printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) fails++;
}

/* Every playing function at twelve readings, into one buffer per function;
   two instruments that play the same bits fill the same bytes. */
typedef struct { float net[12][NO], knn[12][NO], nn[12][NO]; int ids[12][3]; float d[12][3]; } played;
static void play(iris *k, played *p) {
  memset(p, 0, sizeof *p);
  for (int q = 0; q < 12; ++q) {
    const float in[NI] = { (float)q / 11.0f, 1.0f - (float)q / 22.0f, (float)(q % 3) };
    iris_predict(k, in, p->net[q]);
    iris_knn_predict(k, in, p->knn[q], 3);
    iris_classify_1nn(k, in, p->nn[q]);
    iris_nearest(k, in, p->ids[q], p->d[q], 3);
  }
}

int tu_raised(void) {
  static played pa, pb;
  iris *k = iris_init(A, sizeof A, NI, NH, NO, CAP, 1234u);
  iris *r = iris_init(B, sizeof B, NI, NH, NO, CAP, 99u);
  check("raised maxima: iris_init takes a 3-16-31 instrument", k && r);
  if (!k || !r) return fails;
  for (int t = 0; t < 12; ++t) {
    float in[NI], out[NO];
    for (int i = 0; i < NI; ++i) in[i] = (float)((t * 5 + i * 3) % 12) / 11.0f;
    for (int o = 0; o < NO; ++o) out[o] = 0.1f + 0.8f * (float)((t * 7 + o * 2) % 13) / 12.0f;
    iris_record(k, in, out);
  }
  check("raised maxima: iris_train fits 31 outputs", iris_train(k) && iris_is_trained(k)
        && iris_get_status(k) == IRIS_STATUS_OK);
  const size_t n = iris_save(k, FILE31, sizeof FILE31);
  const int loaded = n > 0 && iris_load(r, FILE31, n);
  play(k, &pa); play(r, &pb);
  check("raised maxima: saved, loaded, the same bytes saved and the same bits played",
        loaded && iris_save(r, AGAIN, sizeof AGAIN) == n && memcmp(FILE31, AGAIN, n) == 0
        && memcmp(&pa, &pb, sizeof pa) == 0);
  check("raised maxima: the closed-form trainer fits 31 outputs",
        iris_train_elm(k, 1e-3f, SCR, sizeof SCR) >= 0 && iris_is_trained(k));
  play(k, &pa);
  const int copied = iris_copy(r, k);
  play(r, &pb);
  check("raised maxima: iris_copy, and the copy plays the same bits",
        copied && memcmp(&pa, &pb, sizeof pa) == 0);
  check("raised maxima: the unit with the default maxima refuses it",
        tu_default_refuses(k, NO));
  return fails;
}
