/* SPDX-License-Identifier: BSD-3-Clause
   Copyright (c) 2026 Kyle Smith */
/* ============================================================================
   iris.h  —  interactive machine learning for handmade instruments
   v0.3.0 · single file · C99 · no dependencies · no malloc · no libc

   You show it a handful of examples of "when I do THIS, it sounds like THAT".
   It learns a mapping and fills in everything in between.

   This is the whole brain of the instrument. The same file compiles for a
   laptop, a web browser, and an ESP32-S3, because it contains no hardware,
   no operating system, and no library calls. It is pure arithmetic on
   memory you hand it.

   THE ZERO-DEPENDENCY CLAIM, STATED EXACTLY. A translation unit exercising
   the whole public API compiles under -std=c99 -ffreestanding -nostdlib at
   -O0/-O2/-Os and links with ZERO undefined symbols — but only with
   -fno-stack-protector. On a default macOS/clang invocation the compiler
   injects ___stack_chk_fail and ___stack_chk_guard. Those are a toolchain
   default, not a call this source makes, but the unqualified sentence
   "zero undefined symbols" is FALSE as literally stated on a default
   invocation. Say it with the flag, or say "no libc calls in the source".
   Verified 2026-08-27, Apple clang 17, arm64. xtensa-gcc UNVERIFIED.

   THE THREE RULES THIS FILE OBEYS
     1. No malloc.  You give it one block of memory; it never asks for more.
        You always know exactly how much RAM the instrument uses.
     2. No libc.    No printf, no math.h. Everything it needs is in here.
     3. No doubles. The ESP32-S3 does 32-bit float in hardware and 64-bit
        float in slow software emulation. Doubles would cost ~30x for no
        audible benefit.

   USAGE
     static unsigned char mem[IRIS_ARENA(2, 12, 3, 64)];
     iris *k = iris_init(mem, sizeof mem, 2, 12, 3, 64, 12345);

     iris_record(k, gesture, sound);     // do this a few times
     iris_train_converge(k, 0, 0, 0);    // trains until the error plateaus
     iris_predict(k, gesture, sound);    // now play

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

   ON OLD FILES. iris_save / iris_load carry the INPUT SCALING in the version
   word: v1 and v2 files were written when inputs were scaled to [0,1], v3
   onwards to [-1,+1]. An instrument loaded from an old file keeps the old
   scaling for as long as it exists — including across re-training, and it
   saves itself back as v2 — because its weights mean nothing else. That is
   automatic and you do not have to think about it.

   What you may want to offer the musician is the way out:

     if (!iris_input_scaling(k))          // 0 = this came from an old file
       if (asked_nicely()) iris_migrate_scaling(k);

   iris_migrate_scaling re-fits the same demonstrations under the new scaling.
   It is a NEW FIT, not a conversion: the instrument moves by about the fit
   error (0.045 measured), so it is the musician's decision, never a default.
   See PART 9.

   ============================================================================ */

#ifndef EMBWEK_H
#define EMBWEK_H

/* --------------------------------------------------------------------------
   FLOAT DETERMINISM CONTRACT

   "Same seed, same instrument" is a bitwise promise, and fused multiply-add
   contraction breaks it: the same source at -ffp-contract=off / on / fast
   produces three DIFFERENT weight blobs on Apple clang 17 / M4 (measured).
   Three defences, cheapest first:

   1. -ffast-math tripwire. fast-math implies contract=fast AND removes the
      NaN semantics the guards below depend on. Refuse to compile.          */
#if defined(__FAST_MATH__)
#error "iris: -ffast-math / -Ofast breaks same-seed bit-determinism and disables NaN trapping. Build without it."
#endif
/* 2. Forbid contraction at the source level. Clang honours this pragma at
      default and -ffp-contract=on (measured: blob becomes bit-identical to
      a -ffp-contract=off build); clang IGNORES it under -ffp-contract=fast,
      and GCC (incl. xtensa-esp32s3) ignores it always — those builds must
      pass -ffp-contract=off explicitly. The golden-blob audit check catches
      any build where neither defence held.                                 */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif
/* 3. Golden-blob audit vector (in tests/audit.c) — the runtime backstop.   */

#include <stddef.h>
#include <stdint.h>

/* THE ONLY VERSION NUMBER FOR THIS LIBRARY. Nothing else may state one.
   The predecessor repo carried four different numbers for one artifact (0.3.0
   here, 0.1.0 in the benchmark JSON, "v0.2" in one README, "v1.0" in a release
   note); that is fixed by having exactly one declaration and making every
   other document cite it.

   SEPARATE AXIS: the SAVE FILE format version (v1/v2/v3) is NOT this number.
   It carries the input-scaling semantics and has its own permanent-compat
   promise — see docs/adr/0006 and docs/adr/0018. A library version bump never
   invalidates a saved instrument; only a format bump can, and the loader keeps
   reading every older format. */
#define IRIS_VERSION_MAJOR 0
#define IRIS_VERSION_MINOR 4
#define IRIS_VERSION_PATCH 0
#define IRIS_VERSION_STRING "0.4.0"

#define IRIS_MAX_IN   32   /* sensor features in  */
#define IRIS_MAX_OUT  16   /* sound parameters out */
#define IRIS_MAX_EX   4096 /* demonstrations. THE BOUND EXISTS TO STOP AN
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
#define IRIS_MAX_HID  64   /* hidden units         */

#ifndef IRIS_API
#define IRIS_API static
#endif

/* --------------------------------------------------------------------------
   MEMORY

   Everything the instrument knows lives in one contiguous block you own.
   IRIS_ARENA() computes the size at compile time so you can write

       static unsigned char mem[IRIS_ARENA(2, 12, 3, 64)];

   and put it in .bss instead of on a heap. On an MCU this is the difference
   between "I know this fits" and "I hope this fits".
   -------------------------------------------------------------------------- */

#define IRIS_ARENA(NI, NH, NO, NEX)                                              \
  ( sizeof(iris)                                                                 \
  + sizeof(float) * ( 2*((NI)*(NH) + (NH) + (NH)*(NO) + (NO))  /* w + velocity */\
                    + (NH) + (NO) + (NH) + (NO)                /* acts + deltas */\
                    + 2*((NI) + (NO))                          /* norm ranges   */\
                    + (size_t)(NEX)                            /* residual ledger*/\
                    + (size_t)(NEX) * ((NI) + (NO)) )          /* examples      */\
  + sizeof(int32_t) * (size_t)(NEX) * 2                        /* ids + shuffle */\
  + 64 )                                                       /* alignment pad */

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
  IRIS_DIVERGED_STUCK    = 5,  /* a previous run diverged and left weights at the
                                  clamp. Training refuses until the instrument is
                                  rerolled (iris_retrain_new) — the examples are
                                  intact, the weights are not. See the note at
                                  iris__train_run.                             */
  IRIS_NOT_FITTED        = 4   /* iris_predict was called on an instrument that has
                                never been fitted. Outputs are the centre of the
                                demonstrated range (0 with no demonstrations),
                                never the forward pass over random weights.  */
} iris_status;

