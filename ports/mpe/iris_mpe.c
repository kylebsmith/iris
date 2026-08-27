/* iris_mpe.c — the note engine. Read iris_mpe.h first; the decisions are there.
   The wire layer it sits on is iris_mpe_wire.c. */

#include "iris_mpe_wire.h"

static void forget(iris_mpe_voice *v) {
  v->note = -1; v->bend = IRIS_MPE_UNSENT_14;
  v->press = IRIS_MPE_UNSENT_7; v->slide = IRIS_MPE_UNSENT_7;
  v->state = IRIS_MPE_SILENT; v->step = 0; v->settle = 0; v->ch = -1; v->shared = 0;
}

void iris_mpe_rearm(iris_mpe *m) {
  int i;
  if (!m) return;
  m->cfg_step = 0;
  iris_mpe_pool_init(&m->pool, m->members);
  for (i = 0; i < IRIS_MPE_MAX_VOICES; ++i) forget(&m->v[i]);
}

/* ---- the two bursts, both RESUMABLE -------------------------------------
   If the transport refuses message k the cursor stays at k, IRIS_E_BUSY comes
   back, and the next frame resumes at k: nothing duplicated, nothing lost,
   and a congested FIFO is not fed messages the host already has. Both bursts
   use LATCHED values, so one spanning two frames is internally consistent.

   NOTE ON is five messages, expression strictly BEFORE the Note On (v1.1
   §2.4, §2.2.6/7/8) or the receiver starts the note on its own stale values —
   the "swooping" bug v1.0 §3.3.1 names. Pressure zeroed first, real pressure
   last: D-MPE-4. */
static int attack(iris_mpe *m, iris_mpe_voice *v) {
  unsigned ch = (unsigned)iris_mpe_member_ch(m, v->ch);
  while (v->step < 5) {
    int rc;
    switch (v->step) {
      case 0:  rc = M3(m, 0xE0u|ch, v->l_bend & 127, v->l_bend >> 7); break;
      case 1:  rc = M3(m, 0xB0u|ch, IRIS_MPE_CC_SLIDE, v->l_slide);     break;
      case 2:  rc = M2(m, 0xD0u|ch, 0);                               break;
      case 3:  rc = M3(m, 0x90u|ch, v->l_note, v->l_vel);             break;
      default: rc = M2(m, 0xD0u|ch, v->l_press);                      break;
    }
    if (rc < 0) { m->stalls++; return rc; }
    v->step++;
  }
  v->note = v->l_note; v->bend = v->l_bend;                /* only now is the */
  v->press = v->l_press; v->slide = v->l_slide;            /* cache true      */
  v->state = IRIS_MPE_SOUND; v->step = 0; m->notes++;
  return v->shared ? IRIS_W_SHARED : IRIS_OK;
}

/* NOTE OFF, four messages. Final bend and CC74 go BEFORE it (v1.1 A.4.1 is a
   "shall"), then pressure is zeroed, then an explicit 0x8n with release
   velocity 64 — 0 is not neutral on a synth that maps release velocity to
   release time, and v1.0 §3.3.2 reads note-on-velocity-0 as 64 anyway. */
static int release(iris_mpe *m, iris_mpe_voice *v) {
  unsigned ch = (unsigned)iris_mpe_member_ch(m, v->ch);
  while (v->step < 4) {
    int rc;
    switch (v->step) {
      case 0:  rc = M3(m, 0xE0u|ch, v->l_bend & 127, v->l_bend >> 7); break;
      case 1:  rc = M3(m, 0xB0u|ch, IRIS_MPE_CC_SLIDE, v->l_slide);     break;
      case 2:  rc = M2(m, 0xD0u|ch, 0);                               break;
      default: rc = M3(m, 0x80u|ch, v->note, IRIS_MPE_REL_VEL);         break;
    }
    if (rc < 0) { m->stalls++; return rc; }
    v->step++;
  }
  /* THE MOST IMPORTANT LINE IN THIS FILE is the forget() below. It voids the
     channel's "last sent" cache, so the next note there re-sends bend, CC74
     and pressure UNCONDITIONALLY even when unchanged. Reusing a member
     channel without all three is the commonest way an MPE controller is built
     wrong — the new note inherits the old note's bend, up to four octaves of
     it — and a JUCE host masks it, so the bug ships. Never trust the
     receiver. tests/mpe_test.c asserts it byte for byte; see ADR 0002. */
  iris_mpe_pool_give(&m->pool, v->ch, v->note);
  forget(v);
  return IRIS_OK;
}

