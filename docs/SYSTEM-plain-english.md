# What iris is, explained from scratch

## The problem: an instrument that answers your body

Say you want to build an instrument you play by moving your hand. Tilt one way,
the sound gets brighter. Lean in, it gets louder. You want it to feel like *your*
hand, not like somebody's idea of a hand.

You could try to write the rules down. "When the tilt is 0.4 and the height is
0.2, the brightness should be 0.31." That is miserable work, and it never feels
right, because you do not actually know the rules. You only know what you like
when you hear it.

**iris** is a small piece of software that removes the need to write those rules
down. Instead of describing what you want, you show it. It is a *library*, which
just means a chunk of code that other programs borrow rather than a program you
open by itself. It is one file. Once it is
running it occupies about nine kilobytes of memory, which is what lets it live on
a chip the size of a postage stamp.

## Showing it how you like your coffee

One analogy runs through the rest of this page.

Suppose you are teaching a friend to make coffee the way you like it. You never
give them a recipe. You just stand at the counter and make four or five cups in
front of them. This one is my early-morning cup: strong, lots of milk. This one
is my after-dinner cup: small, no milk, hot. Each cup is a **demonstration** — a
paired thing. *This situation* goes with *this result*.

That is exactly what you do with iris. You strike a pose with your hand, and at
the same time you set the sound the way you want it, and you press record. Hand
position in, sound settings out. One pair. Do that twenty times or so and you
have twenty demonstrations. In the software's language these are called
**examples**, and twenty is a realistic working number — the library's tests use
anywhere from five to two hundred.

Note what you did *not* do. You did not describe your taste. You demonstrated it,
and demonstrating is easy.

## What the computer is actually doing when it "learns"

Your friend, watching you make coffee, is not memorising cups. They are adjusting
something internal — a rough sense of how much milk goes with how much
grogginess. They tune that sense until the cups they make match the cups you made.

The computer does the same thing, and here it is concretely. Inside iris there is
a small set of numbers — for a typical setup, seventy-five of them. Not
seventy-five million. Seventy-five. These numbers are called **weights**. Sixty
of them are connection strengths: each one is a dial controlling how hard one
signal pushes on the next. The remaining fifteen are offsets, which shift a
signal up or down regardless of what comes in.

All of them together are arranged in a particular shape: the inputs feed into a
middle layer of twelve small units, and that middle layer feeds the outputs.
There is one middle layer, shared by every output. That arrangement — numbers, in
layers, each layer feeding the next — is what people mean by a **neural network**.
It is called that because the layered shape was loosely inspired by neurons. The
resemblance stops there. It is arithmetic.

## The network is a surface

Here is the picture to hold on to. Imagine one input and one output: hand
height along the bottom of a graph, brightness up the side. Each demonstration
is a dot on that graph. The network, whatever its weights happen to be, draws
a smooth curve across the whole graph: for every hand height, some brightness.
With two inputs the curve becomes a surface, like a sheet of fabric draped over
a floor, its height at each spot the sound you get with your hand there. More
inputs and outputs mean more dimensions than anyone can draw, but the idea does
not change.

**Training is fitting that curve through your dots.** The seventy-five weights
set the shape of the curve. Change a weight and the curve bends. Training means
finding weights whose curve passes through, or very close to, every dot you
demonstrated.

## How training finds the curve: guess, measure, nudge, repeat

At the start, the sixty connection strengths are set to small random values, and
the fifteen offsets are set to exactly zero. That draws some arbitrary curve
that passes near none of your dots. The network is a friend who has never
tasted your coffee and is guessing. Then this happens, over and over:

1. **Guess.** Take one of your demonstrations. Feed the hand position in and
   see what sound comes out: that is the curve's current height at that spot.
2. **Measure the error.** Compare it with the sound you actually wanted. The
   gap is the **error**.
3. **Nudge.** Change every weight a tiny bit in the direction that would have
   made that gap smaller. The curve moves a little towards that dot.
4. **Repeat**, with the next demonstration.

Each nudge is small, and a nudge towards one dot can pull the curve slightly
away from another, which is why it takes many rounds for the curve to settle
through all of them at once. One pass through all your demonstrations is called
an **epoch**. iris does thousands of them, and stops when another two thousand
epochs would no longer make the error meaningfully smaller. In a small worked
example that ships with the library (`examples/01_hello.c`: three
demonstrations, two inputs, three outputs) it runs 2,557 epochs and gets the
error down to about one millionth. This whole loop is what the word
**training** means, and the trained bundle of weights is what people call a
**model**. Here it is called the instrument, because that is what it is.

