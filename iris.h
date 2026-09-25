/* SPDX-License-Identifier: BSD-3-Clause
   Copyright (c) 2026 Kyle Smith */
/* ============================================================================
   iris.h  —  interactive machine learning for handmade instruments
   v0.1.0 · single file · C99 · no dependencies · no malloc · no libc

   You show it a handful of examples of "when I do THIS, it sounds like THAT".
   It learns a mapping and fills in everything in between.

   This is the whole brain of the instrument. The same file compiles for a
   laptop, a web browser, and an ESP32-S3, because it contains no hardware,
   no operating system, and no library calls. It is pure arithmetic on
   memory you hand it.

   THE ZERO-DEPENDENCY CLAIM, STATED EXACTLY. A translation unit exercising
   the whole public API compiles under -std=c99 -ffreestanding -nostdlib at
   -O0/-O2/-Os and links with ZERO undefined symbols — but only with
   -fno-stack-protector, ON A HOST. Verified 2026-08-27, Apple clang 17, arm64.

   AND ON THE CHIP IT IS ACTUALLY FOR, IT IS NOT ZERO. Measured 2026-08-30 with
   the ESP32-S3's own compiler (xtensa-esp32s3-elf-gcc, -Os -ffreestanding
   -fno-stack-protector), which this line previously marked UNVERIFIED:

     the playing path        __divsf3, memset, sqrtf
     + iris_loo_error        + __adddf3 __divdf3 __extendsfdf2 __floatsidf
                               __muldf3 __subdf3 __truncdfsf2
     + iris_suggest_smoothing  the same, plus memcpy
     + iris_train_elm        adds nothing

   __divsf3 is single-precision DIVISION: the S3's floating-point unit has no
   divide instruction, so every float division is a libgcc call. memset and
   sqrtf are the compiler's and libm's. None of this is a call this source
   writes, and all of it is present on every Arduino build anyway -- but the
   sentence "zero undefined symbols" is FALSE on the target, and it is now
   stated with the compiler, the flags and the list rather than as a claim.

   THE DOUBLES ARE REAL AND THEY ARE ONE FUNCTION. The __*df3 routines above
   are 64-bit soft float, which rule 3 below says this library does not use.
   iris_loo_error accumulates its error sum in double on purpose (iris.h, PART
   8c) and iris_suggest_smoothing calls it. That is a deliberate numerical
   choice in a diagnostic that is not on the playing path, and it is the ONLY
   exception -- the playing path has no doubles anywhere. Rule 3 is restated
   below with that exception named, because a rule with a silent exception is
   worse than no rule.

   THE THREE RULES THIS FILE OBEYS
     1. No malloc.  You give it one block of memory; it never asks for more.
        You always know exactly how much RAM the instrument uses.
     2. No libc.    No printf, no math.h. Everything it needs is in here.
     3. No doubles ON THE PLAYING PATH. The ESP32-S3 does 32-bit float in
        hardware and 64-bit float in slow software emulation. The one
        exception is iris_loo_error (and iris_suggest_smoothing, which calls
        it), a diagnostic that accumulates in double deliberately; measured
        above.

   USAGE
     static unsigned char mem[IRIS_ARENA(2, 12, 3, 64)];
     iris *k = iris_init(mem, sizeof mem, 2, 12, 3, 64, 12345);

     iris_record(k, gesture, sound);     // do this a few times
     iris_train_converge(k, 0, 0, 0);    // trains until the error plateaus
     iris_predict(k, gesture, sound);    // now play

   THE WORDS THIS FILE USES, defined once, here, before it uses them.
   CONTRIBUTING.md asks for no bare acronyms and the audience includes musicians and first-year
   students, so:

     EPOCH        one pass over every demonstration you have recorded. Training
                  is thousands of these. "9,000 epochs" means the network saw
                  each of your takes 9,000 times.
     ELM          extreme learning machine. A second, instant way to train:
                  freeze the random middle layer and solve the output layer
                  exactly, in one step, instead of nudging it thousands of
                  times. PART 8d.
     RIDGE        a small number added down the diagonal of a matrix before
                  solving it, which stops the solve failing when two
                  demonstrations are nearly identical. PART 8d.
     SGD          stochastic gradient descent — nudging the weights after each
                  single demonstration rather than after all of them.
     ULP          unit in the last place: the smallest change you can make to a
                  floating-point number. "1 ulp" means one step, the smallest
                  difference two floats can have.
     CHOLESKY     a standard, fast way to solve a symmetric system of linear
                  equations. Used once, in the ELM path.
     NORMAL MATRIX  the square matrix that least-squares fitting produces and
                  Cholesky then solves.
     ODR          the one-definition rule: C and C++ require that a thing is
                  defined identically everywhere it appears.

   ON TRAINING TIME. iris_train_converge runs until the training error stops
   improving, with a hard ceiling — typically 9,000-18,000 epochs, which is
   ~25-45 ms on a laptop and ~1-4 s on an ESP32-S3 at 20-50 examples. That is
   twenty times the old fixed 600-epoch recommendation and it buys a 5.9x
   better recall of your own demonstrations; the table is in PART 8. If you
   need the UI to stay alive across those seconds, take the same run in
   slices: iris_train_begin / iris_train_slice / iris_train_progress, which is
   bit-identical to the blocking call.

   iris_train_epochs(k, n) is still here, unchanged and permanent: it is the
   fixed-epoch backprop that Wekinator's Weka MultilayerPerceptron does, and
   the audit pins its output to the bit.

   ============================================================================ */

#ifndef IRIS_H
#define IRIS_H

/* ============================================================================
   THE WHOLE INTERFACE, ON ONE SCREEN

   Eleven functions. Everything else in this file is detail you can reach for
   later. `k` is the instrument. `in` and `out` are plain float arrays you own.

     iris *iris_init(mem, sizeof mem, n_in, n_hid, n_out, cap, seed)
         Hands back an instrument built inside YOUR memory. The four numbers
         are: how many sensor values come in, how wide the hidden layer is
         (12 is a good answer; 8 is the minimum), how many things you control,
         and how many demonstrations you can store. They must match the four
         you gave IRIS_ARENA. Returns 0 if they do not.

     int   iris_record(k, in, out)     in: n_in floats     out: n_out floats
         Stores one demonstration: this gesture goes with that sound.
         Returns its identifier (1 or higher). Returns 0 if it refused.

     int   iris_train(k)
         Fits the demonstrations you have now, from a defined start.
         Returns 1, or 0 if it refused. How WELL it fits is a separate
         question: iris_last_error(k).

     int   iris_is_trained(k)          did the last fit actually happen?
         1 if this instrument is fitted, 0 if it is not. Correct after EVERY
         trainer in this file -- which matters, because `if (iris_train_elm(...))`
         is FALSE on its best outcome and `if (iris_train_converge(...))` is
         TRUE on refusal. See "HOW EVERY FUNCTION REPORTS FAILURE" for the
         measured table. If you only ever ask one question about training,
         ask this one.

     void  iris_predict(k, in, out)    READS n_in, WRITES n_out floats
         The playing call. It writes exactly n_out floats into `out`; if your
         array is shorter than that, it writes past the end and nothing warns
         you. This is the one thing to get right.

     int   iris_count(k)               how many demonstrations are stored
     int   iris_delete_id(k, id)       remove one, by the identifier above
     int   iris_worst_example_id(k, m) which demonstration fights the others
     iris_status iris_get_status(k)    is the INSTRUMENT unwell? 0 is healthy
     size_t iris_save(k, buf, cap)     bytes written, or 0
     int   iris_load(k, buf, n)        1, or 0

   FAILURE, in two rules and no exceptions:
     A call that either works or does not returns 0 for "did nothing".
     A call that returns a MEASUREMENT returns it, or -1 if it refused.
   iris_get_status answers a different question -- whether the INSTRUMENT is in
   trouble. Zero means opposite things in the two places, so do not carry one
   habit across: a RETURN VALUE of 0 is bad news (the call did nothing), and a
   STATUS of 0 is good news (IRIS_STATUS_OK, nothing is wrong). Two of these
   sentences used to say "0 is the good news in both" and "0 is the bad news in
   both", and neither was right about both.

   THREADING: never touch the same instrument from two places at once. That is
   the entire contract; see the note above iris_get_status for why.

   Units: none. Feed it raw sensor readings. It fits its own range to whatever
   you actually give it, so scaling, centring and normalising are not merely
   unnecessary, they are the wrong thing to do.
   ========================================================================= */


/* --------------------------------------------------------------------------
   FLOAT DETERMINISM CONTRACT

   "Same seed, same instrument" is a bitwise promise, and fused multiply-add
   contraction breaks it: the same source at -ffp-contract=off / on / fast
   produces three DIFFERENT weight blobs on Apple clang 17 / M4 (measured).
   Four defences, cheapest first:

   1. Tripwires for the flags that change the arithmetic. -ffast-math implies
      contract=fast AND removes the NaN semantics the guards below depend on.
      Refuse to compile.                                                    */
#if defined(__FAST_MATH__)
#error "iris: -ffast-math / -Ofast breaks same-seed bit-determinism and disables NaN trapping. If you did not pass this yourself, your board package did: check compiler.optimization_flag in its platform.txt (Adafruit nRF52 sets -Ofast there). Build without it."
#endif
/* -ffinite-math-only is one of the flags -ffast-math turns on, but on its own
   it does NOT set __FAST_MATH__, so it needs a tripwire of its own. Without
   one, every guard in the library is optimised away: iris_isbad folds to
   false, poisoned demonstrations are accepted, and a broken sensor produces a
   plausible number and a healthy status. Measured on Apple clang 17 with this
   tripwire removed. */
#if defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__
#error "iris: -ffinite-math-only tells the compiler no NaN or infinity can exist, which deletes every guard in this library. Build without it."
#endif
/* THE COMPONENT FLAGS, which -ffast-math turns on and which also work alone.
   -freciprocal-math, -funsafe-math-optimizations and -fassociative-math each
   change the instrument with no diagnostic of any kind (measured). GCC
   announces them and clang does not, so this catches them on GCC only --
   which is the compiler for every ESP32, AVR and RP2040 build, and is where
   it matters most.
   No Arduino core passes any of these: checked platform.txt for arduino:avr,
   esp32:esp32, rp2040:rp2040 and STMicroelectronics:stm32. Reaching this
   #error takes a deliberate flag.

   WHAT THIS DOES NOT CATCH, stated so the coverage is not overstated:
   -freciprocal-math and -funsafe-math-optimizations are caught on GCC (the
   latter defines all four macros). -fassociative-math passed DIRECTLY defines
   no macro at all on gcc-15 -- measured with -dM -E -- so it is undetectable
   here and it does change the instrument. On clang no component flag is
   detectable, because clang defines none of these macros, and clang 22's
   -ffp-model=fast defines only __FINITE_MATH_ONLY__ as 0: it compiles without
   a word and moves the golden hash in tests/audit.c (measured). */
#if defined(__RECIPROCAL_MATH__) && __RECIPROCAL_MATH__
#error "iris: -freciprocal-math rewrites division as multiplication by a reciprocal and changes the instrument. Build without it."
#endif
#if defined(__ASSOCIATIVE_MATH__) && __ASSOCIATIVE_MATH__
#error "iris: -fassociative-math / -funsafe-math-optimizations reorders floating-point arithmetic and changes the instrument. Build without it."
#endif
/* 2. Wider intermediate results. C lets a compiler carry float arithmetic in
      a wider format and round only when a value is stored, and
      __FLT_EVAL_METHOD__ (FLT_EVAL_METHOD in <float.h>) says which format:

        0        every operation rounds to its own type. This file is pinned
                 to this arithmetic.
        16, 32   the same as 0 for float: only the half-precision type
                 _Float16 is widened. GCC reports 16 on 64-bit ARM in GNU mode
                 when half-precision arithmetic is enabled, for example
                 -std=gnu17 -mcpu=cortex-a76, the Raspberry Pi 5's core; the
                 golden hash in tests/audit.c holds there (measured).
        1, 2     float carried as double, or as the 80-bit x87 format. 32-bit
                 x86 without SSE reports 2, and there the golden hash and
                 every other instrument hash measured come out different,
                 with no diagnostic (measured: clang 22
                 --target=i686-linux-gnu -mno-sse -mfpmath=387).
        -1       not known. There is nothing to pin.

      Anything but 0, 16 or 32 refuses to compile. tests/targets.sh checks
      that this fires on x87 and stays silent on every other target this
      library is built for.                                                 */
#if (defined(__FLT_EVAL_METHOD__) && __FLT_EVAL_METHOD__ != 0 \
     && __FLT_EVAL_METHOD__ != 16 && __FLT_EVAL_METHOD__ != 32) \
 || (!defined(__FLT_EVAL_METHOD__) && defined(FLT_EVAL_METHOD) \
     && FLT_EVAL_METHOD != 0 && FLT_EVAL_METHOD != 16 && FLT_EVAL_METHOD != 32)
#error "iris: this build carries float arithmetic in a wider format than float (see __FLT_EVAL_METHOD__), which changes every instrument. On 32-bit x86, build with -msse2 -mfpmath=sse."
#endif
/* 3. Forbid contraction at the source level. Clang honours this pragma at
      default and -ffp-contract=on (measured: blob becomes bit-identical to
      a -ffp-contract=off build); clang IGNORES it under -ffp-contract=fast.
      THIS LINE USED TO SAY GCC IGNORES IT ALWAYS. That is wrong, and wrong in
      the direction that undersold our own defence: measured 2026-08-30 with
      gcc-15 -O2 -ffp-contract=fast, the pragma present gives the SAME
      prediction hash as the clang baseline, and stripping the pragma from a
      copy changes it. The pragma is honoured on GCC and is the thing doing the
      work. A -ffp-contract=fast clang build still must
      pass -ffp-contract=off explicitly. The golden-blob audit check catches
      any build where neither defence held.                                 */
/* GNU compilers ignore the standard pragma, and in GNU mode -- which is what
   the Arduino IDE builds with, g++ -std=gnu++17 -O2 -- they contract by
   default. Measured: the same seed and the same demonstrations produced a
   DIFFERENT instrument there, which breaks the one promise this library
   exists to keep. They do honour this, at file scope, in every mode. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("fp-contract=off")
#endif
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif
/* 4. Golden-blob audit vector (in tests/audit.c) — the runtime backstop.   */

#include <stddef.h>
#include <stdint.h>

/* THE ONLY VERSION NUMBER FOR THIS LIBRARY. Nothing else may state one.

   SEPARATE AXIS: the SAVE FILE format version is NOT this number. It is
   written into every saved file and changes only when the file's layout or
   meaning changes; see PART 9. */
#define IRIS_VERSION_MAJOR 0
#define IRIS_VERSION_MINOR 1
#define IRIS_VERSION_PATCH 0
#define IRIS_VERSION_STRING "0.1.0"

/* THE MAXIMA ARE THE SIZE OF EVERY WORKING ARRAY, SO ON A SMALL BOARD THEY
   ARE THE STACK BUDGET.

   Nine arrays inside this file are sized by these numbers rather than by the
   shape you actually asked for -- iris_internal_train_run alone reserves
   float x[IRIS_MAX_IN] and float t[IRIS_MAX_OUT], 192 bytes, whether your
   instrument has 32 inputs or 2. Measured with avr-gcc -Os -fstack-usage on
   an atmega328p: iris_internal_train_run 286 bytes, iris_predict 164. An Uno has
   2 KB of memory in total and the getting-started sketch leaves a few hundred
   bytes of stack, so the defaults below do not fit it with room to spare.

   They are #ifndef so you can shrink them. Define them BEFORE including this
   file and every working array shrinks with them:

       #define IRIS_MAX_IN  4
       #define IRIS_MAX_OUT 4
       #define IRIS_MAX_HID 12
       #include "iris.h"

   Measured, same compiler and flags (avr-gcc 7.3.0, -mmcu=atmega328p -Os
   -fstack-usage): the deepest frame on the record/train/predict path falls
   from 308 bytes to 148 -- a saving of 160. Two documents used to quote 340
   and 132 for this; that pair no longer reproduces, and the 160-byte saving
   does. A bare caller with no locals of its own measures 288 and 128.
   The only rule is that they must be at least as large as the n_in, n_out and
   n_hid you pass to iris_init -- which iris_init checks, and refuses if not.
   On a 32-bit board (ESP32, RP2040, STM32) leave them alone; the defaults cost
   nothing you have. */
#ifndef IRIS_MAX_IN
#define IRIS_MAX_IN   32   /* sensor features in  */
#endif
#ifndef IRIS_MAX_OUT
#define IRIS_MAX_OUT  16   /* sound parameters out */
#endif
/* THE CAP HAS TO FIT THE MACHINE'S SIZE TYPE.

   4,096 demonstrations is the right ceiling on a 32-bit or 64-bit target: it
   is the point where the arena arithmetic would start to overflow. On a 16-bit
   size type -- every Arduino AVR board -- overflow arrives far sooner, and it
   arrives IDENTICALLY in IRIS_ARENA and in iris_size, so the arena bound wraps
   to the same wrong number and cannot see the problem it was written to catch.
   Compiled for a Mega, IRIS_ARENA(8,8,8,894) came out around 4,000 bytes
   instead of 69,672 and the build succeeded.

   So the cap scales with the machine rather than assuming one. 255 keeps the
   largest legal arena comfortably inside a 16-bit size type, and 255 takes is
   already far more than anyone records by hand. */
#define IRIS_MAX_EX   ((int)(sizeof(size_t) >= 4 ? 4096 : 255)) /* demonstrations. THE BOUND EXISTS TO STOP AN
                            OVERFLOW, not because 4096 is musically special.
                            iris_size multiplies cap by (n_in+n_out) and by
                            sizeof(float); on a 32-bit target (the ESP32-S3)
                            size_t is 32 bits, so a large enough cap wraps,
                            iris_size returns a SMALL number, the arena check
                            passes, and the example store runs off the end of
                            the caller's buffer. At the maxima (32 in, 16 out)
                            4096 examples is ~786 KB of examples alone, already
                            past the S3's 512 KB, so nothing legitimate is being
                            refused. Added 2026-08-27 (gap C11).            */
#ifndef IRIS_MAX_HID
#define IRIS_MAX_HID  64   /* hidden units         */
#endif

#ifndef IRIS_API
/* `static inline`, not plain `static`. A single-header library defines every
   function in every translation unit that includes it, and a caller who uses
   five of them is not doing anything wrong. With plain `static`, -Wall -Wextra
   then emits an unused-function warning for each of the other sixty — measured
   2026-08-27: THIRTY warnings compiling the minimal example, examples/00_minimal.c.
   That is a terrible first thirty seconds for someone who just cloned this.
   `inline` tells the compiler the definition is expected to be unused here,
   silencing that without changing linkage, ODR behaviour or codegen. */
#define IRIS_API static inline
#endif

/* --------------------------------------------------------------------------
   MEMORY

   Everything the instrument knows lives in one contiguous block you own.
   IRIS_ARENA() computes the size at compile time so you can write

       static unsigned char mem[IRIS_ARENA(2, 12, 3, 64)];

   and put it in .bss instead of on a heap. On an MCU this is the difference
   between "I know this fits" and "I hope this fits".
   -------------------------------------------------------------------------- */

/* EVERY PRODUCT IS COMPUTED IN unsigned long, WHICH C GUARANTEES IS AT LEAST
   32 BITS, AND NOT IN size_t.

   On a 16-bit size_t target -- every Arduino AVR board -- these products wrap.
   That on its own would be survivable if anything noticed, but iris_size wrapped
   IDENTICALLY, so iris_init compared a wrapped need against an equally wrapped
   array size and could not refuse: IRIS_ARENA(24,63,16,254) came out as 88
   bytes for an instrument that needs 65,624, and the first loop of iris_reseed
   then wrote 6,048 bytes into those 88. Verified with avr-gcc for atmega328p.

   Computing wide makes the true number appear. In C, that number is then too
   large for an array and the COMPILER refuses the declaration -- a build error
   naming the array, which is the outcome we want.

   IN C++ IT IS A RUNTIME REFUSAL INSTEAD, AND ARDUINO COMPILES .ino AS C++.
   An array declarator's size in C++ converts to std::size_t, 16 bits on AVR, so
   the bound wraps silently where C errors. Measured with avr-g++ on an
   atmega328p, all four as compile-time assertions:

     IRIS_ARENA(24,63,16,254)          65624   (wide, correct)
     sizeof mem                           88   (the ARRAY narrowed)
     iris_size(24,63,16,254)               0   (cannot be sized here)

   The narrowing threshold and the sizing threshold are the SAME number, so
   every shape the array silently shrinks is a shape iris_size refuses: the
   `need == 0` test in iris_init returns 0 and the sketch gets a null pointer,
   which every example in this repository checks. So the failure is loud, just
   later than it should be -- a message at run time rather than a build error.
   Do not read this as memory corruption; an audit reported it as such and the
   assertions above are why that is wrong. */
