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

   THE ZERO-DEPENDENCY CLAIM, STATED EXACTLY. A translation unit that calls
   every public function, compiled with -std=c99 -ffreestanding
   -fno-stack-protector (GCC also -fno-tree-loop-distribute-patterns) at -O0
   to -Os, as C or as C++, has ZERO undefined symbols and links with
   -nostdlib -static and no C library at all: Apple clang, Homebrew clang 22
   and gcc-15 on a 64-bit ARM Mac, and Debian's gcc 14 and clang 19 on 64-bit
   ARM Linux. None of the flags changes an output bit. Each stops the
   COMPILER reaching for the C library on its own account:

     -ffreestanding        clang otherwise turns the loops that zero or copy
                           an array into C library calls: memset, memcpy,
                           bzero and, on macOS, memset_pattern16.
     -fno-stack-protector  where stack protection is on by default (clang on
                           macOS), every function with an array otherwise
                           calls __stack_chk_fail.
     -fno-tree-loop-distribute-patterns   GCC turns the same loops into
                           memset, memcpy and memmove calls even under
                           -ffreestanding.

   -fno-math-errno is not on the list: the square root is integer arithmetic
   (PART 1), so there is no sqrtf call for errno to need. tests/freestanding.sh
   checks all of this, and rebuilds without each flag to show what it still
   keeps out.

   ON THE CHIP IT IS ACTUALLY FOR, THE LIST IS NOT EMPTY. With the ESP32-S3's
   own compiler (xtensa-esp32s3-elf-gcc, the flags above, -O0, -O2 or -Os):

     the playing path        __divsf3
     + iris_loo_error        + __adddf3 __divdf3 __extendsfdf2 __floatsidf
                               __muldf3 __subdf3 __truncdfsf2, and __ledf2
                               at -O0
     + iris_suggest_smoothing  + __ledf2, a comparison of doubles in its
                               scoring, and memcpy, to copy its five-entry
                               constant table
     + iris_train_elm        adds nothing

   __divsf3 is single-precision DIVISION: the S3's floating-point unit has
   divide-step instructions but no single divide instruction, so every float
   division is a routine in libgcc, the compiler's own support library, as
   are the double-precision routines. memcpy is the one C library function.
   None of this is a call this source writes, and all of it is present on
   every Arduino build anyway; tests/freestanding.sh checks this list too.

   THE DOUBLES ARE REAL AND THEY ARE ONE SWEEP. The double-precision
   routines above are 64-bit soft float, which rule 3 below says this library
   does not use. iris_loo_error and iris_suggest_smoothing share one
   leave-one-out sweep (PART 8), which accumulates its error sum in double on
   purpose and, for iris_suggest_smoothing, divides each miss by an output's
   range held in double. That is a deliberate numerical choice in a
   diagnostic that is not on the playing path, and it is the ONLY exception
   -- the playing path has no doubles anywhere. Rule 3 is restated
   below with that exception named, because a rule with a silent exception is
   worse than no rule.

   THE THREE RULES THIS FILE OBEYS
     1. No malloc.  You give it one block of memory; it never asks for more.
        You always know exactly how much RAM the instrument uses.
     2. No libc.    No printf, no math.h. Everything it needs is in here.
     3. No doubles ON THE PLAYING PATH. The ESP32-S3 does 32-bit float in
        hardware and 64-bit float in slow software emulation. The one
        exception is iris_loo_error (and iris_suggest_smoothing, which shares
        its sweep), a diagnostic that accumulates in double deliberately;
        measured above.

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
                 x86 doing its float arithmetic on the x87 unit
                 (-mfpmath=387) reports 2, and there the golden hash and
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
/* 3. Forbid contraction in this file's code, and only there.

      Clang honours #pragma STDC FP_CONTRACT OFF at its default and at
      -ffp-contract=on: the blob becomes bit-identical to a -ffp-contract=off
      build (measured). Clang IGNORES it under -ffp-contract=fast, so a
      -ffp-contract=fast clang build must also pass -ffp-contract=off.

      GNU compilers ignore the standard pragma and contract by default in
      every GNU mode (-std=gnu17; the Arduino IDE's -std=gnu++2a) and in ISO
      C++; only ISO C (-std=c99) leaves contraction off. They do honour
      #pragma GCC optimize ("fp-contract=off"), even under -ffp-contract=fast:
      built with gcc-15 -O2 -ffp-contract=fast, the golden hash in
      tests/audit.c holds with it and moves without it (measured), and the
      ESP32-S3 compiler emits no fused instruction in this file with it and
      dozens without it (tests/pragma_leak.sh prints the count).

      BOTH ARE SCOPED TO THIS FILE. push_options saves GCC's optimisation
      settings, and pop_options, the last line of this file, restores them.
      On clang, float_control(push) saves the whole floating-point state and
      float_control(pop) at the end restores it. Just before that pop,
      STDC FP_CONTRACT DEFAULT puts contraction back to what the command
      line asks for, because clang honours float_control only on processors
      it supports strict floating point for (64-bit ARM, x86, RISC-V and
      PowerPC among them) and ignores it on the rest (32-bit ARM,
      WebAssembly, Xtensa and AVR among them, measured with clang 22), with
      a warning that the diagnostic lines around it keep out of your build.
      Code after the #include therefore compiles as it would without
      iris.h, contraction included where the compiler's default allows it,
      with one exception: on those other clang targets a contraction pragma
      of your own that comes BEFORE the #include gives way to the command
      line's setting, so put yours after it. tests/pragma_leak.sh checks this
      on the generated assembly: a*b+c in a function after the include still
      becomes a fused multiply-add, no iris function contains one, and on a
      laptop your own pragma before the include survives it.

      Two consequences. GCC does not inline a function that carries an
      optimize setting into a function whose settings differ, so when your
      code contracts, your calls into iris stay calls (measured with gcc-15);
      that is what keeps iris's arithmetic unfused inside your functions.
      And your own arithmetic is yours: a program that computes its
      demonstrations itself and needs the same bits from two compilers (a
      laptop and a board, say) switches contraction off in its own code too,
      as tests/audit.c does.                                                */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wignored-pragmas"
#pragma float_control(push)
#pragma clang diagnostic pop
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize ("fp-contract=off")
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
  IRIS_TRAINING_DIVERGED = 1,  /* a run pushed a weight or bias past
                                ±IRIS_W_LIMIT. The guard clamped it to exactly
                                the limit and stopped the run at that epoch.
                                The instrument is fitted and plays (the trainer
                                still reports a fit and iris_is_trained is 1),
                                but from weights that stopped where the guard
                                stopped them. Every trainer that continues from
                                the current weights now refuses: see
                                IRIS_DIVERGED_STUCK.                         */
  IRIS_NAN_TRAPPED       = 2,  /* a not-a-number or an infinity was caught by
                                the call that set this, and contained. For
                                example: a reading refused at iris_record's
                                door, a setting refused by a setter, a played
                                output replaced by the centre of the
                                demonstrated range, or a training run that met
                                one in the error or a weight partway through
                                and re-seeded to a finite start. The
                                backpropagation trainers and their diagnostics
                                (PART 8) do NOT report a stored demonstration
                                holding one here: they refuse it and change
                                nothing, this status included, and say so by
                                their return value.                          */
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
  IRIS_DIVERGED_STUCK    = 5,  /* a trainer that continues from the current
                                weights (iris_train_epochs, iris_train_converge
                                and the functions built on them) refused,
                                because a weight or bias sits exactly on
                                ±IRIS_W_LIMIT, where a divergence left it.
                                They refuse on EVERY call while that is true,
                                whatever else has touched the status since.
                                The way out is iris_train, which starts over
                                from the instrument's own seed (as does
                                iris_train_begin): the demonstrations are
                                intact, only the weights are damaged. That
                                fresh run is deterministic, so if the same
                                demonstrations diverge from the same seed
                                again, the weights are pinned again; a reroll
                                (iris_reseed with a new seed, then iris_train)
                                is a different run. This status means a
                                diverged gradient run and nothing else.      */
  IRIS_SOLVE_COLLAPSED   = 7   /* the closed-form solve (iris_train_elm)
                                produced a constant mapping: the fit is valid
                                and installed, but it ignores the inputs. On
                                every output the demonstrations changed, the
                                fitted outputs move less than half a percent
                                as far (PART 8d). A smaller lam0 brings the
                                mapping back. It locks nothing: the warm
                                trainers refuse only a diverged gradient run
                                (IRIS_DIVERGED_STUCK).                       */
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

/* THE WEIGHT LIMIT. A weight or bias past it means training is running away:
   the guard in PART 8 clamps it to exactly this value, reports
   IRIS_TRAINING_DIVERGED and stops the run, and a trainer that would continue
   from a weight sitting on it refuses with IRIS_DIVERGED_STUCK. iris_tanh is
   exactly ±1 beyond |s| = 3, so one weight of 16 on its own saturates its
   hidden unit whenever its input is more than 3/16 of the way from the centre
   of its range to either end.

   IT FIRES ON SOME GOOD FITS. Measured with iris_train at the defaults on
   2,304 fits (6 target shapes x 5, 10, 20, 50 demonstrations x noise 0, 0.05,
   0.10 x 32 seeds; 2 inputs, 12 hidden, 3 outputs): the healthy fits' largest
   weight has a median of 4.07, a 99th percentile of 14.4 and a maximum of
   15.9953, and the limit fired on 40, every one at 50 demonstrations and 39
   of them on sharp targets (cliffs and ridges). Those 40 are usable
   instruments, and stopping them helped: allowed to run on (a limit of 32 or
   64 gives the same runs; none passes 31.2) they train longer and end 7.6%
   worse on held-out error (geometric mean; 27 of the 40 are worse). Their
   status still says they diverged, and the warm trainers refuse them.

   WHY IT IS NOT RAISED. A higher limit clears those 40 and blinds the guard
   to real runaways. Same 2,304 datasets with the learning rate (lr) and
   momentum forced through iris_internal_set_learning; of the fits whose
   held-out error came out more than twice the default fit's, how many the
   guard reported:

                                  limit 16       limit 32       limit 64
     lr 2.0,  momentum 0.85     512 of 1,718    60 of 1,720     0 of 1,720
     lr 0.10, momentum 0.99     743 of 1,229   228 of 1,257     0 of 1,257
     lr 2.0,  momentum 0.99   1,970 of 1,970 1,967 of 1,978 1,579 of 1,992

   against 40, 0 and 0 reports on the default fits. 16 stays. The golden
   training hash in tests/audit.c is the same at all three limits: no healthy
   reference run comes near it.

   IT IS A DETECTOR FOR GRADIENT TRAINING, NOT A RULE ABOUT INSTRUMENTS. The
   closed-form trainer (PART 8d) solves the output layer directly, and its
   answer can lie beyond the limit without anything having run away: 155 of
   2,000 solves at 12 hidden units and lam0 1e-4 have an output weight past
   16, 132 of 2,000 at 24, the largest 42.6, and none of 2,000 at 48 hidden
   units and lam0 1e-3 (random sessions of 1 to 4 inputs and outputs and 5 to
   104 demonstrations of smooth targets with a little noise). Such an
   instrument plays, saves and loads like any other; a file needs its weights
   finite, nothing more (PART 9). What it cannot do is carry on with gradient
   training: a trainer that continues from the current weights
   (iris_train_epochs, iris_train_converge, iris_correct) clamps every weight
   past the limit in its first epoch and reports IRIS_TRAINING_DIVERGED, and
   from then on refuses with IRIS_DIVERGED_STUCK. iris_train, which starts
   over from the seed, is the way from the closed-form trainer to
   backpropagation (tests/elm.c checks all of this). */
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

/* THE SQUARE ROOT, in integers, correctly rounded.

   Four places take a square root: iris_reseed (the starting weight scales,
   1/sqrt(inputs) and 1/sqrt(hidden units)), iris_novelty (the distance it
   reports, and the sqrt(inputs) it divides that by), and the instant
   trainer (its gain, 2/sqrt(inputs), and the diagonal of every Cholesky
   step). A compiler's square root is one instruction on a laptop, but on the
   ESP32-S3 it is a call to the C library's sqrtf, and on GCC and Linux clang
   it also calls sqrtf for a negative input so that errno can be set. A call
   is an undefined symbol in a freestanding build, and it makes the answer
   belong to whichever C library is linked. This function needs nothing and
   gives the same bits on every target.

   THE METHOD is long-hand square root, the way it is taught with decimal
   digits, done in base 2. Write x = m * 2^p, with m the float's 24
   significant bits as a whole number. Then
   sqrt(x) = sqrt(m * 2^25) * 2^((p - 25) / 2), after moving one factor of 2
   from the power into m whenever p - 25 is odd. The bits of sqrt(m * 2^25)
   come out one at a time, from the top, 25 of them.
   With q the root found so far and b the next bit to try, setting b grows
   the square from q*q to (q+b)*(q+b), an increase of 2*q*b + b*b, so the bit
   is kept exactly when the remainder (the number minus q*q) can pay for it.
   The loop stores the remainder divided by b, which makes the price 2*q + b,
   and moving on to the next bit, half as big, doubles the stored remainder.
   Every quantity stays below 2^27, so 32-bit integers suffice.

   CORRECTLY ROUNDED means the answer is the float nearest the true root. 25
   bits come out: the 24 a float holds and one more, which says whether the
   true root lies above or below the point halfway to the next float. A
   nonzero remainder says something is left further down, so a 1 in that
   extra bit then means past halfway, and the root rounds up. It can never
   land exactly on halfway: that root, doubled, would be an odd whole number,
   and so would its square, but the number being rooted is m shifted left by
   25 places, which is even. IEEE 754 requires exactly this of a hardware
   square root, so the result is the hardware's, bit for bit, for every input
   that has a root, and for -0 and NaN: tools/sqrt_exhaustive.c compares all
   2^32 bit patterns against the host's sqrtf, and tests/portability.c
   re-checks ten million of them on every run.

   Special values follow IEEE 754: sqrt(+0) = +0, sqrt(-0) = -0, sqrt(+infinity)
   = +infinity, a NaN comes back as the same NaN made quiet, and any other
   negative input gives NaN. None of the four places passes a negative
   number: the Cholesky step checks that its diagonal is positive first, and
   the others take the root of a count or of a sum of squares.

   THE PRICE IS SPEED: about 45 nanoseconds a call on an Apple M4, where the
   instruction takes about 5. That adds about 45 nanoseconds to iris_novelty,
   60 to iris_reseed, and 0.6 microseconds to an instant-trainer fit of 20
   demonstrations with 12 hidden units (4.3 before, so 15%). It adds nothing
   to the neighbour searches (iris_knn_predict, iris_classify_1nn,
   iris_delete_nearest), which compare squared distances and never take a
   root. Measured with Apple clang -O2. */
