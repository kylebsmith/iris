/* iris_cc.c — the default sink. Read iris_cc.h first; the decisions are there.
   No libc: the two text functions at the bottom carry their own digits. */

#include "iris_cc.h"

/* One static table, shared by every instance, exactly as iris_desc asks. The
   CC number is deliberately NOT in the unit string: it is per-instance and
   it belongs in iris_cc_print(), where it can be read and changed, not in a
   label that would then be a second place the map lives. dflt is 0.5, the
   least committing value an unnamed output can have — and it is never sent,
   because this sink is change-only and has nothing to say before the model
   says something. */
static const iris_desc iris_cc_desc[IRIS_CC_MAX] = {
  {"out 1","0..1",0.5f},{"out 2","0..1",0.5f},{"out 3","0..1",0.5f},{"out 4","0..1",0.5f},
  {"out 5","0..1",0.5f},{"out 6","0..1",0.5f},{"out 7","0..1",0.5f},{"out 8","0..1",0.5f},
  {"out 9","0..1",0.5f},{"out 10","0..1",0.5f},{"out 11","0..1",0.5f},{"out 12","0..1",0.5f},
  {"out 13","0..1",0.5f},{"out 14","0..1",0.5f},{"out 15","0..1",0.5f},{"out 16","0..1",0.5f}
};

/* D-CC-1: undefined controllers only. 20..31 first (their 14-bit LSB
   partners 52..63 are undefined too), then 102..119. */
static unsigned char dflt_cc(int k) {
  return (unsigned char)(k < 12 ? 20 + k : 102 + (k - 12));
}

/* ---- the wire ------------------------------------------------------------ */
static int cc__put(iris_cc *m, unsigned num, unsigned val) {
  unsigned char msg[3];
  int rc;
  msg[0] = (unsigned char)(0xB0u | (unsigned)m->channel);
  msg[1] = (unsigned char)(num & 0x7Fu);
  msg[2] = (unsigned char)(val & 0x7Fu);
  rc = iris_bytes_write(m->out, msg, 3);
  if (rc < 0) { m->refused++; return rc; }
  m->msgs++;
  return IRIS_OK;
}

/* ---- the quantiser, D-CC-2 ------------------------------------------------
   The rails bypass hysteresis: 0.0 and 1.0 must always be reachable, or an
   output trained to the top or bottom of its range could never get there. */
int iris_cc_quant(float v, int last, int max) {
  float s;
  int q;
  if (!(v > 0.0f)) return 0;               /* zero, negative, NaN */
  if (v >= 1.0f)   return max;             /* one, above, +inf    */
  s = v * (float)max;
  q = (int)(s + 0.5f);
  if (q > max) q = max;
  if (last >= 0) {
    if      (q == last + 1 && s < (float)last + 0.5f + IRIS_CC_HYST) q = last;
    else if (q == last - 1 && s > (float)last - 0.5f - IRIS_CC_HYST) q = last;
  }
  return q;
}

/* ---- the frame ----------------------------------------------------------- */
static int cc_send(iris_sink *s, const float *v) {
  iris_cc *m = (iris_cc *)s;                   /* iris_sink is the first member */
  uint32_t now;
  int k, rc;

  if (m->mode != IRIS_CC_LIVE) return IRIS_OK; /* assign owns the wire, D-CC-5 */
  now = (uint32_t)m->now_us();
  m->frames++;

  for (k = 0; k < (int)m->n; ++k) {
    int max = m->hi[k] ? 16383 : 127, q;

    /* An owed LSB is finished FIRST and unconditionally: the receiver is
       holding half a value until it lands, and the rate floor must never be
       the reason it keeps holding it. */
    if (m->owe_lsb[k]) {
      if ((rc = cc__put(m, (unsigned)(m->cc[k] + IRIS_CC_LSB_OFF),
                        (unsigned)(m->sent[k] & 127))) < 0) return rc;
      m->owe_lsb[k] = 0;
    }

    q = iris_cc_quant(v[k], m->sent[k], max);
    if (q != iris_cc_quant(v[k], IRIS_CC_UNSENT, max)) m->dithers++;  /* suppressed */
    if (q == m->sent[k]) continue;                         /* change-only */
    if (m->sent[k] >= 0 && m->min_us &&
        (uint32_t)(now - m->t_sent[k]) < m->min_us) { m->held++; continue; }

    if (m->hi[k]) {
      /* MSB then LSB, and the LSB goes even when unchanged (D-CC-3). The
         cache is updated the instant the MSB lands, with the remainder
         recorded, so a refusal between the two resumes correctly instead of
         re-sending an MSB the host already has. */
      if ((rc = cc__put(m, m->cc[k], (unsigned)(q >> 7))) < 0) return rc;
      m->sent[k] = (int16_t)q;
      m->t_sent[k] = now;
      m->owe_lsb[k] = 1;
      if ((rc = cc__put(m, (unsigned)(m->cc[k] + IRIS_CC_LSB_OFF),
                        (unsigned)(q & 127))) < 0) return rc;
      m->owe_lsb[k] = 0;
    } else {
      if ((rc = cc__put(m, m->cc[k], (unsigned)q)) < 0) return rc;
      m->sent[k] = (int16_t)q;
      m->t_sent[k] = now;
    }
  }
  return IRIS_OK;
}

