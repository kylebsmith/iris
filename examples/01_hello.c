/* ============================================================================
   01_hello.c — teach it three gestures, then play it.

   The whole point of this library in twenty lines. There is no setup, no
   config file, no training script, no model directory. You hand it a block of
   memory, show it a few examples of "when I do THIS, it sounds like THAT", and
   then ask it what to do about gestures you never showed it.

       cc -std=c99 -O2 -Wall -Wextra -I.. -o hello 01_hello.c && ./hello

   No -lm: the library calls nothing, not even the maths library (see the
   zero-dependency note at the top of iris.h). This file uses <stdio.h> only
   to print.
   ============================================================================ */

#include "../iris.h"
#include <stdio.h>

/* 2 sensor inputs, 12 hidden units, 3 sound parameters, room for 64 examples.
   IRIS_ARENA computes the size at compile time, so this is a fixed array in
   static memory: no malloc, and you know the number before you run it. */
static unsigned char memory[IRIS_ARENA(2, 12, 3, 64)];

int main(void) {
  iris *k = iris_init(memory, sizeof memory, 2, 12, 3, 64, /*seed=*/1234);
  if (!k) { printf("iris_init refused the shape\n"); return 1; }

  /* THREE DEMONSTRATIONS. Left hand low -> dark. Middle -> mid. Right high ->
     bright. In a real instrument these two numbers come off a sensor and these
     three come off your synth's panel. */
  struct { float in[2]; float out[3]; } demos[] = {
    { {0.0f, 0.0f}, {0.10f, 0.20f, 0.05f} },   /* dark   */
    { {0.5f, 0.5f}, {0.50f, 0.50f, 0.50f} },   /* mid    */
    { {1.0f, 1.0f}, {0.95f, 0.80f, 0.90f} },   /* bright */
  };
  for (int i = 0; i < 3; ++i)
    if (!iris_record(k, demos[i].in, demos[i].out))
      { printf("refused example %d\n", i); return 1; }

  /* TRAIN. No epoch count to guess: it starts from the seed and stops when
     the error stops improving (or falls below its floor, which with three
     demonstrations is what usually happens first). */
  if (!iris_train(k)) { printf("training refused\n"); return 1; }
  printf("trained: %d epochs, final error %.2e\n\n",
         iris_train_epochs_done(k), iris_last_error(k));

  /* PLAY, including gestures never demonstrated. The interesting rows are
     the ones nobody showed it, (0.25, 0.25) and (0.75, 0.75). */
  printf("  gesture        ->  sound parameters\n");
  const float probes[5][2] = { {0,0}, {0.25f,0.25f}, {0.5f,0.5f}, {0.75f,0.75f}, {1,1} };
  for (int p = 0; p < 5; ++p) {
    float out[3];
    iris_predict(k, probes[p], out);
    printf("  (%.2f, %.2f)   ->  %.3f  %.3f  %.3f%s\n",
           probes[p][0], probes[p][1], out[0], out[1], out[2],
           (p == 1 || p == 3) ? "   <- never demonstrated" : "");
  }
  return 0;
}