IRIS_API float iris_sqrt(float x) {
  union { float f; uint32_t u; } v;
  v.f = x;
  const uint32_t u = v.u;
  if ((u & 0x7FFFFFFFu) > 0x7F800000u) { v.u = u | 0x00400000u; return v.f; }
  if (u == 0u || u == 0x80000000u || u == 0x7F800000u) return x;
  if (u & 0x80000000u) { v.u = 0x7FC00000u; return v.f; }

  int32_t e = (int32_t)(u >> 23);          /* x = m * 2^p with p = e - 150 */
  uint32_t m = u & 0x007FFFFFu;            /* the 23 stored bits */
  if (e == 0) {                            /* subnormal: bring the leading 1 up */
    e = 1;
    while (m < 0x00800000u) { m <<= 1; --e; }
  } else {
    m |= 0x00800000u;                      /* the leading 1 a float leaves implicit */
  }
  if (!(e & 1)) { m <<= 1; --e; }          /* p - 25 = e - 175 must be even */

  /* q = floor(sqrt(m * 2^25)): 25 bits, from bit 24 down to bit 0 */
  uint32_t rem = m << 1, q = 0u, b = 0x01000000u;
  while (b) {
    const uint32_t t = q + q + b;          /* (2*q*b + b*b) / b */
    if (rem >= t) { rem -= t; q += b; }
    rem <<= 1;
    b >>= 1;
  }
  q += q & (uint32_t)(rem != 0u);          /* round to nearest */
  /* q >> 1 keeps the leading 1 at bit 23, which adds one to the exponent
     field; a round-up that carries out of bit 23 adds one more, correctly */
  v.u = (q >> 1) + (((uint32_t)(e + 125) >> 1) << 23);
  return v.f;
}
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
     you train to convergence. A closed-form solve (PART 8d) has no epochs; it
     stores each example's squared miss under the solve, with res_epochs 1. */
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
   sigma=0.05). "Bricked" means the musician lowers it back and every trainer
   that continues from the current weights answers IRIS_DIVERGED_STUCK, on
   every call; only iris_train, which starts over from the seed, recovers.

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
  /* Identifiers are never reused, so they can run out: once next_id is the
     largest int32_t, handing it out and adding one would overflow. */
  if (k->next_id >= 0x7FFFFFFF) return 0;

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

/* The nearest-demonstration search, defined in PART 10 beside the neighbour
   functions that share it. */
IRIS_API int iris_internal_nearest(iris *k, const float *in);

/* Delete whichever example is closest to where you are standing right now.
   On a device with three buttons this is how you say "not THAT one" without
   needing to read a list.

   "Closest" is measured exactly as iris_classify_1nn measures it, with each
   input counted in fractions of its demonstrated range and the
   earliest-recorded demonstration winning a tie, so this deletes the one the
   classifier would name. In raw units a millimetre sensor would outvote a
   g-force sensor: with takes at (500 mm, -2 g) and (510 mm, +2 g) and the
   hand at (506 mm, -2 g), the raw squared distances are 36 and 32, which
   picks the second take, while in fractions of each range they are 0.36 and
   1.16 and the first take is the one you are standing on.

   Deletes nothing, and returns 0, when the store is empty, the reading is
   not finite, or the instrument's shape is too big for this translation
   unit (see iris_shape_fits). On an instrument that has never been fitted it
   fits the ranges first, as the neighbour functions do. */
/* LENGTHS, same rule as iris_predict and just as unchecked.
   Reads exactly n_in floats from `in`. */
