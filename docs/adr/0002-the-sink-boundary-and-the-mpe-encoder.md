# 0002 — The sink boundary is three function pointers, and the MPE encoder holds no hardware

**Status:** accepted, 2026-08-21
**Affects:** new `src/iris_sink.h`, `src/iris_source.h`, `ports/mpe/`, `ports/null/`,
`tests/mpe_test.c`, one new `build.sh` target. `iris.h` is unchanged
(sha1 `33f371ec361e0d6278e21f60d032e7972460c391`).
**Follows:** D6, D7, D8. The device no longer makes sound; it emits MIDI MPE.

## Context

D6/D7/D8 turned the instrument into an MPE controller, and that moves the
hardest code in the project from "make a sound" to "say the right bytes". The
two are not comparable. A synth that is slightly wrong sounds slightly wrong.
An MPE controller that is slightly wrong is *silently* wrong: it plays in tune
against a JUCE-based host in the studio and four octaves sharp against a
spec-literal one on stage, because JUCE's receiver zeroes its own per-channel
state at Note Off — contradicting MPE v1.1 §2.2.6/7/8's "shall continue to
track" — and therefore hides the commonest sender bug there is.

So the question this ADR answers is not "how do we send MIDI". It is: **where
does the MPE encoder have to live so that every byte it will ever emit can be
asserted on a laptop, before anyone plugs anything in?**

Three things forced the shape of the answer.

1. **The board is not available and never will be, in the sense that matters.**
   D7 says the sensors will change. The board is currently wedged and
   human-owned. A design whose correctness can only be checked by plugging in
   an ESP32 is a design that gets checked once.
2. **`tud_midi_packet_write` returns false and DISCARDS the message.** Measured
   from the installed core: the TX FIFO is 64 bytes = 16 packets
   (`CONFIG_TINYUSB_MIDI_TX_BUFSIZE=64`), and `USBMIDI`'s `noteOn` /
   `controlChange` / `pitchBend` helpers all return `void` and throw that bool
   away. Back-pressure is real, it is frequent at boot, and the convenient API
   drops it on the floor. Any design that cannot express "the transport
   refused, nothing was sent, ask again" is already broken.
3. **A full MPE configuration is 15 messages** for a two-member zone, and every
   one of them is refused if the USB endpoint is not open yet — which at boot
   it never is.

## Decision

**Three types, one function pointer each, and the MPE encoder is a port with
no device in it.**

```
  iris_bytes   carries ONE COMPLETE MESSAGE     src/iris_sink.h
  iris_sink    consumes the parameter vector    src/iris_sink.h
  iris_source  produces the feature vector      src/iris_source.h
```

and one contract that removes an entire config surface: **every float crossing
the boundary is in [0,1]**, checked by the wrapper on every frame, in one place.
`!(v >= 0 && v <= 1)` is false for NaN and for both infinities, so the same
three comparisons reject all of them without `math.h`. Scaling lives in the
port; `iris_desc.unit` is a display string and is explicitly *not* a second
scaling authority. This does not weaken `iris_fit_ranges()`, which is
affine-invariant over the examples actually recorded.

The MPE encoder (`ports/mpe/`) is **pure C99 with no USB, no board and no
serial port**. It emits complete MIDI messages into a caller-supplied
`iris_bytes`. On the device that is TinyUSB; in `tests/mpe_test.c` it is a
buffer that records message boundaries and can be told to refuse. That
substitution is the whole point: a healthy link never takes a back-pressure
path, so the only way to know those paths work is to build an unhealthy one.

Six MPE decisions, each named in the code and asserted by name in the tests:

