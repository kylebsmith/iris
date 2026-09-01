# `iris` — API and Architecture Design

**Status:** design frozen for v0.1. All numerics below were compiled and run (gcc 13, `-O2`, float32 throughout); measured numbers are marked **[measured]** and come from `tests/num_probe.c`, which becomes a CI test.

## Two findings that shaped everything

**1. The bandwidth knob has a hard numerical ceiling in float32, and it's lower than you'd guess.** Sweeping σ as a multiple of median nearest-neighbour distance, K=100 centres, N=8, M=4, jitter 1e-6 **[measured]**:

| σ (× median-NN) | max‖w‖ | max &#124;f(xᵢ)−yᵢ&#124; (should be ~0) |
|---|---|---|
| 0.5 | 1 | 1.8e-06 |
| 1.0 | 12 | 1.4e-05 |
| 1.5 | 102 | 1.0e-04 |
| 2.0 | 530 | 4.7e-04 |
| 3.0 | 5 809 | 5.4e-03 |
| 4.0 | 31 479 | 2.8e-02 |
| 6.0 | 256 713 | 1.8e-01 |

Past ~1.6× the model stops interpolating its own demonstrations — the flagship property is gone — and weights of 10⁵ against kernel values of 10⁻⁵ is exactly how you get a NaN into an audio buffer. I tested Kahan-compensated summation in the Cholesky inner loop: **it changes nothing** (1.38e-05 → 1.09e-05 at σ=1.0; 5.38e-03 → 5.27e-03 at σ=3.0) **[measured]**. The loss is genuine conditioning, not accumulation, so don't pay for compensated summation. **Clamp the Gaussian knob to σ ∈ [0.35, 1.6] × median-NN.**

**2. Wendland C2 is strictly better on this hardware and should be shipped alongside the Gaussian.** φ(r) = (1−q)⁴(4q+1), q = r/σ, zero for q ≥ 1 **[measured]**:

| σ (× median-NN) | max‖w‖ | max &#124;f(xᵢ)−yᵢ&#124; | K fill |
|---|---|---|---|
| 0.5 | 1 | 9.5e-07 | 2 % |
| 1.5 | 1 | 1.2e-06 | 11 % |
| 3.0 | 4 | 4.3e-06 | 98 % |
| 6.0 | 30 | 2.9e-05 | 100 % |

It holds interpolation to 1e-5 across the *entire* knob range, weights stay O(10), it is sparse at narrow bandwidths, and it contains no `exp` at all — one `sqrtf` and six multiplies. Its honest downside: compact support means the output decays to zero outside the demonstrations instead of extrapolating, which is a different musical character (silence rather than wildness). Ship Gaussian as default for the "wild" feel, Wendland for wide, safe, well-behaved mappings.

---

## 1. The public API

