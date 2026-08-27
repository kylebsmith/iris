/* iris_mpe_wire.c — bytes, channels, and the zone configuration script.
   Read iris_mpe.h first; the decisions are there. */

#include "iris_mpe_wire.h"

/* The training screen's four rows per voice, named once, in one place. */
const iris_desc iris_mpe_desc[IRIS_MPE_MAX_VOICES * IRIS_MPE_DIMS] = {
  {"gate 1","on >= 0.6",0.0f},{"pitch 1","note",0.5f},{"press 1","0..127",0.0f},{"slide 1","CC74",0.5f},
  {"gate 2","on >= 0.6",0.0f},{"pitch 2","note",0.5f},{"press 2","0..127",0.0f},{"slide 2","CC74",0.5f},
  {"gate 3","on >= 0.6",0.0f},{"pitch 3","note",0.5f},{"press 3","0..127",0.0f},{"slide 3","CC74",0.5f},
  {"gate 4","on >= 0.6",0.0f},{"pitch 4","note",0.5f},{"press 4","0..127",0.0f},{"slide 4","CC74",0.5f}
};

/* voices 1, members 2, lower zone, +/-48 semitones, notes 36..84, settle 2. */
iris_mpe_cfg iris_mpe_defaults(iris_bytes *out) {
  iris_mpe_cfg c;
  c.out = out; c.zone = IRIS_MPE_LOWER; c.voices = 1; c.members = 2;
  c.bend_range = 48; c.note_lo = 36; c.note_span = 48; c.settle = 2;
  return c;
}

int iris_mpe_manager_ch(const iris_mpe *m) { return m->zone == IRIS_MPE_UPPER ? 15 : 0; }

/* Lower Zone members ascend from MIDI 2; Upper Zone members DESCEND from
   MIDI 15. Nothing outside this function may assume either. */
int iris_mpe_member_ch(const iris_mpe *m, int member) {
  if (member < 0 || member >= (int)m->members) return iris_mpe_manager_ch(m);
  return m->zone == IRIS_MPE_UPPER ? 14 - member : 1 + member;
}

/* Total, and never able to produce a byte with bit 7 set, whatever it is
   handed. iris_sink_send has already rejected anything outside [0,1] and said
   so; this is the second wall, so a bad float can never become bad MIDI. */
unsigned char iris_mpe_q7(float v) {
  if (!(v > 0.0f)) return 0;        /* zero, negative, NaN, -inf */
  if (v >= 1.0f)   return 127;      /* one, above, +inf          */
  return (unsigned char)(v * 127.0f + 0.5f);
}

/* MPE v1.1 Appendix C.3, the SENDER equation, exactly:
       pbVal = min(round(semitones * 8192 / range) + 8192, 16383)
   Verified against the spec's own worked example: range 48, +7 semitones is
   9387. Do NOT implement this from v1.1 Table 4 — Table 4's bytes E2 2B 41
   decode to 8363, which by the spec's own Appendix C.5 receiver equation is
   1.0019 semitones, while the table labels them a quarter tone. A real
   quarter tone at range 48 is 8277 = E2 55 40. The error is in both v1.0 and
   v1.1 and has never been corrected. */
int iris_mpe_bend14(const iris_mpe *m, float pitch, int note) {
  float semis, delta; int b;
  if (!(pitch >= 0.0f && pitch <= 1.0f)) pitch = 0.5f;
  semis = (float)m->note_lo + pitch * (float)m->note_span;
  delta = semis - (float)note;
  b = (int)(delta * (8192.0f / (float)m->bend_range) + (delta >= 0.0f ? 0.5f : -0.5f));
  if (b >  8191) b =  8191;
  if (b < -8192) b = -8192;
  return b + 8192;
}

/* ---- one whole message, or none of it ----------------------------------- */
int iris_mpe__put(iris_mpe *m, unsigned st, unsigned a, int b) {
  unsigned char msg[3];
  int rc, len = (b < 0) ? 2 : 3;
  msg[0] = (unsigned char)st; msg[1] = (unsigned char)(a & 0x7Fu);
  msg[2] = (unsigned char)(b < 0 ? 0 : (b & 0x7F));
  rc = iris_bytes_write(m->out, msg, len);
  if (rc < 0) { m->refused++; return rc; }
  m->msgs++;
  return IRIS_OK;
}
/* ---- configuration, as a resumable script ------------------------------- */
int iris_mpe_config_steps(const iris_mpe *m) { return 3 + 6 * (int)m->members; }
int iris_mpe_configured(const iris_mpe *m)   { return m && m->cfg_step >= iris_mpe_config_steps(m); }

/* A function and not a table, so that changing `members` cannot leave a stale
   table behind. Steps 0..2 are the MCM on the Manager Channel; then six per
   member channel: RPN 0 = bend_range semitones, then RPN Null. */
static int cfg_msg(const iris_mpe *m, int step, unsigned char b[3]) {
  static const unsigned char mcm[3]  = {101, 100, 6};
  static const unsigned char rpn[6][2] = { {101,0},{100,0},{6,0},{38,0},{101,127},{100,127} };
  if (step < 0) return 0;
  if (step < 3) {
    b[0] = (unsigned char)(0xB0u + (unsigned)iris_mpe_manager_ch(m));
    b[1] = mcm[step];
    b[2] = (unsigned char)(step == 0 ? 0 : step == 1 ? 6 : m->members);
    return 1;
  }
  { int t = step - 3, c = t / 6, k = t % 6;
    if (c >= (int)m->members) return 0;
    b[0] = (unsigned char)(0xB0u + (unsigned)iris_mpe_member_ch(m, c));
    b[1] = rpn[k][0];
    b[2] = (k == 2) ? m->bend_range : rpn[k][1];
    return 1; }
}

int iris_mpe_configure_step(iris_mpe *m, int budget) {
  int sent = 0;
  if (!m || budget < 1) return IRIS_E_CONFIG;
  while (sent < budget && m->cfg_step < iris_mpe_config_steps(m)) {
    unsigned char b[3];
    int rc;
    if (!cfg_msg(m, m->cfg_step, b)) break;
    rc = M3(m, b[0], b[1], b[2]);
    if (rc < 0) { m->stalls++; return sent ? sent : rc; }
    m->cfg_step++; sent++;
  }
  return sent;
}

/* ---- what this port costs, asserted on a real 32-bit target -------------
   Xtensa and wasm32 agree on pointer width and on alignment, so wasm32 is a
   usable proxy for the S3 and an honest one to state: these are wasm32
   numbers, not numbers measured on the board. build.sh runs
   `clang --target=wasm32 -fsyntax-only` over this file so they cannot rot.
   The pool is a fixed 15 channels whatever the zone declares, because a
   compile-time array is the only kind this project has. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(iris_bytes)      ==   8, "iris_bytes grew");
_Static_assert(sizeof(iris_desc)       ==  12, "iris_desc grew");
_Static_assert(sizeof(iris_budget)     ==  16, "iris_budget grew");
_Static_assert(sizeof(iris_sink)       ==  36, "iris_sink grew");
_Static_assert(sizeof(iris_mpe_voice)  ==  18, "iris_mpe_voice grew");
_Static_assert(sizeof(iris_mpe_pool)   == 132, "iris_mpe_pool grew");
_Static_assert(sizeof(iris_mpe)        == 276, "iris_mpe grew");
#endif
