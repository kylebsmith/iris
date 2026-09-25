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

Here is the analogy I want to keep for the rest of this.

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

At the start, the sixty connection strengths are set to small random values, and
the fifteen offsets are set to exactly zero. The network is a friend who has never
tasted your coffee and is guessing. Then this happens, over and over:

1. Take one of your demonstrations. Feed the hand position in.
2. See what sound comes out.
3. Compare it to the sound you actually wanted. The gap is the **error**.
4. Nudge every weight a tiny bit in the direction that would have made that gap
   smaller.

One pass through all your demonstrations is called an **epoch**. iris does
thousands of them. In a small worked example that ships with the library — three
demonstrations, two inputs, three outputs — it runs 2,557 epochs and gets the
error down to about one millionth. On a laptop that takes less than a
millisecond. This whole loop is what the word **training** means, and the trained
bundle of weights is what people call a **model**. I will call it the instrument,
because that is what it is.

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
number of demonstrations tested.)

Two related things worth knowing. First, it is repeatable: on the same machine,
built the same way, the same starting random numbers plus the same
demonstrations give you a bit-for-bit identical instrument, every time. (Change
the compiler settings and that can break — the library refuses to build under
the fastest, loosest maths settings for exactly this reason.) Second, you can
deliberately re-roll those starting random numbers and get a *different*
instrument that agrees with your demonstrations almost as closely but takes a
different path between them. Measured: after a re-roll the sound at your
demonstrated poses moves about four and a half times less than the sound in
the gaps. Your
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

None of this is magic, and the library's own documentation is unusually blunt
about the limits. Take them seriously.

**One bad demonstration can wreck it.** Back to the counter. Suppose one day you
had a cold, tasted a cup you would normally hate, and said "yes, that one." Your
friend now has one demonstration that flatly contradicts the other nineteen. They
do not know which one is the lie, so they compromise, and every cup they make
afterward is slightly wrong.

The same thing happens with a bad take — a gesture recorded while your hand
slipped. There is a worked example of exactly this. An instrument trained on
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
something loudly enough to flag it a few times in every hundred. So treat what it
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

**The evidence has holes, and they are disclosed.** Every accuracy number was
measured on artificial test data, some of it with noise added to imitate shaky
takes. iris recommends one way of doing the training loop described earlier:
keep going until the error stops improving. On noisy test data that method gets
noticeably *worse* than a cruder one that simply stops early, because a good
learner will happily learn the noise along with the signal. The smoothing
setting repairs most of that, but it costs accuracy on clean data, so no single
setting suits both, and the default waits for a study of real recorded
gestures. Nothing has yet been tested against real recorded human gesture.

Only a few timings have been taken on the actual chip, all of them with the
previous version of the library and none with a saved record of the run yet.
One prediction takes 14.9 millionths of a second. Training took 595
milliseconds at four demonstrations and 2.7 to 3.0 seconds at eight to twenty,
but on test data that finishes after about 4,000 to 9,000 passes. The
library's own test set of twenty demonstrations needs about 18,000 passes, and
that has not been timed on the chip, so nobody yet knows how long typical
training takes there.

## What it costs to run

Once trained, playing the instrument is cheap. One gesture in, one set of sound
parameters out, in about thirty-six billionths of a second on a laptop. On the
small chip it is 14.9 millionths of a second, read from the board itself. At a
thousand gestures per second, that is about 1.5 percent of the chip's time.

The whole instrument, including room for 256 stored demonstrations, occupies
9,264 bytes on a laptop, and a little less on the chip — about nine kilobytes. A single photo from a phone is several
hundred times larger. It never asks the operating system for memory while it
runs, which is part of why it can live on a battery-powered device in your hands
rather than on a server.

That is the shape of the thing. You demonstrate; it tunes seventy-five numbers
until it agrees with you; then it fills in everything you did not demonstrate,
smoothly, and that filled-in space is where you actually play.