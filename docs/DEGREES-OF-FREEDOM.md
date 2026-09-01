# How many inputs, how many outputs

Short answer: **be generous with outputs and stingy with inputs — but the
reason is not what it sounds like, and the condition on "generous" matters.**

Reproduce everything here with `docs/degrees-of-freedom.c`:

```bash
cc -std=c99 -O2 -I. docs/degrees-of-freedom.c -o /tmp/dof -lm && /tmp/dof
```

Twelve demonstrations, twelve hidden units, error measured on 400 fresh points
the network never saw, averaged over twelve seeds. Error is per output channel,
so every column below compares directly.

## The measurement

**Inputs are close to free here** (two outputs, target driven by all inputs):

| inputs | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| error | 0.0154 | 0.0184 | 0.0259 | 0.0249 | 0.0223 | 0.0217 | 0.0218 | 0.0206 |

**Outputs that are unrelated to each other are expensive:**

| outputs | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| error | 0.0205 | 0.0431 | 0.0551 | 0.0793 | 0.1084 | 0.1293 | 0.1508 | 0.1699 |

**Outputs that move together are free, and slightly better than free:**

| outputs | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| error | 0.0205 | 0.0184 | 0.0151 | 0.0144 | 0.0156 | 0.0168 | 0.0158 | 0.0148 |

Eight coordinated outputs are *more* accurate than one. Every demonstration
supervises all eight at once, so the shared hidden layer gets eight views of
the same underlying gesture instead of one.

## It is not a capacity problem

The obvious guess is that twelve hidden units run out of room. They do not.
Growing the network with the output count barely moves the unrelated case:

| outputs | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| error (hidden grown to 18…60) | 0.0210 | 0.0423 | 0.0563 | 0.0840 | 0.0966 | 0.1218 | 0.1424 | 0.1680 |

Twelve demonstrations cannot specify eight independent functions, and no amount
of network fixes that. It is a shortage of *evidence*, not of capacity.

## What this means for an instrument

The expensive thing is not the number of control parameters. It is **how much
independent behaviour you are asking for**.

Eight synth parameters that all move as one coordinated gesture — a posture, a
shape, a hand opening — cost nothing over one parameter. Eight parameters you
want to move genuinely independently need roughly eight times the
demonstrations, because you have asked eight separate questions.

This is why "map the gesture to everything" is good advice and "give me eight
independent faders" is not. A real instrument is the first thing. Robotics
calls the coordinated version a **synergy**; the same idea, older name.

## Caveats, stated plainly

- Synthetic targets (sums of sines), not recorded human gestures. The shape of
  the result should hold; the exact numbers are about this test.
- Twelve demonstrations; `docs/degrees-of-freedom.c` fixes `ND=12` and prints
  that one case only. The forty-demonstration run was done separately and is not
  reproducible from the shipped program — everything improved and the ordering
  between the three cases was unchanged, but take that as a note, not a result
  you can re-run here.
- "Inputs are free" holds *here* because the target uses every input smoothly.
  A target with sharp structure in many input dimensions is the classic curse
  of dimensionality and will cost you. Inputs are cheap in this test, not in
  general; outputs-that-move-together are cheap in a way that does generalise,
  because the mechanism is shared supervision rather than easy geometry.