#define IRIS_ARENA(NI, NH, NO, NEX)                                              \
  ( (unsigned long)sizeof(iris)                                                  \
  + (unsigned long)sizeof(float)                                                 \
      * ( 2UL*((unsigned long)(NI)*(NH) + (NH) + (unsigned long)(NH)*(NO) + (NO))\
        + (NH) + (NO) + (NH) + (NO)                            /* acts + deltas */\
        + 2UL*((NI) + (NO))                                    /* norm ranges   */\
        + (unsigned long)(NEX)                                 /* residual ledger*/\
        + (unsigned long)(NEX) * ((NI) + (NO)) )               /* examples      */\
  + (unsigned long)sizeof(int32_t) * (unsigned long)(NEX) * 2  /* ids + shuffle */\
  + 64UL )                                                     /* alignment pad */

typedef struct iris iris;

/* --------------------------------------------------------------------------
   HEALTH REPORTING (guard rails)

   Guards never mutate silently: anything they do is announced here. The
   healthy state is 0, so `if (iris_get_status(k))` reads as "is something
   wrong?". (It is spelled IRIS_STATUS_OK, not IRIS_OK: the sink boundary's
   shared error vocabulary in iris_sink.h already owns the bare name
   IRIS_OK — same value, same meaning, different boundary.)
   -------------------------------------------------------------------------- */
typedef enum {
  IRIS_STATUS_OK         = 0,  /* healthy — guards provably touched nothing    */
  IRIS_TRAINING_DIVERGED = 1,  /* weights ran past ±16; clamped and training
                                stopped. Model is usable but suspect: check
                                lr/momentum, or reseed.                      */
  IRIS_NAN_TRAPPED       = 2,  /* NaN/Inf found in an example, the error, or a
                                weight. Poisoned examples: training refused,
                                previous weights preserved. Mid-train blowup:
                                weights re-seeded to a finite start.         */
  IRIS_RIDGE_ESCALATED   = 3,  /* a closed-form solve (ELM) needed its ridge
                                doubled to factor. Result is valid; the data
                                was harder than usual.                       */
  IRIS_NOT_FITTED        = 4,  /* iris_predict was called on an instrument that
                                has never been fitted. Outputs are the centre of
                                the demonstrated range (0 with no
                                demonstrations), never the forward pass over
                                random weights.                              */
  IRIS_STORE_FULL        = 6,  /* iris_record was refused because the store is
                                full. Distinguished from a poisoned reading,
                                which reports IRIS_NAN_TRAPPED: both return 0,
                                and a sketch that printed "full" for either sent
                                the student to delete demonstrations they did
                                not have.                                    */
  IRIS_DIVERGED_STUCK    = 5   /* a previous run diverged and left weights at the
                                clamp. Training refuses until the instrument is
                                rerolled (iris_retrain_new) — the examples are
                                intact, the weights are not.                 */
} iris_status;

/* NaN or Inf, by bit pattern — exponent field all ones. No libc, no fenv,
   and immune to -ffinite-math-only style optimisations on the comparison. */
IRIS_API int iris_isbad(float x) {
  /* Read the bits through a copy the compiler must actually make, not through
     a union it can see through. With a union, clang propagated "this value is
     finite" across the type pun and folded the test to false. Routing it
     through a volatile forces a real store and load, which the optimiser may
     not reason across. (__builtin_memcpy also works on clang but GNU compilers
     turn it into a call to the C library's memcpy -- an undefined symbol, which
     breaks the no-dependency claim. Measured both ways.) */
  volatile float v = x;      /* a store the compiler must actually perform */
  union { float f; uint32_t u; } c; c.f = v;
  return (c.u & 0x7F800000u) == 0x7F800000u;
}

/* Any velocity smaller than this is musically and numerically dead: it can
   never move a weight by even one ulp again. Flushing it to zero (a) matches
   the ESP32-S3 LX7 FPU, which flushes denormals in hardware while the host
   does gradual underflow — closing a real host-vs-device bit divergence —
   and (b) keeps the momentum tail out of denormal territory on hosts that
   stall on denormal arithmetic. 1e-30 is ~8 decades above FLT_MIN, so both
   platforms evaluate the comparison identically.                            */
#ifndef IRIS_TINY
#define IRIS_TINY 1e-30f
#endif
#ifdef IRIS_NO_GUARDS
#define IRIS_FLUSH(v) (v)
#else
#define IRIS_FLUSH(v) ((v) < IRIS_TINY && (v) > -IRIS_TINY ? 0.0f : (v))
#endif

/* Trained-weight audit measured max|w| = 2.8 on the reference tasks; 16 is
   5.7x headroom, so on any healthy run the divergence check never fires and
   the clamp provably never changes a bit. A weight past 16 drives tanh/
   sigmoid so deep into saturation it is indistinguishable from ±1 anyway.   */
#define IRIS_W_LIMIT 16.0f

/* ==========================================================================
   PART 1 — MATH WE PROVIDE OURSELVES

   We can't call math.h, so these are here. They are also *faster* than the
   library versions, which matters more than you'd think: the network calls
   tanh once per hidden unit per direction per example per epoch. With 12
   hidden units, 20 examples and 16,000 epochs that is 7.7 million calls.

   ON THE COST. The primary reason this routine exists is the no-libc rule,
   not speed. On the ESP32-S3, newlib's tanhf is ESTIMATED at 150-400 cycles
   (briefs/03-esp32s3-feasibility.md:112, marked [E] — not measured on the
   part). On a laptop the measured margin over libm tanhf is about 2x, not
   30x. Do not repeat an unqualified "300 cycles" or "worth more than every
   other optimization combined": neither figure survives scrutiny.
   ========================================================================== */

/* THE NONLINEARITY. Chosen, measured, and now permanent.

   p(x) = x(27+x^2)/(27+9x^2), clamped to tanh's codomain.

   THIS IS NOT A CHEAP STAND-IN FOR tanh THAT WE REGRET. It was measured
   against a far more accurate approximant and against true tanh itself, over
   6 target shapes and 2,304 paired runs, and it WON on held-out error. The
   reason is that accuracy is not the objective: this function overshoots tanh
   in the mid-range, which makes it a steeper sigmoid with a hard floor on
   gradient flow past |s| = 3, and that is capacity control. It is doing useful
   work, not merely approximating. Design, arms and numbers: docs/FREEZE.md.

   Two exact facts make the clamp correct rather than arbitrary:
   p(x) - 1 = (x-3)^3/(27+9x^2), so p(3) = 1 EXACTLY, and
   p'(x) = ((x^2-9)/(3(3+x^2)))^2 >= 0, so p is monotone and p'(3) = 0 exactly.
   Clamping the RETURN VALUE is therefore identical to clamping the argument at
   |x| = 3, and strictly better: an argument clamp leaves a 1-ulp escape (10,220
   floats in [2.5,3.0] still evaluate above 1.0f). It is also branch-free, so
   its cost does not depend on the data.

   CONSEQUENCE, STATED PLAINLY AND PERMANENTLY. (1 - a*a) is the derivative of
   TRUE tanh, not of this function, so the backward pass is a surrogate
   gradient -- under-scaled by 2.4-3.3% in aggregate, never wrong-signed. It is
   not a defect being tolerated; it is a described property of a chosen
   nonlinearity, and it can be revisited any time it earns its measured 1.4%
   without changing what a saved instrument means.

   The +/-1e9 test only keeps x*(27+x^2) finite; it is not the saturation point.
   It was documented here as never firing, on the reasoning that pre-activations
   are bounded by IRIS_W_LIMIT. That is wrong, and the guard is load-bearing:
   iris_predict does NOT clamp its input, iris_norm_in scales it, so a reading
   of 1e20 arrives at iris_tanh as 2e20. There x*(27+x^2) overflows to inf and
   27+9x^2 overflows to inf, and inf/inf is a not-a-number -- every hidden unit
   would be NaN. Measured: with the test, iris_predict(1e20) returns 1.000000;
   the bare ratio at that argument is nan.
   Full workings: docs/FREEZE.md, docs/negative-results/. */
IRIS_API float iris_tanh(float x) {
  if (x >  1.0e9f) return  1.0f;
  if (x < -1.0e9f) return -1.0f;
  const float x2 = x * x;
  const float p  = x * (27.0f + x2) / (27.0f + 9.0f * x2);
  return p > 1.0f ? 1.0f : (p < -1.0f ? -1.0f : p);
}

/* Logistic / sigmoid, squashes anything into (0,1). Built from tanh so we
   only have to be fast once. */
IRIS_API float iris_sigmoid(float x) { return 0.5f * (iris_tanh(0.5f * x) + 1.0f); }

/* __builtin_sqrtf compiles to one hardware instruction -- except on GNU
   compilers, which assume it must set errno for a negative input and so also
   emit a call to the C library's sqrtf for that branch. That is an undefined
   symbol, and it breaks the no-dependency claim on the ESP32's own toolchain.
   Build with -fno-math-errno; check-claims.sh verifies it on every compiler it
   can find. This function is never called with a negative argument. */
IRIS_API float iris_sqrt(float x) { return __builtin_sqrtf(x); }
IRIS_API float iris_absf(float x) { return x < 0.0f ? -x : x; }

IRIS_API float iris_clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

/* xorshift32. A tiny, fast, repeatable random number generator.

   Repeatable is the important word. The same seed always produces the same
   sequence, so the same seed always produces the same instrument. That is
   what makes "reroll" a real control rather than a shrug: you can go back. */
typedef struct { uint32_t s; } iris_rng;

IRIS_API uint32_t iris_rand_u32(iris_rng *r) {
  uint32_t x = r->s;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return (r->s = x ? x : 0x9E3779B9u);
}
/* uniform in [-1, 1) */
IRIS_API float iris_rand_sym(iris_rng *r) {
  return (float)(int32_t)iris_rand_u32(r) * (1.0f / 2147483648.0f);
}

/* ==========================================================================
   PART 2 — THE STRUCTURE
   ========================================================================== */

struct iris {
  int32_t n_in, n_hid, n_out, cap;

  /* --- the network -------------------------------------------------------
     One hidden layer. Wekinator uses exactly this shape, and there is a good
     reason beyond tradition: with ten or twenty training examples, a deeper
     network has far more capacity than data and simply memorises noise. One
     layer with a modest number of units is the right size for the amount of
     information a musician actually gives it.

     Weights are stored flat and row-major — w1[h*n_in + i] — so that the
     inner loop walks straight through memory. Pointer-chasing through a
     "layer object holding node objects" is the single most common way small
     neural network code ends up slow on a microcontroller. */
  float *w1, *b1;   /* input  -> hidden */
  float *w2, *b2;   /* hidden -> output */
  float *v_w1, *v_b1, *v_w2, *v_b2;   /* momentum ("velocity") */
  float *hid, *out;                   /* activations, reused every pass */
  float *d_hid, *d_out;               /* error signals during learning   */

  /* --- normalisation -----------------------------------------------------
     Sensor units are wildly different sizes. A distance sensor reads 0–1300
     millimetres; an accelerometer reads -2 to +2 g. Feed those in raw and the
     network spends all its effort on the big number and effectively ignores
     the small one. So we record the range of everything we've seen and
     rescale it before it ever touches a weight. */
  float *in_lo, *in_hi, *out_lo, *out_hi;

  /* --- the examples ------------------------------------------------------
     This is the part Wekinator got right and the embedded systems that came
     after it got wrong. The training examples are not scratch data thrown
     away after training. They ARE the instrument. You must be able to look
     at them, hear them, and delete the bad one. */
  float   *ex;      /* cap * (n_in + n_out), interleaved */
  int32_t *ex_id;   /* stable id per example, so "delete #3" always means #3 */
  int32_t *order;   /* shuffle buffer, reused each epoch */
  int32_t  n_ex, next_id;

  /* --- which demonstration is fighting the others (PART 8f) --------------
     One float per example slot: the example's squared error SUMMED OVER
     EVERY EPOCH of the last training session. Not the final residual — see
     the measurements in PART 8f for why the final residual is worthless once
     you train to convergence. */
  float   *ex_res;
  int32_t  res_epochs;   /* how many epochs are summed into ex_res */

  /* --- training settings ------------------------------------------------- */
  float   lr, momentum;
  float   l2;              /* weight decay. 0 = off, and off is the default */
  uint32_t seed;
  iris_rng  rng;
  int32_t trained;         /* the fit reflects the CURRENT example set      */
  int32_t fitted;          /* this instrument has EVER produced a fit.
                              iris_record/iris_delete clear `trained` (the fit is
                              stale) but must NOT clear this (the instrument
                              still plays). iris_predict guards on this one.   */
  float   last_error;
  int32_t status;          /* iris_status of the last train/predict */

  /* --- training progress, so a progress bar can be honest ----------------
     Written by every trainer entry point. tr_ceiling is the budget the
     caller asked for; tr_done is how much of it has been spent. A converged
     run stops early, so tr_done/tr_ceiling is a LOWER bound on completion —
     iris_train_progress reports it as such and snaps to 1.0 when the run
     ends, which is the only way a plateau-stopped bar can be truthful. */
  int32_t tr_done, tr_ceiling, tr_running;
  int32_t tr_n_ex;         /* how many demonstrations the shuffle covers */
  float   tr_ref;          /* error one plateau-window ago */
};

/* WHAT THIS DOES AND DOES NOT COVER.

   It reports NUMERICAL HEALTH ONLY: a poisoned value trapped at the door or
   before an output, a diverged or stuck run, a ridge escalation, a prediction
   from an instrument that was never fitted, a closed-form solve that collapsed
   to a constant. Those are the conditions a caller cannot detect for itself.

   It does NOT report an argument mistake -- asking for demonstration 5,000 of
   twelve, say. Those come back through the RETURN VALUE and leave the status
   alone, deliberately, so that one out-of-range query cannot leave a polled
   user interface showing a fault for ever. So: check the return value of the
   call you made, and check this for whether the instrument itself is in
   trouble. Two questions, two answers. (CHANGELOG.md, 0.1.0.) */

/* THREADING, IN ONE SENTENCE.

       Never touch the same instrument from two places at once.

   That is the whole contract, and it is short because there is no mutable
   state anywhere outside the instrument you passed in -- no globals, no static
   buffers, no shared scratch -- so two instruments cannot interact on any
   number of cores.

   SAFE: many instruments on one core, one after another; one instrument per
   thread across as many cores as you have; one instrument used only inside an
   interrupt -- but read the PLATFORM CAVEAT below before you do that last one.

   NOT SAFE: the SAME instrument from an interrupt and the main loop.
   iris_predict writes its working values inside the instrument, so an
   interrupt landing mid-call leaves both answers wrong. Give the interrupt its
   own instrument. The full table and the cross-talk verification are in
   README.md under "Threading".

   PLATFORM CAVEAT, and it decides the interrupt case on the board this library
   is usually run on. Everything above is a statement about THIS CODE: iris
   keeps no global or static state, so separate instruments cannot interfere.
   It is not a promise about your chip. On an ESP32 under FreeRTOS the
   floating-point registers are not saved when an interrupt is taken, so any
   float arithmetic inside an interrupt handler -- iris or anyone else's --
   can corrupt the interrupted task's registers, silently. iris is float
   throughout. So on that platform, do not call any iris_ function from an
   interrupt handler: read the sensor there, set a flag, and call iris from the
   main loop. The C-level statement above stands wherever interrupt entry does
   save the floating-point registers.

   TIMING, for the audio case. One prediction is 14.9 microseconds on an
   ESP32-S3 against a 20.8 microsecond audio sample at 48 kHz -- 1.4x of
   margin, enough to run per-sample and not enough to also do anything
   expensive in the same callback. That figure is measured on the part, not
   scaled: device_torture.ino test 9, two boards. This line said 7.4-7.8 until
   2026-08-30, which was a host measurement multiplied by an estimated 270 and
   printed as if taken on the part; the provenance is in README.md and
   docs/SYSTEM-technical.md. TRAINING does not fit and is not close: 595 ms at
   4 demonstrations, 2.7-3.0 s at 8 to 20. Train in slices from the main loop
   -- see iris_train_slice -- and never from an interrupt. */

/* HOW EVERY FUNCTION IN THIS FILE REPORTS FAILURE — two rules, and only two.

   BEFORE THE RULES, THE ONE LINE THAT ANSWERS "DID IT TRAIN?"

       trainer_of_your_choice(k);
       if (!iris_is_trained(k)) { ...it did not fit... }

   Use that, and stop reading here if that is all you need. It is correct after
   EVERY trainer in this file, and no other test is.

   Here is why it has to exist. The two rules below are each individually sound,
   but they meet badly in C, and `if (trainer(...))` is wrong in BOTH directions
   depending on which trainer you called. Measured, all six on the same data:

     on a fit that WORKED        return   if(return)   iris_is_trained
       iris_train                 1.0000    true            1
       iris_train_epochs          0.0002    true            1
       iris_train_converge        0.0000    true            1
       iris_train_elm             0.0000    FALSE           1   <-- best case
       iris_correct               0.0004    true            1
       iris_retrain_new           0.0003    true            1

     on a fit that REFUSED
       iris_train                 0.0000    false           0
       iris_train_epochs         -1.0000    TRUE            0   <-- -1 is truthy
       iris_train_converge       -1.0000    TRUE            0
       iris_train_elm            -1.0000    TRUE            0
       iris_correct              -1.0000    TRUE            0
       iris_retrain_new          -1.0000    TRUE            0

   iris_train_elm returns the number of ridge escalations, so 0 is its BEST
   outcome and reads as false. The rest return a measurement, and -1 is a
   perfectly ordinary non-zero float, so a refusal reads as true. Neither is a
   bug in the rules; it is what happens when "did it work" is asked of a number
   that was never meant to answer it.

   iris_is_trained reads one flag that every trainer sets on success and no
   trainer sets on refusal. It is the same answer whichever door you came in.

   RULE 1, for a call that either works or does not:
       0 means the call did nothing. Non-zero means it worked.
   That covers iris_record, iris_train, the delete functions, iris_load,
   iris_save, iris_size and iris_train_begin. Nothing to look up: zero is bad.
   iris_record returns the new demonstration's identifier on success, which is
   naturally non-zero because identifiers start at 1 -- so it obeys the rule
   AND hands you the number you need later to delete or re-map that specific
   take.

   RULE 2, for a call that returns a MEASUREMENT you asked for:
       the measurement on success, -1 on refusal.
   That covers the detailed trainers (which return the training error),
   iris_loo_error, iris_suggest_smoothing, iris_worst_example, iris_index_of
   and iris_classify_1nn. These cannot use rule 1 because zero is often a
   perfectly good answer -- iris_train_elm returns the number of ridge
   escalations, and none needed is the best possible outcome.

   And a separate question, with a separate answer: is the INSTRUMENT in
   trouble? That is iris_get_status, below. A call can succeed on an
   instrument that is unwell, and a call can fail on a perfectly good one.
   Two questions, two answers -- and zero means the OPPOSITE thing in each. A
   return value of 0 says the call did nothing; a status of 0 (IRIS_STATUS_OK)
   says nothing is wrong. See the note in PART 1, which says the same thing.

   ---------------------------------------------------------------------- */

IRIS_API iris_status iris_get_status(const iris *k) {
  /* A null instrument is not healthy. The header says "if (iris_get_status(k))
     reads as 'is something wrong?'", and for the one input where something is
     definitely wrong it used to answer no. */
  if (!k) return IRIS_NOT_FITTED;
  return (iris_status)k->status;
}

/* ==========================================================================
   PART 3 — SETUP
   ========================================================================== */

/* Bytes an instrument of this shape needs. Returns 0 for a shape that cannot
   be sized safely (any dimension out of range, or cap above IRIS_MAX_EX, which
   would overflow size_t on a 32-bit target such as the ESP32-S3).

   READ THIS BEFORE USING THE RETURN VALUE. 0 is a SENTINEL and it does not
   protect you on its own: size_t is unsigned, so `bytes < iris_size(...)` is
   FALSE when iris_size returns 0, and a caller using that idiom alone would
   sail past a bad shape rather than stop at it. iris_init is safe because it
   validates every dimension INCLUDING cap before it ever calls iris_size
   (see the guard block at the top of iris_init). Any other caller must test
   for 0 explicitly. */
/* How many bytes a shape occupies. Arithmetic only, no opinion about whether
   the shape is a good idea — iris_size below adds that. They are separate
   because conflating them cost us a memory-safety bug: iris_size returned 0
   for widths under its quality floor, iris_init compared `bytes < 0` on
   unsigned types, and the arena bound silently ceased to exist. A size
   function that can refuse is not a size function. */
static size_t iris_internal_bytes(int n_in, int n_hid, int n_out, int cap) {
  /* Wide arithmetic, then a range check -- see the note on IRIS_ARENA. This is
     the runtime twin of that macro and it has to agree with it, including about
     shapes that do not fit. Returning 0 for "cannot be sized on this machine"
     is what iris_size already promises its callers; before this it returned a
     small wrong number instead, and every bound built on it was inert. */
  /* THE MACRO IS THE DEFINITION; THIS CALLS IT RATHER THAN RESTATING IT.
     These were two hand-kept copies of the same arithmetic that had to agree
     or iris_init's bound would compare a need against an unrelated number.
     Now they agree by construction, and the wide-arithmetic note above the
     macro covers both. */
  unsigned long total = IRIS_ARENA(n_in, n_hid, n_out, cap);
  if (total > (unsigned long)(size_t)-1) return 0;   /* will not fit a pointer */
  return (size_t)total;
}

