/* iris_osc.c — /wek/outputs as one line of ASCII. Read iris_osc.h first; the
   format is specified there byte for byte, and the decisions with it. */

#include "iris_osc.h"

static const iris_desc iris_osc_desc[IRIS_OSC_MAX] = {
  {"out 1","0..1",0.5f},{"out 2","0..1",0.5f},{"out 3","0..1",0.5f},{"out 4","0..1",0.5f},
  {"out 5","0..1",0.5f},{"out 6","0..1",0.5f},{"out 7","0..1",0.5f},{"out 8","0..1",0.5f},
  {"out 9","0..1",0.5f},{"out 10","0..1",0.5f},{"out 11","0..1",0.5f},{"out 12","0..1",0.5f},
  {"out 13","0..1",0.5f},{"out 14","0..1",0.5f},{"out 15","0..1",0.5f},{"out 16","0..1",0.5f}
};

/* ---- the field: EXACTLY eight bytes, "d.dddddd" --------------------------
   Single-precision throughout, deliberately: the S3 has no double FPU and a
   soft-double multiply on the gesture path would be the most expensive thing
   in this file. v*1e6f is one correctly-rounded IEEE-754 single operation and
   +0.5f is a second, so the digits are identical on the board and on the
   host, which is what lets the tests assert the bytes. */
int iris_osc_fmt(char *p, float v) {
  long u, ip, fr;
  int i;
  if (!(v > 0.0f)) v = 0.0f;               /* zero, negative, NaN */
  if (v > 1.0f)    v = 1.0f;
  u = (long)(v * 1000000.0f + 0.5f);
  if (u < 0)        u = 0;
  if (u > 1000000L) u = 1000000L;
  ip = u / 1000000L;
  fr = u % 1000000L;
  p[0] = (char)('0' + (int)ip);
  p[1] = '.';
  for (i = 7; i >= 2; --i) { p[i] = (char)('0' + (int)(fr % 10L)); fr /= 10L; }
  return IRIS_OSC_FIELD;
}

void iris_osc_refresh(iris_osc *o) { if (o) o->prev_n = 0; }

/* ---- the frame ----------------------------------------------------------- */
static int osc_send(iris_sink *s, const float *v) {
  iris_osc *o = (iris_osc *)s;                 /* iris_sink is the first member */
  uint32_t now;
  int i, k, at, rc;

  now = (uint32_t)o->now_us();
  o->frames++;

  if (o->prev_n && o->min_us && (uint32_t)(now - o->t_sent) < o->min_us) {
    o->held++;
    return IRIS_OK;
  }

  at = 0;
  for (i = 0; i < o->addr_n; ++i) o->line[at++] = o->addr[i];
  for (k = 0; k < (int)o->n; ++k) {
    o->line[at++] = ' ';
    at += iris_osc_fmt(o->line + at, v[k]);
  }
  o->line[at++] = '\n';
  o->line_n = at;

  /* Change-only, D-OSC-1. Both lines are the same length by construction, so
     this is a byte compare and not a parse. */
  if (o->prev_n == o->line_n) {
    for (i = 0; i < at; ++i) if (o->line[i] != o->prev[i]) break;
    if (i == at) { o->same++; return IRIS_OK; }
  }

  /* One line, including its newline, is ONE message: iris_bytes sends all of it
     or none of it, so a receiver splitting on '\n' can never see half a
     frame interleaved with something else. */
  rc = iris_bytes_write(o->out, (const unsigned char *)o->line, o->line_n);
  if (rc < 0) { o->refused++; return rc; }   /* prev untouched: retried next frame */

  for (i = 0; i < o->line_n; ++i) o->prev[i] = o->line[i];
  o->prev_n = o->line_n;
  o->t_sent = now;
  o->lines++;
  o->bytes += (uint32_t)o->line_n;
  return IRIS_OK;
}

/* D-OSC-3: nothing on the wire. */
static int osc_stop(iris_sink *s) {
  iris_osc_refresh((iris_osc *)s);
  return IRIS_OK;
}

/* ---- configuration -------------------------------------------------------- */
iris_osc_cfg iris_osc_defaults(iris_bytes *out, int64_t (*now_us)(void), int n) {
  iris_osc_cfg c;
  c.out = out; c.now_us = now_us; c.n = n;
  c.min_us = IRIS_OSC_MIN_US; c.addr = 0;
  return c;
}

/* OSC 1.0: an address starts with '/', is printable ASCII, and a LITERAL one
   contains none of the pattern characters. A space would also split our own
   line into two fields, which is the failure a receiver would report as
   "sometimes I get four floats". */
static int addr_ok(const char *a, int *len) {
  int i = 0;
  if (!a || a[0] != '/') return 0;
  while (a[i]) {
    unsigned char c = (unsigned char)a[i];
    if (c < 33 || c > 126) return 0;
    if (c == '#' || c == '*' || c == ',' || c == '?' ||
        c == '[' || c == ']' || c == '{' || c == '}') return 0;
    if (++i > IRIS_OSC_ADDR_MAX) return 0;
  }
  *len = i;
  return i > 0;
}

int iris_osc_init(iris_osc *o, const iris_osc_cfg *c) {
  const char *a;
  int i, rc, len = 0;
  if (!o || !c || !c->out || !c->out->write || !c->now_us) return IRIS_E_CONFIG;
  if (c->n < 1 || c->n > IRIS_OSC_MAX) return IRIS_E_CONFIG;
  if (c->min_us > 1000000u) return IRIS_E_CONFIG;
  a = c->addr ? c->addr : IRIS_OSC_ADDR;
  if (!addr_ok(a, &len)) return IRIS_E_CONFIG;
  /* The bound is arithmetic, not a guess: address + n*(space+field) + LF. */
  if (len + c->n * (1 + IRIS_OSC_FIELD) + 1 > IRIS_OSC_LINE) return IRIS_E_CONFIG;
  if ((rc = iris_desc_check(iris_osc_desc, c->n)) < 0) return rc;

  for (i = 0; i < len; ++i) o->addr[i] = a[i];
  o->addr[len] = 0;
  o->addr_n = len;
  o->base.name = "osc-text"; o->base.n = c->n; o->base.desc = iris_osc_desc;
  o->base.send = osc_send; o->base.stop = osc_stop;
  o->base.budget.worst_us = 0; o->base.budget.p99_us = 0;
  o->base.budget.samples  = 0; o->base.budget.blocking = 0;
  o->out = c->out; o->now_us = c->now_us;
  o->n = (uint8_t)c->n; o->min_us = c->min_us;
  o->line_n = o->prev_n = 0; o->t_sent = 0;
  o->frames = o->lines = o->bytes = o->refused = o->held = o->same = 0;
  return IRIS_OK;
}

#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(iris_osc_cfg) == 20,  "iris_osc_cfg grew");
_Static_assert(sizeof(iris_osc)     == 480, "iris_osc grew");
#endif
