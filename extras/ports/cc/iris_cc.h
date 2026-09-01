/* ============================================================================
   iris_cc.h  —  THE DEFAULT SINK: N outputs -> N plain MIDI CCs
   C99 · no dependencies · no malloc · no libc · NO HARDWARE, NO USB

   A port with no device in it, exactly like ports/mpe: it turns a parameter
   vector into complete MIDI channel-voice messages and hands each one to an
   iris_bytes. Every byte it will ever put on the wire is asserted on a laptop
   in tests/cc_test.c.

   ---------------------------------------------------------------------------
   WHY THIS IS THE DEFAULT AND MPE IS NOT

   Wekinator — the thing this device is a successor to — contains ZERO MIDI
   code. It emits N floats to /wek/outputs and the receiving patch assigns
   every scrap of meaning. That is not an omission, it is the design: the
   mapping is the student's, so the semantics must be the student's too.

   MPE hands you three dimensions whose meanings are already decided (pitch,
   pressure, timbre) plus a note gate. Three problems follow. It privileges a
   PITCH AXIS, and Zappi & McPherson (NIME 2014) measured that adding a pitch
   axis REDUCED exploration. (The note working this through lives in the
   unpublished research tree iris was extracted from.) It translates badly: CC74 is unipolar in one synth and bipolar
   in the next, bend range is negotiated per host, and MPE zone setup is
   fifteen messages of things that can be wrong. And it cannot represent a
   mapping with two outputs, or five.

   A plain CC is SEMANTICALLY EMPTY. It means whatever the thing at the far
   end decides it means, it is learnable in every DAW and every plugin on
   earth, and N of them is N of them. That emptiness is the feature.

   NO OUTPUT IS PRIVILEGED HERE. Output 0 is not pitch. There is no gate,
   because a CC stream has no note to gate: the model's outputs go out, all
   of them, all the time, and if a student wants a gate they train an output
   for it and map it to whatever their patch gates on.

   ---------------------------------------------------------------------------
   THE SIX DECISIONS. Each is asserted by name in tests/cc_test.c.

   D-CC-1  THE DEFAULT CC NUMBERS ARE UNDEFINED CONTROLLERS: output k -> CC
   20+k, and CC 102+k-12 past twelve outputs. MIDI 1.0 leaves 20..31 and
   102..119 undefined, so nothing in the host already believes it owns them.
   The obvious-looking default {1, 11, 74} (mod wheel, expression, brightness)
   is exactly wrong: CC1 and CC11 are intercepted by hosts and synths that
   never asked us, and every one of the three carries a meaning that this
   project spent a research note deciding not to impose. 20..31 has the
   further property that its 14-bit LSB partners (52..63) are also undefined.

   D-CC-2  CHANGE-ONLY, WITH HYSTERESIS, WITH A PER-OUTPUT RATE FLOOR. Three
   different floods, three different walls. A still finger must emit nothing
   at all (change-only on the quantised value). A noisy sensor sitting on a
   quantiser boundary must not dither two messages per frame forever
   (IRIS_CC_HYST: a neighbouring step is only accepted once the float is a
   quarter-step past the boundary; a jump of two or more always goes). And a
   fast sweep must not put 127 messages per axis on the wire in 100 ms
   (min_us, default IRIS_CC_MIN_US = 3000, i.e. 333 Hz per output — high enough
   that ordinary playing never touches it, low enough that three outputs
   cannot exceed 1000 msgs/s = 4 kB/s). Set min_us = IRIS_CC_DIN_MIN_US if the
   iris_bytes is a 31250-baud DIN port; set 0 to disable the floor entirely.

   D-CC-3  14-BIT IS OPT-IN, PER OUTPUT, AND MSB-THEN-LSB WITH THE LSB
   ALWAYS RE-SENT. MSB on cc[k], LSB on cc[k]+32, which forces cc[k] <= 31.
   When the MSB changes the LSB is re-sent even if unchanged: a receiver that
   pairs them holds a stale LSB otherwise and the parameter jumps by up to
   1/128 of full scale at every MSB boundary. That bug is the reason most
   14-bit implementations sound worse than 7-bit ones.
     WHEN IT IS WORTH IT: the receiving parameter is continuous, slow, and
     spans a wide range — a filter cutoff over ten octaves, a tuning, a long
     crossfade — AND the host pairs MSB/LSB (Reaper's 14-bit mode, Bitwig,
     most hardware). WHEN IT IS NOT: anything a human hears as a knob, and
     any host that MIDI-learns "whatever moved", because it will learn the
     MSB as a 7-bit controller and then possibly learn the LSB as a SECOND
     parameter. That is why assign mode sweeps the MSB alone (D-CC-5).

   D-CC-4  THE MAP IS BUILD-TIME, WITH A ONE-LINE SERIAL OVERRIDE. The
   authority is a table at the wiring site — one array a student edits in one
   file with no UI anywhere in the loop. iris_cc_parse() accepts three text
   commands ("ch 1", "cc 3 22 hi", "rate 5") so the map can also be changed on
   a board that is already flashed, and iris_cc_print() renders it back. Both
   are here, platform-free and host-tested, rather than in the device layer,
   because a console is a transport and a grammar is not.
     A rejected change leaves the map EXACTLY as it was. There is no partial
     application, so a typo cannot produce a half-configured instrument.

   D-CC-5  ASSIGN MODE IS TOTAL. DAWs MIDI-learn by binding whatever CC is
   moving; stream all N at once and you can only ever map the first. So one
   output is ARMED, a 0..127..0 triangle walks its CC alone at 100 messages
   per second, and every other CC IS SILENT — not "unlikely to move", silent:
   while assign is on, send() consumes the parameter vector and emits nothing.
   The triangle is a triangle and not a ramp because a ramp's wrap is a jump,
   and a host that learns on the largest observed delta learns the jump.
   Leaving assign voids every cache, so the first live frame re-sends all N
   outputs at their true values and the freshly mapped parameter snaps to the
   instrument instead of to wherever the sweep stopped.

   D-CC-6  STOP() EMITS NOTHING. iris_sink's stop() exists because a sink can
   leave state behind IN ANOTHER MACHINE that only it can release — a held
   MPE note sounds forever after our power is cut. A CC does not: it is a
   number a host is holding, not a note. Zeroing N controllers at shutdown
   would move a mapped fader nobody asked us to move, and since no output is
   privileged as "gain" there is no output for which zero is the safe value.
   stop() therefore voids the caches and returns IRIS_OK. If your patch needs a
   parking position, train it as an example and hold it.

   ---------------------------------------------------------------------------
   BUS COST, so nobody has to guess. One 7-bit CC message is 3 bytes on the
   wire and one 4-byte USB-MIDI packet.

     3 outputs, 7-bit, min_us 3000, worst case ...  1000 msgs/s   4.0 kB/s
     3 outputs, 14-bit, same floor ................ 2000 msgs/s   8.0 kB/s
     16 outputs, 7-bit, same floor ................ 5333 msgs/s  21.3 kB/s
     assign mode, any config ......................  100 msgs/s   0.4 kB/s
     a still finger, any config ...................     0 msgs/s        0

   A full-speed USB-MIDI bulk endpoint carries 16 packets per 1 ms frame =
   16000 msgs/s, so even sixteen 7-bit outputs sit at a third of it. A DIN
   port carries 1041 msgs/s; use IRIS_CC_DIN_MIN_US there.

   ---------------------------------------------------------------------------
   WHAT THE UI OWES ASSIGN MODE, AND WHY IT IS NOT CHROME

   The substrate has no text in PLAY, so "which output is arming" cannot be a
   label. It does not need to be. This port exports three numbers and the
   surface draws the whole answer with the vocabulary it already has:

     iris_cc_assign_state()  LIVE / ARMED / DONE
     iris_cc_assign_index()  which output is armed, 0..n-1
     iris_cc_assign_level()  the value going out RIGHT NOW, 0..1

   The field paints the armed output's OWN learned surface — for every
   lattice vertex, the luminance of iris_predict(x,y)[index] — tinted with a
   hue fixed by the index (360*index/n, so it scales to any N and is the same
   colour every time), and the whole screen is multiplied by
   0.25 + 0.75*level. The screen therefore breathes in exact lock-step with
   the CC being emitted: "the glass is pulsing" IS "that CC is moving", which
   is the only fact the user needs while their other hand clicks Learn. On
   DONE the field returns to full colour and blooms once (EWA_A_PULSE).
   Zero text, zero widgets, and the shape on the glass tells the student what
   that output actually does across the pad before they ever map it.

   ============================================================================ */

