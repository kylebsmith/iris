/* ============================================================================
   iris_mpe.h  —  the MPE sink: feature vector -> MIDI bytes
   C99 · no dependencies · no malloc · no libc · NO HARDWARE, NO USB

   This is a port with no device in it. It turns a parameter vector into
   complete MIDI channel-voice messages and hands each one to an iris_bytes.
   Whether that iris_bytes is TinyUSB, a UART, BLE-MIDI or a buffer in a unit
   test is none of its business — which is exactly why the most error-prone
   code in the instrument can be tested byte for byte on a laptop.

   Written against MPE v1.1 (MMA M1-100-UM, 14-Apr-2022) and, where v1.1 and
   the implementations disagree, against what LinnStrument, JUCE, Surge XT and
   Vital actually do. Every deviation from the plain text of the spec is
   argued in the numbered decisions below, and every one of them is asserted
   in tests/mpe_test.c.

   ---------------------------------------------------------------------------
   THE PARAMETER VECTOR IS FIXED BY POSITION.  n = 4 * voices.

     v[4k + 0]  GATE   is voice k sounding?     >= 0.6 on, <= 0.4 off
     v[4k + 1]  PITCH  0 -> note_lo, 1 -> note_lo + note_span. Continuous.
     v[4k + 2]  PRESS  MPE dimension Z -> Channel Pressure  (0xDn)
     v[4k + 3]  SLIDE  MPE dimension Y -> CC 74             (0xBn 0x4A)

   Three dimensions MPE was designed around, plus the one thing MPE cannot
   infer: whether a note exists at all. A gesture controller has no key, so
   the gate is something the MODEL LEARNS.

   ---------------------------------------------------------------------------
   THE SIX DECISIONS. The argument for each, with the rejected alternative, is
   in docs/adr/0002-the-sink-boundary-and-the-mpe-encoder.md. Each is asserted
   by name in tests/mpe_test.c.

   D-MPE-1  VOICES ARE A NUMBER, NOT AN ASSUMPTION. `voices` and `members` are
   both set at init and are allowed to disagree. Default 1 voice over 2 member
   channels: consecutive notes then alternate, so a long release tail is never
   cut by the next note landing on the same channel (A.3, "oldest last Note
   Off"), and configuration stays at 15 messages, one USB TX FIFO's worth.
   When voices > members the pool runs out and §2.2.4.1 says exactly what must
   happen: the note SHARES a channel, is never dropped, and steals nothing.
   A shared note is no longer independently expressive, so the sink returns
   IRIS_W_SHARED and counts it. See iris_mpe_channels.h.

   D-MPE-2  THE NOTE IS LATCHED AFTER A SETTLE WINDOW, NOT AT THE CROSSING.
   GATE and PITCH are two outputs of the same smooth regressor, so at the
   crossing frame PITCH is by construction mid-transition and PRESS is
   systematically low. The crossing ARMS a note; nothing goes on the wire for
   `settle` frames; then note, velocity, bend, slide and pressure are latched
   together from the settled frame. Onset latency is `settle` frames exactly —
   a count times the rig's frame period, a number to be MEASURED on the
   device, never estimated. Chatter suppression is free: a gate that flickers
   across the threshold inside the window emits zero bytes.

   D-MPE-3  CC 74 IS ABSOLUTE (initial-position), 7-BIT. v1.1 A.4.3 defines
   Absolute and Relative and refuses to choose; Relative describes a finger
   sliding from wherever it landed and we have no finger. A host that reads
   CC74 as BIPOLAR (Surge XT's mpeTimbreIsUnipolar = false) hears our 0 as -1:
   the setup guide must say "set timbre to unipolar". No 14-bit extension is
   emitted — Haken MPE+ (CC87 prefix) and Vital (CC102/CC106) are mutually
   incompatible private schemes.

   D-MPE-4  CHANNEL PRESSURE IS ZEROED AT BOTH NOTE BOUNDARIES. v1.1 A.4.2,
   and what LinnStrument's production firmware does. The real pressure is
   re-sent immediately AFTER the Note On, so dynamics are right within one
   message. Two extra messages per note, never per frame.

   D-MPE-5  ONE MCM. NO MANAGER RPN 0. NO INBOUND MCM. The spec's own "and
   turn off the other zone" second MCM is not sent: §2.2.1 recommends against
   it and it provably turns MPE OFF in Surge XT, whose onRPN ignores the
   channel nibble in the RPN-6 branch. Manager RPN 0 would be five messages
   that change nothing, because the MCM already obliges the receiver to set
   manager sensitivity to +/-2 and this sink never bends the manager channel.
   An inbound MCM is never parsed and never echoed (A.1, "adapt silently").

   D-MPE-6  RPN 0 GOES TO EVERY MEMBER CHANNEL, INDIVIDUALLY. One would
   satisfy the spec; it does not satisfy the field. JUCE sends only to the
   first, Surge XT accepts it ONLY on MIDI channel 2, LinnStrument sends to
   all. Sending to all satisfies every one of them. The trailing RPN Null
   (CC101 = 127, CC100 = 127) is not in MPE at all — it is MIDI 1.0 hygiene
   that stops a later stray Data Entry re-triggering the parameter.

   ============================================================================ */

#ifndef IRIS_MPE_H
#define IRIS_MPE_H

#include "../../iris_sink.h"
#include "iris_mpe_channels.h"