/* D-CC-6: nothing on the wire. A CC is a number the host is holding, not a
   note sounding that only we can stop. */
static int cc_stop(iris_sink *s) {
  iris_cc_refresh((iris_cc *)s);
  return IRIS_OK;
}

void iris_cc_refresh(iris_cc *m) {
  int k;
  if (!m) return;
  for (k = 0; k < IRIS_CC_MAX; ++k) {
    m->sent[k] = IRIS_CC_UNSENT;
    m->owe_lsb[k] = 0;
    m->t_sent[k] = 0;
  }
}

/* ---- configuration -------------------------------------------------------- */
iris_cc_cfg iris_cc_defaults(iris_bytes *out, int64_t (*now_us)(void), int n) {
  iris_cc_cfg c;
  c.out = out; c.now_us = now_us; c.n = n;
  c.channel = 0; c.min_us = IRIS_CC_MIN_US; c.cc = 0; c.hi = 0;
  return c;
}

/* Every number an output occupies: the MSB always, the LSB when 14-bit. Two
   outputs may never share one, and an MSB may never land on another output's
   LSB — that is the aliasing bug this refuses to ship. */
static int map_clashes(const unsigned char *cc, const unsigned char *hi, int n) {
  unsigned char used[128];
  int i, j;
  for (i = 0; i < 128; ++i) used[i] = 0;
  for (i = 0; i < n; ++i) {
    int slots = hi[i] ? 2 : 1;
    for (j = 0; j < slots; ++j) {
      int num = cc[i] + j * IRIS_CC_LSB_OFF;
      if (num > 127 || used[num]) return 1;
      used[num] = 1;
    }
  }
  return 0;
}

static int map_check(const unsigned char *cc, const unsigned char *hi, int n) {
  int i;
  for (i = 0; i < n; ++i) {
    if (cc[i] > IRIS_CC_MAX_NUM) return IRIS_E_CONFIG;      /* 120.. = Channel Mode */
    if (hi[i] && cc[i] > IRIS_CC_MAX_MSB14) return IRIS_E_CONFIG;
  }
  return map_clashes(cc, hi, n) ? IRIS_E_CONFIG : IRIS_OK;
}

int iris_cc_init(iris_cc *m, const iris_cc_cfg *c) {
  int k, rc;
  if (!m || !c || !c->out || !c->out->write || !c->now_us) return IRIS_E_CONFIG;
  if (c->n < 1 || c->n > IRIS_CC_MAX) return IRIS_E_CONFIG;
  if (c->channel < 0 || c->channel > 15) return IRIS_E_CONFIG;
  if (c->min_us > 1000000u) return IRIS_E_CONFIG;
  for (k = 0; k < IRIS_CC_MAX; ++k) {
    m->cc[k] = k < c->n ? (c->cc ? c->cc[k] : dflt_cc(k)) : dflt_cc(k);
    m->hi[k] = (unsigned char)(k < c->n && c->hi && c->hi[k] ? 1 : 0);
  }
  if ((rc = map_check(m->cc, m->hi, c->n)) < 0) return rc;
  if ((rc = iris_desc_check(iris_cc_desc, c->n)) < 0) return rc;

  m->base.name = "cc"; m->base.n = c->n; m->base.desc = iris_cc_desc;
  m->base.send = cc_send; m->base.stop = cc_stop;
  m->base.budget.worst_us = 0; m->base.budget.p99_us = 0;
  m->base.budget.samples  = 0; m->base.budget.blocking = 0;
  m->out = c->out; m->now_us = c->now_us;
  m->n = (uint8_t)c->n; m->channel = (uint8_t)c->channel;
  m->min_us = c->min_us;
  m->frames = m->msgs = m->refused = m->held = m->dithers = 0;
  m->mode = IRIS_CC_LIVE; m->arm = 0; m->ph = 0; m->t_step = 0;
  iris_cc_refresh(m);
  return IRIS_OK;
}

