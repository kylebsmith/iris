/* Translation unit with the DEFAULT maxima. It builds, trains and saves a
   legal 8-input instrument, and hands the instrument, its arena and its
   saved file to the other unit. */
#include "../../iris.h"
#include <string.h>
static unsigned char ARENA[IRIS_ARENA(8, 12, 2, 32)];
static unsigned char FILE8[4096];
static size_t file8_n;
iris *tu_make8(void);
size_t tu_arena8(unsigned char **mem);
size_t tu_file8(const unsigned char **file);
iris *tu_make8(void) {
  iris *k = iris_init(ARENA, sizeof ARENA, 8, 12, 2, 32, 1234u);
  if (!k) return 0;
  for (int i = 0; i < 6; ++i) {
    float in[8], o[2] = { (float)i / 5.0f, 0.25f };
    for (int j = 0; j < 8; ++j) in[j] = (float)(i + j) / 13.0f;
    iris_record(k, in, o);
  }
  iris_train(k);
  file8_n = iris_save(k, FILE8, sizeof FILE8);
  return k;
}
size_t tu_arena8(unsigned char **mem) { *mem = ARENA; return sizeof ARENA; }
size_t tu_file8(const unsigned char **file) { *file = FILE8; return file8_n; }

/* An instrument the raised unit (raised.c) made with more outputs than this
   unit's IRIS_MAX_OUT: every call here that has a working array sized by
   the maxima must refuse it instead of running 31 outputs through 16, and
   leave what it saves as it was. `out` holds n_out floats, as the caller's
   array always must, because a refused playing call still writes its
   substitute into every output. */
int tu_default_refuses(iris *k, int n_out);
int tu_default_refuses(iris *k, int n_out) {
  static unsigned char before[8192], after[8192];
  float in[IRIS_MAX_IN] = { 0.0f }, out[64];
  int ids[3]; float dists[3];
  if (n_out > 64) return 0;
  const size_t n = iris_save(k, before, sizeof before);
  iris_predict(k, in, out);
  int ok = iris_get_status(k) == IRIS_NOT_FITTED;
  iris_knn_predict(k, in, out, 3);
  ok = ok && iris_classify_1nn(k, in, out) == -1 && iris_nearest(k, in, ids, dists, 3) == 0
          && iris_train(k) == 0 && iris_copy(k, k) == 0;
  k->status = IRIS_STATUS_OK;
  return ok && n > 0 && iris_save(k, after, sizeof after) == n && memcmp(before, after, n) == 0;
}