```c
/* ============================================================================
 * iris.h — interactive ML for gestural instruments on microcontrollers
 * SPDX-License-Identifier: BSD-3-Clause
 * C99, header-only, zero dependencies, no dynamic allocation in the core.
 * ==========================================================================*/
#ifndef EMBWEK_H
#define EMBWEK_H

#include <stdint.h>
#include <stddef.h>

#define IRIS_VERSION_MAJOR 0
#define IRIS_VERSION_MINOR 1
#define IRIS_FORMAT_VERSION 1        /* on-disk format; bump only on break */

#define IRIS_MAX_INPUTS   64
#define IRIS_MAX_OUTPUTS  32

/* ---------------------------------------------------------------- status */
typedef enum {
    IRIS_TRAINING       =  1,  /* not an error: iris_train_step() wants more calls */
    IRIS_OK             =  0,
    IRIS_ERR_ARG        = -1,  /* NULL, out-of-range dimension, bad enum        */
    IRIS_ERR_ARENA      = -2,  /* arena smaller than iris_arena_size()            */
    IRIS_ERR_FULL       = -3,  /* example store at max_examples                 */
    IRIS_ERR_EMPTY      = -4,  /* nothing to delete / no examples to train on   */
    IRIS_ERR_UNTRAINED  = -5,  /* infer() before any successful train()         */
    IRIS_ERR_NOTFOUND   = -6,  /* unknown example id or take id                 */
    IRIS_ERR_SOLVE      = -7,  /* kernel/normal matrix not positive definite    */
    IRIS_ERR_IO         = -8,  /* read/write callback returned short or failed  */
    IRIS_ERR_FORMAT     = -9,  /* bad magic, bad CRC, truncated chunk           */
    IRIS_ERR_VERSION    = -10, /* file written by a newer, incompatible library */
    IRIS_ERR_BUSY       = -11, /* async train in flight, or RT audio is active  */
    IRIS_ERR_UNSUPPORTED= -12  /* model/kernel not compiled into this build     */
} iris_status_t;

const char *iris_status_str(iris_status_t s);   /* never NULL; static storage */

/* ---------------------------------------------------------------- models */
typedef enum {
    IRIS_MODEL_RBF   = 0,   /* exact interpolation. THE default.               */
    IRIS_MODEL_RIDGE = 1,   /* polynomial ridge regression, order 1 or 2        */
    IRIS_MODEL_KNN   = 2,   /* classification; outputs are one-hot class scores */
    IRIS_MODEL_MLP   = 3,   /* v0.3                                            */
    IRIS_MODEL_DTW   = 4    /* v0.3                                            */
} iris_model_t;

typedef enum {
    IRIS_KERNEL_GAUSSIAN = 0,  /* exp(-r^2/2s^2). Knob clamped to [0.35,1.6]x. */
    IRIS_KERNEL_WENDLAND = 1   /* (1-q)^4(4q+1), compact support, conditions
                                well across the whole knob range.            */
} iris_kernel_t;

/* ---------------------------------------------------------------- config */
typedef struct {
    uint16_t n_inputs;      /* 1..IRIS_MAX_INPUTS                              */
    uint16_t n_outputs;     /* 1..IRIS_MAX_OUTPUTS (classes, for KNN)          */
    uint16_t max_examples;  /* hard ceiling; arena is sized for exactly this */

    uint8_t  model;         /* iris_model_t                                    */
    uint8_t  kernel;        /* iris_kernel_t; RBF only                         */
    uint8_t  poly_order;    /* RIDGE only: 1 or 2. Order >2 is refused --
                               see the QR note in the numerics section.      */
    uint8_t  knn_k;         /* KNN only; default 3                           */
    uint16_t mlp_hidden;    /* MLP only                                      */

    uint8_t  n_banks;       /* 1 = no undo/A-B. 2 = default. Never >2.        */
    uint8_t  flags;         /* IRIS_F_*                                        */
    float    thin_frac;     /* auto-thinning during a take, as a fraction of
                               the observed per-dim input range. 0 disables.
                               DEFAULT 0.05 -- see iris_record().              */
    float    bandwidth;     /* initial knob, 0..1 -> log-spaced sigma        */
} iris_config_t;

#define IRIS_F_CLAMP_OUTPUT  (1u<<0)  /* clamp infer() to observed output range
                                       +/-20%. DEFAULT ON. Turning this off
                                       is how you send a filter cutoff to
                                       -400 Hz and blow a tweeter.           */
#define IRIS_F_NO_NORMALIZE  (1u<<1)  /* skip input standardisation (expert)    */

/* Sensible defaults for everything; override fields after calling. */
iris_config_t iris_config_default(uint16_t n_in, uint16_t n_out, uint16_t max_ex);

/* ---------------------------------------------------------------- handle */
/* Opaque. Lives *inside* the caller's arena; do not copy or move it.        */
typedef struct iris_s iris_t;

/* Bytes required for this exact config. Deterministic, no allocation.       */
size_t iris_arena_size(const iris_config_t *cfg);

/* Bytes required if this device will only ever LOAD and INFER, never train.
 * Omits the solver scratch (the packed Cholesky factor), which is ~57% of a
 * K=100 RBF arena. Use this for the "performance build" of an instrument.   */
size_t iris_arena_size_infer_only(const iris_config_t *cfg);

/* Place a handle in `arena`. Returns NULL if bytes < iris_arena_size(cfg) or
 * the config is invalid. `arena` must be 8-byte aligned and outlive `iris_t`. */
iris_t *iris_init(const iris_config_t *cfg, void *arena, size_t bytes);

/* Compile-time arena sizing, for `static uint8_t arena[...]` in a sketch.
 * CI asserts IRIS_ARENA_RBF(n,m,k) >= iris_arena_size(equivalent config).       */
#define IRIS_ALIGN8(x)  (((size_t)(x) + 7u) & ~(size_t)7u)
#define IRIS_ARENA_RBF(N,M,K) ( (size_t)256u                                    \
    + IRIS_ALIGN8((size_t)(K) * ((N)+(M)) * 4u)   /* example store           */ \
    + IRIS_ALIGN8((size_t)(K) * 6u)               /* example ids + take ids  */ \
    + IRIS_ALIGN8((size_t)(N) * 8u)               /* running input range     */ \
    + 2u * IRIS_ALIGN8((size_t)(K)*((N)+(M))*4u                                 \
                     + (size_t)((N)+(M))*8u + 32u)   /* two banks          */ \
    + IRIS_ALIGN8((size_t)(K)*((K)+1u)/2u * 4u)   /* packed Cholesky factor  */ \
    + IRIS_ALIGN8((size_t)(K) * 8u) )             /* rhs + nn-dist scratch   */

/* ============================ INPUTS & INFERENCE ==========================
 * Everything in this block is real-time safe: bounded time, no allocation,
 * no locks, no printf, no flash access. Callable from a high-priority task.
 * (NOT from an ISR -- see the ESP32 section: float in an ISR is forbidden.)
 * ==========================================================================*/

/* Publish the current sensor vector. n_inputs floats, any finite values.
 * If called from a different core than iris_infer(), this is a seqlock write:
 * iris_infer() will never observe a torn vector.                             */
iris_status_t iris_set_inputs(iris_t *h, const float *in);

/* Run the live bank on the last iris_set_inputs() vector. Writes n_outputs.  */
iris_status_t iris_infer(iris_t *h, float *out);

/* Stateless convenience; equivalent to set_inputs + infer.                 */
iris_status_t iris_infer_from(iris_t *h, const float *in, float *out);

/* 0..1. Kernel similarity to the nearest demonstration in normalised space.
 * ~1 = you are sitting on a demo. Near 0 = you are extrapolating and the
 * output is fiction. Map this to an LED and students debug their own
 * training sets without a screen.                                          */
float iris_confidence(const iris_t *h);

/* Which demonstration am I nearest? Returns example id, or <0 if untrained.
 * Optionally writes the normalised distance. Real-time safe.               */
int32_t iris_nearest(const iris_t *h, float *dist_out);

/* ============================== EXAMPLE STORE =============================
 * Example ids are monotonic and NEVER reused, even after deletion. On a
 * screenless device the example *count* shifts under the user constantly;
 * a stable id is the only thing you can safely say "delete 7" about.
 * ==========================================================================*/

/* Record the current input vector against `out`. Returns the new example id
 * (>=0), or a negative iris_status_t.
 *
 * Returns IRIS_OK (==0, a valid id only for the very first example -- check
 * for <0 to detect errors) ... no: returns >=0 id on success.
 *
 * THINNING: if a take is open and cfg.thin_frac > 0, the example is silently
 * DROPPED (returning the previous id) when the input vector is closer than
 * thin_frac * (running per-dim range) to the last point recorded in this
 * take. This is not an optimisation -- it is load-bearing. A musician holding
 * a button for three seconds at 200 Hz produces 600 near-duplicate points,
 * which makes the kernel matrix numerically singular and the model useless.
 * Thinning is why "hold the button and wiggle" works at all.               */
int32_t iris_record(iris_t *h, const float *out);

/* Record an explicit input/output pair, ignoring iris_set_inputs().          */
int32_t iris_record_pair(iris_t *h, const float *in, const float *out);

/* A TAKE is one continuous gesture: one press-and-hold of the teach button.
 * Undo operates on takes, not samples, because a musician thinks "that last
 * wiggle was bad", never "example 412 was bad". This is the single most
 * important ergonomic decision in the API.                                 */
int32_t     iris_take_begin(iris_t *h);              /* returns take id, or <0  */
iris_status_t iris_take_end(iris_t *h);
int32_t     iris_take_current(const iris_t *h);      /* -1 if none open         */

iris_status_t iris_delete_example(iris_t *h, int32_t id);
iris_status_t iris_delete_last_example(iris_t *h);
iris_status_t iris_delete_take(iris_t *h, int32_t take_id);
iris_status_t iris_delete_last_take(iris_t *h);        /* THE undo button         */
iris_status_t iris_clear(iris_t *h);                   /* all examples, keep model*/

uint16_t iris_count(const iris_t *h);
uint16_t iris_take_count(const iris_t *h);

/* Walk the store. `index` is 0..iris_count()-1 and is NOT stable across
 * deletions; convert to an id immediately.                                 */
int32_t     iris_example_id_at(const iris_t *h, uint16_t index);
iris_status_t iris_example_get(const iris_t *h, int32_t id,
                           float *in_out, float *out_out, int32_t *take_out);

/* Load a stored example's INPUT vector as if the performer had just made
 * that gesture, so the next iris_infer() renders it. This is how you audit a
 * training set with no screen: scroll an encoder through the examples and
 * listen to each one. Pairs with iris_delete_example() to fix bad demos by
 * ear alone.                                                               */
iris_status_t iris_audition_example(iris_t *h, int32_t id);

/* ================================ TRAINING ===============================
 * Training NEVER touches the live bank. It writes the idle bank and then
 * atomically publishes it. Audio keeps running throughout.
 * NOT real-time safe. Call from the control task, not the audio task.
 * ==========================================================================*/

iris_status_t iris_train(iris_t *h);                        /* blocking          */

/* Cooperative training for models with iterative solvers (MLP), so you can
 * train from loop() without blocking. Returns IRIS_TRAINING until done, then
 * IRIS_OK. `budget_us` is advisory; the routine overshoots by at most one
 * inner iteration. For RBF/RIDGE/KNN this simply calls iris_train().         */
iris_status_t iris_train_step(iris_t *h, uint32_t budget_us);
iris_status_t iris_train_abort(iris_t *h);

/* --- banks: A/B audition and undo-training are the same mechanism -------- */
/* There are exactly two banks. iris_train() fills the idle one and makes it
 * live. So the previous model is always still sitting in the other bank.
 * "Undo the last training run" and "A/B the two models" are then literally
 * the same call, which is one concept for the user instead of two.         */
int  iris_bank_live(const iris_t *h);                /* 0 or 1                  */
void iris_bank_use(iris_t *h, int bank);             /* RT-safe, atomic         */
void iris_bank_flip(iris_t *h);                      /* undo-train / A-B toggle */
int  iris_bank_ready(const iris_t *h, int bank);     /* has it ever been trained*/

/* ============================ BANDWIDTH (the knob) =======================*/
/* knob 0..1 maps log-linearly onto sigma = mult * median_nn_distance, with
 * mult in [0.35, 1.60] for IRIS_KERNEL_GAUSSIAN and [0.35, 6.0] for WENDLAND.
 * The Gaussian clamp is NOT arbitrary: beyond ~1.6x the float32 kernel
 * matrix is numerically singular and the model stops interpolating its own
 * demonstrations. See the numerics section for the measured table.         */
void  iris_set_bandwidth(iris_t *h, float knob01);   /* RT-safe. Marks dirty.   */
float iris_get_bandwidth(const iris_t *h);
float iris_get_sigma(const iris_t *h);               /* resolved, in normalised units */
int   iris_bandwidth_dirty(const iris_t *h);         /* knob moved since resolve*/

/* Re-solve at the current bandwidth, reusing the existing examples and
 * normalisation. Cheaper than iris_train() only in that it skips stats; the
 * Cholesky still dominates. ~8.5 ms at K=100 on ESP32-S3. NOT RT-safe:
 * call it from the control task in response to iris_bandwidth_dirty(),
 * rate-limited to ~25 Hz. See "the knob" analysis below.                   */
iris_status_t iris_resolve(iris_t *h);

/* ============================== INTROSPECTION ============================
 * Designed for a device with two LEDs and a piezo, not a screen.
 * ==========================================================================*/
typedef struct {
    uint16_t n_examples;
    uint16_t n_takes;
    uint16_t n_dead_inputs;      /* dims whose sd is ~0: unplugged sensor    */
    uint16_t n_contradictions;   /* near-identical inputs, far-apart outputs */
    uint16_t n_duplicates;       /* pairs closer than the thinning threshold */

    float in_min[IRIS_MAX_INPUTS];
    float in_max[IRIS_MAX_INPUTS];
    float in_sd [IRIS_MAX_INPUTS];
    float out_min[IRIS_MAX_OUTPUTS];
    float out_max[IRIS_MAX_OUTPUTS];

    float median_nn;             /* median nearest-neighbour dist, normalised*/
    float min_nn;                /* smallest pairwise distance               */
    float coverage;              /* 0..1 heuristic: how much of the observed
                                    input box the demos actually occupy      */
    float fit_error;             /* max |f(xi)-yi| after the last train.
                                    For RBF this should be ~1e-5. If it is
                                    not, the bandwidth is too wide or the
                                    demos are contradictory.                 */
    uint32_t train_ms;           /* wall time of the last train              */
} iris_stats_t;

void iris_stats(const iris_t *h, iris_stats_t *s);

/* One number you can blink. Ordered by severity; report the first that hits.*/
typedef enum {
    IRIS_HEALTH_OK = 0,
    IRIS_HEALTH_UNTRAINED,        /* blink 1: press train                     */
    IRIS_HEALTH_TOO_FEW,          /* blink 2: fewer than 2 examples/class     */
    IRIS_HEALTH_DEAD_INPUT,       /* blink 3: a sensor never moved            */
    IRIS_HEALTH_DUPLICATES,       /* blink 4: demos on top of each other      */
    IRIS_HEALTH_CONTRADICTORY,    /* blink 5: same gesture, two answers       */
    IRIS_HEALTH_ILL_CONDITIONED   /* blink 6: bandwidth too wide for this set */
} iris_health_t;

iris_health_t iris_health(const iris_t *h, int *detail); /* detail = dim/example id */
const char *iris_health_str(iris_health_t hc);

/* ============================== SAVE / LOAD ==============================
 * Callback-based so the same code writes to a file, an NVS blob, an SD card
 * or a serial port. See the format section.
 * NOT RT-safe, and on ESP32 a flash write stalls BOTH cores for 10-50 ms.
 * Returns IRIS_ERR_BUSY if iris_rt_begin() is in effect.
 * ==========================================================================*/
typedef int (*iris_write_fn)(void *ctx, const void *data, size_t n); /* 0 = ok */
typedef int (*iris_read_fn) (void *ctx, void *data, size_t n);       /* 0 = ok */

#define IRIS_SAVE_MODEL     (1u<<0)   /* weights, centres, normalisation       */
#define IRIS_SAVE_EXAMPLES  (1u<<1)   /* the training set, so you can retrain  */
#define IRIS_SAVE_ALL       (IRIS_SAVE_MODEL | IRIS_SAVE_EXAMPLES)

size_t      iris_save_size(const iris_t *h, uint32_t what);
iris_status_t iris_save(const iris_t *h, iris_write_fn w, void *ctx, uint32_t what);
iris_status_t iris_load(iris_t *h, iris_read_fn r, void *ctx);

/* Peek at a file's header without loading it, to check it fits this arena. */
typedef struct {
    uint16_t format_version, model, model_version;
    uint16_t n_inputs, n_outputs;
    uint32_t n_examples;
    uint32_t total_size;
} iris_file_info_t;
iris_status_t iris_peek(iris_read_fn r, void *ctx, iris_file_info_t *info);

/* Declare that audio is running, so iris_save() refuses instead of dropping a
 * 50 ms hole in the output when the flash controller disables both caches. */
void iris_rt_begin(iris_t *h);
void iris_rt_end(iris_t *h);

/* ============================== PORTING LAYER ============================
 * Six functions. Reference implementations in ports/posix and ports/esp32.
 * The CORE never calls alloc/free/printf; only the optional helpers do.
 * ==========================================================================*/
void    *iris_port_malloc(size_t n);
void     iris_port_free(void *p);
uint32_t iris_port_time_ms(void);
void     iris_port_printf(const char *fmt, ...);
uint32_t iris_port_rand(void);
void     iris_port_yield(void);

/* =============================== C++ FAÇADE ============================== */
#ifdef __cplusplus
class EmbWek {
public:
    bool begin(int nIn, int nOut, int maxEx, void *arena, size_t bytes,
               iris_model_t model = IRIS_MODEL_RBF) {
        iris_config_t c = iris_config_default((uint16_t)nIn, (uint16_t)nOut,
                                          (uint16_t)maxEx);
        c.model = (uint8_t)model;
        h_ = iris_init(&c, arena, bytes);
        return h_ != nullptr;
    }
    void  setInputs(const float *in)     { iris_set_inputs(h_, in); }
    int32_t record(const float *out)     { return iris_record(h_, out); }
    int32_t takeBegin()                  { return iris_take_begin(h_); }
    void  takeEnd()                      { iris_take_end(h_); }
    void  deleteLastTake()               { iris_delete_last_take(h_); }
    void  deleteLast()                   { iris_delete_last_example(h_); }
    void  clear()                        { iris_clear(h_); }
    int   train()                        { return iris_train(h_); }
    void  undoTrain()                    { iris_bank_flip(h_); }
    int   infer(float *out)              { return iris_infer(h_, out); }
    void  bandwidth(float k)             { iris_set_bandwidth(h_, k); }
    void  resolve()                      { iris_resolve(h_); }
    bool  bandwidthDirty()               { return iris_bandwidth_dirty(h_) != 0; }
    float confidence()                   { return iris_confidence(h_); }
    int   count()                        { return iris_count(h_); }
    iris_health_t health(int *d = nullptr) { return iris_health(h_, d); }
    iris_t *raw()                          { return h_; }
private:
    iris_t *h_ = nullptr;
};
#endif /* __cplusplus */
#endif /* EMBWEK_H */
```