IRIS_API int iris_delete_nearest(iris *k, const float *in) { if (!k) return 0;
  return iris_delete_index(k, iris_internal_nearest(k, in));
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

   WHERE THIS CONSTANT COMES FROM. 0.1/0.9 is a folklore rule of thumb, not a
   derived value. LeCun's Efficient BackProp section 4.5 derives the
   principled band from the maximum of the sigmoid's second derivative,
   which is 0.2113/0.7887, and docs/MATH-AUDIT.md:101 records that the shipped
   band therefore delivers 1.85x LESS gradient at the targets. It stays because
   moving it changes every frozen hash and every saved file's output mapping
   (docs/MATH-AUDIT.md:274 costs that out), not because it was measured to be
   better. It was not.
   ========================================================================== */

#define IRIS_OUT_LO 0.1f
#define IRIS_OUT_HI 0.9f

/* The largest finite float, which <float.h> calls FLT_MAX. This file includes
   no library headers, so it spells the number out. */
#define IRIS_FLT_MAX 3.40282347e+38f

/* The smallest and largest value that column c of the example store takes
   across the demonstrations (c counts the inputs first, then the outputs).

   The search starts from the largest finite float rather than from a big round
   number, so every finite value takes part however large it is; a not-a-number
   compares false with everything and never takes part. With no demonstrations
   the answer is lo > hi, and no caller uses it. */
IRIS_API void iris_internal_span(const iris *k, int c, float *lo, float *hi) {
  const int stride = k->n_in + k->n_out;
  float a = IRIS_FLT_MAX, b = -IRIS_FLT_MAX;
  for (int r = 0; r < k->n_ex; ++r) {
    const float v = k->ex[(size_t)r * stride + c];
    if (v < a) a = v;
    if (v > b) b = v;
  }
  *lo = a; *hi = b;
}

/* THE RANGES. Every input and every output gets the smallest and largest value
   the demonstrations gave it. Every trainer calls this before it starts; the
   neighbour functions (PART 10) and iris_delete_nearest call it on an
   instrument that has never been fitted. With no demonstrations it leaves the
   ranges as they are.

   AN INPUT THAT NEVER MOVED IS IGNORED. A switch left in one position, a
   sensor resting against its rail, a light sensor under steady light: an input
   that read the same in every demonstration tells the instrument nothing about
   what you want, and dividing by its width would turn the smallest wobble at
   play time into an enormous number. So the rule is:

       an input is STILL when its width, hi - lo, is at most 1e-5 of its
       magnitude (the larger of |lo| and |hi|), or at most 1e-6.

   A still input is stored with zero width (in_hi = in_lo), and iris_norm_in
   gives it the value 0 -- in training and in playing, whatever it reads, so
   moving it cannot change what the instrument plays. A float carries about
   seven significant digits, so 1e-5 of the magnitude is fewer than 170 steps
   of the float's own resolution: a range that narrow is rounding and sensor
   noise, not a gesture. The 1e-6 covers inputs resting near zero, where a
   relative test alone would demand an exact zero.

   Why ignore it rather than give it a small width. Dividing by a width that
   small magnifies any movement at play time: an input held at 500 in every
   demonstration and given a width of 0.005 normalises to about 400 when it
   reads 501, where the demonstrations taught the network only [-1,+1].
   Measured with such a floor in place, on the six demonstrations
   tests/playing.c uses with the still input at 500: a sweep of the other
   input produced an output span of 9.97 (of the 10 demonstrated) with the
   still input at 500, and 0.0000 with it at 501 -- every hidden unit
   saturated and the instrument became a constant.

   AN OUTPUT THAT NEVER MOVED keeps a small nonzero width, because the network
   is trained toward it and the output scaling divides by the width. That floor
   is relative for the reason above: an absolute 1e-6 added to a value above 32
   changes nothing in 32-bit floating point, because the gap between
   representable numbers there is already wider, so the width stayed zero and
   every prediction became not-a-number. Measured: an absolute floor worked up
   to 31.77 and failed from 32.72. */
IRIS_API void iris_fit_ranges(iris *k) { if (!k) return;
  if (k->n_ex == 0) return;
  for (int i = 0; i < k->n_in; ++i) {
    float lo, hi;
    iris_internal_span(k, i, &lo, &hi);
    const float mag = iris_absf(lo) > iris_absf(hi) ? iris_absf(lo) : iris_absf(hi);
    float negligible = mag * 1e-5f;
    if (negligible < 1e-6f) negligible = 1e-6f;
    k->in_lo[i] = lo;
    k->in_hi[i] = (hi - lo <= negligible) ? lo : hi;     /* still: zero width */
  }
  for (int o = 0; o < k->n_out; ++o) {
    float lo, hi;
    iris_internal_span(k, k->n_in + o, &lo, &hi);
    float w = iris_absf(lo) * 1e-5f;  if (w < 1e-6f) w = 1e-6f;
    if (hi - lo < w) hi = lo + w;
    k->out_lo[o] = lo; k->out_hi[o] = hi;
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
   root-mean-square error 0.0129 -> 0.0084 (1.54x).

   A STILL INPUT (zero width, see iris_fit_ranges) maps to 0 for every finite
   reading. It is written v - v rather than 0 so that a reading which is not
   finite -- a disconnected or broken sensor -- still comes out as
   not-a-number, and the guards downstream still report it. */
IRIS_API float iris_norm_in (const iris *k, int i, float v) { if (!k || i < 0 || i >= k->n_in) return 0.0f;
  const float w = k->in_hi[i] - k->in_lo[i];
  if (w <= 0.0f) return v - v;
  const float t = (v - k->in_lo[i]) / w;
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

   That is the network. It is NOT the whole of what iris_predict does; the full
   chain, which is what plays, is:

     x_i     = iris_norm_in(k, i, your_reading)     scale the sensor in
     ...the two lines above...
     out_o   = iris_denorm_out(k, o, output_o)      scale the sound out
     out_o   = iris_clampf(out_o, out_lo[o], out_hi[o])   and hold it in range

   Four steps, two of them arithmetic on ranges the instrument measured for
   itself, and two matrix multiplies with a squashing function after each.
   For 2 inputs, 12 hidden and 3 outputs that is 60 multiply-adds, and one
   whole prediction takes 14.9 microseconds on an ESP32-S3 (measured on the
   part; see the timing note above iris_get_status). Training the same
   instrument takes seconds, so playing is the cheap half.
   ========================================================================== */

/* The network alone, on inputs already normalised. It writes its working
   values -- the hidden and output activations -- into the instrument, which
   is why it takes a non-const instrument and why one instrument must not be
   played from two places at once. */
IRIS_API void iris_forward_norm(iris *k, const float *x_norm) { if (!k) return;
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

   Several functions below declare working arrays such as float
   x[IRIS_MAX_IN]. Those maxima are #ifndef so a small board can shrink them
   (see the note above them), and that is a per-TRANSLATION-UNIT setting:
   define IRIS_MAX_IN 4 in one .c file and not in another, and the two files
   disagree about how big those arrays are while sharing one instrument
   through a pointer.

   iris_init checks the shape against the maxima -- but it checks them in the
   translation unit that CALLS iris_init, which is the one with the large
   maxima, so it passes. The unit with the small maxima then writes n_in floats
   into its own float x[4]. Reproduced under AddressSanitizer:
   "stack-buffer-overflow, WRITE of size 4, [32,48) 'x.i'".

   So the playing functions, the neighbour search and the trainers ask this
   first. It is two comparisons and it turns a memory overwrite into an
   ordinary refusal. */
IRIS_API int iris_shape_fits(const iris *k) {
  return k && k->n_in <= IRIS_MAX_IN && k->n_out <= IRIS_MAX_OUT
           && k->n_hid <= IRIS_MAX_HID;
}

/* THE SUBSTITUTE a playing function writes when it cannot play: the
   instrument was never fitted, its shape does not fit this translation
   unit's working arrays, or the answer came out not-a-number. Writing
   something is the point -- `out` holds whatever the caller played last, and
   leaving it there is stale audio.

   It is the centre of output o's range: the range the instrument was fitted
   to, or, before its first fit, the range of the demonstrations it holds (so
   an instrument that has only been shown takes plays the middle of what it
   was shown), and 0 when it holds none. Written 0.5*lo + 0.5*hi rather than
   0.5*(lo + hi), which overflows when lo + hi passes the largest float. */
IRIS_API float iris_internal_centre(const iris *k, int o) {
  float lo = k->out_lo[o], hi = k->out_hi[o];
  if (!k->fitted) {
    if (k->n_ex == 0) return 0.0f;
    iris_internal_span(k, k->n_in + o, &lo, &hi);
  }
  return 0.5f * lo + 0.5f * hi;
}

/* THE PLAYING CALL. It writes the network's activations and, when it has
   something to report, the status inside the instrument; nothing else. */
IRIS_API void iris_predict(iris *k, const float *in, float *out) { if (!k) return;
  if (!iris_shape_fits(k)) {
    for (int o = 0; o < k->n_out; ++o) out[o] = iris_internal_centre(k, o);
    k->status = IRIS_NOT_FITTED;
    return;
  }
  float x[IRIS_MAX_IN];

#ifndef IRIS_NO_GUARDS
  /* PLAYING AN INSTRUMENT THAT WAS NEVER FITTED. Without this, the forward
     pass runs over the random weights iris_reseed drew and returns
     plausible-looking numbers with no symptom anywhere: no status, no return
     code, no silence. So it plays the substitute above instead and reports
     IRIS_NOT_FITTED.

     IT GUARDS ON `fitted`, NOT ON `trained`, AND THE DIFFERENCE MATTERS.
     iris_record and iris_delete clear `trained` -- the fit no longer reflects
     the current example set -- but the instrument is still a real instrument
     and must keep playing mid-performance (tests/playing.c holds that).
     `fitted` says "this has EVER produced a fit". It is cleared by
     iris_reseed and iris_clear, and by loading a file saved before any fit. */
  if (!k->fitted) {
    for (int o = 0; o < k->n_out; ++o) out[o] = iris_internal_centre(k, o);
    k->status = IRIS_NOT_FITTED;
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
       comparison with NaN is false), so a NaN here -- glitched sensor in,
       poisoned weight -- would land in an audio parameter. Substitute and say
       so. On a healthy run the bit test fails and this changes nothing. */
    if (iris_isbad(out[o])) {
      out[o] = iris_internal_centre(k, o);
      k->status = IRIS_NAN_TRAPPED;
    }
#endif
  }
}

/* ==========================================================================
   PART 7 — HOW LOST AM I?

   Distance from the current gesture to the nearest thing you demonstrated.
   0 means "exactly on an example".

   WHAT 1 MEANS, precisely, because the obvious reading is wrong. The distance
   is taken between normalised inputs, which run from -1 to +1 across each
   demonstrated range (PART 5), and divided by sqrt(n_in)/2 -- a constant that
   depends only on how many sensors you have, NOT on how far apart your
   demonstrations are. The normalised box has side 2 and diagonal
   2*sqrt(n_in), so 1 means "a quarter of that diagonal away from the nearest
   example": a quarter of every sensor's demonstrated range, in every sensor
   at once. That is a fixed distance, not a relative one. An input that never
   moved during the demonstrations adds nothing to the distance but still
   counts in n_in.

   The consequence is worth knowing before you map this to anything. With four
   corner demonstrations -- which is examples/00_minimal.c -- 60.7% of the
   gesture square reads exactly 1.0 (on a 1001 x 1001 grid of probes),
   including the middle of the demonstrated space; it reports the same value
   for "between your four takes" and "ten times outside them". With
   twenty-five demonstrations on a 5 x 5 grid it never exceeds 0.5, reached at
   the centre of each cell. The usable range of the control therefore depends
   on how many takes you recorded, and two instruments are not comparable.

   Making the scale relative to the examples' own spacing would fix that. It
   is deliberately NOT done here: tests/audit.c uses novelty to sort probes
   into near and far bands, so changing the scale moves measured thresholds
   elsewhere, and that deserves its own measurement rather than a quiet edit.

   This costs one pass over the examples. But it lets the instrument know when
   it is improvising rather than recalling, which you can map to anything you
   like: noise, detuning, a light.
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

/* IS ANY WEIGHT OR BIAS SITTING EXACTLY ON THE LIMIT? That is the mark a
   divergence leaves: iris_internal_check_weights writes exactly ±IRIS_W_LIMIT
   into every weight it clamps, and only another training run can move it
   from there. A healthy fit ends strictly inside the limit -- the largest
   weight in 2,264 healthy default fits was 15.9953 (see the note on
   IRIS_W_LIMIT) -- so the test is exact equality, not a band near the limit,
   which that fit would have fallen into. */
IRIS_API int iris_internal_pinned(const iris *k) {
  const int nw = k->n_hid * k->n_in + k->n_hid + k->n_out * k->n_hid + k->n_out;
  const float *w = k->w1;      /* w1,b1,w2,b2 again, walked as one block */
  for (int i = 0; i < nw; ++i)
    if (w[i] == IRIS_W_LIMIT || w[i] == -IRIS_W_LIMIT) return 1;
  return 0;
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
       training as iris_train in slices, so a single-threaded UI can draw a
       frame, read touch and keep the audio half alive between them. A sliced
       run is bit-identical to iris_train: iris_train_begin checks and
       reseeds exactly as iris_train does, the shuffle buffer is initialised
       once there and carried across slices, and so the random draws are the
       same draws in the same order.

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

/* CAN THIS STORE BE TRAINED ON? Every trainer and diagnostic asks this
   FIRST, before it reseeds, fits ranges, touches a progress counter or sets a
   status -- and it writes nothing itself. So a refusal leaves every byte of
   the instrument as it was, the instrument goes on playing exactly as before,
   and the refusal is reported by the return value alone.

   Three conditions. The shape fits this translation unit's working arrays
   (iris_shape_fits). There is at least one demonstration. And every stored
   number is finite: a not-a-number would poison every weight in the first
   epoch. iris_record already refuses one at the door, so that last test is
   defence in depth, for a store that arrived some other way -- a file written
   by a -DIRIS_NO_GUARDS build, say. The bad demonstration stays in the store
   where the musician can find it and delete it. The finiteness test is one of
   the guards, so a -DIRIS_NO_GUARDS build compiles it out, as it does
   iris_record's, and then nothing stops a not-a-number reaching the weights. */
IRIS_API int iris_internal_trainable(const iris *k) {
  if (!iris_shape_fits(k) || k->n_ex < 1) return 0;
#ifndef IRIS_NO_GUARDS
  {
    const int n = k->n_ex * (k->n_in + k->n_out);
    for (int i = 0; i < n; ++i) if (iris_isbad(k->ex[i])) return 0;
  }
#endif
  return 1;
}

/* Start a training session: the progress counters, the shuffle and the
   residual ledger. The blocking trainers start one inside the engine and
   iris_train_begin starts one for a sliced run, through this same function,
   so the two cannot begin differently. The session counts as busy while it
   runs, which is what iris_train_busy reports to a progress callback. */
IRIS_API void iris_internal_begin_session(iris *k, int ceiling) {
  k->tr_ceiling = ceiling;
  k->tr_done    = 0;
  k->tr_ref     = 0.0f;
  k->tr_running = 1;
  k->tr_n_ex    = k->n_ex;
  for (int i = 0; i < k->n_ex; ++i) k->order[i] = i;
  for (int i = 0; i < k->cap;  ++i) k->ex_res[i] = 0.0f;
  k->res_epochs = 0;
}

/* The one epoch engine. Every backprop entry point below is this function
   with a different stopping policy; there is no second copy of the update
   rule to drift out of sync.
     epochs  : the most this call may run. With resume = 0 it is also the
               session's ceiling, which a progress bar divides by.
     conv    : 0 = run the full budget, 1 = stop on the plateau test
     resume  : 0 = start a session (iris_internal_begin_session), run it and
                   end it -- a blocking call
               1 = continue the session already in k -- one slice of it
   Returns the last epoch's mean squared error, or -1 if it refused. */
/* REFUSAL CONVENTION (one convention, whole library): a train call that did
   no training returns -1.0f and leaves `trained` alone, so a caller reading
   only the return value can tell a refusal from a repeat of the previous
   run. */
IRIS_API float iris_internal_train_run(iris *k, int epochs, int conv, int resume,
                           iris_progress_fn cb, void *user) { if (!k) return -1.0f;
  /* Refuse before writing anything. epochs <= 0 is "do nothing", not "train
     instantly": without that test the loop below never runs, err stays 0, and
     the tail reports a freshly randomised network as trained with a perfect
     fit.

     ONE WRITE ON A SLICE'S REFUSAL, and it is the one that has to happen: a
     run in flight that finds nothing it can train on is over, however it got
     that way. The four delete functions empty a store as surely as iris_clear
     does, and ending the run HERE covers every caller instead of every caller
     having to remember. Without it the documented slice loop spins for ever
     with the progress bar frozen -- five million iterations and counting,
     measured. */
  if (epochs <= 0 || !iris_internal_trainable(k)) {
    if (resume) k->tr_running = 0;
    return -1.0f;
  }

#ifndef IRIS_NO_GUARDS
  /* THE DIVERGENCE TRAP, AND WHY THIS REFUSAL EXISTS.
     When a run diverges, iris_internal_check_weights clamps the offending
     weights to exactly ±IRIS_W_LIMIT and stops. A run that continues from
     those weights starts with them sitting on the clamp: epoch 1 pushes one of
     them past, the guard fires again, and training stops after a single epoch.
     Measured on the demonstrations of examples/02_fix_a_mistake.c: 14 good
     ones plus one contradictory take diverge and pin ONE weight of 60.
     Without this refusal, a warm run after the bad take is deleted does
     exactly 1 epoch per call, diverges again on it, and returns an
     ordinary-looking error each time, while the first output creeps 0.087,
     0.091, 0.106 over three calls against the 0.618 it played before the take.
     Zeroing the momentum does not help -- it is the pinned weight, not the
     velocity.

     The demonstrations are fine; the WEIGHTS are damaged. Refitting from the
     seed recovers the instrument: on the same demonstrations iris_train plays
     0.618 again, exactly what it played before the damage. So a run that
     would continue from pinned weights refuses, loudly and distinguishably,
     with IRIS_DIVERGED_STUCK, rather than pretending to train. It does NOT
     reseed on its own: a warm trainer is
     asked to keep the performer's weights, and replacing them silently would
     hand the performer a different instrument, which is the failure mode
     Fiebrink & Sonami describe.

     THE WEIGHTS DECIDE, NOT THE STATUS (iris_internal_pinned). So the refusal
     holds on every call for as long as a weight sits on the limit: a refusal
     moves no weight, so the next call refuses too; zeroing the velocity, as
     iris_correct does before it trains, moves no weight either; and a status
     overwritten by an unrelated call -- a not-a-number refused at
     iris_record's door, say -- cannot let a warm run through. The way out is
     a run that does not continue from these weights: iris_train and
     iris_train_begin reseed from the instrument's own seed before their first
     epoch, so they never meet this test with pinned weights.

     Every entry reaches this test, slices included, but a sliced run starts
     from iris_train_begin's reseed and ends itself if it diverges, so in
     practice what it refuses are the warm trainers: iris_train_epochs,
     iris_train_converge and the functions built on them. The one write is the
     status, plus ending the run if a slice is refused. */
  if (iris_internal_pinned(k)) {
    k->status = IRIS_DIVERGED_STUCK;
    if (resume) k->tr_running = 0;
    return -1.0f;
  }
  k->status = IRIS_STATUS_OK;
#endif
  /* Ranges are fitted only now, after every refusal: they are part of the
     playing instrument (denormalisation reads them on every predict), so a
     refused train must not have moved them. */
  iris_fit_ranges(k);

  const int stride = k->n_in + k->n_out;
  const int NI = k->n_in, NH = k->n_hid, NOUT = k->n_out;
  float x[IRIS_MAX_IN], t[IRIS_MAX_OUT];
  float err = 0.0f;

  if (!resume) {
    iris_internal_begin_session(k, epochs);
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
    /* THE ERROR FLOOR — a fourth stopping rule, and at small demonstration
       counts the one most likely to be what actually stopped you.

       WHAT 1e-6 IS. `err` is the mean squared error over every demonstration
       and output, in the network's own output units, where each output's
       demonstrated range spans the 0.8-wide band [0.1, 0.9]. So the floor is
       a root-mean-square miss of 0.001 in those units: 0.125% of each
       output's demonstrated range. It is ABSOLUTE -- the same number whatever
       the data, the noise or the number of demonstrations -- and it was
       chosen as "close enough to a perfect fit", not derived from anything.

       WHEN IT FIRES. A network with a few dozen weights can fit a handful of
       demonstrations exactly, noise and all, so with few takes the error
       falls through the floor before the plateau test ever looks. Measured,
       iris_train on 2 inputs -> 12 hidden -> 3 outputs, six target shapes x 40
       seeds per cell (240 runs), stopped here in:
           demonstrations       4     5     8    10    12    20    50
           clean              84%   80%   61%   52%   43%   18%   16%
           noise sigma 0.05   84%   85%   88%   79%   65%    3%    0%
           noise sigma 0.10   78%   84%   91%   85%   78%    5%    0%
       Every other run stopped on the plateau test, except 57 of the 5,040
       that the divergence guard stopped and 1 that reached the ceiling.

       It sits outside the `conv` guard on purpose (a perfect fit is a reason
       to stop on any path), so it also fires on the fixed-epoch path. Ask
       iris_train_epochs_done() how many epochs actually ran; if it is below
       what you asked for and no guard fired, this is why. */
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
  if (!resume) k->tr_running = 0;     /* a blocking run is over when it returns */
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
   Do not "improve" it; iris_train_converge is where improvements go.

   It continues from the current weights, and refuses (-1) the way
   iris_train_converge does. */
IRIS_API float iris_train_epochs(iris *k, int epochs) { if (!k) return -1.0f;
  return iris_internal_train_run(k, epochs, 0, 0, 0, 0);
}

/* Train until the training error plateaus. ceiling <= 0 takes
   IRIS_CONV_CEILING. cb may be NULL. Returns the final mean squared error,
   or -1 if it refused.

   It continues from the current weights. A store it cannot train on is
   refused with nothing written. While a weight sits exactly on
   ±IRIS_W_LIMIT it refuses with IRIS_DIVERGED_STUCK, on every call, and
   that status is then the one thing it writes; iris_train is the way out. */
IRIS_API float iris_train_converge(iris *k, int ceiling, iris_progress_fn cb, void *user) { if (!k) return -1.0f;
  return iris_internal_train_run(k, ceiling > 0 ? ceiling : IRIS_CONV_CEILING, 1, 0, cb, user);
}

/* WHAT iris_train DOES BEFORE ITS FIRST EPOCH, in this order, and
   iris_train_begin does exactly the same -- which is what makes a sliced run
   bit-identical to the blocking one:

     1. refuse a store it cannot train on (iris_internal_trainable), having
        written nothing;
     2. reseed from the instrument's own seed, so the fit starts from the
        weights that seed draws, whatever training happened before.

   The order is the whole point. Reseeding first would throw the playing
   instrument away and THEN refuse, leaving the musician with neither. */
IRIS_API int iris_internal_cold_start(iris *k) {
  if (!iris_internal_trainable(k)) return 0;
  iris_reseed(k, k->seed);
  return 1;
}

/* TRAIN. This is the one to call.

   It fits the demonstrations you have now, from a defined start: it checks
   them, then reseeds from the instrument's own seed, then trains until the
   error stops improving. No epoch count to guess, no callback, no ceiling.

   WHEN IT STOPS. Whichever of these comes first:
     - the plateau test: every IRIS_CONV_WINDOW (2,000) epochs it compares the
       error with the error one window earlier, and stops when the window
       bought less than IRIS_CONV_TOL (10%) of it;
     - the ceiling, IRIS_CONV_CEILING (60,000 epochs; 30,000 where int is 16
       bits);
     - the error floor, see the note at `err < 1e-6f` in the engine: an
       ABSOLUTE floor on the mean squared error, not a relative one, and at
       small demonstration counts it, not the plateau test, is usually what
       stops the run;
     - the divergence guard: a weight past ±IRIS_W_LIMIT is clamped and the
       run ends there with status IRIS_TRAINING_DIVERGED.
   iris_train_epochs_done tells you how many epochs actually ran.

   Returns 1 if it fitted, 0 if it refused. It refuses -- and then changes
   nothing at all, not the weights, not the ranges, not the status -- when
   there are no demonstrations, when one of them holds a not-a-number or an
   infinity, when the instrument is null, or when its shape is too big for
   this translation unit's working arrays. A run the divergence guard stopped
   still returns 1: the instrument was fitted, and the status says how. If
   you want to know HOW WELL it fits, that is a separate question with a
   separate answer: iris_last_error(k).

   IT NEVER REFUSES A STUCK INSTRUMENT, and that is deliberate: starting over
   from the seed is the way out of IRIS_DIVERGED_STUCK, whose weights are the
   only damaged part. */
IRIS_API int iris_train(iris *k) {
  if (!k) return 0;
  /* FIT FROM A DEFINED START, always.

     Continuing from whatever weights are already there breaks the loop this
     library exists for. Record a bad take, delete it, retrain -- the
     documented repair -- and a warm start keeps the deleted take's crater,
     because the weights it bent are the weights training resumes from.
     Measured over 40 seeds: the places you did NOT demonstrate came back 215
     times further from the mapping you showed it, in every single run, while
     iris_last_error moved the other way and the status reported perfect
     health. The one number a screen can show said the instrument had
     improved.

     Warm-starting has its use, argued above iris_correct: continuing from the
     current fit adjusts one region without rewriting the mapping everywhere,
     which is how a musician keeps technique. But that is what the warm
     trainers are for. This function is called train, a caller expects it to
     fit the demonstrations it has now, and the two must not be the same act. */
  if (!iris_internal_cold_start(k)) return 0;

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
  /* ASK THE FLAG, NOT THE SIGN. A run that trapped a not-a-number partway
     leaves a non-negative error behind while never fitting (it re-seeds to a
     finite start and clears `trained`), so the sign of the error alone would
     return 1 with iris_is_trained 0 and status 2 -- exactly what Rule 1
     promises cannot happen. k->trained is set by the run itself and is the
     same answer iris_is_trained gives every other caller. */
  { float e = iris_train_converge(k, 0, 0, 0);
    return (e >= 0.0f && k->trained) ? 1 : 0; }
}


/* The same run, in slices, for a UI that must keep drawing.
     iris_train_begin(k, 0);
     while (iris_train_slice(k, 500)) { draw(iris_train_progress(k)); poll(); }

   BIT-IDENTICAL TO iris_train, for any slice sizes. iris_train_begin does
   exactly what iris_train does before its first epoch (iris_internal_cold_start:
   refuse an untrainable store having written nothing, then reseed from the
   instrument's own seed) and starts the session through the same function the
   engine uses. The shuffle buffer and the plateau reference then carry across
   the slices, so the random draws are the same draws in the same order and
   every byte of the arena ends the same. tests/train.c checks that over
   several shapes, seeds and slice sizes, including after deletes.

   ceiling <= 0 takes IRIS_CONV_CEILING, which is what iris_train uses; any
   other ceiling gives the run iris_train would make with that ceiling. Returns
   1 if the run started, 0 if it refused -- for the same reasons, and with the
   same guarantee that nothing changed, as iris_train. */
IRIS_API int iris_train_begin(iris *k, int ceiling) { if (!k) return 0;
  if (!iris_internal_cold_start(k)) return 0;
  iris_internal_begin_session(k, ceiling > 0 ? ceiling : IRIS_CONV_CEILING);
  return 1;
}

/* Runs at most `epochs` more. Returns 1 if there is more to do, 0 when the
   run has finished (plateau, ceiling, early stop, or a guard) -- including
   when the store can no longer be trained on, emptied by deletes or holding a
   not-a-number, which ends the run and writes nothing else. */
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
   that — the plateau test, the error floor (an absolute 1e-6 on the mean
   squared error; see the note in the engine), the divergence guard, or a
   progress callback returning 0. iris_get_status() distinguishes the guard;
   this distinguishes "ran to completion" from "stopped for a good reason",
   which iris_train_progress() deliberately cannot, because it reports 1.0 for
   any finished run. A refused call leaves it where it was. */
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

/* The demonstrated range of output j across the first n stored rows, in
   double so that the difference of two finite floats cannot overflow. */
IRIS_API double iris_internal_out_span(const iris *k, int n, int j) {
  const int stride = k->n_in + k->n_out;
  float lo = k->ex[k->n_in + j], hi = lo;
  for (int r = 1; r < n; ++r) {
    const float v = k->ex[(size_t)r * stride + k->n_in + j];
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  return (double)hi - (double)lo;
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
   example was hidden. epochs <= 0 takes 600. Returns mean squared error per
   output, in the demonstrations' own units, or -1 if it refused: fewer than 3
   demonstrations to fold over, or a store no trainer would accept (see
   iris_internal_trainable). It checks that BEFORE the first fold reseeds
   anything, so a refusal changes nothing and never returns a not-a-number.

   THE INSTRUMENT IS LEFT REFITTED ON ALL EXAMPLES, from that same seed, so it
   is valid to play afterwards — but it is NOT the instrument you had before you
   called this, because it has been retrained. Save first if that matters. */

/* The sweep itself, shared by iris_loo_error and iris_suggest_smoothing.
   per_range = 0 sums each miss in the demonstrations' own units, which is
   what iris_loo_error reports. per_range = 1 first divides each output's miss
   by that output's demonstrated range across all n demonstrations, so no
   output outweighs another because of the units it was recorded in; an output
   whose demonstrations never moved has no range, carries no evidence about
   smoothing, and is left out. */
IRIS_API float iris_internal_loo(iris *k, int epochs, int per_range) {
  if (!k || k->n_ex < 3 || !iris_internal_trainable(k)) return -1.0f;
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
      if (per_range) {
        const double span = iris_internal_out_span(k, n, j);
        if (span <= 0.0) continue;
        e /= span;
      }
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

IRIS_API float iris_loo_error(iris *k, int epochs) { return iris_internal_loo(k, epochs, 0); }

/* SUGGEST A SMOOTHING VALUE — an explicit, occasional act, not an automatic one.

   Runs leave-one-out (above) at each of five smoothing settings -- 0, 0.05,
   0.15, 0.5 and 1 -- and returns the one that scored best, or -1 if it
   refused. IT DOES NOT APPLY IT. You get the number, you decide.

   WHAT IT SCORES IS A PROXY. Every fold is a fixed 600-epoch fit from the
   instrument's seed, NOT the plateau run iris_train makes and you then play:
   a plateau fit costs about 25 times more, averaging 14,000 to 15,600 epochs
   at 20 demonstrations over the runs behind the error-floor table in the
   engine (clean to noise sigma 0.10). The proxy has a price. Measured on 36
   datasets (six target shapes, three noise levels, two draws of 20
   demonstrations) with 8 rerolls each, against held-out error on a clean
   grid: the pick was the best of the five settings for the 600-epoch fit it
   scores 41% of the time, and for the plateau-trained instrument 35% of the
   time; following it cost a geometric-mean 6.6% over the best setting for
   the 600-epoch fit, and 16.2% for the plateau-trained instrument. So it is a
   noisy selector even for the model it scores, and the budget mismatch more
   than doubles what following it costs.

   UNITS DO NOT MATTER. Each output's miss on the hidden demonstration is
   divided by that output's demonstrated range before it is squared, so an
   output recorded in thousands counts the same as one recorded in fractions:
   "Units: none" from the front page, applied here. iris_loo_error itself
   reports raw units. An output whose demonstrations never moved carries no
   evidence about smoothing and is left out.

   WHY IT IS NOT AUTOMATIC — this was tested as an automatic default and it
   failed the bar set for it. On the same 36 datasets with 16 rerolls each:

     - IT IS NOT STABLE. Rerolling the same demonstrations changed its answer:
       2.17 distinct values per dataset on average. An instrument whose
       smoothing changes when you reroll is an instrument that stops being
       predictable, which is worse than one that is merely unsmoothed.
     - IT IS SOMETIMES WORSE THAN DOING NOTHING. 10.9% of its picks gave the
       plateau-trained instrument a worse held-out error than smoothing 0.
     - IT IS SLOW. Five sweeps of n + 1 fits: 104 ms at 20 demonstrations and
       640 ms at 50 on an Apple M4 Max (2 inputs, 12 hidden, 3 outputs). On
       the ESP32-S3, scaling the one measured on-device figure (321 ms for a
       600-epoch fit at 20 demonstrations) in proportion to demonstrations
       times epochs gives about 32 s at 20 demonstrations and 200 s at 50 --
       an estimate, not a board reading. That is not something to hide inside
       a training call.

   WHAT IT IS GOOD FOR: an honest starting point when you genuinely do not know,
   on a machine where 100 ms is nothing. It captured 81% of what always picking
   the best setting would have gained. Treat the number as a suggestion to
   audition, not an answer — and if you like where you land, pin it in your
   code rather than re-deriving it, so your instrument stays put.

   ASKING FOR ADVICE MUST NOT COST YOU YOUR INSTRUMENT. Every fold refits the
   network, so before the first one this copies every byte the instrument
   owns -- weights, momentum velocities, ranges, demonstrations, the residual
   ledger, the random state, the progress counters, the status, `fitted` and
   `trained` -- into your scratch, and copies it all back afterwards. A stale
   instrument that is still playing goes on playing, and a warm trainer
   called afterwards continues exactly as it would have. The arena is exactly
   sized and has nowhere to keep that copy, hence the scratch: give it at
   least IRIS_ARENA(n_in, n_hid, n_out, cap) bytes -- the size of the
   instrument's own arena, which iris_size returns at run time -- at any
   alignment, not overlapping the instrument. It refuses otherwise, and on
   anything iris_loo_error refuses, having written nothing: refusing an
   answer is recoverable, and quietly replacing someone's instrument is not.

   It still suggests; it still does not decide. Applying the number is yours. */
IRIS_API float iris_suggest_smoothing(iris *k, void *scratch, size_t scratch_bytes) {
  if (!k) return -1.0f;
  if (k->n_ex < 3 || !iris_internal_trainable(k)) return -1.0f;   /* what the sweep refuses */
  {
    /* The instrument is every byte from its structure to the end of order[],
       the last array iris_init carves. That span is always smaller than the
       arena iris_size asks for, which also holds the alignment slack. */
    unsigned char *inst = (unsigned char *)k, *copy = (unsigned char *)scratch;
    const size_t span = (size_t)((unsigned char *)(k->order + k->cap) - inst);
    const size_t need = iris_size(k->n_in, k->n_hid, k->n_out, k->cap);
    const float ladder[5] = { 0.0f, 0.05f, 0.15f, 0.5f, 1.0f };
    float best_v = 0.0f, best_e = 0.0f;
    /* need == 0 first: size_t is unsigned, so `scratch_bytes < 0` would wave
       any buffer through (the sentinel trap described above iris_size). */
    if (!copy || need == 0 || scratch_bytes < need) return -1.0f;
    if ((uintptr_t)copy < (uintptr_t)inst + span
        && (uintptr_t)inst < (uintptr_t)copy + span) return -1.0f;   /* overlaps */

    for (size_t i = 0; i < span; ++i) copy[i] = inst[i];
    for (int i = 0; i < 5; ++i) {
      float e;
      iris_set_smoothing(k, ladder[i]);
      e = iris_internal_loo(k, 0, 1);
      if (i == 0 || e < best_e) { best_e = e; best_v = ladder[i]; }
    }
    for (size_t i = 0; i < span; ++i) inst[i] = copy[i];
    return best_v;
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

   The fastest trainer in this file. On a laptop, at 50 demonstrations, two
   inputs and nh=12, a solve takes about 8 microseconds where 600 epochs of
   backprop take 2.5 ms -- roughly 300x (tests/audit.c prints the table as
   TRAINING COST). It has not been timed on the ESP32-S3. The ratio shrinks as
   inputs are added, because the frozen layer is redrawn for every
   demonstration (see NOTHING CHANGES UNLESS THE SOLVE WORKS, below).

   The trick is to stop training half the network. Draw the hidden layer once
   from the seed and FREEZE it; the output layer is then a linear least-squares
   problem with an exact closed-form answer — one (nh+1)x(nh+1) Cholesky solve,
   no epochs, no iteration. This idea has a name in the literature — extreme
   learning machine, ELM — and a 20-year argument about whether it deserves
   one; it is here because it is measured to work here.

   Two measurements make it work in float32 on this network:

   GAIN. The backprop starting weights (1/sqrt(n_in)) rely on training to grow.
   Frozen at that scale, tanh of a normalised input barely bends -- the random
   features are nearly collinear and the normal matrix is numerically
   rank-deficient. The frozen layer is drawn at 2/sqrt(n_in) instead, wide
   enough that the features have real capacity. docs/gain-sweep.c measures the
   choice in one command: mean held-out error over 4 target shapes x 16 seeds x
   {8,20,50} demonstrations x nh {12,24,48}, gain = M/sqrt(n_in):

       M         0.25   0.50   1.00   1.50   2.00   3.00   4.00   8.00
       error    .1143  .1025  .0946  .0919  .0905  .0894  .0920  .1087

   M=2 beats the backprop starting scale M=1 by 4.3%. The minimum is BROAD and
   M=2 sits inside it. M=3 is marginally better (1.2%) and the optimum drifts
   upward with width -- best at 1.5 for nh=12 and at 3.0 for nh=48 -- so 2 is a
   good constant rather than the best one, and it stays because moving it would
   move every frozen hash in the audit for a 1.2% gain.

   RIDGE, MANDATORY. Even with the wider gain, the unridged float32 normal
   matrix fails Cholesky in EVERY realistic scenario measured -- including 20
   well-spread examples. The ridge is relative (lam0 * trace/(nh+1), so it
   scales with the data) plus a floor of 1e-7, and it escalates
   deterministically: double it on a failed factorisation, at most 8 times, and
   report the count. If escalation was needed the status says
   IRIS_RIDGE_ESCALATED -- the result is valid, the data was harder than usual.
   The campaign behind that: docs/adr/0009-ridge-is-mandatory.md. Escalation
   can still run out: lam0 = 0 on 128 demonstrations all made at one gesture
   fails all nine attempts (40 seeds of 40, at 8, 12 and 24 hidden units), and
   that is an ordinary refusal (below).

   SMOOTHING reaches this trainer as extra ridge on the output weights, and
   never on the output biases: a penalised bias drags every output toward the
   middle of its range, where an unpenalised one lets heavy smoothing settle
   near the average of what was demonstrated (averaged in logit units).
   Smoothing s is stored as weight decay 0.3 s (PART 3); the solve adds four
   times that, 1.2 s, to the diagonal entry of every output weight, on top of
   the relative ridge above. At smoothing 0 the added term is exactly zero, so
   the solve is bit-for-bit the unsmoothed one. The extra ridge is absolute,
   not relative to the data: like backprop's weight decay, its pull stays
   fixed while the evidence grows with every demonstration, so smoothing
   matters most with few takes.

   THE FOUR IS MEASURED, NOT DERIVED. Carrying backprop's weight decay into
   logit units at the sigmoid's steepest slope (0.25) gives sixteen. With
   sixteen, the best setting for noisy demonstrations sat between 0.05 and 0.2
   and smoothing 1 cost clean demonstrations 25% at 20 takes; with four, the
   best setting sits between 0.1 and 1 and smoothing 1 costs clean ones 8%.
   The table is `tests/elm.c measure` (the figures for sixteen come from the
   same program with the constant edited): nh 12, lam0 1e-4, 4 target shapes
   x 16 seeds, outputs spanning about 0..1. Each cell is two root-mean-square
   errors: at the demonstrations, then on fresh points against the clean
   target.

       demos noise   smoothing 0     0.1             0.3             1.0
         8   0.10    .0260 .2123     .0698 .1339     .0793 .1283     .0909 .1264
        20   0.00    .0309 .0747     .0456 .0718     .0513 .0747     .0592 .0804
        20   0.05    .0507 .0907     .0644 .0800     .0693 .0812     .0762 .0851
        20   0.10    .0834 .1239     .0984 .0980     .1031 .0954     .1094 .0954
        50   0.00    .0430 .0529     .0516 .0586     .0556 .0619     .0614 .0671
        50   0.05    .0659 .0607     .0736 .0645     .0770 .0676     .0818 .0723

   Recall error rises at every step, which is what smoothing promises. On 8 or
   20 noisy demonstrations the held-out error at 0.3 falls by 10% to 40%, most
   with the fewest and noisiest takes; on 20 clean ones 0.3 costs nothing. On
   50, where least squares already averages the noise away, smoothing only
   adds bias: 0.3 costs 11% on lightly noisy takes and 17% on clean ones. The
   default stays 0, for the reason PART 3 gives.

   NOTHING CHANGES UNLESS THE SOLVE WORKS. Every refusal -- a bad argument, too
   little scratch, a poisoned demonstration, or a factorisation that fails even
   after escalation -- leaves every byte of the instrument as it was, status
   included; the return value is the whole report. That takes some care,
   because a solve needs the new ranges and the new frozen layer before it can
   know whether it will succeed:
     - the solve works in the caller's scratch, never in the weight arrays;
     - the frozen layer is a pure function of the seed, so it is redrawn for
       each demonstration one hidden unit at a time rather than stored, and
       written into the instrument only once the solve has succeeded;
     - the ranges are fitted in place, because every normalisation in this
       file reads them from the instrument, and the old ones wait in scratch
       and are put back byte for byte if the solve fails.
   A solution containing a non-finite number is a failed solve too: it can only
   come from demonstrations so far apart that their span overflows a float.

   WHICH DEMONSTRATION IS FIGHTING (PART 8f). A solve has no epochs to sum
   over, so it leaves each demonstration's squared miss under the solve in the
   ledger, in the same normalised units, with res_epochs 1; whatever an earlier
   backprop run left there is replaced. PART 8f explains why the endpoint
   residual is worthless after backprop: given enough epochs the optimiser
   bends onto the bad take. The ridged solve has only nh+1 numbers per output
   to bend with, so the endpoint still carries the signal -- less of it than the
   integrated ledger does. `tests/elm.c measure`, on the protocol of
   docs/adr/0019-the-residual-ledger-integrates-it-does-not-sample.md (nh 12,
   8 outputs, one take offset on one output; hits of 20 sessions, and clean
   sessions whose margin reaches IRIS_STRESS_FLAG, of 200):

                       demos   +0.05   +0.10   +0.20   +0.40   clean >= flag
       iris_train_elm    20    13/20   15/20   14/20   15/20      25/200
                         50    15/20   19/20   20/20   20/20      14/200
                        100    14/20   19/20   20/20   20/20      24/200
       iris_train        20    18/20   19/20   20/20   20/20       7/200
                         50    20/20   20/20   20/20   19/20      15/200
                        100    20/20   20/20   20/20   20/20       6/200

   So after this trainer the flag speaks on 7-13% of clean sessions, about
   twice as often as after backprop: treat a flagged take as one to listen to
   again, not one to delete unheard.

   The solved instrument is an ordinary iris instrument: same w1/b1/w2/b2
   arrays, same iris_predict, saves and loads as a normal file. Its output
   weights can lie beyond IRIS_W_LIMIT, which is legitimate here; the note at
   IRIS_W_LIMIT says what that means for gradient training afterwards. The solve
   targets logit space -- the exact inverse of our sigmoid -- so the shipping
   forward pass lands on the normalised targets. That makes it a
   bounded-output VARIANT of the backprop head, not an equivalent.

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

   Determinism: the hidden layer is drawn from k->seed by a LOCAL random
   number generator (k->rng is never touched -- a closed-form solve is not an
   event in the correction history), accumulation order is fixed by example
   order, and the escalation schedule is fixed. Same seed + same examples =>
   bit-identical weights, verified at nh 12/24/48.
   ========================================================================== */

/* The solve's working memory, in bytes, for n_hid = NH and n_out = NO.

   It is sized by IRIS_MAX_IN rather than by n_in because this macro is not
   given n_in: the ranges kept in case the solve fails, and one hidden unit's
   frozen weights, each hold up to one float per input. The last
   sizeof(float) - 1 bytes let the buffer start at any address: the solve
   rounds the pointer up to a float boundary itself, so a plain
   `static unsigned char scratch[IRIS_ELM_SCRATCH(12, 3)]` is fine. */
#define IRIS_ELM_SCRATCH(NH, NO)                                                 \
  ( sizeof(float) * ( (size_t)((NH)+1) * ((NH)+1) /* normal matrix, then factor */\
                    + (size_t)((NH)+1) * (NO)     /* right side, then solution */ \
                    + (size_t)((NH)+1)            /* its diagonal, kept        */ \
                    + (size_t)2 * (IRIS_MAX_IN + (NO)) /* ranges, kept         */ \
                    + (size_t)IRIS_MAX_IN )       /* one frozen hidden unit    */ \
    + sizeof(float) - 1 )                         /* alignment padding         */

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

/* INTERNAL: the solve itself, with the hidden-layer gains as arguments. Use
   iris_train_elm, which passes the measured gains; this form exists for
   iris_train_elm and for the gain measurement in docs/gain-sweep.c.

   Returns the number of ridge doublings used (0 = first try; status
   IRIS_RIDGE_ESCALATED if > 0), or -1 refusing, with nothing changed. It
   refuses: a null instrument or scratch, no demonstrations, a shape this
   translation unit cannot hold, nh < 8, scratch smaller than
   IRIS_ELM_SCRATCH(nh, n_out), a lam0 or gain that is negative or not finite,
   a poisoned (not-a-number or infinite) demonstration, and a solve that fails
   even after escalation.

   Zero is legal for all three numbers, and means:
     lam0   = 0  no ridge in proportion to the data; only the 1e-7 floor, which
                 escalation may still double. Near-duplicate demonstrations
                 can then fail all nine attempts, which is a refusal.
     gain_w = 0  the hidden units ignore the inputs, so every demonstration
                 sees the same features and the solve can only return a
                 constant. The collapse check below reports that.
     gain_b = 0  every hidden unit's boundary passes through the centre of the
                 demonstrated input range. A narrower family, still a mapping. */
IRIS_API int iris_train_elm_ex(iris *k, float lam0, float gain_w, float gain_b,
                               void *scratch, size_t scratch_bytes) {
  /* --- refuse before touching anything ----------------------------------- */
  if (!k || !scratch || k->n_ex == 0) return -1;
  if (!iris_shape_fits(k) || k->n_hid < 8) return -1;  /* nh < 8: the reroll floor */
  if (iris_isbad(lam0)   || !(lam0   >= 0.0f)) return -1;  /* !(x >= 0) is also */
  if (iris_isbad(gain_w) || !(gain_w >= 0.0f)) return -1;  /* true for NaN      */
  if (iris_isbad(gain_b) || !(gain_b >= 0.0f)) return -1;
  if (scratch_bytes < IRIS_ELM_SCRATCH(k->n_hid, k->n_out)) return -1;
#ifndef IRIS_NO_GUARDS
  /* a poisoned demonstration is refused here, before anything is written,
     and stays in the store where the musician can find it and delete it */
  for (int i = 0; i < k->n_ex * (k->n_in + k->n_out); ++i)
    if (iris_isbad(k->ex[i])) return -1;
#endif

  const int NI_ = k->n_in, NH_ = k->n_hid, NO_ = k->n_out, K = NH_ + 1;
  const int stride = NI_ + NO_;
  const uint32_t seed = k->seed ? k->seed : 1u;

  /* --- carve the scratch, starting at the first float boundary ------------ */
  unsigned char *p = (unsigned char *)scratch;
  p += ((uintptr_t)p & (sizeof(float) - 1u))
         ? sizeof(float) - (size_t)((uintptr_t)p & (sizeof(float) - 1u)) : 0u;
  float *A    = (float *)p;               /* K x K: normal matrix, then factor */
  float *B    = A + (size_t)K * K;        /* K x NO: right side, then solution */
  float *dg   = B + (size_t)K * NO_;      /* K: the unridged diagonal          */
  float *keep = dg + K;                   /* 2(NI+NO): the ranges as they were */
  float *wj   = keep + 2 * (NI_ + NO_);   /* NI: one frozen hidden unit        */

  for (int i = 0; i < NI_; ++i) { keep[i] = k->in_lo[i]; keep[NI_ + i] = k->in_hi[i]; }
  for (int o = 0; o < NO_; ++o) { keep[2*NI_ + o] = k->out_lo[o];
                                  keep[2*NI_ + NO_ + o] = k->out_hi[o]; }
  iris_fit_ranges(k);

  /* --- accumulate the normal equations: A = H^T H (upper), B = H^T Z,
     where H is the hidden activations plus a bias column and Z is the
     logit of the normalised targets. One pass over the examples; the frozen
     layer is redrawn from the seed for each one, in the order it will be
     stored: unit by unit, its n_in weights and then its bias. ------------- */
  float x[IRIS_MAX_IN];                  /* one normalised demonstration */
  for (int i = 0; i < K * K; ++i)   A[i] = 0.0f;
  for (int i = 0; i < K * NO_; ++i) B[i] = 0.0f;
  {
    float h[IRIS_MAX_HID + 1];
    for (int n = 0; n < k->n_ex; ++n) {
      const float *row = k->ex + (size_t)n * stride;
      for (int i = 0; i < NI_; ++i) x[i] = iris_norm_in(k, i, row[i]);
      iris_rng r = { seed };
      for (int j = 0; j < NH_; ++j) {
        for (int i = 0; i < NI_; ++i) wj[i] = iris_rand_sym(&r) * gain_w;
        /* the bias is drawn after the unit's weights, but the sum starts
           from it, as the forward pass's does (PART 6) */
        float s = iris_rand_sym(&r) * gain_b;
        for (int i = 0; i < NI_; ++i) s += wj[i] * x[i];
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
  const float mu = k->l2 * 4.0f;   /* the smoothing ridge, weights only */

  int doublings = -1;
  for (int att = 0; att <= 8; ++att) {
    for (int i = 0; i < K; ++i) {                      /* restore + ridge */
      float *Ai = A + (size_t)i * K;
      for (int j = 0; j < i; ++j) Ai[j] = A[(size_t)j * K + i];
      Ai[i] = dg[i] + lam + (i < NH_ ? mu : 0.0f);
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

  /* --- back-substitute B in place: L y = B, then L^T beta = y ------------ */
  if (doublings >= 0) {
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
    for (int i = 0; i < K * NO_; ++i) if (iris_isbad(B[i])) doublings = -1;
  }

  /* --- a failed solve puts the ranges back and changes nothing else ------- */
  if (doublings < 0) {
    for (int i = 0; i < NI_; ++i) { k->in_lo[i] = keep[i]; k->in_hi[i] = keep[NI_ + i]; }
    for (int o = 0; o < NO_; ++o) { k->out_lo[o] = keep[2*NI_ + o];
                                    k->out_hi[o] = keep[2*NI_ + NO_ + o]; }
    return -1;
  }

  /* --- commit: the frozen layer exactly as the solve drew it, the solved
     output layer, and no velocity -- velocities are backprop state that no
     longer describes this instrument ------------------------------------ */
  {
    iris_rng r = { seed };
    for (int j = 0; j < NH_; ++j) {
      float *w = k->w1 + (size_t)j * NI_;
      for (int i = 0; i < NI_; ++i) w[i] = iris_rand_sym(&r) * gain_w;
      k->b1[j] = iris_rand_sym(&r) * gain_b;
    }
  }
  for (int o = 0; o < NO_; ++o) {
    float *w = k->w2 + (size_t)o * NH_;
    for (int j = 0; j < NH_; ++j) w[j] = B[(size_t)j * NO_ + o];
    k->b2[o] = B[(size_t)NH_ * NO_ + o];
  }
  iris_zero_velocity(k);
  k->tr_running = 0;         /* end any sliced run: its next slice would
                                 otherwise go on training over the solve */
  k->trained = 1;
  k->fitted  = 1;                    /* a closed-form solve IS a fit */
  k->status  = doublings > 0 ? IRIS_RIDGE_ESCALATED : IRIS_STATUS_OK;

  /* --- recall error (in the units iris_train_epochs reports), the ledger,
     and how far the demonstrations and the fitted outputs move ------------ */
  {
    float err = 0.0f;
    float lo_y[IRIS_MAX_OUT], hi_y[IRIS_MAX_OUT];     /* fitted, normalised */
    for (int o = 0; o < NO_; ++o) { lo_y[o] = 1e30f; hi_y[o] = -1e30f; }
    for (int i = 0; i < k->cap; ++i) k->ex_res[i] = 0.0f;
    for (int n = 0; n < k->n_ex; ++n) {
      const float *row = k->ex + (size_t)n * stride;
      float rse = 0.0f;
      for (int i = 0; i < NI_; ++i) x[i] = iris_norm_in(k, i, row[i]);
      iris_forward_norm(k, x);
      for (int o = 0; o < NO_; ++o) {
        const float y = k->out[o], t = iris_norm_out(k, o, row[NI_ + o]);
        const float e = y - t;
        err += e * e;
        rse += e * e;
        if (y < lo_y[o]) lo_y[o] = y;
        if (y > hi_y[o]) hi_y[o] = y;
      }
      k->ex_res[n] = rse;
    }
    k->res_epochs = 1;
    k->last_error = err / (float)(k->n_ex * NO_);

#ifndef IRIS_NO_GUARDS
    /* DID IT ACTUALLY LEARN A MAPPING? A large enough lam0 -- or a zero
       gain_w -- drives every output weight toward nothing, and the solve then
       maps every gesture to the same sound. That is not a failed solve by any
       numerical test: no value is bad and the ridge did what it was asked.
       So ask the only question that matters to a musician: do the gestures
       they demonstrated still sound different? Everything is compared in
       normalised units, so the answer does not depend on the caller's units.

       Only ask when there was a mapping to learn: some input moved AND some
       output changed across the demonstrations. When every demonstration
       carried the same sound, or every take was made at the same gesture, a
       constant is the right answer, and flagging it would report correct
       behaviour as a fault. Both questions are asked of the demonstrations
       themselves, not of the ranges, which iris_fit_ranges widens for a
       constant output. An input that never moved reads 0 in every
       demonstration (PART 5), so it never counts as moving, and an instrument
       whose inputs all stood still is never reported.

       Collapsed means: on every output the demonstrations changed, the fitted
       outputs move less than half a percent as far as the demonstrations did
       -- a constant with rounding on it. The status says IRIS_SOLVE_COLLAPSED,
       a report of its own: the solve is still installed, a smaller lam0, or a
       larger gain_w, brings the mapping back, and no trainer refuses because
       of it. */
    {
      int moved = 0, changed = 0, collapsed;
      for (int n = 1; n < k->n_ex && !moved; ++n)
        for (int i = 0; i < NI_; ++i)
          if (iris_norm_in(k, i, k->ex[(size_t)n * stride + i])
              != iris_norm_in(k, i, k->ex[i])) moved = 1;
      collapsed = moved;
      for (int o = 0; o < NO_; ++o) {
        float lo_t = 1e30f, hi_t = -1e30f;           /* demonstrated, normalised */
        for (int n = 0; n < k->n_ex; ++n) {
          const float t = iris_norm_out(k, o, k->ex[(size_t)n * stride + NI_ + o]);
          if (t < lo_t) lo_t = t;
          if (t > hi_t) hi_t = t;
        }
        if (hi_t > lo_t) {
          changed = 1;
          if (hi_y[o] - lo_y[o] >= 0.005f * (hi_t - lo_t)) collapsed = 0;
        }
      }
      if (collapsed && changed) k->status = IRIS_SOLVE_COLLAPSED;
    }
#endif
  }
  return doublings;
}

/* The instant trainer with the measured-default gains: 2/sqrt(n_in) for
   weights AND biases. lam0 = 1e-4 is the nh=12 default; 1e-3 at nh=48.
   The scratch is at least IRIS_ELM_SCRATCH(n_hid, n_out) bytes, at any
   alignment. Returns the number of ridge doublings, or -1 having changed
   nothing; see iris_train_elm_ex above for every refusal and for what a
   lam0 of 0 means. */
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
   The file carries the weights AND the demonstrations, so whoever receives it
   can keep working rather than inheriting a sealed box.

   THE FILE, format version 7. Every number is little-endian and is written
   and read one byte at a time, so a file means the same thing on every
   machine and the buffer you hand over needs no particular alignment. A float
   travels as its 32-bit IEEE-754 bit pattern. In the type column, u32 is an
   unsigned 32-bit integer, i32 a signed one, and f32 a float.

     offset  field                                type     rule on load
     ------  -----------------------------------  -------  -------------------------
          0  magic: the letters I R I S           4 bytes  exact
          4  format version: 7                    u32      exact
          8  header bytes: 48, the offset of w1   u32      exact
         12  flags: bit 0 fitted, bit 1 trained   u32      no other bit set; trained
                                                           only together with fitted
         16  n_in, n_hid, n_out                   3 x u32  the receiving instrument's
                                                           shape, and iris_shape_fits
         28  n_ex, demonstrations stored          u32      at most the receiver's
                                                           capacity
         32  seed                                 u32      not 0
         36  next_id, the next identifier         u32      1 <= next_id < 2^31 - 1
         40  random-number state                  u32      not 0
         44  smoothing                            f32      finite, 0 to 1
         48  w1, b1, w2, b2                       f32s     finite
          .  in_lo, in_hi                         f32s     finite, lo <= hi, and
                                                           hi - lo finite
          .  out_lo, out_hi                       f32s     finite, lo < hi, and
                                                           hi - lo finite
          .  demonstrations, each one n_in        f32s     finite
             inputs then n_out outputs
          .  identifiers, one per demonstration   i32s     1 or more, all different,
                                                           each below next_id
      end-4  checksum of every byte before it     u32      exact
             (CRC-32, see iris_crc32)

   And the length: exactly what the header says, not a byte more or less.

   WHAT THE RULES ARE FOR. The checksum catches accidents -- a flipped bit in
   flash, a write cut off by a power failure -- but anyone can recompute it,
   so a file written by a buggy program, or edited by hand, arrives with a
   good checksum and whatever it happens to contain. The rules catch that
   second kind. Each one refuses a value that would go wrong later if it were
   let in:

     - The shape must match because the weights only mean something at the
       size they were trained at. iris_shape_fits is asked too, because the
       loader's own working array is sized by this translation unit's
       IRIS_MAX_IN (see the note above iris_shape_fits).
     - Unsigned throughout: a count with its top bit set is a large number
       that fails "at most the capacity", not a negative one that passes it.
     - next_id stays below 2^31 - 1, the largest int32_t, so a loaded
       instrument has at least one identifier left to hand out. iris_record
       hands out next_id and adds one, and refuses once next_id reaches the
       largest int32_t, so the count never overflows.
     - A weight need only be finite. IRIS_W_LIMIT is backpropagation's
       detector for a runaway run, not a rule about valid instruments: the
       closed-form trainer (PART 8d) legitimately solves output weights beyond
       it (see the note at IRIS_W_LIMIT).
     - An input range may have zero width, an output range may not. An
       input that never moved during the demonstrations is stored with
       in_hi = in_lo, and iris_norm_in reads it as 0 (PART 5), so that is a
       range this library writes. An output that never moved is given a small
       width of its own by iris_fit_ranges, because the output scaling in
       PART 5 divides by the width, so a file carrying an output range of
       zero width did not come from this library. Either way lo must not pass
       hi, and the width must be a finite number.
     - An identifier that repeats would make "delete #3" ambiguous, and one
       at or above next_id would be handed out again by the next record.

   iris_load CHECKS EVERY RULE BEFORE IT WRITES ANYTHING. A refused file
   returns 0 and leaves the instrument you passed exactly as it was, every
   byte of it, its status included. There is no half-loaded instrument.

   AFTER A LOAD the instrument is the saved one, and it is at rest: fitted and
   trained come from the flags, the momentum velocities are zero, the record
   of which demonstration fights the others (PART 8f) is empty, any sliced
   training run is over, the status is IRIS_STATUS_OK, and iris_last_error is
   measured afresh over the demonstrations.

   WHY FITTED AND TRAINED ARE TWO BITS. `fitted` means this instrument has
   produced a fit and plays it; `trained` means that fit still describes the
   demonstrations stored with it. Record one more take after training and the
   instrument is fitted but not trained: it keeps playing. iris_save writes
   the two separately, so after a save and a load it still plays, the same
   bits as before, and iris_is_trained still says 0.

   THE SMOOTHING FIELD is the setting, 0 to 1, as iris_get_smoothing reports
   it, and loading applies it exactly as iris_set_smoothing does. That round
   trip is exact for every setting iris_set_smoothing can produce: checked
   over all 1,065,353,217 floats from 0 to 1, the weight decay after a save
   and a load equals the one before, bit for bit.

   iris_save WRITES ONLY FILES iris_load ACCEPTS. It checks its own output
   against the same rules and returns 0 -- clearing what it wrote -- if the
   instrument breaks one. A finite instrument that this library made always
   saves. What can make it refuse: a weight or a demonstration that is not
   finite, which only an IRIS_NO_GUARDS build lets into the instrument;
   demonstrations spread so far apart that a range's width overflows a float;
   and more than two thousand million recorded takes. Refusing at save time
   tells you while the instrument is still in front of you, rather than after
   a power cycle.

   ONE THING THE CHECKSUM CANNOT DO. It certifies the bytes that were
   written, not that they all came from the same instant. If the instrument
   changes while iris_save is copying it -- a second thread, an interrupt,
   the sliced trainer driven from a timer -- the buffer holds a mixture of two
   instruments, and the checksum, computed over the mixture, matches it by
   construction. If every value in the mixture obeys the rules, it loads. One
   instrument belongs to one thread (see THREADING in PART 2); do not save an
   instrument that something else may be touching.
   ========================================================================== */

#define IRIS_FILE_MAGIC   "IRIS"   /* the first four bytes of every file       */
#define IRIS_FILE_VERSION 7u       /* the one format this file reads and writes */
#define IRIS_FILE_HEADER  48u      /* bytes before w1: the fixed fields above   */

/* CRC-32, the 32-bit cyclic redundancy check of IEEE 802.3 (the one zip and
   Ethernet use), computed a bit at a time so there is no 1 KB table to carry
   onto a microcontroller. It runs only on save and load, never while
   playing. Measured on the development laptop (Apple M4 Max, cc -O2): 5.5
   microseconds for the 872-byte file of a 2-12-3 instrument with 20
   demonstrations, about 6.5 nanoseconds a byte.

   WHY A SAVED INSTRUMENT NEEDS ONE. The file is mostly weights, raw floats
   with no redundancy. Flip one bit in flash and every field may still obey
   every rule above: the instrument loads and plays something subtly wrong
   with nothing reporting anything, and the musician assumes they mis-trained
   it. The checksum turns that into a refusal. */
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

/* LITTLE-ENDIAN, ONE BYTE AT A TIME. Casting the caller's buffer to a
   uint32_t or float pointer instead would demand 4-byte alignment the buffer
   need not have -- undefined behaviour in C, and a fault on microcontrollers
   whose loads must be aligned -- and would write the host's byte order into
   the file. Shifts and masks give the same bytes on every machine. */
IRIS_API void iris_internal_put_u32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v & 0xFFu);
  p[1] = (unsigned char)((v >> 8) & 0xFFu);
  p[2] = (unsigned char)((v >> 16) & 0xFFu);
  p[3] = (unsigned char)((v >> 24) & 0xFFu);
}
IRIS_API uint32_t iris_internal_get_u32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
       | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
/* A float's bit pattern, through a union: reading the member that was not
   written last reinterprets the same four bytes (C99 6.5.2.3; the GCC manual
   allows it in C++ too, under -fstrict-aliasing), with no library call. The
   bits are copied, not converted, so -0 and not-a-number arrive as they left. */
IRIS_API void iris_internal_put_f32(unsigned char *p, float f) {
  union { float f; uint32_t u; } c;
  c.f = f;
  iris_internal_put_u32(p, c.u);
}
IRIS_API float iris_internal_get_f32(const unsigned char *p) {
  union { float f; uint32_t u; } c;
  c.u = iris_internal_get_u32(p);
  return c.f;
}
/* A run of n floats; each returns the position just past what it moved. */
IRIS_API unsigned char *iris_internal_put_f32s(unsigned char *p, const float *src, size_t n) {
  for (size_t i = 0; i < n; ++i, p += 4) iris_internal_put_f32(p, src[i]);
  return p;
}
IRIS_API const unsigned char *iris_internal_get_f32s(const unsigned char *p, float *dst, size_t n) {
  for (size_t i = 0; i < n; ++i, p += 4) dst[i] = iris_internal_get_f32(p);
  return p;
}

/* The exact length of a file of this instrument's shape holding n_ex
   demonstrations. Called only with n_ex at most the capacity, and the arena
   already holds every one of these floats (and more), so the sum fits a
   size_t on any machine the instrument exists on. */
IRIS_API size_t iris_internal_file_bytes(const iris *k, uint32_t n_ex) {
  const size_t floats = (size_t)k->n_hid * k->n_in + k->n_hid
                      + (size_t)k->n_out * k->n_hid + k->n_out
                      + 2u * ((size_t)k->n_in + k->n_out)
                      + (size_t)n_ex * ((size_t)k->n_in + k->n_out);
  return IRIS_FILE_HEADER + 4u * (floats + (size_t)n_ex) + 4u;
}

/* One stored range: both ends finite, lo below hi -- or equal to it when
   zero_width_ok is set, which the input ranges are (see the table above) --
   and a width that is itself a finite number. Two tests are enough for all of
   that: the comparison is false when either end is not-a-number, and hi - lo
   is infinite or not-a-number whenever an end that passed it is infinite. */
IRIS_API int iris_internal_range_ok(float lo, float hi, int zero_width_ok) {
  return (zero_width_ok ? lo <= hi : lo < hi) && !iris_isbad(hi - lo);
}

/* EVERY RULE IN THE TABLE ABOVE, reading the file and writing nothing. It
   answers one question -- would this file load into k? -- and two callers ask
   it: iris_load, before it touches the instrument, and iris_save, of the bytes
   it has just written. One copy of the rules, so the writer and the reader
   cannot come to disagree about what a valid file is. */
IRIS_API int iris_internal_file_ok(const iris *k, const unsigned char *b, size_t bytes) {
  const size_t ni = (size_t)k->n_in, nh = (size_t)k->n_hid, no = (size_t)k->n_out;
  const unsigned char *p;
  uint32_t flags, n_ex, next_id, top = 0u;
  size_t i, j;
  if (bytes < IRIS_FILE_HEADER + 4u) return 0;
  for (i = 0; i < 4; ++i) if (b[i] != (unsigned char)IRIS_FILE_MAGIC[i]) return 0;
  if (iris_internal_get_u32(b + 4) != IRIS_FILE_VERSION) return 0;
  if (iris_internal_get_u32(b + 8) != IRIS_FILE_HEADER)  return 0;
  flags = iris_internal_get_u32(b + 12);
  if (flags & ~3u)  return 0;                  /* a bit this reader does not know */
  if (flags == 2u)  return 0;                  /* trained, but never fitted       */
  if (iris_internal_get_u32(b + 16) != (uint32_t)ni
   || iris_internal_get_u32(b + 20) != (uint32_t)nh
   || iris_internal_get_u32(b + 24) != (uint32_t)no) return 0;
  n_ex = iris_internal_get_u32(b + 28);
  if (n_ex > (uint32_t)k->cap) return 0;
  if (bytes != iris_internal_file_bytes(k, n_ex)) return 0;
  if (iris_crc32(b, bytes - 4u) != iris_internal_get_u32(b + bytes - 4u)) return 0;
  if (iris_internal_get_u32(b + 32) == 0u) return 0;                   /* seed */
  next_id = iris_internal_get_u32(b + 36);
  if (next_id < 1u || next_id >= 0x7FFFFFFFu) return 0;
  if (iris_internal_get_u32(b + 40) == 0u) return 0;       /* random state */
  { const float s = iris_internal_get_f32(b + 44);
    if (iris_isbad(s) || s < 0.0f || s > 1.0f) return 0; }

  p = b + IRIS_FILE_HEADER;
  for (i = 0; i < nh * ni + nh + no * nh + no; ++i, p += 4)
    if (iris_isbad(iris_internal_get_f32(p))) return 0;     /* finite, no more */
  for (i = 0; i < ni; ++i)                       /* in_lo[i] against in_hi[i] */
    if (!iris_internal_range_ok(iris_internal_get_f32(p + 4 * i),
                                iris_internal_get_f32(p + 4 * (ni + i)), 1)) return 0;
  p += 8 * ni;
  for (i = 0; i < no; ++i)                     /* out_lo[i] against out_hi[i] */
    if (!iris_internal_range_ok(iris_internal_get_f32(p + 4 * i),
                                iris_internal_get_f32(p + 4 * (no + i)), 0)) return 0;
  p += 8 * no;
  for (i = 0; i < (size_t)n_ex * (ni + no); ++i, p += 4)
    if (iris_isbad(iris_internal_get_f32(p))) return 0;

  /* The identifiers. The store keeps them in the order they were handed out,
     so each one is normally larger than every one before it and so cannot
     repeat any of them; only an identifier that is not needs the search back
     through the others. */
  for (i = 0; i < (size_t)n_ex; ++i) {
    const uint32_t id = iris_internal_get_u32(p + 4 * i);
    if (id < 1u || id >= next_id) return 0;
    if (id <= top)
      for (j = 0; j < i; ++j) if (iris_internal_get_u32(p + 4 * j) == id) return 0;
    if (id > top) top = id;
  }
  return 1;
}

/* Bytes iris_save needs for this instrument as it stands: exactly the length
   it writes, and the only length iris_load accepts for it. */
IRIS_API size_t iris_save_size(const iris *k) { if (!k) return 0;
  return iris_internal_file_bytes(k, (uint32_t)k->n_ex);
}

/* Writes the instrument into buf, in the order of the table above. Returns the
   number of bytes written, or 0 if buf is missing or smaller than
   iris_save_size(k), or if the instrument breaks a rule (see "iris_save
   WRITES ONLY FILES iris_load ACCEPTS" above; buf is then cleared). */
IRIS_API size_t iris_save(const iris *k, void *buf, size_t cap) {
  unsigned char *b = (unsigned char *)buf, *p;
  size_t need, i;
  if (!k || !b) return 0;
  need = iris_save_size(k);
  if (cap < need) return 0;
  for (i = 0; i < 4; ++i) b[i] = (unsigned char)IRIS_FILE_MAGIC[i];
  iris_internal_put_u32(b + 4,  IRIS_FILE_VERSION);
  iris_internal_put_u32(b + 8,  IRIS_FILE_HEADER);
  iris_internal_put_u32(b + 12, (k->fitted ? 1u : 0u) | (k->trained ? 2u : 0u));
  iris_internal_put_u32(b + 16, (uint32_t)k->n_in);
  iris_internal_put_u32(b + 20, (uint32_t)k->n_hid);
  iris_internal_put_u32(b + 24, (uint32_t)k->n_out);
  iris_internal_put_u32(b + 28, (uint32_t)k->n_ex);
  iris_internal_put_u32(b + 32, k->seed);
  iris_internal_put_u32(b + 36, (uint32_t)k->next_id);
  iris_internal_put_u32(b + 40, k->rng.s);   /* so the next training step after
                                                a load is the one this instrument
                                                would have taken, not a fork */
  iris_internal_put_f32(b + 44, iris_get_smoothing(k));
  p = b + IRIS_FILE_HEADER;
  p = iris_internal_put_f32s(p, k->w1, (size_t)k->n_hid * k->n_in);
  p = iris_internal_put_f32s(p, k->b1, (size_t)k->n_hid);
  p = iris_internal_put_f32s(p, k->w2, (size_t)k->n_out * k->n_hid);
  p = iris_internal_put_f32s(p, k->b2, (size_t)k->n_out);
  p = iris_internal_put_f32s(p, k->in_lo,  (size_t)k->n_in);
  p = iris_internal_put_f32s(p, k->in_hi,  (size_t)k->n_in);
  p = iris_internal_put_f32s(p, k->out_lo, (size_t)k->n_out);
  p = iris_internal_put_f32s(p, k->out_hi, (size_t)k->n_out);
  p = iris_internal_put_f32s(p, k->ex, (size_t)k->n_ex * ((size_t)k->n_in + k->n_out));
  for (i = 0; i < (size_t)k->n_ex; ++i, p += 4) iris_internal_put_u32(p, (uint32_t)k->ex_id[i]);
  iris_internal_put_u32(p, iris_crc32(b, need - 4u));
  if (!iris_internal_file_ok(k, b, need)) {
    for (i = 0; i < need; ++i) b[i] = 0;
    return 0;
  }
  return need;
}

/* Reads a file written by iris_save into k, an instrument of the same shape.
   Returns 1, or 0 with k untouched. */
IRIS_API int iris_load(iris *k, const void *buf, size_t bytes) {
  const unsigned char *b = (const unsigned char *)buf, *p;
  if (!k || !b) return 0;
  if (!iris_shape_fits(k)) return 0;
  if (!iris_internal_file_ok(k, b, bytes)) return 0;

  /* EVERY RULE HAS PASSED. Nothing from here on can refuse, so nothing from
     here on can leave the instrument half-loaded. */
  { const uint32_t flags = iris_internal_get_u32(b + 12);
    k->n_ex    = (int32_t)iris_internal_get_u32(b + 28);
    k->seed    = iris_internal_get_u32(b + 32);
    k->next_id = (int32_t)iris_internal_get_u32(b + 36);
    k->rng.s   = iris_internal_get_u32(b + 40);
    iris_set_smoothing(k, iris_internal_get_f32(b + 44));
    p = b + IRIS_FILE_HEADER;
    p = iris_internal_get_f32s(p, k->w1, (size_t)k->n_hid * k->n_in);
    p = iris_internal_get_f32s(p, k->b1, (size_t)k->n_hid);
    p = iris_internal_get_f32s(p, k->w2, (size_t)k->n_out * k->n_hid);
    p = iris_internal_get_f32s(p, k->b2, (size_t)k->n_out);
    p = iris_internal_get_f32s(p, k->in_lo,  (size_t)k->n_in);
    p = iris_internal_get_f32s(p, k->in_hi,  (size_t)k->n_in);
    p = iris_internal_get_f32s(p, k->out_lo, (size_t)k->n_out);
    p = iris_internal_get_f32s(p, k->out_hi, (size_t)k->n_out);
    p = iris_internal_get_f32s(p, k->ex, (size_t)k->n_ex * ((size_t)k->n_in + k->n_out));
    for (int i = 0; i < k->n_ex; ++i, p += 4) k->ex_id[i] = (int32_t)iris_internal_get_u32(p);
    k->fitted  = (flags & 1u) ? 1 : 0;
    k->trained = (flags & 2u) ? 1 : 0;
  }

  /* At rest: nothing carried over from whatever this arena held before. */
  iris_zero_velocity(k);
  for (int i = 0; i < k->cap; ++i) k->ex_res[i] = 0.0f;
  k->res_epochs = 0;
  k->tr_done = 0; k->tr_ceiling = 0; k->tr_running = 0; k->tr_n_ex = 0; k->tr_ref = 0.0f;
  k->status = IRIS_STATUS_OK;

  /* The training error is not in the file, but the fit and the demonstrations
     are, so it is measured: one forward pass per demonstration, in the units
     every trainer reports. With no fit, or nothing to measure it over, it is
     1, the value a fresh instrument reports. It is 1 as well if the sum is not
     a number, which takes a demonstration so far outside the stored ranges
     that normalising it overflows a float. */
  k->last_error = 1.0f;
  if (k->fitted && k->n_ex > 0) {
    const int stride = k->n_in + k->n_out;
    float xn[IRIS_MAX_IN], e = 0.0f;
    for (int r = 0; r < k->n_ex; ++r) {
      const float *row = k->ex + (size_t)r * stride;
      for (int i = 0; i < k->n_in; ++i) xn[i] = iris_norm_in(k, i, row[i]);
      iris_forward_norm(k, xn);
      for (int o = 0; o < k->n_out; ++o) {
        const float d = k->out[o] - iris_norm_out(k, o, row[k->n_in + o]);
        e += d * d;
      }
    }
    e /= (float)(k->n_ex * k->n_out);
    if (!iris_isbad(e)) k->last_error = e;
  }
  return 1;
}

/* ==========================================================================
   PART 10 — THE SECOND ALGORITHM  (k-NN blending and 1-NN snapping)

   k-NN is "k nearest neighbours": answer a gesture by finding the k
   demonstrations nearest to it and blending what they said. 1-NN is the
   case k = 1, snapping to the single nearest one. The network of PARTS 6
   and 8, a multilayer perceptron, is called the MLP below.

   A different character of instrument, not a quality tier. Desktop
   Wekinator ships k-NN as its default for discrete (classifier) outputs.
   These two functions play both modes with ZERO training, ZERO seed, and
   ZERO extra arena bytes: they are a weighted read of the example store the
   instrument already carries. The examples ARE the model — the design rule
   of this whole file, taken to its logical end.

   Semantics, stated as design and not as apology:

     - THIS ALGORITHM DOES NOT REROLL. There is no seed and nothing random;
       the same examples always give the same instrument, bit for bit. It
       is sampler-like where the MLP is morph-like: it plays back and
       blends your demonstrations.

     - RECALL. Standing on a demonstration returns that demonstration: to
       the bit from iris_classify_1nn, and to within rounding from
       iris_knn_predict, where the demonstration you stand on carries a
       weight of about 1e9 against 1/d^2 for each other neighbour (the
       exact-recall check in tests/audit.c measures a worst error of 6e-8 on
       outputs between 0 and 1). Only another demonstration almost on top of
       it pulls the answer measurably away. The MLP does not quite get there:
       trained to its plateau it misses its own demonstrations by a
       root-mean-square 0.0012 on outputs whose demonstrated ranges are 0.45
       to 0.8 wide, about 0.2% of the range (the convergence check in
       tests/audit.c).

     - SEAMS, ON PURPOSE. Between two demos the output can step 31x more
       sharply than its mean step, where the MLP's morph steps 1.9x
       (measured in the experiment behind docs/adr/0010). That is the
       sampler character, documented, not hidden.

     - STRUCTURAL SAFETY. The answer is a weighted average of demonstrated
       outputs, held inside their range: it is never a not-a-number and
       never leaves the range you demonstrated, whatever the input does.
       When every neighbour carries the same value, the answer is exactly
       that value.

     - THE HONEST FLOOR. The MLP generalises better at EVERY example count
       measured (2.4x at 5 examples, 2.1x at 200; docs/adr/0010). Choose
       k-NN for its character, never for accuracy. For discrete outputs use
       iris_classify_1nn, which returns a stored label verbatim: a blend of
       labels is exactly a label only where all k neighbours agree, and a
       value between labels where they do not.

   Distances count each input in fractions of its demonstrated range, so a
   millimetre sensor and a g-force sensor count equally, and an input that
   never moved counts not at all (PART 5). The ranges are the instrument's
   own: the ones it was last fitted to, or, on an instrument that has never
   been fitted, the current demonstrations' ranges, fitted on every call. A
   fitted instrument keeps its ranges when you record or delete; train again
   to measure in the new ones. (iris_fit_ranges alone would do it too, but it
   would also change what iris_predict plays, because the network was
   trained on the old ranges.) Ties resolve to the earliest-recorded example,
   the same rule as Weka's LinearNNSearch, the engine under desktop
   Wekinator's classifier — so decisions are comparable ("Wekinator-compatible
   semantics"; the desktop Java binary itself has not been run against this
   code, and the label stays this honest until it has).

   Every saved instrument can play this way with nothing added to its file:
   the examples and ranges are already in it. The file does not record which
   algorithm you play it with; that is a choice made at run time.
   ========================================================================== */

#define IRIS_KNN_MAXK 8          /* stack bound; k above this is clamped */
#define IRIS_KNN_GUARD 1e-9f     /* zero-distance guard for the weights */

/* THE NEIGHBOUR SCALE: one multiplier per input, 1/width of its range, so a
   distance counts each input in fractions of its demonstrated range. A still
   input (zero width, PART 5) gets 0 and adds nothing to any distance, except
   that a reading which is not finite still makes the distance not-a-number,
   which the callers report. Taking the reciprocals once keeps divisions out of
   the scan. */
IRIS_API void iris_internal_neighbour_scale(const iris *k, float *inv) {
  for (int i = 0; i < k->n_in; ++i) {
    const float w = k->in_hi[i] - k->in_lo[i];
    inv[i] = (w <= 0.0f) ? 0.0f : 1.0f / w;
  }
}

/* The squared distance from `in` to one stored row, every input counted in
   fractions of its range by the scale above. */
IRIS_API float iris_internal_distance2(const iris *k, const float *inv,
                                       const float *row, const float *in) {
  float d = 0.0f;
  for (int i = 0; i < k->n_in; ++i) {
    const float t = (row[i] - in[i]) * inv[i];
    d += t * t;
  }
  return d;
}

/* THE NEAREST DEMONSTRATION: the index of the stored row closest to `in`, the
   earliest-recorded on a tie (strict <, the Weka rule), or -1 when there is
   none -- an empty store, a shape too big for this translation unit, or a
   query whose distance to every row is not finite (a not-a-number reading,
   or one so far out that its square overflows). The search starts at the
   largest finite float, so every smaller distance counts however far outside
   the demonstrations the query is. Like iris_knn_predict it fits the ranges
   of an instrument that has never been fitted.

   iris_classify_1nn and iris_delete_nearest (PART 4) both use it, so the
   demonstration you delete by standing on it is the one the classifier
   names. */
IRIS_API int iris_internal_nearest(iris *k, const float *in) {
  if (!iris_shape_fits(k) || k->n_ex == 0) return -1;
  if (!k->fitted) iris_fit_ranges(k);
  float inv[IRIS_MAX_IN];
  iris_internal_neighbour_scale(k, inv);
  const int stride = k->n_in + k->n_out;
  int best = -1; float best_d = IRIS_FLT_MAX;
  for (int r = 0; r < k->n_ex; ++r) {
    const float d = iris_internal_distance2(k, inv, k->ex + (size_t)r * stride, in);
    if (d < best_d) { best_d = d; best = r; }
  }
  return best;
}

/* k-NN inverse-squared-distance-weighted regression. k neighbours (default
   choice: 3, at most IRIS_KNN_MAXK), weight 1/(d^2 + guard) each, where d^2
   is the squared distance. Standing exactly on a demonstration gives that row
   a weight of 1e9, so recall is exact to within rounding unless another
   demonstration lies almost on top of it; between demonstrations the
   nearest k blend. Conflicting duplicates average finitely (the guard keeps
   zero-distance weights finite). O(n_ex * n_in) per call, division-free
   scan.

   WHAT IT WRITES INSIDE THE INSTRUMENT: the status, when it has something to
   report, and the ranges of an instrument that has never been fitted (see
   below). That is why it takes a non-const instrument. */
/* LENGTHS, same rule as iris_predict and just as unchecked.
   Reads exactly n_in floats from `in` and writes exactly n_out into `out`. */
IRIS_API void iris_knn_predict(iris *k, const float *in, float *out, int kk) { if (!k) return;
  if (!iris_shape_fits(k)) {
    for (int o = 0; o < k->n_out; ++o) out[o] = iris_internal_centre(k, o);
    k->status = IRIS_NOT_FITTED;
    return;
  }
  /* The distance needs input ranges, and an instrument that has never been
     fitted has none of its own: iris_init's 0..1 describes nothing it was
     shown. So they are fitted here from the demonstrations -- the same work
     iris_fit_ranges does, depending on nothing else -- rather than leaving
     the caller an ordering rule to forget. */
  if (!k->fitted && k->n_ex > 0) iris_fit_ranges(k);
  const int NIn = k->n_in, NOut = k->n_out;
  if (k->n_ex <= 0) { for (int o = 0; o < NOut; ++o) out[o] = 0.0f; return; }
  /* THE NEIGHBOUR COUNT, clamped to at least 1 and at most n_ex and
     IRIS_KNN_MAXK. The test above says <= 0 rather than == 0, which tells
     gcc that n_ex is at least 1 from here on, and the lower clamp comes last,
     which makes kk at least 1 whatever n_ex is. Either of those on its own,
     as does filling every slot below, clears gcc-15's "bi may be used
     uninitialized" in tests/audit.c at -O2 and -O3 (measured). */
  if (kk > k->n_ex) kk = k->n_ex;
  if (kk > IRIS_KNN_MAXK) kk = IRIS_KNN_MAXK;
  if (kk < 1) kk = 1;

  float inv[IRIS_MAX_IN];
  iris_internal_neighbour_scale(k, inv);

  const int stride = NIn + NOut;
  /* Every slot is filled, not only the first kk that the scan and the blend
     use, so bi is initialised whatever a compiler can prove about kk. gcc's
     -Wmaybe-uninitialized cannot always follow kk: filling only kk slots and
     blending in a for loop over kk that stops at the first empty slot draws
     "bi may be used uninitialized" from gcc-15 at -O2 and -O3 and from the
     ESP32-S3's gcc at -O2 and -O3. */
  int   bi[IRIS_KNN_MAXK];
  float bd[IRIS_KNN_MAXK];
  for (int n = 0; n < IRIS_KNN_MAXK; ++n) { bi[n] = -1; bd[n] = IRIS_FLT_MAX; }

  for (int r = 0; r < k->n_ex; ++r) {
    const float d = iris_internal_distance2(k, inv, k->ex + (size_t)r * stride, in);
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
     whatever sits beside the arena. Refuse instead: write the substitute
     (iris_internal_centre) and report, exactly like the MLP backstop. */
  if (bi[0] < 0) {
    for (int o = 0; o < NOut; ++o) out[o] = iris_internal_centre(k, o);
    k->status = IRIS_NAN_TRAPPED;
    return;
  }
#else
  if (bi[0] < 0) bi[0] = 0;   /* no guard: use the first demonstration, as
                                 iris_classify_1nn does, never read outside */
#endif
  /* THE BLEND, written as the nearest neighbour's value plus the weighted mean
     of how far each neighbour's value lies from it:

         out = y0 + sum( s * (y - y0) ),    s = w / sum( w )

     where s is a neighbour's share of the total weight. That is the ordinary
     weighted mean, sum(w * y) / sum(w), rearranged, and the rearrangement is
     what makes it exact when every neighbour agrees: each difference is then
     exactly zero, so the answer is exactly y0. The ordinary form rounds.
     Measured on it with the checks in tests/playing.c: 291,862 of 364,140
     queries on stores whose labels all agreed did not return the label
     exactly -- a label of 3 came back as 2.9999998, which a C cast to int
     turns into class 2 -- and 44,504 of 800,000 random queries landed a few
     steps of float resolution outside the demonstrated range.

     The shares are worked out before they multiply anything. A weight
     reaches 1e9 on a demonstration you stand on, so w * (y - y0) overflows
     once two values differ by more than about 3e29; a share is at most 1, so
     s * (y - y0) is never larger than the difference it scales.

     This form stays inside without help: the nearest neighbour carries the
     largest weight, so the mean keeps a margin from either end of the range
     that rounding cannot cross (0 of the 800,000, and 0 of 1,000,000 queries
     on stores built from values at the very ends of their range). The answer
     is still held between the smallest and largest of the neighbours' values,
     for the one case that argument does not cover: two values of opposite
     sign whose difference is larger than the largest float and overflows to
     infinity.

     The nearest neighbour's own term is s * (y0 - y0), which is zero, so the
     sum starts at the second slot. Fewer than kk rows can be in the slots
     when some distances are not finite; the blend uses the ones that are
     there. */
  const float *y0 = k->ex + (size_t)bi[0] * stride + NIn;
  float share[IRIS_KNN_MAXK], wsum = 0.0f;
  int used = 0;
  while (used < kk && bi[used] >= 0) {
    share[used] = 1.0f / (bd[used] + IRIS_KNN_GUARD);
    wsum += share[used];
    ++used;
  }
  for (int n = 0; n < used; ++n) share[n] = share[n] / wsum;
  for (int o = 0; o < NOut; ++o) {
    float d = 0.0f, lo = y0[o], hi = y0[o];
    for (int n = 1; n < used; ++n) {
      const float y = k->ex[(size_t)bi[n] * stride + NIn + o];
      d += share[n] * (y - y0[o]);
      if (y < lo) lo = y;
      if (y > hi) hi = y;
    }
    out[o] = iris_clampf(y0[o] + d, lo, hi);
  }
#ifndef IRIS_NO_GUARDS
  /* iris_record refuses a not-a-number, but the store is memory the caller
     can reach, and a NaN output that gets in there would be blended straight
     into an audio parameter. Same last line of defence as iris_predict. */
  for (int o = 0; o < NOut; ++o)
    if (iris_isbad(out[o])) {
      out[o] = iris_internal_centre(k, o);
      k->status = IRIS_NAN_TRAPPED;
    }
#endif
}

/* 1-NN classification: snap to the single nearest demonstration and return
   its outputs VERBATIM (bit-for-bit) plus its stable example id, or -1 if
   the store is empty or the reading has no finite distance to any take. For
   a classifier task store the class label in out[0]; this is then exactly
   desktop Wekinator's shipping default for discrete outputs (Weka IBk, k=1,
   min-max normalised Euclidean distance, first-recorded wins ties). Like
   iris_knn_predict it writes the status and the ranges of a never-fitted
   instrument, so it takes a non-const one. */
/* LENGTHS, same rule as iris_predict and just as unchecked.
   Reads exactly n_in floats from `in` and writes exactly n_out into `out`;
   `out` may be null when only the identifier is wanted. */
IRIS_API int iris_classify_1nn(iris *k, const float *in, float *out) { if (!k) return -1;
  if (!iris_shape_fits(k)) {
    if (out) for (int o = 0; o < k->n_out; ++o) out[o] = iris_internal_centre(k, o);
    k->status = IRIS_NOT_FITTED;
    return -1;
  }
  const int NIn = k->n_in, NOut = k->n_out;
  if (k->n_ex == 0) {                 /* nothing to snap to: 0, as iris_predict */
    if (out) for (int o = 0; o < NOut; ++o) out[o] = 0.0f;
    return -1;
  }
  int best = iris_internal_nearest(k, in);   /* fits a never-fitted instrument */
#ifndef IRIS_NO_GUARDS
  /* A query whose distance to every demonstration is not finite -- a
     disconnected sensor reading not-a-number -- has no nearest row. Answering
     with the first demonstration would give a classifier a confident wrong
     class and a healthy status, so it refuses instead, as iris_knn_predict
     does: the substitute, and IRIS_NAN_TRAPPED. */
  if (best < 0) {
    if (out) for (int o = 0; o < NOut; ++o) out[o] = iris_internal_centre(k, o);
    k->status = IRIS_NAN_TRAPPED;
    return -1;
  }
#else
  if (best < 0) best = 0;
#endif
  if (out) {
    const float *row = k->ex + (size_t)best * (NIn + NOut);
    for (int o = 0; o < NOut; ++o) out[o] = row[NIn + o];
#ifndef IRIS_NO_GUARDS
    /* Verbatim means verbatim for every healthy value — but a NaN stored in
       an example's outputs must not escape as a "class label". Substitute
       and report; the returned id still names the row so the musician can
       find and delete it. */
    for (int o = 0; o < NOut; ++o)
      if (iris_isbad(out[o])) {
        out[o] = iris_internal_centre(k, o);
        k->status = IRIS_NAN_TRAPPED;
      }
#endif
  }
  return k->ex_id[best];
}

/* The end of the floating-point scope opened by defence 3 of the determinism
   contract at the top of this file, which explains each line: code after the
   #include gets back the contraction setting it had before it, or on the
   clang targets named there, the command line's. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT DEFAULT
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wignored-pragmas"
#pragma float_control(pop)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif

#endif /* IRIS_H */
