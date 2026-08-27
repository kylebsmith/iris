/* ============================================================================
   iris_osc.h  —  the Wekinator-faithful sink: /wek/outputs, N floats, as text
   C99 · no dependencies · no malloc · no libc · NO HARDWARE, NO NETWORK

   Wekinator emits N floats to the OSC address /wek/outputs and lets the
   receiving patch decide what every one of them means. This sink emits the
   same address and the same N floats, so a Max, Pd or SuperCollider patch
   written against Wekinator receives from this device with the address left
   alone. That is the entire point of the port: the students' existing
   patches, and the course's existing teaching material, keep working.

   THIS BOARD HAS NO NETWORK. There is no UDP, no Wi-Fi, and adding either
   would put a stack with its own allocator inside a no-malloc project. So
   the message goes over the serial link as ONE LINE OF ASCII, and the four
   lines of glue that turn lines into real OSC packets live on the laptop,
   where a socket already exists.

   ---------------------------------------------------------------------------
   THE EXACT FORMAT. One line per frame, byte for byte:

       "/wek/outputs" SP F SP F SP ... F LF

     address   the literal bytes of cfg.addr, "/wek/outputs" by default
     SP        exactly one 0x20 between every pair of fields, never two,
               and never one before the LF
     F         EXACTLY EIGHT BYTES: one digit, '.', six digits.
               "0.000000" .. "1.000000". No sign, no exponent, no locale,
               no variable width. Round-half-up on the seventh decimal.
     LF        one 0x0A. No CR. A bare LF is what every line reader on the
               far side already splits on, and a CRLF makes the last float
               of every line arrive with a stray byte glued to it in exactly
               the languages where that is hardest to notice.

     A three-output frame is therefore always 12 + 3*9 + 1 = 40 bytes:

       2F 77 65 6B 2F 6F 75 74 70 75 74 73 20 30 2E 35 30 30 30 30 30 20 ...
       /  w  e  k  /  o  u  t  p  u  t  s  SP 0  .  5  0  0  0  0  0  SP ...

   Fixed width is not decoration. It means the line length is a function of N
   alone, so the buffer bound is a compile-time fact, a receiver can slice by
   offset instead of parsing, and the change-only comparison below is a plain
   memcmp of two equal-length strings.

   ---------------------------------------------------------------------------
   IT IS OSC-SHAPED TEXT, NOT AN OSC PACKET, AND THE NAME SAYS SO.
   The sink's name is "osc-text". There is no ",fff" type-tag string, no
   4-byte padding, no bundle, no timetag. Claiming otherwise would be the
   kind of lie that costs somebody an afternoon with a packet dump. The
   conversion is trivial and belongs on the host:

     Python   for line in serial: a,*v = line.split(); client.send_message(a, [float(x) for x in v])
     Max      [serial] -> [zl group] -> [fromsymbol] -> [route /wek/outputs]
     SC       SerialPort + a \n accumulator -> .split($ ) -> .asFloat
     Pd       mrpeach [unpackOSC] wants real OSC, so bridge with the Python
              three-liner above, or accumulate bytes to a symbol and use
              [fromsymbol]. Say the bridge out loud in the handout; it is
              twelve lines and it is the same twelve lines every year.

   ---------------------------------------------------------------------------
   THREE DECISIONS. Each is asserted by name in tests/osc_test.c.

   D-OSC-1  CHANGE-ONLY, AND A RATE FLOOR, EXACTLY LIKE THE CC SINK. A still
   finger emits nothing (the rendered line is compared with the previous one
   and dropped if identical), and no more than one line per min_us, default
   IRIS_OSC_MIN_US = 10000 = 100 Hz, which is Wekinator's own working rate. At
   41 bytes a frame that is 4.1 kB/s — comfortable on USB CDC and still
   inside a real 115200 UART's 11.5 kB/s. Sixteen outputs at 100 Hz is
   15.7 kB/s, which is NOT, so raise min_us or drop outputs on a real UART.
   Both walls are the same walls the CC sink uses, for the same reasons, and
   a student who learns one has learned the other.

   D-OSC-2  SHARING THE CONSOLE IS ALLOWED, AND THE FIRST BYTE IS THE FILTER.
   Ideally this sink gets its own iris_bytes — a second CDC interface, a UART.
   When it does not, every line this sink writes begins with '/' and no line
   the iris console writes ever does, so a receiver that keeps only lines
   starting with '/' is correct with no further arrangement. This is stated
   as a contract, not as an accident, because somebody will share the port.

   D-OSC-3  STOP() EMITS NOTHING, for the reason iris_cc.h gives at D-CC-6: a
   receiver holds a number, not a note. There is nothing sounding on the far
   side that only we can release. Sending a final all-zero line would slam
   every mapped parameter to its floor, which for a filter cutoff is a bang
   and for a gain is a silence and for a pan is a lie — and since no output
   is privileged, we cannot know which. It voids the change-only cache and
   returns IRIS_OK.

   ============================================================================ */

