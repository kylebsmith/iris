/* SPDX-License-Identifier: BSD-3-Clause
   Copyright (c) 2026 Kyle Smith */
/* ============================================================================
   iris_sink.h  —  where the parameter vector goes
   v0.2.0 · C99 · no dependencies · no malloc · no libc · no hardware

   iris.h is float[] in, float[] out and knows nothing else. This file and
   its companion iris_source.h are the only things in the system that know
   there is a world on either side of the model.

   TWO TYPES LIVE HERE. ONE FUNCTION POINTER EACH.

     iris_bytes   carries ONE COMPLETE MESSAGE   (USB, a UART, a capture buffer)
     iris_sink    consumes the parameter vector  (MPE, nothing)

   THE ONE CONTRACT: EVERY FLOAT CROSSING THIS BOUNDARY IS IN [0,1]
   ----------------------------------------------------------------
   Both directions, always, and iris_sink_send() CHECKS IT on every frame. That
   single rule deletes the per-channel range / units / scale-offset config
   surface that otherwise grows on an interface like this. A sink converts
   [0,1] into its own domain by a fixed, documented rule; a source converts
   raw sensor units into [0,1] inside the port. Everything in between — the
   example store, the network, normalisation, the save format — is already
   scale-free and stays that way.

   This does NOT weaken iris_fit_ranges(). That function derives its ranges from
   the examples actually recorded and is affine-invariant, so normalising
   inside a port is a change of variable that leaves the trained model alone.

   NaN and both infinities fail the same check. !(v >= 0 && v <= 1) is false
   for NaN and for +inf, and true-to-false for -inf, which is how they are
   rejected without math.h and without a single #include.

   WHY THE ERROR VOCABULARY LIVES IN THE SINK HEADER AND NOT THE SOURCE ONE
   -----------------------------------------------------------------------
   The two ends share one set of codes, one iris_desc and one [0,1] rule, and
   sharing them is the point — a source and a sink that disagreed about what
   "out of range" means would be two boundaries, not one. They are declared
   here because the sink is the end that cannot be omitted: a device with no
   output is not an instrument, whereas a device with no sensor is a perfectly
   ordinary test rig. So iris_source.h includes this file, and never the
   other way round.

   ERRORS, WITH NO malloc, NO libc, NO errno, NO callback
   -----------------------------------------------------
        < 0   one of the IRIS_E_* codes below. Nothing was delivered.
        = 0   IRIS_OK. Sinks: done, possibly with nothing to say.
        > 0   delivered, but the sink has something to tell you. The meaning
              is named by the sink's own header; IRIS_W_SHARED is the only one
              general enough to live here.

   iris_io_str() returns a pointer into .rodata — no buffer, no formatting, no
   allocation, safe from an interrupt. Every string is under 40 characters
   because that is one line of the 8x8 font on a 320-pixel panel, and an error
   you cannot read is an error you did not report.

   "Never fail silently" is enforced by the compiler rather than by
   discipline: every wrapper and every init below is warn_unused_result.
   ============================================================================ */

#ifndef EMBWEK_SINK_H
#define EMBWEK_SINK_H

#ifdef EMBWEK_IO_H
#error "iris_sink.h and the older iris_io.h are two generations of the same boundary and define the same names. Include one."
#endif

/* The core defines the widest vector this boundary can carry. Including it
   rather than restating the number is the only way the two cannot drift: an
   #ifdef-guarded cross-check is inert in exactly the translation unit that
   forgot to include iris.h, which is the one that needed it. iris.h is
   platform-free and header-only, so this costs a few unused static functions
   the compiler drops. */
#include "iris.h"

#define IRIS_SINK_VERSION   2
#define IRIS_SINK_MAX_DIMS  IRIS_MAX_OUT    /* one authority, no restatement */

#ifndef IRIS_IO_API
/* `static inline`, not plain `static`. A single-header library defines every
   function in every translation unit that includes it, and a caller who uses
   five of them is not doing anything wrong. With plain `static`, -Wall -Wextra
   then emits an unused-function warning for each of the other sixty — measured
   2026-08-27: THIRTY warnings compiling the nine-line examples/00_minimal.c.
   That is a terrible first thirty seconds for someone who just cloned this.
   `inline` tells the compiler the definition is expected to be unused here,
   silencing that without changing linkage, ODR behaviour or codegen. */
#define IRIS_IO_API static inline
#endif

#if defined(__GNUC__) || defined(__clang__)
#define IRIS_MUST_CHECK __attribute__((warn_unused_result))
#else
#define IRIS_MUST_CHECK
#endif

/* --------------------------------------------------------------------------
   THE CODES

   Each one names a different thing the caller can DO about it, which is the
   only reason for a code to exist. "It broke" is not a code.
   -------------------------------------------------------------------------- */