| | decision | why it is not the obvious thing |
|---|---|---|
| **D-MPE-1** | `voices` and `members` are both set at init and may disagree. Default 1 voice / 2 member channels. | Two channels, not one, so consecutive notes alternate and a long release tail is never cut (A.3, "oldest last Note Off"). When `voices > members` the pool runs out and §2.2.4.1 is a *shall*: the note **shares**, is never dropped, steals nothing — and the sink returns `IRIS_W_SHARED` and counts it, because a shared note has stopped being independently expressive. |
| **D-MPE-2** | The gate crossing **arms**; note, velocity, bend, slide and pressure are latched together after `settle` frames. | GATE and PITCH are two outputs of the *same* smooth regressor, so at the crossing frame PITCH is by construction mid-transition and PRESS is systematically low. Chatter suppression falls out free. |
| **D-MPE-3** | CC 74 is **Absolute**, 7-bit. | v1.1 A.4.3 defines Absolute and Relative and refuses to choose. Relative describes a finger sliding from where it landed; we have no finger. The setup guide must tell the user to set the host's timbre to unipolar. |
| **D-MPE-4** | Channel Pressure is **zeroed immediately before Note On and Note Off**, real value re-sent immediately after Note On. | v1.1 A.4.2, and what LinnStrument's shipping firmware does. Two extra messages per note, never per frame. |
| **D-MPE-5** | **One** MCM. No manager-channel RPN 0. Inbound MCM never parsed, never echoed. | The spec's own "and turn off the other zone" second MCM provably turns MPE **off** in Surge XT, whose `onRPN` ignores the channel nibble in the RPN-6 branch. §2.2.1 recommends against it anyway. |
| **D-MPE-6** | RPN 0 goes to **every** member channel individually, followed by an RPN Null. | One satisfies the spec and not the field: JUCE sends to the first only, Surge XT accepts it **only on MIDI channel 2**, LinnStrument sends to all. All satisfies all three. The RPN Null is not in MPE at all; it is MIDI 1.0 hygiene. |

And two enforcement decisions that are project rules made executable:

- **Never fail silently** is a compiler setting. Every wrapper and every init is
  `warn_unused_result`. The codes are nine, and each names a different thing the
  caller can *do*; "it broke" is not a code.
- **Measure, never estimate** is a startup refusal. `iris_sink_arm()` refuses an
  endpoint whose `budget.worst_us` is still 0, or one that admits it may block.

### The numbers, all measured on this machine

`./build.sh mpe` — **58 checks, all PASS**, plus the 32-bit size assertions.
`./build.sh audit` — **11 checks, all PASS**, unchanged.

| what the encoder puts on the wire | messages | bytes |
|---|---|---|
| zone configuration, 2 member channels | 15 | 45 |
| note on | 5 | 13 |
| sustained frame, all three dimensions moving | 3 | 8 |
| sustained frame, gesture still | **0** | **0** |
| note off | 4 | 11 |

Worst case in one `send()` is **5 messages**, counted over 400 frames of swept
gesture (555 messages total), against the **16-packet** USB TX FIFO measured
from the installed core. Eleven packets of headroom in the worst frame.

Struct sizes on a real 32-bit target (`clang --target=wasm32`, asserted in
`ports/mpe/iris_mpe_wire.c` so they cannot rot; wasm32 is a proxy for Xtensa —
same pointer width, same alignment — and is stated as a proxy, not as a
measurement of the S3):

| | bytes |
|---|---|
| `iris_bytes` | 8 |
| `iris_desc` | 12 |
| `iris_budget` | 16 |
| `iris_sink` | 36 |
| `iris_mpe_voice` | 18 |
| `iris_mpe_pool` | 132 |
| **`iris_mpe`, the whole sink** | **276** |

276 bytes is the entire output half of the instrument, four voices and a
fifteen-channel pool included, statically sized, caller-supplied, no malloc.

Line budgets, against the 200-line-per-port-file rule:

| file | lines |
|---|---|
| `src/iris_sink.h` | 282 |
| `src/iris_source.h` | 92 |
| `ports/mpe/iris_mpe.h` | 183 |
| `ports/mpe/iris_mpe.c` | 198 |
| `ports/mpe/iris_mpe_wire.c` | 124 |
| `ports/mpe/iris_mpe_channels.h` | 113 |
| `ports/mpe/iris_mpe_wire.h` | 26 |
| `ports/null/iris_null.h` | 72 |
| `tests/mpe_test.c` | 584 |