**Delete a take and train again, and the curve re-fits without it.** Training
always starts over from the same starting weights and fits whatever dots are
there now. Remove a dot and the next training run draws a curve through the
rest as if that dot had never existed. Add one and the curve bends to reach it.
That is how you edit an instrument: change the demonstrations, then train.

## The part that matters: the gestures you never showed

Your friend has watched you make five cups. Now it is a rainy Tuesday afternoon,
which is a situation you never demonstrated. They do not freeze. They make you
something sensible in between the cups they saw.

The network does that too, and this is the entire musical point. In that shipped
example, the person demonstrated only three hand positions. Ask it about a
position halfway between two of them — a position it has never seen — and it
gives a smooth, plausible answer that sits between the two. In the library's own
test program, an instrument trained on twenty demonstrations was asked about 121
positions it had never seen. On average it missed by less than one percent of the
available range; at its single worst point it missed by fifteen percent.

That is why you would use this rather than a lookup table — a stored list of
every pose you recorded, played back on the closest match. A lookup table only
knows the twenty poses. A trained network gives you the whole continuous space
between them, and you play in that space. You demonstrate the corners of the
territory and then you improvise inside it. (iris ships the lookup-style
behaviour too, and it is a real choice, not a worse one: the lookup returns your
demonstrations back exactly, which the network never quite does — in the
library's tests it misses its own demonstrations by a fraction of a percent.
The network is better everywhere in between, by roughly two-fold at every
number of demonstrations tested; iris-studies S14 holds that comparison.)

Two related things worth knowing. First, it is repeatable: on the same machine,
built the same way, the same starting random numbers plus the same
demonstrations give you a bit-for-bit identical instrument, every time. (Change
the compiler settings and that can break — the library refuses to build under
the fastest, loosest maths settings for exactly this reason.) Second, you can
deliberately re-roll those starting random numbers and get a *different*
instrument that agrees with your demonstrations almost as closely but takes a
different path between them. In the library's test program, after a re-roll
the sound at your demonstrated poses moves about four and a half times less
than the sound in the gaps. Your
demonstrations stay put; the in-between changes character. That is a creative
tool, not a bug.

## AI, machine learning, neural network — the actual distinction

Briefly, and correctly.

**Artificial intelligence** is the loosest word of the three. It is an umbrella
term for getting computers to do things that seem to require judgement. Chess
programs from the seventies count. So do chatbots.

**Machine learning** is one approach inside that umbrella: instead of a person
writing the rules, the computer derives the rules from examples. That is the
coffee-demonstration idea. Machine learning is a category, not a technique.

**A neural network** is one technique inside machine learning — the layered
arrangement of weights described above. There are plenty of others; iris also
includes one that simply finds the nearest demonstration you recorded and gives
it back to you unchanged.

So: iris is a small neural network, seventy-five numbers, learning from around
twenty demonstrations. The systems that write essays run the same kind of
arithmetic — weights, layers, error, nudging — with hundreds of billions of
numbers instead of seventy-five. Same word, wildly different animal. It is fair
to call iris AI, and it is more useful to call it a small neural network.

## What it cannot do, and how it breaks

None of this is magic. Take the limits seriously.

**One bad demonstration can wreck it.** Back to the counter. Suppose one day you
had a cold, tasted a cup you would normally hate, and said "yes, that one." Your
friend now has one demonstration that flatly contradicts the other nineteen. They
do not know which one is the lie, so they compromise, and every cup they make
afterward is slightly wrong.

The same thing happens with a bad take — a gesture recorded while your hand
slipped: the curve bends towards the wrong dot and drags its surroundings with
it. There is a worked example of exactly this (`examples/02_fix_a_mistake.c`).
An instrument trained on
fourteen good demonstrations answered one particular gesture with the three sound
settings 0.618, 0.500 and 0.383 (each parameter runs from 0 to 1). One bad
demonstration was added, and the same gesture then produced 0.087, 0.937, 0.071.
Completely different, and wrong. Deleting the bad take and re-fitting brought it
back to 0.618, 0.500, 0.383 exactly — the program prints all of this, and those
are its numbers, not a retelling. The
quieter case is the more common one and the more dangerous: a slightly-off
demonstration that does not break anything, just bends everything a little.

