/* The smallest thing that shows what iris does: two demonstrations, then ask
   it about a point in between that you never demonstrated. */
/* Angle brackets, deliberately, and this is the one place they are right.
   This sketch lives inside the library and only exists once the library is
   installed -- it is what appears under File > Examples > iris. The sketches
   a student is handed are different: they carry iris.h beside them and use
   quotes, so nothing has to be installed at all. Two delivery routes, two
   include styles, and mixing them is what makes the promise confusing. */
#include <iris.h>

static unsigned char arena[IRIS_ARENA(1, 12, 1, 8)];

void setup() {
  Serial.begin(115200);
  delay(400);
  iris *k = iris_init(arena, sizeof arena, 1, 12, 1, 8, /*seed=*/1234);
  if (!k) { Serial.println(F("arena too small")); return; }

  float in, out;
  in = 0.0f; out = 0.0f; iris_record(k, &in, &out);   /* here, be silent */
  in = 1.0f; out = 1.0f; iris_record(k, &in, &out);   /* there, be loud  */
  iris_train(k);

  for (int i = 0; i <= 10; ++i) {
    in = i / 10.0f;
    iris_predict(k, &in, &out);
    Serial.print(in, 1); Serial.print(F(" -> ")); Serial.println(out, 3);
  }
}

void loop() {}
