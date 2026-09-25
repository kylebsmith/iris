/* SPDX-License-Identifier: BSD-3-Clause
   Copyright (c) 2026 Kyle Smith */
/* ============================================================================
   iris.h  —  interactive machine learning for handmade instruments
   v0.2.0 · one file · C99 · no dependencies · no allocation · no C library

   You show it a handful of examples of "when I do THIS, it sounds like THAT".
   It learns a mapping and fills in everything in between.

   This is the whole brain of the instrument. The same file compiles for a
   laptop, a web browser and an ESP32-S3 microcontroller, because it contains
   no hardware, no operating system and no library calls. It is arithmetic on
   memory you hand it. It is also meant to be read: the PARTS below explain
   the mathematics as they implement it, and the words it uses are defined at
   the end of this block.

   USAGE
     static unsigned char mem[IRIS_ARENA(2, 12, 3, 64)];
     iris *k = iris_init(mem, sizeof mem, 2, 12, 3, 64, 12345);

     iris_record(k, gesture, sound);     // do this a few times
     iris_train(k);                      // fit them, starting from the seed
     iris_predict(k, gesture, sound);    // now play

   A bad take is repaired the same way: iris_delete_id, then iris_train
   again. iris_train starts over from the instrument's seed every time, so a
   deleted take leaves nothing behind in the weights.

   THE THREE RULES THIS FILE OBEYS
     1. No allocation. You give it one block of memory, the arena, and it
        never asks for more, so you know exactly how much memory the
        instrument uses before the program runs.
     2. No C library. No printf, no math.h: the square root, the
        nonlinearity and the checksum are written out in this file.
     3. Single precision on the playing path. The ESP32-S3 does 32-bit
        float arithmetic in hardware and 64-bit double arithmetic in slow
        software. The one place that uses doubles is the leave-one-out sweep
        shared by iris_loo_error and iris_suggest_smoothing (PART 8), a
        diagnostic that never runs while you play, which adds up its squared
        misses in double on purpose so that the small ones keep their
        precision.

   NO DEPENDENCIES, EXACTLY. The file includes <stddef.h> and <stdint.h>,
   which define types and nothing else. A translation unit that calls every
   function in it, compiled with

       -std=c99 -ffreestanding -fno-stack-protector              (clang)
       -std=c99 -ffreestanding -fno-stack-protector
                -fno-tree-loop-distribute-patterns                (GCC)

   at -O0, -O1, -O2, -O3 and -Os, as C and as C++, has no undefined symbol
   and links with -nostdlib -static, with no C library at all: Apple clang,
   Homebrew clang 22 and gcc-15 on a 64-bit ARM Mac, and Debian's gcc 14 and
   clang 19 on 64-bit ARM Linux (as C). No flag changes an output bit. Each
   stops the COMPILER, not this code, from reaching for the C library:

     -ffreestanding        without it clang turns the loops that zero or
                           copy an array into calls to memset, memcpy, bzero
                           and, on macOS, memset_pattern16.
     -fno-stack-protector  where stack protection is on by default (clang on
                           macOS), every function with an array calls
                           __stack_chk_fail.
     -fno-tree-loop-distribute-patterns   GCC turns the same loops into
                           memset, memcpy and memmove calls even under
                           -ffreestanding.

   tests/freestanding.sh checks all of this, and rebuilds without each flag
   to show what that flag keeps out.

   ON THE ESP32-S3 THE LIST IS NOT EMPTY, AND EVERY ENTRY IS THE COMPILER'S.
   With the chip's own compiler (xtensa-esp32s3-elf-gcc, the GCC flags above,
   at -O0, -O2 and -Os):

     the playing path     __divsf3 and nothing else. That path is iris_init,
                          iris_record, iris_train, iris_predict, iris_novelty,
                          the neighbour functions, the deletes and iris_clear.
     the whole file       also __adddf3 __divdf3 __extendsfdf2 __floatsidf
                          __ledf2 __muldf3 __subdf3 __truncdfsf2, the
                          double-precision routines of the leave-one-out
                          sweep (rule 3).

   __divsf3 is single-precision division. The chip's floating-point unit has
   divide-step instructions but no single instruction that divides two
   floats, so every float division is a call to this routine in libgcc, the
   compiler's own support library (an Arduino build calls a copy in the
   chip's read-only memory). None of these is a C library function, none is
   a call this source writes, and all of them are in every Arduino build
   anyway. tests/freestanding.sh requires exactly this list.

   HOW LONG TRAINING TAKES, AND WHERE EACH NUMBER COMES FROM. iris_train runs
   until the error stops improving, under a ceiling (PART 8). The epochs it
   takes depend on the data:

     host      On the development laptop (an Apple M4 Max, Apple clang -O2),
               20 demonstrations of the reference task in tests/audit.c
               (2 inputs, 12 hidden units, 3 outputs) take 18,000 epochs and
               about 31 ms; 50 demonstrations take 12,000 epochs and about
               52 ms. `sh build.sh audit` prints these in its training-cost
               table.
     board     The only ESP32-S3 figures come from the starter kit's
               device_torture sketch, run with iris 0.1.0 on two boards at
               240 MHz, and no log of those runs is recorded yet. Test 5
               timed iris_train at 595 ms for 4 demonstrations and 2.7-3.0 s
               for 8 to 20, on data whose two inputs move together and whose
               runs stop after 3,800 to 9,300 epochs. Data that needs 18,000
               epochs has not been timed on the board, so representative
               on-device training time is not yet measured.

   A sketch that must keep drawing while it trains takes the same run in
   slices, iris_train_begin and iris_train_slice, which is bit-identical to
   iris_train.

   iris_continue(k, n) and iris_continue_to_plateau carry on from the
   weights the instrument already holds: the warm trainers. iris_continue is
   the fixed-epoch update of Weka's MultilayerPerceptron, the network
   Wekinator ships, and tests/audit.c pins its output to the bit. Read their
   hazard in PART 8 before using either.

   THE WORDS THIS FILE USES, defined once, here.

     ACTIVATION   what a unit of the network outputs after its squashing
                  function; also used for the squashing function itself.
     ARENA        the one block of memory you give iris_init. Everything the
                  instrument knows lives inside it; IRIS_ARENA computes its
                  size.
     BACKPROPAGATION  the training method of PART 8: run a demonstration
                  forward, measure the miss, and work backwards to how much
                  each weight contributed to it.
     BINARY32, BINARY64  the 32-bit and 64-bit floating-point formats of
                  IEEE 754, the floating-point standard of the Institute of
                  Electrical and Electronics Engineers, which every processor
                  iris targets follows. C calls them float and double.
     BIT-IDENTICAL  equal in every bit, not merely close.
     C99, ISO C   the 1999 edition of the C standard, which this file is
                  written to; "ISO C" means a compiler mode that follows the
                  standard without GNU extensions (-std=c99, not -std=gnu99).
     CHOLESKY     a standard, fast way to solve a symmetric system of linear
                  equations, by factoring its matrix into a triangular matrix
                  times that triangle's mirror image. Used once, in PART 8d.
     CLAMP        hold a value inside limits: below the lower limit it becomes
                  the lower limit, above the upper it becomes the upper.
     CLANG, GCC   the two C compiler families the tests use; GCC is the GNU
                  Compiler Collection, and "GNU mode" means its -std=gnu...
                  settings, which the Arduino build uses. The ESP32-S3,
                  Cortex-M and AVR toolchains are GCC. Apple clang is the
                  clang that ships with macOS.
     CRC-32       a cyclic redundancy check: a 32-bit checksum over a saved
                  file, which catches accidental corruption but not
                  deliberate editing. PART 9.
     DEMONSTRATION  one recorded pair: these sensor readings go with those
                  output values. Also called an example or a take.
     ELM          extreme learning machine, the closed-form trainer: the
                  hidden layer keeps its random starting weights and only the
                  output layer is solved, exactly, in one step. PART 8d.
     EPOCH        one pass over every demonstration you have recorded.
                  Training is thousands of these: "9,000 epochs" means the
                  network saw each of your takes 9,000 times.
     ERROR        how far the network's outputs are from the demonstrated
                  ones. MEAN SQUARED ERROR is the average of the squared
                  misses; ROOT-MEAN-SQUARE error is its square root, in the
                  units of the miss. Training errors in this file are in the
                  network's own output units, where each output's
                  demonstrated range spans 0.1 to 0.9 (PART 5).
     FLOAT        a number stored in binary32: a sign, an 8-bit exponent and
                  24 significant bits, about seven significant decimal digits.
     FNV-1a       the Fowler-Noll-Vo hash, a simple byte-by-byte hash. The
                  tests use it to reduce an instrument to one 32-bit number.
     FUSED MULTIPLY-ADD  one instruction that computes a*b + c with a single
                  rounding instead of two. It changes the last bit of a
                  result, so this file forbids it in its own code (the
                  determinism contract below). CONTRACTION is the compiler
                  turning a*b + c into one.
     GEOMETRIC MEAN  the average of a set of ratios taken by multiplying
                  them and taking the root, so that twice as good and twice
                  as bad cancel. The comparisons below that give one average
                  ratios of errors this way.
     GOLDEN HASH  the FNV-1a hash of the bits a fixed recipe produces,
                  compared in the tests, so any change to the arithmetic
                  fails a check. tests/audit.c pins 0x6805FB0D.
     GRADIENT     for each weight, how much the error would change if that
                  weight moved a little. Training steps every weight against
                  its gradient, which is downhill (PART 8).
     HELD-OUT ERROR  the error at points the instrument was not trained on,
                  measured against the target a test knows is true: how well
                  it fills the gaps between your takes, which is what you
                  play. Compare RECALL.
     HIDDEN LAYER  the middle layer of the network, between the inputs and
                  the outputs (PART 6); its units are HIDDEN UNITS.
     IDE          integrated development environment: the Arduino IDE is the
                  program most students write and upload sketches with.
     k-NN, 1-NN   k-nearest-neighbour: answer a gesture with a blend of the
                  k stored demonstrations nearest to it. 1-nearest-neighbour
                  (k = 1) returns the single nearest one. PART 10.
     LEAVE-ONE-OUT  hide one demonstration, train on the rest, see how far
                  off the hidden one you land; repeat for each and average.
     LEARNING RATE, MOMENTUM  how big each training step is, and how much of
                  the previous step it carries on with (PART 8). Internal:
                  every instrument trains with 0.10 and 0.85.
     LOGIT        the inverse of the sigmoid: the sum a unit must reach for its
                  sigmoid to output a given value (PART 8d solves in it).
     MLP          multilayer perceptron: the network of PARTS 6 and 8, here
                  with one hidden layer.
     NIME         the International Conference on New Interfaces for Musical
                  Expression. "Fiebrink and Sonami, NIME 2020" is Rebecca
                  Fiebrink and Laetitia Sonami, "Reflections on Eight Years of
                  Instrument Creation with Machine Learning", its 2020
                  proceedings, pages 237-242.
     NaN          not-a-number: the value a float takes after 0/0 or
                  infinity minus infinity. Every comparison with it is false
                  and any arithmetic with it gives NaN again, so one of them
                  spreads through everything it touches. The comments spell
                  it out as not-a-number.
     NORMAL MATRIX  the square matrix that least-squares fitting produces
                  and Cholesky then solves.
     NORMALISED   put on a fixed scale: every input onto -1..+1 across its
                  demonstrated range, every output onto 0.1..0.9 (PART 5).
     ODR          the one-definition rule: C and C++ require a thing to be
                  defined identically everywhere it appears.
     PLATEAU      the stretch of a training run where the error has stopped
                  falling by much; iris_train stops at one unless its error
                  floor or its ceiling stops it first (PART 8).
     PRE-ACTIVATION  the weighted sum a unit computes before its squashing
                  function is applied.
     RECALL       how closely a trained instrument plays back its own
                  demonstrations: the error at the demonstrated points.
                  Good recall with a poor HELD-OUT ERROR means the network
                  has fitted the noise in the takes rather than the mapping.
     RIDGE        a small number added down the diagonal of the normal matrix
                  before solving it, which stops the solve failing when two
                  demonstrations nearly repeat and pulls the answer toward
                  smaller weights. PART 8d.
     SANITIZER    compiler instrumentation that stops a program at a fault:
                  AddressSanitizer at an out-of-bounds memory access,
                  UndefinedBehaviorSanitizer at an operation C leaves
                  undefined (a misaligned access, a signed overflow).
     SATURATE     a squashing function saturates when its input is so far
                  from zero that its output sits at, or nearly at, its limit,
                  where a small change to the input no longer moves it.
     SEED         the number the random starting weights are drawn from. The
                  same seed always gives the same weights; a new seed is a
                  reroll. A seed of 0 is taken as 1.
     SIGMOID, TANH  the two S-shaped squashing functions (PART 1): tanh runs
                  from -1 to +1 and the sigmoid from 0 to 1.
     STOCHASTIC GRADIENT DESCENT  nudging the weights after each single
                  demonstration, visited in a shuffled order, rather than
                  once after all of them. It is how PART 8 trains.
     SUBNORMAL    a float so close to zero that the format gives up
                  precision to represent it (also called denormal). Some
                  processors flush them to zero in hardware.
     TRANSLATION UNIT  one .c file together with everything it includes.
     ULP          unit in the last place: the gap between a float and the
                  next one. "1 ulp" is the smallest change a float can make.
     WEIGHT, BIAS  the numbers the network learns. A weight scales one
                  connection; a bias shifts a unit's sum whatever comes in.
     WEKA, WEKINATOR  Weka is a Java machine-learning toolkit; Wekinator,
                  Rebecca Fiebrink's tool for building instruments from
                  demonstrations, trains Weka's MultilayerPerceptron and
                  nearest-neighbour classifier. iris rebuilds that loop for
                  boards with no operating system.

   The hardware named in comments: Arduino, the family of microcontroller
   boards (and the IDE) most students start with; the ESP32 family and the
   ESP32-S3, the Espressif microcontroller the starter kit uses, with an
   Xtensa LX7 processor core; AVR, the 8-bit processor of the Arduino Uno and
   Mega; RP2040 and STM32, other 32-bit microcontrollers; ARM, the processor
   family of phones, Apple-silicon Macs and the Raspberry Pi, whose
   Cortex-M cores are its microcontrollers; x86 and x86-64, the processors of
   most other laptops, and x87, the old floating-point unit of 32-bit x86;
   RISC-V and PowerPC, two more processor families; and WebAssembly, the
   portable instruction format web browsers run.
   ============================================================================ */

#ifndef IRIS_H
#define IRIS_H

/* ============================================================================
   THE WHOLE INTERFACE

   Forty-one functions, three types, and the macros listed at the end. `k` is
   the instrument. `in` and `out` are plain float arrays you own: `in` holds
   n_in floats and `out` n_out, and nothing checks their length, so an array
   shorter than the instrument's shape is read or written past its end.
   Anything named iris_internal_ is part of how the file works, not of what
   it promises, and can change in any release.

   LIFECYCLE
     size_t   iris_size(n_in, n_hid, n_out, cap)   arena bytes for a shape; 0
                                                    if the shape is refused
     iris    *iris_init(mem, bytes, n_in, n_hid, n_out, cap, seed)
                                                    build an instrument in your
                                                    memory; 0 if refused
     void     iris_reseed(k, seed)                  new random weights from a
                                                    new seed: the reroll
     uint32_t iris_seed(k)                          the seed in use

     The four numbers are how many sensor values come in, how wide the
     hidden layer is (12 is a good answer, 8 the minimum), how many values
     go out, and how many demonstrations can be stored. They must match the
     four you gave IRIS_ARENA.

   DEMONSTRATIONS
     int  iris_record(k, in, out)         store one; its identifier (1 or
                                          more), or 0 if refused
     int  iris_count(k)                   how many are stored
     int  iris_capacity(k)                how many can be
     int  iris_get(k, idx, in, out)       copy one out; its identifier, or 0
     int  iris_index_of(k, id)            position of an identifier, or -1
     int  iris_id_at(k, idx)              identifier at a position, or -1
     int  iris_delete_id(k, id)           delete by identifier; 1, or 0
     int  iris_delete_index(k, idx)       delete by position; 1, or 0
     int  iris_delete_last(k)             delete the newest; 1, or 0
     int  iris_delete_nearest(k, in)      delete the one nearest this
                                          reading; 1, or 0
     void iris_clear(k)                   delete them all

   TRAINING
     int   iris_train(k)                  fit the demonstrations, starting
                                          from the seed; 1, or 0 if refused.
                                          THE ONE TO CALL.
     int   iris_train_begin(k, ceiling)   the same run, in slices: start it
     int   iris_train_slice(k, epochs)    run up to `epochs` more; 1 while
                                          there is more to do
     float iris_train_progress(k)         0.0 to 1.0, never ahead of the truth
     int   iris_train_busy(k)             1 while a sliced run is going
     int   iris_train_epochs_done(k)      epochs the last run actually did
     int   iris_is_trained(k)             1 if the fit matches the stored
                                          demonstrations
     float iris_last_error(k)             the training error of the current
                                          weights (PART 8)
     void  iris_set_smoothing(k, amount)  0 sticks to the demonstrations, 1
                                          smooths confidently between them
     float iris_get_smoothing(k)

   TRAINING, ADVANCED: THE WARM TRAINERS (read their hazard in PART 8)
     float iris_continue(k, epochs)       more epochs from the current
                                          weights; the error, or -1
     float iris_continue_to_plateau(k, ceiling, cb, user)
                                          the same, until the error stops
                                          improving; the error, or -1

   TRAINING, CLOSED FORM
     int   iris_train_elm(k, lam0, scratch, bytes)
                                          solve the output layer in one step;
                                          ridge doublings used (0 is best), or
                                          -1 if refused

   PLAYING
     void  iris_predict(k, in, out)       THE PLAYING CALL: the network's
                                          answer
     void  iris_knn_predict(k, in, out, kk)  a blend of the kk nearest
                                          demonstrations
     int   iris_classify_1nn(k, in, out)  the nearest demonstration's outputs
                                          exactly; its identifier, or -1
     float iris_novelty(k, in)            0 on a demonstration, rising to 1
                                          away from them; -1 if refused

   DIAGNOSTICS
     iris_status iris_get_status(k)       is the INSTRUMENT unwell? 0 is
                                          healthy
     float iris_example_stress(k, idx)    how hard one demonstration fought
                                          the others; 1.0 is ordinary, -1 if
                                          refused
     int   iris_worst_example(k, margin)  position of the one that fought
                                          hardest, or -1
     int   iris_worst_example_id(k, margin)  its identifier, or -1
     float iris_loo_error(k, epochs)      a leave-one-out held-out error, or -1;
                                          it REPLACES the weights with a
                                          fixed-epoch refit (PART 8)
     float iris_suggest_smoothing(k, scratch, bytes)
                                          a smoothing value to audition, or -1

   PERSISTENCE
     size_t iris_save_size(k)             exactly the bytes iris_save writes
     size_t iris_save(k, buf, cap)        bytes written, or 0
     int    iris_load(k, buf, bytes)      1, or 0 with k untouched

   TYPES: iris (the instrument), iris_status (what iris_get_status returns),
   iris_progress_fn (the callback iris_continue_to_plateau calls).

   MACROS: IRIS_ARENA(n_in, n_hid, n_out, cap), the arena size at compile
   time; IRIS_ELM_SCRATCH(n_hid, n_out), the closed-form trainer's scratch;
   IRIS_ARENA_ELM, the two added together; IRIS_MAX_IN, IRIS_MAX_OUT and
   IRIS_MAX_HID, which you may lower before the #include, and IRIS_MAX_EX;
   IRIS_VERSION_MAJOR, _MINOR, _PATCH and _STRING; IRIS_STRESS_MIN_EX and
   IRIS_STRESS_FLAG (PART 8f); IRIS_KNN_MAXK (PART 10); IRIS_ID_LIMIT, the
   bound on identifiers on this machine (PART 4); IRIS_W_LIMIT, the
   divergence guard's limit (PART 8); IRIS_FILE_MAGIC, IRIS_FILE_VERSION and
   IRIS_FILE_HEADER, the save format's fixed values (PART 9); the iris_status
   values; IRIS_API, which you may define before the #include to change how
   every function is declared (the WebAssembly port marks them used); and
   IRIS_NO_GUARDS, which compiles the guards out and is for measuring them,
   not for instruments. Every other IRIS_ macro is part of how the file
   works, like the iris_internal_ names: it can change in any release, and
   defining one yourself is not supported.

   FAILURE, in two rules:
     A call that either works or does not returns 0 for "did nothing".
     A call that returns a MEASUREMENT returns it, or -1 if it refused.
   iris_get_status answers a different question: whether the INSTRUMENT is
   in trouble. Zero means opposite things in the two places. A RETURN VALUE
   of 0 is bad news (the call did nothing); a STATUS of 0 is good news
   (IRIS_STATUS_OK, nothing is wrong). The full account, with the one test
   that answers "did it train?" after every trainer and the places where the
   two rules give different answers to similar questions, is above
   iris_get_status.

   THREADING: never touch the same instrument from two places at once. The
   note above iris_get_status says why, and what that allows.

   Units: none. Feed it raw sensor readings. It fits its own range to
   whatever you actually give it, so scaling, centring or normalising them
   first gains nothing, and is one more thing to repeat exactly at play
   time.
   ========================================================================= */


/* --------------------------------------------------------------------------
   FLOAT DETERMINISM CONTRACT

   "Same seed, same demonstrations, same instrument" is a promise about
   every bit, and it holds only if every float operation is done in single
   precision, in the order written, with nothing fused. Without the defences
   below, the golden recipe of tests/audit.c built at -ffp-contract=off, on
   and fast gives three different instruments on Apple clang 17. Four
   defences, cheapest first:

   1. Tripwires for the flags that change the arithmetic. -ffast-math implies
      contract=fast AND removes the NaN semantics the guards below depend on.
      Refuse to compile.                                                    */
#if defined(__FAST_MATH__)
#error "iris: -ffast-math / -Ofast breaks same-seed bit-determinism and disables NaN trapping. If you did not pass this yourself, your board package did: check compiler.optimization_flag in its platform.txt (Adafruit nRF52 sets -Ofast there). Build without it."
#endif
/* -ffinite-math-only is one of the flags -ffast-math turns on, but on its own
   it does NOT set __FAST_MATH__, so it needs a tripwire of its own. Without
   one, every guard in the library is optimised away: iris_internal_isbad
   folds to false, poisoned demonstrations are accepted, and a broken sensor
   produces a plausible number and a healthy status (Apple clang 17, with
   this tripwire removed). */
