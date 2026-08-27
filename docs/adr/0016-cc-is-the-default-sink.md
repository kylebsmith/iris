# ADR 0016 — MIDI CC is the default sink; nothing is privileged

Status: accepted (v3)
Depends on ADR 0002 (the sink boundary).

## Context

v2 wired the app directly to the MPE encoder: `hot.c` wrote
`{gate, pitch, press, slide}` by name, `app.h` held an `iris_mpe`, `cand.c` set
`v[IRIS_MPE_GATE]`, and `app.c` read `mpe.refused`. Three outputs had fixed
meanings and the fourth position was a gate.

Wekinator — the thing this project is an embedded successor to — contains
**zero MIDI code**. It emits N floats over OSC to `/wek/outputs` and the
receiving patch assigns all the meaning.

## Decision

**Plain MIDI CC is the default sink.** It is the nearest thing MIDI has to N
anonymous floats: semantically empty, universally learnable, and N of them is
N of them. Output *k* → CC 20+*k*, continuing at 102 past twelve — MIDI 1.0
leaves 20–31 and 102–119 *undefined*, so nothing in the host already believes
it owns them.

**No output is privileged.** There is no pitch, no gate, no "first one is
special". Zappi & McPherson (NIME 2014) MEASURED that adding a pitch axis
REDUCED exploration; the full reasoning is in
`research/notes/pitch-articulation-and-sensing.md`. If you want a gate,
train an output for it — that is one more row in `EWX_OUTPUT_LIST`, not a
special one.

**MPE and OSC remain, one `#define` away**, in `app/core/outputs.h`. MPE is
spec-verified and byte-tested and a four-output model driving an MPE synth is
a genuinely good instrument. It is simply not what a student who has not
decided anything yet should be handed.

## Assign mode is the load-bearing part

**DAWs MIDI-learn by binding whatever CC is moving.** Stream all N at once and
you can only ever map the first. Without an assign mode the CC sink is not
merely awkward, it is unusable.

`iris_cc_assign_begin()` arms output 0 and TAKES THE WIRE: `send()` consumes the
parameter vector and emits nothing at all — silent, and asserted (50 model
frames while armed → 0 bytes). `iris_cc_assign_tick()` then emits ONE message
every 10 ms on the armed CC alone: a **triangle, not a ramp**, because a
ramp's wrap is a jump and a host that learns on the largest observed delta
learns the jump. 254 steps × 10 ms = a 2.54 s cycle hitting every 7-bit value
once up and once down, and the largest step in a full cycle is 1.

On the glass, with no chrome: the field paints **the armed output's own
learned surface**, tinted by a hue fixed by the index, breathing in lock-step
with the value on the wire. *"The screen is pulsing"* IS *"that CC is
moving"*, which is the only fact the user needs while their other hand clicks
Learn — and they see what that output actually DOES across the pad before
mapping it. Zero text.

Assign is a SETUP concern (Decision 3), so it is not reachable from PLAY:
`A` on the console, or the hardware button in TRAIN. Inside it, the right two
thirds advance and the left third goes back, and the field itself leans right
(12 % dimmer on the left) — a gradient, not a widget.

## Consequences

* `core/outputs.h` is the ONLY place in the app that knows an output has a
  kind. The vector shape asymmetry — MPE prepends a contact gate, CC and OSC
  do not — lives in exactly one function, `ewa_out_vec()`.
* `stop()` emits nothing on CC. A CC is a number the host is HOLDING; zeroing
  N controllers at shutdown moves a mapped fader nobody asked us to move, and
  since no output is privileged there is no output for which zero is safe.
* `host/app_test.c` is compiled TWICE, once per default, because "the MPE
  bytes are still exact" and "the CC bytes are exact" are two claims and each
  needs its own binary. That is the honest price of changing which output is
  default.
* The CC map is editable on a flashed board over the console (`cc 2 30 hi`,
  `ch 10`, `rate 10`) — validated on a copy, so a rejected line changes
  NOTHING and a typo cannot half-configure an instrument.