IRIS_API size_t iris_size(int n_in, int n_hid, int n_out, int cap) {
  if (n_in < 1 || n_in > IRIS_MAX_IN)   return 0;
  if (n_out < 1 || n_out > IRIS_MAX_OUT) return 0;
  /* Floor of 8, not 1. n_hid is written into the file header and iris_load
     refuses a mismatch, so the width chosen on day one is that instrument's
     width forever. Below 8 the network cannot represent the mappings this
     library is for; 8-64 is flat on quality (grid 0.0255-0.0290), so the floor
     costs nothing and prevents a permanent mistake. */
  if (n_hid < 8 || n_hid > IRIS_MAX_HID) return 0;
  if (cap  < 1 || cap  > IRIS_MAX_EX)   return 0;
  return iris_internal_bytes(n_in, n_hid, n_out, cap);
}

/* Randomise the weights. This is the reroll.

   The scale matters. Each hidden unit adds up n_in incoming signals, so if
   the weights are too large the sum lands far out where tanh is flat, the
   error signal underneath it goes to nearly zero, and the network stops
   learning before it starts. Dividing by the square root of the number of
   inputs keeps the sums in the responsive part of the curve. This is a
   standard trick and it is the difference between "trains in 50 ms" and
   "never trains at all". */
IRIS_API void iris_reseed(iris *k, uint32_t seed) { if (!k) return;
  k->seed = seed ? seed : 1u;
  k->rng.s = k->seed;
  const float s1 = 1.0f / iris_sqrt((float)(k->n_in  > 0 ? k->n_in  : 1));
  const float s2 = 1.0f / iris_sqrt((float)(k->n_hid > 0 ? k->n_hid : 1));
  for (int i = 0; i < k->n_hid * k->n_in;  ++i) k->w1[i] = iris_rand_sym(&k->rng) * s1;
  for (int i = 0; i < k->n_hid;            ++i) k->b1[i] = 0.0f;
  for (int i = 0; i < k->n_out * k->n_hid; ++i) k->w2[i] = iris_rand_sym(&k->rng) * s2;
  for (int i = 0; i < k->n_out;            ++i) k->b2[i] = 0.0f;
  for (int i = 0; i < k->n_hid * k->n_in;  ++i) k->v_w1[i] = 0.0f;
  for (int i = 0; i < k->n_hid;            ++i) k->v_b1[i] = 0.0f;
  for (int i = 0; i < k->n_out * k->n_hid; ++i) k->v_w2[i] = 0.0f;
  for (int i = 0; i < k->n_out;            ++i) k->v_b2[i] = 0.0f;
  k->trained = 0;
  k->fitted  = 0;          /* random weights are not a fit */
  k->last_error = 1.0f;
  k->status = IRIS_STATUS_OK;
}

IRIS_API iris *iris_init(void *mem, size_t bytes, int n_in, int n_hid, int n_out,
                   int cap, uint32_t seed) {
  if (!mem) return 0;
  if (n_in  < 1 || n_in  > IRIS_MAX_IN ) return 0;
  if (n_out < 1 || n_out > IRIS_MAX_OUT) return 0;
  /* Floor of 8, matching iris_size. These two used to disagree: iris_size
     returned its "impossible shape" answer of 0 for every width below 8 while
     iris_init happily built one, so `malloc(iris_size(2,4,3,64))` allocated
     nothing and the obvious next line wrote into it. docs/FREEZE.md has said
     "raise the floor to 8 in iris_init. NOW." since before this release; n_hid
     is written into the save file and iris_load refuses a mismatch, so the
     width chosen on day one is that instrument's width for ever, which is why
     a permanent mistake is worth refusing rather than accepting. */
  if (n_hid < 8 || n_hid > IRIS_MAX_HID) return 0;
  if (cap   < 1 || cap > IRIS_MAX_EX) return 0;      /* see IRIS_MAX_EX: overflow */
  /* The floors above now match iris_size exactly, so the two agree on which
     shapes exist. They did not always, and the way that failed is worth
     keeping: iris_size floored n_hid at 8 and returned 0 below it, iris_init
     floored it at 1, and the arena bound was written `bytes < iris_size(...)`.
     For n_hid in 1..7 that became `bytes < 0` on unsigned types -- false,
     always -- so the bound was not merely wrong, it was absent.
     iris_init(a 1-byte arena, n_hid = 4) returned a live instrument and
     training wrote 611 bytes past the end.

     The bound below therefore goes through iris_internal_bytes, which is arithmetic
     with no opinion, rather than through iris_size, which has one. A size
     function that can refuse cannot also be a bound. */
  { size_t need = iris_internal_bytes(n_in, n_hid, n_out, cap);
    /* need == 0 means the shape cannot be sized on this machine at all. Test it
       FIRST: size_t is unsigned, so `bytes < 0` is false for every arena and the
       bound would wave the impossible shape straight through -- the exact
       sentinel trap the note above iris_size warns about, which this line was
       previously walking into. */
    if (need == 0) return 0;
    if (bytes < need) return 0; }

  unsigned char *p = (unsigned char *)mem;
  /* Align BEFORE placing the structure, not after. The caller's arena is only
     guaranteed 1-byte aligned -- the front-page example declares it as
     `unsigned char mem[...]` -- and this used to align the float arrays while
     leaving the structure itself wherever the arena happened to start. A
     sanitizer reports it as a misaligned member access; a chip that faults on
     unaligned loads reports it as a crash. The 64 bytes of slack in
     iris_internal_bytes exist for exactly this. */
  p += ((uintptr_t)p & 7u) ? (8u - (size_t)((uintptr_t)p & 7u)) : 0u;
  iris *k = (iris *)p;  p += sizeof(iris);
  p += ((uintptr_t)p & 7u) ? (8u - (size_t)((uintptr_t)p & 7u)) : 0u;

  k->n_in = n_in; k->n_hid = n_hid; k->n_out = n_out; k->cap = cap;

  #define IRIS_TAKE(field, n) do { k->field = (float *)p; p += sizeof(float) * (size_t)(n); } while (0)
  IRIS_TAKE(w1,   n_hid * n_in);  IRIS_TAKE(b1,   n_hid);
  IRIS_TAKE(w2,   n_out * n_hid); IRIS_TAKE(b2,   n_out);
  IRIS_TAKE(v_w1, n_hid * n_in);  IRIS_TAKE(v_b1, n_hid);
  IRIS_TAKE(v_w2, n_out * n_hid); IRIS_TAKE(v_b2, n_out);
  IRIS_TAKE(hid,  n_hid);         IRIS_TAKE(out,  n_out);
  IRIS_TAKE(d_hid,n_hid);         IRIS_TAKE(d_out,n_out);
  IRIS_TAKE(in_lo, n_in);  IRIS_TAKE(in_hi, n_in);
  IRIS_TAKE(out_lo,n_out); IRIS_TAKE(out_hi,n_out);
  IRIS_TAKE(ex_res, (size_t)cap);
  IRIS_TAKE(ex, (size_t)cap * (n_in + n_out));
  #undef IRIS_TAKE

  k->ex_id = (int32_t *)p; p += sizeof(int32_t) * (size_t)cap;
  k->order = (int32_t *)p;
  /* FILL IT. This was a pointer into memory nobody had written, and the
     trainer's shuffle both reads and writes through it: recording a
     demonstration during a sliced run made the shuffle reach one slot past
     what iris_train_begin had filled -- a crash on a dirty arena.

     AND IT IS NOW BELT-AND-BRACES, which is worth writing down rather than
     leaving as a question. The trainer refills order[] at the start of every
     run and again whenever the example count changes under a running slice, so
     that path covers the case on its own. Verified: 300 trials of randomised
     mid-run records, deletes and slices on deliberately dirty arenas, under
     AddressSanitizer and UndefinedBehaviorSanitizer, produce the identical
     result hash 0x3920621C with this line and without it.

     It stays because it is one loop at construction and the failure it guards
     was real and measured. The mutation harness lists it as a survivor for
     exactly this reason: nothing can observe it, so nothing can test it. That
     is the honest state, not an oversight. */
  for (int i = 0; i < cap; ++i) k->order[i] = i;

  k->n_ex = 0; k->next_id = 1;
  k->lr = 0.10f; k->momentum = 0.85f; k->l2 = 0.0f;
  for (int i = 0; i < cap; ++i) k->ex_res[i] = 0.0f;
  k->res_epochs = 0;
  k->tr_done = 0; k->tr_ceiling = 0; k->tr_running = 0; k->tr_ref = 0.0f;
  for (int i = 0; i < n_in;  ++i) { k->in_lo[i]  = 0.0f; k->in_hi[i]  = 1.0f; }
  for (int i = 0; i < n_out; ++i) { k->out_lo[i] = 0.0f; k->out_hi[i] = 1.0f; }
  iris_reseed(k, seed);
  return k;
}

/* INTERNAL. Was public until 2026-08-27; removed from the public surface
   because it is measurably a footgun and buys nothing.

   MOMENTUM 0.99 — one nudge from the 0.85 default — DIVERGED 21 of 40 runs and
   BRICKED 20 of them at the default learning rate (structured target, 40 seeds,
   sigma=0.05). "Bricked" means the musician lowers it back, retrains, and gets
   IRIS_DIVERGED_STUCK forever; only a reroll recovers, and a reroll is a
   different instrument.

   LR 2.0, the old permitted maximum, destroyed 5 of 16: recall 73x worse than
   default, grid error 6.6x worse. Weka's own documented range for the same
   parameter is 0-1; we permitted double it.

   AND THE SAFE RANGES DO NOTHING. Momentum 0.00 to 0.95 is flat on instrument
   quality (grid 0.0257 to 0.0290) — it is a SPEED knob, and iris_train_converge
   already hides speed. lr's safe range is covered entirely by smoothing: tuning
   lr, tuning l2 and tuning the epoch ceiling land within 2-4% of each other,
   because they are three spellings of one axis.

   WORSE, THE ONLY READOUT A UI CAN SHOW POINTS BACKWARDS. At sigma=0.10:
   lr=0.001 gives training MSE 1.17e-2 and the BEST instrument (grid 0.0693);
   lr=0.050 gives training MSE 1.25e-3 and nearly the WORST (grid 0.1472). A
   student tuning by watching the error readout reliably picks the worst
   setting on offer.

   Retained internally for tests/audit.c's Weka-parity check, which sets
   Weka's own 0.3/0.2 pair. See docs/KNOB-AUDIT.md. */
IRIS_API void iris_internal_set_learning(iris *k, float lr, float momentum) { if (!k) return;
  /* REFUSE A NOT-A-NUMBER BEFORE CLAMPING IT. iris_clampf is a ternary on two
     comparisons, and every comparison with NaN is false, so a NaN falls
     straight through the clamp and into the instrument. docs/FREEZE.md named
     this mechanism and prescribed exactly this guard; it was applied to three
     places and not to the setters. */
  if (iris_isbad(lr) || iris_isbad(momentum)) { k->status = IRIS_NAN_TRAPPED; return; }
  k->lr = iris_clampf(lr, 0.0001f, 2.0f);   /* range kept for Weka parity */
  k->momentum = iris_clampf(momentum, 0.0f, 0.99f);
}

/* WEIGHT DECAY (L2). Off by default, and the default is the finding.

   THE PROBLEM IT ADDRESSES, stated as a reviewer states it: this network has
   ~75 parameters and you are fitting 3xN targets. At N=20 that is more
   parameters than training scalars, trained to a plateau with no capacity
   control. With noisy demonstrations — which is what a human produces — it
   fits the noise.

   MEASURED (32 paired seeds, identical data and identical initial weights,
   held-out RMSE on a fixed clean grid, docs/MATH-FIXES.md defect 2):

     structured target, N=20, no noise      alpha=1e-3  -19.7%   (better)
     structured target, N=20, sigma=0.05    alpha=1e-3  -40.3%   (better)
     smooth target,     N=20, sigma=0.05    best alpha  -65.2%   (better)
     smooth target,     N=20, NO noise      alpha=1e-2  +34.9%   (WORSE)

   Sign test to p = 4.7e-10, disjoint IQRs in the large cells, surviving
   Benjamini-Hochberg over 224 arm-by-cell tests. Also: plain plateau training
   produced 4 IRIS_TRAINING_DIVERGED events at sigma=0.10; alpha >= 1e-3
   produced zero, anywhere.

   WHY THE DEFAULT IS STILL ZERO. No single alpha is safe across every target
   and noise level — the same 1e-3 that wins by 40% on a structured noisy target
   costs 9.3% on a clean smooth one, and 1e-2 costs 34.9%. Shipping a default
   that is wrong half the time to fix a problem that appears the other half is
   not an improvement, it is a coin flip with our name on it. So: the mechanism
   ships, the default does not, and the numbers above tell you when to reach
   for it. If your demonstrations are noisy — recorded from a human, from a real
   sensor — start at 1e-3.

   CAVEAT THAT MUST TRAVEL WITH THE NUMBER. This is NOT directly comparable to
   scikit-learn's alpha, even though the scale looks familiar. iris_fit_ranges
   derives normalisation from the training data's own observed range, and noise
   inflates that range by 0.58x to 1.39x across the grid, so the effective
   penalty moves with N and with noise. Comparable only under matched
   preprocessing, which no sklearn user has.

   Applied as decoupled decay on the weights only, never the biases: penalising
   a bias just shifts the function for no capacity benefit. */
IRIS_API void iris_internal_set_l2(iris *k, float l2) { if (!k) return;
  /* See iris_internal_set_learning: a NaN passes straight through a clamp. */
  if (iris_isbad(l2)) { k->status = IRIS_NAN_TRAPPED; return; }
  k->l2 = iris_clampf(l2, 0.0f, 0.3f);   /* 0.3, not 1.0 — see smoothing */
}
IRIS_API float iris_get_l2(const iris *k) { if (!k) return 0.0f; return k->l2; }

/* SMOOTHING — the one quality knob, in the musician's own terms.

   0 = stick tightly to my demonstrations, whatever they say.
   1 = smooth confidently between them, forgiving my shaky takes.

   This is the ONLY knob in the library that changes how good the instrument is
   rather than how big or how fast it is, and it is the only one worth a
   musician's attention. It maps onto weight decay, but nobody should have to
   know that to use it.

   WHY IT EXISTS AT ALL, measured (12 tasks x 3 noise levels, held-out grid
   error against clean truth):

       noise      smoothing 0     tuned smoothing
       none          0.0693           0.0601
       light         0.1312           0.0755
       heavy         0.2161           0.0904

   At realistic take-to-take inconsistency it is worth about 2.4x. Nothing else
   in the library comes close, and — this is the part that justifies collapsing
   five other knobs into this one — tuning the learning rate, or the epoch
   ceiling, or the hidden width, lands within 2-4% of this. They were five
   spellings of one axis. This is the spelling that is safe: measured monotone
   across its whole range and ZERO divergences at any value, where momentum at
   its old maximum bricked half of all instruments.

   Default is 0 — stick to the demonstrations — because a musician who has not
   asked for smoothing should get exactly what they showed it. */
IRIS_API void iris_set_smoothing(iris *k, float amount) { if (!k) return;
  /* Guard here too: multiplying a NaN by 0.3 is still a NaN, so the inner
     guard would see it but this one gives the caller the earlier refusal. */
  if (iris_isbad(amount)) { k->status = IRIS_NAN_TRAPPED; return; }
  iris_internal_set_l2(k, iris_clampf(amount, 0.0f, 1.0f) * 0.3f);
}
IRIS_API float iris_get_smoothing(const iris *k) { if (!k) return 0.0f; return k->l2 / 0.3f; }

/* ==========================================================================
   PART 4 — THE EXAMPLE STORE

   Add, inspect, delete. Deleting one example is a five-line function and its
   absence is the single biggest usability failure in every embedded system
   that has attempted this. One mistimed button press should not cost you
   twenty minutes of work.
   ========================================================================== */

IRIS_API int iris_count(const iris *k) { if (!k) return 0; return k->n_ex; }
IRIS_API int iris_capacity(const iris *k) { if (!k) return 0; return k->cap; }

/* LENGTHS, same rule as iris_predict and just as unchecked.
   Reads exactly n_in floats from `in` and n_out from `out`. */
IRIS_API int iris_record(iris *k, const float *in, const float *out) { if (!k) return 0;
  if (k->n_ex >= k->cap) { k->status = IRIS_STORE_FULL; return 0; }

#ifndef IRIS_NO_GUARDS
  /* REFUSE A POISONED DEMONSTRATION AT THE DOOR. A NaN or Inf from a glitched
     or unplugged sensor used to be accepted here silently — valid id returned,
     status OK — and was caught three doors later by the trainer's pre-scan,
     which then refused to train at all. One bad frame therefore blocked every
     subsequent training run until the musician worked out which example to
     delete, with nothing telling them.

     Refusing here is strictly better: the store never holds a value that can
     poison a fit, the instrument keeps playing, and the caller finds out
     immediately. The trainer's pre-scan stays as defence in depth — it also
     covers examples that arrived through iris_load. Added 2026-08-27 (gap B4). */
  {
    for (int i = 0; i < k->n_in;  ++i)
      if (iris_isbad(in[i]))  { k->status = IRIS_NAN_TRAPPED; return 0; }
    for (int i = 0; i < k->n_out; ++i)
      if (iris_isbad(out[i])) { k->status = IRIS_NAN_TRAPPED; return 0; }
  }
#endif

  const int stride = k->n_in + k->n_out;
  float *row = k->ex + (size_t)k->n_ex * stride;
  for (int i = 0; i < k->n_in;  ++i) row[i] = in[i];
  for (int i = 0; i < k->n_out; ++i) row[k->n_in + i] = out[i];
  k->ex_id[k->n_ex] = k->next_id++;
  k->n_ex++;
  k->trained = 0;                                   /* model is now stale */

  /* This call just disproved the two complaints this call can raise, so clear
     them. The header tells you to read `if (iris_get_status(k))` as "is
     something wrong?", and without it one full store answered yes for the rest
     of the instrument's life.

     ONLY IRIS_STORE_FULL, and that is deliberate. An earlier version cleared
     IRIS_NAN_TRAPPED here too, which was over-broad: iris_record is one of
     five paths that raise it -- training, predicting and loading raise it as
     well -- and storing one good number does not disprove a not-a-number that
     TRAINING trapped. Only iris_record can raise IRIS_STORE_FULL, so only
     iris_record can retract it; that is the whole rule.
     A bad reading therefore does not alarm for ever either: iris_train clears
     the status on success, and every sketch here trains straight after
     recording, so the flag lifts at the point the instrument is actually
     known to be well again. */
  if (k->status == IRIS_STORE_FULL)
    k->status = IRIS_STATUS_OK;

  return k->ex_id[k->n_ex - 1];
}

IRIS_API int iris_index_of(const iris *k, int id) { if (!k) return -1;
  for (int i = 0; i < k->n_ex; ++i) if (k->ex_id[i] == id) return i;
  return -1;
}

/* The stable id at a position, without copying the row out. */
IRIS_API int iris_id_at(const iris *k, int idx) { if (!k) return -1;
  return (idx < 0 || idx >= k->n_ex) ? -1 : k->ex_id[idx];
}

/* LENGTHS, same rule as iris_predict and just as unchecked.
   Writes exactly n_in floats into `in` and n_out floats into `out`.
   Either may be null if you do not want that half. */
IRIS_API int iris_get(const iris *k, int idx, float *in, float *out) { if (!k) return 0;
  if (idx < 0 || idx >= k->n_ex) return 0;
  const int stride = k->n_in + k->n_out;
  const float *row = k->ex + (size_t)idx * stride;
  if (in)  for (int i = 0; i < k->n_in;  ++i) in[i]  = row[i];
  if (out) for (int i = 0; i < k->n_out; ++i) out[i] = row[k->n_in + i];
  return k->ex_id[idx];
}

IRIS_API int iris_delete_index(iris *k, int idx) { if (!k) return 0;
  if (idx < 0 || idx >= k->n_ex) return 0;
  const int stride = k->n_in + k->n_out;
  for (int r = idx; r < k->n_ex - 1; ++r) {
    float *dst = k->ex + (size_t)r * stride;
    const float *src = k->ex + (size_t)(r + 1) * stride;
    for (int c = 0; c < stride; ++c) dst[c] = src[c];
    k->ex_id[r]  = k->ex_id[r + 1];
    /* The residual ledger is indexed by POSITION, so it has to move with the
       rows. It did not, so after any delete every "which take is fighting the
       others" answer pointed at the wrong demonstration -- 200 times out of
       200, always naming an innocent one, until the next training run. The
       margin usually collapsed below the threshold the header tells a screen
       to require, so the accusation went quiet rather than wrong; but the same
       header invites a screen to show it dimly below that threshold, and that
       mark was on the wrong take every time. */
    k->ex_res[r] = k->ex_res[r + 1];
  }
  k->ex_res[k->n_ex - 1] = 0.0f;
  k->n_ex--;
  k->trained = 0;
  return 1;
}

IRIS_API int iris_delete_id(iris *k, int id) { if (!k) return 0; return iris_delete_index(k, iris_index_of(k, id)); }
IRIS_API int iris_delete_last(iris *k) { if (!k) return 0; return iris_delete_index(k, k->n_ex - 1); }

