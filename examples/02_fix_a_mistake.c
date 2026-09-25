/* ============================================================================
   02_fix_a_mistake.c — the thing this library is actually for.

   People who build instruments with tools like this spend their time on the
   demonstrations, not on the algorithm's settings. In the composers' study
   of Fiebrink, Cook and Trueman, "Human model evaluation in interactive
   supervised learning" (the conference on human factors in computing
   systems, CHI 2011), none of the composers used cross-validation (a
   statistical score of the model); they judged the instrument by playing it
   and repaired it by changing its demonstrations. So the library is built
   around editing demonstrations, and this example is that loop end to end:
   record, train, spot the bad take, delete it, train again.

   It also shows the two guards that make the loop safe to be inside of:
   a glitched sensor frame is refused at the door, and an instrument that has
   never been fitted plays a neutral value rather than noise.

       cc -std=c99 -O2 -Wall -Wextra -I.. -o fix 02_fix_a_mistake.c && ./fix
   ============================================================================ */

#include "../iris.h"
#include <math.h>     /* NAN, not-a-number: a glitched sensor reading */
#include <stdio.h>

static unsigned char memory[IRIS_ARENA(2, 12, 3, 64)];
static const float probe[2] = { 0.60f, 0.40f };

/* The status values by name, for printing: the header keeps no table of
   names, so a program that prints a status carries its own. The meaning of
   each is beside its value in the header, at the iris_status enum. */
static const char *status_name(iris_status s) {
  switch (s) {
    case IRIS_STATUS_OK:         return "IRIS_STATUS_OK";
    case IRIS_TRAINING_DIVERGED: return "IRIS_TRAINING_DIVERGED";
    case IRIS_NAN_TRAPPED:       return "IRIS_NAN_TRAPPED";
    case IRIS_RIDGE_ESCALATED:   return "IRIS_RIDGE_ESCALATED";
    case IRIS_NOT_FITTED:        return "IRIS_NOT_FITTED";
    case IRIS_DIVERGED_STUCK:    return "IRIS_DIVERGED_STUCK";
    case IRIS_STORE_FULL:        return "IRIS_STORE_FULL";
    case IRIS_SOLVE_COLLAPSED:   return "IRIS_SOLVE_COLLAPSED";
  }
  return "an unknown status";
}

static void play(iris *k, const char *label) {
  float out[3];
  iris_predict(k, probe, out);
  printf("  %-26s %.3f  %.3f  %.3f   (%s)\n",
         label, out[0], out[1], out[2], status_name(iris_get_status(k)));
}

int main(void) {
  iris *k = iris_init(memory, sizeof memory, 2, 12, 3, 64, 1234);
  if (!k) { printf("iris_init refused the shape\n"); return 1; }

  /* 1. IT DOES NOT PLAY NOISE BEFORE IT HAS LEARNED ANYTHING.
        Status 4 is IRIS_NOT_FITTED, and the outputs are the centre of what
        was demonstrated, 0 with nothing recorded. Without this guard you
        would get the network's answer from random weights: plausible
        numbers, no symptom. */
  play(k, "before any training:");

  /* 2. Fourteen good demonstrations along a diagonal.
        Why fourteen and not six: the residual ledger in step 5 refuses to
        accuse anything below IRIS_STRESS_MIN_EX (12) demonstrations, because
        with fewer than that the ranking is not meaningful. It returns -1
        instead of guessing. */
  for (int i = 0; i < 14; ++i) {
    float u = (float)i / 13.0f;
    float in[2] = { u, 1.0f - u };
    float out[3] = { u, 0.5f, 1.0f - u };
    iris_record(k, in, out);
  }

  /* 3. A GLITCHED SENSOR FRAME IS REFUSED AT THE DOOR.
        It never enters the store, so it can never poison a fit. */
  { const float bad[2] = { NAN, 0.5f }, any[3] = { 0.5f, 0.5f, 0.5f };
    int id = iris_record(k, bad, any);
    const char *status = status_name(iris_get_status(k));
    int count = iris_count(k);
    printf("  glitched frame -> id %d, %s, still %d demonstrations\n\n",
           id, status, count); }

  if (!iris_train(k)) { printf("training refused\n"); return 1; }
  play(k, "trained on 14 good takes:");

  /* 4. NOW THE MISTAKE. A fifteenth demonstration that contradicts the
        others: the take where your hand slipped. Training on it pulls the
        mapping toward it, and here it pulls hard enough that a weight runs
        into the safety limit: status 1, IRIS_TRAINING_DIVERGED. */
  { float in[2] = { 0.60f, 0.40f }, out[3] = { 0.05f, 0.95f, 0.05f };
    iris_record(k, in, out); }
  if (!iris_train(k)) { printf("training refused\n"); return 1; }
  play(k, "after the bad take:");

  /* 5. THE LIBRARY TELLS YOU WHICH ONE IS WRONG. It ranks every
        demonstration by how hard it fought the others during training, and
        hands back the identifier. Below IRIS_STRESS_FLAG the ranking is a
        hint to listen again rather than an accusation; a sketch would show
        it quietly. */
  float margin = 0.0f;
  int id = iris_worst_example_id(k, &margin);
  if (id < 0) { printf("\n  the ledger declined to accuse (needs >= %d demonstrations)\n",
                       IRIS_STRESS_MIN_EX); return 1; }
  printf("\n  the residual ledger says: id %d is fighting the others"
         " (margin %.2f, flag threshold %.2f)\n", id, (double)margin, (double)IRIS_STRESS_FLAG);
  if (margin < IRIS_STRESS_FLAG)
    printf("  below the flag threshold, so a hint to listen to that take again, not an\n"
           "  accusation; here we recorded the bad take ourselves, so it goes\n");

  /* 6. DELETE IT, AND TRAIN AGAIN. The delete removes the take from the
        store, but not from the weights: they were bent by it, and here one
        of them sits on the safety limit. A warm trainer, which carries on
        from the weights it has, refuses them outright (status 5,
        IRIS_DIVERGED_STUCK), and even without the pinned weight it would
        keep the take's influence. */
  iris_delete_id(k, id);
  { float e = iris_continue(k, 100);
    const char *status = status_name(iris_get_status(k));
    printf("  deleted id %d, %d demonstrations remain\n", id, iris_count(k));
    printf("  iris_continue (warm) returns %.0f, %s\n", (double)e, status); }

  /* 7. THE CURE IS iris_train. It starts over from the instrument's own seed
        and fits only the demonstrations stored now, so the deleted take
        leaves nothing behind: the same seed and the same fourteen takes give
        the same weights, bit for bit, as the first iris_train above. */
  if (!iris_train(k)) { printf("training refused\n"); return 1; }
  play(k, "after iris_train:");
  printf("\n  (compare the \"trained on 14 good takes\" row with the last row:"
         " the instrument came back.)\n");
  return 0;
}