### The knob: can bandwidth be updated incrementally? Honest answer: no.

Changing γ rescales every entry of K by a *different* factor (exp(−γd²ᵢⱼ) with distinct dᵢⱼ). The difference K(γ′) − K(γ) is full-rank. There is no rank-k Cholesky update, no Sherman–Morrison, nothing. Anyone who tells you otherwise is thinking of the *other* update — adding a centre at fixed γ, which genuinely is O(K²) (one triangular solve plus a square root to append a row to L, ≈0.1 ms at K=100). That one is worth implementing; it makes "record a demo and hear it immediately" nearly free. It just isn't this one.

So, in priority order:

**1. Just re-solve on core 0. This is the answer for K ≤ 120.** 8.5 ms is 117 Hz. A knob does not produce more than ~30 distinct perceptible values per second. Rate-limit `iris_resolve()` to 25 Hz on the control core, publish by bank swap. Zero extra memory, no approximation, and it uses a core that is otherwise idle. Ship this in v0.2 and stop.

**2. Bandwidth ladder with crossfade, for larger K.** Pre-solve R log-spaced rungs of γ; the knob crossfades between adjacent rungs, f(x) = (1−t)f_a(x) + t·f_b(x). The reason this is legitimate rather than a hack: each rung interpolates the demonstrations exactly, so *any* convex blend of two rungs also passes through every demonstration. I verified it — blending σ=0.6× and σ=1.4× rungs across t ∈ {0, 0.2, …, 1.0}, max |blend(xᵢ) − yᵢ| = **4.4e-05** **[measured]**, against 1e-06 for a pure rung. The knob stays exact everywhere along its travel; only the shape *between* demos is interpolated. Cost: 2× inference and R·K·M floats (R=8, K=100, M=4 → 12.8 KB). v0.3.

**3. Not worth it.** Warm-started CG converges fast only when the matrix is well-conditioned, which is exactly the regime where the direct solve is already cheap. Skip it.

---

## 2. The ten-minute story

**Arduino (ESP32-S3, 25 lines).** Two flex sensors in, two synth parameters out. Hold the TEACH button and wiggle while turning the two knobs to the sound you want; release; it trains. Tap UNDO to throw away the last wiggle.

```cpp
#include <iris.h>
#include "mysynth.h"                        // synthBegin(), synthSet(a,b)

static uint8_t arena[IRIS_ARENA_RBF(2, 2, 64)];   // 2 in, 2 out, 64 demos
EmbWek ml;
bool wasTeaching = false;

void setup() {
  pinMode(0, INPUT_PULLUP);                 // TEACH (hold)
  pinMode(1, INPUT_PULLUP);                 // UNDO  (tap)
  ml.begin(2, 2, 64, arena, sizeof arena);
  synthBegin();
}

void loop() {
  float in[2] = { analogRead(4) / 4095.0f, analogRead(5) / 4095.0f };
  ml.setInputs(in);                         // your gesture sensors

  bool teaching = !digitalRead(0);
  if (teaching && !wasTeaching) ml.takeBegin();
  if (teaching) {
    float want[2] = { analogRead(6) / 4095.0f, analogRead(7) / 4095.0f };
    ml.record(want);                        // knobs = the sound you want here
  }
  if (!teaching && wasTeaching) { ml.takeEnd(); ml.train(); }
  wasTeaching = teaching;

  if (!digitalRead(1)) { ml.deleteLastTake(); ml.train(); delay(250); }

  ml.bandwidth(analogRead(8) / 4095.0f);    // the wildness knob
  if (ml.bandwidthDirty()) ml.resolve();

  float out[2];
  if (ml.infer(out) == IRIS_OK) synthSet(out[0], out[1]);
}
```

Nothing in there requires understanding arenas, training sets, or kernels. The `IRIS_ARENA_RBF` macro exists precisely so line 4 can be a `static` array — a runtime `iris_arena_size()` cannot size a static buffer, and telling a musician to `malloc` is a non-starter.

**ESP-IDF equivalent**, with the two-task split that the Arduino version elides:

```c
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "iris.h"
#include "mysynth.h"

/* Hot buffers must live in internal SRAM, never PSRAM. */
static EXT_RAM_BSS_ATTR_NEVER uint8_t s_arena[IRIS_ARENA_RBF(2,2,64)]
    __attribute__((aligned(8)));
static iris_t *s_ml;
static volatile float s_params[2] = {0.f, 0.f};

/* ---- core 1: audio. Pinned because it touches float. ------------------- */
static void audio_task(void *arg) {
    iris_rt_begin(s_ml);                     /* makes iris_save() refuse       */
    for (;;) {
        float a = s_params[0], b = s_params[1];   /* smoothed downstream   */
        synth_render_block(a, b);          /* blocks on i2s_channel_write  */
    }
}

/* ---- core 0: sensors, recording, training. Also pinned (float). -------- */
static void control_task(void *arg) {
    bool was = false;
    uint32_t last_resolve = 0;
    for (;;) {
        float in[2];
        sensors_read(in);
        iris_set_inputs(s_ml, in);

        bool teaching = button_held(BTN_TEACH);
        if (teaching && !was) iris_take_begin(s_ml);
        if (teaching) { float want[2]; knobs_read(want); iris_record(s_ml, want); }
        if (!teaching && was) { iris_take_end(s_ml); iris_train(s_ml); }
        was = teaching;

        if (button_tapped(BTN_UNDO))  { iris_delete_last_take(s_ml); iris_train(s_ml); }
        if (button_tapped(BTN_AB))    { iris_bank_flip(s_ml); }

        iris_set_bandwidth(s_ml, knob_read(KNOB_WILD));
        uint32_t now = iris_port_time_ms();
        if (iris_bandwidth_dirty(s_ml) && now - last_resolve > 40) {
            iris_resolve(s_ml);              /* ~8.5 ms; audio unaffected    */
            last_resolve = now;
        }

        float out[2];
        if (iris_infer(s_ml, out) == IRIS_OK) { s_params[0] = out[0]; s_params[1] = out[1]; }
        led_blink_code(iris_health(s_ml, NULL));

        vTaskDelay(pdMS_TO_TICKS(1));      /* ~1 kHz control rate          */
    }
}

void app_main(void) {
    iris_config_t cfg = iris_config_default(2, 2, 64);
    s_ml = iris_init(&cfg, s_arena, sizeof s_arena);
    configASSERT(s_ml);
    model_load_from_nvs(s_ml);             /* before audio starts          */

    xTaskCreatePinnedToCore(audio_task,   "audio",   4096, NULL, 20, NULL, 1);
    xTaskCreatePinnedToCore(control_task, "control", 8192, NULL,  5, NULL, 0);
}
```

