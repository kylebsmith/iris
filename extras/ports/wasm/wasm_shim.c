/* wasm_shim.c — the platform layer for "the platform is a web browser".
 *
 * This is the ONLY file that changes between the browser and the ESP32.
 * iris.h is byte-for-byte identical in both. That is the whole point of
 * keeping the core free of hardware, libc and malloc: it makes the core
 * portable by construction rather than by effort.
 *
 * Build (see bench/build.sh):
 *   clang --target=wasm32 -O2 -nostdlib -ffreestanding \
 *         -Wl,--no-entry -Wl,--export-dynamic -Wl,--allow-undefined \
 *         -Wl,-z,stack-size=32768 -Wl,--initial-memory=1114112 \
 *         -o iris.wasm wasm_shim.c
 *
 * Two model instances live here:
 *   A (g_k)  — the live instrument the musician plays and edits.
 *   B (g_b)  — a snapshot of A taken just before a reroll, so the musician
 *              can hold a key and hear the previous instrument (A/B), or
 *              revert to it entirely.
 * A and B are transferred through iris_save/iris_load bytes (g_ab), never by
 * copying the arena — the arena contains internal pointers.
 */

#define IRIS_API __attribute__((used))
#include "../../../iris.h"

#define NI_MAX  12  /* the shim allows any input count up to this  */
#define NO       4  /* sound: pitch, timbre, pressure, aux         */
#define HID_MAX 32
#define CAP    256  /* examples                                    */

#define EXPORT __attribute__((visibility("default"), used))

static unsigned char g_arena[IRIS_ARENA(NI_MAX, HID_MAX, NO, CAP)];
static int g_ni = 2;
static unsigned char g_file[32768];
static iris *g_k = 0;

/* --- previous-model (B) instance for A/B audition ------------------------- */
static unsigned char g_arena_b[IRIS_ARENA(NI_MAX, HID_MAX, NO, CAP)];
static unsigned char g_ab[32768];   /* internal A<->B transfer, never JS-visible */
static iris *g_b = 0;
static int g_nh = 12;
static int g_b_valid = 0;
static unsigned int g_b_seed = 0;
static float g_b_err = 1.0f;

/* Scratch buffers JS writes into and reads out of. Passing arrays across the
   wasm boundary means agreeing on an address; these are those addresses.
   s_ex_in/s_ex_out receive example rows so that reading a row never clobbers
   the live input (s_in) or the live prediction (s_out). */
static float s_in[IRIS_MAX_IN];
static float s_out[IRIS_MAX_OUT];
static float s_out_b[IRIS_MAX_OUT];
static float s_ex_in[IRIS_MAX_IN];
static float s_ex_out[IRIS_MAX_OUT];

EXPORT float *iris_js_in_ptr(void)     { return s_in; }
EXPORT float *iris_js_out_ptr(void)    { return s_out; }
EXPORT float *iris_js_out_b_ptr(void)  { return s_out_b; }
EXPORT float *iris_js_ex_in_ptr(void)  { return s_ex_in; }
EXPORT float *iris_js_ex_out_ptr(void) { return s_ex_out; }
EXPORT unsigned char *iris_js_file_ptr(void) { return g_file; }
EXPORT int iris_js_file_cap(void)      { return (int)sizeof g_file; }

/* n_in is chosen at run time: 2 for a mouse pad, 4 with velocity, 6 for a
   set of audio bands. The arena is sized for the largest case, so switching
   input rigs never allocates anything. Re-init invalidates the B snapshot —
   its dimensions no longer necessarily match. */
EXPORT int iris_js_init(int n_in, int hidden, unsigned int seed) {
  if (n_in < 1)         n_in = 1;
  if (n_in > NI_MAX)    n_in = NI_MAX;
  if (hidden < 1)       hidden = 1;
  if (hidden > HID_MAX) hidden = HID_MAX;
  g_ni = n_in;
  g_nh = hidden;
  g_b_valid = 0;
  g_k = iris_init(g_arena, sizeof g_arena, n_in, hidden, NO, CAP, seed);
  return g_k != 0;
}
EXPORT int iris_js_n_in(void) { return g_ni; }

/* How close is the current input to stored example idx?
   1 = you are standing exactly on it, 0 = as far as the space gets. */