#ifndef IRIS_OSC_H
#define IRIS_OSC_H

#include "../../iris_sink.h"

#define IRIS_OSC_VERSION   1
#define IRIS_OSC_MAX       IRIS_SINK_MAX_DIMS      /* = IRIS_MAX_OUT = 16 */
#define IRIS_OSC_ADDR      "/wek/outputs"        /* Wekinator's own, unchanged */
#define IRIS_OSC_ADDR_MAX  31
#define IRIS_OSC_FIELD     8                     /* "0.500000" — always eight */
#define IRIS_OSC_LINE      (IRIS_OSC_ADDR_MAX + IRIS_OSC_MAX * (1 + IRIS_OSC_FIELD) + 2)
#define IRIS_OSC_MIN_US    10000u                /* 100 Hz, Wekinator's rate  */

typedef struct {
  iris_bytes *out;
  int64_t (*now_us)(void);   /* monotonic microseconds; the rate floor needs it */
  int      n;                /* outputs, 1 .. IRIS_OSC_MAX                        */
  uint32_t min_us;           /* 0 = every frame                                 */
  const char *addr;          /* NULL = IRIS_OSC_ADDR                              */
} iris_osc_cfg;

typedef struct {
  iris_sink   base;            /* FIRST MEMBER — the downcast depends on it */
  iris_bytes *out;
  int64_t (*now_us)(void);
  char      addr[IRIS_OSC_ADDR_MAX + 1];
  char      line[IRIS_OSC_LINE];
  char      prev[IRIS_OSC_LINE];
  int       addr_n, line_n, prev_n;
  uint32_t  min_us, t_sent;
  uint32_t  frames, lines, bytes, refused, held, same;
  uint8_t   n;
} iris_osc;

#ifdef __cplusplus
extern "C" {
#endif

/* n outputs, "/wek/outputs", 100 Hz floor. */
iris_osc_cfg iris_osc_defaults(iris_bytes *out, int64_t (*now_us)(void), int n);

/* Refuses an address that is not a legal OSC address pattern-free literal:
   it must start with '/', be 1..31 bytes of printable ASCII, and contain
   none of ' ' # * , ? [ ] { } — the characters OSC reserves for pattern
   matching, which in a literal address are undefined behaviour on the far
   side rather than an error anybody reports. */
IRIS_MUST_CHECK int iris_osc_init(iris_osc *o, const iris_osc_cfg *cfg);

/* Void the change-only cache: the next frame emits a line whatever it reads. */
void iris_osc_refresh(iris_osc *o);

/* The formatter, exposed because it is what the tests care about most:
   writes EXACTLY IRIS_OSC_FIELD bytes, no NUL, and returns IRIS_OSC_FIELD.
   v outside [0,1] is clamped — iris_sink_send has already rejected it and
   said so, and this is the second wall, so a bad float cannot become a
   malformed line. */
int iris_osc_fmt(char *p, float v);

#ifdef __cplusplus
}
#endif
#endif /* IRIS_OSC_H */
