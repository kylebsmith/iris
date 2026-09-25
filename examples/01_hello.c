/* ============================================================================
   01_hello.c — teach it three gestures, then play it.

   The whole point of this library in twenty lines. There is no setup, no
   config file, no training script, no model directory. You hand it a block of
   memory, show it a few examples of "when I do THIS, it sounds like THAT", and
   then ask it what to do about gestures you never showed it.

       cc -std=c99 -O2 -I.. -o hello 01_hello.c -lm && ./hello

   -lm is for this example's printf-adjacent maths only. The library itself
   calls nothing — see the zero-dependency note at the top of iris.h.
   ============================================================================ */

#include "../iris.h"
#include <stdio.h>

/* 2 sensor inputs, 12 hidden units, 3 sound parameters, room for 64 examples.
   IRIS_ARENA computes the size at compile time, so this is a fixed array in
   .bss — no malloc, and you know the number before you run it. */
static unsigned char memory[IRIS_ARENA(2, 12, 3, 64)];

int main(void) {
  iris *k = iris_init(memory, sizeof memory, 2, 12, 3, 64, /*seed=*/1234);
  if (!k) { printf("arena too small\n"); return 1; }

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

  /* TRAIN. No epoch count to guess: it stops when it stops improving. */
  float err = iris_continue_to_plateau(k, 0, 0, 0);
  printf("trained: %d epochs, final error %.2e\n\n",
         iris_train_epochs_done(k), err);

  /* PLAY — including gestures never demonstrated. The interesting column is
     the middle one: nobody showed it (0.25, 0.25). */
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
