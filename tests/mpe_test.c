/* mpe_test.c — does the MPE sink emit the bytes the spec actually specifies?
   Run:  ./build.sh mpe

   Every check here is written against MPE v1.1 (MMA M1-100-UM) and against
   what LinnStrument, JUCE, Surge XT and Vital were read to do, not against
   what this encoder happens to produce. Where the two disagreed, the spec
   won and the disagreement is named in the check.

   The tests own a transport that records complete messages and can be told to
   refuse. That is the whole reason the encoder takes an iris_bytes instead of
   calling USB: a healthy link never exercises a back-pressure path, so the
   only way to know those paths work is to make an unhealthy one.           */

#include "../ports/mpe/iris_mpe.h"
#include "../ports/null/iris_null.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

static int failures = 0;
static void ok(const char *name, int pass, const char *fmt, ...) {
  va_list a; va_start(a, fmt);
  printf("%s  %-52s  ", pass ? "PASS" : "FAIL", name);
  vprintf(fmt, a); printf("\n"); va_end(a);
  if (!pass) failures++;
}

/* ==========================================================================
   A TRANSPORT THAT REMEMBERS, AND CAN BE TOLD TO REFUSE
   Message boundaries are kept, because the encoder's contract is one whole
   message per write and a test that flattened them could not see a violation.
   ========================================================================== */