#if defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__
#error "iris: -ffinite-math-only tells the compiler no NaN or infinity can exist, which deletes every guard in this library. Build without it."
#endif
/* THE COMPONENT FLAGS, which -ffast-math turns on and which also work alone.
   -freciprocal-math, -funsafe-math-optimizations and -fassociative-math each
   change the instrument with no diagnostic of any kind. GCC announces them
   with macros and clang does not, so this catches them on GCC only, which
   is the compiler for every ESP32, AVR and RP2040 build.
   No Arduino core passes any of these: checked platform.txt for arduino:avr,
   esp32:esp32, rp2040:rp2040 and STMicroelectronics:stm32. Reaching this
   #error takes a deliberate flag.

   WHAT THIS CANNOT CATCH. -fassociative-math passed on its own defines no
   macro at all on gcc-15 (gcc-15 -dM -E lists none), so it cannot be seen
   here, and it does change the instrument. On clang no component flag can
   be seen, because clang defines none of these macros; clang 22's
   -ffp-model=fast defines only __FINITE_MATH_ONLY__, as 0, so it compiles
   without a word and moves the golden hash in tests/audit.c. Do not build
   with them. */
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
                 -std=gnu17 -mcpu=cortex-a76, the Raspberry Pi 5's core, and
                 tests/audit.c built that way with gcc-15 still gives its
                 golden hash.
        1, 2     float carried as double, or as the 80-bit x87 format. 32-bit
                 x86 doing its float arithmetic on the x87 unit
                 (-mfpmath=387) reports 2, and there every instrument comes
                 out different with no diagnostic: clang 22
                 --target=i686-linux-gnu -mno-sse -mfpmath=387, run in a
                 32-bit Linux container, gives the golden recipe the hash
                 0x2B53B02B instead of 0x6805FB0D.
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
      -ffp-contract=on: the instrument is then bit-identical to a
      -ffp-contract=off build. Clang IGNORES it under -ffp-contract=fast, so
      a -ffp-contract=fast clang build must also pass -ffp-contract=off.

      GNU compilers ignore the standard pragma and contract by default in
      every GNU mode (-std=gnu17; the Arduino IDE's -std=gnu++2a) and in ISO
      C++; only ISO C (-std=c99) leaves contraction off. They do honour
      #pragma GCC optimize ("fp-contract=off"), even under -ffp-contract=fast:
      built with gcc-15 -O2 -ffp-contract=fast, the golden hash in
      tests/audit.c holds with it and moves without it, and the
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
/* 4. The golden hashes, the backstop for everything above. tests/audit.c
      trains a fixed recipe and compares an FNV-1a hash of the instrument's
      bytes with 0x6805FB0D; tests/starter_recipes.c does the same for the
      starter kit's two recipes; tests/load.c loads a committed saved
      instrument and compares what it plays, bit for bit, with what it
      played before it was saved. A build that changes one bit of the
      arithmetic fails there.                                               */

#include <stddef.h>
#include <stdint.h>

/* The library's version, MAJOR.MINOR.PATCH. library.properties,
   CITATION.cff and the release tag state the same number.

   The SAVE FILE has a format number of its own, written into every file
   (PART 9). It changes only when the bytes of a file or their meaning
   change, never merely because the library's version does. */
#define IRIS_VERSION_MAJOR 0
#define IRIS_VERSION_MINOR 2
#define IRIS_VERSION_PATCH 0
#define IRIS_VERSION_STRING "0.2.0"

/* THE MAXIMA ARE THE SIZE OF EVERY WORKING ARRAY, SO ON A SMALL BOARD THEY
   ARE THE STACK BUDGET.

   Every working array in this file is sized by these numbers rather than by
   the shape you asked for: iris_internal_train_run alone reserves
   float x[IRIS_MAX_IN] and float t[IRIS_MAX_OUT], 192 bytes, whether your
   instrument has 32 inputs or 2. avr-gcc 7.3.0 -mmcu=atmega328p -Os
   -fstack-usage, with every function compiled on its own (IRIS_API defined
   empty), gives iris_internal_train_run a frame of 284 bytes and
   iris_predict 158. An Uno has 2 KB of memory in total and a sketch leaves a
   few hundred bytes of it for the stack, so the defaults do not fit it with
   room to spare.

   They are #ifndef so you can shrink them. Define them BEFORE including this
   file and every working array shrinks with them:

       #define IRIS_MAX_IN  4
       #define IRIS_MAX_OUT 4
       #define IRIS_MAX_HID 12
       #include "iris.h"

   With the same compiler and flags iris_internal_train_run, the deepest
   frame on the record/train/predict path, falls from 284 bytes to 124, a
   saving of 160, and iris_predict from 158 to 46. Inlined into its caller
   the path measures a little differently: in a sketch-like file that
   records, trains and predicts, built with -ffunction-sections, iris_train
   (which takes the training run inline) measures 280 bytes and 120 as GNU C
   (-std=gnu11), and as C99 main takes the whole path and measures 290 and
   130. These are
   single frames as the compiler reports them, not a measured run-time
   stack depth. The only rule is that the maxima must be at least the n_in,
   n_out and n_hid you pass to iris_init, which iris_init checks.
   On a 32-bit board (ESP32, RP2040, STM32) leave them alone; the defaults cost
   nothing you have. */
#ifndef IRIS_MAX_IN
#define IRIS_MAX_IN   32   /* sensor features in  */
#endif
#ifndef IRIS_MAX_OUT
#define IRIS_MAX_OUT  16   /* sound parameters out */
#endif
/* THE CAP KEEPS THE SIZE ARITHMETIC INSIDE 32 BITS.

   The bound exists to stop an overflow, not because 4,096 is musically
   special. IRIS_ARENA and iris_size multiply cap by (n_in + n_out) and by
   sizeof(float), in unsigned long, which C guarantees is at least 32 bits
   (see the note at IRIS_ARENA). An uncapped count could wrap that product,
   so that the arena check passed a buffer too small and the demonstration
   store ran off the end of it. At the maxima (32 in, 64 hidden, 16 out)
   4,096 demonstrations make an arena of 862,136 bytes on a 64-bit laptop,
   far below 2^32, so the product cannot wrap. The demonstrations alone are
   786,432 bytes there, already past the ESP32-S3's 512 KB of internal
   memory, so nothing a board can hold is refused.

   Where size_t is 16 bits, every Arduino AVR board, the cap is 255, and there
   it describes the boards rather than an overflow: an int count cannot wrap
   a 32-bit product, and a shape whose arena passes 65,535 bytes is refused
   by the compiler when the array is declared in C, and by iris_size and
   iris_init at run time in C++ (the note at IRIS_ARENA). With avr-gcc 7.3.0
   for the atmega2560 the smallest shape, IRIS_ARENA(1, 8, 1, 255), is 5,580
   bytes, more than an Uno's 2 KB of memory and most of a Mega's 8 KB, and
   255 takes is already more than anyone records by hand. */
#define IRIS_MAX_EX   ((int)(sizeof(size_t) >= 4 ? 4096 : 255))  /* stored takes */
#ifndef IRIS_MAX_HID
#define IRIS_MAX_HID  64   /* hidden units         */
#endif

#ifndef IRIS_API
/* `static inline`, not plain `static`. A single-header library defines every
   function in every translation unit that includes it, and a caller who uses
   five of them is not doing anything wrong. With plain `static`, -Wall
   -Wextra warns about every function the caller leaves unused: 26 warnings
   for examples/00_minimal.c with Apple clang 17 or gcc-15, a bad first
   minute for someone who has just cloned this. `inline` tells the compiler
   the definition may go unused here, which silences that without changing
   linkage, ODR behaviour or the generated code. */
#define IRIS_API static inline
#endif

/* --------------------------------------------------------------------------
   MEMORY

   Everything the instrument knows lives in one contiguous block you own.
   IRIS_ARENA() computes the size at compile time so you can write

       static unsigned char mem[IRIS_ARENA(2, 12, 3, 64)];

   and have the linker reserve it with the program's other static memory
   instead of asking a heap for it at run time. On a microcontroller that is
   the difference between "I know this fits" and "I hope this fits".
   -------------------------------------------------------------------------- */

/* EVERY PRODUCT IS COMPUTED IN unsigned long, WHICH C GUARANTEES IS AT LEAST
   32 BITS, AND NOT IN size_t.

   On a 16-bit size_t target, every Arduino AVR board, products taken in
   size_t wrap. That alone would be survivable if anything noticed, but
   iris_size would wrap IDENTICALLY, so iris_init would compare a wrapped
   need against an equally wrapped array size and could not refuse: computed
   in size_t, IRIS_ARENA(24,63,16,254) comes out as 88 bytes for an
   instrument that needs 65,624, and the first loop of iris_reseed writes
   6,048 bytes into those 88 (avr-gcc for the atmega328p).

   Computing wide makes the true number appear. In C, that number is then too
   large for an array and the COMPILER refuses the declaration -- a build error
   naming the array, which is the outcome we want.

   IN C++ IT IS A RUNTIME REFUSAL INSTEAD, AND ARDUINO COMPILES .ino AS C++.
   An array declarator's size in C++ converts to std::size_t, 16 bits on AVR, so
   the bound wraps silently where C errors. avr-g++ for the atmega328p, each
   line checked as a compile-time assertion:

     IRIS_ARENA(24,63,16,254)          65624   (wide, correct)
     sizeof mem                           88   (the ARRAY narrowed)
     iris_size(24,63,16,254)               0   (cannot be sized here)

   The narrowing threshold and the sizing threshold are the SAME number, so
   every shape the array silently shrinks is a shape iris_size refuses: the
   `need == 0` test in iris_init returns 0 and the sketch gets a null pointer,
   which every example in this repository checks. So the failure is loud, just
   later than it should be: a refusal at run time rather than a build error,
   and never a write past the array. */
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

   Guards never change anything silently: whatever they do is reported
   here. The healthy state is 0, so `if (iris_get_status(k))` reads as "is
   something wrong?". (It is spelled IRIS_STATUS_OK, not IRIS_OK, because
   extras/iris_sink.h, the interface for output ports, already uses the bare
   name, with the same value and meaning.)

   A status stays until something clears it. Every training run that gets
   past its refusals, a successful closed-form solve, iris_reseed and a
   successful iris_load set it back to IRIS_STATUS_OK, and iris_record
   clears IRIS_STORE_FULL. A later healthy call does not: after a
   not-a-number reading, iris_predict goes on reporting IRIS_NAN_TRAPPED on
   good readings until the next training run.
   -------------------------------------------------------------------------- */
