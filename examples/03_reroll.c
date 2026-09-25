/* ============================================================================
   03_reroll.c — same demonstrations, different instrument.

   Laetitia Sonami: "I may go through hundreds of models before settling to a
   particular 'palette' for a particular piece."
     -- Rebecca Fiebrink and Laetitia Sonami, "Reflections on Eight Years of
        Instrument Creation with Machine Learning", Proceedings of the
        International Conference on New Interfaces for Musical Expression
        (NIME 2020), Birmingham, pp. 237-242.
   A hand-written mapping cannot offer that.
   You wrote it; it is what you wrote. Training can, because the search has a
   starting point, and moving the starting point moves everything you did not
   pin down.

   BUT NOT EVERY TRAINER. Measured 2026-08-27 on this task, mean absolute
   difference across the whole space between seed 7 and seed 99:

       iris_continue_to_plateau (backprop to a plateau) : 0.0227
       iris_train_elm      (frozen random layer)   : 0.1468   <- 6.5x more

   Training to a plateau drives toward the same surface whatever the seed — the
   randomness gets washed out on the way. ELM freezes its hidden layer instead
   of training it, so a new seed really is a new instrument. If you want the
   reroll to mean something, reroll the ELM.

       cc -std=c99 -O2 -Wall -Wextra -I.. -o reroll 03_reroll.c -lm && ./reroll
   ============================================================================ */

#include "../iris.h"
#include <stdio.h>

static unsigned char mem[IRIS_ARENA(2, 12, 1, 16)];
static unsigned char scratch[IRIS_ELM_SCRATCH(12, 1)];   /* ELM needs workspace */
static const char *RAMP = " .:-=+*#%@";

static void render(iris *k, const char *title) {
  printf("  %s\n", title);
  for (int y = 10; y >= 0; --y) {
    printf("    ");
    for (int x = 0; x <= 20; ++x) {
      float in[2] = { (float)x / 20.0f, (float)y / 10.0f }, out;
      iris_predict(k, in, &out);
      int l = (int)(out * 9.0f + 0.5f);
      putchar(RAMP[l < 0 ? 0 : (l > 9 ? 9 : l)]);
    }
    putchar('\n');
  }
}

int main(void) {
  const float demo[4][3] = {
    { 0.0f, 0.0f, 0.05f }, { 1.0f, 0.0f, 0.95f },
    { 0.0f, 1.0f, 0.95f }, { 1.0f, 1.0f, 0.05f },
  };
  const uint32_t seeds[3] = { 7, 99, 4242 };

  for (int s = 0; s < 3; ++s) {
    iris *k = iris_init(mem, sizeof mem, 2, 12, 1, 16, seeds[s]);
    for (int i = 0; i < 4; ++i) iris_record(k, demo[i], &demo[i][2]);

    int rc = iris_retrain_elm_new(k, seeds[s], 1e-4f, scratch, sizeof scratch);
    if (rc < 0) { printf("  ELM refused (%d)\n", rc); return 1; }

    char t[64];
    snprintf(t, sizeof t, "seed %u  —  identical demonstrations", seeds[s]);
    render(k, t);
    putchar('\n');
  }
  printf("  Four demonstrations, unchanged. Three instruments.\n");
  return 0;
}