/* ---- one voice, one frame ----------------------------------------------- */
static int voice_frame(iris_mpe *m, iris_mpe_voice *v, const float *g) {
  int rc;
  switch (v->state) {
    case IRIS_MPE_ATTACK:  return attack(m, v);
    case IRIS_MPE_RELEASE: return release(m, v);
    case IRIS_MPE_SILENT:
      if (g[IRIS_MPE_GATE] < IRIS_MPE_GATE_ON) return IRIS_OK;
      v->state = IRIS_MPE_ARMED; v->settle = m->settle;
      return IRIS_OK;                       /* nothing on the wire yet, D-MPE-2 */
    case IRIS_MPE_ARMED:
      if (g[IRIS_MPE_GATE] <= IRIS_MPE_GATE_OFF) { v->state = IRIS_MPE_SILENT; return IRIS_OK; }
      if (--v->settle > 0) return IRIS_OK;
      v->l_note  = (unsigned char)(m->note_lo +
                     (int)(g[IRIS_MPE_PITCH] * (float)m->note_span + 0.5f));
      v->l_bend  = (int16_t)iris_mpe_bend14(m, g[IRIS_MPE_PITCH], v->l_note);
      v->l_press = iris_mpe_q7(g[IRIS_MPE_PRESS]);
      v->l_slide = iris_mpe_q7(g[IRIS_MPE_SLIDE]);
      v->l_vel   = v->l_press ? v->l_press : 1;   /* velocity 0 is a note off */
      { int sh = 0; v->ch = (int8_t)iris_mpe_pool_take(&m->pool, v->l_note, &sh);
        v->shared = (uint8_t)sh; }
      v->state = IRIS_MPE_ATTACK; v->step = 0;
      return attack(m, v);

    default: break;                                          /* IRIS_MPE_SOUND */
  }

  if (g[IRIS_MPE_GATE] <= IRIS_MPE_GATE_OFF) {
    v->l_bend  = (int16_t)iris_mpe_bend14(m, g[IRIS_MPE_PITCH], v->note);
    v->l_slide = iris_mpe_q7(g[IRIS_MPE_SLIDE]);
    v->state = IRIS_MPE_RELEASE; v->step = 0;
    return release(m, v);
  }

  /* HELD. Only changed quantised values go out, so a still gesture emits
     nothing at all. Each cache entry is updated ONLY after the write that
     carried it succeeded: IRIS_E_BUSY costs one late frame, never a wrong host. */
  { unsigned ch = (unsigned)iris_mpe_member_ch(m, v->ch);
    int b14 = iris_mpe_bend14(m, g[IRIS_MPE_PITCH], v->note);
    unsigned char p7 = iris_mpe_q7(g[IRIS_MPE_PRESS]), s7 = iris_mpe_q7(g[IRIS_MPE_SLIDE]);
    if (b14 != v->bend) {
      if ((rc = M3(m, 0xE0u|ch, b14 & 127, b14 >> 7)) < 0) { m->stalls++; return rc; }
      v->bend = (int16_t)b14;
    }
    if (p7 != v->press) {
      if ((rc = M2(m, 0xD0u|ch, p7)) < 0) { m->stalls++; return rc; }
      v->press = p7;
    }
    if (s7 != v->slide) {
      if ((rc = M3(m, 0xB0u|ch, IRIS_MPE_CC_SLIDE, s7)) < 0) { m->stalls++; return rc; }
      v->slide = s7;
    }
  }
  return IRIS_OK;
}

