/* SPDX-License-Identifier: BSD-3-Clause
   Copyright (c) 2026 Kyle Smith */
/* ============================================================================
   iris_source.h  —  where the feature vector comes from
   v0.1.0 · C99 · no dependencies · no malloc · no libc · no hardware

   One type, one function pointer. It shares its error vocabulary, its iris_desc
   and its [0,1] contract with iris_sink.h, which is why it includes it; see
   the note at the top of that file for why the shared half lives there.

   D7 LIVES IN THIS FILE. iris_source is the only thing in the system that knows
   a sensor exists. Swapping the part rewrites one file under ports/ and
   changes one line at the wiring site — the line naming the open function.
   Nothing above it can tell, because there is nothing above it to tell with:
   no part number, no units, no register map, no bus, no pins, and not even a
   feature count, which is s->n and is set by the port.

   TWO SENSORS: CALL BOTH. There is deliberately no mux type, because

       iris_source_read(&touch.base, v,     touch.base.n);
       iris_source_read(&imu.base,   v + 3, imu.base.n);

   is already everything a mux does, keeps each sensor's error code separate
   instead of merging them into one useless status, and gives hold-last-value
   for free. A mux would be a type whose only job is to lose information.
   ============================================================================ */

#ifndef IRIS_SOURCE_H
#define IRIS_SOURCE_H

#include "iris_sink.h"

#define IRIS_SOURCE_MAX_FEAT IRIS_MAX_IN    /* one authority, no restatement */

/* --------------------------------------------------------------------------
   read() is NON-BLOCKING and returns:

        n   a fresh frame was written
        0   no new frame — nothing has changed since last time. The caller
            HOLDS its previous values, which is the correct behaviour for a
            sensor that is simply idle, and costs nothing.
      < 0   the sensor is broken, or absent, and says which.

   Concrete sources put an iris_source as their FIRST member, exactly as sinks
   do, and for the same reason.
   -------------------------------------------------------------------------- */
typedef struct iris_source {
  const char    *name;
  int            n;
  const iris_desc *desc;
  iris_budget      budget;
  int (*read)(struct iris_source *s, float *v);
} iris_source;

/* On IRIS_E_ABSENT the wrapper PINS the slice to the declared defaults and
   STILL reports the absence. This is deliberate. Dropping or reshaping an
   absent source's slice would silently change n_in, and a trained model would
   then no longer match its own save file — the instrument would keep working,
   but wrong. A stated default is a reading; silence is not. */
IRIS_MUST_CHECK IRIS_IO_API int iris_source_read(iris_source *s, float *v, int n) {
  if (!s || !s->read || !v) return IRIS_E_CONFIG;
  if (n != s->n) return IRIS_E_SHAPE;
  int rc = s->read(s, v);
  if (rc == IRIS_E_ABSENT) {
    for (int i = 0; i < n; ++i) v[i] = s->desc[i].dflt;
    return IRIS_E_ABSENT;
  }
  if (rc <= 0) return rc;            /* 0 = nothing new, < 0 = broken */
  if (rc != n) return IRIS_E_SHAPE;    /* a port that lies about its own n */
  for (int i = 0; i < n; ++i)
    if (!(v[i] >= 0.0f && v[i] <= 1.0f)) return IRIS_E_RANGE;
  return rc;
}

/* Fill a slice with its declared defaults. Call once at boot so the feature
   vector is never garbage before the first successful read. */
IRIS_IO_API void iris_source_defaults(const iris_source *s, float *v) {
  if (!s || !v) return;
  for (int i = 0; i < s->n; ++i) v[i] = s->desc[i].dflt;
}

IRIS_MUST_CHECK IRIS_IO_API int iris_source_arm(const iris_source *s) {
  if (!s || !s->read || !s->desc) return IRIS_E_CONFIG;
  return iris_budget_arm(&s->budget);
}

IRIS_MUST_CHECK IRIS_IO_API int iris_source_measure(iris_source *s, uint32_t worst_us,
                                              uint32_t p99_us, uint32_t samples,
                                              int blocking) {
  if (!s) return IRIS_E_CONFIG;
  return iris_budget_measure(&s->budget, worst_us, p99_us, samples, blocking);
}

#endif /* IRIS_SOURCE_H */
