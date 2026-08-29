/* ============================================================================
   02_fix_a_mistake.c — the thing this library is actually for.

   Every account of how musicians use tools like this says the same thing: they
   do not tune the algorithm, they FIX THE EXAMPLES. Fiebrink's 2011 study found
   composers never once used cross-validation; they deleted the bad take and
   recorded it again. So the library is built around editing demonstrations, and
   this example is that loop end to end.

   It also shows the two guards that make the loop safe to be inside of:
   a glitched sensor frame is refused at the door, and an instrument that has
   never been fitted refuses to play rather than emitting noise.

       cc -std=c99 -O2 -I.. -o fix 02_fix_a_mistake.c -lm && ./fix
   ============================================================================ */

#include "../iris.h"
#include <stdio.h>

static unsigned char memory[IRIS_ARENA(2, 12, 3, 64)];
static const float probe[2] = { 0.60f, 0.40f };

static void play(iris *k, const char *label) {
  float out[3];
  iris_predict(k, probe, out);
  printf("  %-26s %.3f  %.3f  %.3f   (status %d)\n",
         label, out[0], out[1], out[2], (int)iris_get_status(k));
}

int main(void) {
  iris *k = iris_init(memory, sizeof memory, 2, 12, 3, 64, 1234);

  /* 1. IT REFUSES TO PLAY BEFORE IT HAS LEARNED ANYTHING.
        Status 4 is IRIS_NOT_FITTED. Without this guard you would get the
        forward pass over random weights: plausible numbers, no symptom. */
  play(k, "before any training:");

  /* 2. FOURTEEN honest demonstrations along a diagonal.
        Why fourteen and not six: the residual ledger in step 5 refuses to
        accuse anything below IRIS_STRESS_MIN_EX (12) demonstrations, because
        with fewer than that the ranking is not meaningful. It returns -1
        instead of guessing. Ask it too early and it will say nothing — which
        is correct, and is the kind of refusal you want. */
  for (int i = 0; i < 14; ++i) {
    float u = (float)i / 13.0f;
    float in[2] = { u, 1.0f - u };
    float out[3] = { u, 0.5f, 1.0f - u };
    iris_record(k, in, out);
  }

  /* 3. A GLITCHED SENSOR FRAME IS REFUSED AT THE DOOR.
        It never enters the store, so it can never poison a fit. */
  { float bad[2] = { 0.0f / 0.0f, 0.5f }, any[3] = { 0.5f, 0.5f, 0.5f };
    int id = iris_record(k, bad, any);
    printf("  glitched frame -> id %d, status %d, still %d examples\n\n",
           id, (int)iris_get_status(k), iris_count(k)); }

  iris_train(k);
  play(k, "trained on 14 good demos:");

  /* 4. NOW THE MISTAKE. A seventh demonstration that contradicts the others —
        the take where your hand slipped. */
  { float in[2] = { 0.60f, 0.40f }, out[3] = { 0.05f, 0.95f, 0.05f };
    iris_record(k, in, out); }
  iris_train(k);
  play(k, "after the bad take:");

  /* 5. THE LIBRARY TELLS YOU WHICH ONE IS WRONG. It ranks every demonstration
        by how hard it is fighting the others, and hands back the id. */
  { float margin = 0.0f;
    int id = iris_worst_example_id(k, &margin);
    if (id < 0) { printf("\n  ledger declined to accuse (needs >= %d examples)\n",
                         IRIS_STRESS_MIN_EX); return 1; }
    printf("\n  the residual ledger says: example id %d is fighting the others"
           " (margin %.2f, flag threshold %.2f)\n", id, margin, (double)IRIS_STRESS_FLAG);

    /* 6. DELETE IT — and now the part worth knowing.
          A contradictory demonstration can DIVERGE the weights: they run past
          the safety limit, the guard clamps them and stops. The examples are
          fine. The weights are not, and no amount of retraining fixes them,
          because one clamped weight trips the guard again on the first epoch.
          So the library refuses, with a status you can act on, instead of
          quietly doing nothing. */
    iris_delete_id(k, id);
    int ok = iris_train(k);
    printf("  deleted id %d, %d examples remain\n", id, iris_count(k));
    printf("  re-fit %s, status %d%s\n\n", ok ? "worked" : "was REFUSED", (int)iris_get_status(k),
           iris_get_status(k) == IRIS_DIVERGED_STUCK
             ? "  <- IRIS_DIVERGED_STUCK: reroll to recover" : ""); }

  /* 7. THE CURE. Re-fit the same demonstrations from a fresh random start.
        Your examples are untouched; only the damaged weights are discarded. */
  if (iris_get_status(k) == IRIS_DIVERGED_STUCK) {
    iris_retrain_new(k, 1234, 600);
    play(k, "after iris_retrain_new:");
  } else {
    play(k, "after deleting it:");
  }
  printf("\n  (compare row 2 and the last row — the instrument came back.)\n");
  return 0;
}