int iris_cc_channel(const iris_cc *m) { return m ? (int)m->channel : -1; }
int iris_cc_number (const iris_cc *m, int k) {
  return (m && k >= 0 && k < (int)m->n) ? (int)m->cc[k] : -1;
}
int iris_cc_is14(const iris_cc *m, int k) {
  return (m && k >= 0 && k < (int)m->n) ? (int)m->hi[k] : 0;
}

/* ---- assign mode, D-CC-5 -------------------------------------------------- */
static void arm_at(iris_cc *m, int k) {
  m->arm = (uint8_t)k;
  m->ph  = 0;
  /* back-date the step clock so the very first tick emits immediately: an
     armed output that stayed silent for 10 ms would read as "nothing is
     happening" at exactly the moment the user is looking for movement. */
  m->t_step = (uint32_t)m->now_us() - IRIS_CC_ASSIGN_STEP_US;
}

int iris_cc_assign_begin(iris_cc *m) {
  if (!m || !m->now_us) return IRIS_E_CONFIG;
  iris_cc_refresh(m);              /* leaving assign must re-send everything */
  m->mode = IRIS_CC_ARMED;
  arm_at(m, 0);
  return IRIS_OK;
}

int iris_cc_assign_end(iris_cc *m) {
  if (!m) return IRIS_E_CONFIG;
  m->mode = IRIS_CC_LIVE;
  m->arm = 0; m->ph = 0;
  iris_cc_refresh(m);
  return IRIS_CC_LIVE;
}

int iris_cc_assign_step(iris_cc *m, int dir) {
  if (!m) return IRIS_E_CONFIG;
  if (m->mode == IRIS_CC_LIVE) return IRIS_CC_LIVE;
  if (dir > 0) {
    if (m->mode == IRIS_CC_DONE) return iris_cc_assign_end(m);
    if ((int)m->arm + 1 >= (int)m->n) { m->mode = IRIS_CC_DONE; m->ph = 0; return IRIS_CC_DONE; }
    arm_at(m, (int)m->arm + 1);
    return IRIS_CC_ARMED;
  }
  if (dir < 0) {
    if (m->mode == IRIS_CC_DONE) { m->mode = IRIS_CC_ARMED; arm_at(m, (int)m->n - 1); return IRIS_CC_ARMED; }
    if (m->arm > 0) arm_at(m, (int)m->arm - 1);
    else            arm_at(m, 0);      /* clamp: back at the first is a bump */
    return IRIS_CC_ARMED;
  }
  return m->mode;
}

int   iris_cc_assign_state(const iris_cc *m) { return m ? (int)m->mode : IRIS_CC_LIVE; }
int   iris_cc_assign_index(const iris_cc *m) {
  return (m && m->mode == IRIS_CC_ARMED) ? (int)m->arm : -1;
}

/* The triangle: ph 0..126 climbs 0..126, ph 127..253 descends 127..1, then
   wraps to 0. Every 7-bit value is hit once per direction and there is no
   discontinuity anywhere in the cycle. */
static int tri7(int ph) { return ph < 128 ? ph : IRIS_CC_ASSIGN_STEPS - ph; }

float iris_cc_assign_level(const iris_cc *m) {
  if (!m || m->mode != IRIS_CC_ARMED) return 0.0f;
  return (float)tri7((int)m->ph) / 127.0f;
}

int iris_cc_assign_tick(iris_cc *m) {
  uint32_t now;
  int rc;
  if (!m || !m->now_us) return IRIS_E_CONFIG;
  if (m->mode != IRIS_CC_ARMED) return 0;
  now = (uint32_t)m->now_us();
  if ((uint32_t)(now - m->t_step) < IRIS_CC_ASSIGN_STEP_US) return 0;
  m->t_step = now;
  /* 14-bit output: the MSB alone. A host that learns "whatever moved" must
     see one controller move, never two (D-CC-3). */
  rc = cc__put(m, m->cc[m->arm], (unsigned)tri7((int)m->ph));
  m->ph = (int16_t)((m->ph + 1) % IRIS_CC_ASSIGN_STEPS);
  return rc < 0 ? rc : 1;      /* a dropped sweep sample means nothing */
}

/* ==========================================================================
   THE MAP AS TEXT, D-CC-4. No libc, no sscanf, no locale, no allocation.
   ========================================================================== */
static const char *skip_sp(const char *p) { while (*p == ' ' || *p == '\t') ++p; return p; }

/* Match a bare word followed by a space or end. Returns the rest, or NULL. */
static const char *word(const char *p, const char *w) {
  p = skip_sp(p);
  while (*w) { if (*p != *w) return 0; ++p; ++w; }
  if (*p && *p != ' ' && *p != '\t') return 0;
  return p;
}

