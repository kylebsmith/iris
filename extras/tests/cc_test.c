/* cc_test.c — does the CC sink emit the bytes iris_cc.h promises?
   Run:  ./build.sh sinks

   Every check is named after the decision in iris_cc.h it defends, so a check
   that fails tells you which paragraph is now a lie. The transport records
   whole messages and can be told to refuse, because a healthy link never
   exercises a back-pressure path and those are the paths that strand a
   receiver holding half a value. */

#include "../ports/cc/iris_cc.h"
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

/* ---- the clock the port is given ---------------------------------------- */
static int64_t clk;
static int64_t nowf(void) { return clk; }

/* ---- a transport that remembers, and can be told to refuse -------------- */
#define CAPN 8192
typedef struct {
  iris_bytes base;
  unsigned char b[CAPN];
  int start[CAPN], len[CAPN];
  int n, nb, allow, code, refused;
} cap;

static int cap_write(iris_bytes *w, const unsigned char *msg, int len) {
  cap *c = (cap *)w;
  if (c->allow == 0) { c->refused++; return c->code; }
  if (c->allow > 0) c->allow--;
  if (c->n >= CAPN || c->nb + len > CAPN) return IRIS_E_IO;
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

/* message i, as three bytes; -1 if it is not there or not three bytes long */
static int m0(const cap *c, int i) { return i < c->n && c->len[i] == 3 ? c->b[c->start[i]]     : -1; }
static int m1(const cap *c, int i) { return i < c->n && c->len[i] == 3 ? c->b[c->start[i] + 1] : -1; }
static int m2(const cap *c, int i) { return i < c->n && c->len[i] == 3 ? c->b[c->start[i] + 2] : -1; }

static int is_cc(const cap *c, int i, int ch, int num, int val) {
  return m0(c, i) == (0xB0 | ch) && m1(c, i) == num && m2(c, i) == val;
}

/* every message so far is a CC on `num` and on no other controller */
static int only_cc(const cap *c, int num) {
  int i;
  for (i = 0; i < c->n; ++i) if (m1(c, i) != num) return 0;
  return c->n > 0;
}

static cap  C;
static iris_cc M;

static int boot(int n, uint32_t min_us, const unsigned char *cc, const unsigned char *hi) {
  iris_cc_cfg cfg;
  cap_open(&C);
  clk = 1000000;
  cfg = iris_cc_defaults(&C.base, nowf, n);
  cfg.min_us = min_us; cfg.cc = cc; cfg.hi = hi;
  return iris_cc_init(&M, &cfg);
}
static int snd(const float *v, int n) { return iris_sink_send(&M.base, v, n); }

int main(void) {
  printf("\nCC SINK — the default output, byte for byte\n");
  printf("--------------------------------------------------------------------------------\n");

  /* ===================================================================== */
  printf("\nD-CC-1  the default map is UNDEFINED controllers\n");
  {
    int rc = boot(3, 0, 0, 0);
    ok("D-CC-1: three outputs default to CC 20, 21, 22",
       rc == IRIS_OK && iris_cc_number(&M, 0) == 20 && iris_cc_number(&M, 1) == 21 &&
       iris_cc_number(&M, 2) == 22, "rc %d  %d %d %d", rc,
       iris_cc_number(&M, 0), iris_cc_number(&M, 1), iris_cc_number(&M, 2));
    rc = boot(16, 0, 0, 0);
    ok("D-CC-1: past twelve it continues at 102, never at a defined CC",
       rc == IRIS_OK && iris_cc_number(&M, 11) == 31 && iris_cc_number(&M, 12) == 102 &&
       iris_cc_number(&M, 15) == 105, "%d %d %d",
       iris_cc_number(&M, 11), iris_cc_number(&M, 12), iris_cc_number(&M, 15));
  }
  {
    unsigned char bad[3] = {20, 21, 120}, dup[3] = {20, 21, 20};
    unsigned char alias[2] = {20, 52}, hi2[2] = {1, 0};
    unsigned char wide[2] = {40, 60}, hiw[2] = {1, 0};
    ok("D-CC-1: CC 120 is a Channel Mode message, not a controller: refused",
       boot(3, 0, bad, 0) == IRIS_E_CONFIG, "");
    ok("D-CC-1: two outputs on one CC is silent aliasing: refused",
       boot(3, 0, dup, 0) == IRIS_E_CONFIG, "");
    ok("D-CC-1: a 7-bit CC landing on a 14-bit output's LSB: refused",
       boot(2, 0, alias, hi2) == IRIS_E_CONFIG, "");
    ok("D-CC-1: a 14-bit MSB above 31 has no LSB partner: refused",
       boot(2, 0, wide, hiw) == IRIS_E_CONFIG, "");
  }

  /* ===================================================================== */
  printf("\nD-CC-2  change-only, hysteresis, rate floor\n");
  {
    float v[3] = {0.0f, 0.5f, 1.0f};
    boot(3, 0, 0, 0);
    snd(v, 3);
    ok("D-CC-2: the first frame sends all three, in index order, on channel 1",
       C.n == 3 && is_cc(&C, 0, 0, 20, 0) && is_cc(&C, 1, 0, 21, 64) &&
       is_cc(&C, 2, 0, 22, 127), "n %d  %02X %02X %02X",
       C.n, m2(&C, 0), m2(&C, 1), m2(&C, 2));
    ok("D-CC-2: the rails are reachable: 0.0 -> 0 and 1.0 -> 127 exactly",
       m2(&C, 0) == 0 && m2(&C, 2) == 127, "%d %d", m2(&C, 0), m2(&C, 2));
    cap_clear(&C);
    snd(v, 3); snd(v, 3); snd(v, 3);
    ok("D-CC-2: a still finger emits NOTHING at all, forever",
       C.n == 0, "n %d", C.n);
  }
  {
    /* 64/127 = 0.503937. Dither either side of the 0.5 boundary between 63
       and 64 must not become two messages a frame. */
    float lo[1] = {0.4999f}, hi[1] = {0.5001f}, far[1] = {0.52f};
    boot(1, 0, 0, 0);
    snd(lo, 1);
    cap_clear(&C);
    { int i; for (i = 0; i < 20; ++i) { snd(hi, 1); snd(lo, 1); } }
    ok("D-CC-2: 40 frames of dither across a quantiser boundary: 0 messages",
       C.n == 0 && M.dithers > 0, "n %d dithers %u", C.n, (unsigned)M.dithers);
    snd(far, 1);
    ok("D-CC-2: ...a real move of two steps still goes, immediately",
       C.n == 1 && m2(&C, 0) == 66, "n %d val %d", C.n, m2(&C, 0));
  }
  {
    float a[1] = {0.10f}, b[1] = {0.20f}, c[1] = {0.30f};
    boot(1, IRIS_CC_MIN_US, 0, 0);
    snd(a, 1); cap_clear(&C);
    clk += 1000;  snd(b, 1);
    ok("D-CC-2: a second move 1 ms later is HELD by the 3 ms floor",
       C.n == 0 && M.held == 1, "n %d held %u", C.n, (unsigned)M.held);
    clk += 3000;  snd(c, 1);
    ok("D-CC-2: ...and goes as soon as the floor expires, at its latest value",
       C.n == 1 && m2(&C, 0) == 38, "n %d val %d", C.n, m2(&C, 0));
  }

  /* ===================================================================== */
  printf("\nD-CC-3  14-bit is MSB then LSB, and the LSB is always re-sent\n");
  {
    unsigned char cc[1] = {20}, hi[1] = {1};
    float v[1];
    boot(1, 0, cc, hi);
    v[0] = 8192.0f / 16383.0f;
    snd(v, 1);
    ok("D-CC-3: one 14-bit output is two messages: MSB on 20, LSB on 52",
       C.n == 2 && is_cc(&C, 0, 0, 20, 64) && is_cc(&C, 1, 0, 52, 0),
       "n %d  %d/%d %d/%d", C.n, m1(&C, 0), m2(&C, 0), m1(&C, 1), m2(&C, 1));
    cap_clear(&C);
    v[0] = 8320.0f / 16383.0f;                  /* MSB 65, LSB 0 again */
    snd(v, 1);
    ok("D-CC-3: MSB moves, LSB unchanged: the LSB IS RE-SENT (the stale-LSB bug)",
       C.n == 2 && is_cc(&C, 0, 0, 20, 65) && is_cc(&C, 1, 0, 52, 0),
       "n %d  %d/%d %d/%d", C.n, m1(&C, 0), m2(&C, 0), m1(&C, 1), m2(&C, 1));
  }
  {
    unsigned char cc[1] = {20}, hi[1] = {1};
    float v[1] = {8192.0f / 16383.0f};
    int rc;
    boot(1, 0, cc, hi);
    C.allow = 1;                                /* the MSB lands, the LSB is refused */
    rc = snd(v, 1);
    ok("D-CC-3: transport refuses between MSB and LSB: IRIS_E_BUSY, MSB kept",
       rc == IRIS_E_BUSY && C.n == 1 && is_cc(&C, 0, 0, 20, 64) && M.owe_lsb[0] == 1,
       "rc %d n %d owe %d", rc, C.n, M.owe_lsb[0]);
    C.allow = -1; cap_clear(&C);
    rc = snd(v, 1);
    ok("D-CC-3: ...the retry sends the OWED LSB ONLY, never the MSB twice",
       rc == IRIS_OK && C.n == 1 && is_cc(&C, 0, 0, 52, 0),
       "rc %d n %d  %d/%d", rc, C.n, m1(&C, 0), m2(&C, 0));
  }

  /* ===================================================================== */
  printf("\nD-CC-4  the map is text, and a bad line changes nothing\n");
  {
    char buf[256];
    boot(3, 0, 0, 0);
    ok("D-CC-4: \"ch 10\" moves the wire channel to 9",
       iris_cc_parse(&M, "ch 10") == IRIS_OK && iris_cc_channel(&M) == 9,
       "ch %d", iris_cc_channel(&M));
    ok("D-CC-4: \"cc 2 30 hi\" makes output 2 a 14-bit pair on 30/62",
       iris_cc_parse(&M, "cc 2 30 hi") == IRIS_OK && iris_cc_number(&M, 1) == 30 &&
       iris_cc_is14(&M, 1), "%d hi %d", iris_cc_number(&M, 1), iris_cc_is14(&M, 1));
    ok("D-CC-4: \"cc 3 62\" would land on that pair's LSB: refused, unchanged",
       iris_cc_parse(&M, "cc 3 62") == IRIS_E_CONFIG && iris_cc_number(&M, 2) == 22,
       "out3 %d", iris_cc_number(&M, 2));
    ok("D-CC-4: \"cc 9 40\" names an output that does not exist: refused",
       iris_cc_parse(&M, "cc 9 40") == IRIS_E_CONFIG, "");
    ok("D-CC-4: \"ch 0\", \"ch 17\", \"nonsense\", \"\" are all refused",
       iris_cc_parse(&M, "ch 0") == IRIS_E_CONFIG && iris_cc_parse(&M, "ch 17") == IRIS_E_CONFIG &&
       iris_cc_parse(&M, "nonsense") == IRIS_E_CONFIG && iris_cc_parse(&M, "") == IRIS_E_CONFIG, "");
    ok("D-CC-4: \"rate 10\" is milliseconds on the wire, microseconds inside",
       iris_cc_parse(&M, "rate 10") == IRIS_OK && M.min_us == 10000u, "%u", (unsigned)M.min_us);
    iris_cc_print(&M, buf, sizeof buf);
    ok("D-CC-4: print renders the map a student can read back",
       strcmp(buf, "cc ch 10  rate 10 ms\ncc 1 20\ncc 2 30 hi +62\ncc 3 22\n") == 0,
       "\n%s", buf);
  }

  /* ===================================================================== */
  printf("\nD-CC-5  ASSIGN MODE: one CC moves, everything else is silent\n");
  {
    float v[3] = {0.1f, 0.9f, 0.4f};
    int i, rc;
    boot(3, 0, 0, 0);
    snd(v, 3); cap_clear(&C);
    rc = iris_cc_assign_begin(&M);
    ok("D-CC-5: begin arms output 0 and emits nothing by itself",
       rc == IRIS_OK && iris_cc_assign_state(&M) == IRIS_CC_ARMED &&
       iris_cc_assign_index(&M) == 0 && C.n == 0, "rc %d n %d", rc, C.n);

    v[0] = 0.8f; v[1] = 0.1f; v[2] = 0.9f;
    for (i = 0; i < 50; ++i) { snd(v, 3); clk += 1000; }
    ok("D-CC-5: 50 model frames while armed put ZERO bytes on the wire",
       C.n == 0, "n %d", C.n);

    cap_clear(&C); clk = 2000000;
    rc = iris_cc_assign_begin(&M);
    { int msgs = 0;
      for (i = 0; i < IRIS_CC_ASSIGN_STEPS; ++i) { msgs += iris_cc_assign_tick(&M); clk += 10000; }
      ok("D-CC-5: one full triangle is 254 messages at 10 ms — 100/s, 2.54 s",
         rc == IRIS_OK && msgs == IRIS_CC_ASSIGN_STEPS && C.n == IRIS_CC_ASSIGN_STEPS,
         "msgs %d", msgs); }
    ok("D-CC-5: ...all of them on CC 20 and on NOTHING else",
       only_cc(&C, 20), "n %d", C.n);
    ok("D-CC-5: ...the triangle: 0,1,2 up, 127 at the apex, 126 down, 1 last",
       m2(&C, 0) == 0 && m2(&C, 1) == 1 && m2(&C, 127) == 127 &&
       m2(&C, 128) == 126 && m2(&C, 253) == 1,
       "%d %d %d %d %d", m2(&C, 0), m2(&C, 1), m2(&C, 127), m2(&C, 128), m2(&C, 253));
    { int j, span = 1;
      for (j = 1; j < C.n; ++j) { int d = m2(&C, j) - m2(&C, j - 1); if (d < 0) d = -d; if (d > span) span = d; }
      ok("D-CC-5: ...and it never jumps: a ramp's wrap is what a DAW mislearns",
         span == 1, "largest step %d", span); }

    rc = iris_cc_assign_tick(&M);       /* due: the loop left the clock at +10 ms */
    cap_clear(&C);
    rc = iris_cc_assign_tick(&M);       /* immediately again, same microsecond */
    ok("D-CC-5: a second tick in the same microsecond emits nothing",
       rc == 0 && C.n == 0, "rc %d n %d", rc, C.n);

    cap_clear(&C);
    ok("D-CC-5: +1 advances to output 1, whose CC is 21",
       iris_cc_assign_step(&M, +1) == IRIS_CC_ARMED && iris_cc_assign_index(&M) == 1, "");
    rc = iris_cc_assign_tick(&M);
    ok("D-CC-5: ...and its first sweep message is immediate, on CC 21, at 0",
       C.n == 1 && is_cc(&C, 0, 0, 21, 0), "n %d %d/%d", C.n, m1(&C, 0), m2(&C, 0));
    ok("D-CC-5: -1 goes back to output 0; -1 at the first output clamps",
       iris_cc_assign_step(&M, -1) == IRIS_CC_ARMED && iris_cc_assign_index(&M) == 0 &&
       iris_cc_assign_step(&M, -1) == IRIS_CC_ARMED && iris_cc_assign_index(&M) == 0, "");
    iris_cc_assign_step(&M, +1); iris_cc_assign_step(&M, +1);
    ok("D-CC-5: past the last output is DONE, which emits nothing",
       iris_cc_assign_step(&M, +1) == IRIS_CC_DONE && iris_cc_assign_index(&M) == -1, "");
    cap_clear(&C); clk += 100000;
    { int t = iris_cc_assign_tick(&M);
    ok("D-CC-5: ...DONE really is silent",
       t == 0 && C.n == 0, "n %d", C.n); }
    ok("D-CC-5: ...one more advance leaves assign entirely",
       iris_cc_assign_step(&M, +1) == IRIS_CC_LIVE, "");

    cap_clear(&C);
    snd(v, 3);
    ok("D-CC-5: leaving assign re-sends ALL outputs, so the DAW snaps to the "
       "instrument", C.n == 3 && is_cc(&C, 0, 0, 20, 102) &&
       is_cc(&C, 1, 0, 21, 13) && is_cc(&C, 2, 0, 22, 114),
       "n %d %d %d %d", C.n, m2(&C, 0), m2(&C, 1), m2(&C, 2));
    ok("D-CC-5: assign_level is the value on the wire, for the field to breathe on",
       iris_cc_assign_level(&M) == 0.0f, "%f", (double)iris_cc_assign_level(&M));
  }

  /* ===================================================================== */
  printf("\nD-CC-6  stop() and the wrapper\n");
  {
    float v[3] = {0.3f, 0.3f, 0.3f};
    float bad[3] = {0.3f, 1.5f, 0.3f};
    boot(3, 0, 0, 0);
    snd(v, 3); cap_clear(&C);
    ok("D-CC-6: stop() puts NOTHING on the wire: a CC is a number, not a note",
       iris_sink_stop(&M.base) == IRIS_OK && C.n == 0, "n %d", C.n);
    snd(v, 3);
    ok("D-CC-6: ...but it voids the caches, so the next frame re-sends all three",
       C.n == 3, "n %d", C.n);
    cap_clear(&C);
    ok("D-CC-6: the shared wrapper rejects a vector outside [0,1] before we see it",
       snd(bad, 3) == IRIS_E_RANGE && C.n == 0, "n %d", C.n);
    ok("D-CC-6: ...and a vector of the wrong length",
       snd(v, 2) == IRIS_E_SHAPE, "");
  }
  {
    float v[3] = {0.9f, 0.9f, 0.9f};
    int rc;
    boot(3, 0, 0, 0);
    C.allow = 1;
    rc = snd(v, 3);
    ok("D-CC-6: a refusal stops the frame; later outputs keep their old cache",
       rc == IRIS_E_BUSY && C.n == 1 && M.sent[0] == 114 && M.sent[1] == IRIS_CC_UNSENT,
       "rc %d n %d sent %d %d", rc, C.n, M.sent[0], M.sent[1]);
    C.allow = -1; cap_clear(&C);
    rc = snd(v, 3);
    ok("D-CC-6: ...the retry sends exactly the two that never landed",
       rc == IRIS_OK && C.n == 2 && is_cc(&C, 0, 0, 21, 114) && is_cc(&C, 1, 0, 22, 114),
       "rc %d n %d", rc, C.n);
  }

  printf("--------------------------------------------------------------------------------\n");
  printf("cc_test: %s (%d failures)\n\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