---

## 3. Memory layout and sizing

The arena is one contiguous block, carved once in `iris_init()` and never re-carved. Layout, in order, 8-byte aligned:

```
+--------------------------------------------------------------+
| iris_t header                                       256 B fixed |
+--------------------------------------------------------------+
| example store   X[K][N] then Y[K][M], float32   K*(N+M)*4     |
| example ids     uint32 id[K]                    K*4           |
| take ids        uint16 take[K]                  K*2           |
| running input range  min[N], max[N]             N*8           |
+--------------------------------------------------------------+
| BANK 0 : centres C[K][N]                        K*N*4         |
|          weights W[K][M]                        K*M*4         |
|          norm mean/scale for in+out             (N+M)*8       |
|          gamma, sigma, kernel, n_centres, fit   32 B          |
| BANK 1 : identical                                            |
+--------------------------------------------------------------+
| SOLVER SCRATCH (training only)                                |
|   packed lower-triangular L                     K(K+1)/2 * 4  |
|   rhs column + nn-distance temp                 K*8           |
+--------------------------------------------------------------+
```

Each bank carries its **own copy of the centres**. That costs K·N·4 bytes per bank and it is the single most important real-time decision in the library: inference then reads *only* bank memory and never the mutable example store. Recording a new example while the audio task is inferring cannot race, because they touch disjoint memory. Without this you need a lock on the hot path, and locks in an audio callback are how instruments click.

**Worked example — N=8, M=4, K=100, RBF:**

```
header                                          256
example store  100*12*4                       4,800
ids + take ids 100*6                            600
running range  8*8                               64
banks          2 * (3200 + 1600 + 96 + 32)    9,856
L (packed)     100*101/2 * 4                 20,200
rhs + tmp      100*8                            800
                                            -------
                                             36,576 B  = 35.7 KB
```

The Cholesky factor is 55 % of that and is needed **only while training**. `iris_arena_size_infer_only()` drops it and the scratch: **15.6 KB** for a load-and-perform build. That is the difference between one model and two on a board that also runs a synth.

**Practical ceilings on ESP32-S3's 512 KB internal SRAM.** Assume ~320 KB usable with Wi-Fi/BT disabled, ~200 KB with Wi-Fi up. L dominates at 2K² bytes. Solve time scales K³; anchoring to your measured 8.5 ms at K=100:

| K | L | full arena (N=8,M=4) | est. solve | verdict |
|---|---|---|---|---|
| 50 | 5.0 KB | 12 KB | ~1.5 ms | trivial |
| 100 | 20 KB | 36 KB | 8.5 ms **[given]** | **live knob, 25 Hz re-solve** |
| 150 | 45 KB | 68 KB | ~26 ms | live knob at 10 Hz, sluggish |
| 200 | 80 KB | 112 KB | ~60 ms | retrain on button press only |
| 300 | 180 KB | 240 KB | ~195 ms | fits, but only just, and feels dead |
| 400 | 320 KB | 415 KB | ~450 ms | does not coexist with a synth |

**The ceiling is time, not memory.** Memory would let you reach K≈350; K³ means the knob stops being a knob past ~120. Default `max_examples` to 64 and document 100 as the live-knob limit. If a student needs 300 demos, that is a signal to switch to `IRIS_KERNEL_WENDLAND` (sparse below σ≈2×, **[measured]** 2–39 % fill) or to `IRIS_MODEL_RIDGE`.

Note also that kernel *construction* is not free: K²/2 entries × (N multiply-subtract-adds + ~15 flops for the fast exp) ≈ 195 kflops at K=100, which is the same order as the Cholesky's 167 k multiply-adds. Only past K≈200 does the cubic term truly dominate.

**Inference cost per frame**, K=100, N=8, M=4: 100 kernel evaluations × (24 + ~15 flops) + 400 multiply-adds ≈ 4.3 kflops, roughly **25–40 µs**. Fine at a 1 kHz control rate. Not fine per-sample at 48 kHz (20 µs budget). **Run inference at control rate and smooth to audio rate** — one-pole per parameter in the audio task. Say this to students on day one; it is the mistake every cohort makes.

**PSRAM.** At 33–58 MB/s against 1830 MB/s internal, PSRAM is ~40× slower and the S3's cache will thrash on a Cholesky's strided access. Rules: `L`, the banks, and the input vector are internal SRAM, always. The example store *may* go to PSRAM (it is touched once per training run, sequentially) if a student wants K=1000 for offline experiments. The save-staging buffer may go to PSRAM. Nothing else. Provide `IRIS_INTERNAL_ATTR` / `IRIS_SLOW_ATTR` macros in the ESP32 port and use them in the reference app so the pattern is copyable.

---

## 4. The numerics

All of the following is the actual shipping code, compiled and tested.

### 4.1 Fast `exp(-t)` for the Gaussian kernel

Range-reduce with **round-to-nearest** rather than floor, so the polynomial argument lands in [−0.5, 0.5] instead of [−1, 0]. Same instruction count, and the Taylor truncation error drops by 2⁶. Measured worst relative error over t ∈ [0, 20): **3.75e-06** **[measured]** — versus 2.80e-04 for the floor variant, which is the version most people write.

```c
/* exp(-t) for t >= 0. Max rel. error 3.8e-6 over [0,20). ~15 flops, no LUT. */
static inline float iris_expneg(float t)
{
    if (t >= 30.0f) return 0.0f;                 /* exp(-30) = 9.4e-14 */
    float u = t * 1.44269504088896341f;          /* t / ln 2 */
    float n = floorf(u + 0.5f);                  /* nearest integer */
    float g = n - u;                             /* g in [-0.5, 0.5]; want 2^g */
    float p = 1.0f + g*(0.69314718f + g*(0.24022651f + g*(0.05550411f +
                     g*(0.00961812f + g*0.00133335f))));
    int32_t ni = (int32_t)n;
    if (ni > 126) return 0.0f;
    union { float f; uint32_t u; } s;
    s.u = (uint32_t)(127 - ni) << 23;            /* 2^-n by exponent field */
    return p * s.f;
}
```

### 4.2 Kernels

```c
#define IRIS_LT(i,j)  ( ((size_t)(i)*((size_t)(i)+1u))/2u + (size_t)(j) )  /* i>=j */
#define IRIS_LTN(n)   ( ((size_t)(n)*((size_t)(n)+1u))/2u )

static inline float iris_sqdist(const float *a, const float *b, int n)
{
    float s = 0.0f;
    for (int i = 0; i < n; ++i) { float d = a[i] - b[i]; s += d * d; }
    return s;
}

/* Wendland C2: phi(r) = (1-q)^4 (4q+1), q = r/sigma, zero for q >= 1.
 * Compactly supported => sparse, well conditioned, and no exp at all.     */
static inline float iris_wendland(float r2, float inv_s2)
{
    float q2 = r2 * inv_s2;
    if (q2 >= 1.0f) return 0.0f;
    float q = sqrtf(q2), a = 1.0f - q, a2 = a * a;
    return a2 * a2 * (4.0f * q + 1.0f);
}

static inline float iris_kern(const iris_bank_t *b, float r2)
{
    return (b->kernel == IRIS_KERNEL_WENDLAND) ? iris_wendland(r2, b->inv_s2)
                                             : iris_expneg(b->gamma * r2);
}
```

### 4.3 Bandwidth auto-scaling

A single absolute σ is meaningless across a flex sensor, an IMU and a photoresistor. Scale it to the data: σ = mult × median nearest-neighbour distance **in normalised space**. Then the knob means the same thing on every instrument a student builds.