EXPORT float iris_js_similarity(int idx) {
  if (!g_k || idx < 0 || idx >= iris_count(g_k)) return 0.0f;
  float gin[IRIS_MAX_IN], gout[IRIS_MAX_OUT];
  iris_get(g_k, idx, gin, gout);
  float d = 0.0f;
  for (int i = 0; i < g_ni; ++i) {
    float t = iris_norm_in(g_k, i, gin[i]) - iris_norm_in(g_k, i, s_in[i]);
    d += t * t;
  }
  d = iris_sqrt(d) / (iris_sqrt((float)g_ni) * 0.5f);
  return iris_clampf(1.0f - d, 0.0f, 1.0f);
}

EXPORT int   iris_js_record(void)        { return g_k ? iris_record(g_k, s_in, s_out) : -1; }
EXPORT int   iris_js_count(void)         { return g_k ? iris_count(g_k) : 0; }
EXPORT int   iris_js_capacity(void)      { return CAP; }
EXPORT int   iris_js_delete_id(int id)   { return g_k ? iris_delete_id(g_k, id) : 0; }
EXPORT int   iris_js_delete_last(void)   { return g_k ? iris_delete_last(g_k) : 0; }
EXPORT int   iris_js_delete_near(void)   { return g_k ? iris_delete_nearest(g_k, s_in) : 0; }
EXPORT void  iris_js_clear(void)         { if (g_k) iris_clear(g_k); }

/* Read example idx into the example scratch buffers (NOT the live in/out —
   reading a row must never disturb what the instrument is doing right now).
   Returns the row's stable id, 0 on a bad index. */
EXPORT int   iris_js_get(int idx)        { return g_k ? iris_get(g_k, idx, s_ex_in, s_ex_out) : 0; }

EXPORT float iris_js_train(int epochs)   { return g_k ? iris_train_epochs(g_k, epochs) : 1.0f; }
EXPORT float iris_js_reroll(unsigned int seed, int epochs) {
  return g_k ? iris_retrain_new(g_k, seed, epochs) : 1.0f;
}
/* REMOVED 2026-08-27. Exposing lr/momentum to a browser UI is exposing the
   two knobs measured to brick instruments: momentum 0.99 bricked 20 of 40 runs
   at the default lr. See docs/KNOB-AUDIT.md. The internal setter still exists
   for the test suite's Weka-parity check; it is not for users. */

/* --- the instant trainer (the substrate's path, ADR 0008 / 0012) ----------
   A commit, a dissolve, a reroll and an undo each end in one of these; the
   solve is closed-form (~1 ms) and deterministic in seed + dataset, so the
   n-th reroll after a given seed is the same instrument here and on the
   board. Returns ridge doublings (>= 0) or -1 refused (weights untouched). */
#define LAMBDA_DEFAULT 1e-4f
static unsigned char g_elm_scratch[IRIS_ELM_SCRATCH(HID_MAX, NO)];
EXPORT int iris_js_train_elm(float lam) {
  if (!g_k) return -1;
  return iris_train_elm(g_k, lam > 0.0f ? lam : LAMBDA_DEFAULT, g_elm_scratch, sizeof g_elm_scratch);
}
EXPORT int iris_js_reroll_elm(unsigned int seed, float lam) {
  if (!g_k) return -1;
  return iris_retrain_elm_new(g_k, seed, lam > 0.0f ? lam : LAMBDA_DEFAULT, g_elm_scratch, sizeof g_elm_scratch);
}
EXPORT int iris_js_status(void) { return g_k ? (int)iris_get_status(g_k) : -1; }

/* --- the candidate sound (app/core/cand.c, bit-exact) ---------------------
   rng = seed ^ golden*(cand_n); per dimension 0.1 + 0.8*u. The dims are
   drawn in order, so dims 0..2 equal the device's three and dim 3 is the
   shim's extra (aux). Written into s_cand; read through iris_js_cand_ptr. */
static float s_cand[NO];
EXPORT float *iris_js_cand_ptr(void) { return s_cand; }
EXPORT void iris_js_cand(unsigned int cand_n) {
  iris_rng r;
  r.s = (g_k ? iris_seed(g_k) : 1u) ^ (0x9E3779B9u * cand_n);
  for (int d = 0; d < NO; ++d)
    s_cand[d] = 0.1f + 0.8f * (float)iris_rand_u32(&r) / 4294967296.0f;
}