/* NaN or Inf, by bit pattern — exponent field all ones. No libc, no fenv,
   and immune to -ffinite-math-only style optimisations on the comparison. */
IRIS_API int iris_isbad(float x) {
  union { float f; uint32_t u; } c; c.f = x;
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

/* Padé approximation of tanh.

   ACCURACY, stated honestly: maximum absolute error 0.0283, at x = 4.9. The
   "about 0.001" figure this comment used to carry holds only for |x| < 0.31
   and was wrong by 28x everywhere else.

   IT ALSO LEAVES THE CODOMAIN. For 3 < |x| <= 4.9 this returns a magnitude
   slightly above 1.0, which makes the backprop factor y*(1-y) go negative in
   that band — bounded at 5.7% of the peak derivative, and invisible to the
   guards. This is a real algebraic defect and it is documented rather than
   fixed on purpose: over 100 seeds, clamping the derivative to >= 0 moves
   recall and grid RMSE only in the fourth decimal, and substituting the
   mathematically exact Pade derivative makes both MEASURABLY WORSE. Swapping
   in libm tanhf is a coin toss (21 of 40 seeds favour this routine, paired
   t = 0.06). See research/prior-art/CORE-AUDIT-vs-wekinator.md section 4.6. */
IRIS_API float iris_tanh(float x) {
  if (x >  4.9f) return  1.0f;
  if (x < -4.9f) return -1.0f;
  const float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Logistic / sigmoid, squashes anything into (0,1). Built from tanh so we
   only have to be fast once. */
IRIS_API float iris_sigmoid(float x) { return 0.5f * (iris_tanh(0.5f * x) + 1.0f); }

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

  /* WHICH INPUT SCALING THIS INSTRUMENT USES, and why it is per-instrument
     state rather than a build option. 0 = [0,1], the v0.1/v0.2 scaling, kept
     for every file ever written by those versions. 1 = [-1,+1], which is what
     Weka's MultilayerPerceptron does with normalizeAttributes on — the
     setting Wekinator ships — and what LeCun et al. 1998 ("Efficient
     BackProp", 4.3) prescribes: uncentered inputs give every first-layer
     weight a gradient of the same sign, so the descent has to zig-zag.
     Measured on the 8-output reference task at 600 epochs: train MSE
     5.94e-4 -> 6.38e-5 (9.3x), grid RMSE 0.0129 -> 0.0084 (1.54x).

     iris_load SETS THIS FROM THE FILE VERSION. A stored weight only means
     something against the scaling it was trained in, so the two travel
     together or the instrument silently becomes a different instrument. */
  int32_t in_center;

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
  float   tr_ref;          /* error one plateau-window ago */
};

IRIS_API iris_status iris_get_status(const iris *k) { return (iris_status)k->status; }

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
IRIS_API size_t iris_size(int n_in, int n_hid, int n_out, int cap) {
  if (n_in < 1 || n_in > IRIS_MAX_IN)   return 0;
  if (n_out < 1 || n_out > IRIS_MAX_OUT) return 0;
  if (n_hid < 1 || n_hid > IRIS_MAX_HID) return 0;
  if (cap  < 1 || cap  > IRIS_MAX_EX)   return 0;
  return sizeof(iris)
       + sizeof(float) * (size_t)( 2*(n_in*n_hid + n_hid + n_hid*n_out + n_out)
                                 + n_hid + n_out + n_hid + n_out
                                 + 2*(n_in + n_out)
                                 + (size_t)cap
                                 + (size_t)cap * (n_in + n_out) )
       + sizeof(int32_t) * (size_t)cap * 2
       + 64;
}

/* Randomise the weights. This is the reroll.

   The scale matters. Each hidden unit adds up n_in incoming signals, so if
   the weights are too large the sum lands far out where tanh is flat, the
   error signal underneath it goes to nearly zero, and the network stops
   learning before it starts. Dividing by the square root of the number of
   inputs keeps the sums in the responsive part of the curve. This is a
   standard trick and it is the difference between "trains in 50 ms" and
   "never trains at all". */
IRIS_API void iris_reseed(iris *k, uint32_t seed) {
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
  if (n_hid < 1 || n_hid > IRIS_MAX_HID) return 0;
  if (cap   < 1 || cap > IRIS_MAX_EX) return 0;      /* see IRIS_MAX_EX: overflow */
  if (bytes < iris_size(n_in, n_hid, n_out, cap)) return 0;

  unsigned char *p = (unsigned char *)mem;
  iris *k = (iris *)p;  p += sizeof(iris);
  /* align to 8 bytes so float loads are never unaligned */
  p += ((uintptr_t)p & 7u) ? (8u - ((uintptr_t)p & 7u)) : 0u;

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
  k->order = (int32_t *)p; p += sizeof(int32_t) * (size_t)cap;

  k->n_ex = 0; k->next_id = 1;
  k->lr = 0.10f; k->momentum = 0.85f;
  /* A FRESH instrument is a v3 instrument: inputs in [-1,+1]. Only iris_load
     of a v1/v2 file moves it back, and only for that instrument. */
  k->in_center = 1;
  for (int i = 0; i < cap; ++i) k->ex_res[i] = 0.0f;
  k->res_epochs = 0;
  k->tr_done = 0; k->tr_ceiling = 0; k->tr_running = 0; k->tr_ref = 0.0f;
  for (int i = 0; i < n_in;  ++i) { k->in_lo[i]  = 0.0f; k->in_hi[i]  = 1.0f; }
  for (int i = 0; i < n_out; ++i) { k->out_lo[i] = 0.0f; k->out_hi[i] = 1.0f; }
  iris_reseed(k, seed);
  return k;
}

IRIS_API void iris_set_learning(iris *k, float lr, float momentum) {
  k->lr = iris_clampf(lr, 0.0001f, 2.0f);
  k->momentum = iris_clampf(momentum, 0.0f, 0.99f);
}

/* ==========================================================================
   PART 4 — THE EXAMPLE STORE

   Add, inspect, delete. Deleting one example is a five-line function and its
   absence is the single biggest usability failure in every embedded system
   that has attempted this. One mistimed button press should not cost you
   twenty minutes of work.
   ========================================================================== */

IRIS_API int iris_count(const iris *k) { return k->n_ex; }
IRIS_API int iris_capacity(const iris *k) { return k->cap; }

IRIS_API int iris_record(iris *k, const float *in, const float *out) {
  if (k->n_ex >= k->cap) return -1;                 /* full — say so, loudly */

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
    const int st = k->n_in + k->n_out;
    for (int i = 0; i < k->n_in;  ++i)
      if (iris_isbad(in[i]))  { k->status = IRIS_NAN_TRAPPED; return -1; }
    for (int i = 0; i < k->n_out; ++i)
      if (iris_isbad(out[i])) { k->status = IRIS_NAN_TRAPPED; return -1; }
    (void)st;
  }
#endif

  const int stride = k->n_in + k->n_out;
  float *row = k->ex + (size_t)k->n_ex * stride;
  for (int i = 0; i < k->n_in;  ++i) row[i] = in[i];
  for (int i = 0; i < k->n_out; ++i) row[k->n_in + i] = out[i];
  k->ex_id[k->n_ex] = k->next_id++;
  k->n_ex++;
  k->trained = 0;                                   /* model is now stale */
  return k->ex_id[k->n_ex - 1];
}