```c
/* O(K^2 N). ~80 kflops at K=100,N=8: negligible next to the Cholesky.
 * `tmp` is K floats of scratch and is destroyed.                          */
static float iris_median_nn(const float *X, int k, int n, float *tmp)
{
    for (int i = 0; i < k; ++i) {
        float best = 3.4e38f;
        for (int j = 0; j < k; ++j) if (j != i) {
            float d = iris_sqdist(&X[(size_t)i*n], &X[(size_t)j*n], n);
            if (d < best) best = d;
        }
        tmp[i] = sqrtf(best);
    }
    iris_sort_f32(tmp, k);              /* insertion sort; K<=400, no qsort dep */
    return tmp[k >> 1];
}

/* knob 0..1 -> sigma, log-spaced, clamped to the numerically safe range.  */
static float iris_sigma_from_knob(float knob, float med_nn, int kernel)
{
    const float lo = 0.35f;
    const float hi = (kernel == IRIS_KERNEL_WENDLAND) ? 6.0f : 1.60f;
    if (knob < 0.0f) knob = 0.0f; else if (knob > 1.0f) knob = 1.0f;
    float mult = lo * iris_expneg(-knob * logf(hi / lo));   /* lo*(hi/lo)^knob */
    float s = mult * med_nn;
    return (s > 1e-6f) ? s : 1e-6f;
}
```

### 4.4 Packed Cholesky and triangular solves

Packed lower-triangular halves the memory (20.2 KB instead of 40 KB at K=100), which directly buys you √2× more centres. There is no cache penalty on internal SRAM.

```c
/* In-place Cholesky of a packed lower-triangular SPD matrix. A -> L.
 * Returns IRIS_ERR_SOLVE if a pivot is non-positive (or NaN). ~n^3/6 madds. */
static iris_status_t iris_chol(float *L, int n)
{
    for (int i = 0; i < n; ++i) {
        float *Li = &L[IRIS_LT(i, 0)];
        for (int j = 0; j <= i; ++j) {
            const float *Lj = &L[IRIS_LT(j, 0)];
            float s = Li[j];
            for (int k = 0; k < j; ++k) s -= Li[k] * Lj[k];
            if (i == j) {
                if (!(s > 0.0f)) return IRIS_ERR_SOLVE;   /* also catches NaN */
                Li[i] = sqrtf(s);
            } else {
                Li[j] = s / Lj[j];
            }
        }
    }
    return IRIS_OK;
}

/* Solve L L^T x = b in place on x. ~n^2 madds. */
static void iris_chol_solve(const float *L, float *x, int n)
{
    for (int i = 0; i < n; ++i) {                   /* forward: L y = b */
        const float *Li = &L[IRIS_LT(i, 0)];
        float s = x[i];
        for (int k = 0; k < i; ++k) s -= Li[k] * x[k];
        x[i] = s / Li[i];
    }
    for (int i = n - 1; i >= 0; --i) {              /* back: L^T x = y */
        float s = x[i];
        for (int k = i + 1; k < n; ++k) s -= L[IRIS_LT(k, i)] * x[k];
        x[i] = s / L[IRIS_LT(i, i)];
    }
}
```

**Do not add Kahan summation to the inner loop.** I implemented and measured it: at σ=1.0× the error went 1.38e-05 → 1.09e-05, at σ=3.0× it went 5.38e-03 → 5.27e-03 **[measured]**. The residual is conditioning, not accumulation, and you would be paying four extra flops per inner iteration — roughly a 60 % slowdown of the hottest loop in the library — for nothing.

### 4.5 The RBF solve, end to end

```c
/* Build the packed kernel matrix with ridge/jitter on the diagonal.
 * X is the NORMALISED centre array, K x N, row-major.                     */
static void iris_rbf_build(float *K_, const float *X, int k, int n,
                         const iris_bank_t *b, float jitter)
{
    for (int i = 0; i < k; ++i)
        for (int j = 0; j <= i; ++j) {
            float v = iris_kern(b, iris_sqdist(&X[(size_t)i*n], &X[(size_t)j*n], n));
            K_[IRIS_LT(i, j)] = (i == j) ? v + jitter : v;
        }
}

/* Full solve. L is K(K+1)/2 floats of scratch, col is K floats.
 * On success W (K x M) holds the interpolation weights.                   */
static iris_status_t iris_rbf_solve(iris_bank_t *b, const float *X, const float *Y,
                                int k, int n, int m,
                                float *L, float *col, float *W)
{
    /* Jitter: 1e-6 is enough at every bandwidth we allow, and it costs only
     * ~1e-5 of interpolation exactness. Larger jitter does NOT rescue a
     * too-wide Gaussian -- it just trades one kind of wrongness for
     * another (measured: jitter 1e-3 at sigma=4x gives fit error 0.90).
     * The bandwidth clamp is the real fix; jitter only guards against
     * exact-duplicate demonstrations that slipped past thinning.          */
    const float jitter = 1e-6f;

    iris_rbf_build(L, X, k, n, b, jitter);
    iris_status_t st = iris_chol(L, k);
    if (st != IRIS_OK) return st;                    /* -> IRIS_HEALTH_ILL_CONDITIONED */

    for (int d = 0; d < m; ++d) {
        for (int i = 0; i < k; ++i) col[i] = Y[(size_t)i * m + d];
        iris_chol_solve(L, col, k);
        for (int i = 0; i < k; ++i) W[(size_t)i * m + d] = col[i];
    }
    b->n_centres = (uint16_t)k;
    return IRIS_OK;
}

/* Inference. Real-time safe: bounded, allocation-free, branch-light. */
static void iris_rbf_eval(const iris_bank_t *b, const float *xn, int n, int m,
                        float *out)
{
    const float *C = b->centres, *W = b->weights;
    const int k = b->n_centres;
    for (int d = 0; d < m; ++d) out[d] = 0.0f;
    for (int i = 0; i < k; ++i) {
        float phi = iris_kern(b, iris_sqdist(xn, &C[(size_t)i*n], n));
        if (phi == 0.0f) continue;                 /* Wendland fast path */
        const float *w = &W[(size_t)i * m];
        for (int d = 0; d < m; ++d) out[d] += w[d] * phi;
    }
}
```

### 4.6 Ridge regression

Accumulate the normal equations directly — never materialise the K×P design matrix — then reuse the same Cholesky. This is why ridge costs about sixty extra lines rather than a new solver.

```c
/* A: packed P x P, B: P x M, W: P x M (output).
 * P is the polynomial feature count: order 1 -> 1+N, order 2 -> 1+N+N(N+1)/2. */
static iris_status_t iris_ridge_fit(const float *Phi, const float *Y,
                                int k, int p, int m, float *A, float *B, float *W,
                                float *col)
{
    for (size_t i = 0; i < IRIS_LTN(p); ++i) A[i] = 0.0f;
    for (size_t i = 0; i < (size_t)p * m; ++i) B[i] = 0.0f;

    for (int r = 0; r < k; ++r) {
        const float *x = &Phi[(size_t)r * p];
        for (int i = 0; i < p; ++i) {
            for (int j = 0; j <= i; ++j) A[IRIS_LT(i, j)] += x[i] * x[j];
            for (int d = 0; d < m; ++d) B[(size_t)i*m + d] += x[i] * Y[(size_t)r*m + d];
        }
    }

    /* lambda = 1e-3 * trace(A)/P. Scale-free: A grows like K, so a fixed
     * absolute lambda would silently stop regularising as demos accumulate.
     * The bias term (i==0) is never penalised -- penalising it makes the
     * model refuse to predict a constant offset, which musicians hear
     * immediately as "the mapping is quiet in the middle".                */
    float tr = 0.0f;
    for (int i = 0; i < p; ++i) tr += A[IRIS_LT(i, i)];
    float lambda = 1e-3f * tr / (float)p;
    if (lambda < 1e-6f) lambda = 1e-6f;
    for (int i = 1; i < p; ++i) A[IRIS_LT(i, i)] += lambda;
    A[IRIS_LT(0, 0)] += 1e-8f;

    iris_status_t st = iris_chol(A, p);
    if (st != IRIS_OK) return st;
    for (int d = 0; d < m; ++d) {
        for (int i = 0; i < p; ++i) col[i] = B[(size_t)i*m + d];
        iris_chol_solve(A, col, p);
        for (int i = 0; i < p; ++i) W[(size_t)i*m + d] = col[i];
    }
    return IRIS_OK;
}
```

Verified against a known quadratic in two variables: recovered coefficients `[0.5000, −1.2500, 2.0000, 0.7500, −1.5000, 0.2500]` against truth `[0.5, −1.25, 2, 0.75, −1.5, 0.25]` **[measured]** — exact to four decimals in float32.

**When to use Householder QR instead — and why we don't.** Normal equations square the condition number: cond(A) = cond(Φ)². The S3 has no hardware `double`; soft-float would be 50–100× slower and is not an option for a live retrain. float32 has ε ≈ 1.2e-7, so you lose all significance once cond(Φ)² > 10⁷, i.e. cond(Φ) > ~3×10³. With standardised features, order-1 gives cond(Φ) ~10¹–10², comfortably safe. Order 2 on 8 inputs (P=45) reaches cond(Φ) ~10³–10⁴ — right at the edge, which is exactly why the λ above is not optional. Order 3 (P=165) is past it, and QR would need the full K×P matrix resident: 100×165×4 = 66 KB, more than the RBF's entire arena, for a model that is worse than RBF at everything a musician wants. **Verdict: cap `poly_order` at 2, ship normal equations, never ship QR.** `iris_init()` returns `IRIS_ERR_ARG` for order ≥ 3 with that reasoning in a comment.

### 4.7 MLP activation

`tanhf` from newlib is ~300 cycles. A rational approximation is ~25 and is *more* accurate than a 65-entry interpolated LUT while using zero memory **[measured]**:

