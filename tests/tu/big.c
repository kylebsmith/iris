/* Translation unit with the DEFAULT maxima. It builds, trains and saves a
   legal 8-input instrument, and hands the instrument, its arena and its
   saved file to the other unit. */
#include "../../iris.h"
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