EXPORT void  iris_js_predict(void)       { if (g_k) iris_predict(g_k, s_in, s_out); }
EXPORT float iris_js_novelty(void)       { return g_k ? iris_novelty(g_k, s_in) : 1.0f; }
EXPORT int   iris_js_trained(void)       { return g_k ? iris_is_trained(g_k) : 0; }
EXPORT float iris_js_error(void)         { return g_k ? iris_last_error(g_k) : 1.0f; }
EXPORT unsigned int iris_js_seed(void)   { return g_k ? iris_seed(g_k) : 0; }

EXPORT int   iris_js_save(void)          { return g_k ? (int)iris_save(g_k, g_file, sizeof g_file) : 0; }

/* The authoritative save size, straight from the C. page.html carries a JS
   mirror of this formula for validating a file before it touches wasm; that
   mirror is a drift risk with nothing checking it, so the page now compares
   the two at startup and refuses to run if they disagree. Added 2026-08-26. */
EXPORT int   iris_js_save_size(void)     { return g_k ? (int)iris_save_size(g_k) : 0; }
EXPORT int   iris_js_load(int nbytes)    {
  g_b_valid = 0;   /* a loaded instrument has no "previous model" */
  return g_k ? iris_load(g_k, g_file, (size_t)nbytes) : 0;
}
EXPORT int   iris_js_arena_bytes(void)   { return (int)(sizeof g_arena + sizeof g_arena_b); }

/* --- A/B ------------------------------------------------------------------ */

/* Snapshot the live model into B. Call immediately BEFORE a reroll.
   last_error is not part of the save format, so it is cached here alongside
   the seed for the "prev:" stats line. */
EXPORT int iris_js_ab_snapshot(void) {
  if (!g_k || !iris_is_trained(g_k)) return 0;
  size_t n = iris_save(g_k, g_ab, sizeof g_ab);
  if (!n) return 0;
  g_b = iris_init(g_arena_b, sizeof g_arena_b, g_ni, g_nh, NO, CAP, 1);
  if (!g_b) { g_b_valid = 0; return 0; }
  if (!iris_load(g_b, g_ab, n)) { g_b_valid = 0; return 0; }
  g_b_seed = iris_seed(g_k);
  g_b_err  = iris_last_error(g_k);
  g_b_valid = 1;
  return 1;
}

/* Restore B back into the live model (the "revert" button). */
EXPORT int iris_js_ab_restore(void) {
  if (!g_k || !g_b || !g_b_valid) return 0;
  size_t n = iris_save(g_b, g_ab, sizeof g_ab);
  if (!n) return 0;
  return iris_load(g_k, g_ab, n);
}

/* Predict with the previous model (hold-B audition). Reads s_in like
   iris_js_predict, writes s_out_b instead of s_out. */
EXPORT void iris_js_predict_b(void) {
  if (g_b && g_b_valid) iris_predict(g_b, s_in, s_out_b);
}

EXPORT int          iris_js_b_valid(void) { return g_b_valid; }
EXPORT unsigned int iris_js_b_seed(void)  { return g_b_seed; }
EXPORT float        iris_js_b_err(void)   { return g_b_err; }

/* --- learned ranges -------------------------------------------------------
   iris_predict clamps its outputs to the demonstrated range, so the UI shows
   that range honestly on the target sliders; a near-zero range is a dead
   dimension the UI warns about by name. */
EXPORT float iris_js_in_lo(int i)  { return (g_k && i >= 0 && i < g_ni) ? g_k->in_lo[i]  : 0.0f; }
EXPORT float iris_js_in_hi(int i)  { return (g_k && i >= 0 && i < g_ni) ? g_k->in_hi[i]  : 1.0f; }
EXPORT float iris_js_out_lo(int i) { return (g_k && i >= 0 && i < NO)   ? g_k->out_lo[i] : 0.0f; }
EXPORT float iris_js_out_hi(int i) { return (g_k && i >= 0 && i < NO)   ? g_k->out_hi[i] : 1.0f; }