| candidate | worst abs error over [−6,6] | cost |
|---|---|---|
| `x(27+x²)/(27+9x²)` | 2.35e-02 | cheapest |
| 65-entry LUT + lerp | 1.50e-03 | 260 B + a load |
| **7th-order Padé (below)** | **1.11e-04** | ~7 mul + 1 div |

```c
/* tanh, max abs error 1.1e-4 over [-6,6]. Beats a 65-entry LUT, no memory. */
static inline float iris_tanh(float x)
{
    if (x >  4.9f) return  1.0f;
    if (x < -4.9f) return -1.0f;
    float x2 = x * x;
    return x * (135135.0f + x2*(17325.0f + x2*(378.0f + x2)))
             / (135135.0f + x2*(62370.0f + x2*(3150.0f + x2*28.0f)));
}
```

Ship this and skip the LUT entirely. Offer ReLU as `IRIS_ACT_RELU` for students who want to see the difference — it is genuinely faster and for 8→16→4 regression the quality difference is usually inaudible, which is itself a useful thing for a student to discover.

### 4.8 Feature normalization

**Where:** computed inside `iris_train()` from the current example set, **frozen into the bank**, and applied inside `iris_infer()`. Never recomputed at inference — a normalization that drifts with incoming data is a mapping that drifts under the performer's hands, and it is maddening to debug by ear.

Per input dimension, standardize: `xn = (x − mean) / sd`, with `sd` floored at 1e-6 and the dimension flagged dead (and forced to zero) below that. Standardization rather than min-max because the Gaussian kernel measures Euclidean distance, and a single σ is only meaningful when every axis contributes comparably.

Outputs are **not** normalized for RBF or ridge (both handle arbitrary output scale natively) but their observed min/max **are** recorded, for two reasons: `IRIS_F_CLAMP_OUTPUT` clamps inference to `[min − 0.2·range, max + 0.2·range]`, and the MLP needs the scaling to keep tanh in range.

Because each bank owns its normalization, the two A/B banks can have been trained on different example sets with different normalizations and still be switchable mid-performance. That falls out of the per-bank layout for free.

**Persistence:** the `NORM` chunk stores `mean` and `scale` (= 1/sd, pre-inverted so inference is a multiply) for all N inputs, then `min`/`max` for all M outputs. A model file without its normalization is not a model.

---

## 5. The model file format

Frozen as of v0.1. Little-endian, chunked, CRC'd. Round-trip verified **[measured]**: 72-byte file written, magic/version/CRC validated on read, known chunks decoded, unknown chunks skipped by length.

```
byte  size  field
 0     4    magic, ASCII "EWEK" (never byte-swapped; also the endian check)
 4     2    format_version, u16 LE  = 1
 6     2    flags, u16 LE           = 0 (reserved, readers must reject non-zero)
 8     4    total_size, u32 LE      (whole file, including this header)
12     4    crc32 (IEEE 802.3, init 0xFFFFFFFF, reflected) over bytes [16, total_size)
16    ..    chunk stream
```

Each chunk: `fourcc[4]` + `length u32 LE` + `payload[length]` + zero padding to a 4-byte boundary (padding not counted in `length`).

| fourcc | contents |
|---|---|
| `MDL0` | model u16, model_version u16, n_in u16, n_out u16, n_examples u32, kernel u8, poly u8, knn_k u8, flags u8, sigma f32, gamma f32 |
| `NORM` | N × {mean f32, scale f32}, then M × {min f32, max f32} |
| `RBFC` | centres, K·N f32, row-major |
| `RBFW` | weights, K·M f32, row-major |
| `RIDG` | P·M f32 |
| `KNNL` | K u16 class labels |
| `MLPW` | layer sizes u16[], then weights and biases f32 |
| `EXIN` | example inputs, K·N f32 |
| `EXOU` | example outputs, K·M f32 |
| `EXID` | example ids u32[K], take ids u16[K] |
| `TEXT` | **mandatory.** ASCII, human-readable summary |
| `META` | optional: instrument name, ISO-8601 date, library version string |

Readers must skip unknown chunks by length and must reject a `MDL0.model_version` they do not recognise rather than guessing.

Serialization is byte-at-a-time, always:

```c
static void iris_w8 (iris_wbuf *b, uint8_t v);
static void iris_w16(iris_wbuf *b, uint16_t v){ iris_w8(b,v&0xFF); iris_w8(b,(v>>8)&0xFF); }
static void iris_w32(iris_wbuf *b, uint32_t v){ iris_w16(b,v&0xFFFF); iris_w16(b,v>>16); }
static void iris_wf (iris_wbuf *b, float f)   { uint32_t u; memcpy(&u,&f,4); iris_w32(b,u); }
```

### What makes a format survive twenty years

Pure Data's `.pd` has outlived several generations of compilers, two CPU architecture transitions, and its own GUI. Here is what actually did the work, and what we copy:

1. **Skip-unknown-by-length.** Every extension is additive; a 2026 reader opens a 2040 file and gets everything it understands. This is the whole trick, and it is why IFF, RIFF, PNG and Pd all made it. A packed struct with a version switch does not.
2. **No struct dumps, ever.** `fwrite(&s, sizeof s, 1, f)` encodes your compiler's padding rules, enum width and alignment into the file. Those change. Byte-at-a-time serialization has no such dependency. This is the single most common way formats die.
3. **Length prefixes everywhere**, so truncation fails loudly at a chunk boundary instead of silently producing a model with garbage weights that a performer discovers on stage.
4. **One stated endianness.** Little-endian, chosen because every plausible target (Xtensa, ARM, RISC-V, x86) is LE. The read helpers assemble from bytes, so a big-endian host would still work.
5. **A checksum**, so corruption is distinguishable from incompatibility. On flash-backed MCU storage with brownouts, this is not optional.
6. **A mandatory human-readable chunk.** Pd survives partly because you can open a patch in a text editor and see what it is. We cannot be a text format on an MCU, so we embed text instead: `TEXT` holds something like `iris 0.1 RBF gaussian n_in=2 n_out=2 k=41 sigma=0.184 2026-08-21`. Someone in 2046 with a hex editor recovers the semantics without the spec.
7. **The spec lives in the repo as `docs/format-v1.md`, and `tests/golden/` contains real `.ewek` files that CI decodes and checks against expected outputs on every commit.** This is the actual mechanism. Formats do not survive because they were well designed; they survive because something automatically screams when you break them. Everything above is necessary and none of it is sufficient without this.

Corollary rule for the team: **`IRIS_FORMAT_VERSION` bumps only for a breaking change, and a breaking change requires deleting a golden file, which requires a discussion.** Adding a chunk is not a breaking change and must not bump it.

---

## 6. Real-time architecture on ESP32-S3

**Core allocation.**

| | core 0 | core 1 |
|---|---|---|
| task | `control` prio 5 | `audio` prio 20 |
| rate | 1 kHz | I²S DMA driven |
| does | sensors, buttons, `iris_record`, `iris_train`, `iris_resolve`, `iris_infer`, LEDs | `synth_render_block`, parameter smoothing |
| touches float | yes → **pinned** | yes → **pinned** |

Inference runs on core 0 with the control loop, not in audio. At 25–40 µs per frame it fits a 1 ms budget with room to spare, and keeping it off core 1 means a pathological training set can never lengthen an audio block. Core 1 reads two smoothed floats and nothing else.

**Three hard platform constraints, and what we do about each.**

*Float is forbidden in an ISR under ESP-IDF FreeRTOS.* The FPU register file is not saved on interrupt entry, so a float operation in an ISR silently corrupts the interrupted task's registers — which manifests as an instrument that goes slightly out of tune after ten minutes and is essentially undebuggable. Therefore: **no `iris_*` function is ever called from an ISR.** The I²S ISR does exactly one thing, `xQueueSendFromISR` / semaphore give; all float work happens in task context. This is documented at the top of `iris.h` and repeated in the ESP32 port header, because it is the constraint students will violate.

*Any task touching float must be pinned.* FPU context is per-core and lazily saved; an unpinned task that migrates cores loses its FPU state. Both of our tasks use `xTaskCreatePinnedToCore` with an explicit core id. Debug builds compile `IRIS_ASSERT_PINNED()` into `iris_init`, `iris_infer` and `iris_train`, checking `xTaskGetCoreID(NULL) != tskNO_AFFINITY`. Note the rule is *pinned*, not *same core* — inference on core 0 and audio on core 1 is fine.

*A flash write stalls both cores for 10–50 ms.* During an SPI-flash erase or program, the cache is disabled and any code or `const` data resident in flash is unreachable — both cores, not just the writing one. Our position:

- `iris_save()` returns `IRIS_ERR_BUSY` if `iris_rt_begin()` is in effect. Making this an error rather than a warning is deliberate: a student who hits it learns the rule in one build cycle.
- The reference app's save handler fades the output over 200 ms, calls `iris_rt_end()`, saves, then `iris_rt_begin()` and fades back in. Save is a deliberate, audible, non-performance action, like a tape machine spinning up.
- The "keep playing through it" alternative — `IRAM_ATTR` on the audio task and every function it calls, no flash-resident `const` tables, `ESP_INTR_FLAG_IRAM` on the I²S interrupt, and a DMA ring long enough to cover the stall — is achievable but marginal: eight 512-frame buffers at 48 kHz is ~85 ms, which covers a 50 ms worst case only if nothing else contends. We document it as an advanced option and do not make it the default. Mute and save.

