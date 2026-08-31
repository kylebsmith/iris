/* Translation unit that SHRANK the maxima, exactly as iris.h documents for
   small boards. Its copy of iris_predict declares float x[4]. It is handed an
   8-input instrument built by the other unit.

   Before iris_shape_fits, this wrote 8 floats into that 4-float array:
   AddressSanitizer "stack-buffer-overflow, WRITE of size 4, [32,48) 'x.i'".
   The check iris.h names runs in the unit that CALLS iris_init -- the one with
   the large maxima -- so it passed and this unit corrupted its own stack. */
#define IRIS_MAX_IN  4
#define IRIS_MAX_OUT 4
#define IRIS_MAX_HID 12
#define IRIS_IMPLEMENTATION
#include "../../iris.h"
#include <stdio.h>
extern iris *tu_make8(void);
int main(void) {
  iris *k = tu_make8();
  if (!k) { printf("  FAIL  the other unit could not build the instrument\n"); return 1; }
  float in[8], out[2] = { -1.0f, -1.0f };
  for (int j = 0; j < 8; ++j) in[j] = 0.3f;
  iris_predict(k, in, out);                 /* must refuse, not overflow */
  int refused = (iris_get_status(k) == IRIS_NOT_FITTED);
  int wrote   = (out[0] != -1.0f);          /* and not leave stale audio */
  printf("  [%s] a shape too big for THIS unit's arrays is refused   "
         "status %d, out %.4f\n",
         (refused && wrote) ? "PASS" : "FAIL",
         (int)iris_get_status(k), (double)out[0]);
  return (refused && wrote) ? 0 : 1;
}
