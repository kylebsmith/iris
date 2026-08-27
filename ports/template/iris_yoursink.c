/* ============================================================================
   iris_yoursink.c  —  ADD YOUR OWN OUTPUT. COPY THIS FILE. THAT IS THE WHOLE
                     PROCEDURE.
   C99 · no dependencies · no malloc · no libc · no hardware

   This file compiles and works as it stands: it is a real sink that turns the
   parameter vector into a DMX-style 8-bit frame. Copy it, rename it, change
   the three marked places, and you have your own. Nothing else in the system
   changes — not the core, not the UI, not the save format, not a registry,
   because THERE IS NO REGISTRY. The list of endpoints is one line at the
   wiring site and nothing needs a type for it.

   ---------------------------------------------------------------------------
   THE FIVE RULES. Break one and the compiler or a startup refusal tells you,
   which is the point: nothing here relies on you remembering it.

   1. iris_sink IS THE FIRST MEMBER of your struct. The app holds an iris_sink*;
      the downcast back to your type is a plain cast and is only valid
      because C99 6.7.2.1p15 says the first member shares the address.

   2. YOU NEVER CHECK THE VECTOR. iris_sink_send() has already proven it is
      exactly n floats, every one of them in [0,1], no NaN, no infinity.
      Re-checking it is dead code; assuming anything else is a bug.

   3. ONE WHOLE MESSAGE PER iris_bytes_write(). Not a byte stream. Half a
      message must be unsayable, not merely unlikely: a receiver that sees
      half of one and then the start of another stays confused until its
      parser resynchronises, which on a continuous controller can be a very
      long time.

   4. NEVER UPDATE A CACHE BEFORE THE WRITE THAT CARRIED IT SUCCEEDED. A
      transport that returns IRIS_E_BUSY sent NOTHING; if you have already
      recorded the value as sent, the host is now permanently wrong and only
      a reboot fixes it. Cost of getting this right: one late frame.

   5. SAY WHAT WENT WRONG, WITH A CODE THAT NAMES WHAT THE CALLER CAN DO.
      IRIS_E_BUSY means retry. IRIS_E_ABSENT means nobody is listening. IRIS_E_IO
      means the message itself was rejected. "It broke" is not a code, and
      returning IRIS_OK when it did not work is the one unforgivable thing.

   ---------------------------------------------------------------------------
   WIRING IT UP. One declaration and one init, wherever your rig is built:

       static iris_yoursink my;
       ...
       if (iris_yoursink_init(&my, &bytes_port, 3) != IRIS_OK) fail("sink");
       if (iris_sink_measure(&my.base, worst_us, p99_us, samples, 0) != IRIS_OK) ...
       if (iris_sink_arm(&my.base) != IRIS_OK) fail("sink not measured");
       h->sink = &my.base;                 <-- the only line that names it

   iris_sink_arm() REFUSES A SINK THAT WAS NEVER MEASURED. That is deliberate:
   this project measures and does not estimate, and an unmeasured endpoint on
   the gesture path is a latency claim nobody made. Run it, time it, pass the
   numbers to iris_sink_measure(), and the refusal goes away.

   THREE THINGS TO CHANGE. They are marked (1) (2) (3) below.
   ============================================================================ */

#include "../../iris_sink.h"

/* (1) YOUR STATIC DESCRIPTOR TABLE. One row per output, static storage
   always, names of eight characters or fewer because that is what a training
   screen can draw on a 320-pixel panel. dflt is what an output means before
   anything has been demonstrated, and must itself be in [0,1]. */
#define IRIS_YOURSINK_MAX 8
static const iris_desc iris_yoursink_desc[IRIS_YOURSINK_MAX] = {
  {"dmx 1","0..255",0.0f},{"dmx 2","0..255",0.0f},
  {"dmx 3","0..255",0.0f},{"dmx 4","0..255",0.0f},
  {"dmx 5","0..255",0.0f},{"dmx 6","0..255",0.0f},
  {"dmx 7","0..255",0.0f},{"dmx 8","0..255",0.0f}
};

typedef struct {
  iris_sink   base;                        /* RULE 1: FIRST. Always. */
  iris_bytes *out;
  unsigned char frame[1 + IRIS_YOURSINK_MAX];
  int16_t   sent[IRIS_YOURSINK_MAX];       /* last value SENT, or -1 */
  uint32_t  frames, msgs, refused;
  uint8_t   n;
} iris_yoursink;

/* (2) YOUR FRAME. Turn n floats in [0,1] into your own domain by a fixed,
   documented rule, and put the result on the wire as whole messages.
   This one: a start byte then one 8-bit level per output, sent only when
   something in it changed, which is the difference between an instrument and
   a device that saturates its own bus while standing still. */