#define IRIS_MPE_DIMS        4
#define IRIS_MPE_MAX_VOICES  (IRIS_SINK_MAX_DIMS / IRIS_MPE_DIMS)   /* = 4 */
#define IRIS_MPE_CC_SLIDE    74
#define IRIS_MPE_GATE_ON     0.6f
#define IRIS_MPE_GATE_OFF    0.4f
#define IRIS_MPE_REL_VEL     0x40   /* release velocity 64, never 0 */
#define IRIS_MPE_UNSENT_7    0xFFu  /* an impossible 7-bit value  */
#define IRIS_MPE_UNSENT_14   0x7FFF /* an impossible 14-bit value */

/* Worst case messages one voice can put on the wire in one send(): the
   five-message note-on burst. Asserted by counting, in tests/mpe_test.c. */
#define IRIS_MPE_MAX_MSGS_PER_VOICE 5

enum { IRIS_MPE_LOWER = 0, IRIS_MPE_UPPER = 1 };
enum { IRIS_MPE_GATE = 0, IRIS_MPE_PITCH = 1, IRIS_MPE_PRESS = 2, IRIS_MPE_SLIDE = 3 };
enum { IRIS_MPE_SILENT = 0, IRIS_MPE_ARMED, IRIS_MPE_ATTACK, IRIS_MPE_SOUND, IRIS_MPE_RELEASE };

typedef struct {
  iris_bytes *out;
  int zone;        /* IRIS_MPE_LOWER or IRIS_MPE_UPPER                     */
  int voices;      /* 1 .. IRIS_MPE_MAX_VOICES                           */
  int members;     /* member channels the MCM declares, 1 .. 15        */
  int bend_range;  /* semitones, 0 .. 96. 48 unless you have a reason. */
  int note_lo;     /* MIDI note at PITCH = 0                           */
  int note_span;   /* semitones from PITCH 0 to PITCH 1                */
  int settle;      /* frames armed before a note starts, 1 .. 64       */
} iris_mpe_cfg;

typedef struct {
  uint8_t state, settle, step, shared;
  int8_t  ch;                     /* member index, -1 when none */
  int16_t note;                   /* sounding note number, -1 when silent */
  int16_t bend;                   /* last value SENT, or IRIS_MPE_UNSENT_14 */
  int16_t l_bend;                 /* latched for the burst in progress    */
  uint8_t press, slide;           /* last values SENT, or IRIS_MPE_UNSENT_7 */
  uint8_t l_note, l_vel, l_press, l_slide;
} iris_mpe_voice;

typedef struct {
  iris_sink      base;              /* FIRST MEMBER — the downcast depends on it */
  iris_bytes    *out;
  iris_mpe_pool  pool;
  iris_mpe_voice v[IRIS_MPE_MAX_VOICES];
  uint32_t     frames;            /* send() calls that got past the checks  */
  uint32_t     notes;             /* completed note-on bursts               */
  uint32_t     msgs;              /* messages handed to the transport, ok   */
  uint32_t     refused;           /* messages the transport would not take  */
  uint32_t     stalls;            /* frames that ended with a burst unfinished */
  int16_t      cfg_step;          /* 0 .. steps; == steps means configured  */
  uint8_t      voices, members, zone, settle;
  uint8_t      bend_range, note_lo, note_span;
} iris_mpe;

#ifdef __cplusplus
extern "C" {
#endif

/* voices 1, members 2, lower zone, +/-48 semitones, note 36..84, settle 2. */
iris_mpe_cfg iris_mpe_defaults(iris_bytes *out);

IRIS_MUST_CHECK int  iris_mpe_init(iris_mpe *m, const iris_mpe_cfg *cfg);

/* Configuration is a RESUMABLE SCRIPT, not a loop inside init(). Two measured
   reasons. At boot the USB endpoint is not open yet, so tud_midi_packet_write
   returns false and the message is DISCARDED rather than queued — a burst
   fired from init() is refused in its entirety, on every boot. And the TX
   FIFO is 64 bytes = 16 packets, so even an open link only takes so much at
   once. Step this from the rig until it returns 0.

   Until it completes, send() returns IRIS_E_NOTREADY. A note played against an
   unconfigured host lands at the host's default +/-2 bend range and is 24x
   out of tune; refusing is the only honest thing to do.

   Returns the number of messages sent (0 when already complete), or < 0. */
IRIS_MUST_CHECK int  iris_mpe_configure_step(iris_mpe *m, int budget);
int                iris_mpe_configured(const iris_mpe *m);
int                iris_mpe_config_steps(const iris_mpe *m);

/* The host went away and came back. Nothing is released — the host has no
   notes, it was unplugged, and a note-off into a closed endpoint is only
   refused. Configuration restarts from step 0. */
void               iris_mpe_rearm(iris_mpe *m);

/* Exposed because they are the arithmetic the tests care about most.
   iris_mpe_bend14 is MPE v1.1 Appendix C.3's SENDER equation. */
int                iris_mpe_bend14(const iris_mpe *m, float pitch, int note);
unsigned char      iris_mpe_q7(float v);
int                iris_mpe_member_ch(const iris_mpe *m, int member); /* 0-based MIDI ch */
int                iris_mpe_manager_ch(const iris_mpe *m);            /* 0-based MIDI ch */

#ifdef __cplusplus
}
#endif
#endif /* IRIS_MPE_H */
