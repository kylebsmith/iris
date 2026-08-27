/* 00_minimal.c — the whole library in nine lines of body.
   cc -std=c99 -O2 -I.. -o min 00_minimal.c -lm && ./min                    */
#include "../iris.h"
#include <stdio.h>

static unsigned char mem[IRIS_ARENA(1, 8, 1, 16)];   /* 1 in, 8 hidden, 1 out */

int main(void) {
  iris *k = iris_init(mem, sizeof mem, 1, 8, 1, 16, /*seed=*/1);
  float in, out;
  in = 0.0f; out = 0.0f; iris_record(k, &in, &out);         /* show it two   */
  in = 1.0f; out = 1.0f; iris_record(k, &in, &out);         /* demonstrations */
  iris_train_converge(k, 0, 0, 0);                          /* learn         */
  in = 0.5f; iris_predict(k, &in, &out);                    /* ask about one */
  printf("never demonstrated 0.5 -> %.3f\n", out);          /* it never saw  */

  /* and it is hard to misuse: bad input is refused, not absorbed. */
  in = 0.0f / 0.0f; out = 0.5f;
  printf("NaN input          -> record returns %d, status %d, %d examples\n",
         iris_record(k, &in, &out), (int)iris_get_status(k), iris_count(k));
  return 0;
}
