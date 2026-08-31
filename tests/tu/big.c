/* Translation unit with the DEFAULT maxima. Builds a legal 8-input instrument. */
#define IRIS_IMPLEMENTATION
#include "../../iris.h"
static unsigned char ARENA[IRIS_ARENA(8, 12, 2, 32)];
iris *tu_make8(void) {
  iris *k = iris_init(ARENA, sizeof ARENA, 8, 12, 2, 32, 1234u);
  if (!k) return 0;
  for (int i = 0; i < 6; ++i) {
    float in[8], o[2] = { (float)i / 5.0f, 0.25f };
    for (int j = 0; j < 8; ++j) in[j] = (float)(i + j) / 13.0f;
    iris_record(k, in, o);
  }
  iris_train(k);
  return k;
}