/* Delete whichever example is closest to where you are standing right now.
   On a device with three buttons this is how you say "not THAT one" without
   needing to read a list. */
/* LENGTHS, same rule as iris_predict and just as unchecked.
   Reads exactly n_in floats from `in`. */
IRIS_API int iris_delete_nearest(iris *k, const float *in) { if (!k) return 0;
  int best = -1; float best_d = 1e30f;
  const int stride = k->n_in + k->n_out;
  for (int r = 0; r < k->n_ex; ++r) {
    const float *row = k->ex + (size_t)r * stride;
    float d = 0.0f;
    for (int i = 0; i < k->n_in; ++i) { float t = row[i] - in[i]; d += t * t; }
    if (d < best_d) { best_d = d; best = r; }
  }
  return iris_delete_index(k, best);
}

IRIS_API void iris_clear(iris *k) {
  if (!k) return;
  k->n_ex = 0; k->trained = 0; k->fitted = 0;
  /* End any run in flight. Without this, iris_train_slice kept reporting
     "there is more to do" for ever: the trainer returns immediately when there
     are no demonstrations, so tr_done never advances and tr_running is never
     cleared, and the documented loop
         while (iris_train_slice(k, 500)) { draw(); poll(); }
     never terminates. Both Arduino sketches wire iris_clear to a button and
     the library recommends training in slices, so those two are one press
     apart. */
  k->tr_running = 0; k->tr_done = 0; k->tr_n_ex = 0; k->tr_ref = 0.0f;
}

/* ==========================================================================
   PART 5 — NORMALISATION

   Find the range of every input and output across the examples, then map
   everything into a common scale before training.

   Outputs go to 0.1–0.9 rather than 0–1 on purpose. The output layer uses a
   sigmoid. A true logistic only APPROACHES 0 and 1; this one is built on a
   clamped rational function and reaches them exactly — iris_sigmoid(6.0f) is
   1.0f on the nose. Asking it to hit exactly 1.0 means pushing a weight toward
   infinity forever; leaving headroom at both ends means the network can
   actually arrive.

   WHERE THIS CONSTANT ACTUALLY COMES FROM, stated honestly because this
   paragraph used to promise "the measurement below" and there is no
   measurement below -- the block ends here. 0.1/0.9 is a folklore rule of
   thumb, not a derived value. LeCun's Efficient BackProp section 4.5 derives
   the principled band from the maximum of the sigmoid's second derivative,
   which is 0.2113/0.7887, and docs/MATH-AUDIT.md:101 records that the shipped
   band therefore delivers 1.85x LESS gradient at the targets. It stays because
   moving it changes every frozen hash and every saved file's output mapping
   (docs/MATH-AUDIT.md:274 costs that out), not because it was measured to be
   better. It was not.
   ========================================================================== */

#define IRIS_OUT_LO 0.1f
#define IRIS_OUT_HI 0.9f

IRIS_API void iris_fit_ranges(iris *k) { if (!k) return;
  const int stride = k->n_in + k->n_out;
  if (k->n_ex == 0) return;
  for (int i = 0; i < k->n_in;  ++i) { k->in_lo[i]  =  1e30f; k->in_hi[i]  = -1e30f; }
  for (int i = 0; i < k->n_out; ++i) { k->out_lo[i] =  1e30f; k->out_hi[i] = -1e30f; }
  for (int r = 0; r < k->n_ex; ++r) {
    const float *row = k->ex + (size_t)r * stride;
    for (int i = 0; i < k->n_in; ++i) {
      if (row[i] < k->in_lo[i]) k->in_lo[i] = row[i];
      if (row[i] > k->in_hi[i]) k->in_hi[i] = row[i];
    }
    for (int i = 0; i < k->n_out; ++i) {
      float v = row[k->n_in + i];
      if (v < k->out_lo[i]) k->out_lo[i] = v;
      if (v > k->out_hi[i]) k->out_hi[i] = v;
    }
  }
  /* A dimension where every example is identical has zero range. Dividing by
     that is how you get NaN into an audio buffer. Give it a floor. */
  /* The floor has to be RELATIVE. Adding an absolute 1e-6 to a value above 32
     changes nothing at all in 32-bit floating point -- the gap between
     representable numbers there is already wider than 1e-6 -- so the range
     stayed exactly zero, the normalisation divided zero by zero, and every
     prediction became not-a-number, which the guards then replaced with the
     middle of the range. A light sensor reads 0..4095 and a distance sensor
     reads millimetres, so ANY of those channels sitting still killed the whole
     instrument silently. Measured: worked to 31.77, dead from 32.72. */
  for (int i = 0; i < k->n_in;  ++i) {
    float w = iris_absf(k->in_lo[i]) * 1e-5f;  if (w < 1e-6f) w = 1e-6f;
    if (k->in_hi[i]  - k->in_lo[i]  < w) k->in_hi[i]  = k->in_lo[i]  + w;
  }
  for (int i = 0; i < k->n_out; ++i) {
    float w = iris_absf(k->out_lo[i]) * 1e-5f; if (w < 1e-6f) w = 1e-6f;
    if (k->out_hi[i] - k->out_lo[i] < w) k->out_hi[i] = k->out_lo[i] + w;
  }
}

/* THE INPUT SCALING: each input maps to [-1,+1] across the range the
   demonstrations covered.

   Centred, not [0,1], because inputs that are all positive give every
   first-layer weight of a hidden unit a gradient of the same sign, so the
   descent has to zig-zag toward the answer (LeCun et al. 1998, "Efficient
   BackProp", section 4.3). [-1,+1] is also what Weka's MultilayerPerceptron
   does with normalizeAttributes on, the setting Wekinator ships. Measured on
   the 8-output reference task at 600 epochs, against the same network fed
   [0,1] inputs: training mean squared error 5.94e-4 -> 6.38e-5 (9.3x), grid
   root-mean-square error 0.0129 -> 0.0084 (1.54x). */
IRIS_API float iris_norm_in (const iris *k, int i, float v) { if (!k || i < 0 || i >= k->n_in) return 0.0f;
  const float t = (v - k->in_lo[i]) / (k->in_hi[i] - k->in_lo[i]);
  return 2.0f * t - 1.0f;
}

IRIS_API float iris_norm_out(const iris *k, int i, float v) { if (!k || i < 0 || i >= k->n_out) return 0.0f;
  float t = (v - k->out_lo[i]) / (k->out_hi[i] - k->out_lo[i]);
  return IRIS_OUT_LO + t * (IRIS_OUT_HI - IRIS_OUT_LO);
}
IRIS_API float iris_denorm_out(const iris *k, int i, float y) { if (!k || i < 0 || i >= k->n_out) return 0.0f;
  float t = (y - IRIS_OUT_LO) / (IRIS_OUT_HI - IRIS_OUT_LO);
  return k->out_lo[i] + t * (k->out_hi[i] - k->out_lo[i]);
}

/* ==========================================================================
   PART 6 — FORWARD PASS  (this is "playing the instrument")

     hidden_h = tanh( sum_i w1[h][i] * x_i + b1[h] )
     output_o = sigmoid( sum_h w2[o][h] * hidden_h + b2[o] )

   That is the network. It is NOT the whole of what iris_predict does, and this
   line used to say it was -- someone following it got a wrong number. The full
   chain, which is what plays:

     x_i     = iris_norm_in(k, i, your_reading)     scale the sensor in
     ...the two lines above...
     out_o   = iris_denorm_out(k, o, output_o)      scale the sound out
     out_o   = iris_clampf(out_o, out_lo[o], out_hi[o])   and hold it in range

   Four steps, two of them arithmetic on ranges the instrument measured for
   itself. Two matrix multiplies with a squashing function
   after each one. For 2 inputs, 12 hidden and 3 outputs that is 60
   multiply-adds — about one microsecond on the S3. Playing is free; only
   learning costs anything.
   ========================================================================== */

IRIS_API void iris_forward_norm(const iris *k, const float *x_norm) { if (!k) return;
  for (int h = 0; h < k->n_hid; ++h) {
    const float *w = k->w1 + (size_t)h * k->n_in;
    float s = k->b1[h];
    for (int i = 0; i < k->n_in; ++i) s += w[i] * x_norm[i];
    k->hid[h] = iris_tanh(s);
  }
  for (int o = 0; o < k->n_out; ++o) {
    const float *w = k->w2 + (size_t)o * k->n_hid;
    float s = k->b2[o];
    for (int h = 0; h < k->n_hid; ++h) s += w[h] * k->hid[h];
    k->out[o] = iris_sigmoid(s);
  }
}

/* DOES THIS INSTRUMENT FIT THIS TRANSLATION UNIT'S WORKING ARRAYS?

   Nine functions below declare float x[IRIS_MAX_IN] and friends. Those maxima
   are #ifndef so a small board can shrink them (see the note above them), and
   that is a per-TRANSLATION-UNIT setting: define IRIS_MAX_IN 4 in one .c file
   and not in another, and the two files disagree about how big those arrays
   are while sharing one instrument through a pointer.

   iris_init checks the shape against the maxima -- but it checks them in the
   translation unit that CALLS iris_init, which is the one with the large
   maxima, so it passes. The unit with the small maxima then writes n_in floats
   into its own float x[4]. Reproduced under AddressSanitizer:
   "stack-buffer-overflow, WRITE of size 4, [32,48) 'x.i'".

   So every function that declares one of those arrays asks this first. It is
   two comparisons and it turns a memory overwrite into an ordinary refusal. */
IRIS_API int iris_shape_fits(const iris *k) {
  return k && k->n_in <= IRIS_MAX_IN && k->n_out <= IRIS_MAX_OUT
           && k->n_hid <= IRIS_MAX_HID;
}

IRIS_API void iris_predict(const iris *k, const float *in, float *out) { if (!k) return;
  if (!iris_shape_fits(k)) {
    /* Write a safe value rather than returning silently: `out` holds whatever
       the caller last played, and leaving it there is stale audio, which is the
       failure this library refuses everywhere else. Same substitute the
       unfitted path uses -- the centre of the demonstrated range. */
    for (int o = 0; o < k->n_out; ++o)
      out[o] = (k->n_ex > 0) ? 0.5f * (k->out_lo[o] + k->out_hi[o]) : 0.0f;
    ((iris *)k)->status = IRIS_NOT_FITTED;
    return;
  }
  float x[IRIS_MAX_IN];

#ifndef IRIS_NO_GUARDS
  /* PLAYING AN INSTRUMENT THAT WAS NEVER FITTED. Without this, the forward
     pass runs over the random weights iris_reseed drew and returns
     plausible-looking numbers with NO SYMPTOM anywhere: no status, no return
     code, no silence. The robustness audit ranked it the highest on-stage
     risk in the library precisely because nothing reports it.

     IT GUARDS ON `fitted`, NOT ON `trained`, AND THE DIFFERENCE MATTERS.
     iris_record and iris_delete clear `trained` — the fit no longer reflects the
     current example set — but the instrument is still a real instrument and
     must keep playing. Guarding on `trained` breaks that, which audit check 13
     exists to protect, and an attempt to do so on 2026-08-26 failed exactly
     there. `fitted` says "this has EVER produced a fit" and is cleared only by
     iris_reseed and iris_clear.

     Remedy is the NaN guard's: the centre of the demonstrated range, or 0 when
     there are no demonstrations to have a range from. Silence beats noise. */
  if (!k->fitted) {
    for (int o = 0; o < k->n_out; ++o)
      out[o] = (k->n_ex > 0) ? 0.5f * (k->out_lo[o] + k->out_hi[o]) : 0.0f;
    ((iris *)k)->status = IRIS_NOT_FITTED;
    return;
  }
#endif


  for (int i = 0; i < k->n_in; ++i) x[i] = iris_norm_in(k, i, in[i]);
  iris_forward_norm(k, x);
  for (int o = 0; o < k->n_out; ++o) {
    float v = iris_denorm_out(k, o, k->out[o]);
    out[o] = iris_clampf(v, k->out_lo[o], k->out_hi[o]);
#ifndef IRIS_NO_GUARDS
    /* Last line of defence. iris_clampf passes NaN straight through (every
       comparison with NaN is false), so a NaN here — glitched sensor in,
       poisoned weight — would land in an audio parameter. Substitute the
       centre of the demonstrated range and say so. On a healthy run the
       bit test fails and this changes nothing.                            */
    if (iris_isbad(out[o])) {
      out[o] = 0.5f * (k->out_lo[o] + k->out_hi[o]);
      ((iris *)k)->status = IRIS_NAN_TRAPPED;   /* reporting beats const purity */
    }
#endif
  }
}

/* ==========================================================================
   PART 7 — HOW LOST AM I?

   Distance from the current gesture to the nearest thing you demonstrated.
   0 means "exactly on an example".

   WHAT 1 MEANS, precisely, because the obvious reading is wrong. The scale is
   sqrt(n_in)/2 -- a constant that depends only on how many sensors you have,
   NOT on how far apart your demonstrations are. So 1 means "half the diagonal
   of the normalised input box away from the nearest example", and that is a
   fixed distance, not a relative one.

   The consequence is worth knowing before you map this to anything. With four
   corner demonstrations -- which is examples/00_minimal.c -- 60.3% of the
   gesture square reads exactly 1.0, including the middle of the demonstrated
   space; it reports the same value for "between your four takes" and "ten
   times outside them". With twenty-five demonstrations it never exceeds 0.48.
   The usable range of the control therefore depends on how many takes you
   recorded, and two instruments are not comparable.

   Making the scale relative to the examples' own spacing would fix that and is
   what this comment used to promise. It is deliberately NOT done here: the
   audit uses novelty to sort probes into near and far bands, so changing the
   scale moves measured thresholds elsewhere, and that deserves its own
   measurement rather than a quiet edit.

   This costs one pass over the examples — nothing. But it lets the instrument
   know when it is improvising rather than recalling, which you can map to
   anything you like: noise, detuning, a light. As far as I can find, nobody
   has done this, and it is four lines.
   ========================================================================== */

IRIS_API float iris_novelty(const iris *k, const float *in) { if (!k) return 0.0f;
  if (!iris_shape_fits(k)) return 0.0f;
  if (k->n_ex == 0) return 1.0f;
  const int stride = k->n_in + k->n_out;
  float best = 1e30f;
  for (int r = 0; r < k->n_ex; ++r) {
    const float *row = k->ex + (size_t)r * stride;
    float d = 0.0f;
    for (int i = 0; i < k->n_in; ++i) {
      float t = iris_norm_in(k, i, row[i]) - iris_norm_in(k, i, in[i]);
      d += t * t;
    }
    if (d < best) best = d;
  }
  float scale = iris_sqrt((float)k->n_in) * 0.5f;
  return iris_clampf(iris_sqrt(best) / (scale > 0.0f ? scale : 1.0f), 0.0f, 1.0f);
}

/* ==========================================================================
   PART 8 — TRAINING  (backpropagation)

   The only genuinely new idea in this file, and it is one idea:

     Run an example forward. Compare what came out to what you demonstrated.
     Nudge every weight a little in whichever direction would have reduced
     that gap. Repeat.

   "Backpropagation" is just bookkeeping for the middle layer: the hidden
   units don't have a target of their own, so you work out how much each one
   contributed to the final error and blame it proportionally.

   Two details that matter in practice:

   MOMENTUM. Instead of stepping purely downhill each time, keep a running
   velocity. Steps in a consistent direction accumulate; steps that jitter
   back and forth cancel. It makes training roughly three times faster and
   costs one extra array.

   SHUFFLING. Present the examples in a different order every epoch. Fixed
   order lets the network learn the order instead of the mapping — the last
   example seen always gets the final say.
   ========================================================================== */

/* Guard sweep, run once per epoch: NaN/Inf in any weight (or in the epoch
   error) means the numbers are gone — report and recover to a finite state.
   |w| past IRIS_W_LIMIT means divergence in progress — clamp, report, stop.
   Cost is one pass over the weights per EPOCH; the backprop pass over the
   weights runs once per EXAMPLE, so this is < 1/n_ex relative overhead.     */
#ifndef IRIS_NO_GUARDS
IRIS_API int iris_internal_check_weights(iris *k) { if (!k) return 0;
  const int nw = k->n_hid * k->n_in + k->n_hid + k->n_out * k->n_hid + k->n_out;
  /* w1,b1,w2,b2 are carved consecutively from the arena; walk them as one */
  float *w = k->w1;
  int worst = IRIS_STATUS_OK;
  for (int i = 0; i < nw; ++i) {
    if (iris_isbad(w[i])) return IRIS_NAN_TRAPPED;
    if (w[i] >  IRIS_W_LIMIT) { w[i] =  IRIS_W_LIMIT; worst = IRIS_TRAINING_DIVERGED; }
    if (w[i] < -IRIS_W_LIMIT) { w[i] = -IRIS_W_LIMIT; worst = IRIS_TRAINING_DIVERGED; }
  }
  return worst;
}
#endif

/* --------------------------------------------------------------------------
   TRAINING TO CONVERGENCE, AND SAYING SO OUT LOUD

   The masthead used to recommend 600 epochs. Measured, that budget stops the
   optimiser less than a fifth of the way down: going to 200,000 improves
   recall 5.9x and held-out error 1.8x for nothing but time, and time is the
   cheap thing here. The full epoch table is docs/adr/0017-train-to-the-plateau-not-to-a-constant.md.

   So the budget is no longer a number the caller guesses. iris_train_converge
   runs until the training error PLATEAUS: every IRIS_CONV_WINDOW epochs it
   compares the error against the error one window ago and stops when the
   window bought less than IRIS_CONV_TOL of it. Window and tolerance are
   measured, not guessed -- a short window (200-500 epochs) mistakes the
   ordinary epoch-to-epoch noise of a shuffled SGD trace for a plateau and
   stops at a quarter of the achievable fit.

   THE ONE PLACE THIS IS NOT FREE. At 50 examples a fixed 200,000-epoch budget
   is measurably WORSE on held-out error than 60,000 -- the point where more
   convergence starts costing generalisation. A plateau criterion stops before
   that on its own; a bigger constant would not have. That is the argument for
   a criterion over a constant.

   AND THE CAVEAT THAT GOVERNS THE WHOLE TABLE. The truth function those
   numbers come from is smooth and noiseless. "More convergence never hurts"
   is exactly the conclusion most at risk from real sensor noise and human
   inconsistency, and none of it is verified on hardware or on recorded human
   gesture. Treat the ceiling as a ceiling.

   HONEST PROGRESS. A converged run at 50 examples is seconds on the S3, long
   enough that the glass must show something true. Two ways in, both free:

     - iris_train_converge(k, ceiling, cb, user) calls cb every window with
       (done, ceiling, err); returning 0 from cb aborts, leaving a usable
       partially-trained instrument.
     - iris_train_begin / iris_train_slice / iris_train_progress run the SAME
       training in slices, so a single-threaded UI can draw a frame, read
       touch and keep the audio half alive between them. A sliced run is
       bit-identical to the equivalent unsliced one: the shuffle buffer is
       initialised once at iris_train_begin and carried across slices, so the
       rng draws are the same draws in the same order.

   iris_train_epochs IS UNCHANGED AND STAYS UNCHANGED. It is the Wekinator
   fidelity path -- fixed-epoch backprop is what Weka's MultilayerPerceptron
   does -- and audit check 12 pins its output to the bit.
   -------------------------------------------------------------------------- */

#define IRIS_CONV_WINDOW  2000    /* epochs between plateau tests (measured)   */
#define IRIS_CONV_TOL     0.10f   /* stop when a window buys < 10% of the error */
/* THE CEILING HAS TO FIT THE MACHINE'S int, BECAUSE IT IS PASSED AS ONE.

   iris_train_converge does `const int ceil_ = ceiling > 0 ? ceiling : ...` and
   hands that to iris_internal_train_run(int epochs). Where int is 16 bits --
   every Arduino AVR board -- 60000 truncates to -5536, the trainer's
   `epochs <= 0` guard correctly refuses, iris_train correctly returns 0, and
   the instrument is never fitted. The library was honest about it; every sketch
   that ignored the return value was not. Confirmed with avr-gcc for atmega328p:
   (int)60000 == -5536.

   30000 is the largest round number that fits a signed 16-bit int. It is not a
   compromise in practice: an 8-bit AVR at 16 MHz does not reach 30,000 epochs
   in a time anyone will wait for, so the ceiling is not the binding constraint
   there -- being positive is. */
#define IRIS_CONV_CEILING ((int)(sizeof(int) >= 4 ? 60000 : 30000))

/* Called every IRIS_CONV_WINDOW epochs. Return 0 to abort the run. */
typedef int (*iris_progress_fn)(void *user, int done, int ceiling, float err);

/* The one epoch engine. Every backprop entry point below is this function
   with a different stopping policy; there is no second copy of the update
   rule to drift out of sync.
     conv    : 0 = run the full budget, 1 = stop on the plateau test
     resume  : 0 = start a session (init shuffle, clear the residual ledger)
               1 = continue the session already in k
   Returns the last epoch's mean squared error. */
/* REFUSAL CONVENTION (one convention, whole library): a train call that did
   no training returns -1.0f and leaves `trained` alone, so a caller reading
   only the return value can tell a refusal from a repeat of the previous
   run. */
