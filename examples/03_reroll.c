/* ============================================================================
   03_reroll.c — same demonstrations, different instrument.

   Laetitia Sonami: "I may go through hundreds of models before settling to a
   particular 'palette' for a particular piece."
     -- Rebecca Fiebrink and Laetitia Sonami, "Reflections on Eight Years of
        Instrument Creation with Machine Learning", Proceedings of the
        International Conference on New Interfaces for Musical Expression
        (NIME 2020), Birmingham, pp. 237-242.
   A hand-written mapping cannot offer that. You wrote it; it is what you
   wrote. Training can, because the search has a starting point, and moving
   the starting point moves everything you did not pin down.

   A REROLL IS TWO CALLS: iris_reseed(k, seed) picks new random starting
   weights, and iris_train(k) fits the same demonstrations from them.

   BUT NOT EVERY TRAINER MOVES AS FAR. This program prints the mean absolute
   difference between seed 7 and seed 99 over a 101 x 101 grid of the gesture
   square, for both trainers: about 0.024 for iris_train and 0.155 for
   iris_train_elm, six times more. Training to a plateau drives toward the
   same surface whatever the seed, so the randomness gets washed out on the
   way. The closed-form trainer (ELM) freezes its hidden layer instead of
   training it, so a new seed really is a new instrument. If you want the
   reroll to mean something, reroll the closed-form trainer: iris_reseed,
   then iris_train_elm.

       cc -std=c99 -O2 -Wall -Wextra -I.. -o reroll 03_reroll.c && ./reroll
   ============================================================================ */

#include "../iris.h"
#include <stdio.h>

#define SEEDS 3
static unsigned char mem[SEEDS][IRIS_ARENA(2, 12, 1, 16)];
static unsigned char scratch[IRIS_ELM_SCRATCH(12, 1)];   /* the solve's workspace */
static const char *RAMP = " .:-=+*#%@";
static const uint32_t seeds[SEEDS] = { 7, 99, 4242 };

/* The three instruments side by side, one character per point of the
   gesture square. */
static void render(iris *k[SEEDS], const char *title) {
  printf("  %s\n", title);
  for (int y = 10; y >= 0; --y) {
    printf("    ");
    for (int s = 0; s < SEEDS; ++s) {
      if (s > 0) printf("    ");
      for (int x = 0; x <= 20; ++x) {
        float in[2] = { (float)x / 20.0f, (float)y / 10.0f }, out;
        iris_predict(k[s], in, &out);
        int l = (int)(out * 9.0f + 0.5f);
        putchar(RAMP[l < 0 ? 0 : (l > 9 ? 9 : l)]);
      }
    }
    putchar('\n');
  }
}

/* Mean absolute difference between two instruments over a 101 x 101 grid. */
static double difference(iris *a, iris *b) {
  double sum = 0.0;
  for (int y = 0; y <= 100; ++y)
    for (int x = 0; x <= 100; ++x) {
      float in[2] = { (float)x / 100.0f, (float)y / 100.0f }, oa, ob;
      iris_predict(a, in, &oa);
      iris_predict(b, in, &ob);
      sum += oa > ob ? oa - ob : ob - oa;
    }
  return sum / (101.0 * 101.0);
}

int main(void) {
  const float demo[4][3] = {
    { 0.0f, 0.0f, 0.05f }, { 1.0f, 0.0f, 0.95f },
    { 0.0f, 1.0f, 0.95f }, { 1.0f, 1.0f, 0.05f },
  };
  iris *k[SEEDS];

  for (int s = 0; s < SEEDS; ++s) {
    k[s] = iris_init(mem[s], sizeof mem[s], 2, 12, 1, 16, 1u);
    if (!k[s]) { printf("iris_init refused the shape\n"); return 1; }
    for (int i = 0; i < 4; ++i) iris_record(k[s], demo[i], &demo[i][2]);
  }

  /* THE REROLL, with the network trained to its plateau. */
  for (int s = 0; s < SEEDS; ++s) {
    iris_reseed(k[s], seeds[s]);                 /* new starting weights */
    if (!iris_train(k[s])) { printf("training refused\n"); return 1; }
  }
  render(k, "iris_train, seeds 7, 99 and 4242, identical demonstrations");
  printf("  mean difference, seed 7 against seed 99: %.4f\n\n", difference(k[0], k[1]));

  /* THE SAME REROLL, with the closed-form trainer. */
  for (int s = 0; s < SEEDS; ++s) {
    iris_reseed(k[s], seeds[s]);                 /* a new frozen random layer */
    if (iris_train_elm(k[s], 1e-4f, scratch, sizeof scratch) < 0) {
      printf("the closed-form trainer refused\n"); return 1;
    }
  }
  render(k, "iris_train_elm, the same seeds and demonstrations");
  printf("  mean difference, seed 7 against seed 99: %.4f\n\n", difference(k[0], k[1]));

  printf("  Four demonstrations, unchanged. Six instruments.\n");
  return 0;
}
