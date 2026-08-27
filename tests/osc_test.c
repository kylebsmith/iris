/* osc_test.c — is the /wek/outputs line the bytes iris_osc.h promises?
   Run:  ./build.sh sinks

   A Max patch written for Wekinator slices this line by eye and by offset, so
   "close enough" is not a category here: the address, the single spaces, the
   eight-byte fields and the bare LF are all asserted literally. */

#include "../ports/osc/iris_osc.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static int failures = 0;
static void ok(const char *name, int pass, const char *fmt, ...) {
  va_list a; va_start(a, fmt);
  printf("%s  %-58s  ", pass ? "PASS" : "FAIL", name);
  vprintf(fmt, a); printf("\n"); va_end(a);
  if (!pass) failures++;
}

static int64_t clk;
static int64_t nowf(void) { return clk; }

#define CAPN 8192
typedef struct {
  iris_bytes base;
  char b[CAPN];
  int start[512], len[512];
  int n, nb, allow, code, refused;
} cap;

static int cap_write(iris_bytes *w, const unsigned char *msg, int len) {
  cap *c = (cap *)w;
  if (c->allow == 0) { c->refused++; return c->code; }
  if (c->allow > 0) c->allow--;
  if (c->n >= 512 || c->nb + len > CAPN) return IRIS_E_IO;
  c->start[c->n] = c->nb; c->len[c->n] = len; c->n++;
  memcpy(c->b + c->nb, msg, (size_t)len); c->nb += len;
  return IRIS_OK;
}
static void cap_open(cap *c) {
  memset(c, 0, sizeof *c);
  c->base.name = "capture"; c->base.write = cap_write;
  c->allow = -1; c->code = IRIS_E_BUSY;
}
static void cap_clear(cap *c) { c->n = 0; c->nb = 0; c->refused = 0; }

/* message i, compared as a whole: the boundary is part of the contract */
static int line_is(const cap *c, int i, const char *want) {
  int n = (int)strlen(want);
  if (i >= c->n || c->len[i] != n) return 0;
  return memcmp(c->b + c->start[i], want, (size_t)n) == 0;
}
static const char *line_of(const cap *c, int i, char *tmp) {
  int n = i < c->n ? c->len[i] : 0;
  memcpy(tmp, c->b + c->start[i], (size_t)n);
  tmp[n] = 0;
  return tmp;
}

static cap    C;
static iris_osc O;
static char   tmp[512];

static int boot(int n, uint32_t min_us, const char *addr) {
  iris_osc_cfg cfg;
  cap_open(&C);
  clk = 1000000;
  cfg = iris_osc_defaults(&C.base, nowf, n);
  cfg.min_us = min_us; cfg.addr = addr;
  return iris_osc_init(&O, &cfg);
}
static int snd(const float *v, int n) { return iris_sink_send(&O.base, v, n); }