iris can often point at the culprit. It keeps a running score of how much each
individual demonstration is fighting the others, and it points at the one that
stands out. In the worked example it correctly named the bad take. Be clear
about how far that goes. On made-up test data, where one take is wrong on one
of its outputs and everything else is clean, it names that take nearly every
time, even when the take is only five percent off. Real mistakes are messier
than that, and it has never been tried on recorded human gestures. It also
raises false alarms: on clean sessions with nothing wrong, it points at
something loudly enough to flag it a few times in every hundred (the library's
`tests/elm.c` measures both). So treat what it
names as a take to listen to again, not a take to delete unheard.

When it does find one: you delete that demonstration and train again. In the
worked example the bad take had pushed one of the network's numbers against
the library's safety limit, and the library said so. Deleting the take and
training again fixed that too: training always starts over from the same
starting numbers, so the deleted take leaves nothing behind, and the
instrument came back to exactly 0.618, 0.500, 0.383, the numbers it started
with. That is the repair procedure. Find the contradiction, delete it, train
again.

Two caveats on that. The flagging stays silent until you have at least twelve
demonstrations, because with fewer than that there is not enough agreement to
measure disagreement against. And deleting a bad take and re-recording it does
*not* give you back exactly the instrument you would have had if you had never
made the mistake, because the re-recorded take is now last in the list and the
order changes the training a little. It gives you a very close one: a few times
closer than re-rolling would.

**It cannot handle a hard edge.** If you want the sound to change abruptly at a
boundary — nothing, nothing, nothing, then suddenly everything — this shape of
network is bad at it. It rounds the corner off, and on sharp test targets it is
the case where the library's safety limit trips most often. Your friend cannot
learn a cliff by tasting cups on either side of it.

**It has no sense of time.** It looks at where your hand is *right now*. It has no
idea where your hand was a moment ago. A gesture that is defined by its motion — a
flick, a swipe, a shape drawn in the air — is outside what this can do.
Wekinator, the older tool iris is modelled on, has a whole subsystem for that,
several thousand lines of it. iris has none: search the file for anything to do
with time and you get nothing.

**It cannot reach past what you showed it.** The output is deliberately held
inside the range you demonstrated — it is not allowed out, and there is no way to
switch that off. If your loudest demonstration was at 80 percent, you cannot get
to 100 percent by pushing your hand further. You get a wall. This was chosen on
purpose, so the instrument can never send an insane value to hardware, and it is
a genuine cost.

**The evidence has holes.** Every accuracy number was
measured on artificial test data, some of it with noise added to imitate shaky
takes. iris recommends one way of doing the training loop described earlier:
keep going until the error stops improving. On noisy test data that method gets
noticeably *worse* than a cruder one that simply stops early, because a good
learner will happily learn the noise along with the signal (iris-studies S08
holds that comparison). The smoothing
setting repairs most of that, but it costs accuracy on clean data, so no single
setting suits both, and the default waits for a study of real recorded
gestures. Nothing has yet been tested against real recorded human gesture.

Training takes much longer on the small chip than on a laptop. On one board,
an ESP32-S3 running at 240 MHz, the library's own test set of twenty
demonstrations needs about 18,000 epochs and took 13.4 seconds; ten
demonstrations took 10.6 seconds and fifty took 22.1 seconds (the board log,
[`board/2026-09-25-es3c28p.txt`](board/2026-09-25-es3c28p.txt)). A sketch can
train in small slices so the screen keeps drawing meanwhile. Only one board has
been measured.

## What it costs to run

Once trained, playing the instrument is cheap. One gesture in, one set of sound
parameters out, in about thirty-six billionths of a second on a laptop. On the
small chip it is 14.9 millionths of a second (the same board log). At a
thousand gestures per second, that is about 1.5 percent of the chip's time.

The whole instrument, including room for 256 stored demonstrations, occupies
9,264 bytes on a laptop, and a little less on the chip — about nine kilobytes. A single photo from a phone is several
hundred times larger. It never asks the operating system for memory while it
runs, which is part of why it can live on a battery-powered device in your hands
rather than on a server.

That is the shape of the thing. You demonstrate; it tunes seventy-five numbers
until it agrees with you; then it fills in everything you did not demonstrate,
smoothly, and that filled-in space is where you actually play.