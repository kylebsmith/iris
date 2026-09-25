/* experiment.c — WHEN does rerolling actually give you a different instrument?
 *
 * The audit found that with 20 examples and 800 epochs, eight different random
 * starts converge to nearly the same instrument. The data pins the network down
 * and the randomness gets trained away.
 *
 * Sonami says she goes through hundreds of models before one sticks. If that is
 * real, there must be a regime where the starting point still matters. This maps
 * out where that regime is.
 *
 * Build: cc -O2 -o experiment experiment.c -lm && ./experiment
 */

#include "../iris.h"
#include <stdio.h>

#define NI 2
#define NO 3
#define CAP 128
#define SEEDS 12

static unsigned char arena[IRIS_ARENA(NI, 64, NO, CAP)];

static void truth(float x, float y, float *o) {
  o[0] = 0.5f + 0.45f * iris_internal_tanh(3.0f * (x - 0.5f));
  o[1] = 0.5f + 0.40f * iris_internal_tanh(2.5f * (y - 0.5f) * (x + 0.3f));
  o[2] = 0.2f + 0.6f  * (x * y);
}

/* Fill the model with n examples drawn from a scattered but repeatable set. */
static void load_examples(iris *k, int n) {
  iris_clear(k);
  for (int i = 0; i < n; ++i) {
    float u = (float)((i * 7919) % 97) / 97.0f;
    float v = (float)((i * 6131) % 89) / 89.0f;
    float in[NI] = { u, v }, out[NO];
    truth(u, v, out);
    iris_record(k, in, out);
  }
}

/* Train the same examples from SEEDS different starting points, then measure
 * how much the resulting instruments disagree. Reported separately for probes
 * that sit ON demonstrated ground vs probes out in the gaps. */
static void reroll_spread(iris *k, int n_ex, int epochs,
                          float *near_spread, float *far_spread,
                          float *train_err) {
  static float pred[SEEDS][441][NO];
  float errs = 0.0f;

  for (int s = 0; s < SEEDS; ++s) {
    load_examples(k, n_ex);
    errs += iris_retrain_new(k, 1000u + (uint32_t)s * 7919u, epochs);
    int p = 0;
    for (int a = 0; a <= 20; ++a) for (int b = 0; b <= 20; ++b, ++p) {
      float in[NI] = { a / 20.0f, b / 20.0f };
      iris_predict(k, in, pred[s][p]);
    }
  }
  *train_err = errs / SEEDS;

  /* how far is each probe from the nearest demonstration? */
  load_examples(k, n_ex);
  float nov[441];
  int p = 0;
  for (int a = 0; a <= 20; ++a) for (int b = 0; b <= 20; ++b, ++p) {
    float in[NI] = { a / 20.0f, b / 20.0f };
    nov[p] = iris_novelty(k, in);
  }

  float sn = 0.0f, sf = 0.0f; int cn = 0, cf = 0;
  for (p = 0; p < 441; ++p) {
    float worst = 0.0f;
    for (int i = 0; i < SEEDS; ++i) for (int j = i + 1; j < SEEDS; ++j)
      for (int o = 0; o < NO; ++o) {
        float d = iris_internal_absf(pred[i][p][o] - pred[j][p][o]);
        if (d > worst) worst = d;
      }
    if (nov[p] < 0.15f) { sn += worst; cn++; }
    else if (nov[p] > 0.35f) { sf += worst; cf++; }
  }
  *near_spread = cn ? sn / cn : -1.0f;
  *far_spread  = cf ? sf / cf : -1.0f;   /* -1 = no such region exists */
}

int main(void) {
  printf("\nHOW MUCH DOES REROLLING CHANGE THE INSTRUMENT?\n");
  printf("Same examples, %d different random starts. Numbers are how far apart\n", SEEDS);
  printf("the resulting sounds are, on a 0-1 parameter scale.\n\n");

  const int hids[]   = { 4, 12, 32 };
  const int exs[]    = { 5, 10, 20, 40 };
  const int epochs[] = { 100, 400, 1600 };

  printf("  hidden  examples  epochs   near demos   in the gaps   fit error\n");
  printf("  ------  --------  ------   ----------   -----------   ---------\n");

  for (int h = 0; h < 3; ++h) {
    iris *k = iris_init(arena, sizeof arena, NI, hids[h], NO, CAP, 1);
    for (int e = 0; e < 4; ++e) {
      for (int p = 0; p < 3; ++p) {
        float nr, fr, te;
        reroll_spread(k, exs[e], epochs[p], &nr, &fr, &te);
        char nb[16], fb[16];
        if (nr < 0.0f) snprintf(nb, sizeof nb, "%10s", "--"); else snprintf(nb, sizeof nb, "%10.4f", nr);
        if (fr < 0.0f) snprintf(fb, sizeof fb, "%11s", "no gaps"); else snprintf(fb, sizeof fb, "%11.4f", fr);
        printf("  %6d  %8d  %6d   %s   %s   %9.5f%s\n",
               hids[h], exs[e], epochs[p], nb, fb, te,
               fr > 0.06f ? "   <-- lively" : "");
      }
    }
    printf("\n");
  }

  printf("READ THIS AS:\n");
  printf("  'near demos'  = how much the sound differs where you DID demonstrate.\n");
  printf("                  Small is good. It means retraining keeps your work.\n");
  printf("  'in the gaps' = how much it differs where you did NOT demonstrate.\n");
  printf("                  This is the part you are rerolling for.\n\n");
  return 0;
}