enum {
  IRIS_W_SHARED     =  1,  /* delivered, but expression is not independent      */
  IRIS_OK           =  0,
  IRIS_E_SHAPE      = -1,  /* vector length != endpoint length. Fix the wiring. */
  IRIS_E_RANGE      = -2,  /* outside [0,1], NaN or infinite. Fix the port.     */
  IRIS_E_IO         = -3,  /* the transport refused the message outright.       */
  IRIS_E_BUSY       = -4,  /* queue full. Nothing was sent. Retry.              */
  IRIS_E_ABSENT     = -5,  /* nobody is listening / the part is not fitted.     */
  IRIS_E_CONFIG     = -6,  /* bad arguments at init. Fix the wiring site.       */
  IRIS_E_NOTREADY   = -7,  /* endpoint has not finished configuring. Step it.   */
  IRIS_E_UNMEASURED = -8,  /* budget.worst_us == 0. Measure it, don't guess.    */
  IRIS_E_BLOCKING   = -9   /* declared blocking; forbidden on the gesture path. */
};

IRIS_IO_API const char *iris_io_str(int rc) {
  switch (rc) {
    case IRIS_W_SHARED:     return "sent, sharing a channel";
    case IRIS_OK:           return "ok";
    case IRIS_E_SHAPE:      return "vector length does not fit endpoint";
    case IRIS_E_RANGE:      return "value outside [0,1] or not a number";
    case IRIS_E_IO:         return "transport refused the message";
    case IRIS_E_BUSY:       return "transport queue full, nothing sent";
    case IRIS_E_ABSENT:     return "nobody listening / part not fitted";
    case IRIS_E_CONFIG:     return "endpoint built with bad arguments";
    case IRIS_E_NOTREADY:   return "endpoint still configuring";
    case IRIS_E_UNMEASURED: return "budget never measured";
    case IRIS_E_BLOCKING:   return "blocking call on the gesture path";
    default:              return rc > 0 ? "sent, with a caveat" : "unknown error";
  }
}

/* --------------------------------------------------------------------------
   iris_desc — ONE ROW PER DIMENSION

   name  8 characters or fewer; this is what the training screen draws.
   unit  what the number MEANS on the far side. Display only. Never parsed,
         so it is not a second scaling authority — scaling lives in the port.
   dflt  in [0,1]. What a missing source's slice pins to, and what a sink
         assumes before anything has been demonstrated.

   Static storage, always. Nothing to own, nothing to free, nothing to
   overflow. It exists because the musician sets targets ON THE DEVICE, so
   the training screen needs a string per dimension and there must be exactly
   one place those strings live.
   -------------------------------------------------------------------------- */
typedef struct {
  const char *name;
  const char *unit;
  float       dflt;
} iris_desc;

IRIS_MUST_CHECK IRIS_IO_API int iris_desc_check(const iris_desc *d, int n) {
  if (!d || n < 1 || n > IRIS_MAX_IN) return IRIS_E_CONFIG;
  for (int i = 0; i < n; ++i) {
    if (!d[i].name || !d[i].unit) return IRIS_E_CONFIG;
    if (!(d[i].dflt >= 0.0f && d[i].dflt <= 1.0f)) return IRIS_E_RANGE;
  }
  return IRIS_OK;
}

/* --------------------------------------------------------------------------
   iris_budget — A MEASUREMENT, NOT A PROMISE

   worst_us == 0 means NEVER MEASURED, and an endpoint that was never measured
   refuses to arm. samples travels with the numbers so a budget taken over
   four frames is distinguishable from one taken over four thousand. This is
   "measure, never estimate" and "no blocking calls on the gesture path" as
   startup refusals instead of as comments somebody violates.
   -------------------------------------------------------------------------- */
typedef struct {
  uint32_t worst_us;
  uint32_t p99_us;
  uint32_t samples;
  uint8_t  blocking;      /* 1 = may wait on a bus or a lock */
} iris_budget;

IRIS_MUST_CHECK IRIS_IO_API int iris_budget_arm(const iris_budget *b) {
  if (!b) return IRIS_E_CONFIG;
  if (b->blocking) return IRIS_E_BLOCKING;
  if (b->worst_us == 0 || b->samples == 0) return IRIS_E_UNMEASURED;
  return IRIS_OK;
}

IRIS_MUST_CHECK IRIS_IO_API int iris_budget_measure(iris_budget *b, uint32_t worst_us,
                                              uint32_t p99_us, uint32_t samples,
                                              int blocking) {
  if (!b || worst_us == 0 || samples == 0) return IRIS_E_CONFIG;
  if (p99_us > worst_us) return IRIS_E_CONFIG;
  b->worst_us = worst_us; b->p99_us = p99_us;
  b->samples  = samples;  b->blocking = (uint8_t)(blocking ? 1 : 0);
  return IRIS_OK;
}