typedef enum {
  IRIS_STATUS_OK         = 0,  /* healthy: nothing to report                  */
  IRIS_TRAINING_DIVERGED = 1,  /* a run pushed a weight or bias past
                                ±IRIS_W_LIMIT. The guard clamped it to exactly
                                the limit and stopped the run at that epoch.
                                The instrument is fitted and plays (the trainer
                                still reports a fit and iris_is_trained is 1),
                                but from weights that stopped where the guard
                                stopped them. Every trainer that continues from
                                the current weights now refuses: see
                                IRIS_DIVERGED_STUCK. On a sharp target this
                                can mark a good fit: see IRIS_W_LIMIT.       */
  IRIS_NAN_TRAPPED       = 2,  /* a not-a-number or an infinity was caught by
                                the call that set this, and contained. For
                                example: a reading refused at iris_record's
                                door, a setting refused by a setter, a played
                                output replaced by the centre of the
                                demonstrated range, a training run that met
                                one in the error or a weight partway through
                                and re-seeded to a finite, unfitted start, or
                                a trainer
                                or diagnostic (PART 8, closed-form included)
                                that found one in a stored demonstration and
                                refused. That refusal writes this status and
                                nothing else; the demonstration stays in the
                                store, where it can be found and deleted.    */
  IRIS_RIDGE_ESCALATED   = 3,  /* a closed-form solve (ELM) needed its ridge
                                doubled to factor. Result is valid; the data
                                was harder than usual.                       */
  IRIS_NOT_FITTED        = 4,  /* iris_predict was called on an instrument that
                                has never been fitted, or a playing function
                                (iris_predict, iris_knn_predict,
                                iris_classify_1nn) on one whose shape is too
                                big for this translation unit's working
                                arrays (see iris_internal_shape_fits).
                                Outputs are the centre of the demonstrated
                                range (0 with no demonstrations), never the
                                forward pass over random weights.            */
  IRIS_DIVERGED_STUCK    = 5,  /* a trainer that continues from the current
                                weights (iris_continue or
                                iris_continue_to_plateau) refused, because
                                a weight or bias sits exactly on
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
  IRIS_STORE_FULL        = 6,  /* iris_record was refused because the store is
                                full. A poisoned reading reports
                                IRIS_NAN_TRAPPED instead: both return 0, and
                                the status is how a sketch tells a student
                                whether to delete a demonstration or to check
                                the sensor.                                  */
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

/* Not-a-number or infinity, by bit pattern: the exponent field all ones. No
   C library, and no comparison for an optimiser to assume away. */
IRIS_API int iris_internal_isbad(float x) {
  /* Read the bits through a copy the compiler must actually make, not through
     a union it can see through: through a plain union, clang carries "this
     value is finite" across the type pun and folds the test to false. A
     volatile forces a real store and load, which the optimiser may not reason
     across. (__builtin_memcpy works on clang, but GNU compilers turn it into
     a call to the C library's memcpy, an undefined symbol in a freestanding
     build.) */
  volatile float v = x;      /* a store the compiler must actually perform */
  union { float f; uint32_t u; } c; c.f = v;
  return (c.u & 0x7F800000u) == 0x7F800000u;
}

/* Any momentum velocity smaller than this is musically and numerically dead:
   added to a weight of ordinary size it cannot move it by even one ulp, and
   a weight this small moves nothing either. Flushing the velocities, and the
   weights smoothing's decay shrinks (PART 8), to zero keeps both decaying
   tails out of subnormal numbers, which some processors flush to zero in
   hardware and others compute slowly or exactly, so a tail cannot make a
   host and a board disagree. Whether the ESP32-S3's floating-point unit flushes
   subnormals has not been measured on the chip. 1e-30 is about eight powers
   of ten above the smallest normal float (about 1.2e-38), so every processor
   evaluates the comparison identically.                                     */
#define IRIS_TINY 1e-30f
#ifdef IRIS_NO_GUARDS
#define IRIS_FLUSH(v) (v)
#else
#define IRIS_FLUSH(v) ((v) < IRIS_TINY && (v) > -IRIS_TINY ? 0.0f : (v))
#endif

/* THE WEIGHT LIMIT. A weight or bias past it means training is running away:
   the guard in PART 8 clamps it to exactly this value, reports
   IRIS_TRAINING_DIVERGED and stops the run, and a trainer that would continue
   from a weight sitting on it refuses with IRIS_DIVERGED_STUCK.
   iris_internal_tanh is exactly ±1 beyond |s| = 3, so one weight of 16 on its
   own saturates its hidden unit whenever its input is more than 3/16 of the
   way from the centre of its range to either end.

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
   (iris_continue or iris_continue_to_plateau) clamps every weight past the
   limit in its first epoch and reports IRIS_TRAINING_DIVERGED, and from then
   on refuses with IRIS_DIVERGED_STUCK. iris_train, which starts
   over from the seed, is the way from the closed-form trainer to
   backpropagation (tests/elm.c checks all of this). */
#define IRIS_W_LIMIT 16.0f

/* ==========================================================================
   PART 1 — MATH WE PROVIDE OURSELVES

   This file cannot call math.h, so the few functions the network needs are
   written here: a squashing function, a square root and a random number
   generator. The squashing function is the hot one. With 2 inputs, 12
   hidden units and 3 outputs the network calls it 15 times per
   demonstration per epoch (once per hidden unit, and once inside each
   output's sigmoid), so 20 demonstrations trained for 18,000 epochs make
   5.4 million calls.

   The reason for writing them here is the no-library rule, not speed, but
   the squashing function below also happens to be cheaper than the C
   library's: on the development laptop, the same network trained with
   tanhf takes 53% longer per epoch. It has not been timed on the ESP32-S3.
   ========================================================================== */

/* THE NONLINEARITY, AND WHY IT IS FROZEN.

   p(x) = x(27+x^2)/(27+9x^2), clamped to [-1, +1].

   This is a rational approximation to tanh, the S-shaped curve that runs
   from -1 to +1. Near zero it is almost a straight line and far out it
   flattens against its limits, and that bend is what lets a network of
   these units draw a curved mapping instead of a flat one. Its largest
   distance from true tanh is 0.0235, near x = 1.566 (every float from 0 to
   20 compared with tanhf).

   Two exact facts make the clamp the right one:
   p(x) - 1 = (x-3)^3/(27+9x^2), so p(3) = 1 EXACTLY, and
   p'(x) = ((x^2-9)/(3(3+x^2)))^2 >= 0, so p only ever rises and is flat at 3.
   Past |x| = 3 the formula would climb above 1, so the RETURN VALUE is
   clamped, which keeps every output inside [-1, +1] whatever rounding does.
   Clamping the argument at 3 instead would not: rounding lets 10,220 floats
   between 2.5 and 3 evaluate above 1.0f. The two clamps give different
   answers for 20,671 non-negative floats (and as many negative ones), each
   time by one ulp. The clamp is branch-free, so its cost does not depend on
   the data.

   WHY THIS FUNCTION AND NOT TRUE tanh. It is cheaper and no worse. Measured
   against true tanh and against a rational approximation 245 times more
   accurate, on 2,304 paired runs (6 synthetic target shapes, 5 to 50
   demonstrations, 3 noise levels, 32 seeds, iris_train, held-out error on a
   41 x 41 grid): the accurate function's held-out error is 1.3% higher as a
   geometric mean, higher on 57.1% of the pairs, and only 0.2% higher with
   the one saturating target left out, which is far less than a reroll of
   the seed changes on the same data. It costs 12% more per epoch and 18%
   more per prediction. So it did not win a contest by much, and it is not a
   compromise either. It is FROZEN because saved instruments depend on it: a
   file's weights mean what they mean only through this exact function.

   CONSEQUENCE. (1 - a*a) is the derivative of TRUE tanh, not of this
   function, so the backward pass in PART 8 is a surrogate gradient: it
   points downhill, never with the wrong sign (the clamp keeps a inside
   [-1, +1]), and it is under-scaled by 2.4-3.3% in aggregate
   (docs/MATH-FIXES.md, defect 1). Training with the exact derivative instead
   makes no measurable difference on the same 2,304 pairs (geometric mean of
   held-out error 0.997, with a 95% confidence interval, the range the true
   ratio lies in at 95% confidence, of 0.988 to 1.005).

   The +/-1e9 test keeps x*(27+x^2) finite; it is not where p saturates,
   which is |x| = 3. It is needed because iris_predict does not clamp its
   input: a reading of 1e20 on an input demonstrated between 0 and 1 arrives
   here as a sum near 1e20, where both halves of the ratio overflow to infinity
   and infinity over infinity is not-a-number, so every hidden unit would be
   not-a-number. With the test that reading plays an ordinary output with a
   healthy status. */
IRIS_API float iris_internal_tanh(float x) {
  if (x >  1.0e9f) return  1.0f;
  if (x < -1.0e9f) return -1.0f;
  const float x2 = x * x;
  const float p  = x * (27.0f + x2) / (27.0f + 9.0f * x2);
  return p > 1.0f ? 1.0f : (p < -1.0f ? -1.0f : p);
}

/* The sigmoid, or logistic function: an S-curve from 0 to 1, built from the
   function above as 0.5 * (tanh(x/2) + 1), which is exact for true tanh. It
   reaches 0 and 1 exactly once |x| >= 6, where the rational saturates. */
IRIS_API float iris_internal_sigmoid(float x) {
  return 0.5f * (iris_internal_tanh(0.5f * x) + 1.0f);
}

/* THE SQUARE ROOT, in integers, correctly rounded.

   Four places take a square root: iris_reseed (the starting weight scales,
   1/sqrt(inputs) and 1/sqrt(hidden units)), iris_novelty (the distance it
   reports, and the sqrt(inputs) it divides that by), and the closed-form
   trainer (its gain, 2/sqrt(inputs), and the diagonal of every Cholesky
   step). A compiler's square root is one instruction on a laptop, but on the
   ESP32-S3 it is a call to the C library's sqrtf, and on GCC and Linux clang
   it also calls sqrtf for a negative input so that errno, the C library's
   error variable, can be set. A call
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

   THE PRICE IS SPEED: about 50 nanoseconds a call on the development laptop
   (an Apple M4 Max, Apple clang -O2), where the hardware instruction takes
   about 6. That adds about 45 nanoseconds to iris_novelty, 60 to
   iris_reseed, and 0.6 of the 5.0 microseconds a closed-form fit of 20
   demonstrations with 12 hidden units takes. It adds nothing to the
   neighbour searches (iris_knn_predict, iris_classify_1nn,
   iris_delete_nearest), which compare squared distances and never take a
   root. */
IRIS_API float iris_internal_sqrt(float x) {
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
IRIS_API float iris_internal_absf(float x) { return x < 0.0f ? -x : x; }

IRIS_API float iris_internal_clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

/* xorshift32. A tiny, fast, repeatable random number generator.

   Repeatable is the important word. The same seed always produces the same
   sequence, so the same seed always produces the same instrument. That is
   what makes "reroll" a real control rather than a shrug: you can go back.
   Each step mixes the 32-bit state with shifted copies of itself; a state
   of 0 would stay 0 for ever, which is why iris_reseed takes a seed of 0 as
   1. From any other state the step never reaches 0, so the replacement below
   of a 0 result by 0x9E3779B9 (2^32 divided by the golden ratio) is never
   taken by an instrument this file made; it keeps a zeroed generator handed
   in from outside from returning 0 for ever. */
typedef struct { uint32_t s; } iris_internal_rng;

IRIS_API uint32_t iris_internal_rand_u32(iris_internal_rng *r) {
  uint32_t x = r->s;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return (r->s = x ? x : 0x9E3779B9u);
}
/* Uniform over [-1, +1]: a random 32-bit pattern read as a signed integer and
   divided by 2^31. A float holds 24 significant bits, so the integers within
   64 of the largest round up to 2^31 when converted: exactly +1.0 comes out
   for 64 of the 2^32 patterns, and exactly -1.0 for 65. */
IRIS_API float iris_internal_rand_sym(iris_internal_rng *r) {
  return (float)(int32_t)iris_internal_rand_u32(r) * (1.0f / 2147483648.0f);
}

/* ==========================================================================
   PART 2 — THE STRUCTURE
   ========================================================================== */

struct iris {
  int32_t n_in, n_hid, n_out, cap;

  /* --- the network -------------------------------------------------------
     One hidden layer. Wekinator's default network has this shape, and there
     is a good reason beyond tradition: with ten or twenty training examples,
     a deeper network has far more capacity than data and simply memorises
     noise. One layer with a modest number of units is the right size for the
     amount of information a musician actually gives it.

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
  iris_internal_rng rng;
  int32_t trained;         /* the fit reflects the CURRENT example set      */
  int32_t fitted;          /* this instrument has EVER produced a fit.
                              iris_record and the delete functions clear
                              `trained` (the fit is stale) but must NOT clear
                              this (the instrument still plays). iris_predict
                              guards on this one.                           */
  float   last_error;
  int32_t status;          /* an iris_status; it stays until cleared (see
                              HEALTH REPORTING)                             */

  /* --- training progress, so a progress bar can tell the truth -----------
     Written by every trainer entry point. tr_ceiling is the budget the
     caller asked for; tr_done is how much of it has been spent. A converged
     run stops early, so tr_done/tr_ceiling is a LOWER bound on completion —
     iris_train_progress reports it as such and snaps to 1.0 when the run
     ends, which is the only way a plateau-stopped bar can be truthful. */
  int32_t tr_done, tr_ceiling, tr_running;
  int32_t tr_n_ex;         /* how many demonstrations the shuffle covers,
                              or -1 once a record or a delete has edited the
                              store under a sliced run (see the change test
                              in iris_internal_train_run) */
  float   tr_ref;          /* error one plateau-window ago */
  float   tr_err;          /* the last epoch's error, added up while the
                              weights moved: what the error floor and the
                              plateau test read. iris_last_error is a
                              different measurement, taken after the run. */
};

/* WHAT THE STATUS DOES AND DOES NOT COVER.

   It reports NUMERICAL HEALTH, and a full store: a poisoned value trapped at
   the door or before an output, a diverged or stuck run, a ridge escalation,
   a prediction from an instrument that was never fitted, a closed-form solve
   that collapsed to a constant. Those are the conditions a caller cannot
   detect for itself.

   It does NOT report an argument mistake: asking for demonstration 5,000 of
   twelve, say, or handing the closed-form trainer too little scratch. Those
   come back through the RETURN VALUE and leave the status alone, so that one
   out-of-range query cannot leave a polled screen showing a fault for ever.
   Check the return value of the call you made, and check the status for
   whether the instrument itself is in trouble. Two questions, two answers. */

/* THREADING, IN ONE SENTENCE.

       Never touch the same instrument from two places at once.

   That is the whole contract, and it is short because this file keeps no
   writable state outside the instrument you pass in: no global or static
   variables, no shared scratch. (Its one static object is a constant table
   in iris_suggest_smoothing, which nothing writes.) So two instruments
   cannot interact, on any number of cores.

   SAFE: many instruments on one core, one after another; one instrument per
   thread across as many cores as you have; one instrument used only inside an
   interrupt, but read the PLATFORM CAVEAT below before you do that last one.

   NOT SAFE: the SAME instrument from two threads, or from an interrupt and
   the main loop, even if both only play it. iris_predict, iris_knn_predict
   and iris_classify_1nn write inside the instrument (the network's working
   values, the status, and on an instrument never fitted the neighbour
   ranges), and iris_novelty fits those ranges too, which is why all four
   take a non-const iris *. An interrupt landing
   mid-call leaves both answers wrong. Give the interrupt its own instrument.
   Functions that take a const iris * write nothing, but reading an
   instrument while another thread writes it is still a race.

   PLATFORM CAVEAT, and it decides the interrupt case on the board this library
   is usually run on. Everything above is a statement about THIS CODE. It is
   not a promise about your chip. On an ESP32 under FreeRTOS, the real-time
   operating system its Arduino core runs on, the floating-point registers are
   not saved when an interrupt is taken (the core's configuration leaves
   CONFIG_FREERTOS_FPU_IN_ISR unset), so float arithmetic inside an interrupt
   handler, iris's or anyone's, can corrupt the interrupted task. iris is
   float throughout. So on that platform, do not call any iris_ function from
   an interrupt handler: read the sensor there, set a flag, and call iris from
   the main loop. The statement about the code stands wherever interrupt entry
   does save the floating-point registers.

   TIMING, for the audio case. One prediction of a 2-12-3 instrument takes
   14.9 microseconds on an ESP32-S3 at 240 MHz: the mean of 20,000 calls in
   the starter kit's device_torture sketch, test 9, run with iris 0.1.0 on
   two boards. No log of that run is recorded yet, and a mean is not a worst
   case. Against the 20.8 microseconds of one audio sample at 48 kHz that is
   1.4 times of margin, 72% of a core spent on playing alone. At a control
   rate of 1,000 predictions a second it is 1.5% of a core. On the
   development laptop one prediction takes about 0.04 microseconds. Training
   does not fit inside an audio callback: train in slices from the main loop
   (iris_train_slice), and never from an interrupt. */

/* HOW EVERY FUNCTION IN THIS FILE REPORTS FAILURE: two rules.

   BEFORE THE RULES, THE ONE LINE THAT ANSWERS "DID IT TRAIN?"

       trainer_of_your_choice(k);
       if (!iris_is_trained(k)) { ...it did not fit... }

   It is correct after EVERY trainer in this file, and no other test is.

   Here is why it has to exist. The two rules below are each sound, but they
   meet badly in C, and `if (trainer(...))` is wrong in BOTH directions
   depending on which trainer you called. The four trainers on the 20
   demonstrations of the reference task in tests/audit.c (2-12-3, seed 1234,
   each from a fresh reseed, the closed-form one at lam0 1e-4; the refusal is
   an empty store):

     on a fit that WORKED        return   if(return)   iris_is_trained
       iris_train                 1          true            1
       iris_continue(k, 600)      0.00012    true            1
       iris_continue_to_plateau   0.0000043  true            1
       iris_train_elm             0          FALSE           1   <-- best case

     on a fit that REFUSED
       iris_train                 0          false           0
       iris_continue(k, 600)     -1          TRUE            0   <-- -1 is truthy
       iris_continue_to_plateau  -1          TRUE            0
       iris_train_elm            -1          TRUE            0

   iris_train_elm returns the number of ridge doublings it needed, so 0 is
   its BEST outcome and reads as false. The warm trainers return a
   measurement, and -1 is an ordinary non-zero float, so a refusal reads as
   true. One more path returns -1 without being a refusal: when a gradient
   run meets a not-a-number partway (demonstrations spread wider than the
   largest float can span, say), its fit is lost, so it resets the
   instrument to its seed's starting weights, unfitted, sets
   IRIS_NAN_TRAPPED and returns -1 (iris_train returns 0), with
   iris_is_trained 0. iris_is_trained reads one flag that every trainer sets
   on success and none sets otherwise, so it gives the same answer whichever
   door you came in by. What it
   answers is whether the fit matches the demonstrations stored now: a call
   refused for a mistake in its arguments changes nothing, so an instrument
   trained before that call still reads 1 after it.

   RULE 1, for a call that either works or does not:
       0 means the call did nothing. Non-zero means it worked.
   That covers iris_init (a null pointer), iris_size, iris_record,
   iris_get, iris_train, iris_train_begin, the delete functions, iris_save
   and iris_load. iris_record returns the new demonstration's identifier,
   which is never 0 because identifiers start at 1, so it obeys the rule AND
   hands you the number you need later to delete that take; iris_get returns
   the identifier of the row it copied.

   RULE 2, for a call that returns a MEASUREMENT you asked for:
       the measurement on success, -1 on refusal.
   That covers iris_continue, iris_continue_to_plateau, iris_train_elm,
   iris_loo_error, iris_suggest_smoothing, iris_worst_example,
   iris_worst_example_id, iris_index_of, iris_id_at, iris_classify_1nn,
   iris_novelty and iris_example_stress. These cannot use rule 1 because 0 is
   often a good answer: a training error of 0 is a perfect fit, no ridge
   doublings is the best a solve can do, and a novelty of 0 means the
   reading is exactly on a demonstration.

   WHERE THE RULES DIFFER FOR SIMILAR QUESTIONS, so a table is worth having:
     - A position with no demonstration: iris_get answers 0 (rule 1: it
       copied nothing, and no identifier is 0) and iris_id_at answers -1
       (rule 2).
     - iris_train_elm's best outcome is 0 ridge doublings, which reads as
       false (the table above).
     - iris_set_smoothing returns nothing: a value outside 0 to 1 is clamped
       to the nearer end without a status, and iris_get_smoothing reports
       what was kept. Only a value that is not finite is refused, with
       IRIS_NAN_TRAPPED.
     - With no demonstrations stored, iris_predict writes 0 and reports
       IRIS_NOT_FITTED, while iris_knn_predict and iris_classify_1nn write 0
       (iris_classify_1nn also returns -1) and leave the status alone.

   THE READERS cannot fail and so answer every question: iris_count,
   iris_capacity, iris_seed, iris_is_trained, iris_last_error,
   iris_train_progress, iris_train_busy, iris_train_epochs_done and
   iris_get_smoothing answer a null instrument with 0 (iris_get_status with
   IRIS_NOT_FITTED). iris_train_slice returns 1 while its run has more to do
   and 0 once the run is over, whatever ended it.

   And a separate question, with a separate answer: is the INSTRUMENT in
   trouble? That is iris_get_status, below. A call can succeed on an
   instrument that is unwell, and a call can fail on a perfectly good one.
   A return value of 0 says the call did nothing; a status of 0
   (IRIS_STATUS_OK) says nothing is wrong.

   ---------------------------------------------------------------------- */

IRIS_API iris_status iris_get_status(const iris *k) {
  /* A null instrument is not healthy: `if (iris_get_status(k))` reads as "is
     something wrong?", and for a null pointer the answer is yes. */
  if (!k) return IRIS_NOT_FITTED;
  return (iris_status)k->status;
}

/* ==========================================================================
   PART 3 — SETUP
   ========================================================================== */

/* Bytes an instrument of this shape needs. Returns 0 for a shape that cannot
   be sized safely: any dimension out of range (cap above IRIS_MAX_EX
   included, see the note there), or an arena larger than this machine's
   size_t can hold, which on an AVR board is anything past 65,535 bytes.

   READ THIS BEFORE USING THE RETURN VALUE. 0 is a SENTINEL and it does not
   protect you on its own: size_t is unsigned, so `bytes < iris_size(...)` is
   FALSE when iris_size returns 0, and a caller using that idiom alone would
   sail past a bad shape rather than stop at it. iris_init does not use it
   (see the bound inside iris_init). Any other caller must test for 0
   explicitly. */
/* Bytes a shape occupies: IRIS_ARENA evaluated at run time, so the macro and
   this cannot disagree (the note on IRIS_ARENA says why it computes in
   unsigned long). 0 when the total does not fit a size_t on this machine. No
   opinion on whether the shape is sensible; that is iris_size. iris_init
   bounds the arena with this, not with iris_size, because a bound written
   `bytes < iris_size(...)` vanishes whenever iris_size refuses: `bytes < 0`
   is false on an unsigned type, so a 1-byte arena with n_hid 4 would pass
   and training would write 611 bytes past it. */
static size_t iris_internal_bytes(int n_in, int n_hid, int n_out, int cap) {
  unsigned long total = IRIS_ARENA(n_in, n_hid, n_out, cap);
  if (total > (unsigned long)(size_t)-1) return 0;   /* will not fit a pointer */
  return (size_t)total;
}

IRIS_API size_t iris_size(int n_in, int n_hid, int n_out, int cap) {
  if (n_in < 1 || n_in > IRIS_MAX_IN)   return 0;
  if (n_out < 1 || n_out > IRIS_MAX_OUT) return 0;
  /* Floor of 8, not 1. Below 8 the network cannot represent the mappings
     this library is for, and the closed-form trainer refuses such a width
     (PART 8d). n_hid is written into the saved file and iris_load refuses a
     mismatch, so the width chosen on day one is that instrument's width for
     ever: the floor prevents a permanent mistake. */
  if (n_hid < 8 || n_hid > IRIS_MAX_HID) return 0;
  if (cap  < 1 || cap  > IRIS_MAX_EX)   return 0;
  return iris_internal_bytes(n_in, n_hid, n_out, cap);
}

/* The momentum velocities back to zero: the state of every fresh start
   (iris_reseed), of a closed-form solve (PART 8d), whose weights no velocity
   describes, and of a loaded instrument (PART 9), whose file does not carry
   them. */
IRIS_API void iris_internal_zero_velocity(iris *k) { if (!k) return;
  for (int i = 0; i < k->n_hid * k->n_in;  ++i) k->v_w1[i] = 0.0f;
  for (int i = 0; i < k->n_hid;            ++i) k->v_b1[i] = 0.0f;
  for (int i = 0; i < k->n_out * k->n_hid; ++i) k->v_w2[i] = 0.0f;
  for (int i = 0; i < k->n_out;            ++i) k->v_b2[i] = 0.0f;
}

/* Randomise the weights. This is the reroll.

   The scale matters. Each hidden unit adds up n_in incoming signals, so if
   the weights are too large the sum lands far out where tanh is flat, the
   error signal underneath it goes to nearly zero, and the network stops
   learning before it starts. Scaling each weight by one over the square root
   of the number of inputs to its unit keeps the sums in the responsive part
   of the curve. This is a standard trick and it is the difference between
   "trains in 50 ms" and "never trains at all".

   The code multiplies each draw by that reciprocal, rounded once to a float
   (s1 and s2 below), rather than dividing each draw by the square root. The
   two round differently: over seeds 1 to 1,000 at 2 inputs, 12 hidden units
   and 3 outputs, dividing would give a different float for 12 to 37 of the
   60 weights. The multiplication is the one the golden hash pins.

   A seed of 0 is taken as 1, because the random number generator cannot
   start from 0, so seeds 0 and 1 give the same instrument.

   TO REROLL a trained instrument, give it a new seed and train again:
   iris_reseed(k, new_seed) then iris_train(k), or iris_train_elm for the
   closed-form trainer (PART 8d). The random state starts again from the new
   seed. The new starting weights replace the old ones at once, so if the
   trainer then refuses -- no demonstrations, say -- the instrument is left
   unfitted and plays as one (IRIS_NOT_FITTED) until a trainer succeeds.

   A sliced run in flight ends here, as it does at iris_clear, a closed-form
   solve and a load: its remaining slices would train the new seed's weights
   inside the old run's session, with its shuffle, its epoch count and its
   plateau reference, and give an instrument that neither seed reproduces.
   iris_train_slice returns 0 from then on; iris_train or iris_train_begin
   starts the run for the new seed. */
IRIS_API void iris_reseed(iris *k, uint32_t seed) { if (!k) return;
  k->seed = seed ? seed : 1u;
  k->rng.s = k->seed;
  const float s1 = 1.0f / iris_internal_sqrt((float)(k->n_in  > 0 ? k->n_in  : 1));
  const float s2 = 1.0f / iris_internal_sqrt((float)(k->n_hid > 0 ? k->n_hid : 1));
  for (int i = 0; i < k->n_hid * k->n_in;  ++i) k->w1[i] = iris_internal_rand_sym(&k->rng) * s1;
  for (int i = 0; i < k->n_hid;            ++i) k->b1[i] = 0.0f;
  for (int i = 0; i < k->n_out * k->n_hid; ++i) k->w2[i] = iris_internal_rand_sym(&k->rng) * s2;
  for (int i = 0; i < k->n_out;            ++i) k->b2[i] = 0.0f;
  iris_internal_zero_velocity(k);
  k->trained = 0;
  k->fitted  = 0;          /* random weights are not a fit */
  k->last_error = 1.0f;
  k->status = IRIS_STATUS_OK;
  k->tr_running = 0;
}

/* The learning rate and momentum every instrument starts with, 0.10 and 0.85,
   and gets back when a file is loaded into it: the file does not carry them,
   and every instrument this library saves trained with them unless
   iris_internal_set_learning (below) changed them. */
IRIS_API void iris_internal_default_learning(iris *k) {
  k->lr = 0.10f;
  k->momentum = 0.85f;
}

IRIS_API iris *iris_init(void *mem, size_t bytes, int n_in, int n_hid, int n_out,
                   int cap, uint32_t seed) {
  if (!mem) return 0;
  if (n_in  < 1 || n_in  > IRIS_MAX_IN ) return 0;
  if (n_out < 1 || n_out > IRIS_MAX_OUT) return 0;
  /* Floor of 8, as in iris_size, so the two agree on which shapes exist. */
  if (n_hid < 8 || n_hid > IRIS_MAX_HID) return 0;
  if (cap   < 1 || cap > IRIS_MAX_EX) return 0;      /* see IRIS_MAX_EX: overflow */
  /* The arena bound, through iris_internal_bytes (see the note there). */
  { size_t need = iris_internal_bytes(n_in, n_hid, n_out, cap);
    /* need == 0: this shape cannot be sized on this machine. Test it first;
       the unsigned comparison below cannot see a 0. */
    if (need == 0) return 0;
    if (bytes < need) return 0; }

  unsigned char *p = (unsigned char *)mem;
  /* Align BEFORE placing the structure, and again after it. The caller's
     arena is only guaranteed 1-byte aligned (the usage block declares it as
     `unsigned char mem[...]`), and a structure left wherever the arena
     starts is a misaligned access: a sanitizer reports it, and a chip that
     faults on unaligned loads crashes. The 64 bytes of slack in IRIS_ARENA
     pay for this padding; at most 14 of them are used. */
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
  /* order[] is carved last, and iris_suggest_smoothing's snapshot of the
     instrument ends where it ends: an array carved after it must extend that
     snapshot. tests/train.c compares the whole arena after a suggestion, so
     an array the snapshot misses fails there. */
  k->order = (int32_t *)p;
  /* Filled here, so the shuffle buffer never holds memory nobody wrote: the
     trainer's shuffle reads and writes through it, and an unfilled slot
     reached by a record made during a sliced run would be a crash on a dirty
     arena. The trainer also refills order[] at the start of every run and
     whenever the demonstration count changes under a running slice, so this
     loop is a second line of defence that no test can observe: 300 trials of
     random mid-run records, deletes and slices on deliberately dirty arenas,
     under AddressSanitizer and UndefinedBehaviorSanitizer, give the same
     result hash, 0x3920621C, with it and without it. It costs one loop at
     construction. */
  for (int i = 0; i < cap; ++i) k->order[i] = i;

  k->n_ex = 0; k->next_id = 1;
  iris_internal_default_learning(k); k->l2 = 0.0f;
  for (int i = 0; i < cap; ++i) k->ex_res[i] = 0.0f;
  k->res_epochs = 0;
  k->tr_done = 0; k->tr_ceiling = 0; k->tr_running = 0; k->tr_ref = 0.0f; k->tr_err = 0.0f;
  for (int i = 0; i < n_in;  ++i) { k->in_lo[i]  = 0.0f; k->in_hi[i]  = 1.0f; }
  for (int i = 0; i < n_out; ++i) { k->out_lo[i] = 0.0f; k->out_hi[i] = 1.0f; }
  iris_reseed(k, seed);
  return k;
}

/* INTERNAL: the learning rate and momentum are not part of the interface,
   because a musician cannot choose them well and a wrong choice destroys the
   instrument. Measured with iris_train's plateau run on six synthetic target
   shapes, output noise 0.05, 40 seeds each:

   MOMENTUM 0.99, one nudge from the 0.85 default, diverges 11 to 39 of 40
   runs depending on the target, against none at the default. A diverged
   run leaves a weight pinned on IRIS_W_LIMIT, and every warm trainer then
   refuses the instrument with IRIS_DIVERGED_STUCK until iris_train starts it
   over.

   A LEARNING RATE OF 2.0 diverges only 4 of 16 runs on a clean smooth
   target, but leaves 15 of the 16 more than five times worse than the
   default on held-out error, 12 of them with a healthy status.

   THE SAFE RANGES BUY LITTLE THAT SMOOTHING DOES NOT. Tuned per task by an
   oracle (24 synthetic tasks at 3 noise levels), the learning rate, the
   weight decay and the epoch ceiling each improve held-out error by 22% to
   59% over the defaults and land within 8-10% of one another: three ways of
   saying how hard to chase the demonstrations. Smoothing is the one of the
   three that is exposed.

   AND THE ONE READOUT A SCREEN CAN SHOW POINTS BACKWARDS. At noise 0.10,
   across 7 learning rates on each of the 24 tasks, training error and
   held-out error move in opposite directions (mean rank correlation -0.63,
   where -1 would mean every step down in one is a step up in the other),
   and the setting with the lowest training error is the worst or
   second-worst instrument in 14 of the 24. A student tuning by the error
   readout picks the worst setting on offer.

   Kept as an internal hook: the tests use it to force divergences and wild
   settings (tests/audit.c, tests/train.c) and to check that a load puts the
   defaults back (tests/load.c). The clamps allow Weka's own pair, 0.3 and
   0.2. */
IRIS_API void iris_internal_set_learning(iris *k, float lr, float momentum) { if (!k) return;
  /* REFUSE A NOT-A-NUMBER BEFORE CLAMPING IT. iris_internal_clampf is a
     ternary on two comparisons, and every comparison with not-a-number is
     false, so one would fall straight through the clamp and into the
     instrument. */
  if (iris_internal_isbad(lr) || iris_internal_isbad(momentum)) {
    k->status = IRIS_NAN_TRAPPED;
    return;
  }
  k->lr = iris_internal_clampf(lr, 0.0001f, 2.0f);   /* allows Weka's 0.3; see above */
  k->momentum = iris_internal_clampf(momentum, 0.0f, 0.99f);
}

/* WEIGHT DECAY, the mechanism under smoothing. Each training step shrinks
   every weight by a small fraction of itself, which pulls the network toward
   flatter mappings between the demonstrations. The reason to want that: a
   2-12-3 network has 75 weights and biases, 20 demonstrations give it 60
   target numbers to fit, and trained to a plateau on noisy takes it has the
   freedom to bend through the noise. The decay is the price it pays for
   bending.

   Applied as decoupled decay (to the weight itself, not folded into the
   gradient, so it does not build up in the momentum) and to the weights
   only, never the biases: penalising a bias just shifts the function, for no
   reduction in its freedom. Clamped to 0.3, the value smoothing 1 maps onto.

   Not comparable to scikit-learn's alpha, although the scale looks familiar:
   the ranges this file normalises by come from the demonstrations themselves
   (PART 5), and noise widens them, so the effective penalty moves with the
   number of demonstrations and with the noise. */
IRIS_API void iris_internal_set_l2(iris *k, float l2) { if (!k) return;
  /* See iris_internal_set_learning: a NaN passes straight through a clamp. */
  if (iris_internal_isbad(l2)) { k->status = IRIS_NAN_TRAPPED; return; }
  k->l2 = iris_internal_clampf(l2, 0.0f, 0.3f);   /* 0.3, not 1.0 — see smoothing */
}
IRIS_API float iris_internal_get_l2(const iris *k) { if (!k) return 0.0f; return k->l2; }

/* SMOOTHING — the one quality knob, in the musician's own terms.

   0 = stick tightly to my demonstrations, whatever they say.
   1 = smooth confidently between them, forgiving my shaky takes.

   It is the only setting that changes how good the instrument is rather
   than how big or how fast it is. It maps onto weight decay, 0.3 times the
   smoothing (above), but nobody should have to know that to use it.

   WHAT IT BUYS, AND WHAT IT COSTS. Measured with iris_train on six synthetic
   target shapes (2 inputs, 12 hidden units, 3 outputs), 12 seeds each, with
   Gaussian noise of standard deviation sigma added to the demonstrated
   outputs; root-mean-square error on held-out points against the clean
   target:

       demonstrations   noise sigma    smoothing 0    smoothing 0.33
             10            0             0.0940           0.1094
             10            0.10          0.2070           0.1421
             20            0             0.0604           0.0780
             20            0.10          0.2264           0.1082
             50            0             0.0273           0.0453
             50            0.10          0.1044           0.0723

   On noisy takes smoothing repairs the plateau run, which otherwise fits the
   noise: at sigma 0.05 and above, smoothing 0 is 1.2 to 2.2 times worse than
   simply stopping after 100 epochs. On clean takes it costs 16%, 29% and 66%
   more held-out error at 10, 20 and 50 demonstrations. What it buys depends
   on the target: at sigma 0.10, smoothing 1 against smoothing 0 ranges from
   no gain to 4.1 times better across the six shapes, and on one periodic
   target at sigma 0.05 it is worse. Recall of the demonstrations gets
   steadily worse as smoothing rises, as it should; held-out error usually
   has its best value somewhere inside the range, not at either end. Every
   run above 0 in those measurements ended with a healthy status (0 of
   5,760).

   The default is 0, stick to the demonstrations, because a musician who has
   not asked for smoothing should get exactly what they showed it, and
   because no single value serves both clean and noisy takes. If your takes
   are noisy, audition a few values, or ask iris_suggest_smoothing for a
   starting point. */
IRIS_API void iris_set_smoothing(iris *k, float amount) { if (!k) return;
  /* Guard here too: a not-a-number times 0.3 is still one, so the inner guard
     would catch it, but this one refuses before any arithmetic. */
  if (iris_internal_isbad(amount)) { k->status = IRIS_NAN_TRAPPED; return; }
  iris_internal_set_l2(k, iris_internal_clampf(amount, 0.0f, 1.0f) * 0.3f);
}
IRIS_API float iris_get_smoothing(const iris *k) { if (!k) return 0.0f; return k->l2 / 0.3f; }

/* ==========================================================================
   PART 4 — THE EXAMPLE STORE

   Add, inspect, delete. Deleting one demonstration is a five-line function,
   and its absence is the biggest usability failure of the embedded systems
   that have attempted this. One mistimed button press should not cost you
   twenty minutes of work.

   Every stored demonstration has two numbers. Its IDENTIFIER is handed out
   by iris_record, starts at 1, is never reused, and never changes, so
   "delete #3" always means the same take. Its INDEX is its position in the
   store, 0 to iris_count - 1, and shifts down when an earlier one is
   deleted.
   ========================================================================== */

IRIS_API int iris_count(const iris *k) { if (!k) return 0; return k->n_ex; }
IRIS_API int iris_capacity(const iris *k) { if (!k) return 0; return k->cap; }

/* THE IDENTIFIERS HAVE TO FIT THE MACHINE'S int, BECAUSE THEY ARE RETURNED AS
   ONE. They are stored as int32_t, and iris_record, iris_get, iris_id_at,
   iris_classify_1nn and iris_worst_example_id hand them back as int. Where
   int is 16 bits, every Arduino AVR board, identifier 32,768 would come back
   as -32,768, 65,535 as -1 (the answer that means "none") and 65,536 as 0
   (the answer that means "refused"), and iris_delete_id could not name them.
   So an identifier stays below this: 2^31 - 1, the largest int32_t, where int
   has 32 bits, and 32,767 where it has 16. iris_record refuses once next_id
   reaches it, and iris_load refuses a file whose next_id is not below it, so
   a laptop's file with identifiers past 32,766 does not load on an AVR board
   (PART 9). Identifiers are never reused, so it counts every take ever
   recorded into the instrument, deleted ones included. */
#define IRIS_ID_LIMIT ((int32_t)(sizeof(int) >= 4 ? 0x7FFFFFFFL : 0x7FFFL))

/* LENGTHS, same rule as iris_predict and just as unchecked.
   Reads exactly n_in floats from `in` and n_out from `out`. */
IRIS_API int iris_record(iris *k, const float *in, const float *out) { if (!k) return 0;
  if (k->n_ex >= k->cap) { k->status = IRIS_STORE_FULL; return 0; }
  /* Identifiers are never reused, so they can run out: once next_id reaches
     IRIS_ID_LIMIT, handing it out would give an identifier the return type
     cannot carry, or adding one would overflow. */
  if (k->next_id >= IRIS_ID_LIMIT) return 0;

#ifndef IRIS_NO_GUARDS
  /* REFUSE A POISONED DEMONSTRATION AT THE DOOR. A not-a-number or an
     infinity from a glitched or unplugged sensor, once stored, makes every
     trainer refuse (see iris_internal_trainable), so one bad frame would
     block every training run until the musician worked out which take to
     delete. Refused here, the store never holds a value that can poison a
     fit, the instrument keeps playing, and the caller finds out at once, from
     the return value and IRIS_NAN_TRAPPED. iris_load refuses such a value
     too; the trainers' own test stays as defence in depth, for a store
     written some other way. */
  {
    for (int i = 0; i < k->n_in;  ++i)
      if (iris_internal_isbad(in[i]))  { k->status = IRIS_NAN_TRAPPED; return 0; }
    for (int i = 0; i < k->n_out; ++i)
      if (iris_internal_isbad(out[i])) { k->status = IRIS_NAN_TRAPPED; return 0; }
  }
#endif

  const int stride = k->n_in + k->n_out;
  float *row = k->ex + (size_t)k->n_ex * stride;
  for (int i = 0; i < k->n_in;  ++i) row[i] = in[i];
  for (int i = 0; i < k->n_out; ++i) row[k->n_in + i] = out[i];
  k->ex_id[k->n_ex] = k->next_id++;
  k->n_ex++;
  k->trained = 0;                                   /* model is now stale */
  if (k->tr_running) k->tr_n_ex = -1;   /* a sliced run must re-read the store */

  /* A take was just stored, so the store is not full: clear IRIS_STORE_FULL.
     Without this, `if (iris_get_status(k))` would answer "something is
     wrong" for the rest of the instrument's life after one full store.

     ONLY IRIS_STORE_FULL. iris_record is one of several calls that raise
     IRIS_NAN_TRAPPED (the trainers, the playing and neighbour functions and
     the setters raise it too), and storing one good number does not disprove
     a not-a-number that training trapped. Only iris_record raises
     IRIS_STORE_FULL, so only iris_record retracts it. A bad reading does not
     alarm for ever either: the next training run clears the status, which
     is the point at which the instrument is known to be well again. */
  if (k->status == IRIS_STORE_FULL)
    k->status = IRIS_STATUS_OK;

  return (int)k->ex_id[k->n_ex - 1];   /* below IRIS_ID_LIMIT, so it fits */
}

IRIS_API int iris_index_of(const iris *k, int id) { if (!k) return -1;
  for (int i = 0; i < k->n_ex; ++i) if (k->ex_id[i] == id) return i;
  return -1;
}

/* The identifier at a position, without copying the row out. */
IRIS_API int iris_id_at(const iris *k, int idx) { if (!k) return -1;
  return (idx < 0 || idx >= k->n_ex) ? -1 : (int)k->ex_id[idx];
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
  return (int)k->ex_id[idx];
}

IRIS_API int iris_delete_index(iris *k, int idx) { if (!k) return 0;
  if (idx < 0 || idx >= k->n_ex) return 0;
  const int stride = k->n_in + k->n_out;
  for (int r = idx; r < k->n_ex - 1; ++r) {
    float *dst = k->ex + (size_t)r * stride;
    const float *src = k->ex + (size_t)(r + 1) * stride;
    for (int c = 0; c < stride; ++c) dst[c] = src[c];
    k->ex_id[r]  = k->ex_id[r + 1];
    /* The residual ledger (PART 8f) is indexed by POSITION, so it moves with
       the rows; otherwise, until the next training run, "which take is
       fighting the others" would name the demonstration that slid into the
       deleted one's place. */
    k->ex_res[r] = k->ex_res[r + 1];
  }
  k->ex_res[k->n_ex - 1] = 0.0f;
  k->n_ex--;
  k->trained = 0;
  if (k->tr_running) k->tr_n_ex = -1;   /* a sliced run must re-read the store */
  return 1;
}

IRIS_API int iris_delete_id(iris *k, int id) { if (!k) return 0; return iris_delete_index(k, iris_index_of(k, id)); }
IRIS_API int iris_delete_last(iris *k) { if (!k) return 0; return iris_delete_index(k, k->n_ex - 1); }

/* The nearest-demonstration search, defined in PART 10 beside the neighbour
   functions that share it. */
IRIS_API int iris_internal_nearest(iris *k, const float *in);

/* Delete whichever demonstration is closest to where you are standing now.
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
   not finite (which also reports IRIS_NAN_TRAPPED, and changes nothing
   else), or the instrument's shape is too big for this translation unit
   (see iris_internal_shape_fits). On an instrument that has never been
   fitted it fits the ranges first, as the neighbour functions do. */
/* LENGTHS, same rule as iris_predict and just as unchecked.
   Reads exactly n_in floats from `in`. */
IRIS_API int iris_delete_nearest(iris *k, const float *in) { if (!k) return 0;
  return iris_delete_index(k, iris_internal_nearest(k, in));
}

IRIS_API void iris_clear(iris *k) {
  if (!k) return;
  k->n_ex = 0; k->trained = 0; k->fitted = 0; k->last_error = 1.0f;
  /* Every demonstration goes, and with them the fit: the instrument is no
     longer fitted and plays 0 until it is trained again. Any sliced run in
     flight ends too. The trainer returns at once when there are no
     demonstrations, so without this a run would never advance or finish,
     and the loop
         while (iris_train_slice(k, 500)) { draw(); poll(); }
     would never end: a sketch with a clear button and sliced training is
     one press from that. */
  k->tr_running = 0; k->tr_done = 0; k->tr_n_ex = 0; k->tr_ref = 0.0f; k->tr_err = 0.0f;
  /* The worst-demonstration ledger (PART 8f) goes with the demonstrations it
     describes. It is indexed by position, so without this the takes recorded
     next would read the cleared takes' sums until the next training run:
     in tests/train.c, twelve clean takes recorded after a clear inherit a
     bad take's sum, and iris_worst_example names a clean one with a margin
     of 3.45, past IRIS_STRESS_FLAG. Emptied, the ledger's readers answer -1,
     as on a fresh instrument, and with the ceiling at 0 as well
     iris_train_progress reads 0.0: no run has started on these
     demonstrations. */
  for (int i = 0; i < k->cap; ++i) k->ex_res[i] = 0.0f;
  k->res_epochs = 0; k->tr_ceiling = 0;
}

/* ==========================================================================
   PART 5 — NORMALISATION

   Find the range of every input and output across the demonstrations, then
   map everything onto a common scale before training.

   Outputs go to 0.1-0.9 rather than 0-1 on purpose. The output layer uses a
   sigmoid. A true logistic only APPROACHES 0 and 1, so a target of exactly 1
   would push a weight toward infinity for ever. This sigmoid, built on the
   clamped rational of PART 1, does reach 1 (iris_internal_sigmoid(6.0f) is
   1.0f exactly), but the training signal through it, y*(1-y), is 0 there,
   so a target at either end would still drive the output into the flat
   region where it stops learning. Leaving headroom at both ends means the
   network can actually arrive.

   WHERE THE CONSTANT COMES FROM. 0.1/0.9 is a rule of thumb, not a derived
   value. LeCun's "Efficient BackProp" (section 4.5) derives a band from the
   maximum of the sigmoid's second derivative, 0.2113/0.7887, and against it
   0.1/0.9 gives 1.85 times less gradient at the targets (docs/MATH-AUDIT.md
   works it out). It stays because it is part of what a saved instrument plays
   (PART 9), not because it was measured to be better. It was not.
   ========================================================================== */

#define IRIS_OUT_LO 0.1f
#define IRIS_OUT_HI 0.9f

/* The largest finite float, which <float.h> calls FLT_MAX. This file includes
   no library headers, so it spells the number out. */
#define IRIS_FLT_MAX 3.40282347e+38f

/* The smallest and largest value that column c of the example store takes
   across the demonstrations (c counts the inputs first, then the outputs).
   Called only with at least one demonstration.

   The search starts from the first demonstration's value, not from a big
   round number standing in for "larger than anything", so every finite value
   takes part however large it is (tests/playing.c records values beyond
   1e30, and the largest float itself). */
IRIS_API void iris_internal_span(const iris *k, int c, float *lo, float *hi) {
  const int stride = k->n_in + k->n_out;
  float a = k->ex[c], b = a;
  for (int r = 1; r < k->n_ex; ++r) {
    const float v = k->ex[(size_t)r * stride + c];
    if (v < a) a = v;
    if (v > b) b = v;
  }
  *lo = a; *hi = b;
}

/* THE RANGES. Every input and every output gets the smallest and largest value
   the demonstrations gave it. Every trainer calls this before it starts; the
   neighbour functions (PART 10), iris_delete_nearest and iris_novelty call it
   on an instrument that has never been fitted. With no demonstrations it
   leaves the ranges as they are.

   AN INPUT THAT NEVER MOVED IS IGNORED. A switch left in one position, a
   sensor resting against its rail, a light sensor under steady light: an input
   that read the same in every demonstration tells the instrument nothing about
   what you want, and dividing by its width would turn the smallest wobble at
   play time into an enormous number. So the rule is:

       an input is STILL when its width, hi - lo, is at most 1e-5 of its
       magnitude (the larger of |lo| and |hi|), or at most 1e-6.

   A still input is stored with zero width (in_hi = in_lo), and
   iris_internal_norm_in gives it the value 0 -- in training and in playing,
   whatever it reads, so moving it cannot change what the instrument plays.
   A float carries about seven significant digits, so 1e-5 of the magnitude
   is fewer than 170 steps of the float's own resolution: a range that narrow
   is rounding and sensor noise, not a gesture. The 1e-6 covers inputs
   resting near zero, where a relative test alone would demand an exact zero.

   Why ignore it rather than give it a small width. Dividing by a width that
   small magnifies any movement at play time: an input held at 500 in every
   demonstration and given a width of 0.005 normalises to about 400 when it
   reads 501, where the demonstrations taught the network only [-1,+1].
   With such a floor in place of this rule, on the six demonstrations
   tests/playing.c uses with the still input at 500, a sweep of the other
   input spans 9.97 of the 10 demonstrated with the still input at 500, and
   0.0000 with it at 501: every hidden unit saturates and the instrument
   becomes a constant.

   AN OUTPUT THAT NEVER MOVED keeps a small nonzero width, because the network
   is trained toward it and the output scaling divides by the width. The rule:

       an output's width is at least max(1e-5 * |lo|, 1e-6), where lo is the
       smallest value it was shown; a narrower output gets hi = lo + that,
       or, when lo + that would pass the largest float, lo = hi - that.

   That floor is relative for the reason above: an absolute 1e-6 added to a
   value above 32 changes nothing in 32-bit floating point, because the gap
   between representable numbers there is already wider, so the width would
   stay zero and every prediction would be not-a-number. An absolute floor
   works up to 31.77 and fails from 32.72.

   The widening goes downward only at the very top of the float range: an
   output shown nothing below about 3.40279e38, within 1e-5 of the largest
   float, would get hi = infinity, and then the output scaling, the
   substitute a playing function writes (the centre of the range) and the
   saved file would all hold an infinity. Widened downward, both ends stay
   finite and lo < hi, so the instrument plays finite numbers and saves
   (tests/playing.c trains one with each trainer at the largest float).

   A LIMIT, STATED RATHER THAN GUARDED. A width is a float, so demonstrations
   that span more than the largest float, about 3.4e38 end to end (values
   beyond about 1.7e38 on both sides of zero), give a width that overflows to
   infinity. Normalising a demonstration at either end of such a range then
   gives not-a-number, so training traps it partway through and leaves the
   instrument reseeded and unfitted (two inputs, one demonstrated at -2e38
   and 2e38: iris_train returns 0 with IRIS_NAN_TRAPPED), and iris_save
   refuses an instrument holding such a range (PART 9). No sensor reads
   numbers of that size. */
IRIS_API void iris_internal_fit_ranges(iris *k) { if (!k) return;
  if (k->n_ex == 0) return;
  for (int i = 0; i < k->n_in; ++i) {
    float lo, hi;
    iris_internal_span(k, i, &lo, &hi);
    const float alo = iris_internal_absf(lo), ahi = iris_internal_absf(hi);
    const float mag = alo > ahi ? alo : ahi;
    float negligible = mag * 1e-5f;
    if (negligible < 1e-6f) negligible = 1e-6f;
    k->in_lo[i] = lo;
    k->in_hi[i] = (hi - lo <= negligible) ? lo : hi;     /* still: zero width */
  }
  for (int o = 0; o < k->n_out; ++o) {
    float lo, hi;
    iris_internal_span(k, k->n_in + o, &lo, &hi);
    float w = iris_internal_absf(lo) * 1e-5f;  if (w < 1e-6f) w = 1e-6f;
    if (hi - lo < w) {
      const float up = lo + w;
      if (up <= IRIS_FLT_MAX) hi = up;   /* upward, unless it overflows: */
      else lo = hi - w;                  /* then downward, see above     */
    }
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

   A STILL INPUT (zero width, see iris_internal_fit_ranges) maps to 0 for every
   finite reading. It is written v - v rather than 0 so that a reading which is
   not finite -- a disconnected or broken sensor -- still comes out as
   not-a-number, and the guards downstream still report it. */
IRIS_API float iris_internal_norm_in(const iris *k, int i, float v) {
  if (!k || i < 0 || i >= k->n_in) return 0.0f;
  const float w = k->in_hi[i] - k->in_lo[i];
  if (w <= 0.0f) return v - v;
  const float t = (v - k->in_lo[i]) / w;
  return 2.0f * t - 1.0f;
}

IRIS_API float iris_internal_norm_out(const iris *k, int i, float v) {
  if (!k || i < 0 || i >= k->n_out) return 0.0f;
  float t = (v - k->out_lo[i]) / (k->out_hi[i] - k->out_lo[i]);
  return IRIS_OUT_LO + t * (IRIS_OUT_HI - IRIS_OUT_LO);
}
IRIS_API float iris_internal_denorm_out(const iris *k, int i, float y) {
  if (!k || i < 0 || i >= k->n_out) return 0.0f;
  float t = (y - IRIS_OUT_LO) / (IRIS_OUT_HI - IRIS_OUT_LO);
  return k->out_lo[i] + t * (k->out_hi[i] - k->out_lo[i]);
}

/* ==========================================================================
   PART 6 — FORWARD PASS  (this is "playing the instrument")

     hidden_h = tanh( sum_i w1[h][i] * x_i + b1[h] )
     output_o = sigmoid( sum_h w2[o][h] * hidden_h + b2[o] )

   That is the network. Each hidden unit adds up the inputs, each scaled by
   its own weight, adds its bias, and squashes the sum; each output does the
   same with the hidden units' answers. A single unit can only draw one soft
   step across the input space. Adding a dozen of them, each with its step in
   a different place and direction, is what lets the output bend into the
   shape you demonstrated, and training (PART 8) is the search for weights
   that put the steps in the right places.

   It is NOT the whole of what iris_predict does; the full chain, which is
   what plays, is:

     x_i   = iris_internal_norm_in(k, i, your_reading)    scale the sensor in
     ...the two lines above...
     out_o = iris_internal_denorm_out(k, o, output_o)     scale the sound out
     out_o = iris_internal_clampf(out_o, out_lo[o], out_hi[o])
                                                          and hold it in range

   Four steps, two of them arithmetic on ranges the instrument measured for
   itself, and two matrix multiplies with a squashing function after each.
   For 2 inputs, 12 hidden and 3 outputs that is 60 multiply-adds and 20
   float divisions (one in each input's scaling, one in each of the 15
   squashing functions, one in each output's scaling back), and on the
   ESP32-S3, which divides in a library routine, the divisions are most of
   the cost. One whole prediction takes 14.9 microseconds there, a board
   figure awaiting a recorded log (the timing note above iris_get_status),
   and about 0.04 microseconds on the development laptop. Training the same
   instrument takes seconds, so playing is the cheap half.
   ========================================================================== */

/* The network alone, on inputs already normalised. It writes its working
   values -- the hidden and output activations -- into the instrument, which
   is why it takes a non-const instrument and why one instrument must not be
   played from two places at once. */
IRIS_API void iris_internal_forward_norm(iris *k, const float *x_norm) { if (!k) return;
  for (int h = 0; h < k->n_hid; ++h) {
    const float *w = k->w1 + (size_t)h * k->n_in;
    float s = k->b1[h];
    for (int i = 0; i < k->n_in; ++i) s += w[i] * x_norm[i];
    k->hid[h] = iris_internal_tanh(s);
  }
  for (int o = 0; o < k->n_out; ++o) {
    const float *w = k->w2 + (size_t)o * k->n_hid;
    float s = k->b2[o];
    for (int h = 0; h < k->n_hid; ++h) s += w[h] * k->hid[h];
    k->out[o] = iris_internal_sigmoid(s);
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
IRIS_API int iris_internal_shape_fits(const iris *k) {
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
  if (!iris_internal_shape_fits(k)) {
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
     iris_record and the delete functions clear `trained` -- the fit no
     longer reflects the current example set -- but the instrument is still a
     real instrument and must keep playing mid-performance (tests/playing.c
     holds that).
     `fitted` says "this has EVER produced a fit". It is cleared by
     iris_reseed and iris_clear, and by loading a file saved before any fit. */
  if (!k->fitted) {
    for (int o = 0; o < k->n_out; ++o) out[o] = iris_internal_centre(k, o);
    k->status = IRIS_NOT_FITTED;
    return;
  }
#endif

  for (int i = 0; i < k->n_in; ++i) x[i] = iris_internal_norm_in(k, i, in[i]);
  iris_internal_forward_norm(k, x);
  for (int o = 0; o < k->n_out; ++o) {
    float v = iris_internal_denorm_out(k, o, k->out[o]);
    out[o] = iris_internal_clampf(v, k->out_lo[o], k->out_hi[o]);
#ifndef IRIS_NO_GUARDS
    /* Last line of defence. iris_internal_clampf passes NaN straight through
       (every comparison with NaN is false), so a NaN here -- glitched sensor
       in, poisoned weight -- would land in an audio parameter. Substitute and
       say so. On a healthy run the bit test fails and this changes nothing. */
    if (iris_internal_isbad(out[o])) {
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

   Making the scale relative to the demonstrations' own spacing would fix
   that, and would change what every sketch that maps novelty hears, so the
   scale stays as it is and this note says what it means.

   This costs one pass over the demonstrations. But it lets the instrument
   know when it is improvising rather than recalling, which you can map to
   anything you like: noise, detuning, a light.

   THE RANGES are the ones the neighbour functions measure in (PART 10): the
   instrument's own once it has been fitted, and on an instrument that has
   never been fitted the ranges of the demonstrations it holds, fitted on
   every call. That fitting is the one thing iris_novelty writes, and why it
   takes a non-const instrument. Without it, an instrument that has only been
   shown takes would measure in the 0..1 ranges iris_init starts with, which
   describe nothing it was shown: with one input demonstrated at 0 and 1000,
   a reading of 10 would read 1 before the first training run and 0.04 after
   it; it reads 0.04 in both.

   A reading that is not finite reads as 1, as far from home as it gets, and
   is answered before any fitting, so then iris_novelty writes nothing, the
   status included. An empty store reads 1 as well.

   It refuses with -1, rule 2 of the failure rules above iris_get_status, for
   a null instrument and for a shape too big for this translation unit's
   working arrays (iris_internal_shape_fits). A distance is never negative,
   so a refusal cannot be mistaken for 0, a reading exactly on a take.
   ========================================================================== */

IRIS_API float iris_novelty(iris *k, const float *in) { if (!k) return -1.0f;
  if (!iris_internal_shape_fits(k)) return -1.0f;
  if (k->n_ex == 0) return 1.0f;
  for (int i = 0; i < k->n_in; ++i) if (iris_internal_isbad(in[i])) return 1.0f;
  if (!k->fitted) iris_internal_fit_ranges(k);
  const int stride = k->n_in + k->n_out;
  float best = 1e30f;
  for (int r = 0; r < k->n_ex; ++r) {
    const float *row = k->ex + (size_t)r * stride;
    float d = 0.0f;
    for (int i = 0; i < k->n_in; ++i) {
      float t = iris_internal_norm_in(k, i, row[i]) - iris_internal_norm_in(k, i, in[i]);
      d += t * t;
    }
    if (d < best) best = d;
  }
  float scale = iris_internal_sqrt((float)k->n_in) * 0.5f;
  return iris_internal_clampf(iris_internal_sqrt(best) / (scale > 0.0f ? scale : 1.0f),
                              0.0f, 1.0f);
}

/* ==========================================================================
   PART 8 — TRAINING  (backpropagation)

   The one idea in this file that needs explaining, and it is one idea:

     Run a demonstration forward. Compare what came out with what you
     demonstrated. Nudge every weight a little in whichever direction would
     have made that gap smaller. Repeat.

   "Which direction" is calculus: for each weight, how much the squared miss
   would change if that weight changed a little, which is its gradient. For
   a weight into an output unit that is the miss, times the slope of the
   sigmoid at that output, times the hidden value feeding the weight.
   "Backpropagation" is just the bookkeeping for the middle layer: a hidden
   unit has no target of its own, so its share of the blame is the output
   misses passed back through the weights that connect them, times the slope
   of its own tanh. Every weight then steps against its gradient, scaled by
   the learning rate, 0.10. Doing that after every single demonstration, in
   a shuffled order, is stochastic gradient descent.

   Two details that matter in practice:

   MOMENTUM. Instead of stepping purely downhill each time, keep a running
   velocity: each step is 0.85 of the previous step plus the new nudge.
   Steps in a consistent direction build up; steps that jitter back and
   forth cancel. On 20 demonstrations of a smooth 2-input, 3-output target it
   reaches a mean squared error of 1e-4 in 2,000 to 4,500 epochs, where the
   same run without momentum takes 8,700 to 28,000 (8 seeds): about five
   times fewer epochs, for one extra array.

   SHUFFLING. Present the demonstrations in a different order every epoch.
   A fixed order lets the network learn the order instead of the mapping: the
   last demonstration seen always gets the final say.
   ========================================================================== */

/* Guard sweep, run once per epoch: a not-a-number or infinity in any weight
   (or in the epoch's error) means the numbers are gone, so report it and
   recover to a finite state (iris_internal_trap_nan, below); a weight past
   IRIS_W_LIMIT means a divergence in progress, so clamp it, report and
   stop. It costs one pass over the
   weights per EPOCH, where backpropagation passes over them once per
   DEMONSTRATION, so it adds less than 1/n_ex to the work.                  */
#ifndef IRIS_NO_GUARDS
IRIS_API int iris_internal_check_weights(iris *k) { if (!k) return 0;
  const int nw = k->n_hid * k->n_in + k->n_hid + k->n_out * k->n_hid + k->n_out;
  /* w1,b1,w2,b2 are carved consecutively from the arena; walk them as one */
  float *w = k->w1;
  int worst = IRIS_STATUS_OK;
  for (int i = 0; i < nw; ++i) {
    if (iris_internal_isbad(w[i])) return IRIS_NAN_TRAPPED;
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

/* A NOT-A-NUMBER PARTWAY THROUGH A RUN. The stored demonstrations were all
   finite when the run began (iris_internal_trainable), so a not-a-number
   in the epoch's error or in a weight was made by the arithmetic itself:
   demonstrations spread wider than a float can span, whose width overflows
   to infinity (PART 5). The weights are then gone, so the run cannot hand
   back a fit. It puts the instrument where iris_reseed(k, iris_seed(k))
   puts it -- the starting weights the seed draws, the random state back at
   the seed, velocities zero, not fitted and not trained, iris_last_error 1
   -- empties the worst-demonstration ledger (PART 8f), which the failed
   epoch filled with not-a-number, ends any sliced run, and reports
   IRIS_NAN_TRAPPED. The trainer then returns -1, as it does when it
   refuses, and iris_train returns 0. Unlike a refusal the call has changed
   the instrument: it no longer plays its old fit, but the centre of the
   demonstrated range (IRIS_NOT_FITTED), until a trainer succeeds. */
IRIS_API float iris_internal_trap_nan(iris *k) {
  iris_reseed(k, k->seed);                 /* which also ends a sliced run */
  for (int i = 0; i < k->cap; ++i) k->ex_res[i] = 0.0f;
  k->res_epochs = 0;
  k->status = IRIS_NAN_TRAPPED;
  return -1.0f;
}
#endif

/* --------------------------------------------------------------------------
   TRAINING TO A PLATEAU, AND SAYING HOW FAR ALONG IT IS

   iris_train does not ask you for an epoch count. It runs until the
   training error PLATEAUS: every IRIS_CONV_WINDOW epochs it compares the
   error with the error one window earlier, and stops when the window bought
   less than IRIS_CONV_TOL of it. A short window (200-500 epochs) mistakes the
   ordinary epoch-to-epoch jitter of a shuffled run for a plateau and stops
   early (docs/adr/0017-train-to-the-plateau-not-to-a-constant.md
   records that measurement).

   WHAT THE PLATEAU BUYS: RECALL. On the reference task of tests/audit.c (20
   demonstrations, 2 inputs, 12 hidden units, 3 outputs), 600 epochs recall
   the demonstrations to a root-mean-square miss of 0.0066 and the plateau
   run, 18,000 epochs, to 0.0012, 5.5 times closer. On six synthetic target
   shapes with clean demonstrations the gain is 8.4, 4.2 and 2.8 times at
   10, 20 and 50 demonstrations.

   WHAT IT COSTS. Recall is not generalisation. On the same clean shapes the
   plateau also improves held-out error against 600 epochs, but by much less
   (7%, 7% and 19%). On noisy demonstrations a plateau run at smoothing 0
   fits the noise: it is 1.2 to 2.2 times worse on held-out error than
   stopping after a fixed 100 epochs (the table at iris_set_smoothing, where
   smoothing is the repair). Running on longer is worse still: a fixed
   60,000 epochs is 2% to 29% worse than the plateau under noise. None of
   this has been checked on recorded human gesture. Treat the ceiling as a
   ceiling, and smoothing as the knob for noisy takes.

   PROGRESS THAT DOES NOT LIE. A plateau run takes seconds on a board, long
   enough that a screen should show something true. Two ways in:

     - iris_train_begin / iris_train_slice / iris_train_progress run the SAME
       training as iris_train in slices, so a single-threaded sketch can draw
       a frame, read a touch and keep sound going between them. A sliced run
       is bit-identical to iris_train: iris_train_begin checks and reseeds
       exactly as iris_train does, the shuffle buffer is filled once there and
       carried across slices, and so the random draws are the same draws in
       the same order.
     - iris_continue_to_plateau(k, ceiling, cb, user), one of the warm
       trainers below, calls cb every window with (done, ceiling, err);
       returning 0 from cb ends the run, leaving a usable, partly trained
       instrument.

   iris_continue IS THE FIXED-EPOCH PATH: exactly the per-demonstration
   update of Weka's MultilayerPerceptron, the network Wekinator ships, with a
   fixed epoch count. tests/audit.c pins its output to the bit.
   -------------------------------------------------------------------------- */

#define IRIS_CONV_WINDOW  2000    /* epochs between plateau tests             */
#define IRIS_CONV_TOL     0.10f   /* stop when a window buys < 10% of the error */
/* THE CEILING HAS TO FIT THE MACHINE'S int, BECAUSE IT IS PASSED AS ONE.

   iris_train, iris_train_begin and iris_continue_to_plateau hand it to
   iris_internal_train_run as an int epoch count. Where int is 16 bits, every
   Arduino AVR board, 60000 would become -5536 (avr-gcc for the atmega328p),
   the trainer's `epochs <= 0` guard would refuse, iris_train would return 0,
   and a sketch that ignored the return value would play an instrument that
   was never fitted.

   30000 is the largest round number that fits a signed 16-bit int. It is not a
   compromise in practice: an 8-bit AVR at 16 MHz does not reach 30,000 epochs
   in a time anyone will wait for, so the ceiling is not the binding constraint
   there -- being positive is. */
#define IRIS_CONV_CEILING ((int)(sizeof(int) >= 4 ? 60000 : 30000))

/* Called every IRIS_CONV_WINDOW epochs. Return 0 to abort the run. */
typedef int (*iris_progress_fn)(void *user, int done, int ceiling, float err);

/* CAN THIS STORE BE TRAINED ON? Every trainer and diagnostic asks this
   FIRST, before it reseeds, fits ranges or touches a progress counter. So a
   refusal leaves the instrument as it was, and it goes on playing exactly as
   before.

   Three conditions. The shape fits this translation unit's working arrays
   (iris_internal_shape_fits). There is at least one demonstration. And every
   stored number is finite: a not-a-number would poison every weight in the
   first epoch.

   THE ONE WRITE. A shape that does not fit, or an empty store, is a mistake
   in how the call was made, and like every argument mistake it is reported
   by the return value alone (see the failure rules above iris_get_status). A
   stored number that is not finite is a matter of numerical health, which is
   what the status reports, so that refusal sets IRIS_NAN_TRAPPED -- and
   writes nothing else.

   iris_record and iris_load both refuse a non-finite number at the door, so
   this test is defence in depth, for a store that got one another way: a
   translation unit built with -DIRIS_NO_GUARDS recording into the same
   instrument, or a program writing into the store directly. The bad
   demonstration stays in the store where the musician can find it and
   delete it. The finiteness test is one of the guards, so a -DIRIS_NO_GUARDS
   build compiles it out, as it does iris_record's, and then nothing stops a
   not-a-number reaching the weights. */
IRIS_API int iris_internal_trainable(iris *k) {
  if (!iris_internal_shape_fits(k) || k->n_ex < 1) return 0;
#ifndef IRIS_NO_GUARDS
  {
    const int n = k->n_ex * (k->n_in + k->n_out);
    for (int i = 0; i < n; ++i)
      if (iris_internal_isbad(k->ex[i])) { k->status = IRIS_NAN_TRAPPED; return 0; }
  }
#endif
  return 1;
}

/* THE TRAINING ERROR OF THE CURRENT WEIGHTS, which is what iris_last_error
   reports: the mean squared error over every stored demonstration and every
   output, in the network's own output units (each output's demonstrated
   range spans 0.1 to 0.9, PART 5), from one forward pass per demonstration
   with the weights as they are now. Every trainer that fits calls it once
   when it finishes, and iris_load calls it, so the same weights and
   demonstrations give the same bits whichever way the instrument got them.
   The squares are added in one running sum, demonstration by demonstration
   and output by output, and divided once.

   It is not the figure the plateau test reads. That one (tr_err) is added up
   during an epoch while every visit moves the weights, so it describes no
   single set of weights: on the reference task of tests/audit.c trained by
   iris_train it is 4.57e-6 for the last epoch, where the weights the run
   ends with measure 4.29e-6.

   1 when the instrument is not fitted or holds no demonstrations, the value
   a fresh instrument reports, and 1 if the sum is not a number, which takes
   a demonstration so far outside the stored ranges that normalising it
   overflows a float. It writes the network's activations, and uses x, a
   working array of at least n_in floats that every caller already has: an
   array of its own would add IRIS_MAX_IN floats to the deepest stack frame
   of a training run, 128 bytes an Uno cannot spare. */
IRIS_API float iris_internal_recall_error(iris *k, float *x) {
  if (!k->fitted || k->n_ex < 1) return 1.0f;
  const int stride = k->n_in + k->n_out;
  float e = 0.0f;
  for (int r = 0; r < k->n_ex; ++r) {
    const float *row = k->ex + (size_t)r * stride;
    for (int i = 0; i < k->n_in; ++i) x[i] = iris_internal_norm_in(k, i, row[i]);
    iris_internal_forward_norm(k, x);
    for (int o = 0; o < k->n_out; ++o) {
      const float d = k->out[o] - iris_internal_norm_out(k, o, row[k->n_in + o]);
      e += d * d;
    }
  }
  e /= (float)(k->n_ex * k->n_out);
  return iris_internal_isbad(e) ? 1.0f : e;
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
  k->tr_err     = 0.0f;
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
   Returns the training error of the weights it ends with
   (iris_internal_recall_error, which iris_last_error then reports), or -1 if
   it refused or met a not-a-number partway. */
/* REFUSAL CONVENTION (one convention, whole library): a train call that did
   no training returns -1.0f and leaves `trained` alone, so a caller reading
   only the return value can tell a refusal from a repeat of the previous
   run. A run that meets a not-a-number partway returns -1.0f too, having
   reset the instrument to its seed's unfitted start
   (iris_internal_trap_nan). */
IRIS_API float iris_internal_train_run(iris *k, int epochs, int conv, int resume,
                           iris_progress_fn cb, void *user) { if (!k) return -1.0f;
  /* Refuse before writing anything else. epochs <= 0 is "do nothing", not
     "train instantly": without that test the loop below would never run,
     err would stay 0, and the tail would report a freshly randomised
     network as trained with a perfect fit. A poisoned demonstration sets IRIS_NAN_TRAPPED inside
     iris_internal_trainable.

     ONE MORE WRITE ON A SLICE'S REFUSAL, and it is the one that has to
     happen: a run in flight that finds nothing it can train on is over,
     however it got that way. The delete functions can empty a store as
     surely as iris_clear does, and ending the run HERE covers every caller
     instead of every caller having to remember. Without it the slice loop
     would spin for ever with the progress bar frozen. */
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
     reseed on its own: a warm trainer is asked to keep the performer's
     weights, and replacing them silently would hand the performer a
     different instrument, the loss of accumulated technique that Fiebrink
     and Sonami describe (NIME 2020; see the warm trainers below).

     THE WEIGHTS DECIDE, NOT THE STATUS (iris_internal_pinned). So the refusal
     holds on every call for as long as a weight sits on the limit: a refusal
     moves no weight, so the next call refuses too, and a status overwritten by
     an unrelated call -- a not-a-number refused at iris_record's door, say --
     cannot let a warm run through. The way out is a run that does not continue
     from these weights: iris_train and iris_train_begin reseed from the
     instrument's own seed before their first epoch, so they never meet this
     test with pinned weights.

     Every entry reaches this test, slices included, but a sliced run starts
     from iris_train_begin's reseed and ends itself if it diverges, so in
     practice what it refuses are the warm trainers, iris_continue and
     iris_continue_to_plateau. The one write is the status, plus ending the
     run if a slice is refused. */
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
  iris_internal_fit_ranges(k);

  const int stride = k->n_in + k->n_out;
  const int NI = k->n_in, NH = k->n_hid, NOUT = k->n_out;
  float x[IRIS_MAX_IN], t[IRIS_MAX_OUT];
  float err = 0.0f;

  if (!resume) {
    iris_internal_begin_session(k, epochs);
  } else if (k->tr_n_ex != k->n_ex) {
    /* The data changed under a running slice -- a record or a delete between
       two calls. Each of them sets tr_n_ex to -1 while a run is going, which
       matches no count, so an edit that leaves the count as it was is seen
       too: delete a bad take and record its replacement, the repair this
       library teaches, and the count is the same while the data is not. The
       shuffle covers a fixed count, so the permutation no longer describes
       the data: rebuild it, or the new demonstration is never visited and a
       deleted one still is.

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
    /* Fisher-Yates shuffle: swap each slot, from the last down, with a
       random slot at or below it, which makes every order equally likely */
    for (int i = k->n_ex - 1; i > 0; --i) {
      int j = (int)(iris_internal_rand_u32(&k->rng) % (uint32_t)(i + 1));
      int tmp = k->order[i]; k->order[i] = k->order[j]; k->order[j] = tmp;
    }

    err = 0.0f;
    for (int s = 0; s < k->n_ex; ++s) {
      const int row_ix = k->order[s];
      const float *row = k->ex + (size_t)row_ix * stride;
      for (int i = 0; i < NI; ++i) x[i] = iris_internal_norm_in (k, i, row[i]);
      for (int o = 0; o < NOUT; ++o) t[o] = iris_internal_norm_out(k, o, row[NI + o]);

      iris_internal_forward_norm(k, x);

      /* --- output layer error ---------------------------------------------
         THIS IS A SURROGATE GRADIENT, NOT THE GRADIENT. Read this before
         citing anything about the trainer.

         d_out = (predicted - target) * y*(1-y). y*(1-y) is the exact
         derivative of the TRUE logistic. This forward pass does not use the
         true logistic: iris_internal_sigmoid is built from iris_internal_tanh,
         the clamped rational of PART 1. So the backward pass is not the
         derivative of the forward pass. It is a surrogate, close enough in
         shape to point downhill.

         HOW FAR OFF. Exact only at zero, and under-scaled by up to 2 times
         across the ordinary operating range (docs/MATH-AUDIT.md, section 5,
         tabulates the ratio for the rational without its clamp, where it
         also changes sign). The shipped function does not change sign: the
         clamp holds a inside [-1, +1], so 1-a*a is never negative. Checked
         over 66,368,438 finite float bit patterns: 0 negatives, where the
         same check on the unclamped form finds 3,970,919 of 16,527,549.

         WHY IT STAYS. The textbook objection is that y*(1-y) collapses the
         gradient exactly when a unit is confidently wrong. Instrumented for
         that event, it fired ZERO times in 48.96 million output-unit
         updates, and it cannot at the defaults: targets live in [0.1, 0.9],
         so y*(1-y) >= 0.09 whenever the network is near its target. It does
         happen above a learning rate of 0.5, which only the internal setter
         reaches. Every repair tried measured worse (docs/MATH-FIXES.md,
         defect 3), and the exact derivative of the rational makes no
         measurable difference (PART 1).

         WHAT IS TRADED AWAY. One alternative does beat this: a cross-entropy
         gradient (the gradient of the logarithmic loss usually paired with a
         sigmoid output, which is the miss alone, with no y*(1-y) factor)
         with the targets left in [0.1, 0.9] wins 32 of 32 seeds at
         20 demonstrations by about 5%, and about 12% at a tuned learning
         rate. It loses at 10 demonstrations (1.094 times worse), the regime a
         musician actually demonstrates in, and it widens the reroll spread in
         the undemonstrated gaps by 1.6 times. A reroll that changes the
         instrument is one of this library's promises, and y*(1-y) is the
         brake that keeps it steady at the demonstrations. That is a judgement
         about the use case made on top of a measurement
         (docs/MATH-FIXES.md, defect 3; docs/MATH-AUDIT.md, section 5). */
      float rse = 0.0f;
      for (int o = 0; o < NOUT; ++o) {
        float y = k->out[o];
        float e = y - t[o];
        err += e * e;
        rse += e * e;
        k->d_out[o] = e * y * (1.0f - y);
      }
      /* THE RESIDUAL LEDGER (PART 8f). A separate accumulator: it reads the
         same errors and touches no weight, so it changes no bit of the
         update below. */
      k->ex_res[row_ix] += rse;

      /* --- hidden layer error: blame flows backward through the weights ----
         Each hidden unit's share is the output errors, weighted by the
         connections they came through, times the slope of its own
         squashing function. (1 - a*a) is the slope of the TRUE tanh, written
         in terms of the unit's output a; iris_internal_tanh is not tanh, so
         the surrogate note above applies here too. The exact slope of the
         rational is ((x*x - 9) / (3*(3 + x*x)))^2, and it is not what this
         uses. */
      for (int h = 0; h < NH; ++h) {
        float acc = 0.0f;
        for (int o = 0; o < NOUT; ++o) acc += k->w2[(size_t)o * NH + h] * k->d_out[o];
        float a = k->hid[h];
        k->d_hid[h] = acc * (1.0f - a * a);
      }

      /* --- apply the nudges, with momentum -------------------------------- */
      /* Each weight's velocity is 0.85 of the last one minus the learning
         rate times its gradient; the weight moves by its velocity.

         WEIGHT DECAY, when asked for. In full, per weight and per
         demonstration visited:
             v = momentum * v - lr * gradient      (flushed)
             w = w + v
             w = w - wd * w                        (flushed)
         with wd = l2 * lr / n_ex, where l2 is 0.3 times the smoothing
         setting. So the decay is taken after the velocity step, from the
         weight that step produced, and it is scaled by the learning rate as
         the gradient step is. `wd` is zero unless iris_set_smoothing set it,
         and at zero `w[h] - 0.0f * w[h]` subtracts an exact zero, so the
         update is the undecayed one bit for bit; the default path pays one
         multiply and one subtraction per weight for it.

         The decayed weight is flushed like the velocities (IRIS_FLUSH). A
         weight with no gradient -- one from an input that never moved, say
         -- only shrinks, by the same fraction on every visit, and without the
         flush it would pass through the subnormal numbers on its way to
         zero, where a processor that flushes them and one that does not
         part ways (see IRIS_TINY). Six demonstrations with one still input
         at smoothing 1 (the check in tests/train.c) have a weight below
         IRIS_TINY after 2,225 epochs and a subnormal one after 2,832 without
         the flush, and every such weight at exactly zero after 2,285 with it.
         A weight smaller than IRIS_TINY moves nothing, so zero loses
         nothing, and no healthy run at smoothing 0 has one: the golden hash
         does not move.

         Decoupled (applied to the weight, not folded into the gradient, so it
         does not build up in the momentum) and on WEIGHTS ONLY. Biases are
         never decayed: penalising a bias shifts the function without
         reducing its freedom, which is cost with no benefit. Scaled by 1/n_ex
         so that a setting means the same thing however many demonstrations
         you have, as scikit-learn scales its penalty against the data. */
      const float wd = k->l2 * k->lr / (float)k->n_ex;
      for (int o = 0; o < NOUT; ++o) {
        float g = k->d_out[o];
        float *w = k->w2 + (size_t)o * NH, *v = k->v_w2 + (size_t)o * NH;
        for (int h = 0; h < NH; ++h) {
          v[h] = IRIS_FLUSH(k->momentum * v[h] - k->lr * g * k->hid[h]);
          w[h] += v[h];
          w[h] = IRIS_FLUSH(w[h] - wd * w[h]);
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
          w[i] = IRIS_FLUSH(w[i] - wd * w[i]);
        }
        k->v_b1[h] = IRIS_FLUSH(k->momentum * k->v_b1[h] - k->lr * g);
        k->b1[h]  += k->v_b1[h];      /* biases are not decayed */
      }
    }
    err /= (float)(k->n_ex * NOUT);
    k->tr_err = err;
    k->res_epochs++;
    k->tr_done++;

#ifndef IRIS_NO_GUARDS
    /* Health check, once per epoch. The error accumulator has touched every
       activation this epoch, so it is a one-float summary of the network's
       numerical health; the weight sweep catches saturation-style divergence
       the error can't see (err stays finite while weights run away).        */
    if (iris_internal_isbad(err)) return iris_internal_trap_nan(k);
    {
      int st = iris_internal_check_weights(k);
      if (st == IRIS_NAN_TRAPPED) return iris_internal_trap_nan(k);
      if (st == IRIS_TRAINING_DIVERGED) {      /* clamped; stop and report */
        k->status = IRIS_TRAINING_DIVERGED;
        k->tr_running = 0;
        break;
      }
    }
#endif
    /* THE ERROR FLOOR — a fourth stopping rule, and at small demonstration
       counts the one most likely to be what actually stopped you.

       WHAT 1e-6 IS. `tr_err` is the mean squared error over every
       demonstration and output, in the network's own output units, where
       each output's demonstrated range spans the 0.8-wide band [0.1, 0.9],
       added up during the epoch while the weights are still moving (not
       iris_last_error, which is measured after the run). So the floor is
       a root-mean-square miss of 0.001 in those units: 0.125% of each
       output's demonstrated range. It is ABSOLUTE -- the same number whatever
       the data, the noise or the number of demonstrations -- and it was
       chosen as "close enough to a perfect fit", not derived from anything.

       WHEN IT FIRES. A network with a few dozen weights can fit a handful of
       demonstrations exactly, noise and all, so with few takes the error
       falls through the floor before the plateau test ever looks. iris_train
       on 2 inputs -> 12 hidden -> 3 outputs, six synthetic target shapes x 40
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
    if (k->tr_err < 1e-6f) { k->tr_running = 0; break; }

    /* --- the plateau test, and the progress report ----------------------- */
    if (conv && (k->tr_done % IRIS_CONV_WINDOW) == 0) {
      if (cb && !cb(user, k->tr_done, k->tr_ceiling, k->tr_err)) { k->tr_running = 0; break; }
      if (k->tr_ref > 0.0f && (k->tr_ref - k->tr_err) <= IRIS_CONV_TOL * k->tr_ref) {
        k->tr_running = 0;
        break;
      }
      k->tr_ref = k->tr_err;
    }
  }

  k->trained = 1;
  k->fitted  = 1;
  /* The training error of the weights the run ends with, measured now:
     one forward pass per demonstration, the cost of a third of an epoch. */
  k->last_error = iris_internal_recall_error(k, x);
  if (!resume) k->tr_running = 0;     /* a blocking run is over when it returns */
  return k->last_error;
}

/* --------------------------------------------------------------------------
   THE WARM TRAINERS: iris_continue AND iris_continue_to_plateau

   Both carry on from the weights the instrument holds now, with the momentum
   and the random state where the last run left them. iris_train, below, is
   the second of them run from a fresh start: it reseeds from the
   instrument's own seed and then calls iris_continue_to_plateau.

   EVERY CALL IS A SESSION OF ITS OWN. A blocking call starts its shuffle
   from the identity order (iris_internal_begin_session), whatever order the
   last run left; only a sliced run carries its order from one slice to the
   next. So n calls of iris_continue(k, 1) are not iris_continue(k, n): the
   same random draws permute a different starting order. On the golden
   recipe of tests/audit.c, 800 one-epoch calls end with weights up to 0.0182
   away from one 800-epoch call.

   WHY THEY EXIST. A musician who has practised an instrument and records
   one more take wants that corner fixed without the rest of the mapping
   moving. Retraining from the seed rewrites the mapping everywhere, which
   is how musicians lose accumulated technique to retraining (Fiebrink and
   Sonami, NIME 2020). A short warm run starts from the practised weights
   instead. The tests/audit.c check "correction: parity fit, surgical
   drift" adds one take to a practised 20-take instrument and runs
   iris_continue(k, 20): the training root-mean-square
   error reaches 0.0187, against 0.0181 for a cold 600-epoch retrain from
   the same seed, on a thirtieth of the epochs, and the mapping far from the
   new take moves 0.0025 on average, against 0.0033 for the retrain.

   THE HAZARD. The weights remember every demonstration they were trained
   on, including one you have since deleted: a deleted bad take's influence
   survives in the weights, and a warm run starts from exactly those
   weights, so continuing after a delete does not undo the take. On 20
   demonstrations of a smooth target plus one contradictory take, trained,
   the take deleted, then trained again: continuing leaves the instrument 14
   times further from the true mapping than iris_train does (mean error
   0.1416 against 0.0100, 40 of 40 seeds, every one with a healthy status),
   and when the deleted take sat between demonstrations it leaves the
   mapping there 0.75 to 0.82 of full scale wrong (8 of 8 seeds). A take
   outside the demonstrated range does damage of its own, because every run
   refits the ranges: correcting at twice the range moves the whole mapping
   by 0.07 to 0.16, against about 0.0015 for a take inside it. (These
   figures come from studies whose programs are not in this repository.)
   After deleting a take, call iris_train, which starts over from the seed
   and fits only the demonstrations stored now.

   THE SEED ALONE NO LONGER DESCRIBES THE INSTRUMENT. A warm run draws its
   shuffle from the random state the last run left, so after one the seed
   reproduces only a fresh iris_train; replaying the same records, deletes
   and runs in the same order reproduces the instrument to the bit (the
   tests/audit.c check "event-sourced determinism"). A saved file carries
   the random state (PART 9) but not the momentum velocities, which a load
   sets to zero, so a warm run after a load matches the same run on the
   unsaved instrument only when its velocities were zero as well, and then
   it matches to the bit (the check "save round trip carries the random
   state").

   Both refuse (-1) what every trainer refuses (iris_internal_trainable),
   and while a weight sits exactly on ±IRIS_W_LIMIT they refuse with
   IRIS_DIVERGED_STUCK on every call, that status being the one thing they
   write; iris_train is the way out. A run that meets a not-a-number
   partway also returns -1, but that one has changed the instrument: it is
   back at its seed's starting weights, unfitted, with IRIS_NAN_TRAPPED (see
   iris_internal_trap_nan and the failure rules above iris_get_status).
   -------------------------------------------------------------------------- */

/* THE FIXED-EPOCH TRAINER: exactly `epochs` more epochs from the current
   weights, fewer only if the error floor or the divergence guard stops the
   run (iris_train_epochs_done says how many ran). Returns the training
   error of the weights it leaves, the value iris_last_error then reports,
   or -1 if it refused, which it also does for a budget of zero or less, or
   if it met a not-a-number partway, which leaves the instrument at its
   seed's unfitted start (iris_internal_trap_nan). Mind THE HAZARD above:
   after deleting a take, call iris_train, not this.

   It matches Weka MultilayerPerceptron's per-weight update recursion and its
   per-demonstration update granularity.

   WHAT THAT DOES AND DOES NOT CLAIM. The recursion is an exact algebraic
   rewrite of Weka's (ours: v = momentum*v - lr*g*x, w += v; theirs:
   delta = lr*err*x + momentum*delta_prev, w += delta — same formula, opposite
   sign convention, both starting at zero). The granularity matches: n_ex
   weight writes per epoch, not one.

   It is NOT numerically identical to Weka and cannot be. iris computes in
   binary32; Weka computes in binary64 at every step, so exact agreement is
   impossible in principle, not merely unachieved. Nor is it identical in
   behaviour: iris's hidden units use the rational of PART 1 where Weka's
   use the logistic, its outputs are a sigmoid then a clamp where Weka's are
   linear and unclamped, its learning rate and momentum are 0.10 and 0.85
   where Weka's are 0.3 and 0.2, and it reshuffles every epoch where Weka
   shuffles once. Same rule, different quantities entering it, therefore
   different trajectories.

   Every "bit-identical" claim in this file is a claim about THIS FILE's
   self-consistency (sliced against unsliced runs, save and load round
   trips, -O0 against -O3), never about Weka. The golden hash in
   tests/audit.c pins the weights this produces, so any change to it is a
   change to training, made deliberately with the hash re-pinned and a
   changelog entry (CONTRIBUTING.md). */
IRIS_API float iris_continue(iris *k, int epochs) { if (!k) return -1.0f;
  return iris_internal_train_run(k, epochs, 0, 0, 0, 0);
}

/* Train until the training error plateaus, from the current weights: the
   plateau test, the ceiling, the error floor and the divergence guard stop
   it exactly as they stop iris_train. ceiling <= 0 takes IRIS_CONV_CEILING.
   cb may be NULL; otherwise it is called every IRIS_CONV_WINDOW epochs with
   (user, epochs done, ceiling, error), where the error is the one the
   plateau test is about to compare: the epoch's mean squared error, added
   up while the weights moved. Returning 0 from it ends the run there,
   leaving a usable, partly trained instrument. Returns the training error
   of the weights it leaves, the value iris_last_error then reports, or -1
   if it refused or met a not-a-number partway (as iris_continue). Mind THE
   HAZARD above: after deleting a take, call iris_train, not this. */
IRIS_API float iris_continue_to_plateau(iris *k, int ceiling, iris_progress_fn cb,
                                        void *user) { if (!k) return -1.0f;
  return iris_internal_train_run(k, ceiling > 0 ? ceiling : IRIS_CONV_CEILING, 1, 0, cb, user);
}

/* WHAT iris_train DOES BEFORE ITS FIRST EPOCH, in this order, and
   iris_train_begin does exactly the same -- which is what makes a sliced run
   bit-identical to the blocking one:

     1. refuse a store it cannot train on (iris_internal_trainable), having
        written nothing but IRIS_NAN_TRAPPED for a demonstration that is not
        finite;
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
     - the error floor, see the note at `tr_err < 1e-6f` in the engine: an
       ABSOLUTE floor on the epoch's mean squared error, not a relative one,
       and at small demonstration counts it, not the plateau test, is
       usually what stops the run;
     - the divergence guard: a weight past ±IRIS_W_LIMIT is clamped and the
       run ends there with status IRIS_TRAINING_DIVERGED.
   iris_train_epochs_done tells you how many epochs actually ran.

   Returns 1 if it fitted, 0 if it refused. It refuses when there are no
   demonstrations, when the instrument is null, or when its shape is too big
   for this translation unit's working arrays, and then changes nothing at
   all; and it refuses when a demonstration holds a not-a-number or an
   infinity, and then the one thing it changes is the status, to
   IRIS_NAN_TRAPPED. Neither refusal touches the weights or the ranges. A run
   the divergence guard stopped still returns 1: the instrument was fitted,
   and the status says how. It also returns 0, with IRIS_NAN_TRAPPED, when it
   meets a not-a-number partway through a run (demonstrations spread wider
   than a float can span), which leaves the instrument reseeded and
   unfitted. If you want to know HOW WELL it fits, that is a separate
   question with a separate answer: iris_last_error(k).

   IT NEVER REFUSES A STUCK INSTRUMENT, and that is deliberate: starting over
   from the seed is the way out of IRIS_DIVERGED_STUCK, whose weights are the
   only damaged part.

   CALL IT AFTER DELETING A TAKE. It starts over from the seed, so the take
   you deleted leaves nothing behind in the weights; the warm trainers above
   would keep its influence (THE HAZARD). */
IRIS_API int iris_train(iris *k) {
  if (!k) return 0;
  /* FIT FROM A DEFINED START, always.

     Continuing from whatever weights are already there breaks the loop this
     library exists for. Record a bad take, delete it, retrain (the repair
     the usage block teaches) and a warm start keeps the deleted take's
     crater, because the weights it bent are the weights training resumes
     from: 14 times further from the true mapping than this function, in 40
     of 40 seeds, with a healthy status and a training error as small as a
     clean fit's, so nothing a screen can show gives it away (THE HAZARD,
     above).

     Warm-starting has its use, argued with the warm trainers above: continuing
     from the current fit adjusts one region without rewriting the mapping
     everywhere, which is how a musician keeps technique. But that is what the
     warm trainers are for. This function is called train, a caller expects it
     to fit the demonstrations it has now, and the two must not be the same
     act. */
  if (!iris_internal_cold_start(k)) return 0;

  /* WHAT THE PLATEAU RULE BETS ON, AND WHEN THE BET IS WRONG.

     Training stops when a window of epochs stops buying much error. Against
     running longer that is the right bet on noisy takes (a fixed 60,000
     epochs generalises 2% to 29% worse than the plateau), and against
     stopping sooner it is the wrong one (a fixed 100 epochs beats it by 1.2
     to 2.2 times at noise 0.05 and above, unless smoothing is on): see
     TRAINING TO A PLATEAU above, and the table at iris_set_smoothing.

     On clean demonstrations it can stop far too early. One input, 12 hidden
     units, one output, twelve takes evenly spaced on a straight ramp from 0
     to 1, seed 1, then iris_continue up to each mark:

       epochs      training error   worst miss on a demonstrated take
       4,000       3.10e-04         0.0433    <- where this function stops
       60,000      2.86e-04         0.0425    <- IRIS_CONV_CEILING
       160,000     2.79e-04         0.0395
       320,000     7.88e-06         0.0055
       388,478     1.09e-06         0.0025    <- the error floor stops it

     (training error is iris_last_error; the floor reads the last epoch's
     own figure, which has just fallen under 1e-6.)

     The error sits on a FALSE plateau from epoch 4,000 past 160,000 and then
     falls two orders of magnitude (seeds 7, 42 and 12345 do the same; seed
     1234 stays on it until after 320,000). The plateau outlasts this
     library's maximum budget, so nothing here can see past it, and raising
     `ceiling` on iris_continue_to_plateau cannot help: a ceiling is a
     maximum, and the run stops far below it.

     If your demonstrations are clean and recall matters more to you than
     what happens between takes, the escape is iris_continue(k, 400000) after
     this. That is a real choice with a real cost, which is why it is
     written here rather than made for you. */
  /* The run returns -1 both when it refuses and when it meets a
     not-a-number partway (iris_internal_trap_nan), and a non-negative error
     exactly when it fitted, which is when it sets `trained`. */
  return iris_continue_to_plateau(k, 0, 0, 0) >= 0.0f ? 1 : 0;
}


/* The same run, in slices, for a sketch that must keep drawing.
     iris_train_begin(k, 0);
     while (iris_train_slice(k, 500)) { draw(iris_train_progress(k)); poll(); }

   BIT-IDENTICAL TO iris_train, for any slice sizes. iris_train_begin does
   exactly what iris_train does before its first epoch (iris_internal_cold_start:
   refuse an untrainable store, then reseed from the instrument's own seed)
   and starts the session through the same function the
   engine uses. The shuffle buffer and the plateau reference then carry across
   the slices, so the random draws are the same draws in the same order and
   every byte of the arena ends the same. tests/train.c checks that over
   several shapes, seeds and slice sizes, including after deletes.

   So a sliced run starts over from the seed as iris_train does, and is as
   right a call after deleting a take. A take deleted between two slices
   drops out of the rest of the run, but what it has already done to the
   weights stays, as it would for a warm trainer, until the next
   iris_train_begin.

   ceiling <= 0 takes IRIS_CONV_CEILING, which is what iris_train uses; any
   other ceiling gives the run iris_train would make with that ceiling. Returns
   1 if the run started, 0 if it refused -- for the same reasons, and with the
   same guarantee about what it writes, as iris_train. */
IRIS_API int iris_train_begin(iris *k, int ceiling) { if (!k) return 0;
  if (!iris_internal_cold_start(k)) return 0;
  iris_internal_begin_session(k, ceiling > 0 ? ceiling : IRIS_CONV_CEILING);
  return 1;
}

/* Runs at most `epochs` more. Returns 1 if there is more to do, 0 when the
   run has finished (plateau, ceiling, early stop, or a guard) -- including
   when the store can no longer be trained on: emptied by deletes, which ends
   the run and writes nothing else, or holding a not-a-number, which ends the
   run and writes IRIS_NAN_TRAPPED. */
IRIS_API int iris_train_slice(iris *k, int epochs) { if (!k) return 0;
  /* A budget of zero or less is "do nothing", not "use the default": a caller
     whose computed slice size comes out zero trains nothing, and the return
     value still says whether the run goes on. */
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
     for both would draw a progress bar full before anything was pressed,
     empty one slice later, then full again. After any gradient run the
     ceiling is set -- iris_train_begin sets it first thing -- and a
     closed-form solve, which has no epochs and no ceiling, leaves its ledger
     behind (res_epochs 1, PART 8d). All three are 0 only on an instrument
     that is fresh, freshly loaded or freshly cleared, so they are what tells
     the two apart. */
  if (!k->tr_running)
    return (k->tr_ceiling > 0 || k->tr_done > 0 || k->res_epochs > 0) ? 1.0f : 0.0f;
  if (k->tr_ceiling <= 0) return 1.0f;
  {
    float f = (float)k->tr_done / (float)k->tr_ceiling;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
  }
}
IRIS_API int iris_train_busy(const iris *k) { if (!k) return 0; return k->tr_running; }

/* How many epochs the last run ACTUALLY did. Compare against what you asked
   for: fewer means it stopped early, and there are four rules that can do
   that — the plateau test, the error floor (an absolute 1e-6 on the epoch's
   mean squared error; see the note in the engine), the divergence guard, or a
   progress callback returning 0. iris_get_status() distinguishes the guard;
   this distinguishes "ran to completion" from "stopped for a good reason",
   which iris_train_progress() deliberately cannot, because it reports 1.0 for
   any finished run. A refused call leaves it where it was, and a closed-form
   solve (PART 8d), which runs no epochs, sets it to 0. */
IRIS_API int iris_train_epochs_done(const iris *k) { if (!k) return 0; return k->tr_done; }

/* 1 when the instrument's fit matches the demonstrations stored now: set by
   every trainer that fits, cleared by iris_record, the deletes, iris_clear,
   iris_reseed and a run that ends unfitted. An instrument can be fitted and
   still playing while this says 0 (PART 6). */
IRIS_API int   iris_is_trained(const iris *k) { if (!k) return 0; return k->trained; }
/* The training error of the current weights: the mean squared error over
   every stored demonstration and output, in the network's output units (see
   ERROR in the masthead), measured with one forward pass per demonstration
   when a trainer finishes -- a gradient run or slice, or a closed-form
   solve -- and when a file is loaded (iris_internal_recall_error). So it
   has one meaning whichever way the weights arrived: the same weights and
   demonstrations read the same bits after iris_train, after iris_train_elm
   and after a save and a load. 1.0 before any fit, after iris_clear and
   after a run that ended unfitted. A record or a delete does not re-measure
   it: until the next training run it describes the demonstrations the fit
   was made on. A load does measure it, over the demonstrations in the file,
   so on a stale instrument (iris_is_trained 0) the figure changes across a
   save and a load; on a trained one it does not. */
IRIS_API float iris_last_error(const iris *k) { if (!k) return 0.0f; return k->last_error; }
IRIS_API uint32_t iris_seed(const iris *k)    { if (!k) return 0u; return k->seed; }

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
   against the demonstrations it was fitted on. Training stops when TRAINING
   error plateaus, which is not the same event as "it got as good as it is
   going to get at things it has not seen", and a machine-learning reviewer
   is right to say so. The usual remedy, holding back 20% as a validation
   set, is unavailable here: at 20 demonstrations that discards 4 of them, and
   you cannot spare 4.

   Leave-one-out is the remedy that fits this regime. Hide ONE demonstration,
   refit on the rest, and see how far off the hidden one you land. Do that
   once per demonstration and average. Nothing is discarded; every
   demonstration is used for training in every fold but its own.

   WHAT IT COSTS. n_ex + 1 fits of `epochs` each (600 by default): the folds,
   then the final refit. At 20 demonstrations of a 2-12-3 instrument that is
   about 21 ms on the development laptop (an Apple M4 Max, Apple clang -O2).
   It has not been timed on the ESP32-S3. It is a deliberate, occasional act,
   "how good is this actually?", not something to put in a play loop.

   HOW TO USE IT. For comparison, not as an absolute grade: run it at several
   smoothing values and take the lowest. That picks a setting without tuning
   on the very numbers you then report.

   HOW WELL THAT WORKS. Twenty demonstrations on a 5 x 4 grid of a smooth
   2-input, 3-output target, with Gaussian noise of standard deviation 0.05
   on the outputs, seed 1234. The left column is this function; the right is
   the mean squared error of the plateau-trained instrument against the clean
   target on a 21 x 21 grid:

       smoothing   leave-one-out    held-out truth
       0              0.00645          0.00213
       0.05           0.00555          0.00082   <- actually best
       0.15           0.00534          0.00084   <- leave-one-out's pick
       0.5            0.00692          0.00085
       1              0.00890          0.00164

   It ranks the ends correctly (no smoothing and full smoothing are both
   worse) and picks a neighbour of the true best, which here is nearly as
   good. That is the known behaviour of leave-one-out at small n: nearly
   unbiased but noisy, and each fold trains on n-1 demonstrations rather than
   n. It also scores 600-epoch fits, not the plateau run you play. Treat it
   as a coarse ranking: it tells you whether to smooth and roughly how much,
   not the exact optimum. Its absolute value is NOT comparable to a held-out
   error (the columns above differ by three to eight times), so use it only
   to compare settings with each other.

   Every fold trains from the SAME seed, so the folds differ only by which
   demonstration was hidden. epochs <= 0 takes 600. Returns mean squared error per
   output, in the demonstrations' own units, or -1 if it refused: fewer than 3
   demonstrations to fold over, or a store no trainer would accept (see
   iris_internal_trainable). It checks that BEFORE the first fold reseeds
   anything, so a refusal changes nothing but the status, which it sets to
   IRIS_NAN_TRAPPED only for a demonstration that is not finite, and it never
   returns a not-a-number.

   THE INSTRUMENT IS LEFT REFITTED ON ALL DEMONSTRATIONS, from that same
   seed, with iris_continue for `epochs`, so it is valid to play afterwards,
   but it is NOT the instrument you had before you called this: it has been
   retrained, and for a fixed 600 epochs rather than to the plateau. Save
   first, or call iris_train afterwards, if that matters. */

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
    iris_reseed(k, seed0);
    iris_continue(k, ep);
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

  iris_reseed(k, seed0);               /* leave it playable, fitted on all */
  iris_continue(k, ep);
  return (float)(total / ((double)n * (double)no));
}

IRIS_API float iris_loo_error(iris *k, int epochs) { return iris_internal_loo(k, epochs, 0); }

/* SUGGEST A SMOOTHING VALUE: an explicit, occasional act, not an automatic one.

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
   output recorded in thousands counts the same as one recorded in
   fractions: "Units: none" from the interface block, applied here. iris_loo_error itself
   reports raw units. An output whose demonstrations never moved carries no
   evidence about smoothing and is left out.

   WHY IT IS NOT AUTOMATIC. Tried as an automatic default, on the same 36
   datasets with 16 rerolls each:

     - IT IS NOT STABLE. Rerolling the same demonstrations changed its answer:
       2.17 distinct values per dataset on average. An instrument whose
       smoothing changes when you reroll is an instrument that stops being
       predictable, which is worse than one that is merely unsmoothed.
     - IT IS SOMETIMES WORSE THAN DOING NOTHING. 10.9% of its picks gave the
       plateau-trained instrument a worse held-out error than smoothing 0.
     - IT IS SLOW. Five sweeps of n + 1 fits: 104 ms at 20 demonstrations and
       640 ms at 50 on the development laptop (an Apple M4 Max; 2 inputs, 12
       hidden units, 3 outputs). It has not been timed on the ESP32-S3. At
       the 32 to 40 microseconds per demonstration per epoch that
       device_torture's test 5 implies (a board figure awaiting a recorded
       log), the 1.2 million demonstration-epochs of a 20-demonstration
       suggestion would take 40 to 50 seconds: an estimate, not a board
       reading. That is not something to hide inside a training call.

   WHAT IT IS GOOD FOR: a starting point when you genuinely do not know, on a
   machine where 100 ms is nothing. It captured 81% of what always picking
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
   anything iris_loo_error refuses, having changed nothing but, for a
   demonstration that is not finite, the status: refusing an answer is
   recoverable, and quietly replacing someone's instrument is not. A
   mistake in the arguments is found before the store is looked at, so it
   leaves the status alone even when a demonstration is poisoned too.

   It still suggests; it still does not decide. Applying the number is yours. */
IRIS_API float iris_suggest_smoothing(iris *k, void *scratch, size_t scratch_bytes) {
  if (!k) return -1.0f;
  {
    /* The instrument is every byte from its structure to the end of order[],
       the last array iris_init carves. That span is always smaller than the
       arena iris_size asks for, which also holds the alignment slack. */
    unsigned char *inst = (unsigned char *)k, *copy = (unsigned char *)scratch;
    const size_t span = (size_t)((unsigned char *)(k->order + k->cap) - inst);
    const size_t need = iris_size(k->n_in, k->n_hid, k->n_out, k->cap);
    static const float ladder[5] = { 0.0f, 0.05f, 0.15f, 0.5f, 1.0f };
    float best_v = 0.0f, best_e = 0.0f;
    /* need == 0 first: size_t is unsigned, so `scratch_bytes < 0` would wave
       any buffer through (the sentinel trap described above iris_size). */
    if (!copy || need == 0 || scratch_bytes < need) return -1.0f;
    if ((uintptr_t)copy < (uintptr_t)inst + span
        && (uintptr_t)inst < (uintptr_t)copy + span) return -1.0f;   /* overlaps */
    /* what the sweep refuses, asked after the arguments so that a mistake in
       them leaves the status alone even when a demonstration is poisoned */
    if (k->n_ex < 3 || !iris_internal_trainable(k)) return -1.0f;

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

   The trial-and-error trap in interactive machine learning is that when the
   instrument
   feels wrong you have no idea WHICH of your twenty demonstrations is
   wrong, so you re-record at random. This points at one.

   IT IS THE INTEGRAL, NOT THE ENDPOINT, AND THAT IS THE WHOLE IDEA.
   The obvious detector is the final training residual: after training, ask
   each demonstration how badly the network still misses it. It gets WORSE as the
   mistake gets bigger, because given enough epochs the optimiser bends the
   surface far enough to fit the bad point too, after which it looks like
   every other point. So this sums each demonstration's squared error over
   EVERY epoch instead, which measures how long it fought rather than where
   it ended up. A demonstration that agrees with its neighbours is fitted
   early and
   stays fitted; one that contradicts them stays wrong for thousands of
   epochs. That ranking is stable across training budgets where the endpoint
   is not.

   WHAT YOU GET BACK IS A MARGIN, NOT A LEVEL, and that is deliberate. The
   worst-of-n stress score rises with n on clean data with nothing wrong at
   all, so a screen wired to a fixed level would be silent on small
   instruments and cry wolf on large ones. Worst divided by second-worst
   does not drift. IRIS_STRESS_FLAG is 2.5.

   BELOW IRIS_STRESS_MIN_EX (12) IT RETURNS -1 AND SAYS NOTHING. A
   demonstration can only be caught disagreeing with a crowd if there is a
   crowd; at ten demonstrations the clean margin alone reaches 5.04
   (docs/adr/0019-the-residual-ledger-integrates-it-does-not-sample.md),
   which would be a false accusation.

   THE LIMIT THAT TRAVELS WITH IT. Every number below comes from one clean
   offset on one output of a smooth, noiseless truth. A 5% offset of that kind
   is smaller than the spread between two takes of the same human gesture,
   and on clean synthetic sessions iris_train still ranks it first (18 of 20,
   below), but real demonstrations are inconsistent in ways that are not one
   displaced output, and none of this has been checked against a recorded
   human gesture.

   THE FLAG IS NOT SILENT ON CLEAN SESSIONS, AND IT CAN MISS. Measured with
   `tests/elm.c measure` on the protocol of the note named below (12 hidden
   units, 8 outputs, 200 clean sessions at each of 20, 50 and 100
   demonstrations): after iris_train the largest clean margin is 3.23, 4.05
   and 3.22, and 7, 15 and 6 of the 200 reach IRIS_STRESS_FLAG; after the
   closed-form trainer, whose ledger is the miss that remains after its solve
   (PART 8d), 25, 14 and 24 of 200 reach it, the largest 9.99. With one
   output of one take offset by 0.05 to 0.40, iris_train ranks that take
   first in 18 to 20 of 20 sessions, the closed-form ledger in 13 to 20 of 20
   (13 to 15 at 20 demonstrations). So a flag is a take to listen to again,
   not a take to delete unheard, and no flag does not prove every take is
   good. A candidate improvement for the closed-form trainer, not built: the
   leave-one-out residual, each demonstration's miss divided by one minus its
   leverage, which is how strongly that demonstration pulls the fit toward
   itself (its diagonal entry of the hat matrix, the matrix that turns the
   demonstrated targets into the fitted ones). The same Cholesky factor
   gives it in about n times (nh+1) squared operations.

   COST. sizeof(float) * cap in the arena (512 bytes at cap 128, 1 KB at
   cap 256) and one float addition per demonstration per epoch, a sliver of
   the backpropagation work already being done for it.

   The detector comparison, the margin table and the relation to TracIn (a
   method from the machine-learning literature that credits each training
   example with its gradient's effect summed over training):
   docs/adr/0019-the-residual-ledger-integrates-it-does-not-sample.md
   ========================================================================== */

/* Fewer demonstrations than this and there is no crowd to disagree with. */
#define IRIS_STRESS_MIN_EX 12
/* Margin (worst / second-worst) at which a screen should say something.
   Most clean sessions stay below it and some do not: see THE FLAG IS NOT
   SILENT ON CLEAN SESSIONS above. */
#define IRIS_STRESS_FLAG 2.5f

/* Relative stress of one demonstration: its integrated training error
   divided by the mean over all of them, so 1.0 is an ordinary one. This is
   a RANKING, and it is meaningful at any count; only the decision to speak
   needs a crowd. It refuses with -1 (rule 2 of the failure rules) for a null
   instrument, for an index out of range, and when there is no ledger to
   read: no training run or solve since the instrument was made, loaded or
   cleared.
   A stress is never negative, so a refusal cannot be mistaken for 0, a
   demonstration the network never missed. It is 0 for every demonstration
   when none was ever missed at all. */
IRIS_API float iris_example_stress(const iris *k, int idx) { if (!k) return -1.0f;
  if (idx < 0 || idx >= k->n_ex || k->res_epochs == 0) return -1.0f;
  {
    float sum = 0.0f;
    for (int i = 0; i < k->n_ex; ++i) sum += k->ex_res[i];
    if (sum <= 0.0f) return 0.0f;
    return k->ex_res[idx] * (float)k->n_ex / sum;
  }
}

/* The one to point at. Returns the INDEX of the demonstration that fought
   hardest, or -1 when there is nothing to point at: no training run or
   solve since the instrument was made, loaded or cleared, or fewer than
   IRIS_STRESS_MIN_EX demonstrations. *margin, when given, receives worst
   divided by second-worst.

   THE CALLER DECIDES WHETHER TO SPEAK, and the condition is written once,
   here, so that every sketch uses the same one:

       float m; int id = iris_worst_example_id(k, &m);
       if (id >= 0 && m >= IRIS_STRESS_FLAG)  say("example %d is fighting the
                                                 others", id);

   USE iris_worst_example_id, NOT iris_worst_example + iris_id_at: it returns
   the identifier directly, and identifiers survive deletions where indices
   do not. Both stay public for callers who want the position.

   The index is returned even below the flag because the ranking is still
   real and a screen may want to show it quietly (a dimmer mark, say)
   without accusing anything. Ties go to the earliest-recorded
   demonstration, the same rule as iris_knn_predict. */
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

/* The identifier of that demonstration, which is what a sketch should show,
   because identifiers survive deletions and indices do not. -1 when there
   is nothing to say. */
IRIS_API int iris_worst_example_id(const iris *k, float *margin) { if (!k) return -1;
  int i = iris_worst_example(k, margin);
  return i < 0 ? -1 : (int)k->ex_id[i];
}

/* ==========================================================================
   PART 8d — THE CLOSED-FORM TRAINER  (ELM: freeze the randomness, solve the rest)

   The fastest trainer in this file. On the development laptop, at 50
   demonstrations, two inputs and nh = 12 (12 hidden units), a solve takes
   about 8 microseconds where 600 epochs of backpropagation take 2.6 ms,
   roughly 300 times longer (tests/audit.c prints both in its training-cost
   table). It has not been timed on the ESP32-S3. The ratio shrinks as
   inputs are added, because the frozen layer is redrawn for every
   demonstration (see NOTHING CHANGES UNLESS THE SOLVE WORKS, below).

   The trick is to stop training half the network. Draw the hidden layer
   once from the seed and FREEZE it. Each demonstration then gives the
   hidden units' answers h (plus a constant 1 for the bias) and a target z
   for each output (the demonstrated output, mapped back through the
   sigmoid). The output weights beta that fit best in the least-squares
   sense solve the linear system

       (H^T H + ridge) beta = H^T z

   where H stacks one row of h per demonstration. H^T H is the normal
   matrix, (nh+1) x (nh+1), and one Cholesky factorisation solves it: no
   epochs, no iteration. This idea has a name in the literature, extreme
   learning machine, and a 20-year argument about whether it deserves one;
   it is here because it works here.

   Two measurements make it work in single precision on this network:

   GAIN. The backprop starting weights (1/sqrt(n_in)) rely on training to grow.
   Frozen at that scale, tanh of a normalised input barely bends -- the random
   features are nearly collinear and the normal matrix is numerically
   rank-deficient. The frozen layer is drawn at 2/sqrt(n_in) instead, wide
   enough that the features have real capacity. docs/gain-sweep.c measures the
   choice in one command: mean held-out error over 4 target shapes x 16 seeds x
   {8,20,50} demonstrations x nh {12,24,48}, gain = M/sqrt(n_in):

       M         0.25   0.50   1.00   1.50   2.00   3.00   4.00   8.00
       error    .1143  .1025  .0946  .0919  .0905  .0894  .0920  .1087

   M=2 beats the backpropagation starting scale, M=1, by 4.3%. The minimum
   is BROAD and M=2 sits inside it. M=3 is marginally better (1.2%) and the
   optimum drifts upward with width (best at 1.5 for nh=12 and at 3.0 for
   nh=48), so 2 is a good constant rather than the best one. It stays because
   a 1.2% gain on synthetic targets does not justify a change to training,
   which would re-pin the closed-form hashes in tests/elm.c.

   RIDGE, MANDATORY. Even with the wider gain, the unridged float32 normal
   matrix fails Cholesky in EVERY realistic scenario measured, including 20
   well-spread demonstrations. The ridge is relative (lam0 * trace/(nh+1), so it
   scales with the data) plus a floor of 1e-7, and it escalates
   deterministically: double it on a failed factorisation, at most 8 times, and
   report the count. If escalation was needed the status says
   IRIS_RIDGE_ESCALATED -- the result is valid, the data was harder than usual.
   The measurements: docs/adr/0009-ridge-is-mandatory.md. Escalation
   can still run out: lam0 = 0 on 128 demonstrations all made at one gesture
   fails all nine attempts (40 seeds of 40, at 8, 12 and 24 hidden units), and
   that is an ordinary refusal (below).

   SMOOTHING reaches this trainer as extra ridge on the output weights, and
   never on the output biases: a penalised bias drags every output toward the
   middle of its range, where an unpenalised one lets heavy smoothing settle
   near the average of what was demonstrated (averaged in logit units: the
   logit is the inverse of the sigmoid, the space the solve works in).
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
   after escalation -- leaves every byte of the instrument as it was, and the
   return value is the report. The one exception is the poisoned
   demonstration, which sets the status to IRIS_NAN_TRAPPED, as every trainer
   does (see iris_internal_trainable). That takes some care,
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
   IRIS_W_LIMIT says what that means for gradient training afterwards. The
   solve targets logit space, the exact inverse of this file's sigmoid, so
   the forward pass lands on the normalised targets. That makes it a
   bounded-output VARIANT of the backpropagation network's output layer, not
   an equivalent. docs/adr/0008-elm-same-network-better-math.md compares it
   with a solve whose outputs are linear; nothing in this repository bounds
   the difference between the closed-form fit and a backpropagation fit.

   REROLL is the reason to love it: a new seed literally IS a new frozen
   random layer, undiluted by any training, steady at the demonstrations and
   lively in the gaps (the tests/audit.c check "ELM: same seed, same bits;
   reroll character" measures the demonstrations moving 0.0106 between
   seeds and the gaps 0.1123). iris_reseed(k, new_seed) then iris_train_elm
   is the whole gesture: the purest form of "same demonstrations, different
   instrument" this library has. That needs nh >= 8, since four frozen
   random features cannot recall five demonstrations, so this trainer
   refuses a narrower instrument outright. Numbers and the recommended lam0
   per width: docs/adr/0008-elm-same-network-better-math.md.

   Determinism: the hidden layer is drawn from k->seed by a LOCAL random number
   generator (k->rng is never touched, so a solve does not move the random
   state a warm trainer draws from), accumulation order is fixed by
   demonstration order, and the escalation schedule is fixed. Same seed and
   same demonstrations give bit-identical weights (checked at nh 12, 24 and
   48 in tests/audit.c).
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

/* The arena and the solve's scratch added together, for a sketch that
   reserves one static block for both: give iris_init the first
   IRIS_ARENA(NI, NH, NO, NEX) bytes and iris_train_elm the rest. */
#define IRIS_ARENA_ELM(NI, NH, NO, NEX)                                          \
  ( IRIS_ARENA(NI, NH, NO, NEX) + IRIS_ELM_SCRATCH(NH, NO) )

/* Exact inverse of iris_internal_tanh (the rational, not the true tanh),
   by Newton's method (repeatedly stepping to where the tangent line of
   x*(27+x^2) - y*(27+9x^2) crosses zero). Five steps reach single-precision
   rounding over |y| <= 0.98, which covers the whole 0.1-0.9 target band.
   Deterministic: a fixed number of steps, no early exit. */
IRIS_API float iris_internal_artanh(float y) {
  y = iris_internal_clampf(y, -0.98f, 0.98f);
  float x = y * (1.0f + 0.33333333f * y * y);      /* series starting point */
  for (int it = 0; it < 5; ++it) {
    const float x2 = x * x;
    const float f  = x * (27.0f + x2) - y * (27.0f + 9.0f * x2);
    const float fp = 27.0f + 3.0f * x2 - 18.0f * y * x;
    x -= f / fp;
  }
  return x;
}

/* Inverse of iris_internal_sigmoid: the pre-activation z with
   iris_internal_sigmoid(z) == t. */
IRIS_API float iris_internal_logit(float t) {
  return 2.0f * iris_internal_artanh(2.0f * t - 1.0f);
}

/* INTERNAL: the solve itself, with the hidden-layer gains as arguments. Use
   iris_train_elm, which passes the measured gains; this form exists for
   iris_train_elm and for the gain measurement in docs/gain-sweep.c.

   Returns the number of ridge doublings used (0 = first try; status
   IRIS_RIDGE_ESCALATED if > 0), or -1 refusing, with nothing changed but the
   status after a poisoned demonstration (IRIS_NAN_TRAPPED). It
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
IRIS_API int iris_internal_train_elm_ex(iris *k, float lam0, float gain_w, float gain_b,
                               void *scratch, size_t scratch_bytes) {
  /* --- refuse before touching anything ----------------------------------- */
  if (!k || !scratch || k->n_ex == 0) return -1;
  if (!iris_internal_shape_fits(k) || k->n_hid < 8) return -1;  /* nh < 8: the reroll floor */
  if (iris_internal_isbad(lam0)   || !(lam0   >= 0.0f)) return -1;  /* !(x >= 0) is also */
  if (iris_internal_isbad(gain_w) || !(gain_w >= 0.0f)) return -1;  /* true for NaN      */
  if (iris_internal_isbad(gain_b) || !(gain_b >= 0.0f)) return -1;
  if (scratch_bytes < IRIS_ELM_SCRATCH(k->n_hid, k->n_out)) return -1;
  /* a poisoned demonstration is refused here, after the arguments and before
     anything else is written, by the test every trainer shares: it sets
     IRIS_NAN_TRAPPED, and the demonstration stays in the store where the
     musician can find it and delete it */
  if (!iris_internal_trainable(k)) return -1;

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
  iris_internal_fit_ranges(k);

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
      for (int i = 0; i < NI_; ++i) x[i] = iris_internal_norm_in(k, i, row[i]);
      iris_internal_rng r = { seed };
      for (int j = 0; j < NH_; ++j) {
        for (int i = 0; i < NI_; ++i) wj[i] = iris_internal_rand_sym(&r) * gain_w;
        /* the bias is drawn after the unit's weights, but the sum starts
           from it, as the forward pass's does (PART 6) */
        float s = iris_internal_rand_sym(&r) * gain_b;
        for (int i = 0; i < NI_; ++i) s += wj[i] * x[i];
        h[j] = iris_internal_tanh(s);
      }
      h[NH_] = 1.0f;                                   /* bias feature */
      for (int i = 0; i < K; ++i) {
        const float hi = h[i];
        float *Ai = A + (size_t)i * K;
        for (int j = i; j < K; ++j) Ai[j] += hi * h[j];
      }
      for (int o = 0; o < NO_; ++o) {
        const float z = iris_internal_logit(iris_internal_norm_out(k, o, row[NI_ + o]));
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
      const float lj = iris_internal_sqrt(d);
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
    for (int i = 0; i < K * NO_; ++i) if (iris_internal_isbad(B[i])) doublings = -1;
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
    iris_internal_rng r = { seed };
    for (int j = 0; j < NH_; ++j) {
      float *w = k->w1 + (size_t)j * NI_;
      for (int i = 0; i < NI_; ++i) w[i] = iris_internal_rand_sym(&r) * gain_w;
      k->b1[j] = iris_internal_rand_sym(&r) * gain_b;
    }
  }
  for (int o = 0; o < NO_; ++o) {
    float *w = k->w2 + (size_t)o * NH_;
    for (int j = 0; j < NH_; ++j) w[j] = B[(size_t)j * NO_ + o];
    k->b2[o] = B[(size_t)NH_ * NO_ + o];
  }
  iris_internal_zero_velocity(k);
  k->tr_running = 0;         /* end any sliced run: its next slice would
                                 otherwise go on training over the solve */
  /* the progress fields describe this fit, not the gradient run before it:
     no epochs and no ceiling, and iris_train_progress reads the solve's
     ledger (res_epochs 1, below) as a finished fit */
  k->tr_done = 0; k->tr_ceiling = 0; k->tr_n_ex = 0; k->tr_ref = 0.0f; k->tr_err = 0.0f;
  k->trained = 1;
  k->fitted  = 1;                    /* a closed-form solve IS a fit */
  k->status  = doublings > 0 ? IRIS_RIDGE_ESCALATED : IRIS_STATUS_OK;

  /* --- the ledger, and how far the demonstrations and the fitted outputs
     move; then the training error, as every trainer measures it ---------- */
  {
    float lo_y[IRIS_MAX_OUT], hi_y[IRIS_MAX_OUT];     /* fitted, normalised */
    for (int o = 0; o < NO_; ++o) { lo_y[o] = 1e30f; hi_y[o] = -1e30f; }
    for (int i = 0; i < k->cap; ++i) k->ex_res[i] = 0.0f;
    for (int n = 0; n < k->n_ex; ++n) {
      const float *row = k->ex + (size_t)n * stride;
      float rse = 0.0f;
      for (int i = 0; i < NI_; ++i) x[i] = iris_internal_norm_in(k, i, row[i]);
      iris_internal_forward_norm(k, x);
      for (int o = 0; o < NO_; ++o) {
        const float y = k->out[o], t = iris_internal_norm_out(k, o, row[NI_ + o]);
        const float e = y - t;
        rse += e * e;
        if (y < lo_y[o]) lo_y[o] = y;
        if (y > hi_y[o]) hi_y[o] = y;
      }
      k->ex_res[n] = rse;
    }
    k->res_epochs = 1;
    k->last_error = iris_internal_recall_error(k, x);

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
       themselves, not of the ranges, which iris_internal_fit_ranges widens for
       a constant output. An input that never moved reads 0 in every
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
          if (iris_internal_norm_in(k, i, k->ex[(size_t)n * stride + i])
              != iris_internal_norm_in(k, i, k->ex[i])) moved = 1;
      collapsed = moved;
      for (int o = 0; o < NO_; ++o) {
        float lo_t = 1e30f, hi_t = -1e30f;           /* demonstrated, normalised */
        for (int n = 0; n < k->n_ex; ++n) {
          const float t = iris_internal_norm_out(k, o, k->ex[(size_t)n * stride + NI_ + o]);
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

/* THE CLOSED-FORM TRAINER, with the gains of the sweep above: 2/sqrt(n_in)
   for weights AND biases. lam0 is the ridge in proportion to the data: 1e-4
   is the recommended value at nh=12 and 1e-3 at nh=48. The scratch is at
   least IRIS_ELM_SCRATCH(n_hid, n_out) bytes, at any alignment, not
   overlapping the instrument. Smoothing applies (see SMOOTHING above).

   Returns the number of ridge doublings it needed (0 is the best case), or
   -1 if it refused, having changed nothing but, for a poisoned
   demonstration, the status; iris_internal_train_elm_ex above lists every
   refusal and says what a lam0 of 0 means. After a solve the status is
   IRIS_STATUS_OK, IRIS_RIDGE_ESCALATED when doublings were needed, or
   IRIS_SOLVE_COLLAPSED when the fit ignores the inputs;
   iris_train_epochs_done reads 0, because no epochs ran, and the momentum
   velocities are zero. */
IRIS_API int iris_train_elm(iris *k, float lam0, void *scratch, size_t scratch_bytes) { if (!k) return -1;
  const float g = 2.0f / iris_internal_sqrt((float)(k->n_in > 0 ? k->n_in : 1));
  return iris_internal_train_elm_ex(k, lam0, g, g, scratch, scratch_bytes);
}

/* ==========================================================================
   PART 9 — SAVING

   A trained instrument has to be a thing you can put somewhere and get back.
   The file carries the weights AND the demonstrations, so whoever receives it
   can keep working rather than inheriting a sealed box.

   THE FILE, format version 7. Every number is little-endian (least
   significant byte first) and is written and read one byte at a time, so a
   file means the same thing on every machine and the buffer you hand over
   needs no particular alignment. A float travels as its binary32 bit
   pattern. In the type column, u32 is an unsigned 32-bit integer, i32 a
   signed one, and f32 a float. Format 7 is the first format this library
   promises to keep reading in every later release (README.md, "What is
   promised").

     offset  field                                type     rule on load
     ------  -----------------------------------  -------  -------------------------
          0  magic: the letters I R I S           4 bytes  exact
          4  format version: 7                    u32      exact
          8  header bytes: 48, the offset of w1   u32      exact
         12  flags: bit 0 fitted, bit 1 trained   u32      no other bit set; trained
                                                           only together with fitted
         16  n_in, n_hid, n_out                   3 x u32  the receiving instrument's
                                                           shape, and
                                                           iris_internal_shape_fits
         28  n_ex, demonstrations stored          u32      at most the receiver's
                                                           capacity
         32  seed                                 u32      not 0
         36  next_id, the next identifier         u32      1 <= next_id <
                                                           IRIS_ID_LIMIT
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
             (CRC-32, see iris_internal_crc32)

   And the length: exactly what the header says, not a byte more or less.

   WHAT THE RULES ARE FOR. The checksum catches accidents -- a flipped bit in
   flash, a write cut off by a power failure -- but anyone can recompute it,
   so a file written by a buggy program, or edited by hand, arrives with a
   good checksum and whatever it happens to contain. The rules catch that
   second kind. Each one refuses a value that would go wrong later if it were
   let in:

     - The shape must match because the weights only mean something at the size
       they were trained at. iris_internal_shape_fits is asked too, because the
       loader's own working array is sized by this translation unit's
       IRIS_MAX_IN (see the note above iris_internal_shape_fits).
     - Unsigned throughout: a count with its top bit set is a large number
       that fails "at most the capacity", not a negative one that passes it.
     - next_id stays below IRIS_ID_LIMIT -- 2^31 - 1, the largest int32_t,
       where int has 32 bits, and 32,767 where it has 16 -- so a loaded
       instrument has at least one identifier left to hand out, and every
       identifier fits the int the functions return it as. iris_record hands
       out next_id and adds one, and refuses once next_id reaches the limit,
       so the count never overflows. The limit belongs to the receiving
       machine, like the capacity: the bytes mean the same everywhere.
     - A weight need only be finite. IRIS_W_LIMIT is backpropagation's
       detector for a runaway run, not a rule about valid instruments: the
       closed-form trainer (PART 8d) legitimately solves output weights beyond
       it (see the note at IRIS_W_LIMIT).
     - An input range may have zero width, an output range may not. An input
       that never moved during the demonstrations is stored with in_hi = in_lo,
       and iris_internal_norm_in reads it as 0 (PART 5), so that is a range
       this library writes. An output that never moved is given a small width
       of its own by iris_internal_fit_ranges, because the output scaling in
       PART 5 divides by the width, so a file carrying an output range of zero
       width did not come from this library. Either way lo must not pass hi,
       and the width must be a finite number.
     - An identifier that repeats would make "delete #3" ambiguous, and one
       at or above next_id would be handed out again by the next record.

   iris_load CHECKS EVERY RULE BEFORE IT WRITES ANYTHING. A refused file
   returns 0 and leaves the instrument you passed exactly as it was, every
   byte of it, its status included. There is no half-loaded instrument.

   AFTER A LOAD the instrument is the saved one, and it is at rest: fitted and
   trained come from the flags, the momentum velocities are zero, the
   learning rate and momentum are the defaults every instrument starts with,
   the record of which demonstration fights the others (PART 8f) is empty,
   any sliced training run is over, the status is IRIS_STATUS_OK, and
   iris_last_error is measured afresh over the demonstrations. For an
   instrument whose fit still matches its demonstrations that is the figure
   it reported before the save, to the bit. For a stale one -- a take
   recorded or deleted since the last fit -- it is not: the saved instrument
   still reported the error over the demonstrations it was fitted on, and
   the load measures the same weights over the demonstrations the file
   holds now.

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
   instrument belongs to one thread (see THREADING, above iris_get_status); do
   not save an instrument that something else may be touching.
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
IRIS_API uint32_t iris_internal_crc32(const void *buf, size_t n) {
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
  return (zero_width_ok ? lo <= hi : lo < hi) && !iris_internal_isbad(hi - lo);
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
  if (iris_internal_crc32(b, bytes - 4u) != iris_internal_get_u32(b + bytes - 4u)) return 0;
  if (iris_internal_get_u32(b + 32) == 0u) return 0;                   /* seed */
  next_id = iris_internal_get_u32(b + 36);
  if (next_id < 1u || next_id >= (uint32_t)IRIS_ID_LIMIT) return 0;
  if (iris_internal_get_u32(b + 40) == 0u) return 0;       /* random state */
  { const float s = iris_internal_get_f32(b + 44);
    if (iris_internal_isbad(s) || s < 0.0f || s > 1.0f) return 0; }

  p = b + IRIS_FILE_HEADER;
  for (i = 0; i < nh * ni + nh + no * nh + no; ++i, p += 4)
    if (iris_internal_isbad(iris_internal_get_f32(p))) return 0;     /* finite, no more */
  for (i = 0; i < ni; ++i)                       /* in_lo[i] against in_hi[i] */
    if (!iris_internal_range_ok(iris_internal_get_f32(p + 4 * i),
                                iris_internal_get_f32(p + 4 * (ni + i)), 1)) return 0;
  p += 8 * ni;
  for (i = 0; i < no; ++i)                     /* out_lo[i] against out_hi[i] */
    if (!iris_internal_range_ok(iris_internal_get_f32(p + 4 * i),
                                iris_internal_get_f32(p + 4 * (no + i)), 0)) return 0;
  p += 8 * no;
  for (i = 0; i < (size_t)n_ex * (ni + no); ++i, p += 4)
    if (iris_internal_isbad(iris_internal_get_f32(p))) return 0;

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
  iris_internal_put_u32(p, iris_internal_crc32(b, need - 4u));
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
  if (!iris_internal_shape_fits(k)) return 0;
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
  iris_internal_default_learning(k);
  iris_internal_zero_velocity(k);
  for (int i = 0; i < k->cap; ++i) k->ex_res[i] = 0.0f;
  k->res_epochs = 0;
  k->tr_done = 0; k->tr_ceiling = 0; k->tr_running = 0; k->tr_n_ex = 0;
  k->tr_ref = 0.0f; k->tr_err = 0.0f;
  k->status = IRIS_STATUS_OK;

  /* The training error is not in the file, but the fit and the demonstrations
     are, so it is measured exactly as every trainer measures it when it
     finishes (iris_internal_recall_error). A loaded instrument whose fit
     matches its demonstrations reports the bits the saved one did; a stale
     one reports the error over the demonstrations it holds now, which the
     saved one, not re-measured since its last fit, did not (see AFTER A
     LOAD above). */
  { float x[IRIS_MAX_IN];
    k->last_error = iris_internal_recall_error(k, x); }
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
   ZERO extra arena bytes: they are a weighted read of the demonstrations
   the instrument already carries. The demonstrations ARE the model, the
   design rule of this whole file taken to its logical end.

   What that means:

     - THIS ALGORITHM DOES NOT REROLL. There is no seed and nothing random;
       the same demonstrations always give the same instrument, bit for bit. It
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

     - SEAMS, ON PURPOSE. Between two demonstrations the output can step 31
       times more sharply than its mean step, where the MLP's morph steps
       1.9 times (docs/adr/0010-knn-the-sampler-beside-the-morpher.md records
       the measurement). That is the
       sampler character.

     - STRUCTURAL SAFETY. The answer is a weighted average of demonstrated
       outputs, held inside their range: it is never a not-a-number and
       never leaves the range you demonstrated, whatever finite reading comes
       in, however far from every take (a reading that is not finite is
       refused, with the substitute and IRIS_NAN_TRAPPED). When every
       neighbour carries the same value, the answer is exactly that value.

     - THE ACCURACY FLOOR. The MLP generalises better at EVERY count of
       demonstrations measured (2.4 times at 5, 2.1 times at 200;
       docs/adr/0010-knn-the-sampler-beside-the-morpher.md). Choose
       k-NN for its character, never for accuracy. For discrete outputs use
       iris_classify_1nn, which returns a stored label verbatim: a blend of
       labels is exactly a label only where all k neighbours agree, and a
       value between labels where they do not.

   Distances count each input in fractions of its demonstrated range, so a
   millimetre sensor and a g-force sensor count equally, and an input that
   never moved counts not at all (PART 5). The ranges are the instrument's own:
   the ones it was last fitted to, or, on an instrument that has never been
   fitted, the current demonstrations' ranges, fitted on every call. A fitted
   instrument keeps its ranges when you record or delete; train again to
   measure in the new ones. (iris_internal_fit_ranges alone would do it too,
   but it would also change what iris_predict plays, because the network was
   trained on the old ranges.) Ties resolve to the earliest-recorded
   demonstration, the same rule as Weka's LinearNNSearch, the search under
   desktop Wekinator's classifier, so decisions are comparable. The Java
   program itself has not been run against this code: the tests/audit.c
   check "1-NN: agrees with the Weka-IBk reference" compares it with a
   double-precision reference written to the same rules.

   Every saved instrument can play this way with nothing added to its file:
   the demonstrations and ranges are already in it. The file does not record which
   algorithm you play it with; that is a choice made at run time.
   ========================================================================== */

#define IRIS_KNN_MAXK 8          /* stack bound; k above this is clamped */
#define IRIS_KNN_GUARD 1e-9f     /* zero-distance guard for the weights */
#define IRIS_KNN_FAR 1e15f       /* the most ranges one input can count */

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

/* THE FAR DISTANCE, for when every ordinary distance has overflowed: the
   same count, with no input counting more than IRIS_KNN_FAR (10^15) ranges
   and a still input skipped outright.

   WHEN IT IS NEEDED. The ranges are the ones the instrument was last fitted
   to, and a take recorded since can lie any distance outside them: a take at
   1e20 on an input fitted to 0..1 is 1e20 ranges from a reading of 0.5, and
   the square of that overflows to infinity. With every stored take that far
   out no ordinary distance is finite, so a finite reading would have no
   nearest take: iris_knn_predict would play its substitute, outside the
   outputs demonstrated, iris_classify_1nn would answer -1 and
   iris_delete_nearest would delete nothing, with takes stored. The neighbour
   functions scan again with this distance instead. Capped, each input adds at
   most 10^30, so between finite numbers it is always finite, and below 10^38
   for any IRIS_MAX_IN up to 340,000. Skipping a still input keeps an overflowed
   difference times its zero scale from making a not-a-number.

   WHY ONLY THEN. The skip and the two comparisons per input slow the scan
   by more than half: 4,000 k-nearest and 1-nearest queries over 2,000 takes
   of 8 inputs take 0.186 s with them in the ordinary distance and 0.118 s
   without (Apple clang -O2, the development laptop). Used only when no
   ordinary distance is finite, they cost nothing otherwise, and every
   reading with a finite distance to some take plays exactly what it did
   without them. */
IRIS_API float iris_internal_distance2_far(const iris *k, const float *inv,
                                           const float *row, const float *in) {
  float d = 0.0f;
  for (int i = 0; i < k->n_in; ++i) {
    if (inv[i] == 0.0f) continue;
    float t = (row[i] - in[i]) * inv[i];
    if (t >  IRIS_KNN_FAR) t =  IRIS_KNN_FAR;
    if (t < -IRIS_KNN_FAR) t = -IRIS_KNN_FAR;
    d += t * t;
  }
  return d;
}

/* One row, index r at squared distance d, offered to the kk nearest found so
   far, which bd and bi hold nearest first. Strict <: on a tie the earlier
   take keeps its slot (the Weka rule). */
IRIS_API void iris_internal_knn_insert(float *bd, int *bi, int kk, int r, float d) {
  int p = kk;
  while (p > 0 && d < bd[p - 1]) --p;
  if (p < kk) {
    for (int q = kk - 1; q > p; --q) { bd[q] = bd[q-1]; bi[q] = bi[q-1]; }
    bd[p] = d; bi[p] = r;
  }
}

/* THE NEAREST DEMONSTRATION: the index of the stored row closest to `in`, the
   earliest-recorded on a tie (strict <, the Weka rule), or -1 when there is
   none -- an empty store, a shape too big for this translation unit, or a
   query whose distance to every row is not a number, which takes a reading
   or a stored value that is not finite. The search starts at the largest
   finite float, so every smaller distance counts however far outside the
   demonstrations the query is, and when no ordinary distance is finite it
   searches again with the far distance above, which between finite numbers
   always is. Like iris_knn_predict it fits the ranges
   of an instrument that has never been fitted -- but first it refuses a
   reading that is not finite, which has no nearest demonstration, and then
   the one thing it writes is the status, IRIS_NAN_TRAPPED, as iris_record
   does for a reading it refuses.

   iris_classify_1nn and iris_delete_nearest (PART 4) both use it, so the
   demonstration you delete by standing on it is the one the classifier
   names. */
IRIS_API int iris_internal_nearest(iris *k, const float *in) {
  if (!iris_internal_shape_fits(k) || k->n_ex == 0) return -1;
#ifndef IRIS_NO_GUARDS
  for (int i = 0; i < k->n_in; ++i)
    if (iris_internal_isbad(in[i])) { k->status = IRIS_NAN_TRAPPED; return -1; }
#endif
  if (!k->fitted) iris_internal_fit_ranges(k);
  float inv[IRIS_MAX_IN];
  iris_internal_neighbour_scale(k, inv);
  const int stride = k->n_in + k->n_out;
  int best = -1; float best_d = IRIS_FLT_MAX;
  for (int r = 0; r < k->n_ex; ++r) {
    const float d = iris_internal_distance2(k, inv, k->ex + (size_t)r * stride, in);
    if (d < best_d) { best_d = d; best = r; }
  }
  if (best < 0)
    for (int r = 0; r < k->n_ex; ++r) {
      const float d = iris_internal_distance2_far(k, inv, k->ex + (size_t)r * stride, in);
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
   below). That is why it takes a non-const instrument. A reading that is not
   finite is refused before any fitting, so then the status is all it
   writes. */
/* LENGTHS, same rule as iris_predict and just as unchecked.
   Reads exactly n_in floats from `in` and writes exactly n_out into `out`. */
IRIS_API void iris_knn_predict(iris *k, const float *in, float *out, int kk) { if (!k) return;
  if (!iris_internal_shape_fits(k)) {
    for (int o = 0; o < k->n_out; ++o) out[o] = iris_internal_centre(k, o);
    k->status = IRIS_NOT_FITTED;
    return;
  }
  const int NIn = k->n_in, NOut = k->n_out;
  if (k->n_ex <= 0) { for (int o = 0; o < NOut; ++o) out[o] = 0.0f; return; }
  /* THE NEIGHBOUR COUNT, clamped to at least 1 and at most n_ex and
     IRIS_KNN_MAXK. The test above says <= 0 rather than == 0, which tells
     gcc that n_ex is at least 1 from here on, and the lower clamp comes last,
     which makes kk at least 1 whatever n_ex is. Either of those on its own,
     as does filling every slot below, clears gcc-15's "bi may be used
     uninitialized" warning on tests/audit.c at -O2 and -O3. */
  if (kk > k->n_ex) kk = k->n_ex;
  if (kk > IRIS_KNN_MAXK) kk = IRIS_KNN_MAXK;
  if (kk < 1) kk = 1;

#ifndef IRIS_NO_GUARDS
  /* A reading that is not finite -- a disconnected sensor -- has no nearest
     demonstrations. Refuse it here, before the ranges below are fitted, so
     that the refusal changes nothing in the instrument but the status: the
     substitute, and IRIS_NAN_TRAPPED, as the MLP backstop does. */
  for (int i = 0; i < NIn; ++i)
    if (iris_internal_isbad(in[i])) {
      for (int o = 0; o < NOut; ++o) out[o] = iris_internal_centre(k, o);
      k->status = IRIS_NAN_TRAPPED;
      return;
    }
#endif
  /* The distance needs input ranges, and an instrument that has never been
     fitted has none of its own: iris_init's 0..1 describes nothing it was
     shown. So they are fitted here from the demonstrations -- the same work
     iris_internal_fit_ranges does, depending on nothing else -- rather than
     leaving the caller an ordering rule to forget. */
  if (!k->fitted) iris_internal_fit_ranges(k);

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

  /* The ordinary scan, and when it finds no finite distance at all -- every
     take far outside the ranges the instrument was fitted to -- the same scan
     with the far distance (see iris_internal_distance2_far). */
  for (int r = 0; r < k->n_ex; ++r)
    iris_internal_knn_insert(bd, bi, kk, r,
                             iris_internal_distance2(k, inv, k->ex + (size_t)r * stride, in));
  if (bi[0] < 0)
    for (int r = 0; r < k->n_ex; ++r)
      iris_internal_knn_insert(bd, bi, kk, r,
                               iris_internal_distance2_far(k, inv, k->ex + (size_t)r * stride, in));

#ifndef IRIS_NO_GUARDS
  /* A distance that is not a number to every take makes every comparison
     false, so no row is ever inserted and each bi[n] is still -1, and -1 *
     stride is an out-of-bounds read into whatever sits beside the arena. A
     reading that is not finite was refused above, and the far distance is
     finite between finite numbers, so that takes a stored value that is not
     finite: iris_record and iris_load refuse one, but the store is memory
     the caller can reach. Refuse instead: write the substitute
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
     exactly zero, so the answer is exactly y0. The ordinary form rounds: run
     through the checks in tests/playing.c, it fails to return the label on
     291,862 of 364,140 queries on stores whose labels all agree (a label of 3
     comes back as 2.9999998, which a C cast to int turns into class 2), and
     44,504 of 800,000 random queries land a few steps of float resolution
     outside the demonstrated range.

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
    out[o] = iris_internal_clampf(y0[o] + d, lo, hi);
  }
#ifndef IRIS_NO_GUARDS
  /* iris_record refuses a not-a-number, but the store is memory the caller
     can reach, and a NaN output that gets in there would be blended straight
     into an audio parameter. Same last line of defence as iris_predict. */
  for (int o = 0; o < NOut; ++o)
    if (iris_internal_isbad(out[o])) {
      out[o] = iris_internal_centre(k, o);
      k->status = IRIS_NAN_TRAPPED;
    }
#endif
}

/* 1-NN classification: snap to the single nearest demonstration and return
   its outputs VERBATIM (bit-for-bit) plus its identifier, or -1 if the store
   is empty, the shape is too big for this translation unit, or the reading
   is not finite. For a classifier task store the class
   label in out[0]; this then follows the rules of desktop Wekinator's
   default for discrete outputs, Weka's IBk nearest-neighbour classifier with
   k=1: distance normalised by each input's range, Euclidean (straight-line),
   the first-recorded demonstration winning a tie. Like iris_knn_predict it
   writes the status and the ranges of a never-fitted instrument, so it takes
   a non-const one. */
/* LENGTHS, same rule as iris_predict and just as unchecked.
   Reads exactly n_in floats from `in` and writes exactly n_out into `out`;
   `out` may be null when only the identifier is wanted. */
IRIS_API int iris_classify_1nn(iris *k, const float *in, float *out) { if (!k) return -1;
  if (!iris_internal_shape_fits(k)) {
    if (out) for (int o = 0; o < k->n_out; ++o) out[o] = iris_internal_centre(k, o);
    k->status = IRIS_NOT_FITTED;
    return -1;
  }
  const int NIn = k->n_in, NOut = k->n_out;
  if (k->n_ex == 0) {                 /* nothing to snap to: 0, as iris_predict */
    if (out) for (int o = 0; o < NOut; ++o) out[o] = 0.0f;
    return -1;
  }
  int best = iris_internal_nearest(k, in);   /* fits a never-fitted instrument,
                                                after refusing a reading that
                                                is not finite */
#ifndef IRIS_NO_GUARDS
  /* A query with no nearest row -- a disconnected sensor reading
     not-a-number, or a store whose every take holds a value that is not
     finite, which only a write into the arena can make -- gets none. Answering
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
      if (iris_internal_isbad(out[o])) {
        out[o] = iris_internal_centre(k, o);
        k->status = IRIS_NAN_TRAPPED;
      }
#endif
  }
  return (int)k->ex_id[best];
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