IRIS_API int iris_index_of(const iris *k, int id) {
  for (int i = 0; i < k->n_ex; ++i) if (k->ex_id[i] == id) return i;
  return -1;
}

/* The stable id at a position, without copying the row out. */
IRIS_API int iris_id_at(const iris *k, int idx) {
  return (idx < 0 || idx >= k->n_ex) ? -1 : k->ex_id[idx];
}

IRIS_API int iris_get(const iris *k, int idx, float *in, float *out) {
  if (idx < 0 || idx >= k->n_ex) return 0;
  const int stride = k->n_in + k->n_out;
  const float *row = k->ex + (size_t)idx * stride;
  if (in)  for (int i = 0; i < k->n_in;  ++i) in[i]  = row[i];
  if (out) for (int i = 0; i < k->n_out; ++i) out[i] = row[k->n_in + i];
  return k->ex_id[idx];
}

IRIS_API int iris_delete_index(iris *k, int idx) {
  if (idx < 0 || idx >= k->n_ex) return 0;
  const int stride = k->n_in + k->n_out;
  for (int r = idx; r < k->n_ex - 1; ++r) {
    float *dst = k->ex + (size_t)r * stride;
    const float *src = k->ex + (size_t)(r + 1) * stride;
    for (int c = 0; c < stride; ++c) dst[c] = src[c];
    k->ex_id[r] = k->ex_id[r + 1];
  }
  k->n_ex--;
  k->trained = 0;
  return 1;
}

IRIS_API int iris_delete_id(iris *k, int id) { return iris_delete_index(k, iris_index_of(k, id)); }
IRIS_API int iris_delete_last(iris *k) { return iris_delete_index(k, k->n_ex - 1); }

/* Delete whichever example is closest to where you are standing right now.
   On a device with three buttons this is how you say "not THAT one" without
   needing to read a list. */