IRIS_API float iris_internal_train_run(iris *k, int epochs, int conv, int resume,
                           iris_progress_fn cb, void *user) { if (!k) return -1.0f;
  if (!iris_shape_fits(k)) { k->status = IRIS_NOT_FITTED; return -1.0f; }
  if (k->n_ex == 0) {
    /* A run with nothing left to train on is over, however it got that way.
       iris_clear was taught to end a run; the four delete functions were not,
       and they reach the same state. Ending it HERE covers every caller,
       present and future, instead of every caller having to remember. Without
       it the documented slice loop spins for ever with the progress bar
       frozen -- five million iterations and counting, measured. */
    k->tr_running = 0; k->tr_n_ex = 0;
    return -1.0f;
  }

#ifndef IRIS_NO_GUARDS
  /* THE DIVERGENCE TRAP, AND WHY THIS REFUSAL EXISTS.
     When a run diverges, iris_internal_check_weights clamps the offending weights to
     +/-IRIS_W_LIMIT and stops. On the NEXT fresh run those weights are still
     sitting exactly at the clamp: epoch 1 pushes one of them past, the guard
     fires again, and training stops after a single epoch. Forever.

     MEASURED 2026-08-27: 14 good demonstrations plus one contradictory take
     diverges; ONE weight of 60 ends up pinned. After deleting the bad example,
     iris_train_converge ran exactly 1 epoch and returned OK-looking on every
     subsequent call, leaving the instrument frozen at its damaged output.
     Zeroing the momentum does not help — it is the pinned weight, not the
     velocity. The musician deletes the bad take, retrains, and nothing happens,
     with no message.

     The examples are fine; the WEIGHTS are destroyed. Refitting from a fresh
     random start recovers the instrument (verified: 0.621 against the 0.618 it
     produced before the damage). So this refuses, loudly and distinguishably,
     rather than pretending to train. Recovery is iris_retrain_new(). We do NOT
     reseed automatically: that would silently hand the performer a different
     instrument, which is the failure mode Fiebrink & Sonami describe. */
  /* Not `!resume`. iris_train_slice enters with resume = 1, so a diverged
     instrument that iris_train refuses to touch used to be trained anyway if
     you drove it in slices -- and the header calls those two paths
     bit-identical. They must refuse identically too. */
  if (k->status == IRIS_TRAINING_DIVERGED) {
    int pinned = 0, i;
    const float lim = IRIS_W_LIMIT - 0.01f;
    /* All FOUR arrays, not two. iris_internal_check_weights clamps the biases as well
       as the weights and walks them as one block; this check scanned only w1
       and w2, so a clamp that landed on a bias left the instrument stuck with
       nothing noticing -- exactly the silent state the note above says this
       exists to prevent. Unreachable at the shipped defaults (0 of 400), but
       reachable through iris_internal_set_learning at its permitted maximum, where it
       happened 60 times out of 60. */
    for (i = 0; i < k->n_hid * k->n_in;  ++i)
      if (k->w1[i] >= lim || k->w1[i] <= -lim) pinned = 1;
    for (i = 0; i < k->n_out * k->n_hid; ++i)
      if (k->w2[i] >= lim || k->w2[i] <= -lim) pinned = 1;
    for (i = 0; i < k->n_hid;  ++i)
      if (k->b1[i] >= lim || k->b1[i] <= -lim) pinned = 1;
    for (i = 0; i < k->n_out;  ++i)
      if (k->b2[i] >= lim || k->b2[i] <= -lim) pinned = 1;
    if (pinned) { k->status = IRIS_DIVERGED_STUCK; k->tr_running = 0; return -1.0f; }
  }
#endif

  /* epochs <= 0 is not "train instantly", it is "do nothing". Without this,
     the loop below never runs, err stays 0.0f, and the tail unconditionally
     sets trained = 1 with last_error = 0.0 — reporting a freshly randomised
     network as trained with a perfect fit. Two live callers pass an
     unvalidated integer straight through (ports/wasm/wasm_shim.c and
     benchmark/adapters/iris_adapter.c). Fixed 2026-08-26. */
  if (epochs <= 0 && !resume) { k->tr_running = 0; return -1.0f; }

#ifndef IRIS_NO_GUARDS
  /* A NaN/Inf in a recorded example would poison every weight in the first
     epoch. Refuse up front: the previous instrument keeps playing, the bad
     example is still in the store where the musician can find and delete it.
     The scan runs BEFORE iris_fit_ranges for the same reason — a refused train
     must leave the playing instrument bit-identical, and ranges are part of
     the instrument (denormalisation reads them on every predict). The ELM
     trainer scans first for the same reason. */
  {
    const int st = k->n_in + k->n_out;
    for (int i = 0; i < k->n_ex * st; ++i)
      if (iris_isbad(k->ex[i])) { k->status = IRIS_NAN_TRAPPED; k->tr_running = 0;
                                return -1.0f; }   /* refusal convention */
  }
  k->status = IRIS_STATUS_OK;
#endif
  iris_fit_ranges(k);

  const int stride = k->n_in + k->n_out;
  const int NI = k->n_in, NH = k->n_hid, NOUT = k->n_out;
  float x[IRIS_MAX_IN], t[IRIS_MAX_OUT];
  float err = 0.0f;

  if (!resume) {
    for (int i = 0; i < k->n_ex; ++i) k->order[i] = i;
    k->tr_n_ex = k->n_ex;
    for (int i = 0; i < k->cap;  ++i) k->ex_res[i] = 0.0f;
    k->res_epochs = 0;
    k->tr_done = 0;
    k->tr_ref = 0.0f;
  } else if (k->tr_n_ex != k->n_ex) {
    /* The data changed under a running slice -- a record or a delete between
       two calls. The shuffle covers a fixed count, so the permutation no
       longer describes the data: rebuild it, or the new demonstration is never
       visited and a deleted one still is.

       Also restart the plateau window. The stopping test asks whether the
       error fell since last window, and new data makes the error JUMP UP, so
       a stale reference reads that rise as a plateau and ends the run on the
       exact epoch the student added something.

       Deliberately NOT reset: tr_done. The epoch budget stays monotonic, so a
       caller who records between every slice still reaches the ceiling instead
       of training for ever. */
    for (int i = 0; i < k->n_ex; ++i) k->order[i] = i;
    k->tr_n_ex = k->n_ex;
    k->tr_ref  = 0.0f;
  }

  for (int ep = 0; ep < epochs; ++ep) {
    /* Fisher-Yates shuffle */
    for (int i = k->n_ex - 1; i > 0; --i) {
      int j = (int)(iris_rand_u32(&k->rng) % (uint32_t)(i + 1));
      int tmp = k->order[i]; k->order[i] = k->order[j]; k->order[j] = tmp;
    }

    err = 0.0f;
    for (int s = 0; s < k->n_ex; ++s) {
      const int row_ix = k->order[s];
      const float *row = k->ex + (size_t)row_ix * stride;
      for (int i = 0; i < NI; ++i) x[i] = iris_norm_in (k, i, row[i]);
      for (int o = 0; o < NOUT; ++o) t[o] = iris_norm_out(k, o, row[NI + o]);

      iris_forward_norm(k, x);

      /* --- output layer error ---------------------------------------------
         ⚠️ THIS IS A SURROGATE GRADIENT, NOT THE GRADIENT. Read this before
         citing anything about the trainer.

         d_out = (predicted - target) * y*(1-y). y*(1-y) is the exact
         derivative of the TRUE logistic. Our forward pass does not use the
         true logistic: iris_sigmoid is built from iris_tanh, the clamped
         rational approximant of PART 1. So the backward pass is not the
         derivative of the forward pass. It is a surrogate -- close enough in
         shape to point downhill, and kept because the measured fits are good
         and changing it would move the golden training hash in the audit.

         HOW WRONG. Exact only at zero, and under-scaling by up to 2x across
         the ordinary operating range. The full ratio table is
         docs/MATH-AUDIT.md:153, which is computed on the UNCLAMPED rational
         and does change sign there. THE SHIPPED FUNCTION DOES NOT: the clamp
         bounds a to [-1,1], so 1-a*a is never negative. Measured over
         66,368,438 finite float bit patterns: 0 negatives, with a positive
         control on the unclamped form finding 3,970,919 of 16,527,549. This
         line used to claim the shipped code changes sign, contradicting the
         "never wrong-signed" statement in PART 1; PART 1 was the correct one.

         WHY IT STAYS. The textbook objection is that y*(1-y) collapses the
         gradient exactly when a unit is confidently wrong. Instrumented for
         that event, it fired ZERO times in 48.96 million output-unit updates
         -- it cannot fire at the defaults, because targets live in [0.1,0.9]
         so y*(1-y) >= 0.09 whenever the network is near its target. THE
         HONEST CAVEAT: that is measured absent at the defaults and measured
         PRESENT above a learning rate of 0.5, which the internal setter used
         to permit up to 2.0. Every proposed repair measured worse
         (docs/MATH-FIXES.md defect 3).

         WHAT IS BEING TRADED AWAY, stated plainly rather than buried: one arm
         does beat this -- a cross-entropy gradient with targets left in
         [0.1,0.9], which wins 32/32 seeds at N=20 by ~5%, and ~12% at a tuned
         learning rate. It LOSES at N=10 (1.094x), which is the regime a
         musician actually demonstrates in, and it widens the reroll spread in
         the undemonstrated gaps by 1.6x. Reroll being a real control rather
         than a shrug is a stated promise of this library, and y*(1-y) is the
         brake that keeps it. That is a judgement about the use case sitting on
         top of a measurement, not a measurement by itself, and it is recorded
         here as such.

         See docs/MATH-FIXES.md defect 3 and docs/MATH-AUDIT.md section 5. */
      float rse = 0.0f;
      for (int o = 0; o < NOUT; ++o) {
        float y = k->out[o];
        float e = y - t[o];
        err += e * e;
        rse += e * e;
        k->d_out[o] = e * y * (1.0f - y);
      }
      /* THE RESIDUAL LEDGER (PART 8f). A separate accumulator: it reads the
         same errors and touches no weight, so every bit of the update below
         is what it was before this line existed. */
      k->ex_res[row_ix] += rse;

      /* --- hidden layer error: blame flows backward through the weights ----
         (1 - a*a) is the exact derivative of the TRUE tanh. iris_tanh is not
         tanh — see the surrogate-gradient note on the output layer above, which
         applies here identically. The exact derivative of the approximant
         is ((x*x - 9) / (3*(3 + x*x)))^2, and it is not what this uses. */
      for (int h = 0; h < NH; ++h) {
        float acc = 0.0f;
        for (int o = 0; o < NOUT; ++o) acc += k->w2[(size_t)o * NH + h] * k->d_out[o];
        float a = k->hid[h];
        k->d_hid[h] = acc * (1.0f - a * a);
      }

      /* --- apply the nudges, with momentum -------------------------------- */
      /* WEIGHT DECAY, when asked for. `wd` is zero unless iris_set_l2 was
         called, and when it is zero this is bit-for-bit the update that shipped
         before decay existed — `w[h] -= 0.0f * w[h]` is exact in IEEE, so the
         golden training hash is unaffected and the default path costs one
         multiply that the optimiser can see is dead.

         Decoupled (applied to the weight, not folded into the gradient, so it
         does not accumulate in the momentum term) and on WEIGHTS ONLY. Biases
         are never decayed: penalising a bias shifts the function without
         reducing capacity, which is cost with no benefit. Scaled by 1/n_ex so
         that alpha means the same thing regardless of how many demonstrations
         you have, matching scikit-learn's penalty-to-data ratio. */
      const float wd = k->l2 * k->lr / (float)k->n_ex;
      for (int o = 0; o < NOUT; ++o) {
        float g = k->d_out[o];
        float *w = k->w2 + (size_t)o * NH, *v = k->v_w2 + (size_t)o * NH;
        for (int h = 0; h < NH; ++h) {
          v[h] = IRIS_FLUSH(k->momentum * v[h] - k->lr * g * k->hid[h]);
          w[h] += v[h];
          w[h] -= wd * w[h];
        }
        k->v_b2[o] = IRIS_FLUSH(k->momentum * k->v_b2[o] - k->lr * g);
        k->b2[o]  += k->v_b2[o];      /* biases are not decayed */
      }
      for (int h = 0; h < NH; ++h) {
        float g = k->d_hid[h];
        float *w = k->w1 + (size_t)h * NI, *v = k->v_w1 + (size_t)h * NI;
        for (int i = 0; i < NI; ++i) {
          v[i] = IRIS_FLUSH(k->momentum * v[i] - k->lr * g * x[i]);
          w[i] += v[i];
          w[i] -= wd * w[i];
        }
        k->v_b1[h] = IRIS_FLUSH(k->momentum * k->v_b1[h] - k->lr * g);
        k->b1[h]  += k->v_b1[h];      /* biases are not decayed */
      }
    }
    err /= (float)(k->n_ex * NOUT);
    k->res_epochs++;
    k->tr_done++;

#ifndef IRIS_NO_GUARDS
    /* Health check, once per epoch. The error accumulator has touched every
       activation this epoch, so it is a one-float summary of the network's
       numerical health; the weight sweep catches saturation-style divergence
       the error can't see (err stays finite while weights run away).        */
    if (iris_isbad(err)) {
      iris_reseed(k, k->seed);                 /* finite again, deterministic */
      k->status = IRIS_NAN_TRAPPED;
      k->last_error = 1.0f;
      k->tr_running = 0;
      return 1.0f;
    }
    {
      int st = iris_internal_check_weights(k);
      if (st == IRIS_NAN_TRAPPED) {
        iris_reseed(k, k->seed);
        k->status = IRIS_NAN_TRAPPED;
        k->last_error = 1.0f;
        k->tr_running = 0;
        return 1.0f;
      }
      if (st == IRIS_TRAINING_DIVERGED) {      /* clamped; stop and report */
        k->status = IRIS_TRAINING_DIVERGED;
        k->tr_running = 0;
        break;
      }
    }
#endif
    /* THE ERROR FLOOR — a fourth stopping rule, and the one most likely to be
       what actually stopped you. It sits outside the `conv` guard on purpose
       (a perfect fit is a reason to stop on any path), but that means it also
       fires on the fixed-epoch path, which is the reference implementation.
       MEASURED: at 5 examples, 36-39 of 40 seeds stop HERE, not on the plateau
       test. Ask iris_train_epochs_done() how many epochs actually ran; if it is
       below what you asked for and no guard fired, this is why. */
    if (err < 1e-6f) { k->tr_running = 0; break; }

    /* --- the plateau test, and the progress report ----------------------- */
    if (conv && (k->tr_done % IRIS_CONV_WINDOW) == 0) {
      if (cb && !cb(user, k->tr_done, k->tr_ceiling, err)) { k->tr_running = 0; break; }
      if (k->tr_ref > 0.0f && (k->tr_ref - err) <= IRIS_CONV_TOL * k->tr_ref) {
        k->tr_running = 0;
        break;
      }
      k->tr_ref = err;
    }
  }

  k->trained = 1;
  k->fitted  = 1;
  k->last_error = err;
  return err;
}

/* THE FIXED-EPOCH TRAINER. Matches Weka MultilayerPerceptron's per-weight
   update recursion and its per-sample update granularity; see the divergence
   table for defaults and activations.

   WHAT THAT DOES AND DOES NOT CLAIM. The recursion is an exact algebraic
   rewrite of Weka's (ours: v = momentum*v - lr*g*x, w += v; theirs:
   delta = lr*err*x + momentum*delta_prev, w += delta — same formula, opposite
   sign convention, both starting at zero). The granularity matches: n_ex
   weight writes per epoch, not one.

   It is NOT numerically identical to Weka and cannot be. We compute in
   binary32; Weka computes in binary64 at every step. Exact agreement is
   impossible in principle, not merely unachieved. It also is not identical in
   behaviour: our hidden units use the approximant against their logistic, our output
   units are sigmoid-then-clamp against their unthresholded linear, our
   defaults are lr 0.10 / momentum 0.85 against their 0.3 / 0.2, and we
   reshuffle every epoch where they shuffle once. Same rule, different
   quantities entering it, therefore different trajectories.

   Every "bit-identical" claim in this file is a claim about THIS FILE's
   self-consistency — sliced vs unsliced runs, save/load round trips, -O0 vs
   -O3 — never about Weka. Audit check 12 hashes the weights this produces.
   Do not "improve" it; iris_train_converge is where improvements go. */
IRIS_API float iris_train_epochs(iris *k, int epochs) { if (!k) return -1.0f;
  k->tr_ceiling = epochs > 0 ? epochs : 0;
  k->tr_running = 0;
  return iris_internal_train_run(k, epochs, 0, 0, 0, 0);
}

/* Train until the training error plateaus. ceiling <= 0 takes
   IRIS_CONV_CEILING. cb may be NULL. Returns the final mean squared error. */
IRIS_API float iris_train_converge(iris *k, int ceiling, iris_progress_fn cb, void *user) { if (!k) return -1.0f;
  const int ceil_ = ceiling > 0 ? ceiling : IRIS_CONV_CEILING;
  k->tr_ceiling = ceil_;
  k->tr_running = 1;
  {
    float e = iris_internal_train_run(k, ceil_, 1, 0, cb, user);
    k->tr_running = 0;
    return e;
  }
}

/* TRAIN. This is the one to call.

   It runs until the error stops improving, which is what you want and what the
   other trainers are for tuning. No epoch count to guess, no callback, no
   ceiling: those live on iris_train_converge for the rare caller who needs
   them, and every one of them has a good default here.

   Returns 1 if it trained, 0 if it refused -- no demonstrations, a poisoned
   one, or a null instrument. If you want to know HOW WELL it fits, that is a
   separate question with a separate answer: iris_last_error(k). */
IRIS_API int iris_train(iris *k) {
  if (!k) return 0;
  /* FIT FROM A DEFINED START, always.

     This used to continue from whatever weights were already there, and that
     quietly broke the loop this library exists for. Record a bad take, delete
     it, retrain -- the documented repair -- and the deleted take's crater
     stayed in the instrument, because the weights it had bent were the weights
     training resumed from. Measured over 40 seeds: the places you did NOT
     demonstrate came back 215 times further from the mapping you showed it,
     in every single run, while iris_last_error moved the other way and the
     status reported perfect health. The one number a screen can show said the
     instrument had improved.

     Warm-starting is still right, and the argument for it above iris_correct
     is still correct: continuing from the current fit is how you adjust one
     region without rewriting the mapping everywhere, which is how a musician
     keeps technique. But that is what iris_correct is FOR. This function is
     called train, a caller expects it to fit the demonstrations it has now,
     and the two must not be the same act. */
  if (k->n_ex == 0) return 0;      /* nothing to fit is not a successful fit */

  /* WHAT THIS BETS ON, AND WHEN THE BET IS WRONG.

     Training stops when a window of epochs stops buying much error. That rule
     is right for demonstrations recorded by a human hand, which are noisy: a
     long run there fits the noise and generalises 13-52% WORSE.

     It is expensive for clean demonstrations, and the size of that is worth
     knowing. On a straight one-sensor ramp with twelve evenly spaced takes:

       epochs      training error   worst miss on a demonstrated pose
       4,000       3.26e-04         0.0337    <- where this function stops
       60,000      3.07e-04         0.0299    <- IRIS_CONV_CEILING
       160,000     2.93e-04         0.0323
       320,000     2.06e-06         0.0023    <- 15x better recall
       640,000     9.90e-07         0.0020

     The error sits on a FALSE plateau from epoch 4,000 to 160,000 and then
     falls two orders of magnitude. The plateau outlasts this library's entire
     maximum budget by nearly three times, so nothing here can see past it, and
     raising `ceiling` on iris_train_converge cannot help -- a ceiling is a
     maximum, and the run is stopping far below it.

     If your demonstrations are clean and the recall matters more than the
     generalisation, the escape is iris_train_epochs(k, 320000) or more. That
     is a real choice with a real cost, which is why it is written down here
     rather than made for you. */
  iris_reseed(k, k->seed);
  /* ASK THE FLAG, NOT THE SIGN. This tested only that the returned error was
     non-negative, and a run that trapped a not-a-number partway leaves a
     non-negative error behind while never fitting: measured, iris_train
     returned 1 with iris_is_trained 0 and status 2, which is exactly what
     Rule 1 promises cannot happen. k->trained is set by the run itself and is
     the same answer iris_is_trained gives every other caller. */
  { float e = iris_train_converge(k, 0, 0, 0);
    return (e >= 0.0f && k->trained) ? 1 : 0; }
}


/* The same run, in slices, for a UI that must keep drawing.
     iris_train_begin(k, ceiling);
     while (iris_train_slice(k, 500)) { draw(iris_train_progress(k)); poll(); }
   Bit-identical to iris_train_converge with the same ceiling: the shuffle
   buffer is initialised once, here, and carried across every slice. */
IRIS_API int iris_train_begin(iris *k, int ceiling) { if (!k) return 0;
  if (k->n_ex == 0) return 0;
  k->tr_ceiling = ceiling > 0 ? ceiling : IRIS_CONV_CEILING;
  k->tr_done = 0;
  k->tr_ref = 0.0f;
  k->tr_running = 1;
  k->tr_n_ex = k->n_ex;
  for (int i = 0; i < k->n_ex; ++i) k->order[i] = i;
  for (int i = 0; i < k->cap;  ++i) k->ex_res[i] = 0.0f;
  k->res_epochs = 0;
  return 1;
}