/* ---- the sink vtable ---------------------------------------------------- */
static int mpe_send(iris_sink *s, const float *v) {
  iris_mpe *m = (iris_mpe *)s;                 /* iris_sink is the first member */
  int i, out = IRIS_OK;
  if (!iris_mpe_configured(m)) return IRIS_E_NOTREADY;
  m->frames++;
  for (i = 0; i < (int)m->voices; ++i) {
    int rc = voice_frame(m, &m->v[i], v + i * IRIS_MPE_DIMS);
    if (rc < 0) return rc;                 /* cursor kept; resume next frame */
    if (rc > 0) out = rc;
  }
  return out;
}

/* Release whatever is held, or the host sounds forever after we are gone. On
   IRIS_E_BUSY NOTHING HAS BEEN FORGOTTEN — cursor and note number are intact and
   the caller calls again (iris_sink_stop_tries). Clearing state on a failed
   write would destroy the only thing that makes recovery possible. */
static int mpe_stop(iris_sink *s) {
  iris_mpe *m = (iris_mpe *)s;
  int i, first = IRIS_OK;
  for (i = 0; i < (int)m->voices; ++i) {
    iris_mpe_voice *v = &m->v[i];
    int rc;
    if (v->state == IRIS_MPE_SILENT) continue;
    if (v->state == IRIS_MPE_ARMED) { v->state = IRIS_MPE_SILENT; continue; }
    if (v->state == IRIS_MPE_ATTACK) {            /* never a half-started note */
      rc = attack(m, v);
      if (rc < 0) { if (first == IRIS_OK) first = rc; continue; }
    }
    if (v->state != IRIS_MPE_RELEASE) {
      v->l_bend  = (v->bend  == IRIS_MPE_UNSENT_14) ? 8192 : v->bend;
      v->l_slide = (v->slide == IRIS_MPE_UNSENT_7)  ? 64   : v->slide;
      v->state = IRIS_MPE_RELEASE; v->step = 0;
    }
    rc = release(m, v);
    if (rc < 0 && first == IRIS_OK) first = rc;
  }
  return first;
}
int iris_mpe_init(iris_mpe *m, const iris_mpe_cfg *c) {
  int rc, n;
  if (!m || !c || !c->out || !c->out->write) return IRIS_E_CONFIG;
  if (c->voices  < 1 || c->voices  > IRIS_MPE_MAX_VOICES  ||
      c->members < 1 || c->members > IRIS_MPE_MAX_MEMBERS ||
      (c->zone != IRIS_MPE_LOWER && c->zone != IRIS_MPE_UPPER) ||
      c->bend_range < 1 || c->bend_range > 96 ||
      c->note_lo < 0 || c->note_span < 1 || c->note_lo + c->note_span > 127 ||
      c->settle < 1 || c->settle > 64) return IRIS_E_CONFIG;
  n = c->voices * IRIS_MPE_DIMS;
  if ((rc = iris_desc_check(iris_mpe_desc, n)) < 0) return rc;
  m->base.name = "mpe"; m->base.n = n; m->base.desc = iris_mpe_desc;
  m->base.send = mpe_send; m->base.stop = mpe_stop;
  m->base.budget.worst_us = 0; m->base.budget.p99_us = 0;
  m->base.budget.samples = 0;  m->base.budget.blocking = 0;
  m->out = c->out;
  m->voices = (uint8_t)c->voices;   m->members   = (uint8_t)c->members;
  m->zone   = (uint8_t)c->zone;     m->settle    = (uint8_t)c->settle;
  m->note_lo= (uint8_t)c->note_lo;  m->note_span = (uint8_t)c->note_span;
  m->bend_range = (uint8_t)c->bend_range;
  m->frames = m->notes = m->msgs = m->refused = m->stalls = 0;
  iris_mpe_rearm(m);
  return IRIS_OK;
}