## Rejected alternatives

**1. A MIDI-shaped sink interface: `noteOn(ch,n,v)`, `bend(ch,v)`, `cc(ch,n,v)`.**
Rejected, and this is the one that will be proposed again, because it looks
tidier and it is what `ports/common/iris_midi.h` already does.

It fails for a reason that only shows up in the tests. Such an interface can
express "send a note on" but cannot express **the ordering constraint that is
the whole of MPE correctness** — pitch bend, CC 74 and channel pressure must
reach the receiver *before* the Note On, and the final values must reach it
*before* the Note Off (v1.1 A.4.1 is a "shall"). With five typed calls the
ordering lives at the call site, which means it lives in five places, which
means it is enforced nowhere. With `write(bytes, len)` the burst is one loop
with one cursor, and the test can assert the byte stream, in order, exactly.
It is also the only shape that lets a transport packetise: TinyUSB wants 4-byte
USB-MIDI packets, not bytes, and a byte-stream interface would force it to
reassemble.

**2. A byte-stream `write()` instead of a message-oriented one.** Rejected. A
MIDI message split across two writes and interleaved with another corrupts the
receiver's parser until the next status byte, and on a controller emitting
continuous bend that can be a very long time. Half a message must be
*unsayable*, not merely checked for.

**3. Clamping out-of-range floats instead of rejecting the frame.** Rejected.
Clamping produces a musically plausible result and hides the broken port that
produced it. The frame is refused whole, 0 bytes go out, the sounding note
keeps its last good value, and `IRIS_E_RANGE` comes back. (The encoder's own
`q7` and `bend14` *are* total, so a caller who bypasses the wrapper still
cannot make a byte with bit 7 set — but the wrapper still reports. Defence in
depth, not silence.)

**4. Monophonic, one voice, no channel allocator.** Tempting, and it is what a
single regressor can honestly drive today. Rejected because it makes the
channel-exhaustion path *unreachable*, and an unreachable path is an untested
path that ships. `voices` and `members` are separate numbers precisely so that
`voices=3, members=2` is a case the test suite can construct. The default is
still 1 voice; nothing about the default changed, only whether the degradation
can be exercised.

**5. Dropping or stealing a note on channel exhaustion.** Rejected: v1.1
§2.2.4.1 is a "shall" and v1.0 §1.2 says why — sharing "can be preferable to
limiting polyphony by preventing a new note from sounding, or stopping an older
note". Both alternatives are non-conformant.

**6. Sharing silently.** Rejected, and this is the more interesting half. It
would satisfy the spec. It fails the project's own rule: two notes on one
channel are *one expressive voice*, because bend, pressure and CC74 are channel
messages. The player is owed that fact. Hence `IRIS_W_SHARED` as a positive
return and `pool.shared` as a counter.

**7. Round-robin channel assignment.** Rejected by the spec in as many words —
A.3 opens with "Simple circular assignment of new notes to Member Channels of a
Zone will not usually provide satisfactory results" — because it stacks and
choruses repeated pitches. The pool implements A.3's actual order: a free
channel that last played *this* note number, else lowest active count, ties
broken by oldest last Note Off.

**8. Firing the 15 configuration messages from `init()`.** Rejected on measured
grounds. At boot the USB endpoint is not open, `tud_midi_packet_write` returns
false without queueing, and the whole burst is discarded — every time, on every
boot. Configuration is a resumable, budgeted script, and `send()` returns
`IRIS_E_NOTREADY` until it finishes, because a note against an unconfigured host
lands at the receiver's default ±2 and is 24× out of tune.

**9. Retrying a refused burst from the beginning.** Rejected. It re-injects
messages the host already has into a FIFO that is full *because* it is
congested. Both bursts carry a cursor and resume at the refused message.