#ifndef IRIS_CC_H
#define IRIS_CC_H

#include "../../iris_sink.h"

#define IRIS_CC_VERSION      1
#define IRIS_CC_MAX          IRIS_SINK_MAX_DIMS   /* = IRIS_MAX_OUT = 16 */
#define IRIS_CC_MAX_NUM      119                /* 120..127 are Channel Mode */
#define IRIS_CC_LSB_OFF      32                 /* MIDI 1.0: LSB = MSB + 32  */
#define IRIS_CC_MAX_MSB14    31                 /* so that MSB+32 <= 63      */
#define IRIS_CC_UNSENT       (-1)

#define IRIS_CC_HYST         0.25f              /* steps, D-CC-2 */
#define IRIS_CC_MIN_US       3000u              /* per output, USB default    */
#define IRIS_CC_DIN_MIN_US   10000u             /* per output, 31250-baud DIN */

/* assign mode, D-CC-5. 254 steps of 10 ms = one 2.54 s triangle that hits
   every 7-bit value exactly once going up and once coming down. */
#define IRIS_CC_ASSIGN_STEPS   254
#define IRIS_CC_ASSIGN_STEP_US 10000u
#define IRIS_CC_ASSIGN_PERIOD_US ((uint32_t)IRIS_CC_ASSIGN_STEPS * IRIS_CC_ASSIGN_STEP_US)