/* Runs at most `epochs` more. Returns 1 if there is more to do, 0 when the
   run has finished (plateau, ceiling, early stop, or a guard). */
IRIS_API int iris_train_slice(iris *k, int epochs) { if (!k) return 0;
  /* A budget of zero or less is "do nothing", not "use the default". It used
     to fall through to the engine's own default of 2,000 epochs, so a caller
     computing a slice size that came out zero silently trained instead. */
  if (epochs <= 0) return k->tr_running;
  if (!k->tr_running) return 0;
  {
    int left = k->tr_ceiling - k->tr_done;
    if (epochs > left) epochs = left;
    if (epochs <= 0) { k->tr_running = 0; return 0; }
    iris_internal_train_run(k, epochs, 1, 1, 0, 0);
  }
  if (k->tr_done >= k->tr_ceiling) k->tr_running = 0;
  return k->tr_running;
}

/* 0.0 at the start, 1.0 when the run has finished. While a converged run is
   still going this is epochs-spent / ceiling, which is a LOWER bound — the
   run will usually stop early — so the bar never goes backwards and never
   claims to be further along than it is. */
IRIS_API float iris_train_progress(const iris *k) { if (!k) return 0.0f;
  /* "Not running" covers two situations that need opposite answers: a run
     that FINISHED is 1.0, and a run that never STARTED is 0.0. Returning 1.0
     for both drew a student's progress bar full before they pressed anything,
     empty one slice later, then full again. tr_ceiling and tr_done are both 0
     only on a fresh or freshly-loaded instrument -- iris_train_begin sets the
     ceiling first thing -- so they are what tells the two apart. */
  if (!k->tr_running)
    return (k->tr_ceiling > 0 || k->tr_done > 0) ? 1.0f : 0.0f;
  if (k->tr_ceiling <= 0) return 1.0f;
  {
    float f = (float)k->tr_done / (float)k->tr_ceiling;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
  }
}
IRIS_API int iris_train_busy(const iris *k) { if (!k) return 0; return k->tr_running; }

/* How many epochs the last run ACTUALLY did. Compare against what you asked
   for: fewer means it stopped early, and there are four rules that can do
   that — the plateau test, the error floor (1e-6), the divergence guard, or a
   progress callback returning 0. iris_get_status() distinguishes the guard;
   this distinguishes "ran to completion" from "stopped for a good reason",
   which iris_train_progress() deliberately cannot, because it reports 1.0 for
   any finished run. Added 2026-08-26 — before this there was no way to tell. */
IRIS_API int iris_train_epochs_done(const iris *k) { if (!k) return 0; return k->tr_done; }

/* Train and immediately reroll from a fresh random start. This is the
   "give me a different instrument from the same examples" button — the thing
   a deterministic model fundamentally cannot offer. */
IRIS_API float iris_retrain_new(iris *k, uint32_t seed, int epochs) { if (!k) return -1.0f;
  /* Check what the trainer will refuse BEFORE throwing the weights away.
     iris_reseed destroys the instrument; iris_train_epochs then declined a
     zero budget and returned -1.0, so the caller saw a refusal and had
     nevertheless lost their instrument. Refuse first, destroy nothing. */
  if (epochs <= 0)  return -1.0f;
  if (k->n_ex == 0) return -1.0f;
  iris_reseed(k, seed);
  return iris_train_epochs(k, epochs);
}
/* LEAVE-ONE-OUT CROSS-VALIDATION — a real held-out error, at a size where you
   can afford it.

   THE OBJECTION THIS ANSWERS. Everything else in this library measures itself
   against the examples it was fitted on. Training stops when TRAINING error
   plateaus, which is not the same event as "it got as good as it is going to
   get at things it has not seen", and an ML reviewer is right to say so. The
   usual remedy — hold back 20% as a validation set — is unavailable here: at 20
   demonstrations that discards 4 of them, and you cannot spare 4.

   Leave-one-out is the remedy that fits this regime. Hide ONE demonstration,
   refit on the rest, and see how far off the hidden one you land. Do that once
   per demonstration and average. Nothing is discarded; every example is used
   for training in every fold but its own.

   WHAT IT COSTS. n_ex full retrains. At 20 examples on a laptop that is roughly
   half a second; on the ESP32-S3, using the one measured on-device training
   figure (321 ms at 20 examples for 600 epochs), roughly 6 s. That is a
   deliberate, occasional act — "how good is this actually?" — not something to
   put in a play loop.

   HOW TO USE IT. The honest use is comparison, not an absolute grade: run it at
   several l2 values and take the lowest. That is a principled way to pick a
   penalty without the circularity of tuning on the numbers you then report,
   which is the mistake this project has made twice and caught twice.

   HOW WELL IT ACTUALLY WORKS, measured 2026-08-27 rather than assumed. On a
   20-demonstration noisy task, sweeping l2 over {0, 1e-4, 1e-3, 1e-2, 1e-1} and
   comparing what LOO chose against the true error on a clean held-out grid:

       l2        LOO said     truth said
       0         0.002296     0.000577
       1e-4      0.002293     0.000576
       1e-3      0.002273     0.000565
       1e-2      0.002135     0.000500   <- LOO's pick
       1e-1      0.002396     0.000378   <- actually best

   LOO ranked the coarse direction correctly — it put the unregularised arms
   last and identified that a penalty helps — and then chose ONE STEP
   CONSERVATIVE of the true optimum. That is the known behaviour of
   leave-one-out at small n: nearly unbiased but high variance, and each fold
   trains on n-1 examples rather than n, which makes it pessimistic about how
   much regularisation you need. Treat it as a coarse ranking instrument, not a
   precision one: it will tell you whether to regularise and roughly how much,
   and it will not find the exact optimum. Its absolute value is also NOT
   comparable to a grid error — the two columns above differ by ~4x — so use it
   only to compare settings against each other.

   Every fold trains from the SAME seed so the folds differ only by which
   example was hidden. Returns mean squared error per output, or -1 if there are
   fewer than 3 demonstrations to fold over.

   THE INSTRUMENT IS LEFT REFITTED ON ALL EXAMPLES, from that same seed, so it
   is valid to play afterwards — but it is NOT the instrument you had before you
   called this, because it has been retrained. Save first if that matters. */
IRIS_API float iris_loo_error(iris *k, int epochs) { if (!k) return -1.0f;
  if (!iris_shape_fits(k)) { k->status = IRIS_NOT_FITTED; return -1.0f; }
  if (k->n_ex < 3) return -1.0f;
  const int n = k->n_ex, ni = k->n_in, no = k->n_out, stride = ni + no;
  const uint32_t seed0 = k->seed;
  const int ep = epochs > 0 ? epochs : 600;
  float held[IRIS_MAX_IN + IRIS_MAX_OUT], pred[IRIS_MAX_OUT];
  double total = 0.0;

  for (int i = 0; i < n; ++i) {
    float *row_i = k->ex + (size_t)i * stride;
    float *row_l = k->ex + (size_t)(n - 1) * stride;
    int    id_i  = k->ex_id[i];
    int    j;
    for (j = 0; j < stride; ++j) held[j] = row_i[j];        /* remember it */
    for (j = 0; j < stride; ++j) row_i[j] = row_l[j];       /* swap to end */
    for (j = 0; j < stride; ++j) row_l[j] = held[j];
    k->ex_id[i] = k->ex_id[n - 1]; k->ex_id[n - 1] = id_i;

    k->n_ex = n - 1;                                        /* hide it     */
    iris_retrain_new(k, seed0, ep);
    iris_predict(k, held, pred);
    for (j = 0; j < no; ++j) {
      double e = (double)pred[j] - (double)held[ni + j];
      total += e * e;
    }
    k->n_ex = n;                                            /* put it back */
    for (j = 0; j < stride; ++j) held[j] = row_i[j];
    for (j = 0; j < stride; ++j) row_i[j] = row_l[j];
    for (j = 0; j < stride; ++j) row_l[j] = held[j];
    id_i = k->ex_id[i]; k->ex_id[i] = k->ex_id[n - 1]; k->ex_id[n - 1] = id_i;
  }

  iris_retrain_new(k, seed0, ep);      /* leave it playable, fitted on all */
  return (float)(total / ((double)n * (double)no));
}

/* SUGGEST A SMOOTHING VALUE — an explicit, occasional act, not an automatic one.

   Runs leave-one-out across five smoothing settings and returns the one that
   scored best. IT DOES NOT APPLY IT. You get the number, you decide.

   WHY IT IS NOT AUTOMATIC — this was tested as an automatic default and it
   failed the bar set for it:

     - IT IS NOT STABLE. On ONE fixed dataset, re-rolled 16 times, it returned
       2.36 distinct values on average. An instrument whose smoothing changes
       when you reroll is an instrument that stops being predictable, which is
       worse than one that is merely unsmoothed.
     - IT IS SOMETIMES WORSE THAN DOING NOTHING. On 16.7% of datasets its pick
       scored worse than smoothing 0.
     - IT IS SLOW. Five leave-one-out sweeps: ~120 ms on a laptop, but roughly
       37 s on the ESP32-S3 at 20 demonstrations and 202 s at 50, scaled from
       the one measured on-device figure. That is not something to hide inside
       a training call.

   WHAT IT IS GOOD FOR: an honest starting point when you genuinely do not know,
   on a machine where 120 ms is nothing. It captured about 80% of what a perfect
   oracle would have gained at realistic noise levels. Treat the number as a
   suggestion to audition, not an answer — and if you like where you land, pin
   it in your code rather than re-deriving it, so your instrument stays put. */
/* Forward declarations: the save/load functions are defined further down the
   file, and this one needs them to protect the caller's instrument. */
IRIS_API size_t iris_save_size(const iris *k);
IRIS_API size_t iris_save(const iris *k, void *buf, size_t cap);
IRIS_API int    iris_load(iris *k, const void *buf, size_t bytes);

/* ASKING FOR ADVICE MUST NOT COST YOU YOUR INSTRUMENT.

   This runs a leave-one-out sweep across five smoothing settings, and each
   one refits the network from scratch, once per demonstration. It used to
   restore only the SETTING, so a performer who called it to ask a question got
   their answer and, silently, a different instrument: whatever the last rung
   of the ladder left behind at a 600-epoch budget, in place of the one they
   had trained to a plateau. Measured drift on one output: 0.14 of full scale,
   which on a filter cutoff is plainly audible.

   So it now saves the instrument first and puts it back afterwards, which is
   why it needs scratch space: the arena is exactly sized and has nowhere to
   keep a copy. Give it iris_save_size(k) bytes. It refuses rather than
   proceeding if you do not -- refusing an answer is recoverable, and quietly
   replacing someone's instrument is not.

   It still suggests; it still does not decide. Applying the number is yours. */
IRIS_API float iris_suggest_smoothing(iris *k, void *scratch, size_t scratch_bytes) {
  if (!k) return -1.0f;
  if (!scratch || scratch_bytes < iris_save_size(k)) return -1.0f;
  {
    const size_t saved = iris_save(k, scratch, scratch_bytes);
    if (saved == 0) return -1.0f;
    {
      const float ladder[5] = { 0.0f, 0.05f, 0.15f, 0.5f, 1.0f };
      const float keep        = iris_get_smoothing(k);
      const int32_t keep_done = k->tr_done;
      const float keep_err    = k->last_error;
      const int32_t keep_status = k->status;
      float best_v = 0.0f, best_e = -1.0f;
      int i;
      for (i = 0; i < 5; ++i) {
        iris_set_smoothing(k, ladder[i]);
        {
          float e = iris_loo_error(k, 0);
          if (e >= 0.0f && (best_e < 0.0f || e < best_e)) { best_e = e; best_v = ladder[i]; }
        }
      }
      /* Put the performer's instrument back, exactly. The file carries the
         weights, the demonstrations and the smoothing setting -- but not what
         the instrument REPORTS about its own training, so a caller watching
         iris_train_epochs_done saw it fall to zero after asking a question.
         Snapshot those fields and restore them on top of the load. */
      iris_load(k, scratch, saved);
      iris_set_smoothing(k, keep);
      k->tr_done   = keep_done;
      k->last_error = keep_err;
      k->status    = keep_status;
      return best_e < 0.0f ? -1.0f : best_v;
    }
  }
}


/* ==========================================================================
   PART 8f — WHICH DEMONSTRATION IS FIGHTING THE OTHERS

   The trial-and-error trap in interactive ML is that when the instrument
   feels wrong you have no idea WHICH of your twenty demonstrations is wrong,
   so you re-record at random. This points at one.

   IT IS THE INTEGRAL, NOT THE ENDPOINT, AND THAT IS THE WHOLE IDEA.
   The obvious detector is the final training residual: after training, ask
   each example how badly the model still misses it. It gets WORSE as the
   mistake gets bigger, because given enough epochs the optimiser bends the
   surface far enough to fit the bad point too, after which it looks like
   every other point. So this sums each example's squared error over EVERY
   epoch instead, which measures how long it fought rather than where it
   ended up. An example that agrees with its neighbours is fitted early and
   stays fitted; one that contradicts them stays wrong for thousands of
   epochs. That ranking is stable across training budgets where the endpoint
   is not.

   WHAT YOU GET BACK IS A MARGIN, NOT A LEVEL, and that is deliberate. The
   worst-of-n stress score rises with n on clean data with nothing wrong at
   all, so a user interface wired to a fixed level would be silent on small
   rigs and cry wolf on large ones. Worst divided by second-worst does not
   drift. IRIS_STRESS_FLAG is 2.5.

   BELOW IRIS_STRESS_MIN_EX (12) IT RETURNS -1 AND SAYS NOTHING. An example
   can only be caught disagreeing with a crowd if there is a crowd; at ten
   demonstrations the clean margin alone reaches 5.04, which would be a false
   accusation. That is the situation, not a tuning failure.

   TWO LIMITS THAT TRAVEL WITH IT. A 5% offset on one of eight outputs is
   smaller than the spread between two takes of the same human gesture, and
   nothing here finds it reliably -- this function does not pretend to. And
   every number behind it comes from a clean offset on a smooth, noiseless
   truth: real demonstrations are inconsistent in ways that are not one
   displaced output, and none of this has been checked against a recorded
   human gesture.

   COST. sizeof(float) * cap in the arena -- 512 B at cap 128, 1 KB at cap 256
   -- and one float add per example per epoch, under 0.1% of the backprop work
   already being done for that example. Not free; that is the price.

   All the measurements, the detector comparison, the margin table and the
   relation to TracIn: docs/adr/0019-the-residual-ledger-integrates-it-does-not-sample.md
   ========================================================================== */

/* Fewer demonstrations than this and there is no crowd to disagree with. */
#define IRIS_STRESS_MIN_EX 12
/* Margin (worst / second-worst) at which a UI should say something out loud.
   Above every clean-data margin measured at 20, 50 and 100 examples. */
#define IRIS_STRESS_FLAG 2.5f

/* Relative stress of one example: its integrated training error divided by
   the mean over all examples, so 1.0 is an ordinary example. This is a
   RANKING, and it is meaningful at any example count — it is only the
   decision to speak that needs a crowd. 0.0f before any training, or for an
   index out of range. */
IRIS_API float iris_example_stress(const iris *k, int idx) { if (!k) return 0.0f;
  if (idx < 0 || idx >= k->n_ex || k->res_epochs == 0 || k->n_ex == 0) return 0.0f;
  {
    float sum = 0.0f;
    for (int i = 0; i < k->n_ex; ++i) sum += k->ex_res[i];
    if (sum <= 0.0f) return 0.0f;
    return k->ex_res[idx] * (float)k->n_ex / sum;
  }
}

/* The one to point at. Returns the INDEX of the example that fought hardest,
   or -1 when there is nothing to point at: untrained, or fewer than
   IRIS_STRESS_MIN_EX demonstrations. *margin, when given, receives worst
   divided by second-worst.

   THE CALLER DECIDES WHETHER TO SPEAK, and the condition is written once,
   here, so that every UI uses the same one:

       float m; int id = iris_worst_example_id(k, &m);
       if (id >= 0 && m >= IRIS_STRESS_FLAG)  say("example %d is fighting the
                                                 others", id);

   USE iris_worst_example_id, NOT iris_worst_example + iris_id_at: it returns
   the stable example ID directly, and ids survive deletions where indices do
   not. Both stay public for callers who want the positional index, but they
   are not the recommended path.

   The index is returned even below the flag because the ranking is still
   real and a UI may want to show it quietly (a dimmer mark, say) without
   accusing anything. Ties go to the earliest-recorded example, the same rule
   as iris_knn_predict. */
IRIS_API int iris_worst_example(const iris *k, float *margin) { if (!k) return -1;
  if (margin) *margin = 0.0f;
  if (k->n_ex < IRIS_STRESS_MIN_EX || k->res_epochs == 0) return -1;
  {
    int best = 0, second = -1;
    for (int i = 1; i < k->n_ex; ++i) if (k->ex_res[i] > k->ex_res[best]) best = i;
    for (int i = 0; i < k->n_ex; ++i)
      if (i != best && (second < 0 || k->ex_res[i] > k->ex_res[second])) second = i;
    if (margin) {
      float d = second >= 0 ? k->ex_res[second] : 0.0f;
      *margin = (d > 1e-20f) ? k->ex_res[best] / d : 0.0f;
    }
    return best;
  }
}

/* The stable id of that example — what a UI should say out loud, because ids
   survive deletions and indices do not. -1 when there is nothing to say. */
IRIS_API int iris_worst_example_id(const iris *k, float *margin) { if (!k) return -1;
  int i = iris_worst_example(k, margin);
  return i < 0 ? -1 : k->ex_id[i];
}

/* ==========================================================================
   PART 8b — THE CORRECTION  (warm start)

   The musician just recorded one more example (or deleted one) and wants the
   instrument fixed NOW, without losing the instrument they practised. The
   old way — retrain from the seed — rewrites the mapping EVERYWHERE at an
   honest budget (measured drift 0.030 far from the correction at 30 epochs);
   that is the documented way musicians lose accumulated technique to
   retraining (Fiebrink & Sonami, NIME 2020). The fix is embarrassingly
   simple: don't reseed. Keep the trained weights, zero the momentum, run a
   short burst. Measured at 20 examples: equal fit to a cold 600-epoch
   retrain, 9x less collateral change, 27x faster.

   THE POLICY AS DESIGNED: record or delete an example, then call iris_correct —
   same instrument, fixed. An explicit reroll gesture calls iris_retrain_new —
   deliberately a NEW instrument. Nothing else reseeds.

   ⚠️ NOTHING SHIPPED CALLS THIS. iris_correct has no caller in this
   repository outside the tests, and the instrument application it was written
   for stopped using it in August 2026 in favour of the ELM solve. ADR 0005 is
   still marked accepted and still describes it as the policy. It is a working,
   measured library facility with no production consumer; treat it as such
   until that decision is revisited.

   Determinism becomes event-sourced: replaying the identical operation
   history (records / corrections / deletes, in order) reproduces the
   instrument bit-exactly, because the corrections draw from the same rng
   stream. The seed ALONE now reproduces only a from-scratch retrain — a
   saved file carries the live rng state (PART 9) so a reloaded
   instrument continues exactly where it left off.

   Zeroing the velocity at entry is what makes that cheap: it turns the
   momentum arrays into transient scratch instead of hidden persistent state,
   so the file needs one extra word (rng), not four extra weight arrays.
   A converged net's velocities are already ~0; measured cost of the zeroing:
   every correction metric identical to 4 decimals.
   ========================================================================== */

IRIS_API void iris_zero_velocity(iris *k) { if (!k) return;
  for (int i = 0; i < k->n_hid * k->n_in;  ++i) k->v_w1[i] = 0.0f;
  for (int i = 0; i < k->n_hid;            ++i) k->v_b1[i] = 0.0f;
  for (int i = 0; i < k->n_out * k->n_hid; ++i) k->v_w2[i] = 0.0f;
  for (int i = 0; i < k->n_out;            ++i) k->v_b2[i] = 0.0f;
}

/* Warm correction. epochs <= 0 takes the default budget of 20, which reaches
   cold-600 parity on the reference task (train rms 0.019) with far-field
   drift under 0.004. There is deliberately no "present the new example
   extra times" parameter: measured, every boost k >= 1 slows convergence
   and k >= 2 oscillates on contradictory corrections. */
IRIS_API float iris_correct(iris *k, int epochs) { if (!k) return -1.0f;
  iris_zero_velocity(k);
  return iris_train_epochs(k, epochs > 0 ? epochs : 20);
}

IRIS_API int   iris_is_trained(const iris *k) { if (!k) return 0; return k->trained; }
IRIS_API float iris_last_error(const iris *k) { if (!k) return 0.0f; return k->last_error; }
IRIS_API uint32_t iris_seed(const iris *k)    { if (!k) return 0u; return k->seed; }