/* A non-negative decimal integer. Returns the rest, or NULL. */
static const char *number(const char *p, int *out) {
  int v = 0, got = 0;
  p = skip_sp(p);
  while (*p >= '0' && *p <= '9') {
    v = v * 10 + (*p - '0');
    if (v > 100000) return 0;
    ++p; got = 1;
  }
  if (!got) return 0;
  *out = v;
  return p;
}

static int end_of_line(const char *p) {
  p = skip_sp(p);
  return *p == 0 || *p == '\n' || *p == '\r';
}

int iris_cc_parse(iris_cc *m, const char *line) {
  const char *p;
  int a, b;
  if (!m || !line) return IRIS_E_CONFIG;

  if ((p = word(line, "ch")) != 0) {
    if (!(p = number(p, &a)) || !end_of_line(p)) return IRIS_E_CONFIG;
    if (a < 1 || a > 16) return IRIS_E_CONFIG;
    m->channel = (uint8_t)(a - 1);
    iris_cc_refresh(m);
    return IRIS_OK;
  }

  if ((p = word(line, "rate")) != 0) {
    if (!(p = number(p, &a)) || !end_of_line(p)) return IRIS_E_CONFIG;
    if (a > 1000) return IRIS_E_CONFIG;
    m->min_us = (uint32_t)a * 1000u;
    return IRIS_OK;
  }

  if ((p = word(line, "cc")) != 0) {
    unsigned char cc[IRIS_CC_MAX], hi[IRIS_CC_MAX];
    int k, wide;
    if (!(p = number(p, &a))) return IRIS_E_CONFIG;
    if (!(p = number(p, &b))) return IRIS_E_CONFIG;
    if (a < 1 || a > (int)m->n) return IRIS_E_CONFIG;
    if (b < 0 || b > IRIS_CC_MAX_NUM) return IRIS_E_CONFIG;
    if (end_of_line(p))                    wide = m->hi[a - 1];
    else if (word(p, "hi") && end_of_line(word(p, "hi"))) wide = 1;
    else if (word(p, "lo") && end_of_line(word(p, "lo"))) wide = 0;
    else return IRIS_E_CONFIG;
    /* Validate a COPY. A rejected line must leave the instrument exactly as
       it was, not half-applied. */
    for (k = 0; k < (int)m->n; ++k) { cc[k] = m->cc[k]; hi[k] = m->hi[k]; }
    cc[a - 1] = (unsigned char)b;
    hi[a - 1] = (unsigned char)wide;
    if (map_check(cc, hi, (int)m->n) < 0) return IRIS_E_CONFIG;
    for (k = 0; k < (int)m->n; ++k) { m->cc[k] = cc[k]; m->hi[k] = hi[k]; }
    iris_cc_refresh(m);
    return IRIS_OK;
  }

  return IRIS_E_CONFIG;
}

static int put_uint(char *b, int cap, int at, unsigned v) {
  char d[6];
  int i = 0;
  if (v == 0) d[i++] = '0';
  while (v && i < 6) { d[i++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
  while (i-- > 0) { if (at < cap - 1) b[at++] = d[i]; }
  return at;
}

static int put_str(char *b, int cap, int at, const char *s) {
  while (*s) { if (at < cap - 1) b[at++] = *s; ++s; }
  return at;
}

int iris_cc_print(const iris_cc *m, char *buf, int cap) {
  int at = 0, k;
  if (!m || !buf || cap < 2) return 0;
  at = put_str(buf, cap, at, "cc ch ");
  at = put_uint(buf, cap, at, (unsigned)m->channel + 1u);
  at = put_str(buf, cap, at, "  rate ");
  at = put_uint(buf, cap, at, m->min_us / 1000u);
  at = put_str(buf, cap, at, " ms\n");
  for (k = 0; k < (int)m->n; ++k) {
    at = put_str(buf, cap, at, "cc ");
    at = put_uint(buf, cap, at, (unsigned)k + 1u);
    at = put_str(buf, cap, at, " ");
    at = put_uint(buf, cap, at, m->cc[k]);
    if (m->hi[k]) {
      at = put_str(buf, cap, at, " hi +");
      at = put_uint(buf, cap, at, (unsigned)m->cc[k] + IRIS_CC_LSB_OFF);
    }
    at = put_str(buf, cap, at, "\n");
  }
  buf[at < cap ? at : cap - 1] = 0;
  return at;
}

/* ---- what this port costs, on a real 32-bit target -----------------------
   wasm32, the same honest proxy ports/mpe uses (same pointer width and
   alignment as the Xtensa S3). build.sh checks these so they cannot rot. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(iris_cc_cfg) == 28,  "iris_cc_cfg grew");
_Static_assert(sizeof(iris_cc)     == 224, "iris_cc grew");
#endif