enum { IRIS_CC_LIVE = 0, IRIS_CC_ARMED = 1, IRIS_CC_DONE = 2 };

/* Every field has a stated default so a student can leave all but `n` alone.
   iris_cc_defaults() fills them; edit the two lines you care about. */
typedef struct {
  iris_bytes *out;
  int64_t (*now_us)(void);      /* monotonic microseconds. Required: the rate
                                   floor and the sweep are both real time.  */
  int n;                        /* outputs, 1 .. IRIS_CC_MAX                  */
  int channel;                  /* WIRE value 0..15. 0 is "MIDI channel 1". */
  uint32_t min_us;              /* per-output floor; 0 disables it          */
  const unsigned char *cc;      /* n numbers, 0..119. NULL = D-CC-1 default */
  const unsigned char *hi;      /* n flags, 1 = 14-bit. NULL = all 7-bit    */
} iris_cc_cfg;

typedef struct {
  iris_sink   base;               /* FIRST MEMBER — the downcast depends on it */
  iris_bytes *out;
  int64_t (*now_us)(void);
  unsigned char cc[IRIS_CC_MAX], hi[IRIS_CC_MAX];
  int16_t   sent[IRIS_CC_MAX];    /* last value SENT, own resolution, or -1    */
  uint8_t   owe_lsb[IRIS_CC_MAX]; /* MSB landed, LSB still owed (D-CC-3)       */
  uint32_t  t_sent[IRIS_CC_MAX];  /* us, wrap-safe unsigned difference         */
  uint32_t  min_us;
  uint32_t  frames, msgs, refused, held, dithers;
  uint32_t  t_step;             /* assign: when the last sweep msg went out  */
  uint8_t   n, channel, mode, arm;
  int16_t   ph;                 /* assign: 0 .. IRIS_CC_ASSIGN_STEPS-1         */
} iris_cc;

