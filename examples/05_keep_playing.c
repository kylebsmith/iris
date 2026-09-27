/* ============================================================================
   05_keep_playing.c — keep playing while a new fit trains.

   iris_train starts over from the seed. A take you deleted must leave
   nothing of itself in the weights, and starting again is the only way to be
   sure of that. So an instrument being retrained does not play its old fit
   while the run goes on: it plays whatever the run has reached, random
   weights first and then the new fit taking shape. A sliced run
   (iris_train_begin, then iris_train_slice from the main loop) keeps the
   sketch responsive, but what it plays meanwhile is that half-trained
   network.

   THE REMEDY IS A SECOND INSTRUMENT. Play one and train the other:

     1. iris_copy(learn, play)       the learner starts as the player
     2. record the new take into the learner
     3. iris_train_begin(learn, 0), then iris_train_slice(learn, ...) one
        slice at a time, while the sound keeps calling iris_predict(play)
     4. when the run has finished, iris_copy(play, learn)

   iris_copy is iris_save followed by iris_load, with no buffer in between,
   so step 4 hands the player exactly the instrument that trained, and
   neither step needs a file. The price is a second arena of the same shape.

   The printout follows the centre of the gesture square, where the new take
   lands, through the run: what the player plays there (the old fit, steady)
   and what the learner plays (what the player would play if it were being
   retrained itself).

       cc -std=c99 -O2 -Wall -Wextra -I.. -o keep_playing 05_keep_playing.c && ./keep_playing
   ============================================================================ */

#include "../iris.h"
#include <stdio.h>

static unsigned char play_mem[IRIS_ARENA(2, 12, 1, 16)];
static unsigned char learn_mem[IRIS_ARENA(2, 12, 1, 16)];

int main(void) {
  const float corner[4][3] = {
    { 0.0f, 0.0f, 0.1f }, { 1.0f, 0.0f, 0.5f },
    { 0.0f, 1.0f, 0.5f }, { 1.0f, 1.0f, 0.9f },
  };
  const float centre[3] = { 0.5f, 0.5f, 0.95f };   /* the new take */
  float p, l;

  iris *play  = iris_init(play_mem,  sizeof play_mem,  2, 12, 1, 16, 1234u);
  iris *learn = iris_init(learn_mem, sizeof learn_mem, 2, 12, 1, 16, 1234u);
  if (!play || !learn) { printf("iris_init refused the shape\n"); return 1; }
  for (int i = 0; i < 4; ++i) iris_record(play, corner[i], &corner[i][2]);
  if (!iris_train(play)) { printf("training refused\n"); return 1; }
  iris_predict(play, centre, &p);
  printf("  The player, trained on four corners, plays %.3f at the centre.\n", (double)p);

  /* 1 and 2: the learner starts as the player and takes the new take. */
  if (!iris_copy(learn, play)) { printf("iris_copy refused\n"); return 1; }
  iris_record(learn, centre, &centre[2]);
  printf("  A new take asks for %.3f there. The learner trains; the player plays.\n\n",
         (double)centre[2]);

  /* 3: the learner trains a slice at a time; the player keeps playing. */
  if (!iris_train_begin(learn, 0)) { printf("training refused\n"); return 1; }
  printf("  slice   epochs   player plays   learner plays\n");
  int more = 1;
  for (int slice = 0; more; ++slice) {
    if (slice > 0) more = iris_train_slice(learn, 40);
    iris_predict(play, centre, &p);
    iris_predict(learn, centre, &l);
    if (slice <= 5 || slice % 5 == 0 || !more)
      printf("  %5d   %6d          %.3f           %.3f\n",
             slice, iris_train_epochs_done(learn), (double)p, (double)l);
  }

  /* 4: the run has finished, and the player takes the new fit. */
  if (!iris_is_trained(learn)) { printf("the learner did not fit\n"); return 1; }
  if (!iris_copy(play, learn)) { printf("iris_copy refused\n"); return 1; }
  iris_predict(play, centre, &p);
  iris_predict(learn, centre, &l);
  printf("\n  After iris_copy(play, learn) the player plays %.3f at the centre,\n"
         "  %s the learner, and holds %d takes.\n",
         (double)p, p == l ? "to the bit what is played by" : "NOT what is played by",
         iris_count(play));
  return p == l ? 0 : 1;
}
