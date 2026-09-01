/* ============================================================================
   iris_null.h  —  the sink that goes nowhere, and the transport that goes
                 nowhere with it
   C99 · no dependencies · no malloc · header-only · platform-free

   Not stubs. Each of these has one job that no real endpoint can do.

   iris_null_sink is the sink you wire up to time the mapping with the transport
   REMOVED from the measurement. Subtract it from a run through a real sink
   and what is left is the transport. It also gives the tests something to
   push a deliberately bad vector at, so that "the wrapper rejects it" can be
   demonstrated without a MIDI stream in the way. It counts, because "it ran"
   should be a number and not an assumption.

   iris_null_bytes is the same idea one layer down: an encoder driving nothing,
   so the cost of the encoder can be measured on its own. It can also be told
   to refuse, which is how the back-pressure paths get exercised at all — a
   healthy link never takes them.

   It is also the file to copy when writing a new sink: two functions, one
   init, no registration table anywhere.
   ============================================================================ */

#ifndef IRIS_NULL_H
#define IRIS_NULL_H

#include "../../iris_sink.h"

typedef struct { iris_sink base; uint32_t frames, stops; } iris_null_sink;

static int iris_null__send(iris_sink *s, const float *v) {
  (void)v; ((iris_null_sink *)s)->frames++; return IRIS_OK;
}
static int iris_null__stop(iris_sink *s) { ((iris_null_sink *)s)->stops++; return IRIS_OK; }

IRIS_MUST_CHECK static inline int iris_null_sink_init(iris_null_sink *z, int n, const iris_desc *desc) {
  int rc;
  if (!z || n < 1 || n > IRIS_SINK_MAX_DIMS) return IRIS_E_CONFIG;
  if ((rc = iris_desc_check(desc, n)) < 0) return rc;
  z->base.name = "null"; z->base.n = n; z->base.desc = desc;
  z->base.send = iris_null__send; z->base.stop = iris_null__stop;
  z->base.budget.worst_us = 0; z->base.budget.p99_us = 0;
  z->base.budget.samples = 0;  z->base.budget.blocking = 0;
  z->frames = 0; z->stops = 0;
  return IRIS_OK;
}

/* `refuse` is a code, not a flag, so a test can choose between "the queue is
   full, try again" and "there is no host" — the two states a port must never
   confuse, because one is a retry and the other is a message on the screen. */
typedef struct {
  iris_bytes base;
  uint32_t msgs, bytes, refused;
  int      refuse;        /* IRIS_OK to accept, or the code to refuse with */
} iris_null_bytes;

static int iris_null_bytes__write(iris_bytes *w, const unsigned char *msg, int len) {
  iris_null_bytes *b = (iris_null_bytes *)w;
  (void)msg;
  if (b->refuse != IRIS_OK) { b->refused++; return b->refuse; }
  b->msgs++; b->bytes += (uint32_t)len;
  return IRIS_OK;
}

IRIS_MUST_CHECK static inline int iris_null_bytes_init(iris_null_bytes *b) {
  if (!b) return IRIS_E_CONFIG;
  b->base.name = "null-bytes"; b->base.write = iris_null_bytes__write;
  b->msgs = b->bytes = b->refused = 0; b->refuse = IRIS_OK;
  return IRIS_OK;
}

#endif /* IRIS_NULL_H */