#define CAPN 4096
typedef struct {
  iris_bytes base;
  unsigned char b[CAPN];
  int start[CAPN], len[CAPN];
  int n, nb;
  int allow;          /* -1 unlimited, else accept this many more */
  int code;           /* what to refuse with once allow hits 0 */
  int refused;
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

static char g_why[512];
static int stream_is(cap *c, const unsigned char *want, int n) {
  int i;
  if (c->nb != n) { sprintf(g_why, "%d bytes, wanted %d", c->nb, n); return 0; }
  for (i = 0; i < n; ++i)
    if (c->b[i] != want[i]) {
      sprintf(g_why, "byte %d is %02X, wanted %02X", i, c->b[i], want[i]);
      return 0;
    }
  sprintf(g_why, "%d bytes, %d messages", c->nb, c->n);
  return 1;
}

/* ---- driving the sink ---------------------------------------------------- */
static void frame(float *v, float g, float p, float pr, float sl) {
  v[0] = g; v[1] = p; v[2] = pr; v[3] = sl;
}
static int send1(iris_mpe *m, const float *v) { return iris_sink_send(&m->base, v, m->base.n); }
static int sendn(iris_mpe *m, const float *v, int n) {
  int rc = IRIS_OK, i;
  for (i = 0; i < n; ++i) rc = send1(m, v);
  return rc;
}
/* Bring a sink all the way up, then forget the configuration bytes so the
   note checks below start from an empty stream. */
static int bring_up(iris_mpe *m, cap *c, const iris_mpe_cfg *cfg) {
  int rc = iris_mpe_init(m, cfg);
  if (rc < 0) return rc;
  rc = iris_mpe_configure_step(m, 256);
  if (rc < 0) return rc;
  cap_clear(c);
  return IRIS_OK;
}

int main(void) {
  cap c; iris_mpe m; iris_mpe_cfg cfg;
  float v[IRIS_SINK_MAX_DIMS];
  char buf[256];
  int rc, i;

  printf("\nmpe_test — MPE v1.1 byte-level checks\n");
  printf("--------------------------------------------------------------------------------\n");

  /* ======================================================================
     1. THE ZONE CONFIGURATION MESSAGE
     ====================================================================== */
  cap_open(&c);
  cfg = iris_mpe_defaults(&c.base);
  rc = iris_mpe_init(&m, &cfg);
  ok("init with the defaults", rc == IRIS_OK, "voices %u, members %u, +/-%u st",
     m.voices, m.members, m.bend_range);

  ok("send refuses to play before the zone is configured",
     send1(&m, (frame(v, 1.0f, 0.5f, 1.0f, 0.5f), v)) == IRIS_E_NOTREADY && c.nb == 0,
     "IRIS_E_NOTREADY, %d bytes — a note at the host's default +/-2 would be 24x out", c.nb);

  ok("configuration is 15 messages for a 2-member zone",
     iris_mpe_config_steps(&m) == 15, "3 MCM + 6 per member channel");

  rc = iris_mpe_configure_step(&m, 256);
  ok("stepping the script completes it", rc == 15 && iris_mpe_configured(&m),
     "%d messages sent, configured=%d", rc, iris_mpe_configured(&m));

  {
    static const unsigned char want[] = {
      0xB0,0x65,0x00, 0xB0,0x64,0x06, 0xB0,0x06,0x02,      /* the one MCM */
      0xB1,0x65,0x00, 0xB1,0x64,0x00, 0xB1,0x06,0x30,      /* ch2: RPN 0 = 48 */
      0xB1,0x26,0x00, 0xB1,0x65,0x7F, 0xB1,0x64,0x7F,      /* CC38, RPN Null  */
      0xB2,0x65,0x00, 0xB2,0x64,0x00, 0xB2,0x06,0x30,      /* ch3: the same   */
      0xB2,0x26,0x00, 0xB2,0x65,0x7F, 0xB2,0x64,0x7F
    };
    ok("configuration bytes are exact (lower zone, v1.1 order)",
       stream_is(&c, want, (int)sizeof want), "%s", g_why);
  }
  ok("no second MCM disabling the other zone",
     c.n == 15, "%d messages — BF 06 00 turns MPE OFF in Surge XT", c.n);

  /* Upper zone members DESCEND from MIDI 15. Nothing above the port may
     assume ascending, so the port has to be asked. */
  {
    cap u; iris_mpe mu; iris_mpe_cfg cu;
    static const unsigned char want[] = {
      0xBF,0x65,0x00, 0xBF,0x64,0x06, 0xBF,0x06,0x02,
      0xBE,0x65,0x00, 0xBE,0x64,0x00, 0xBE,0x06,0x30,
      0xBE,0x26,0x00, 0xBE,0x65,0x7F, 0xBE,0x64,0x7F,
      0xBD,0x65,0x00, 0xBD,0x64,0x00, 0xBD,0x06,0x30,
      0xBD,0x26,0x00, 0xBD,0x65,0x7F, 0xBD,0x64,0x7F
    };
    cap_open(&u); cu = iris_mpe_defaults(&u.base); cu.zone = IRIS_MPE_UPPER;
    rc = iris_mpe_init(&mu, &cu); rc = (rc < 0) ? rc : iris_mpe_configure_step(&mu, 256);
    ok("upper zone: manager 16, members descend 15 then 14",
       rc == 15 && stream_is(&u, want, (int)sizeof want), "%s", g_why);
  }

  /* ======================================================================
     2. PACING, ABSENCE, REARM
     ====================================================================== */
  {
    cap p; iris_mpe mp; iris_mpe_cfg cp;
    cap_open(&p); cp = iris_mpe_defaults(&p.base);
    rc = iris_mpe_init(&mp, &cp);
    rc = iris_mpe_configure_step(&mp, 4);
    ok("a budget of 4 sends exactly 4 and no more",
       rc == 4 && p.n == 4 && !iris_mpe_configured(&mp), "%d messages, configured=%d",
       p.n, iris_mpe_configured(&mp));
    rc = iris_mpe_configure_step(&mp, 256);
    ok("the rest resumes where it stopped",
       rc == 11 && p.n == 15 && iris_mpe_configured(&mp), "%d then %d = 15 messages", 4, rc);
    rc = iris_mpe_configure_step(&mp, 256);
    ok("a completed script sends nothing more", rc == 0 && p.n == 15, "returns 0");

    iris_mpe_rearm(&mp);
    ok("rearm() restarts configuration after a replug",
       !iris_mpe_configured(&mp) &&
       send1(&mp, (frame(v, 1.0f, 0.5f, 1.0f, 0.5f), v)) == IRIS_E_NOTREADY,
       "back to step 0, send refuses again");
  }
  {
    cap a; iris_mpe ma; iris_mpe_cfg ca;
    cap_open(&a); a.allow = 0; a.code = IRIS_E_ABSENT;
    ca = iris_mpe_defaults(&a.base);
    rc = iris_mpe_init(&ma, &ca);
    rc = iris_mpe_configure_step(&ma, 256);
    ok("configuring an unplugged host reports absence and does not spin",
       rc == IRIS_E_ABSENT && a.refused == 1 && a.nb == 0,
       "one attempt, IRIS_E_ABSENT (\"%s\"), not IRIS_E_BUSY", iris_io_str(rc));
  }

  /* ======================================================================
     3. PITCH BEND ARITHMETIC — v1.1 Appendix C.3, NOT Table 4
     ====================================================================== */
  ok("centre is 8192", iris_mpe_bend14(&m, 0.5f, 60) == 8192,
     "pitch 0.5 on note 60 = %d", iris_mpe_bend14(&m, 0.5f, 60));
  ok("+7 semitones at range 48 is 9387 (Appendix C.4 worked example)",
     iris_mpe_bend14(&m, 7.0f / 48.0f, 36) == 9387,
     "pitch 7/48 above note 36 = %d", iris_mpe_bend14(&m, 7.0f / 48.0f, 36));
  {
    /* A quarter tone above note 60. round(0.5 * 8192 / 48) + 8192 = 8277,
       which is LSB 0x55, MSB 0x40. v1.1 Table 4 prints 2B 41 = 8363 and calls
       it a quarter tone; by the spec's OWN Appendix C.5 that is 1.0019
       semitones. The erratum is in v1.0 and v1.1 and has never been fixed. */
    int b = iris_mpe_bend14(&m, (60.5f - 36.0f) / 48.0f, 60);
    ok("a quarter tone is 8277 = 55 40, not Table 4's 2B 41",
       b == 8277 && (b & 127) == 0x55 && (b >> 7) == 0x40,
       "got %d = %02X %02X; Table 4's 2B 41 would be %d", b, b & 127, b >> 7, 0x41 * 128 + 0x2B);
  }
  ok("full bend up clamps to 16383, never 16384",
     iris_mpe_bend14(&m, 1.0f, 36) == 16383, "got %d (C.3's min(...))",
     iris_mpe_bend14(&m, 1.0f, 36));
  ok("full bend down reaches 0", iris_mpe_bend14(&m, 0.0f, 84) == 0,
     "got %d — the spec's own asymmetry, 0.6 cents", iris_mpe_bend14(&m, 0.0f, 84));
  {
    cap t; iris_mpe mt; iris_mpe_cfg ct;
    cap_open(&t); ct = iris_mpe_defaults(&t.base); ct.bend_range = 12; ct.note_span = 12;
    ct.note_lo = 60;
    rc = iris_mpe_init(&mt, &ct);
    ok("the range is not hardcoded: +7 st at range 12 is 12971",
       rc == IRIS_OK && iris_mpe_bend14(&mt, 7.0f / 12.0f, 60) == 12971,
       "got %d", iris_mpe_bend14(&mt, 7.0f / 12.0f, 60));
  }
  {
    int b = iris_mpe_bend14(&m, NAN, 60), lo = iris_mpe_bend14(&m, -INFINITY, 60);
    ok("bend14 is total: NaN and -inf cannot make a bad 14-bit value",
       b >= 0 && b <= 16383 && lo >= 0 && lo <= 16383, "NaN -> %d, -inf -> %d", b, lo);
  }
  ok("q7 is total: NaN, 2.0 and -1.0 all stay inside 0..127",
     iris_mpe_q7(NAN) == 0 && iris_mpe_q7(2.0f) == 127 && iris_mpe_q7(-1.0f) == 0 &&
     iris_mpe_q7(INFINITY) == 127,
     "0 / 127 / 0 / 127 — the second wall behind iris_sink_send");

  /* ======================================================================
     4. THE SETTLE WINDOW AND THE NOTE-ON BURST
     ====================================================================== */
  cap_open(&c); cfg = iris_mpe_defaults(&c.base);
  rc = bring_up(&m, &c, &cfg);

  frame(v, 1.0f, 0.5f, 1.0f, 0.5f);              /* note 60, full press, mid slide */
  rc = send1(&m, v);
  ok("the gate crossing sends nothing — it arms", rc == IRIS_OK && c.nb == 0,
     "%d bytes after the crossing frame", c.nb);
  rc = send1(&m, v);
  ok("still nothing one frame before the window closes", c.nb == 0,
     "settle = %u frames", m.settle);
  rc = send1(&m, v);
  ok("the burst fires after exactly `settle` frames", c.n == 5,
     "%d messages on the third frame", c.n);
  {
    static const unsigned char want[] = {
      0xE1,0x00,0x40,        /* bend, LSB first, centre                  */
      0xB1,0x4A,0x40,        /* CC 74 = 64                               */
      0xD1,0x00,             /* channel pressure ZEROED  (A.4.2, D-MPE-4)*/
      0x91,0x3C,0x7F,        /* note on, C4, velocity 127                */
      0xD1,0x7F              /* the real pressure, immediately after     */
    };
    ok("note-on bytes are exact, expression strictly before the note",
       stream_is(&c, want, (int)sizeof want), "%s", g_why);
  }

  cap_clear(&c);
  rc = sendn(&m, v, 200);
  ok("200 frames of a held gesture emit nothing at all", rc == IRIS_OK && c.n == 0,
     "%d messages — only changed quantised values go out", c.n);

  cap_clear(&c);
  frame(v, 1.0f, (63.0f - 36.0f) / 48.0f, 1.0f, 0.5f);   /* +3 semitones */
  rc = send1(&m, v);
  {
    static const unsigned char want[] = { 0xE1,0x00,0x44 };   /* 8704 */
    ok("a 3-semitone move sends one bend and nothing else",
       c.n == 1 && stream_is(&c, want, 3), "%s", g_why);
  }
  ok("the note number never changes while a note is sounding",
     m.v[0].note == 60, "still note %d — pitch is note + bend", m.v[0].note);

  cap_clear(&c);
  frame(v, 0.0f, 0.5f, 1.0f, 0.5f);
  rc = send1(&m, v);
  {
    static const unsigned char want[] = {
      0xE1,0x00,0x40,        /* final bend  BEFORE the note off (A.4.1 "shall") */
      0xB1,0x4A,0x40,        /* final CC 74 BEFORE the note off                 */
      0xD1,0x00,             /* pressure zeroed  (A.4.2)                        */
      0x81,0x3C,0x40         /* note off, RELEASE VELOCITY 0x40 — never 0       */
    };
    ok("note-off bytes are exact, release velocity 0x40",
       c.n == 4 && stream_is(&c, want, (int)sizeof want), "%s", g_why);
  }

  /* ======================================================================
     5. THE INVARIANT: NOTE OFF VOIDS THE CACHE
     A byte-identical next note must re-send all five messages. If it does
     not, the note inherits the previous note's bend, and at +/-48 semitones
     that is four octaves. A JUCE host hides this; a spec-literal one does not.
     ====================================================================== */
  {
    cap s1; iris_mpe m1; iris_mpe_cfg c1;
    static const unsigned char want[] = {
      0xE1,0x00,0x40, 0xB1,0x4A,0x40, 0xD1,0x00, 0x91,0x3C,0x7F, 0xD1,0x7F
    };
    cap_open(&s1); c1 = iris_mpe_defaults(&s1.base); c1.members = 1;  /* force reuse */
    rc = bring_up(&m1, &s1, &c1);
    frame(v, 1.0f, 0.5f, 1.0f, 0.5f); rc = sendn(&m1, v, 3);        /* note on  */
    frame(v, 0.0f, 0.5f, 1.0f, 0.5f); rc = send1(&m1, v);           /* note off */
    cap_clear(&s1);
    frame(v, 1.0f, 0.5f, 1.0f, 0.5f); rc = sendn(&m1, v, 3);        /* identical */
    ok("note off voids the cache: an IDENTICAL next note re-sends all five",
       s1.n == 5 && stream_is(&s1, want, (int)sizeof want), "%s", g_why);

    /* And the reverse. Bend the sounding note well off centre, release it, and
       land the next note on the same channel. The new note must carry its OWN
       bend before its Note On. If the encoder suppressed it as "unchanged",
       the note would inherit 8448 and start a semitone and a half sharp. */
    frame(v, 1.0f, (61.5f - 36.0f) / 48.0f, 1.0f, 0.5f);   /* +1.5 st -> 8448 */
    rc = send1(&m1, v);
    frame(v, 0.0f, (61.5f - 36.0f) / 48.0f, 1.0f, 0.5f);   /* release, bent   */
    rc = send1(&m1, v);
    ok("the release carries the bent value, before the note off",
       s1.b[s1.start[s1.n - 4]] == 0xE1 && s1.b[s1.start[s1.n - 4] + 2] == 0x42,
       "E1 00 42 = 8448, then B1 4A ..., D1 00, 81 3C 40 (A.4.1 is a \"shall\")");
    cap_clear(&s1);
    frame(v, 1.0f, (63.0f - 36.0f) / 48.0f, 1.0f, 0.5f);   /* note 63, centred */
    rc = sendn(&m1, v, 3);
    ok("no stale bend leaks: the new note bends itself first",
       s1.n == 5 && s1.b[0] == 0xE1 && s1.b[1] == 0x00 && s1.b[2] == 0x40 &&
       s1.b[8] == 0x91 && s1.b[9] == 63,
       "E1 00 40 (its own 8192, not the channel's 8448) then 91 3F");
  }

  /* ======================================================================
     6. CHANNEL ROTATION AND CHANNEL EXHAUSTION
     ====================================================================== */
  {
    cap r; iris_mpe mr; iris_mpe_cfg cr;
    int ch1, ch2;
    cap_open(&r); cr = iris_mpe_defaults(&r.base);
    rc = bring_up(&mr, &r, &cr);
    frame(v, 1.0f, 0.25f, 1.0f, 0.5f); rc = sendn(&mr, v, 3);
    ch1 = r.b[0] & 0x0F;
    frame(v, 0.0f, 0.25f, 1.0f, 0.5f); rc = send1(&mr, v);
    cap_clear(&r);
    frame(v, 1.0f, 0.75f, 1.0f, 0.5f); rc = sendn(&mr, v, 3);
    ch2 = r.b[0] & 0x0F;
    ok("consecutive notes at different pitches alternate member channels",
       ch1 == 1 && ch2 == 2, "MIDI channel %d then %d — a long release is not cut",
       ch1 + 1, ch2 + 1);

    /* A.3, last bullet: a free channel that last played this note number is
       preferred, so repeating one pitch does not stack or chorus. */
    frame(v, 0.0f, 0.75f, 1.0f, 0.5f); rc = send1(&mr, v);
    cap_clear(&r);
    frame(v, 1.0f, 0.75f, 1.0f, 0.5f); rc = sendn(&mr, v, 3);
    ok("the same pitch returns to the channel that last played it",
       (r.b[0] & 0x0F) == ch2, "MIDI channel %d again (A.3: no stacking)",
       (r.b[0] & 0x0F) + 1);
  }
  {
    /* Three voices, two member channels. MPE §2.2.4.1 is a "shall": the third
       note SHARES. It is not dropped and no older note is stolen. But sharing
       costs independent expression, so it must be reported, not swallowed. */
    cap x; iris_mpe mx; iris_mpe_cfg cx;
    int notes_on = 0, j;
    cap_open(&x); cx = iris_mpe_defaults(&x.base); cx.voices = 3; cx.members = 2;
    rc = bring_up(&mx, &x, &cx);
    frame(v + 0, 1.0f, 0.25f, 1.0f, 0.5f);
    frame(v + 4, 1.0f, 0.50f, 1.0f, 0.5f);
    frame(v + 8, 1.0f, 0.75f, 1.0f, 0.5f);
    rc = send1(&mx, v); rc = send1(&mx, v);
    rc = send1(&mx, v);
    for (j = 0; j < x.n; ++j) if ((x.b[x.start[j]] & 0xF0) == 0x90) notes_on++;
    ok("channel exhaustion shares, it does not drop the note",
       notes_on == 3, "%d note-ons for 3 voices over 2 member channels", notes_on);
    ok("channel exhaustion is REPORTED, not swallowed",
       rc == IRIS_W_SHARED && mx.pool.shared == 1,
       "send() -> %d (\"%s\"), pool.shared = %u", rc, iris_io_str(rc), mx.pool.shared);
    ok("the shared note went to a real member channel",
       mx.v[2].ch >= 0 && mx.v[2].ch < 2 && mx.pool.c[mx.v[2].ch].active == 2,
       "member %d now carries %u notes — one expressive voice, not two",
       mx.v[2].ch, mx.pool.c[mx.v[2].ch].active);

    /* Sharing is a consequence of the arithmetic, not a latch. Release the
       shared note AND the one it landed on top of, so a channel is genuinely
       free again, and the next note must be independent. */
    frame(v + 0, 0.0f, 0.25f, 1.0f, 0.5f);
    frame(v + 8, 0.0f, 0.75f, 1.0f, 0.5f);
    rc = send1(&mx, v);
    frame(v + 0, 1.0f, 0.25f, 1.0f, 0.5f);
    rc = send1(&mx, v); rc = send1(&mx, v); rc = send1(&mx, v);
    ok("once a channel frees up, sharing stops",
       rc == IRIS_OK && mx.pool.shared == 1 && mx.pool.c[mx.v[0].ch].active == 1,
       "%u shared notes in total, and member %d carries one note again",
       mx.pool.shared, mx.v[0].ch);
  }

  /* ======================================================================
     7. A BAD FLOAT IS CAUGHT AND REPORTED, NEVER EMITTED
     ====================================================================== */
  cap_open(&c); cfg = iris_mpe_defaults(&c.base);
  rc = bring_up(&m, &c, &cfg);
  frame(v, 1.0f, 0.5f, 1.0f, 0.5f); rc = sendn(&m, v, 3);       /* a live note */
  cap_clear(&c);
  {
    struct { const char *what; float p; } bad[] = {
      { "1.5",   1.5f }, { "-0.1", -0.1f }, { "NaN", NAN },
      { "+inf",  INFINITY }, { "-inf", -INFINITY }
    };
    int all = 1;
    for (i = 0; i < 5; ++i) {
      frame(v, 1.0f, bad[i].p, 1.0f, 0.5f);
      if (send1(&m, v) != IRIS_E_RANGE) all = 0;
    }
    ok("out of range, NaN and both infinities are all IRIS_E_RANGE",
       all && c.nb == 0, "5 bad frames, %d bytes emitted, \"%s\"", c.nb,
       iris_io_str(IRIS_E_RANGE));
  }
  ok("a refused frame leaves the sounding note exactly as it was",
     m.v[0].note == 60 && m.v[0].bend == 8192 && m.v[0].state == IRIS_MPE_SOUND,
     "note %d, bend %d, still sounding — no half-applied frame",
     m.v[0].note, m.v[0].bend);
  {
    frame(v, 1.0f, (63.0f - 36.0f) / 48.0f, 1.0f, 0.5f);
    rc = send1(&m, v);
    ok("and the next good frame carries on normally",
       rc == IRIS_OK && c.n == 1 && c.b[0] == 0xE1 && c.b[2] == 0x44,
       "one bend, E1 %02X %02X", c.b[1], c.b[2]);
  }
  ok("a vector of the wrong length is IRIS_E_SHAPE, not a truncation",
     iris_sink_send(&m.base, v, 3) == IRIS_E_SHAPE && iris_sink_send(&m.base, v, 8) == IRIS_E_SHAPE,
     "n_out must equal the sink's n (%d)", m.base.n);

  /* ======================================================================
     8. BACK-PRESSURE: A REFUSING TRANSPORT MUST SURFACE, NOT DROP
     ====================================================================== */
  {
    cap ref; iris_mpe mb; iris_mpe_cfg cb;
    unsigned char whole[16]; int wn;
    /* First, the reference: what an unobstructed note-on burst looks like. */
    cap_open(&ref); cb = iris_mpe_defaults(&ref.base);
    rc = bring_up(&mb, &ref, &cb);
    frame(v, 1.0f, 0.5f, 1.0f, 0.5f); rc = sendn(&mb, v, 3);
    wn = ref.nb; memcpy(whole, ref.b, (size_t)wn);

    /* Now the same note against a link that dies two messages in. */
    cap_open(&ref); cb = iris_mpe_defaults(&ref.base);
    rc = iris_mpe_init(&mb, &cb);
    rc = iris_mpe_configure_step(&mb, 256);
    cap_clear(&ref);
    ref.allow = 2;
    frame(v, 1.0f, 0.5f, 1.0f, 0.5f);
    rc = sendn(&mb, v, 3);
    ok("a refused message stops the burst there and says IRIS_E_BUSY",
       rc == IRIS_E_BUSY && ref.n == 2 && mb.stalls == 1 && mb.refused == 1,
       "2 of 5 messages, \"%s\", %u stall, %u refusal", iris_io_str(rc),
       mb.stalls, mb.refused);
    ref.allow = -1;
    rc = send1(&mb, v);
    ok("the resumed burst is byte-identical: nothing lost, nothing duplicated",
       rc == IRIS_OK && ref.nb == wn && memcmp(ref.b, whole, (size_t)wn) == 0,
       "%d bytes, matching the unobstructed burst", ref.nb);

    /* stop() is the one call that MUST eventually get through: a held note on
       a host we have left keeps sounding until the host is restarted. */
    ref.allow = 0;
    cap_clear(&ref);
    rc = iris_sink_stop(&mb.base);
    ok("stop() on a refusing link returns IRIS_E_BUSY and sends nothing",
       rc == IRIS_E_BUSY && ref.nb == 0, "\"%s\", %d bytes", iris_io_str(rc), ref.nb);
    ref.allow = -1;
    rc = iris_sink_stop_tries(&mb.base, 4);
    ok("stop() kept its state and released the note on retry",
       rc == IRIS_OK && ref.n == 4 &&
       ref.b[ref.start[3]] == 0x81 && ref.b[ref.start[3] + 2] == 0x40,
       "4 messages, note off 81 %02X 40", ref.b[ref.start[3] + 1]);
  }

  /* ======================================================================
     9. FRAMING LEGALITY AND THE PER-FRAME BUDGET, OVER A LONG SWEEP
     ====================================================================== */
  {
    cap s; iris_mpe ms; iris_mpe_cfg cs;
    int worst = 0, bad_status = 0, bad_len = 0, hi_bit = 0, on_manager = 0, poly = 0;
    int j, k;
    cap_open(&s); cs = iris_mpe_defaults(&s.base);
    rc = bring_up(&ms, &s, &cs);
    for (i = 0; i < 400; ++i) {
      float t = (float)i / 400.0f;
      float g = ((i / 37) % 2) ? 1.0f : 0.0f;
      int before = s.n;
      frame(v, g, t, 0.5f + 0.5f * (float)((i % 7) - 3) / 3.0f * 0.9f,
            (float)(i % 101) / 100.0f);
      if (v[2] < 0.0f) v[2] = 0.0f;
      if (v[2] > 1.0f) v[2] = 1.0f;
      rc = send1(&ms, v);
      if (rc < 0) break;
      if (s.n - before > worst) worst = s.n - before;
    }
    for (j = 0; j < s.n; ++j) {
      const unsigned char *b = s.b + s.start[j];
      int len = s.len[j], cin = b[0] >> 4, ch = b[0] & 0x0F;
      if (cin < 0x8 || cin > 0xE) bad_status++;
      if (((cin == 0xC || cin == 0xD) ? 2 : 3) != len) bad_len++;
      for (k = 1; k < len; ++k) if (b[k] & 0x80) hi_bit++;
      if ((cin == 0x9 || cin == 0x8) && ch == 0) on_manager++;
      if (cin == 0xA) poly++;
    }
    ok("400 swept frames: every message is legal MIDI",
       rc >= 0 && bad_status == 0 && bad_len == 0 && hi_bit == 0,
       "%d messages, 0 bad status, 0 bad length, 0 data bytes with bit 7 set", s.n);
    ok("no note ever lands on the Manager Channel", on_manager == 0,
       "channel 1 carries configuration and nothing else");
    ok("no Polyphonic Key Pressure anywhere — it is PROHIBITED on members",
       poly == 0, "0 x 0xAn messages (v1.1 Appendix E)");
    sprintf(buf, "%d messages, and the FIFO measured on the S3 holds 16 packets", worst);
    ok("worst case in one send() is the 5-message note-on burst",
       worst == IRIS_MPE_MAX_MSGS_PER_VOICE, "%s", buf);
  }

  /* ======================================================================
     10. THE WRAPPERS, THE BUDGET, THE NULL SINK, AND BAD WIRING
     ====================================================================== */
  {
    iris_null_sink z;
    static const iris_desc d3[3] = { {"a","u",0.0f}, {"b","u",0.5f}, {"c","u",1.0f} };
    rc = iris_null_sink_init(&z, 3, d3);
    ok("the null sink is a real sink", rc == IRIS_OK && z.base.n == 3, "3 dimensions");
    ok("an unmeasured endpoint refuses to arm",
       iris_sink_arm(&z.base) == IRIS_E_UNMEASURED, "\"%s\"", iris_io_str(IRIS_E_UNMEASURED));
    rc = iris_sink_measure(&z.base, 12, 9, 4096, 0);
    ok("a measured endpoint arms", rc == IRIS_OK && iris_sink_arm(&z.base) == IRIS_OK,
       "worst 12 us over 4096 samples");
    rc = iris_sink_measure(&z.base, 12, 9, 4096, 1);
    ok("an endpoint that admits it may block is refused",
       iris_sink_arm(&z.base) == IRIS_E_BLOCKING, "\"%s\"", iris_io_str(IRIS_E_BLOCKING));
    {
      float w[3] = { 0.0f, 0.5f, 1.0f };
      rc = iris_sink_send(&z.base, w, 3);
      w[1] = NAN;
      ok("the wrapper checks every sink, not just the MPE one",
         rc == IRIS_OK && z.frames == 1 && iris_sink_send(&z.base, w, 3) == IRIS_E_RANGE,
         "1 frame through, the NaN frame rejected");
    }
    {
      static const iris_desc bad_name[2] = { {"a","u",0.0f}, {NULL,"u",0.0f} };
      static const iris_desc bad_dflt[2] = { {"a","u",0.0f}, {"b","u",2.0f} };
      ok("a descriptor row with no name is refused at init",
         iris_null_sink_init(&z, 2, bad_name) == IRIS_E_CONFIG, "IRIS_E_CONFIG on the bench");
      ok("a descriptor default outside [0,1] is refused at init",
         iris_null_sink_init(&z, 2, bad_dflt) == IRIS_E_RANGE, "IRIS_E_RANGE on the bench");
    }
  }
  {
    cap b; iris_mpe mb; iris_mpe_cfg cb; int all = 1;
    cap_open(&b);
    cb = iris_mpe_defaults(&b.base); cb.voices = 0;       if (iris_mpe_init(&mb, &cb) != IRIS_E_CONFIG) all = 0;
    cb = iris_mpe_defaults(&b.base); cb.voices = 9;       if (iris_mpe_init(&mb, &cb) != IRIS_E_CONFIG) all = 0;
    cb = iris_mpe_defaults(&b.base); cb.members = 16;     if (iris_mpe_init(&mb, &cb) != IRIS_E_CONFIG) all = 0;
    cb = iris_mpe_defaults(&b.base); cb.members = 0;      if (iris_mpe_init(&mb, &cb) != IRIS_E_CONFIG) all = 0;
    cb = iris_mpe_defaults(&b.base); cb.bend_range = 0;   if (iris_mpe_init(&mb, &cb) != IRIS_E_CONFIG) all = 0;
    cb = iris_mpe_defaults(&b.base); cb.bend_range = 97;  if (iris_mpe_init(&mb, &cb) != IRIS_E_CONFIG) all = 0;
    cb = iris_mpe_defaults(&b.base); cb.settle = 0;       if (iris_mpe_init(&mb, &cb) != IRIS_E_CONFIG) all = 0;
    cb = iris_mpe_defaults(&b.base); cb.zone = 7;         if (iris_mpe_init(&mb, &cb) != IRIS_E_CONFIG) all = 0;
    cb = iris_mpe_defaults(&b.base); cb.note_lo = 100;    if (iris_mpe_init(&mb, &cb) != IRIS_E_CONFIG) all = 0;
    cb = iris_mpe_defaults(&b.base); cb.out = NULL;       if (iris_mpe_init(&mb, &cb) != IRIS_E_CONFIG) all = 0;
    ok("ten ways of wiring the sink up wrong are all refused at init", all,
       "on the bench, not as a wrong sound on stage");
  }
  {
    int codes[] = { IRIS_W_SHARED, IRIS_OK, IRIS_E_SHAPE, IRIS_E_RANGE, IRIS_E_IO, IRIS_E_BUSY,
                    IRIS_E_ABSENT, IRIS_E_CONFIG, IRIS_E_NOTREADY, IRIS_E_UNMEASURED,
                    IRIS_E_BLOCKING };
    int all = 1, longest = 0;
    for (i = 0; i < (int)(sizeof codes / sizeof codes[0]); ++i) {
      const char *s = iris_io_str(codes[i]);
      int n = s ? (int)strlen(s) : 999;
      if (!s || n >= 40) all = 0;
      if (n > longest) longest = n;
    }
    ok("every code has a message that fits one line of the panel", all,
       "longest is %d characters, the 8x8 font gives 40", longest);
  }

  printf("--------------------------------------------------------------------------------\n");
  printf("%s  (%d failed)\n\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", failures);
  return failures ? 1 : 0;
}