/* ==========================================================================
   PART 8d — THE INSTANT TRAINER  (ELM: freeze the randomness, solve the rest)

   The third trainer, and the fastest thing in this file by two orders of
   magnitude: retrain at 50 examples in ~0.2 ms estimated on the S3 (nh=12),
   300-425x the 600-epoch backprop path. The trick is to stop training half
   the network. Draw the hidden layer once from the seed and FREEZE it; the
   output layer is then a linear least-squares problem with an exact
   closed-form answer — one (nh+1)x(nh+1) Cholesky solve, no epochs, no
   iteration, no possibility of divergence (the ridged normal matrix is
   symmetric positive definite BY CONSTRUCTION). This idea has a name in the
   literature — extreme learning machine, ELM — and a 20-year argument about
   whether it deserves one; we use it because it is measured to work here.

   Two findings make it work in float32 on this network:

   GAIN. The backprop init (1/sqrt(n_in)) relies on training to grow the
   weights. Frozen, at that scale, tanh of a normalised input barely bends --
   the random features are nearly collinear and the normal matrix is
   numerically rank-deficient. The frozen layer is drawn at 2/sqrt(n_in)
   instead, wide enough that the features have real capacity.

   PROVENANCE OF THE 2/sqrt(n_in), settled 2026-08-30. This comment used to
   cite a measured optimum from a sweep that was not in the tree, and then said
   plainly that the number was unsupported. The sweep IS reachable through the
   public API -- iris_train_elm_ex takes gain_w and gain_b -- so it was re-run
   and is now docs/gain-sweep.c, one command to reproduce. Mean held-out error
   over 4 target shapes x 16 seeds x {8,20,50} demonstrations x nh {12,24,48},
   gain = M/sqrt(n_in):

       M         0.25   0.50   1.00   1.50   2.00   3.00   4.00   8.00
       error    .1143  .1025  .0946  .0919  .0905  .0894  .0920  .1087

   The structural argument holds: M=2 beats the backprop init at M=1 by 4.3%.
   The minimum is BROAD and M=2 sits inside it. Stated honestly, M=3 is
   marginally better (1.2%) and the optimum drifts upward with width -- best at
   1.5 for nh=12 and at 3.0 for nh=48 -- so 2 is a good constant rather than
   the best one, and it stays because moving it would move every frozen hash in
   the audit for a 1.2% gain.

   RIDGE, MANDATORY. Even with the wider gain, the unridged float32 normal
   matrix failed Cholesky in EVERY realistic scenario measured -- including 20
   well-spread examples. The ridge is relative (lam0 * trace/(nh+1), so it
   scales with the data) and escalates deterministically: double lambda on a
   failed factorisation, at most 8 times, report the count. If escalation was
   needed the status says IRIS_RIDGE_ESCALATED -- the result is valid, the data
   was harder than usual. The campaign behind that: docs/adr/0009-ridge-is-mandatory.md.

   The solved instrument is an ordinary iris instrument: same w1/b1/w2/b2
   arrays, same iris_predict, saves and loads as a normal file. The solve
   targets logit space -- the exact inverse of our sigmoid -- so the shipping
   forward pass lands on the normalized targets. Stated honestly: that makes it
   a bounded-output VARIANT of the backprop head, not an equivalent.

   THE 4.6e-2 FIGURE, SCOPED. It measures logit-space-sigmoid ELM against
   linear-head ELM -- an internal ablation between two ELM variants
   (docs/adr/0008-elm-same-network-better-math.md). It is NOT the
   ELM-vs-backprop gap, which is not bounded pointwise anywhere in this repo.
   Do not cite it as one.

   REROLL is the reason to love it: a new seed literally IS a new frozen random
   layer, undiluted by any training -- measurably steadier at the demos and
   livelier in the gaps. The purest form of "same examples, different
   instrument" this project has. That corner lives at nh >= 8: four frozen
   random features cannot recall five demos, so ELM refuses nh < 8 outright
   rather than shipping a config that breaks the reroll promise. Numbers and
   the recommended lam0 per width: docs/adr/0008-elm-same-network-better-math.md.

   Determinism: the hidden layer is redrawn from k->seed by a LOCAL rng
   (k->rng is never touched -- a closed-form solve is not an event in the
   correction history), accumulation order is fixed by example order, and the
   escalation schedule is fixed. Same seed + same examples => bit-identical
   weights, verified at nh 12/24/48.
   ========================================================================== */

#define IRIS_ELM_SCRATCH(NH, NO)                                                 \
  ( sizeof(float) * ( (size_t)((NH)+1) * ((NH)+1)      /* A: normal matrix */  \
                    + (size_t)((NH)+1) * (NO)          /* B: rhs           */  \
                    + (size_t)((NH)+1) ) )             /* pristine diagonal */

/* arena + solve scratch in one block, for callers who want one number */
#define IRIS_ARENA_ELM(NI, NH, NO, NEX)                                          \
  ( IRIS_ARENA(NI, NH, NO, NEX) + IRIS_ELM_SCRATCH(NH, NO) )

/* Exact inverse of iris_tanh — our approximant, not the true tanh — via
   Newton on x*(27+x^2) = y*(27+9x^2). Five iterations reach float32
   roundoff over |y| <= 0.98, which covers the whole 0.1-0.9 target band.
   Deterministic: fixed iteration count, no early exit. */
IRIS_API float iris_artanh(float y) {
  y = iris_clampf(y, -0.98f, 0.98f);
  float x = y * (1.0f + 0.33333333f * y * y);      /* series starting point */
  for (int it = 0; it < 5; ++it) {
    const float x2 = x * x;
    const float f  = x * (27.0f + x2) - y * (27.0f + 9.0f * x2);
    const float fp = 27.0f + 3.0f * x2 - 18.0f * y * x;
    x -= f / fp;
  }
  return x;
}

/* Inverse of iris_sigmoid: the pre-activation z with iris_sigmoid(z) == t. */
IRIS_API float iris_logit(float t) { return 2.0f * iris_artanh(2.0f * t - 1.0f); }

/* The full-argument solve, with explicit hidden-layer gains. Returns the
   number of ridge doublings used (0 = first try, status IRIS_RIDGE_ESCALATED
   if > 0), or -1 refusing: nh < 8, no examples, scratch too small, or a
   poisoned (NaN/Inf) example — weights untouched on every refusal. */
IRIS_API int iris_train_elm_ex(iris *k, float lam0, float gain_w, float gain_b,
                           void *scratch, size_t scratch_bytes) { if (!k) return -1;
  if (!scratch || k->n_ex == 0) return -1;
  if (!iris_shape_fits(k)) { k->status = IRIS_NOT_FITTED; return -1; }
  const int NI_ = k->n_in, NH_ = k->n_hid, NO_ = k->n_out, K = NH_ + 1;
  if (NH_ < 8) return -1;              /* below the measured reroll floor */
  if (scratch_bytes < IRIS_ELM_SCRATCH(NH_, NO_)) return -1;

#ifndef IRIS_NO_GUARDS
  /* same door as every other trainer: refuse poisoned examples up front,
     previous weights bit-preserved, the bad example still in the store */
  {
    const int st = NI_ + NO_;
    for (int i = 0; i < k->n_ex * st; ++i)
      if (iris_isbad(k->ex[i])) { k->status = IRIS_NAN_TRAPPED; return -1; }
  }
  k->status = IRIS_STATUS_OK;
#endif

  float *A  = (float *)scratch;        /* K x K   */
  float *B  = A + (size_t)K * K;       /* K x NO  */
  float *dg = B + (size_t)K * NO_;     /* K       */

  iris_fit_ranges(k);

  /* --- the frozen layer, redrawn deterministically from the seed --------- */
  {
    iris_rng r = { k->seed ? k->seed : 1u };
    for (int j = 0; j < NH_; ++j) {
      float *w = k->w1 + (size_t)j * NI_;
      for (int i = 0; i < NI_; ++i) w[i] = iris_rand_sym(&r) * gain_w;
      k->b1[j] = iris_rand_sym(&r) * gain_b;
    }
  }

  /* --- accumulate the normal equations: A = H^T H (upper), B = H^T Z,
     where H is the hidden activations plus a bias column and Z is the
     logit of the normalized targets. One pass over the examples. --------- */
  for (int i = 0; i < K * K; ++i)   A[i] = 0.0f;
  for (int i = 0; i < K * NO_; ++i) B[i] = 0.0f;
  {
    const int stride = NI_ + NO_;
    float x[IRIS_MAX_IN], h[IRIS_MAX_HID + 1];
    for (int n = 0; n < k->n_ex; ++n) {
      const float *row = k->ex + (size_t)n * stride;
      for (int i = 0; i < NI_; ++i) x[i] = iris_norm_in(k, i, row[i]);
      for (int j = 0; j < NH_; ++j) {
        const float *w = k->w1 + (size_t)j * NI_;
        float s = k->b1[j];
        for (int i = 0; i < NI_; ++i) s += w[i] * x[i];
        h[j] = iris_tanh(s);
      }
      h[NH_] = 1.0f;                                   /* bias feature */
      for (int i = 0; i < K; ++i) {
        const float hi = h[i];
        float *Ai = A + (size_t)i * K;
        for (int j = i; j < K; ++j) Ai[j] += hi * h[j];
      }
      for (int o = 0; o < NO_; ++o) {
        const float z = iris_logit(iris_norm_out(k, o, row[NI_ + o]));
        for (int i = 0; i < K; ++i) B[(size_t)i * NO_ + o] += h[i] * z;
      }
    }
  }

  /* --- relative ridge + deterministic lambda-doubling escalation.
     The pristine matrix survives every failed attempt: the factorisation
     writes only the lower triangle, the upper stays as accumulated, and
     the diagonal is parked in dg. ---------------------------------------- */
  float tr = 0.0f;
  for (int i = 0; i < K; ++i) { dg[i] = A[(size_t)i * K + i]; tr += dg[i]; }
  float lam = lam0 * tr / (float)K + 1e-7f /* absolute floor: trace can be ~0 when every
                                 hidden unit saturates identically (all-equal
                                 inputs); the relative term is then 0 and the
                                 solve needs SOME positive diagonal. 1e-7 is
                                 ~1 ulp at the |G|~1 scale tanh features give. */;

  int doublings = -1;
  for (int att = 0; att <= 8; ++att) {
    for (int i = 0; i < K; ++i) {                      /* restore + ridge */
      float *Ai = A + (size_t)i * K;
      for (int j = 0; j < i; ++j) Ai[j] = A[(size_t)j * K + i];
      Ai[i] = dg[i] + lam;
    }
    int okf = 1;                                       /* factor, lower only */
    for (int j = 0; j < K && okf; ++j) {
      float d = A[(size_t)j * K + j];
      for (int c = 0; c < j; ++c) d -= A[(size_t)j * K + c] * A[(size_t)j * K + c];
      if (!(d > 0.0f)) { okf = 0; break; }             /* catches NaN too */
      const float lj = iris_sqrt(d);
      A[(size_t)j * K + j] = lj;
      for (int i = j + 1; i < K; ++i) {
        float s = A[(size_t)i * K + j];
        for (int c = 0; c < j; ++c) s -= A[(size_t)i * K + c] * A[(size_t)j * K + c];
        A[(size_t)i * K + j] = s / lj;
      }
    }
    if (okf) { doublings = att; break; }
    lam *= 2.0f;
  }
  if (doublings < 0) {
    /* REACHABLE, despite what this comment said until 2026-08-30. It claimed
       "unreachable by construction (measured zero failures across the whole
       campaign)". The campaign used the shipped defaults; lam0 is a public
       argument. Measured: iris_train_elm(k, 0.0f, ...) on 256 identical
       demonstrations returns -1 with status 2. So this is an ordinary failure
       path, not an impossible one — recover to a finite,
       deterministic instrument and SAY SO, never sit on broken weights
       (the hidden layer above was already overwritten). */
    iris_reseed(k, k->seed);
    k->status = IRIS_NAN_TRAPPED;
    return -1;
  }

  /* --- back-substitute B in place: L y = B, then L^T beta = y ------------ */
  for (int o = 0; o < NO_; ++o) {
    for (int i = 0; i < K; ++i) {
      float s = B[(size_t)i * NO_ + o];
      for (int c = 0; c < i; ++c) s -= A[(size_t)i * K + c] * B[(size_t)c * NO_ + o];
      B[(size_t)i * NO_ + o] = s / A[(size_t)i * K + i];
    }
    for (int i = K - 1; i >= 0; --i) {
      float s = B[(size_t)i * NO_ + o];
      for (int c = i + 1; c < K; ++c) s -= A[(size_t)c * K + i] * B[(size_t)c * NO_ + o];
      B[(size_t)i * NO_ + o] = s / A[(size_t)i * K + i];
    }
  }

  /* --- install into the ordinary weight arrays; velocities are stale
     backprop state that no longer describes this instrument — zero them */
  for (int o = 0; o < NO_; ++o) {
    float *w = k->w2 + (size_t)o * NH_;
    for (int j = 0; j < NH_; ++j) w[j] = B[(size_t)j * NO_ + o];
    k->b2[o] = B[(size_t)NH_ * NO_ + o];
  }
  iris_zero_velocity(k);

  /* --- recall error, in the same units iris_train_epochs reports ----------- */
  {
    const int stride = NI_ + NO_;
    float x[IRIS_MAX_IN], err = 0.0f;
    for (int n = 0; n < k->n_ex; ++n) {
      const float *row = k->ex + (size_t)n * stride;
      for (int i = 0; i < NI_; ++i) x[i] = iris_norm_in(k, i, row[i]);
      iris_forward_norm(k, x);
      for (int o = 0; o < NO_; ++o) {
        const float e = k->out[o] - iris_norm_out(k, o, row[NI_ + o]);
        err += e * e;
      }
    }
    k->last_error = err / (float)(k->n_ex * NO_);
  }
  k->trained = 1;
  k->fitted  = 1;                    /* a closed-form solve IS a fit */
  if (doublings > 0) k->status = IRIS_RIDGE_ESCALATED;

#ifndef IRIS_NO_GUARDS
  /* DID IT ACTUALLY LEARN A MAPPING? A large enough lam0 -- or a zero gain --
     drives every weight toward nothing, and the solve then maps every gesture
     to the same sound. That is not a failed solve by any numerical test: the
     residual is small, no value is bad, and this returned success with a
     healthy status while the instrument had become a constant.
     So ask the only question that matters to a musician: does it still tell
     two different gestures apart? Sweep the corners of the demonstrated input
     range and measure how far the outputs move. */
  { float lo_o[IRIS_MAX_OUT], hi_o[IRIS_MAX_OUT], probe[IRIS_MAX_IN];
    for (int o = 0; o < NO_; ++o) { lo_o[o] = 1e30f; hi_o[o] = -1e30f; }
    for (int c = 0; c < 4; ++c) {
      /* NORMALISE THE CORNER BEFORE FEEDING IT FORWARD. in_lo and in_hi hold
         RAW sensor values; iris_forward_norm's parameter is named x_norm and
         every other call site normalises first. Handing it raw values made
         this guard's verdict depend on the caller's UNITS: the same instrument
         learning the same mapping reported a healthy solve when the sensor
         spanned 0..1 or 0..4095, and IRIS_DIVERGED_STUCK when it spanned
         0..0.001 -- a false fault telling the musician to reroll an instrument
         that was fine. The header tells callers to feed raw readings and not to
         scale anything (see "Units: none" in PART 1), so the units are the
         caller's business and must not change a verdict. */
      for (int i = 0; i < NI_; ++i)
        probe[i] = iris_norm_in(k, i, ((c >> (i & 1)) & 1) ? k->in_hi[i]
                                                           : k->in_lo[i]);
      iris_forward_norm(k, probe);
      /* Measure in NORMALISED output space, where the band is always
         [0.1,0.9] whatever the demonstrations looked like. Comparing against
         the raw demonstrated range instead would make a single 1e6 outlier
         shrink every honest ratio to nothing and report a healthy solve as
         collapsed -- which is a different defect, and not this one's to
         report. */
      for (int o = 0; o < NO_; ++o) {
        float y = k->out[o];
        if (y < lo_o[o]) lo_o[o] = y;
        if (y > hi_o[o]) hi_o[o] = y;
      }
    }
    float widest = 0.0f;
    for (int o = 0; o < NO_; ++o) { float w = hi_o[o] - lo_o[o]; if (w > widest) widest = w; }

    /* Only ask the question when there was a mapping to learn. If every
       demonstration carried the same sound, a constant IS the right answer and
       flagging it would be reporting correct behaviour as a fault.
       iris_fit_ranges floors a degenerate range at 1e-6, so anything at or
       near that floor means the targets never moved. */
    int something_to_learn = 0;
    for (int o = 0; o < NO_; ++o)
      if (k->out_hi[o] - k->out_lo[o] > 1e-5f) something_to_learn = 1;

    /* The normalised band is 0.8 wide. Moving less than half a percent of it
       across the whole input range is a constant with rounding on it. */
    if (something_to_learn && widest < 0.004f) k->status = IRIS_DIVERGED_STUCK;
  }
#endif
  return doublings;
}

/* The instant trainer with the measured-default gains: 2/sqrt(n_in) for
   weights AND biases. lam0 = 1e-4 is the nh=12 default; 1e-3 at nh=48. */
IRIS_API int iris_train_elm(iris *k, float lam0, void *scratch, size_t scratch_bytes) { if (!k) return -1;
  const float g = 2.0f / iris_sqrt((float)(k->n_in > 0 ? k->n_in : 1));
  return iris_train_elm_ex(k, lam0, g, g, scratch, scratch_bytes);
}

/* The reroll button, ELM flavour: a new seed IS a new frozen random layer,
   refit exactly. This is the deliberate new-instrument gesture, so it also
   resets the rng stream, exactly as iris_retrain_new does. */
IRIS_API int iris_retrain_elm_new(iris *k, uint32_t seed, float lam0,
                              void *scratch, size_t scratch_bytes) { if (!k) return -1;
  k->seed = seed ? seed : 1u;
  k->rng.s = k->seed;
  return iris_train_elm(k, lam0, scratch, scratch_bytes);
}


/* ==========================================================================
   PART 9 — SAVING

   A trained instrument has to be a thing you can put somewhere and get back.
   The file carries the weights AND the examples, so whoever receives it can
   keep working rather than inheriting a sealed box.

   Magic number and version go first so that a file from 2026 can still be
   recognised — or politely refused — in 2036.

   THE LAYOUT. Formats 5 and 6 are byte-for-byte the same; the version word
   alone says whether the instrument had been fitted when it was saved.

     9 words   magic, version, n_in, n_hid, n_out, n_ex, seed, next_id, and
               the live random-number state -- so that a reloaded
               instrument's next correction is the one the saved instrument
               would have made, rather than a fork of it
     floats    w1, b1, w2, b2, in_lo, in_hi, out_lo, out_hi, then every
               demonstration (n_in inputs followed by n_out outputs)
     int32s    one identifier per demonstration
     1 float   the weight decay behind the smoothing setting (PART 3)
     1 word    a checksum over every byte before it (see iris_crc32)

   Inputs are always scaled to [-1,+1] (PART 5), so a stored weight means one
   thing only and the file does not need to say which scaling it was fitted
   under.
   ========================================================================== */

#define IRIS_MAGIC 0x4B455745u  /* "EWEK" */
/* EVERY FILE THIS LOADER READS CARRIES A CHECKSUM: every single-bit change
   anywhere in the file is detected and the load is refused.

   AND ONE THING THE CHECKSUM CANNOT DO, WHICH IS WORTH SAYING HERE RATHER THAN
   LEAVING TO BE DISCOVERED. iris_save copies every field into your buffer and
   computes the checksum over the buffer AFTERWARDS. So the checksum certifies
   the bytes that were written -- not that they all came from the same instant.
   If the instrument changes while the copy is running (a second thread, an
   interrupt, the sliced trainer driven from a timer) the buffer holds a
   mixture of two instruments, and the checksum is computed over the mixture
   and therefore matches. By construction, always. A reviewer raced saves
   against training and got 4,096 of 4,096 torn files loading clean, 32 of them
   badly wrong while reporting healthy.

   This is already out of contract -- see the threading note in PART 1: one
   instrument belongs to one thread. It is called out again here because this
   is the one corruption the guard rail specifically cannot catch, and a
   checksum that passes is exactly the evidence that would persuade you the
   file is fine. Do not save an instrument that something else may be touching.
   There is no allocation and no lock in this library to do it for you. */
/* FORMAT v6 CHANGES NO BYTES AT ALL. Same header, same payload, same length,
   same checksum as v5. What it changes is one fact ABOUT the instrument: v6
   means "this was saved before it was ever fitted".

   Why it has to exist. iris_load used to set trained = 1 unconditionally, so
   record-save-load-play on a never-trained instrument ran the forward pass over
   the random weights iris_reseed left, returned numbers that vary with the
   gesture, and reported IRIS_STATUS_OK. The unfitted guard in iris_predict is
   the defence the front page calls the one thing to get right, and the loader
   walked around it. Nothing in the bytes could tell the two apart, because
   untrained weights are just weights.

   v5 means fitted. */
#define IRIS_FORMAT_V6 6u       /* v5 layout, and it was NOT fitted when saved */
#define IRIS_FORMAT 5u          /* what iris_save writes when trained */