**10. Clearing voice state when `stop()` fails.** Rejected. A held note on a
host we have walked away from sounds until the host is restarted; the state
`stop()` needs to release it is the only thing that makes recovery possible, so
`IRIS_E_BUSY` forgets nothing and `iris_sink_stop_tries()` is the bounded retry.

**11. Assuming member channels ascend.** Rejected: they ascend from MIDI 2 in
the Lower Zone and **descend from MIDI 15** in the Upper Zone. One function
knows this and the test asserts the Upper Zone byte stream (`BF … BE … BD …`).

**12. A `rig` object owning the endpoints.** Rejected. The wiring site already
*is* the list of endpoints. A registry with budgets, strike counters and a mute
policy is a second scheduler the instrument does not need; `iris_sink_arm()` on
each endpoint is the whole of the enforcement.

**13. A source multiplexer type.** Rejected. Calling `iris_source_read` twice
into two slices of one array is already everything a mux does, keeps each
sensor's error code separate instead of merging them into one useless status,
and gives hold-last-value for free.

**14. Implementing pitch bend from v1.1 Table 4.** Rejected as *arithmetically
wrong*. Table 4 prints `E2 2B 41` and calls it a quarter tone above middle C at
range 48. Those bytes decode to 8363, which by the spec's own Appendix C.5
receiver equation is **1.0019 semitones**. A real quarter tone is 8277 =
`55 40`. The error is in v1.0 and v1.1 alike and has never been corrected.
`iris_mpe_bend14` implements Appendix C.3's sender equation, and the test asserts
both the correct value and that we do *not* emit Table 4's.

## Consequences

- `./build.sh mpe` is a new merge gate alongside `./build.sh audit`. Both must
  pass. Any change to the cache invalidation in `iris_mpe.c`'s `release()` must
  keep the check that proves it — that one line is the difference between an
  instrument that is in tune and one that is in tune only against JUCE.
- The instrument's output half is now testable end to end with no hardware,
  and **nothing in `ports/mpe/` has ever been run on the board.** It compiles,
  its bytes are asserted, and that is all that may be claimed for it.
- Three numbers still have to be measured on the device and cannot be inferred
  from anything here: onset latency (`settle` × the real frame period),
  action-to-MIDI-out (GPIO toggle at sensor read, second toggle after the write
  returns, scope the pair — and D6 says action-to-MIDI-out, *never*
  action-to-sound), and the sustained rate before refusals appear.
- There are now **two generations of this boundary in the tree**:
  `src/iris_io.h` + `src/iris_sinks.h` + `ports/common/*` from the earlier
  round, and these files. They define the same names with different values and
  cannot be included together; `src/iris_sink.h` `#error`s if it sees the old
  guard, so the collision is a sentence rather than a pile of redefinition
  errors. One generation has to be deleted, and that deletion is not part of
  this change.
- `docs/adr/README.md`'s index does not yet list this ADR.
- The setup guide is still owed to the user, and it is a deliverable, not an
  afterthought: no mainstream DAW turns MPE on because it received an MCM.
  Live, Bitwig, Logic and Vital all need an out-of-band toggle; the plugin's
  bend range must be set to 48; timbre must be unipolar; Bitwig needs "Steal
  Same Key" off.
- The README's "8,192 bytes for a complete instrument" is the three-output
  case. The MPE sink has four outputs per voice, so that number needs
  recomputing. Flagged, not silently edited.

## How to re-derive every number here

```sh
cd firmware/library
./build.sh audit    # the eleven model checks, unchanged
./build.sh mpe      # the 58 MPE checks, then the 32-bit struct sizes
```

Measured on Apple M4 Max (arm64), Apple clang 17.0.0, `-O2`, 2026-08-21.
The 32-bit sizes come from `clang --target=wasm32 -fsyntax-only` over
`ports/mpe/iris_mpe_wire.c` on the same machine.
