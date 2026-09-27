/* ============================================================================
   04_categories.c — one instrument, read two ways.

   Some sound parameters are continuous. A filter's brightness can take any
   value, and between two takes you want something between their sounds.
   Others are categories. A chord is one of the chords you taught, and
   "halfway between C major and A minor" is a third chord that nobody asked
   for. The network cannot tell the two apart: it stores a category as a
   number and interpolates it like any other.

   SO READ THE INSTRUMENT TWICE. iris_predict for the continuous outputs.
   For the category, the nearest take's own value: iris_nearest names the
   take, and iris_get reads what it holds. Four takes teach a tilt sensor two
   places, C major around 10 to 30 degrees and A minor around 60 to 80, with
   a brightness rising across them. Between the two places the chord as the
   network plays it is a number running from 0 to 5, so it passes through
   Dm, Em, F and G, chords nobody taught; the printout's five-degree steps
   land on three of them. The snapped chord plays only C and Am.

   HYSTERESIS. Snapping alone flickers wherever two takes are equally near,
   because a sensor never holds still. So keep the chord that is playing
   until a take holding another chord is clearly nearer: here, nearer than
   0.8 times the nearest take holding the current one. iris_nearest gives
   every distance in one unit, fractions of each input's range, so the rule
   is one comparison. Going up the chord changes at 50 degrees, going down
   at 40: the printout shows both sweeps side by side.

   THE SAME DISTANCE REFUSES. A reading far from every take is a gesture
   nobody taught; with a threshold on the nearest distance the sketch can
   choose silence instead of the nearest chord (the last line printed).

   The brightness column shows two more things about the network, which
   iris.h PART 6 describes: it passes through each take without being flat
   there, and beyond the outermost takes it holds the end of the range it
   was shown.

       cc -std=c99 -O2 -Wall -Wextra -I.. -o categories 04_categories.c && ./categories
   ============================================================================ */

#include "../iris.h"
#include <stdio.h>

static unsigned char mem[IRIS_ARENA(1, 12, 2, 8)];
static const char *const CHORD[7] = { "C", "Dm", "Em", "F", "G", "Am", "Bdim" };
#define HOLD  0.8f   /* change only when another chord is this much nearer */
#define STEPS 18     /* the sweep, 0 to 90 degrees in steps of 5 */

/* The chord a take holds: its second output. */
static int chord_of(iris *k, int id) {
  float tilt, out[2];
  iris_get(k, iris_index_of(k, id), &tilt, out);
  return (int)out[1];
}

/* The snapped chord with hysteresis. `playing` is the chord playing now, or
   -1 before the first reading. */
static int snap(iris *k, float tilt, int playing, int *id, float *dist) {
  int ids[4];
  float dists[4];
  const int n = iris_nearest(k, &tilt, ids, dists, 4);
  *id = ids[0];
  *dist = dists[0];
  if (playing < 0) return chord_of(k, ids[0]);
  float same = -1.0f;            /* the nearest take holding the chord playing */
  for (int j = 0; j < n; ++j)
    if (chord_of(k, ids[j]) == playing) { same = dists[j]; break; }
  for (int j = 0; j < n; ++j) {  /* the nearest take holding another chord */
    const int c = chord_of(k, ids[j]);
    if (c != playing) return dists[j] < HOLD * same ? c : playing;
  }
  return playing;
}

int main(void) {
  /* tilt in degrees; brightness 0 to 1; the chord as a number, CHORD[] */
  const float take[4][3] = {
    { 10.0f, 0.20f, 0.0f }, { 30.0f, 0.35f, 0.0f },
    { 60.0f, 0.70f, 5.0f }, { 80.0f, 0.90f, 5.0f },
  };
  iris *k = iris_init(mem, sizeof mem, 1, 12, 2, 8, 1234u);
  if (!k) { printf("iris_init refused the shape\n"); return 1; }
  for (int i = 0; i < 4; ++i) iris_record(k, &take[i][0], &take[i][1]);
  if (!iris_train(k)) { printf("training refused\n"); return 1; }

  /* The sweep down first, so both directions print on one line. */
  int down[STEPS + 1], playing = -1, id;
  float dist;
  for (int s = STEPS; s >= 0; --s) playing = down[s] = snap(k, 5.0f * (float)s, playing, &id, &dist);

  printf("  tilt  brightness   chord as played   nearest take    snapped, going up / down\n");
  playing = -1;
  for (int s = 0; s <= STEPS; ++s) {
    const float tilt = 5.0f * (float)s;
    float out[2];
    iris_predict(k, &tilt, out);
    int c = (int)(out[1] + 0.5f);
    c = c < 0 ? 0 : c > 6 ? 6 : c;
    playing = snap(k, tilt, playing, &id, &dist);
    printf("  %4.0f    %.3f     %5.2f  %-4s %s    #%d at %.3f     %-4s %s\n",
           (double)tilt, (double)out[0], (double)out[1], CHORD[c],
           c == 0 || c == 5 ? " " : "*", id, (double)dist, CHORD[playing], CHORD[down[s]]);
  }
  printf("  (* a chord nobody taught)\n\n");

  const float far = 150.0f;
  snap(k, far, -1, &id, &dist);
  printf("  At %.0f degrees the nearest take, #%d, is %.2f of the range away: past a\n"
         "  threshold of 0.5, a gesture nobody taught.\n", (double)far, id, (double)dist);
  return 0;
}