/* CRC32 (IEEE 802.3), computed a bit at a time so there is no 1 KB table to
   carry onto a microcontroller. A few KB takes microseconds and it only runs on
   save and load, never while playing.

   WHY A SAVED INSTRUMENT NEEDS ONE. The file is weights — raw floats with no
   redundancy. Flip one bit in flash, or lose power halfway through an SD write,
   and every field still parses: the magic matches, the shape matches, the sizes
   match, and the instrument loads and plays something subtly wrong with nothing
   reporting anything. That is the worst failure this library can have, because
   the musician will assume they mis-trained it. A checksum turns it into a
   refusal. */
IRIS_API uint32_t iris_crc32(const void *buf, size_t n) {
  const unsigned char *p = (const unsigned char *)buf;
  uint32_t c = 0xFFFFFFFFu;
  size_t i; int b;
  for (i = 0; i < n; ++i) {
    c ^= (uint32_t)p[i];
    for (b = 0; b < 8; ++b) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
  }
  return c ^ 0xFFFFFFFFu;
}

IRIS_API size_t iris_save_size(const iris *k) { if (!k) return 0;
  return sizeof(uint32_t) * 9                      /* 8 header words + rng.s */
       + sizeof(float) * (size_t)( k->n_hid*k->n_in + k->n_hid
                                 + k->n_out*k->n_hid + k->n_out
                                 + 2*(k->n_in + k->n_out)
                                 + (size_t)k->n_ex * (k->n_in + k->n_out) )
       + sizeof(int32_t) * (size_t)k->n_ex
       /* The smoothing setting. docs/FREEZE.md called this "cheap today and
          impossible tomorrow" -- the weights round-trip perfectly without it,
          so nothing sounds wrong until the musician retrains a loaded
          instrument and gets a different one for a reason the file never
          recorded. */
       + sizeof(float)
       + sizeof(uint32_t);                         /* the trailing checksum */
}

IRIS_API size_t iris_save(const iris *k, void *buf, size_t cap) { if (!k) return 0;
  size_t need = iris_save_size(k);
  if (!buf || cap < need) return 0;
  uint32_t *h = (uint32_t *)buf;
  h[0] = IRIS_MAGIC;
  /* v6 when `trained` is clear -- same bytes, and the loader will not claim
     the instrument is trained. */
  h[1] = k->trained ? IRIS_FORMAT : IRIS_FORMAT_V6;
  h[2] = (uint32_t)k->n_in; h[3] = (uint32_t)k->n_hid;
  h[4] = (uint32_t)k->n_out; h[5] = (uint32_t)k->n_ex;
  h[6] = k->seed; h[7] = (uint32_t)k->next_id;
  h[8] = k->rng.s;                                  /* the live rng state */
  float *f = (float *)(h + 9);
  #define IRIS_PUT(src, n) do { for (int _i = 0; _i < (n); ++_i) *f++ = (src)[_i]; } while (0)
  IRIS_PUT(k->w1, k->n_hid*k->n_in);  IRIS_PUT(k->b1, k->n_hid);
  IRIS_PUT(k->w2, k->n_out*k->n_hid); IRIS_PUT(k->b2, k->n_out);
  IRIS_PUT(k->in_lo, k->n_in);   IRIS_PUT(k->in_hi, k->n_in);
  IRIS_PUT(k->out_lo, k->n_out); IRIS_PUT(k->out_hi, k->n_out);
  IRIS_PUT(k->ex, (int)((size_t)k->n_ex * (k->n_in + k->n_out)));
  #undef IRIS_PUT
  int32_t *ids = (int32_t *)f;
  for (int i = 0; i < k->n_ex; ++i) ids[i] = k->ex_id[i];
  {
    /* The smoothing word, then the checksum over everything written so far. */
    uint32_t *tail = (uint32_t *)(ids + k->n_ex);
    float *sm = (float *)tail;
    *sm = k->l2;
    tail = (uint32_t *)(sm + 1);
    *tail = iris_crc32(buf, need - sizeof(uint32_t));
  }
  return need;
}

IRIS_API int iris_load(iris *k, const void *buf, size_t bytes) { if (!k) return 0;
  if (!buf || bytes < sizeof(uint32_t) * 8) return 0;
  const uint32_t *h = (const uint32_t *)buf;
  if (h[0] != IRIS_MAGIC) return 0;
  if (h[1] != IRIS_FORMAT && h[1] != IRIS_FORMAT_V6) {
    k->status = IRIS_NAN_TRAPPED; return 0; }

  /* VERIFY THE CHECKSUM before trusting a single weight. A mismatch means the
     file is damaged -- refuse it rather than play weights that will be subtly
     wrong with nothing reporting anything.
     No bounds test here: the function refuses anything under 8 words at
     entry, so bytes >= 32 and the 4-byte trailer is always present. */
  {
    const unsigned char *b8 = (const unsigned char *)buf;
    uint32_t stored;
    const unsigned char *tp = b8 + bytes - sizeof(uint32_t);
    int i; stored = 0u;
    for (i = 0; i < 4; ++i) stored |= (uint32_t)tp[i] << (8 * i);
    if (iris_crc32(buf, bytes - sizeof(uint32_t)) != stored) return 0;
  }
  if (bytes < sizeof(uint32_t) * 9) return 0;
  if ((int)h[2] != k->n_in || (int)h[3] != k->n_hid || (int)h[4] != k->n_out) return 0;
  /* n_ex is validated UNSIGNED: a corrupted high byte makes (int)h[5]
     negative, which sails under a signed "> cap" check and loads an
     instrument with -16 million examples. */
  if (h[5] > (uint32_t)k->cap) return 0;
  /* And the whole payload must actually be present. The header alone used
     to be enough to start the copy loops, so a truncated file with a valid
     header read kilobytes past the caller's buffer and loaded garbage —
     silently. Compute what this header promises and refuse anything short. */
  {
    const size_t hdr  = sizeof(uint32_t) * 9;
    const size_t nex  = (size_t)h[5];
    const size_t body = sizeof(float) * ( (size_t)k->n_hid*k->n_in + k->n_hid
                                        + (size_t)k->n_out*k->n_hid + k->n_out
                                        + 2u*((size_t)k->n_in + k->n_out)
                                        + nex * ((size_t)k->n_in + k->n_out) )
                      + sizeof(int32_t) * nex
                      + sizeof(float)                         /* smoothing */
                      + sizeof(uint32_t);                     /* checksum */

    /* EXACTLY, not at least. The checksum covers whatever length the caller
       passes, so it cannot notice a file that is longer or shorter than its
       own header describes; the length can. */
    if (bytes != hdr + body) return 0;
  }
  k->n_ex = (int32_t)h[5]; k->seed = h[6]; k->next_id = (int32_t)h[7];
  k->rng.s = h[8] ? h[8] : (k->seed ? k->seed : 1u);
  const float *f = (const float *)(h + 9);
  #define IRIS_GET(dst, n) do { for (int _i = 0; _i < (n); ++_i) (dst)[_i] = *f++; } while (0)
  IRIS_GET(k->w1, k->n_hid*k->n_in);  IRIS_GET(k->b1, k->n_hid);
  IRIS_GET(k->w2, k->n_out*k->n_hid); IRIS_GET(k->b2, k->n_out);
  IRIS_GET(k->in_lo, k->n_in);   IRIS_GET(k->in_hi, k->n_in);
  IRIS_GET(k->out_lo, k->n_out); IRIS_GET(k->out_hi, k->n_out);
  IRIS_GET(k->ex, (int)((size_t)k->n_ex * (k->n_in + k->n_out)));
  #undef IRIS_GET
  const int32_t *ids = (const int32_t *)f;
  for (int i = 0; i < k->n_ex; ++i) k->ex_id[i] = ids[i];

  /* THE RANGES COME OUT OF THE FILE UNCHECKED. iris_fit_ranges floors a
     degenerate range so that dividing by its width cannot produce
     not-a-number, but that floor only ran when ranges were FITTED. A file can
     carry a zero-width range -- written by an older build, or corrupted within
     a valid checksum, or saved from an instrument whose sensor never moved --
     and the loaded instrument then divided by zero on every prediction and
     played the middle of its range for ever. Same floor, same reason, at the
     other door. */
  for (int i = 0; i < k->n_in; ++i) {
    float w = iris_absf(k->in_lo[i]) * 1e-5f;  if (w < 1e-6f) w = 1e-6f;
    if (k->in_hi[i]  - k->in_lo[i]  < w) k->in_hi[i]  = k->in_lo[i]  + w;
  }
  for (int i = 0; i < k->n_out; ++i) {
    float w = iris_absf(k->out_lo[i]) * 1e-5f; if (w < 1e-6f) w = 1e-6f;
    if (k->out_hi[i] - k->out_lo[i] < w) k->out_hi[i] = k->out_lo[i] + w;
  }

  /* THE SMOOTHING SETTING TRAVELS WITH THE FILE. The weights do not depend
     on it; it matters the moment somebody retrains. */
  { const float *sm = (const float *)(ids + k->n_ex);
    k->l2 = iris_clampf(*sm, 0.0f, 0.3f); }

  /* v6 says this instrument was saved before it was ever fitted, so do not
     claim it is; v5 means fitted. Without this the loader walked around
     iris_predict's unfitted guard: record, save, load, play, and the forward
     pass ran over random weights while the status read healthy. */
  { const int was_fit = (h[1] != IRIS_FORMAT_V6);
    k->trained = was_fit;
    /* AND `fitted` TOO, which is the one iris_predict actually guards on.
       `fitted` means "has EVER produced a fit" and is what stops the forward
       pass running over random weights; `trained` only means "the fit still
       matches the examples" and is cleared by any record or delete, which must
       NOT silence an instrument mid-performance. Setting only `trained` here
       left the hole open: is_trained said 0 and iris_predict played anyway. */
    k->fitted  = was_fit; }
  k->status = IRIS_STATUS_OK;   /* a freshly loaded instrument carries no stale error */

  /* MEASURE the loaded instrument's error instead of leaving whatever the
     destination happened to hold -- which for a fresh one is 1.0, the WORST
     possible value, so a user interface showing "training error" read 1.0 for
     a perfectly good instrument. The error is not in the file (that would be a
     format change), but the demonstrations are, so it can simply be computed:
     one forward pass per demonstration, once, at load. */
  { const int st_ = k->n_in + k->n_out;
    float e_ = 0.0f;
    for (int r = 0; r < k->n_ex; ++r) {
      const float *row = k->ex + (size_t)r * st_;
      float xn[IRIS_MAX_IN];
      for (int i = 0; i < k->n_in; ++i) xn[i] = iris_norm_in(k, i, row[i]);
      iris_forward_norm(k, xn);
      for (int o = 0; o < k->n_out; ++o) {
        float d = k->out[o] - iris_norm_out(k, o, row[k->n_in + o]);
        e_ += d * d;
      }
    }
    k->last_error = (k->n_ex > 0) ? e_ / (float)(k->n_ex * k->n_out) : 0.0f;
  }
  /* The residual ledger belongs to a training run, not to a file: a loaded
     instrument has not been trained in this process, so it has no opinion
     about which demonstration is fighting the others until it is. */
  for (int i = 0; i < k->cap; ++i) k->ex_res[i] = 0.0f;
  k->res_epochs = 0;
  k->tr_done = 0; k->tr_ceiling = 0; k->tr_running = 0; k->tr_ref = 0.0f;
  return 1;
}

/* ==========================================================================
   PART 10 — THE SECOND ALGORITHM  (k-NN blending and 1-NN snapping)

   A different character of instrument, not a quality tier. Desktop
   Wekinator ships k-NN as its default for discrete (classifier) outputs;
   until now iris only answered the continuous case. These two functions
   add both modes with ZERO training, ZERO seed, and ZERO extra arena
   bytes: they are a weighted read of the example store the instrument
   already carries. The examples ARE the model — the design rule of this
   whole file, taken to its logical end.

   Semantics, stated as design and not as apology:

     - THIS ALGORITHM DOES NOT REROLL. There is no seed and nothing random;
       the same examples always give the same instrument, bit for bit. It
       is sampler-like where the MLP is morph-like: it plays back and
       blends your demonstrations.

     - EXACT RECALL. Standing on a demonstration returns that
       demonstration — the property backprop never quite delivers (the MLP
       audibly misses its own demos by ~1%).

     - SEAMS, ON PURPOSE. Between two demos the output can step 31x more
       sharply than its mean step (measured; the MLP's morph is 1.9x).
       That is the sampler character, documented, not hidden.

     - STRUCTURAL SAFETY. Output is a convex combination of demonstrated
       outputs: it cannot NaN and cannot leave the range you demonstrated,
       whatever the input does.

     - THE HONEST FLOOR. The MLP generalises better at EVERY example count
       measured (2.4x at 5 examples, 2.1x at 200). Choose k-NN for its
       character or for discrete outputs, never for accuracy.

   Distances live in the min-max normalised input space — the same space
   iris_novelty uses — so a millimetre sensor and a g-force sensor count
   equally. Call iris_fit_ranges(k) (or any train) after editing examples and
   before predicting, exactly as iris_novelty already requires. Ties resolve
   to the earliest-recorded example, the same rule as Weka's
   LinearNNSearch, the engine under desktop Wekinator's classifier — so
   decisions are comparable ("Wekinator-compatible semantics"; the desktop
   Java binary itself has not been run against this code, and the label
   stays this honest until it has).

   Every saved instrument can play this way with nothing added to its file:
   the examples and ranges are already in it. That was the point of the
   format. Algorithm choice is a runtime call in v0.2; a persisted
   algorithm-selector tag waits for the next format bump.
   ========================================================================== */

#define IRIS_KNN_MAXK 8          /* stack bound; k above this is clamped */
#define IRIS_KNN_GUARD 1e-9f     /* zero-distance guard for the weights */

/* k-NN inverse-squared-distance-weighted regression. k neighbours (default
   choice: 3), weight 1/(d^2 + guard) each. Standing exactly on a
   demonstration gives that row a weight of ~1e9 — recall exact to float
   precision; between demonstrations the nearest k blend. Conflicting
   duplicates average finitely (the guard keeps zero-distance weights
   finite). O(n_ex * n_in) per call, division-free scan, no state touched. */
/* LENGTHS, same rule as iris_predict and just as unchecked.
   Reads exactly n_in floats from `in` and writes exactly n_out into `out`. */
IRIS_API void iris_knn_predict(const iris *k, const float *in, float *out, int kk) { if (!k) return;
  if (!iris_shape_fits(k)) { ((iris *)k)->status = IRIS_NOT_FITTED; return; }
  /* The distance measure needs the input ranges, and those are only set by a
     fit. Called on a recorded-but-never-trained instrument this silently used
     the default range of 0..1 and gave a quietly wrong answer. Fit them here:
     it is the same work iris_fit_ranges does, it depends on nothing but the
     demonstrations, and a caller who has to remember an ordering rule will
     eventually forget it. */
  if (!k->fitted && k->n_ex > 0) iris_fit_ranges((iris *)k);
  const int NIn = k->n_in, NOut = k->n_out;
  if (k->n_ex == 0) { for (int o = 0; o < NOut; ++o) out[o] = 0.0f; return; }
  if (kk > k->n_ex) kk = k->n_ex;
  if (kk > IRIS_KNN_MAXK) kk = IRIS_KNN_MAXK;
  if (kk < 1) kk = 1;

  /* precompute 1/range so the scan does no divisions */
  float inv[IRIS_MAX_IN];
  for (int i = 0; i < NIn; ++i) inv[i] = 1.0f / (k->in_hi[i] - k->in_lo[i]);

  const int stride = NIn + NOut;
  int   bi[IRIS_KNN_MAXK];
  float bd[IRIS_KNN_MAXK];
  for (int n = 0; n < kk; ++n) { bi[n] = -1; bd[n] = 1e30f; }

  for (int r = 0; r < k->n_ex; ++r) {
    const float *row = k->ex + (size_t)r * stride;
    float d = 0.0f;
    for (int i = 0; i < NIn; ++i) {
      float t = (row[i] - in[i]) * inv[i];
      d += t * t;
    }
    /* strict < : on a tie the earlier example keeps its slot (Weka rule) */
    int p = kk;
    while (p > 0 && d < bd[p - 1]) --p;
    if (p < kk) {
      for (int q = kk - 1; q > p; --q) { bd[q] = bd[q-1]; bi[q] = bi[q-1]; }
      bd[p] = d; bi[p] = r;
    }
  }

#ifndef IRIS_NO_GUARDS
  /* A non-finite query (or one so far out that every distance overflows to
     +inf) makes every comparison false, so no row is ever inserted and each
     bi[n] is still -1 — and -1 * stride is an out-of-bounds read into
     whatever sits beside the arena. Refuse instead: substitute the centre
     of each demonstrated range and report, exactly like the MLP backstop. */
  if (bi[0] < 0) {
    for (int o = 0; o < NOut; ++o) out[o] = 0.5f * (k->out_lo[o] + k->out_hi[o]);
    ((iris *)k)->status = IRIS_NAN_TRAPPED;
    return;
  }
#endif
  float wsum = 0.0f;
  for (int o = 0; o < NOut; ++o) out[o] = 0.0f;
  for (int n = 0; n < kk; ++n) {
    if (bi[n] < 0) break;   /* fewer than kk insertable rows: use what exists */
    float w = 1.0f / (bd[n] + IRIS_KNN_GUARD);
    const float *row = k->ex + (size_t)bi[n] * stride;
    wsum += w;
    for (int o = 0; o < NOut; ++o) out[o] += w * row[NIn + o];
  }
  float s = 1.0f / wsum;
  for (int o = 0; o < NOut; ++o) out[o] *= s;
#ifndef IRIS_NO_GUARDS
  /* The store admits examples with NaN OUTPUTS (the record-door trap is a
     documented follow-up), and this path would blend such a NaN straight
     into an audio parameter. Same last line of defence as iris_predict. */
  for (int o = 0; o < NOut; ++o)
    if (iris_isbad(out[o])) {
      out[o] = 0.5f * (k->out_lo[o] + k->out_hi[o]);
      ((iris *)k)->status = IRIS_NAN_TRAPPED;
    }
#endif
}

/* 1-NN classification: snap to the single nearest demonstration and return
   its outputs VERBATIM (bit-for-bit) plus its stable example id, or -1 if
   the store is empty. For a classifier task store the class label in
   out[0]; this is then exactly desktop Wekinator's shipping default for
   discrete outputs (Weka IBk, k=1, min-max normalised Euclidean distance,
   first-recorded wins ties). */
/* LENGTHS, same rule as iris_predict and just as unchecked.
   Reads exactly n_in floats from `in` and writes exactly n_out into `out`. */
IRIS_API int iris_classify_1nn(const iris *k, const float *in, float *out) { if (!k) return -1;
  if (!iris_shape_fits(k)) { ((iris *)k)->status = IRIS_NOT_FITTED; return -1; }
  /* Same reason as iris_knn_predict: the distance measure needs the input
     ranges, and only a fit sets them. Without this, a classifier called on a
     recorded-but-never-trained instrument used the default 0..1 range and
     could return the wrong class with a healthy status. */
  if (!k->fitted && k->n_ex > 0) iris_fit_ranges((iris *)k);
  const int NIn = k->n_in, NOut = k->n_out;
  if (k->n_ex == 0) return -1;
  float inv[IRIS_MAX_IN];
  for (int i = 0; i < NIn; ++i) inv[i] = 1.0f / (k->in_hi[i] - k->in_lo[i]);
  const int stride = NIn + NOut;
  int best = -1; float best_d = 1e30f;
  for (int r = 0; r < k->n_ex; ++r) {
    const float *row = k->ex + (size_t)r * stride;
    float d = 0.0f;
    for (int i = 0; i < NIn; ++i) {
      float t = (row[i] - in[i]) * inv[i];
      d += t * t;
    }
    if (d < best_d) { best_d = d; best = r; }
  }
#ifndef IRIS_NO_GUARDS
  /* A non-finite query makes every comparison false, so nothing is ever
     chosen. `best` used to start at 0, which meant a disconnected sensor
     reliably returned the FIRST demonstration and its identifier as though
     they were a real answer, with a healthy status — for a classifier, that
     is a confident wrong class every time. Starting at -1 and refusing here
     matches iris_knn_predict, which has always guarded this case. */
  if (best < 0) {
    if (out) for (int o = 0; o < NOut; ++o)
      out[o] = 0.5f * (k->out_lo[o] + k->out_hi[o]);
    ((iris *)k)->status = IRIS_NAN_TRAPPED;
    return -1;
  }
#else
  if (best < 0) best = 0;
#endif
  if (out) {
    const float *row = k->ex + (size_t)best * stride;
    for (int o = 0; o < NOut; ++o) out[o] = row[NIn + o];
#ifndef IRIS_NO_GUARDS
    /* Verbatim means verbatim for every healthy value — but a NaN stored in
       an example's outputs must not escape as a "class label". Substitute
       the range centre and report; the returned id still names the row so
       the musician can find and delete it. */
    for (int o = 0; o < NOut; ++o)
      if (iris_isbad(out[o])) {
        out[o] = 0.5f * (k->out_lo[o] + k->out_hi[o]);
        ((iris *)k)->status = IRIS_NAN_TRAPPED;
      }
#endif
  }
  return k->ex_id[best];
}

#endif /* IRIS_H */
