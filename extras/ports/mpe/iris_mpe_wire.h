/* iris_mpe_wire.h — what the note engine borrows from the wire layer.

   Internal to ports/mpe/. Nothing outside this directory includes it. The
   split it marks is a real one: iris_mpe_wire.c is the bytes, the channel
   arithmetic and the one-shot zone configuration — everything that happens
   on the Manager Channel or once per plug-in. iris_mpe.c is the note engine,
   which runs every frame and touches Member Channels only.                 */

#ifndef IRIS_MPE_WIRE_H
#define IRIS_MPE_WIRE_H

#include "iris_mpe.h"

/* One whole message or none of it. b < 0 means a two-byte message. Counts
   into m->msgs on success and m->refused on refusal, so back-pressure is a
   number somebody can print rather than a thing that happened once. */
int iris_mpe__put(iris_mpe *m, unsigned status, unsigned a, int b);

/* One row per dimension, IRIS_MPE_MAX_VOICES * IRIS_MPE_DIMS of them. The sink
   points base.desc at it; the training screen draws from base.desc. */
extern const iris_desc iris_mpe_desc[];

#define M3(m,st,a,b) iris_mpe__put((m),(st),(a),(int)(b))
#define M2(m,st,a)   iris_mpe__put((m),(st),(a),-1)

#endif