int main(void) {
  printf("\nOSC-TEXT SINK — /wek/outputs, the Wekinator-faithful path\n");
  printf("--------------------------------------------------------------------------------\n");

  printf("\nTHE FORMAT\n");
  {
    float v[3] = {0.0f, 0.5f, 1.0f};
    boot(3, 0, 0);
    snd(v, 3);
    ok("format: address, one space per field, eight-byte fields, bare LF",
       line_is(&C, 0, "/wek/outputs 0.000000 0.500000 1.000000\n"),
       "\"%s\"", line_of(&C, 0, tmp));
    ok("format: a three-output line is always exactly 40 bytes",
       C.len[0] == 40, "%d", C.len[0]);
    ok("format: the last byte is 0x0A and there is no 0x0D anywhere",
       C.b[C.start[0] + C.len[0] - 1] == '\n' && !memchr(C.b, '\r', (size_t)C.nb), "");
    ok("format: ONE line is ONE iris_bytes message — a receiver can never see half",
       C.n == 1, "n %d", C.n);
  }
  {
    float v[4];
    v[0] = 1.0f / 3.0f; v[1] = 2.0f / 3.0f; v[2] = 0.000001f; v[3] = 0.9999999f;
    boot(4, 0, 0);
    snd(v, 4);
    ok("format: six decimals, round-half-up, no exponent, no locale, no libc",
       line_is(&C, 0, "/wek/outputs 0.333333 0.666667 0.000001 1.000000\n"),
       "\"%s\"", line_of(&C, 0, tmp));
  }
  {
    char f[9];
    f[8] = 0;
    iris_osc_fmt(f, -1.0f);
    ok("format: the second wall — a value below 0 still renders eight legal bytes",
       strcmp(f, "0.000000") == 0, "\"%s\"", f);
    iris_osc_fmt(f, 2.0f);
    ok("format: ...and one above 1",
       strcmp(f, "1.000000") == 0, "\"%s\"", f);
  }
  {
    float v[16];
    int i;
    for (i = 0; i < 16; ++i) v[i] = (float)i / 15.0f;
    boot(16, 0, 0);
    snd(v, 16);
    ok("format: sixteen outputs is 12 + 16*9 + 1 = 157 bytes, inside the buffer",
       C.n == 1 && C.len[0] == 157, "%d", C.n ? C.len[0] : -1);
  }

  printf("\nD-OSC-1  change-only and the rate floor\n");
  {
    float v[2] = {0.25f, 0.75f};
    boot(2, 0, 0);
    snd(v, 2); cap_clear(&C);
    snd(v, 2); snd(v, 2); snd(v, 2);
    ok("D-OSC-1: a still finger emits nothing: the line is compared, not the floats",
       C.n == 0 && O.same == 3, "n %d same %u", C.n, (unsigned)O.same);
    v[1] = 0.7500001f;
    snd(v, 2);
    ok("D-OSC-1: ...a change in the seventh decimal is not a change",
       C.n == 0, "n %d", C.n);
    v[1] = 0.751f;
    snd(v, 2);
    ok("D-OSC-1: ...a change at the sixth decimal is",
       C.n == 1 && line_is(&C, 0, "/wek/outputs 0.250000 0.751000\n"),
       "\"%s\"", line_of(&C, 0, tmp));
  }
  {
    float v[1] = {0.1f};
    boot(1, IRIS_OSC_MIN_US, 0);
    snd(v, 1); cap_clear(&C);
    clk += 5000;  v[0] = 0.2f; snd(v, 1);
    ok("D-OSC-1: 5 ms after a line, the 100 Hz floor holds the next one",
       C.n == 0 && O.held == 1, "n %d held %u", C.n, (unsigned)O.held);
    clk += 6000;  v[0] = 0.3f; snd(v, 1);
    ok("D-OSC-1: ...11 ms and it goes, carrying the latest value",
       C.n == 1 && line_is(&C, 0, "/wek/outputs 0.300000\n"),
       "\"%s\"", line_of(&C, 0, tmp));
  }

  printf("\nD-OSC-2  the address\n");
  {
    float v[1] = {0.5f};
    ok("D-OSC-2: the default address is Wekinator's own, unchanged",
       boot(1, 0, 0) == IRIS_OK && strcmp(O.addr, "/wek/outputs") == 0, "%s", O.addr);
    ok("D-OSC-2: a custom address is allowed, for two devices in one patch",
       boot(1, 0, "/wek/outputs/2") == IRIS_OK, "");
    snd(v, 1);
    ok("D-OSC-2: ...and it is the literal bytes of the line's head",
       line_is(&C, 0, "/wek/outputs/2 0.500000\n"), "\"%s\"", line_of(&C, 0, tmp));
    ok("D-OSC-2: no leading slash is not an OSC address: refused",
       boot(1, 0, "wek/outputs") == IRIS_E_CONFIG, "");
    ok("D-OSC-2: a space would split the line into a wrong number of fields: refused",
       boot(1, 0, "/wek out") == IRIS_E_CONFIG, "");
    ok("D-OSC-2: OSC pattern characters in a literal address: refused",
       boot(1, 0, "/wek/*") == IRIS_E_CONFIG && boot(1, 0, "/wek/{a,b}") == IRIS_E_CONFIG, "");
    ok("D-OSC-2: every line begins with '/', which is the console-sharing contract",
       boot(2, 0, 0) == IRIS_OK && (snd(v, 1), 1) && O.addr[0] == '/', "");
  }

  printf("\nD-OSC-3  stop, and back-pressure\n");
  {
    float v[2] = {0.4f, 0.6f};
    boot(2, 0, 0);
    snd(v, 2); cap_clear(&C);
    ok("D-OSC-3: stop() emits nothing: a receiver holds a number, not a note",
       iris_sink_stop(&O.base) == IRIS_OK && C.n == 0, "n %d", C.n);
    snd(v, 2);
    ok("D-OSC-3: ...but voids the cache, so the next frame states the values again",
       C.n == 1, "n %d", C.n);
  }
  {
    float v[1] = {0.4f};
    int rc;
    boot(1, 0, 0);
    C.allow = 0;
    rc = snd(v, 1);
    ok("D-OSC-3: a refused line is reported and NOT remembered",
       rc == IRIS_E_BUSY && O.refused == 1 && O.prev_n == 0, "rc %d", rc);
    C.allow = -1;
    rc = snd(v, 1);
    ok("D-OSC-3: ...so the very next frame sends it again, whole",
       rc == IRIS_OK && C.n == 1 && line_is(&C, 0, "/wek/outputs 0.400000\n"),
       "\"%s\"", line_of(&C, 0, tmp));
  }
  {
    float bad[2] = {0.5f, 2.0f}, good[2] = {0.5f, 0.5f};
    boot(2, 0, 0);
    ok("D-OSC-3: the shared wrapper rejects out-of-range before we render it",
       snd(bad, 2) == IRIS_E_RANGE && C.n == 0, "n %d", C.n);
    ok("D-OSC-3: ...and the wrong vector length",
       snd(good, 1) == IRIS_E_SHAPE, "");
  }

  printf("--------------------------------------------------------------------------------\n");
  printf("osc_test: %s (%d failures)\n\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
