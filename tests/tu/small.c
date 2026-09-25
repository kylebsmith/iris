/* Translation unit that SHRANK the maxima, exactly as iris.h documents for
   small boards: its copies of the library's functions declare working arrays
   such as float x[4]. It is handed an 8-input instrument built by the other
   unit, and calls every entry point that has such an array. Each must refuse
   the instrument (iris_internal_shape_fits) instead of writing eight floats
   into four, and a refusal must leave every byte of the instrument but its
   status as it was. The playing calls must still write their substitute, the
   range centre, so the caller is not left holding stale output. Built under
   AddressSanitizer (sh build.sh tu), an overflow of any of those arrays is a
   report; without the refusal, iris_predict alone writes past float x[4]. */
#define IRIS_MAX_IN  4
#define IRIS_MAX_OUT 4
#define IRIS_MAX_HID 12
#include "../../iris.h"
#include <stdio.h>
#include <string.h>
extern iris *tu_make8(void);
extern size_t tu_arena8(unsigned char **mem);
extern size_t tu_file8(const unsigned char **file);
static unsigned char before[IRIS_ARENA(8, 12, 2, 32)];
static unsigned char scratch[65536];
static int fails = 0;
static void check(const char *name, int ok) {
  printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) fails++;
}
int main(void) {
  iris *k = tu_make8();
  unsigned char *mem;
  const unsigned char *file;
  const size_t bytes = tu_arena8(&mem), fn = tu_file8(&file);
  if (!k || fn == 0 || bytes > sizeof before) {
    printf("  FAIL  the other unit could not build and save the instrument\n");
    return 1;
  }
  memcpy(before, mem, bytes);
  const int32_t st = k->status;
  float in[8], out[2] = { -1.0f, -1.0f };
  for (int j = 0; j < 8; ++j) in[j] = 0.3f;
  /* the substitute: the centre of each demonstrated output range */
  const float c0 = 0.5f * k->out_lo[0] + 0.5f * k->out_hi[0];
  const float c1 = 0.5f * k->out_lo[1] + 0.5f * k->out_hi[1];

  iris_predict(k, in, out);
  check("iris_predict refuses and writes the range centre",
        iris_get_status(k) == IRIS_NOT_FITTED && out[0] == c0 && out[1] == c1);
  out[0] = out[1] = -1.0f;
  iris_knn_predict(k, in, out, 3);
  check("iris_knn_predict refuses and writes the range centre",
        iris_get_status(k) == IRIS_NOT_FITTED && out[0] == c0 && out[1] == c1);
  out[0] = out[1] = -1.0f;
  check("iris_classify_1nn refuses and writes the range centre",
        iris_classify_1nn(k, in, out) == -1 && out[0] == c0 && out[1] == c1);
  check("iris_novelty refuses with -1", iris_novelty(k, in) == -1.0f);
  check("iris_delete_nearest refuses", iris_delete_nearest(k, in) == 0);
  check("iris_train refuses", iris_train(k) == 0);
  check("iris_train_begin refuses", iris_train_begin(k, 0) == 0);
  check("iris_train_slice refuses", iris_train_slice(k, 10) == 0);
  check("iris_continue refuses", iris_continue(k, 10) == -1.0f);
  check("iris_continue_to_plateau refuses", iris_continue_to_plateau(k, 0, 0, 0) == -1.0f);
  check("iris_loo_error refuses", iris_loo_error(k, 10) == -1.0f);
  check("iris_suggest_smoothing refuses", iris_suggest_smoothing(k, scratch, sizeof scratch) == -1.0f);
  check("iris_train_elm refuses", iris_train_elm(k, 1e-4f, scratch, sizeof scratch) == -1);
  check("iris_load refuses", iris_load(k, file, fn) == 0);

  k->status = st;
  check("every refusal left every byte but the status unmoved", memcmp(before, mem, bytes) == 0);
  printf(fails ? "  %d FAILING\n" : "  a shape too big for this unit's arrays is refused everywhere\n", fails);
  return fails ? 1 : 0;
}