/* --------------------------------------------------------------------------
   iris_bytes — ONE MESSAGE, WHOLE OR NOT AT ALL

   write() is MESSAGE oriented, never a byte stream. One call carries one
   complete thing: one MIDI channel-voice message, one line of text including
   its newline. It sends all of it or none of it.

   That is not tidiness. A MIDI message split across two writes and then
   interleaved with another corrupts the receiver's parser until the next
   status byte, and on a controller emitting continuous pitch bend that can be
   a very long time. Half a message must be UNSAYABLE, not merely checked for.
   It is also what lets a USB port packetise — TinyUSB wants 4-byte USB-MIDI
   packets, not bytes — without ever buffering or reassembling.

   Returns IRIS_OK, or IRIS_E_BUSY (queue full, NOTHING sent, retry the whole
   message), IRIS_E_ABSENT (no host), IRIS_E_IO (malformed or rejected).
   -------------------------------------------------------------------------- */
typedef struct iris_bytes {
  const char *name;
  int (*write)(struct iris_bytes *w, const unsigned char *msg, int len);
} iris_bytes;

IRIS_MUST_CHECK IRIS_IO_API int iris_bytes_write(iris_bytes *w, const unsigned char *msg, int len) {
  if (!w || !w->write || !msg || len < 1) return IRIS_E_CONFIG;
  return w->write(w, msg, len);
}

/* --------------------------------------------------------------------------
   iris_sink — the parameter vector's destination

   Concrete sinks put an iris_sink as their FIRST member, so the iris_sink* the
   app holds and the concrete struct share an address (C99 6.7.2.1p15) and the
   downcast is a plain cast. No void *ctx, no container_of, no allocation.
   IF YOU WRITE A SINK, iris_sink MUST BE ITS FIRST MEMBER.

   send() receives a vector already proven to be n floats in [0,1], so no sink
   repeats that check.

   stop() exists and the source has no matching call, because symmetry is not
   a reason. A sink leaves state behind IN ANOTHER MACHINE — a held MPE note
   goes on sounding on the host forever after our power is cut — and releasing
   it is the sink's job because nothing else can do it. A source leaves
   nothing behind anywhere.

   stop() may return IRIS_E_BUSY. When it does, NOTHING has been forgotten and
   the caller must call it again; iris_sink_stop_tries() is the bounded retry so
   that no caller has to invent one.
   -------------------------------------------------------------------------- */
typedef struct iris_sink {
  const char    *name;
  int            n;
  const iris_desc *desc;      /* n rows, static storage, never NULL */
  iris_budget      budget;
  int (*send)(struct iris_sink *s, const float *v);
  int (*stop)(struct iris_sink *s);
} iris_sink;

IRIS_MUST_CHECK IRIS_IO_API int iris_sink_send(iris_sink *s, const float *v, int n) {
  if (!s || !s->send || !v) return IRIS_E_CONFIG;
  if (n != s->n) return IRIS_E_SHAPE;
  for (int i = 0; i < n; ++i)
    if (!(v[i] >= 0.0f && v[i] <= 1.0f)) return IRIS_E_RANGE;  /* NaN, +/-inf too */
  return s->send(s, v);
}

IRIS_MUST_CHECK IRIS_IO_API int iris_sink_stop(iris_sink *s) {
  if (!s || !s->stop) return IRIS_E_CONFIG;
  return s->stop(s);
}

/* `tries` is a count and not a timeout, because there is no clock at this
   layer and inventing one here would make every port carry it. */
IRIS_MUST_CHECK IRIS_IO_API int iris_sink_stop_tries(iris_sink *s, int tries) {
  int rc = IRIS_E_BUSY;
  for (int i = 0; i < tries; ++i) {
    rc = iris_sink_stop(s);
    if (rc != IRIS_E_BUSY) return rc;
  }
  return rc;
}

/* Call once per sink, after init, before the first frame. There is no rig
   object and no registry: the list of endpoints is something the wiring site
   already has, and does not need a type for. */
IRIS_MUST_CHECK IRIS_IO_API int iris_sink_arm(const iris_sink *s) {
  if (!s || !s->send || !s->stop || !s->desc) return IRIS_E_CONFIG;
  return iris_budget_arm(&s->budget);
}

IRIS_MUST_CHECK IRIS_IO_API int iris_sink_measure(iris_sink *s, uint32_t worst_us,
                                            uint32_t p99_us, uint32_t samples,
                                            int blocking) {
  if (!s) return IRIS_E_CONFIG;
  return iris_budget_measure(&s->budget, worst_us, p99_us, samples, blocking);
}

#endif /* EMBWEK_SINK_H */