IRIS_API int iris_delete_nearest(iris *k, const float *in) {
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

IRIS_API void iris_clear(iris *k) { k->n_ex = 0; k->trained = 0; k->fitted = 0; }

/* ==========================================================================
   PART 5 — NORMALISATION

   Find the range of every input and output across the examples, then map
   everything into a common scale before training.

   Outputs go to 0.1–0.9 rather than 0–1 on purpose. The output layer uses a
   sigmoid, which can only *approach* 0 and 1 and never reach them. Asking it
   to hit exactly 1.0 means pushing a weight toward infinity forever. Leaving
   headroom at both ends means the network can actually arrive.
   ========================================================================== */

#define IRIS_OUT_LO 0.1f
#define IRIS_OUT_HI 0.9f

IRIS_API void iris_fit_ranges(iris *k) {
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
  for (int i = 0; i < k->n_in;  ++i) if (k->in_hi[i]  - k->in_lo[i]  < 1e-6f) k->in_hi[i]  = k->in_lo[i]  + 1e-6f;
  for (int i = 0; i < k->n_out; ++i) if (k->out_hi[i] - k->out_lo[i] < 1e-6f) k->out_hi[i] = k->out_lo[i] + 1e-6f;
}

/* THE INPUT SCALING. Two of them, chosen per instrument by k->in_center,
   which iris_load sets from the file version. See the field's comment in
   struct iris for the measurement and the citation; see PART 9 for what
   happens to a saved instrument if the [0,1] branch is ever deleted. */
IRIS_API float iris_norm_in (const iris *k, int i, float v) {
  const float t = (v - k->in_lo[i]) / (k->in_hi[i] - k->in_lo[i]);
  return k->in_center ? (2.0f * t - 1.0f) : t;
}

/* Put this instrument back on the v0.1/v0.2 input scaling.

   WHO ACTUALLY CALLS THIS: tests/audit.c only, to hold the pre-v3 training
   path against its frozen hash. iris_load does NOT call it — iris_load sets
   k->in_center directly from the file's version word (see PART 9). An earlier
   version of this comment said otherwise and was wrong; adr/0018 repeats the
   same error and is also wrong. PART 9's comment is the correct account.

   Nothing else should call it: changing the scaling under trained weights
   changes what those weights mean. */
IRIS_API void iris__set_legacy_norm(iris *k, int legacy) { k->in_center = legacy ? 0 : 1; }

/* WHICH SCALING IS THIS INSTRUMENT ON. 0 = the legacy [0,1] of v1/v2 files,
   1 = the centred [-1,+1] of v3. A UI needs this to tell the musician why an
   instrument restored from an old file did not get the better fit, and to
   offer iris_migrate_scaling. */
IRIS_API int iris_input_scaling(const iris *k) { return k->in_center ? 1 : 0; }
IRIS_API float iris_norm_out(const iris *k, int i, float v) {
  float t = (v - k->out_lo[i]) / (k->out_hi[i] - k->out_lo[i]);
  return IRIS_OUT_LO + t * (IRIS_OUT_HI - IRIS_OUT_LO);
}
IRIS_API float iris_denorm_out(const iris *k, int i, float y) {
  float t = (y - IRIS_OUT_LO) / (IRIS_OUT_HI - IRIS_OUT_LO);
  return k->out_lo[i] + t * (k->out_hi[i] - k->out_lo[i]);
}

/* ==========================================================================
   PART 6 — FORWARD PASS  (this is "playing the instrument")

     hidden_h = tanh( sum_i w1[h][i] * input_i + b1[h] )
     output_o = sigmoid( sum_h w2[o][h] * hidden_h + b2[o] )

   That is the entire model. Two matrix multiplies with a squashing function
   after each one. For 2 inputs, 12 hidden and 3 outputs that is 60
   multiply-adds — about one microsecond on the S3. Playing is free; only
   learning costs anything.
   ========================================================================== */

IRIS_API void iris_forward_norm(const iris *k, const float *x_norm) {
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

IRIS_API void iris_predict(const iris *k, const float *in, float *out) {
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

   Distance from the current gesture to the nearest thing you demonstrated,
   scaled so that 0 means "exactly on an example" and 1 means "as far away as
   the examples are from each other".

   This costs one pass over the examples — nothing. But it lets the instrument
   know when it is improvising rather than recalling, which you can map to
   anything you like: noise, detuning, a light. As far as I can find, nobody
   has done this, and it is four lines.
   ========================================================================== */

IRIS_API float iris_novelty(const iris *k, const float *in) {
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
IRIS_API int iris__check_weights(iris *k) {
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

   The masthead used to recommend 600 epochs. Measured on the 8-output
   reference task, 20 examples, nh=12, mean of 9 seeds, that budget stops the
   optimiser less than a fifth of the way down:

       epochs      train MSE      recall      grid RMSE     host ms
          600       7.87e-4       0.0112        0.0158          1.5
        2 000       2.77e-4       0.0078        0.0136          4.9
        6 000       2.58e-5       0.0032        0.0094         14.5
       20 000       8.87e-6       0.0019        0.0086         50.0
       60 000       3.23e-6       0.0012        0.0083        150.0
      200 000       2.19e-6       0.0009        0.0083        501.1

   Same code, same seeds, one integer. Recall improves 5.9x and held-out grid
   error 1.8x for nothing but time, and the project's brief is explicit that
   time is the cheap thing.

   So the budget is no longer a number the caller guesses. iris_train_converge
   runs until the training error PLATEAUS: every IRIS_CONV_WINDOW epochs it
   compares the error against the error one window ago and stops when the
   window bought less than IRIS_CONV_TOL of it. Window and tolerance are
   measured, not guessed — a short window (200-500 epochs) mistakes the
   ordinary epoch-to-epoch noise of a shuffled SGD trace for a plateau and
   stops at a quarter of the achievable fit.

   THE ONE PLACE THIS IS NOT FREE. At 50 examples the fixed 200,000-epoch
   budget is measurably WORSE on held-out grid error than 60,000 (0.0065 vs
   0.0063) — the point where more convergence starts costing generalisation.
   A plateau criterion stops before that on its own; a bigger constant would
   not have. That is the argument for a criterion over a constant.

   AND THE CAVEAT THAT GOVERNS THE WHOLE TABLE. The truth function these
   numbers come from is smooth and noiseless. "More convergence never hurts"
   is exactly the conclusion most at risk from real sensor noise and human
   inconsistency, and none of this is verified on hardware or on recorded
   human gesture. Treat the ceiling as a ceiling.

   HONEST PROGRESS. A converged run at 50 examples is ~4 s on the S3, which
   is long enough that the glass must show something true. Two ways in, both
   costing nothing:

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
   fidelity path — fixed-epoch backprop is what Weka's MultilayerPerceptron
   does — and audit check 12 pins its output to the bit.
   -------------------------------------------------------------------------- */

#define IRIS_CONV_WINDOW  2000    /* epochs between plateau tests (measured)   */
#define IRIS_CONV_TOL     0.10f   /* stop when a window buys < 10% of the error */
#define IRIS_CONV_CEILING 60000   /* hard stop; ~150 ms host / ~4.8 s S3 at 20 ex */

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
   no training returns -1.0f and leaves `trained` alone. Previously this path
   returned k->last_error on refusal, so a caller reading only the return value
   could not tell a refusal from a repeat of the previous run — while the
   L-BFGS trainer (now experimental/iris_lbfgs.h) already returned -1.0f for
   the same situation. Two conventions, one library. Fixed 2026-08-26. */
IRIS_API float iris__train_run(iris *k, int epochs, int conv, int resume,
                           iris_progress_fn cb, void *user) {
  if (k->n_ex == 0) return -1.0f;

#ifndef IRIS_NO_GUARDS
  /* THE DIVERGENCE TRAP, AND WHY THIS REFUSAL EXISTS.
     When a run diverges, iris__check_weights clamps the offending weights to
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
  if (!resume && k->status == IRIS_TRAINING_DIVERGED) {
    int pinned = 0, i;
    const float lim = IRIS_W_LIMIT - 0.01f;
    for (i = 0; i < k->n_hid * k->n_in;  ++i)
      if (k->w1[i] >= lim || k->w1[i] <= -lim) pinned = 1;
    for (i = 0; i < k->n_out * k->n_hid; ++i)
      if (k->w2[i] >= lim || k->w2[i] <= -lim) pinned = 1;
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
     the instrument (denormalisation reads them on every predict). L-BFGS and
     ELM already scan first; this path once fitted first, and a refusal
     silently moved out_lo/out_hi. */
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
  const int NI = k->n_in, NH = k->n_hid, NO = k->n_out;
  float x[IRIS_MAX_IN], t[IRIS_MAX_OUT];
  float err = 0.0f;

  if (!resume) {
    for (int i = 0; i < k->n_ex; ++i) k->order[i] = i;
    for (int i = 0; i < k->cap;  ++i) k->ex_res[i] = 0.0f;
    k->res_epochs = 0;
    k->tr_done = 0;
    k->tr_ref = 0.0f;
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
      for (int o = 0; o < NO; ++o) t[o] = iris_norm_out(k, o, row[NI + o]);

      iris_forward_norm(k, x);

      /* --- output layer error ---------------------------------------------
         d_out = (predicted - target) * sigmoid'(z), and sigmoid'(z) is
         conveniently y*(1-y) using the value we already computed. */
      float rse = 0.0f;
      for (int o = 0; o < NO; ++o) {
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
         tanh'(z) = 1 - tanh(z)^2, again reusing the stored activation. */
      for (int h = 0; h < NH; ++h) {
        float acc = 0.0f;
        for (int o = 0; o < NO; ++o) acc += k->w2[(size_t)o * NH + h] * k->d_out[o];
        float a = k->hid[h];
        k->d_hid[h] = acc * (1.0f - a * a);
      }

      /* --- apply the nudges, with momentum -------------------------------- */
      for (int o = 0; o < NO; ++o) {
        float g = k->d_out[o];
        float *w = k->w2 + (size_t)o * NH, *v = k->v_w2 + (size_t)o * NH;
        for (int h = 0; h < NH; ++h) {
          v[h] = IRIS_FLUSH(k->momentum * v[h] - k->lr * g * k->hid[h]);
          w[h] += v[h];
        }
        k->v_b2[o] = IRIS_FLUSH(k->momentum * k->v_b2[o] - k->lr * g);
        k->b2[o]  += k->v_b2[o];
      }
      for (int h = 0; h < NH; ++h) {
        float g = k->d_hid[h];
        float *w = k->w1 + (size_t)h * NI, *v = k->v_w1 + (size_t)h * NI;
        for (int i = 0; i < NI; ++i) {
          v[i] = IRIS_FLUSH(k->momentum * v[i] - k->lr * g * x[i]);
          w[i] += v[i];
        }
        k->v_b1[h] = IRIS_FLUSH(k->momentum * k->v_b1[h] - k->lr * g);
        k->b1[h]  += k->v_b1[h];
      }
    }
    err /= (float)(k->n_ex * NO);
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
      int st = iris__check_weights(k);
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
   behaviour: our hidden units are Pade tanh against their logistic, our output
   units are sigmoid-then-clamp against their unthresholded linear, our
   defaults are lr 0.10 / momentum 0.85 against their 0.3 / 0.2, and we
   reshuffle every epoch where they shuffle once. Same rule, different
   quantities entering it, therefore different trajectories.

   Every "bit-identical" claim in this file is a claim about THIS FILE's
   self-consistency — sliced vs unsliced runs, save/load round trips, -O0 vs
   -O3 — never about Weka. Audit check 12 hashes the weights this produces.
   Do not "improve" it; iris_train_converge is where improvements go.
   Full audit: research/prior-art/CORE-AUDIT-vs-wekinator.md */
IRIS_API float iris_train_epochs(iris *k, int epochs) {
  k->tr_ceiling = epochs > 0 ? epochs : 0;
  k->tr_running = 0;
  return iris__train_run(k, epochs, 0, 0, 0, 0);
}

/* Train until the training error plateaus. ceiling <= 0 takes
   IRIS_CONV_CEILING. cb may be NULL. Returns the final mean squared error. */
IRIS_API float iris_train_converge(iris *k, int ceiling, iris_progress_fn cb, void *user) {
  const int ceil_ = ceiling > 0 ? ceiling : IRIS_CONV_CEILING;
  k->tr_ceiling = ceil_;
  k->tr_running = 1;
  {
    float e = iris__train_run(k, ceil_, 1, 0, cb, user);
    k->tr_running = 0;
    return e;
  }
}

/* The same run, in slices, for a UI that must keep drawing.
     iris_train_begin(k, ceiling);
     while (iris_train_slice(k, 500)) { draw(iris_train_progress(k)); poll(); }
   Bit-identical to iris_train_converge with the same ceiling: the shuffle
   buffer is initialised once, here, and carried across every slice. */
IRIS_API int iris_train_begin(iris *k, int ceiling) {
  if (k->n_ex == 0) return 0;
  k->tr_ceiling = ceiling > 0 ? ceiling : IRIS_CONV_CEILING;
  k->tr_done = 0;
  k->tr_ref = 0.0f;
  k->tr_running = 1;
  for (int i = 0; i < k->n_ex; ++i) k->order[i] = i;
  for (int i = 0; i < k->cap;  ++i) k->ex_res[i] = 0.0f;
  k->res_epochs = 0;
  return 1;
}

/* Runs at most `epochs` more. Returns 1 if there is more to do, 0 when the
   run has finished (plateau, ceiling, early stop, or a guard). */
IRIS_API int iris_train_slice(iris *k, int epochs) {
  if (!k->tr_running) return 0;
  {
    int left = k->tr_ceiling - k->tr_done;
    if (epochs <= 0) epochs = IRIS_CONV_WINDOW;
    if (epochs > left) epochs = left;
    if (epochs <= 0) { k->tr_running = 0; return 0; }
    iris__train_run(k, epochs, 1, 1, 0, 0);
  }
  if (k->tr_done >= k->tr_ceiling) k->tr_running = 0;
  return k->tr_running;
}

/* 0.0 at the start, 1.0 when the run has finished. While a converged run is
   still going this is epochs-spent / ceiling, which is a LOWER bound — the
   run will usually stop early — so the bar never goes backwards and never
   claims to be further along than it is. */
IRIS_API float iris_train_progress(const iris *k) {
  if (!k->tr_running) return 1.0f;
  if (k->tr_ceiling <= 0) return 1.0f;
  {
    float f = (float)k->tr_done / (float)k->tr_ceiling;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
  }
}
IRIS_API int iris_train_busy(const iris *k) { return k->tr_running; }

/* How many epochs the last run ACTUALLY did. Compare against what you asked
   for: fewer means it stopped early, and there are four rules that can do
   that — the plateau test, the error floor (1e-6), the divergence guard, or a
   progress callback returning 0. iris_get_status() distinguishes the guard;
   this distinguishes "ran to completion" from "stopped for a good reason",
   which iris_train_progress() deliberately cannot, because it reports 1.0 for
   any finished run. Added 2026-08-26 — before this there was no way to tell. */
IRIS_API int iris_train_epochs_done(const iris *k) { return k->tr_done; }

/* Train and immediately reroll from a fresh random start. This is the
   "give me a different instrument from the same examples" button — the thing
   a deterministic model fundamentally cannot offer. */
IRIS_API float iris_retrain_new(iris *k, uint32_t seed, int epochs) {
  iris_reseed(k, seed);
  return iris_train_epochs(k, epochs);
}


/* ==========================================================================
   PART 8f — WHICH DEMONSTRATION IS FIGHTING THE OTHERS

   The trial-and-error trap in interactive ML is that when the instrument
   feels wrong you have no idea WHICH of your twenty demonstrations is wrong,
   so you re-record at random. This points at one. It costs one float per
   example slot and one add per example per epoch.

   THE OBVIOUS THING DOES NOT WORK, AND TRAINING TO CONVERGENCE IS WHY.
   The obvious thing is the final training residual: after training, ask each
   example how badly the model still misses it. Corrupt one demonstration of
   twenty by +mag on one output and count how often it ranks first out of
   twenty (nh=12, 8 outputs, 20 trials, iris_train_converge):

     corruption                        +0.05  +0.10  +0.20  +0.40
     ----------------------------------------------------------------
     final residual                     8/20   5/20   2/20   2/20
     leave-one-out CV (n retrains)      1/20   1/20  10/20  11/20
     INTEGRATED residual (this)         6/20  18/20  17/20  17/20

   Read the first row backwards. The final residual gets WORSE as the mistake
   gets BIGGER — 2/20 at +0.40 is barely above the 1/20 you would get by
   guessing. Of course it does: give the optimiser enough epochs and it bends
   the surface far enough to fit the bad point too, after which the bad point
   looks like every other point. The same measurement at 6,000 epochs scores
   16/20 at +0.05 and 8/20 at +0.40, which is how a detector measured at one
   budget and shipped at another becomes a feature that quietly does nothing.

   WHAT WORKS IS THE INTEGRAL, NOT THE ENDPOINT. An example that agrees with
   its neighbours is fitted early and stays fitted. An example that
   contradicts them stays wrong for thousands of epochs while the optimiser
   trades it against everything else. Summing each example's squared error
   over every epoch of the run measures how long it fought, and that is
   stable across budgets: 5/17/17/18 at 6,000 epochs, 9/17/17/16 at 20,000,
   11/16/17/15 at 60,000.

   THE +0.05 COLUMN IS HONEST AND STAYS HONEST. A 5% offset on one of eight
   outputs is smaller than the spread between two takes of the same human
   gesture. Nothing in this table finds it reliably and this function does
   not pretend to.

   HOW LOUD TO BE. The stress LEVEL cannot be thresholded, because the worst
   of n scores rises with n whether or not anything is wrong — measured on
   clean data with no corrupted example at all, worst-of-n stress runs to
   1.76 at 5 examples, 2.95 at 20, 6.34 at 50 and 9.23 at 100. A UI wired to
   a fixed level would be silent on small rigs and would cry wolf on large
   ones. The MARGIN — worst divided by second-worst — is the statistic that
   does not drift: on clean data its maximum over 40 trials is 1.44 at 20
   examples, 2.27 at 50 and 2.06 at 100.

     examples   clean margin (median / max)   with a +0.40 demo (median / max)
     --------   --------------------------   -------------------------------
            5        1.12 / 1.55                     1.33 / 5.10
           10        1.35 / 5.04                     1.24 / 2.35
           20        1.12 / 1.44                     2.00 / 4.14
           50        1.43 / 2.27                     5.07 / 30.89
          100        1.37 / 2.06                    27.95 / 83.47

   BELOW ~12 EXAMPLES THERE IS NOTHING TO SAY AND THIS FUNCTION SAYS NOTHING.
   Look at the 5 and 10 rows: the corrupted set is indistinguishable from the
   clean one, and at 10 examples the clean margin reaches 5.04 — a false
   accusation. That is not a tuning failure, it is the situation: an example
   can only be caught disagreeing with a crowd if there is a crowd. Under
   IRIS_STRESS_MIN_EX the call returns -1.

   CAVEAT, the same one that governs everything in this file: the corruption
   above is a clean offset on a smooth, noiseless truth. Real demonstrations
   are inconsistent in ways that are not one displaced output, and none of
   this has been checked against a recorded human gesture.

   COST. sizeof(float) * cap in the arena — 512 B at cap 128, 1 KB at cap 256
   — and one float add per example per epoch, which is under 0.1% of the
   backprop work already being done for that example. Not free; that is the
   price.
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
IRIS_API float iris_example_stress(const iris *k, int idx) {
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

   USE iris_worst_example_id, NOT iris_worst_example + iris_id_at. The _id form
   returns the stable example ID directly and is what the one production
   consumer calls (firmware/app/core/surface.c:385). This comment previously
   prescribed the index form composed with iris_id_at; that idiom has no caller
   anywhere, and iris_id_at's only appearance in the tree is inside this very
   comment. Both remain public API for callers who want the positional index,
   but they are not the recommended path.

   The index is returned even below the flag because the ranking is still
   real and a UI may want to show it quietly (a dimmer mark, say) without
   accusing anything. Ties go to the earliest-recorded example, the same rule
   as iris_knn_predict. */
IRIS_API int iris_worst_example(const iris *k, float *margin) {
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
IRIS_API int iris_worst_example_id(const iris *k, float *margin) {
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

   ⚠️ THE FIRMWARE DOES NOT DO THIS, AND HAS NOT SINCE 22 AUG 2026. D11.3
   (admin/DECISIONS.md:181) dropped iris_correct from the device; the firmware's
   record and delete paths both go to the ELM solve instead. Grep confirms zero
   uses of iris_correct anywhere under firmware/app. This banner previously
   claimed the policy was "also the firmware's" — it was not. adr/0005 (still
   marked accepted), firmware/library/README.md and RELEASE-v1.0.md carry the
   same stale claim. iris_correct is a library facility with no production
   caller. Treat it as such until D11.3 is revisited.

   Determinism becomes event-sourced: replaying the identical operation
   history (records / corrections / deletes, in order) reproduces the
   instrument bit-exactly, because the corrections draw from the same rng
   stream. The seed ALONE now reproduces only a from-scratch retrain — a
   saved file carries the live rng state (format v2, below) so a reloaded
   instrument continues exactly where it left off.

   Zeroing the velocity at entry is what makes that cheap: it turns the
   momentum arrays into transient scratch instead of hidden persistent state,
   so the file needs one extra word (rng), not four extra weight arrays.
   A converged net's velocities are already ~0; measured cost of the zeroing:
   every correction metric identical to 4 decimals.
   ========================================================================== */

IRIS_API void iris_zero_velocity(iris *k) {
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
IRIS_API float iris_correct(iris *k, int epochs) {
  iris_zero_velocity(k);
  return iris_train_epochs(k, epochs > 0 ? epochs : 20);
}

IRIS_API int   iris_is_trained(const iris *k) { return k->trained; }
IRIS_API float iris_last_error(const iris *k) { return k->last_error; }
IRIS_API uint32_t iris_seed(const iris *k)    { return k->seed; }

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
   weights. Frozen, at that scale, tanh of a [0,1] input barely bends — the
   random features are nearly collinear and the normal matrix is numerically
   rank-deficient. The frozen layer is drawn at 2/sqrt(n_in) instead, wide
   enough that the features have real capacity.

   ⚠️ PROVENANCE OF THE 2/sqrt(n_in): this comment used to cite a "measured
   optimum of {0.71..4.0}" from a gain sweep. That sweep is not in the tree and
   cannot be reproduced through the shipped API — iris_train_elm_ex takes
   separate gain_w and gain_b, but its only caller passes the same value for
   both, so two of its six parameters are constants in practice and the split
   the sweep explored is unreachable. Treat 2/sqrt(n_in) as a working default
   whose superiority over 1/sqrt(n_in) is argued structurally above (collinear
   features, rank-deficient normal matrix) and is NOT currently backed by a
   reproducible measurement. Re-running that sweep is on the open list.

   RIDGE, MANDATORY. Even with the wider gain, the unridged float32 normal
   matrix failed Cholesky in EVERY realistic scenario measured — including
   20 well-spread examples. The ridge is relative (lam0 * trace/(nh+1), so
   it scales with the data) and escalates deterministically: double lambda
   on a failed factorisation, at most 8 times, report the count. Measured
   across six hostile scenarios x five lambdas x three widths: zero
   unfixable failures, at most 2 doublings ever needed. If escalation was
   needed the status says IRIS_RIDGE_ESCALATED — the result is valid, the
   data was harder than usual (256 duplicates, say).

   The solved instrument is an ordinary iris instrument: same w1/b1/w2/b2
   arrays, same iris_predict, saves and loads as a normal v1/v2 file
   (bit-identical round trip, verified over 441 probes). The solve targets
   logit space — the exact inverse of our sigmoid — so the shipping sigmoid
   forward pass lands on the normalized targets. Stated honestly: that makes
   it a bounded-output VARIANT of the backprop head, not an equivalent.

   THE 4.6e-2 FIGURE, SCOPED. It measures logit-space-sigmoid ELM against
   linear-head ELM — an internal ablation between two ELM variants, largest
   near the demos (ADR 0008:51-52). It is NOT the ELM-vs-backprop gap, which
   is not bounded pointwise anywhere in this repo. Do not cite it as one.

   REROLL is the reason to love it: a new seed literally IS a new frozen
   random layer, undiluted by any training — measurably steadier at the
   demos (0.016 vs backprop's 0.017) and ~45% livelier in the gaps (0.11 vs
   0.076) at nh=12. The purest form of "same examples, different instrument"
   this project has. The lively corner lives at nh >= 8: four frozen random
   features cannot recall five demos (measured — the near-band fails at
   every lambda), so ELM refuses nh < 8 outright rather than shipping a
   config that breaks the reroll promise.

   Defaults that pass every band: lam0 = 1e-4 at nh=12 (equal fit to
   backprop, 425x). nh=48 with lam0 = 1e-3 fits BETTER than backprop on
   both recall and generalisation (296x, ~0.9 ms S3 at 50 examples).

   Determinism: the hidden layer is redrawn from k->seed by a LOCAL rng
   (k->rng is never touched — a closed-form solve is not an event in the
   correction history), accumulation order is fixed by example order, and
   the escalation schedule is fixed. Same seed + same examples =>
   bit-identical weights, verified at nh 12/24/48.
   ========================================================================== */

#define IRIS_ELM_SCRATCH(NH, NO)                                                 \
  ( sizeof(float) * ( (size_t)((NH)+1) * ((NH)+1)      /* A: normal matrix */  \
                    + (size_t)((NH)+1) * (NO)          /* B: rhs           */  \
                    + (size_t)((NH)+1) ) )             /* pristine diagonal */

/* arena + solve scratch in one block, for callers who want one number */
#define IRIS_ARENA_ELM(NI, NH, NO, NEX)                                          \
  ( IRIS_ARENA(NI, NH, NO, NEX) + IRIS_ELM_SCRATCH(NH, NO) )

/* Exact inverse of iris_tanh — the Pade approximant, not the true tanh — via
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
                           void *scratch, size_t scratch_bytes) {
  if (!k || !scratch || k->n_ex == 0) return -1;
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
    /* Unreachable by construction (SPD after enough ridge; measured zero
       failures across the whole campaign). If it ever fires, the Gram
       accumulation overflowed to non-finite — recover to a finite,
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
  return doublings;
}

/* The instant trainer with the measured-default gains: 2/sqrt(n_in) for
   weights AND biases. lam0 = 1e-4 is the nh=12 default; 1e-3 at nh=48. */
IRIS_API int iris_train_elm(iris *k, float lam0, void *scratch, size_t scratch_bytes) {
  const float g = 2.0f / iris_sqrt((float)(k->n_in > 0 ? k->n_in : 1));
  return iris_train_elm_ex(k, lam0, g, g, scratch, scratch_bytes);
}

/* The reroll button, ELM flavour: a new seed IS a new frozen random layer,
   refit exactly. This is the deliberate new-instrument gesture, so it also
   resets the rng stream, exactly as iris_retrain_new does. */
IRIS_API int iris_retrain_elm_new(iris *k, uint32_t seed, float lam0,
                              void *scratch, size_t scratch_bytes) {
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

   FORMAT v2 is format v1 plus ONE uint32 — the live rng state — inserted
   right after the 8-word header. Why: a warm correction (iris_correct)
   advances rng.s past the seed, and v1's loader resets rng.s = seed, so a
   corrected instrument saved as v1 and reloaded would take a DIFFERENT
   shuffle path on its NEXT correction than the in-memory instrument —
   same weights, quietly diverging futures. v2 persists the stream, so
   save/load is transparent to the correction chain.

   FORMAT v3 changes NO BYTES AT ALL. Same header, same nine words, same
   payload, same length. What it changes is the MEANING of the weights: a v3
   file was trained with inputs scaled to [-1,+1], a v1 or v2 file with
   inputs scaled to [0,1]. The version number is the only thing that can tell
   them apart, so the version number is what selects the scaling — not a
   build flag, not a global, not the caller. iris_load sets k->in_center from
   h[1] and nothing else ever writes it except iris_init (which starts every
   fresh instrument at v3) and iris__set_legacy_norm (which the audit uses to
   hold the pre-v3 training path against its frozen hash).

   WHAT BREAKS IF SOMEONE DELETES THE v1/v2 PATH. Not a load failure — that
   would be survivable, because it is loud. What breaks is silent: a v1 or v2
   file would still load, every field would arrive intact, the instrument
   would report itself trained and would play — and every prediction would be
   wrong, because weights fitted against inputs in [0,1] would be fed inputs
   in [-1,+1]. Every first-layer pre-activation shifts by -sum_i w1[h][i],
   which for a trained net is an arbitrary, per-hidden-unit constant: the
   mapping is not degraded, it is a different mapping. The musician would
   load the instrument they practised for a year, hear something else, and
   have nothing to point at. That is precisely the failure Fiebrink & Sonami
   (NIME 2020) describe and precisely what ADR 0006 promised would never
   happen here. Audit check 11 loads a frozen v1 file and compares the
   prediction BITS; deleting the branch fails it in one run.

   THE v1 LOADER IS PERMANENT. Old files load byte-identically to the old
   behaviour, forever. Fiebrink & Sonami's users lost technique to
   retraining; a frozen old model is sacred and this loader is the vow.
   ========================================================================== */

#define IRIS_MAGIC 0x4B455745u  /* "EWEK" */
#define IRIS_FORMAT 3u          /* what iris_save writes: inputs in [-1,+1]      */
#define IRIS_FORMAT_V2 2u       /* v1 + the rng word; inputs in [0,1]. Forever. */
#define IRIS_FORMAT_V1 1u       /* the original; inputs in [0,1]. Forever.      */

IRIS_API size_t iris_save_size(const iris *k) {
  return sizeof(uint32_t) * 9                      /* v2/v3: 8 header + rng.s */
       + sizeof(float) * (size_t)( k->n_hid*k->n_in + k->n_hid
                                 + k->n_out*k->n_hid + k->n_out
                                 + 2*(k->n_in + k->n_out)
                                 + (size_t)k->n_ex * (k->n_in + k->n_out) )
       + sizeof(int32_t) * (size_t)k->n_ex;
}

IRIS_API size_t iris_save(const iris *k, void *buf, size_t cap) {
  size_t need = iris_save_size(k);
  if (!buf || cap < need) return 0;
  uint32_t *h = (uint32_t *)buf;
  /* THE VERSION WORD DESCRIBES THE SCALING THE WEIGHTS WERE FITTED UNDER.
     It is not a build stamp and it is not "the newest format we know" — it
     is the only thing in the file that can tell a reader whether these
     weights expect inputs in [0,1] or in [-1,+1], and iris_load believes it
     absolutely. So it is written from k->in_center, never from a constant.

     Stamping IRIS_FORMAT unconditionally is the bug this comment exists to
     prevent, and it was a real one: an instrument restored from a v1 file
     keeps the legacy scaling (it must — its weights mean nothing else), and
     if the musician then re-trains and saves, a constant here would label
     [0,1] weights as v3. The next load would read that label, switch to
     [-1,+1], and play a DIFFERENT INSTRUMENT out of a file that round-trips
     every weight bit perfectly. Measured before the fix: 0.427 of full scale
     on a 441-probe grid, from the ordinary open / train / save the app does
     in core/store.c. Loud failures are survivable; this one was silent.

     v2 and v3 have identical layouts, so this costs nothing but the truth. */
  h[0] = IRIS_MAGIC;
  h[1] = k->in_center ? IRIS_FORMAT : IRIS_FORMAT_V2;
  h[2] = (uint32_t)k->n_in; h[3] = (uint32_t)k->n_hid;
  h[4] = (uint32_t)k->n_out; h[5] = (uint32_t)k->n_ex;
  h[6] = k->seed; h[7] = (uint32_t)k->next_id;
  h[8] = k->rng.s;                                  /* the one v2 word */
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
  return need;
}

IRIS_API int iris_load(iris *k, const void *buf, size_t bytes) {
  if (!buf || bytes < sizeof(uint32_t) * 8) return 0;
  const uint32_t *h = (const uint32_t *)buf;
  if (h[0] != IRIS_MAGIC) return 0;
  if (h[1] != IRIS_FORMAT_V1 && h[1] != IRIS_FORMAT_V2 && h[1] != IRIS_FORMAT) return 0;
  {
    const int has_rng = (h[1] != IRIS_FORMAT_V1);
    if (has_rng && bytes < sizeof(uint32_t) * 9) return 0;
  }
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
    const size_t hdr  = sizeof(uint32_t) * (h[1] == IRIS_FORMAT_V1 ? 8 : 9);
    const size_t nex  = (size_t)h[5];
    const size_t body = sizeof(float) * ( (size_t)k->n_hid*k->n_in + k->n_hid
                                        + (size_t)k->n_out*k->n_hid + k->n_out
                                        + 2u*((size_t)k->n_in + k->n_out)
                                        + nex * ((size_t)k->n_in + k->n_out) )
                      + sizeof(int32_t) * nex;
    if (bytes < hdr + body) return 0;
  }
  k->n_ex = (int32_t)h[5]; k->seed = h[6]; k->next_id = (int32_t)h[7];
  if (h[1] != IRIS_FORMAT_V1) k->rng.s = h[8] ? h[8] : (k->seed ? k->seed : 1u);
  else                      k->rng.s = k->seed ? k->seed : 1u;  /* v1: old behaviour, untouched */
  /* THE INPUT SCALING TRAVELS WITH THE FILE. v1 and v2 were written by builds
     that scaled inputs to [0,1]; their weights only mean anything against
     that scaling, so that is the scaling they get, for ever. v3 onwards is
     [-1,+1]. Both branches are live in every build — see the header comment
     for what silently breaks if one is removed. */
  k->in_center = (h[1] == IRIS_FORMAT_V1 || h[1] == IRIS_FORMAT_V2) ? 0 : 1;
  const float *f = (const float *)(h + (h[1] == IRIS_FORMAT_V1 ? 8 : 9));
  #define IRIS_GET(dst, n) do { for (int _i = 0; _i < (n); ++_i) (dst)[_i] = *f++; } while (0)
  IRIS_GET(k->w1, k->n_hid*k->n_in);  IRIS_GET(k->b1, k->n_hid);
  IRIS_GET(k->w2, k->n_out*k->n_hid); IRIS_GET(k->b2, k->n_out);
  IRIS_GET(k->in_lo, k->n_in);   IRIS_GET(k->in_hi, k->n_in);
  IRIS_GET(k->out_lo, k->n_out); IRIS_GET(k->out_hi, k->n_out);
  IRIS_GET(k->ex, (int)((size_t)k->n_ex * (k->n_in + k->n_out)));
  #undef IRIS_GET
  const int32_t *ids = (const int32_t *)f;
  for (int i = 0; i < k->n_ex; ++i) k->ex_id[i] = ids[i];
  k->trained = 1;
  k->fitted  = 1;             /* the weights in the file came from a real fit */
  k->status = IRIS_STATUS_OK;   /* a freshly loaded instrument carries no stale error */
  /* The residual ledger belongs to a training run, not to a file: a loaded
     instrument has not been trained in this process, so it has no opinion
     about which demonstration is fighting the others until it is. */
  for (int i = 0; i < k->cap; ++i) k->ex_res[i] = 0.0f;
  k->res_epochs = 0;
  k->tr_done = 0; k->tr_ceiling = 0; k->tr_running = 0; k->tr_ref = 0.0f;
  return 1;
}

/* THE ONLY WAY AN OLD INSTRUMENT EVER CHANGES SCALING, AND THE CALLER HAS TO
   ASK FOR IT BY NAME.

   A v1/v2 instrument keeps the legacy [0,1] scaling for as long as it exists,
   including across re-training and re-saving, because its weights mean
   nothing else and iris_save now labels it honestly as v2. That is the safe
   default and it is the right one. But it leaves an old instrument stuck on
   the worse-conditioned fit for ever, and silently declining to improve is
   its own kind of dishonesty.

   So: this. It throws the old weights away and re-fits the SAME stored
   demonstrations under the centred scaling — the musician's recorded
   gestures are raw sensor values in their own units and mean exactly the
   same thing under either scaling, which is why the re-fit is legitimate.
   What comes out is a v3 instrument that saves as v3.

   ⚠️ NO PRODUCTION CALLER. This comment used to claim "the schema-migration
   path in the app already relies on it." It does not: firmware/app/core/
   schema.c does iris_clear -> replay -> iris_retrain_elm_new, and the token
   in_center does not appear anywhere under firmware/app. The only callers are
   tests/audit.c. This is 21 lines of documentation for 8 lines of code that
   nothing ships. Keep it — a v1/v2 file in the wild will need it — but do not
   cite an app dependency that does not exist.

   It is a NEW FIT, not a conversion: predictions move by about the fit error
   (measured 0.0447 worst-case on the audit's reference instrument, check 30).
   The musician must be told that and must choose it — hence a function they
   call, not something iris_load does behind their back. Fiebrink & Sonami's
   users lost technique to retraining they did not ask for.

   Returns 1 if the instrument was migrated, 0 if there was nothing to do
   (already centred, or no demonstrations to re-fit from). */
IRIS_API int iris_migrate_scaling(iris *k) {
  if (k->in_center) return 0;          /* already on the new scaling */
  if (k->n_ex <= 0) return 0;          /* nothing to re-fit from */
  k->in_center = 1;
  iris_reseed(k, k->seed);               /* a fit from a defined start */
  iris_train_converge(k, 0, 0, 0);
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

   Every v1 file ever saved gains this mode with zero migration: the
   examples and ranges are already in the file. That was the point of the
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
IRIS_API void iris_knn_predict(const iris *k, const float *in, float *out, int kk) {
  const int NIn = k->n_in, NOut = k->n_out;
  if (k->n_ex == 0) { for (int o = 0; o < NOut; ++o) out[o] = 0.0f; return; }
  if (kk < 1) kk = 1;
  if (kk > IRIS_KNN_MAXK) kk = IRIS_KNN_MAXK;
  if (kk > k->n_ex) kk = k->n_ex;

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
IRIS_API int iris_classify_1nn(const iris *k, const float *in, float *out) {
  const int NIn = k->n_in, NOut = k->n_out;
  if (k->n_ex == 0) return -1;
  float inv[IRIS_MAX_IN];
  for (int i = 0; i < NIn; ++i) inv[i] = 1.0f / (k->in_hi[i] - k->in_lo[i]);
  const int stride = NIn + NOut;
  int best = 0; float best_d = 1e30f;
  for (int r = 0; r < k->n_ex; ++r) {
    const float *row = k->ex + (size_t)r * stride;
    float d = 0.0f;
    for (int i = 0; i < NIn; ++i) {
      float t = (row[i] - in[i]) * inv[i];
      d += t * t;
    }
    if (d < best_d) { best_d = d; best = r; }
  }
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

#endif /* EMBWEK_H */
