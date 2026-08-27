/* ============================================================================
   00_minimal.c — see the instrument you just made.

   Four demonstrations. One command. What you get back is not four numbers, it
   is a whole continuous space that only four points of were ever specified —
   and that space is the instrument. Everything you play that you did not
   demonstrate lives in the gaps.

   Four points in, a whole surface out. That surface IS the instrument, and you
   never wrote it — you demonstrated the corners and it filled in the rest.
   See 03_reroll.c for the same examples producing a different instrument.

       cc -std=c99 -O2 -Wall -Wextra -I.. -o min 00_minimal.c -lm && ./min
   ============================================================================ */

#include "../iris.h"
#include <stdio.h>

static unsigned char mem[IRIS_ARENA(2, 12, 1, 16)];
static const char *RAMP = " .:-=+*#%@";          /* quiet ......... loud */

/* Draw the learned mapping: sweep the whole 2-D gesture space, ask the
   instrument what it would do at every point, print one character per answer. */
static void render(iris *k, const char *title) {
  printf("  %s\n", title);
  for (int y = 10; y >= 0; --y) {
    printf("    ");
    for (int x = 0; x <= 20; ++x) {
      float in[2] = { (float)x / 20.0f, (float)y / 10.0f }, out;
      iris_predict(k, in, &out);
      int lvl = (int)(out * 9.0f + 0.5f);
      putchar(RAMP[lvl < 0 ? 0 : (lvl > 9 ? 9 : lvl)]);
    }
    putchar('\n');
  }
}

int main(void) {
  iris *k = iris_init(mem, sizeof mem, 2, 12, 1, 16, /*seed=*/7);

  /* FOUR DEMONSTRATIONS — one per corner of the space. That is all. */
  const float demo[4][3] = {          /*  x     y   ->  loudness */
    { 0.0f, 0.0f, 0.05f },            /*  near, low  ->  quiet   */
    { 1.0f, 0.0f, 0.95f },            /*  far,  low  ->  loud    */
    { 0.0f, 1.0f, 0.95f },            /*  near, high ->  loud    */
    { 1.0f, 1.0f, 0.05f },            /*  far,  high ->  quiet   */
  };
  for (int i = 0; i < 4; ++i)
    iris_record(k, demo[i], &demo[i][2]);

  iris_train_converge(k, 0, 0, 0);
  printf("\n  4 demonstrations, %d epochs. 231 points shown, 4 were specified.\n\n",
         iris_train_epochs_done(k));
  render(k, "seed 7");

  /* And it is hard to misuse: a glitched sensor frame is refused, not absorbed. */
  { float bad[2] = { 0.0f / 0.0f, 0.5f }, any = 0.5f;
    printf("\n  NaN input -> record returns %d, status %d, still %d examples\n",
           iris_record(k, bad, &any), (int)iris_get_status(k), iris_count(k)); }
  return 0;
}