#ifdef __cplusplus
extern "C" {
#endif

/* n outputs, channel 1, default CC numbers, all 7-bit, IRIS_CC_MIN_US. */
iris_cc_cfg iris_cc_defaults(iris_bytes *out, int64_t (*now_us)(void), int n);

/* Refuses a map that cannot work rather than one that merely looks odd:
   a CC above 119, a 14-bit MSB above 31, or ANY collision between the
   numbers two outputs occupy (including an MSB landing on another output's
   LSB, which is the aliasing bug you would otherwise chase for an evening). */
IRIS_MUST_CHECK int iris_cc_init(iris_cc *m, const iris_cc_cfg *cfg);

/* Void every cache: the next live frame re-sends all n outputs whatever they
   read. Call after the host reconnects, after a map change, and after assign.
   (iris_cc does it for you in the last two cases.) */
void iris_cc_refresh(iris_cc *m);

/* ---- assign mode, D-CC-5 -------------------------------------------------
   begin  arms output 0 and takes the wire: send() goes silent.
   step   +1 advances (past the last output -> IRIS_CC_DONE; a further +1 ends
          assign and returns IRIS_CC_LIVE), -1 goes back and clamps at 0.
   tick   emits at most one message; call it every UI/hot tick. Returns the
          number of messages sent (0 or 1) or a negative code. A refusal is
          not retried: the next tick simply sends the next step, because a
          sweep is a stream and a dropped sample of it means nothing.
   end    returns to LIVE and voids the caches. Always succeeds.            */
IRIS_MUST_CHECK int iris_cc_assign_begin(iris_cc *m);
int   iris_cc_assign_step (iris_cc *m, int dir);     /* returns the new state */
IRIS_MUST_CHECK int iris_cc_assign_tick(iris_cc *m);
int   iris_cc_assign_end  (iris_cc *m);              /* returns IRIS_CC_LIVE    */
int   iris_cc_assign_state(const iris_cc *m);
int   iris_cc_assign_index(const iris_cc *m);        /* -1 unless ARMED       */
float iris_cc_assign_level(const iris_cc *m);        /* 0..1, what is on the wire */

/* ---- the map, readable and writable without a control panel, D-CC-4 -----
   iris_cc_parse takes ONE line, no newline needed, leading spaces allowed:

       ch <1..16>              the MIDI channel, as a human writes it
       cc <1..n> <0..119>      output k (1-based) -> that CC, 7-bit
       cc <1..n> <0..31> hi    ...as a 14-bit pair, MSB there, LSB there+32
       cc <1..n> <0..119> lo   ...back to 7-bit
       rate <0..1000>          the per-output floor, in MILLISECONDS

   Anything else, or a change that would collide, returns IRIS_E_CONFIG and
   changes NOTHING. A successful change voids the caches.

   iris_cc_print renders the current map into buf as lines ending "\n" and
   returns the length written (never more than cap-1, always NUL-terminated).
   Neither function touches libc.                                           */
IRIS_MUST_CHECK int iris_cc_parse(iris_cc *m, const char *line);
int iris_cc_print(const iris_cc *m, char *buf, int cap);

/* what the map says, for a report or a UI that wants to draw it */
int iris_cc_channel(const iris_cc *m);               /* 0..15, wire value */
int iris_cc_number (const iris_cc *m, int k);        /* MSB CC, or -1     */
int iris_cc_is14   (const iris_cc *m, int k);

/* Exposed because it is the arithmetic the tests care about most: the
   quantiser with hysteresis. `last` is the previous quantised value or -1. */
int iris_cc_quant(float v, int last, int max);

#ifdef __cplusplus
}
#endif
#endif /* IRIS_CC_H */