**Lock-free bank publication.** Training writes only the idle bank, then:

```c
static inline void iris_publish(iris_t *h, int bank)
{
    __atomic_store_n(&h->live_bank, (uint8_t)bank, __ATOMIC_RELEASE);
}
static inline const iris_bank_t *iris_acquire(const iris_t *h)
{
    uint8_t b = __atomic_load_n(&h->live_bank, __ATOMIC_ACQUIRE);
    return &h->bank[b & 1u];
}
```

No mutex on the inference path. The release/acquire pair is what makes the weights written before the store visible to the reader after the load; a plain `volatile` would not guarantee that on a dual-core Xtensa.

**Torn input vectors.** In the common case `iris_set_inputs` and `iris_infer` are both on core 0 and nothing can tear. For cross-core use (sensors on core 0, inference somewhere else), the input vector is a seqlock — ten lines that eliminate a whole class of "my mapping glitches once a minute" bug reports:

```c
static void iris_input_write(iris_t *h, const float *in, int n)
{
    uint32_t s = h->in_seq;
    __atomic_store_n(&h->in_seq, s + 1, __ATOMIC_RELAXED);   /* odd = writing */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    for (int i = 0; i < n; ++i) h->in[i] = in[i];
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&h->in_seq, s + 2, __ATOMIC_RELAXED);   /* even = stable */
}
static void iris_input_read(const iris_t *h, float *dst, int n)
{
    uint32_t a, b;
    do {
        a = __atomic_load_n(&h->in_seq, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        for (int i = 0; i < n; ++i) dst[i] = h->in[i];
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        b = __atomic_load_n(&h->in_seq, __ATOMIC_RELAXED);
    } while ((a & 1u) || a != b);
}
```

The reader can spin, so it is bounded only if the writer is not preempted mid-write. The control task writes at 1 kHz and the write is a dozen stores; in practice one retry is the worst case. Documented as such.

**The mutable example store is single-owner: core 0 writes it, core 0 reads it during training, and inference never touches it.** That invariant is what the per-bank centre copies buy, and it is worth the 3.2 KB.

---

## 7. Extensibility for student sensors

**The interface is: fill a float array and call `iris_set_inputs()`. That's it. There is no sensor abstraction, and adding one would be a mistake.**

The tempting alternative is `iris_source_t` — a struct of function pointers, `init/read/n_channels`, registered with the library and called at the right time. I have watched enough of these go wrong to argue against it concretely:

- **It makes the student reason about *when* their code runs and *on which core*.** That is the single hardest concept in the whole system and it is not what the course is about. A registered callback invoked from inside the library will eventually be invoked from a context the student did not expect, and a blocking 400 µs I²C read will land in it.
- **It makes the bad thing structurally possible.** With a plain array, the student's `Wire.requestFrom()` physically cannot execute inside a timing-critical region, because they wrote the call site themselves and it is sitting in `loop()`.
- **Sensor code is the part students most want to copy-paste from a datasheet or an Adafruit example.** An array accepts any of that verbatim. An interface demands it be rewritten into a shape the library likes, which is a tax on exactly the work that is supposed to be theirs.
- **A float array is inspectable.** You can print it, log it to SD, replay it from a file on the desktop build, and diff it. A struct of function pointers is none of those. Our host-side test harness feeds recorded arrays through the identical code path the MCU runs, which is how we debug a student's instrument over email without the instrument.
- **It removes a whole category of support burden**: lifetime bugs where a registered source outlives its backing object.

The cost of this choice is that sensor code is not *shareable* as a library-recognised plug-in. That cost is real and I accept it. In practice students share sensor code as a `.h` with a `read_myimu(float *out)` function, which is a better unit of sharing anyway because it has no dependency on us at all.

What students *do* need is the layer above raw reads, and that goes in a separate optional header, `iris_features.h` — pure functions on float arrays, not part of the core, not required, not linked if unused:

```c
/* ---- iris_features.h : optional. Pure functions, no iris_t, no state
   owned by the library. Everything here operates on plain floats. ------- */

/* One-pole smoother. Sensor noise is the #1 cause of a "twitchy" mapping. */
typedef struct { float y, a; } ewf_lp1_t;
static inline void  ewf_lp1_init(ewf_lp1_t *s, float cutoff_hz, float rate_hz) {
    float k = 6.2831853f * cutoff_hz / rate_hz;
    s->a = k / (k + 1.0f); s->y = 0.0f;
}
static inline float ewf_lp1(ewf_lp1_t *s, float x) { s->y += s->a*(x - s->y); return s->y; }

/* First difference -> velocity. "Map the SPEED of my flex sensor" is the
   most-requested feature every single cohort, and it is four lines.       */
typedef struct { float prev; float gain; } ewf_diff_t;
static inline float ewf_diff(ewf_diff_t *s, float x) {
    float d = (x - s->prev) * s->gain; s->prev = x; return d;
}

/* Auto-calibrating range normaliser with slow decay.
   "My flex sensor reads 1830..2740, make it 0..1" is 80% of week-one
   debugging. Wave the sensor through its full range once and this is done. */
typedef struct { float lo, hi, decay; } ewf_range_t;
static inline void ewf_range_init(ewf_range_t *r, float decay /*e.g. 0.99995f*/) {
    r->lo =  3.4e38f; r->hi = -3.4e38f; r->decay = decay;
}
static inline float ewf_autoscale(ewf_range_t *r, float x) {
    if (x < r->lo) r->lo = x;
    if (x > r->hi) r->hi = x;
    float mid = 0.5f*(r->lo + r->hi), half = 0.5f*(r->hi - r->lo);
    r->lo = mid - half*r->decay;            /* slowly forget stale extremes */
    r->hi = mid + half*r->decay;
    return (half > 1e-9f) ? (x - r->lo) / (2.0f*half) : 0.5f;
}
```

Three structs, no allocation, no core coupling. A student writes:

```c
float in[3];
in[0] = ewf_autoscale(&r0, (float)analogRead(4));
in[1] = ewf_lp1(&s1, ewf_autoscale(&r1, (float)analogRead(5)));
in[2] = ewf_diff(&d1, in[1]);          /* velocity of the second sensor */
iris_set_inputs(ml, in);
```

and they have a three-dimensional feature space including a derivative, with no framework in sight.

---

## 8. Repo layout

```
iris/
├── LICENSE                       BSD-3-Clause
├── README.md
├── CMakeLists.txt                dual-mode, see below
├── library.properties            Arduino IDE / Library Manager
├── library.json                  PlatformIO
├── idf_component.yml             ESP-IDF component registry
├── src/
│   ├── iris.h                  THE library. Header-only C99 core + C++ façade.
│   ├── iris_features.h         optional sensor helpers (section 7)
│   └── iris.c                  1 line: #define IRIS_IMPLEMENTATION + include
│                                 (exists only so Arduino's build system finds a TU)
├── ports/
│   ├── iris_port.h             the six prototypes + IRIS_PORT_* config macros
│   ├── posix/iris_port_posix.c
│   └── esp32/iris_port_esp32.c
├── examples/
│   ├── 01_minimal/01_minimal.ino          the 25-line sketch from section 2
│   ├── 02_two_sensors_rbf/
│   ├── 03_classifier_knn/
│   ├── 04_save_load_nvs/
│   └── idf_instrument/                    full two-task ESP-IDF app
│       ├── CMakeLists.txt
│       └── main/
├── tests/
│   ├── test_main.c               tiny assert harness, no framework
│   ├── test_numerics.c           Cholesky, expneg, tanh, ridge vs known truth
│   ├── test_store.c              record/delete/take/thinning invariants
│   ├── test_format.c             round-trip + skip-unknown-chunk + bad-CRC
│   ├── test_arena.c              asserts IRIS_ARENA_* macros >= iris_arena_size()
│   ├── num_probe.c               the bandwidth/kernel sweep from this document
│   └── golden/
│       ├── rbf_2in_2out_41ex.ewek
│       ├── rbf_2in_2out_41ex.expected
│       └── ridge_8in_4out.ewek
├── docs/
│   ├── format-v1.md              the frozen spec
│   ├── porting.md
│   └── teaching/                 lab handouts, wiring diagrams
└── .github/workflows/ci.yml      ubuntu-latest: build, -Werror, asan+ubsan, golden
```

**`library.properties`** (Arduino):

```ini
name=iris
version=0.1.0
author=<team>
maintainer=<team> <email>
sentence=Interactive machine learning for gestural musical instruments on microcontrollers.
paragraph=Header-only C99 core with an Arduino C++ facade. RBF/GP interpolation, ridge regression and k-NN, trained on the device from demonstrations. No dependencies, no dynamic allocation.
category=Data Processing
url=https://github.com/<org>/iris
architectures=esp32,*
includes=iris.h
license=BSD-3-Clause
```

**`library.json`** (PlatformIO):

