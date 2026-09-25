# Negative result — dyadic positional encoding

Dyadic positional encoding is not in `iris.h`. This page keeps its code, its
measurements and the reasons it is off, so the idea can be judged on its
record rather than rediscovered. The tables are recorded figures from a study
whose program is not yet published; iris-studies lists it as S20 and points
back to this page. [The negative-results index](README.md) gives the one-line
summary.

The code below is written for inputs scaled to [0, 1]. `iris.h` scales inputs
to [-1, +1]
([architecture decision record (ADR) 0018](../adr/0018-the-input-scaling-travels-with-the-file.md)),
so reviving it means adapting the encoder's range first, then pasting it into
its own optional module.

```c
/* ==========================================================================
   PART 8g — DYADIC POSITIONAL ENCODING   (OFF, AND STAYING OFF FOR NOW)

   Compile with -DIRIS_OPT_POSENC to get the encoder. NOTHING IN THIS FILE
   CALLS IT. It is here because it was measured, the measurement is
   interesting, and the measurement is not yet good enough to ship — and a
   result that lives only in a scratch directory is a result that gets found
   again by the next person from scratch.

   WHAT IT IS. Replace the raw input vector x with
       [ x, sin(2^0 pi x), cos(2^0 pi x), ..., sin(2^(L-1) pi x), cos(...) ]
   the encoding NeRF (neural radiance fields; Mildenhall et al., ECCV 2020,
   the European Conference on Computer Vision) uses to let a small
   multilayer perceptron represent high-frequency detail, computed here with ONE sin/cos pair per
   input dimension and every higher octave from the double-angle identity.

   WHAT IT BUYS, measured on a detail-bearing truth (two narrow bumps added
   to the eight-output reference), 20 examples, nh=12, 6000 epochs, 8 seeds:

     encoding        dim   recall    grid    roughness   train ms
     -------------  ----  -------  -------  ----------  ---------
     raw (shipping)    2   0.0166   0.0596     0.00188      15.2
     dyadic L=2       10   0.0032   0.0587     0.00265      18.9
     dyadic L=3       14   0.0014   0.0649     0.00417      20.3
     dyadic L=4       18   0.0008   0.0794     0.00959      17.1

   Recall of the musician's own demonstrations improves up to 20x. The
   encoder adds 1,152 arena bytes at 8 outputs.

   AND IT SHOWS WHAT BOUNDS A CORRECTION, WHICH IS THE PART WORTH KEEPING.
   The ceiling on how much of a contradictory +0.15 correction training
   closes (the raw rows below) is not a capacity bound of the 12-hidden
   net.
   Widening the hidden layer is FLAT; changing the input representation is
   not:

     model           nhid   closed@30   closed@600   far drift   cold closed
     -------------  -----  ----------  -----------  ----------  -----------
     raw               12       27.2%        47.9%      0.0123        59.7%
     raw               24       29.5%        50.0%      0.0131        59.5%
     raw               48       28.6%        45.9%      0.0116        57.7%
     dyadic L=3        12       43.3%        67.9%      0.0104        95.7%
     dyadic L=4        12       49.8%        87.1%      0.0092        99.1%

   The edit authority a musician has over one demonstration is bounded by
   what the INPUT REPRESENTATION can localise, not by how many hidden units
   there are.

   WHY IT IS NOT ON.

     1. IT DOES NOT GENERALISE BETTER. Held-out grid error is neutral to
        WORSE at every L on the same runs (0.0596 raw vs 0.0649 at L=3,
        0.0794 at L=4). It memorises the demonstrations harder; it does not
        learn the mapping better. On this project's own terms — the
        instrument is the part between the demonstrations — that is the
        axis that matters and it is not moving.

     2. IT TRIPLES THE NEAR-DEMO REROLL SPREAD. Roughness rises 1.4x at L=2
        and 5.1x at L=4. The tests/audit.c check "reroll is steady at the
        demonstrations" requires rerolls to stay STEADY on
        demonstrated ground while moving in the gaps; that band is a
        designed musical property, not a numerical tolerance, and this
        encoding aims straight at it.

     3. NOTHING HERE HAS SEEN A HUMAN. Every number above comes from a
        smooth, noiseless truth function evaluated exactly. An encoding
        whose entire benefit is representing high-frequency detail is
        precisely the change most likely to reverse sign under real sensor
        noise, where the high frequencies are the noise.

   WHAT WOULD CHANGE THE ANSWER: the same table computed on RECORDED HUMAN
   GESTURES, with the reroll band measured rather than assumed. Until then
   this stays a compile-time flag that nothing sets.
   ========================================================================== */

#ifdef IRIS_OPT_POSENC

#ifndef IRIS_POSENC_L
#define IRIS_POSENC_L 3          /* octaves; dim = n_raw * (1 + 2L)            */
#endif
#define IRIS_POSENC_DIM(NRAW) ((NRAW) * (1 + 2 * (IRIS_POSENC_L)))

/* sin and cos of 2*pi*t, minimax polynomial on the quarter turn. No libc. */
IRIS_API void iris_sincos2pi(float t, float *so, float *co) {
  float q = t * 4.0f;
  float nf = q >= 0.0f ? (float)(int32_t)(q + 0.5f) : -(float)(int32_t)(0.5f - q);
  float r  = (q - nf) * 1.57079632679489662f;      /* pi/2 per quarter-turn */
  float r2 = r * r;
  float sn = r * (1.0f + r2 * (-0.16666667f + r2 * (0.00833333f + r2 * (-0.00019841270f))));
  float cs = 1.0f + r2 * (-0.5f + r2 * (0.04166667f + r2 * (-0.00138889f + r2 * 0.0000248016f)));
  int32_t n = (int32_t)nf & 3; if (n < 0) n += 4;
  switch (n) {
    case 0: *so =  sn; *co =  cs; break;
    case 1: *so =  cs; *co = -sn; break;
    case 2: *so = -sn; *co = -cs; break;
    default:*so = -cs; *co =  sn; break;
  }
}

/* z must hold IRIS_POSENC_DIM(n_raw) floats. x is expected in [0,1]; the
   instrument built on top of this one must be created with n_in =
   IRIS_POSENC_DIM(n_raw) and must NOT re-fit its input ranges per session
   (the encoded axes are already bounded), which is the second reason this
   is not simply switched on. */
IRIS_API void iris_posenc(const float *x, int n_raw, float *z) {
  int p = 0, i, l;
  for (i = 0; i < n_raw; ++i) z[p++] = x[i];
  for (i = 0; i < n_raw; ++i) {
    float s, c;
    iris_sincos2pi(0.5f * x[i], &s, &c);
    for (l = 0; l < IRIS_POSENC_L; ++l) {
      float s2, c2;
      z[p++] = s; z[p++] = c;
      s2 = 2.0f * s * c; c2 = 1.0f - 2.0f * s * s;   /* double angle */
      s = s2; c = c2;
    }
  }
}
#endif  /* IRIS_OPT_POSENC */
```