static int yoursink_send(iris_sink *s, const float *v) {
  iris_yoursink *y = (iris_yoursink *)s;     /* RULE 1 is what makes this legal */
  int k, changed = 0, rc;

  y->frames++;
  y->frame[0] = 0x00;                    /* your start code */
  for (k = 0; k < (int)y->n; ++k) {
    /* RULE 2: v[k] is already known good. Total, saturating, no libc. */
    int q = (int)(v[k] * 255.0f + 0.5f);
    if (q < 0) q = 0;
    if (q > 255) q = 255;
    if (q != y->sent[k]) changed = 1;
    y->frame[1 + k] = (unsigned char)q;
  }
  if (!changed) return IRIS_OK;            /* a still finger emits nothing */

  /* RULE 3: the whole frame is one message. */
  rc = iris_bytes_write(y->out, y->frame, 1 + (int)y->n);
  if (rc < 0) { y->refused++; return rc; }   /* RULE 5 */

  /* RULE 4: only now. */
  for (k = 0; k < (int)y->n; ++k) y->sent[k] = y->frame[1 + k];
  y->msgs++;
  return IRIS_OK;
}

/* (3) YOUR STOP. Release anything you are holding IN THE OTHER MACHINE, and
   nothing else. A held note must be stopped here or it sounds forever after
   our power is cut. A value the far side is merely remembering — a CC, a
   fader position, a DMX level — is NOT that, and moving it at shutdown is a
   change nobody asked for. Blacking out a lighting rig is, so this one does.
   May return IRIS_E_BUSY, in which case NOTHING has been forgotten and the
   caller will call again (iris_sink_stop_tries). */
static int yoursink_stop(iris_sink *s) {
  iris_yoursink *y = (iris_yoursink *)s;
  int k, rc;
  for (k = 0; k < (int)y->n; ++k) y->frame[1 + k] = 0;
  y->frame[0] = 0x00;
  rc = iris_bytes_write(y->out, y->frame, 1 + (int)y->n);
  if (rc < 0) { y->refused++; return rc; }   /* state intact: call again */
  for (k = 0; k < IRIS_YOURSINK_MAX; ++k) y->sent[k] = -1;
  return IRIS_OK;
}

/* Init: validate, fill the vtable, void every cache. Note what is NOT here —
   no allocation, no registration, no global, no side effect on anything
   outside the struct you were handed. */
IRIS_MUST_CHECK int iris_yoursink_init(iris_yoursink *y, iris_bytes *out, int n) {
  int k, rc;
  if (!y || !out || !out->write) return IRIS_E_CONFIG;
  if (n < 1 || n > IRIS_YOURSINK_MAX) return IRIS_E_CONFIG;
  if ((rc = iris_desc_check(iris_yoursink_desc, n)) < 0) return rc;

  y->base.name = "yoursink";
  y->base.n    = n;
  y->base.desc = iris_yoursink_desc;
  y->base.send = yoursink_send;
  y->base.stop = yoursink_stop;
  y->base.budget.worst_us = 0;     /* 0 = NEVER MEASURED. iris_sink_arm() will
                                      refuse until you call iris_sink_measure()
                                      with numbers you actually observed. */
  y->base.budget.p99_us = 0;
  y->base.budget.samples = 0;
  y->base.budget.blocking = 0;     /* 1 if you may wait on a bus or a lock —
                                      and then arm() refuses you outright,
                                      because the gesture path cannot block. */
  y->out = out;
  y->n = (unsigned char)n;
  y->frames = y->msgs = y->refused = 0;
  for (k = 0; k < IRIS_YOURSINK_MAX; ++k) y->sent[k] = -1;
  return IRIS_OK;
}

/* ---------------------------------------------------------------------------
   NOW TEST IT ON YOUR LAPTOP, WITH NO BOARD.

   Your sink takes an iris_bytes, so give it one that remembers what it was
   handed and can be told to refuse. Copy the twenty lines at the top of
   tests/cc_test.c, then:

       cap_open(&C);
       iris_yoursink_init(&Y, &C.base, 3);
       float v[3] = {0.0f, 0.5f, 1.0f};
       iris_sink_send(&Y.base, v, 3);
       assert(C.n == 1 && C.b[1] == 0 && C.b[2] == 128 && C.b[3] == 255);
       C.allow = 0;                       // now the transport refuses
       assert(iris_sink_send(&Y.base, v, 3) == IRIS_E_BUSY);

   Add it to build.sh beside `sinks`. Every byte your instrument will ever
   emit is now asserted on a machine with a keyboard and a debugger, which is
   the reason ports look like this in the first place.
   --------------------------------------------------------------------------- */