```json
{
  "name": "iris",
  "version": "0.1.0",
  "description": "Interactive machine learning for gestural musical instruments on microcontrollers. RBF/GP interpolation, ridge regression and k-NN, trained on-device. Header-only C99, zero dependencies, no dynamic allocation.",
  "keywords": ["machine-learning", "music", "nime", "esp32", "gesture", "interpolation"],
  "license": "BSD-3-Clause",
  "repository": { "type": "git", "url": "https://github.com/<org>/iris.git" },
  "frameworks": ["arduino", "espidf"],
  "platforms": "*",
  "headers": ["iris.h"],
  "build": {
    "srcDir": "src",
    "includeDir": "src",
    "flags": ["-I ports"]
  },
  "examples": [{ "name": "minimal", "base": "examples/01_minimal", "files": ["01_minimal.ino"] }]
}
```

**`idf_component.yml`** (ESP-IDF component registry):

```yaml
version: "0.1.0"
description: >
  Interactive machine learning for gestural musical instruments on
  microcontrollers. RBF/GP interpolation, ridge regression and k-NN,
  trained on-device from demonstrations. Header-only C99, zero
  dependencies, no dynamic allocation in the core.
url: https://github.com/<org>/iris
repository: https://github.com/<org>/iris.git
documentation: https://github.com/<org>/iris/blob/main/README.md
issues: https://github.com/<org>/iris/issues
license: BSD-3-Clause
tags:
  - machine-learning
  - audio
  - music
  - gesture
dependencies:
  idf:
    version: ">=4.4"
targets:
  - esp32
  - esp32s3
  - esp32c3
  - esp32c6
files:
  exclude:
    - "tests/**"
    - "docs/**"
    - ".github/**"
```

**`CMakeLists.txt`** — dual-mode, so the same file is an IDF component under `idf.py` and a normal library plus test binary under plain CMake:

```cmake
cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# Mode A: ESP-IDF component. idf.py includes this file with ESP_PLATFORM set
# and expects idf_component_register() and nothing else.
# ---------------------------------------------------------------------------
if(ESP_PLATFORM)
    idf_component_register(
        SRCS        "src/iris.c"
                    "ports/esp32/iris_port_esp32.c"
        INCLUDE_DIRS "src" "ports"
        REQUIRES     esp_timer
        PRIV_REQUIRES nvs_flash)

    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wall -Wextra -Wdouble-promotion -ffast-math -fno-math-errno)
    # -Wdouble-promotion is not optional here: a stray `2.0 * x` silently
    # promotes to double, which the S3 emulates in software, and a 40x
    # slowdown in the Cholesky inner loop is very hard to find by reading.
    return()
endif()

# ---------------------------------------------------------------------------
# Mode B: host build (ubuntu-latest CI, desktop development, no MCU).
# ---------------------------------------------------------------------------
project(iris C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(iris STATIC src/iris.c ports/posix/iris_port_posix.c)
target_include_directories(iris PUBLIC src ports)
target_compile_options(iris PRIVATE
    -Wall -Wextra -Werror -Wdouble-promotion -Wconversion -Wshadow)
target_link_libraries(iris PUBLIC m)

option(EMBWEK_BUILD_TESTS "Build the golden-vector tests" ON)
if(EMBWEK_BUILD_TESTS)
    enable_testing()
    foreach(t numerics store format arena)
        add_executable(test_${t} tests/test_${t}.c tests/test_main.c)
        target_link_libraries(test_${t} PRIVATE iris)
        target_compile_options(test_${t} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(test_${t} PRIVATE -fsanitize=address,undefined)
        add_test(NAME ${t}
                 COMMAND test_${t}
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # The numerical characterisation from the design doc, kept runnable so
    # the bandwidth clamp can be re-derived rather than trusted.
    add_executable(num_probe tests/num_probe.c)
    target_link_libraries(num_probe PRIVATE iris)
endif()
```

CI (`ubuntu-latest`) runs `cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`. No MCU, no toolchain, no hardware in the loop. A separate job runs `idf.py build` on `examples/idf_instrument` to catch component-manifest breakage, but it is allowed to be the slow job.

---

## 9. What I would build first

### v0.1 — two weeks, useful to a cohort starting now

The bet: a student's semester succeeds or fails on *record → undo → retrain → hear it*, at speed, with no screen. Everything else is decoration.

**Ship:**
- Config, `iris_arena_size`, `iris_init`, the `IRIS_ARENA_RBF` macro, and the CI assert that the macro is never smaller than the function.
- Example store: monotonic ids, takes, auto-thinning, `delete_example` / `delete_last_example` / `delete_take` / `delete_last_take` / `clear`. **Thinning is in v0.1, not later** — without it, hold-to-record produces a singular matrix and the flagship model appears broken on day one.
- Two banks. `iris_train` fills the idle bank and publishes; `iris_bank_flip` is both undo-train and A/B.
- Normalization, frozen per bank, with dead-dimension detection.
- **RBF with the Gaussian kernel**, bandwidth clamped to [0.35, 1.6]×, jitter 1e-6, `iris_set_bandwidth` + `iris_resolve` (synchronous; the caller rate-limits).
- **k-NN**, because it is a storage-only afternoon and it gives the classification half of the course somewhere to stand.
- **Ridge order ≤ 2** — it reuses `iris_chol` and `iris_chol_solve` verbatim, so it is ~60 new lines for a second model. Include it; it would be strange not to.
- Numerics: `iris_expneg`, packed Cholesky and solves, `iris_median_nn`, output clamping.
- `iris_stats`, `iris_health`, `iris_confidence`, `iris_nearest`, `iris_audition_example`. The last one is the screenless training-set inspector and it costs almost nothing once the store exists.
- Format v1: `MDL0`/`NORM`/`RBFC`/`RBFW`/`EXIN`/`EXOU`/`EXID`/`TEXT`. Save/load to a byte buffer, a POSIX file, and an NVS blob.
- Ports: posix + esp32.
- C++ façade, the 25-line sketch, one wiring diagram.
- CI on ubuntu-latest with golden vectors, `-Werror`, asan + ubsan.

**Deliberately not in v0.1:** MLP, DTW, Wendland, the ladder, async/incremental training, PSRAM support, SD cards, MIDI, any desktop tool. Every one of these is a thing a student can live a semester without.

### v0.2 — mid-semester, driven by what actually breaks

- **`IRIS_KERNEL_WENDLAND`.** It is thirty lines and it is measurably better than the Gaussian everywhere except musical character. It is the escape hatch for the student who wants 250 demos or a very smooth mapping.
- **Bandwidth re-solve on core 0 with rate limiting**, done properly in the reference IDF app: dirty flag, 25 Hz cap, bank swap. This is what makes the knob feel like an instrument control rather than a settings menu.
- `iris_train_step` cooperative training (needed once the MLP lands; build the plumbing now).
- The full two-task ESP-IDF reference instrument, `IRAM`/`DRAM` attributes and all, as the thing students fork.
- Save to SPIFFS/SD, and multiple model slots on disk with `iris_peek` so a performer can carry a set list.
- Classification confidence and per-class example counts in `iris_stats` — "you have 40 examples of gesture A and 3 of gesture B" is the most common student bug and the library should just say it.

### v0.3 — end of semester, next cohort

- **MLP** with `iris_tanh` and a ReLU option, trained via `iris_train_step` so it never blocks. Its real value is pedagogical: students compare 0.3 s of backprop against 8.5 ms of exact interpolation and reach their own conclusions about what "learning" buys them here.
- **Bandwidth ladder + crossfade**, the K > 120 answer, now that the exactness-under-blending property is verified.
- **O(K²) incremental centre addition** via Cholesky border update — makes "record a demo and hear it immediately, without a retrain" essentially free at ~0.1 ms.
- **DTW** template matching.
- **Desktop tools**: a small Python module that parses `.ewek`, plots the training set, and re-solves with the identical algorithm. This is the payoff for freezing the format in v0.1 — an offline debugger for a screenless instrument, built from files students already have. It is also the thing that most reduces our support burden across cohorts, which is the real long-term constraint.

---

## Open questions where I'm not certain

- **Whether the Gaussian should be the default at all**, given Wendland is better on every measurable axis. My judgment is yes, because "the mapping goes wild between demonstrations" is a musical feature that compact support cannot produce, and the flagship's selling point is character, not accuracy. But if the first cohort mostly reports "it sounds broken when I turn the knob up", flip the default and let the Gaussian be the expert option.
- **The 0.05 thinning default.** I picked it by reasoning about a 200 Hz record loop and a 3-second gesture, not by measurement. It needs one afternoon with a real flex sensor and a real performer. It is a `iris_config_t` field precisely so this is cheap to correct.
- **Whether `n_banks` should ever exceed 2.** Musicians will ask for a set list of eight mappings. My instinct is that this belongs on disk with `iris_peek` and fast NVS load, not in RAM as eight banks — but a fast bank switch is glitch-free and a disk load is not. If mid-performance switching between more than two mappings turns out to matter, that assumption should be revisited before the arena layout ossifies.
- **The extrapolation behaviour under Wendland.** Decaying to zero output is safe but may be musically dead. Blending toward the nearest demo's output as confidence drops is the obvious alternative and I have not tested how it sounds.

**Verification artifacts:** the numeric work behind this section -- the bandwidth/kernel/jitter sweeps, the Kahan comparison, the ladder-blend exactness proof, the activation benchmarks and the file-format round-trip -- was run in a scratch directory that was not retained, and the intended `tests/num_probe.c` was never extracted from it. The figures above are a record of what was measured, not a harness you can re-run; what *is* re-runnable is `sh build.sh audit`, which pins the same behaviour by hash.
