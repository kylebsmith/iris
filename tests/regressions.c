/* SPDX-License-Identifier: BSD-3-Clause
   Copyright (c) 2026 Kyle Smith */
/* ============================================================================
   regressions.c — one test per defect found by an independent review.

   Every one of these was written BEFORE its fix and watched to fail. That is
   the only way to know a test can fail at all, which this project has had to
   learn three times: a guards check that compared two identical builds, a
   version check that counted one place while the file contradicted itself, and
   a continuous-integration job that grepped for a word matching both PASS and
   FAIL.

   One of these tests was itself wrong when written: F sized its arena for 12
   hidden units and asked for 16, so iris_init correctly refused and the test
   read a null instrument -- reporting the library broken when the library was
   right. It is recorded here rather than quietly corrected, because a test
   suite that hides its own history teaches nothing.

     sh build.sh regressions
   ========================================================================= */
#include "iris.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static int fails = 0;
static void check(const char *name, int ok, const char *detail) {
  printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", name, detail);
  if (!ok) fails++;
}
static unsigned char A[IRIS_ARENA(2,16,3,64)];   /* widest shape any test uses */
static unsigned char B[IRIS_ARENA(1,12,1,32)];
static void demos(iris *k,int n){ for(int i=0;i<n;i++){
  float in[2]={i/(float)(n-1),(i%3)/2.0f}, o[3]={0.2f+i*0.02f,0.5f,0.8f-i*0.02f};
  iris_record(k,in,o);} }

int main(void){
  char d[160];

  /* A — iris_save must not write past what iris_save_size asked for, on a
         legacy-scaled instrument (the kind restored from an old file). */
  { iris *k=iris_init(A,sizeof A,2,12,3,64,7); demos(k,8);
    iris_train_converge(k,0,0,0);
    iris__set_legacy_norm(k,1);
    size_t need=iris_save_size(k);
    unsigned char *buf=malloc(need+8);
    memset(buf,0xA5,need+8);
    size_t n=iris_save(k,buf,need);
    int clean = (buf[need]==0xA5 && buf[need+1]==0xA5 && buf[need+2]==0xA5 && buf[need+3]==0xA5);
    snprintf(d,sizeof d,"wrote %zu into %zu; bytes past the end untouched: %s",
             n, need, clean?"yes":"NO -- 4 bytes clobbered");
    check("A save stays inside iris_save_size", clean, d);
    free(buf); }

  /* B — deleting every demonstration must end a training run, not just clear. */
  { iris *k=iris_init(B,sizeof B,1,12,1,32,1);
    for(int i=0;i<6;i++){ float in=i/5.0f,o=i/5.0f; iris_record(k,&in,&o); }
    iris_train_begin(k,20000); iris_train_slice(k,200);
    while(iris_count(k)) iris_delete_last(k);
    long n=0; while(iris_train_slice(k,200)){ if(++n>200000) break; }
    snprintf(d,sizeof d,"slice loop ran %ld times, busy=%d", n, iris_train_busy(k));
    check("B delete-to-empty ends the run", n<200000 && !iris_train_busy(k), d); }

  /* C — a null instrument must fail the way that function fails ordinarily. */
  { iris *k=iris_init(B,sizeof B,1,12,1,32,1);
    float in=0.5f,o=0.5f; iris_record(k,&in,&o);
    int io_null=iris_index_of(0,1),  io_bad=iris_index_of(k,99999);
    int ia_null=iris_id_at(0,0),     ia_bad=iris_id_at(k,99999);
    int st_null=(int)iris_get_status(0);
    snprintf(d,sizeof d,"index_of %d/%d  id_at %d/%d  status(NULL)=%d",
             io_null,io_bad,ia_null,ia_bad,st_null);
    check("C null guards match each function's convention",
          io_null==io_bad && ia_null==ia_bad && st_null!=IRIS_STATUS_OK, d); }

  /* D — a loaded file with a degenerate range must not produce not-a-number. */
  { iris *k=iris_init(A,sizeof A,2,12,3,64,7); demos(k,8);
    iris_train_converge(k,0,0,0);
    static unsigned char f[8192];
    size_t n=iris_save(k,f,sizeof f);
    /* forge a file whose stored input range is exactly zero-width */
    { float *fl=(float*)(f+9*sizeof(uint32_t));
      size_t off=(size_t)12*2 + 12 + (size_t)3*12 + 3;   /* w1,b1,w2,b2 */
      fl[off+0]=100.0f; fl[off+1]=0.0f;                  /* in_lo */
      fl[off+2]=100.0f; fl[off+3]=1.0f;                  /* in_hi: channel 0 zero-width */
      uint32_t c=iris_crc32(f,n-sizeof(uint32_t));
      memcpy(f+n-sizeof(uint32_t),&c,4); }
    static unsigned char C2[IRIS_ARENA(2,12,3,64)];
    iris *k2=iris_init(C2,sizeof C2,2,12,3,64,7);
    int ok=iris_load(k2,f,n);
    /* Finite is not enough: the guard substitutes the range centre, so a dead
       instrument also looks finite. Ask whether it still RESPONDS. */
    float lo=1e30f, hi=-1e30f;
    if(ok) for(int t=0;t<=10;t++){
      float q[2]={100.0f, t/10.0f}, out[3]; iris_predict(k2,q,out);
      if(out[0]<lo) lo=out[0]; if(out[0]>hi) hi=out[0]; }
    int responds = ok && (hi-lo) > 1e-4f;
    snprintf(d,sizeof d,"load=%d  output span across the live channel %.6f  status %d",
             ok, hi-lo, iris_get_status(k2));
    check("D a loaded zero-width range leaves a working instrument", !ok || responds, d); }

  /* E — asking for advice must leave every reported field as it found it. */
  { iris *k=iris_init(A,sizeof A,2,12,3,64,7); demos(k,10);
    iris_set_smoothing(k,0.25f); iris_train_converge(k,0,0,0);
    int tr0=iris_is_trained(k), ep0=iris_train_epochs_done(k);
    float q[2]={0.31f,0.44f}, p0[3],p1[3]; iris_predict(k,q,p0);
    static unsigned char sc[16384];
    iris_suggest_smoothing(k,sc,sizeof sc);
    iris_predict(k,q,p1);
    int same = (p0[0]==p1[0]&&p0[1]==p1[1]&&p0[2]==p1[2])
            && iris_is_trained(k)==tr0 && iris_train_epochs_done(k)==ep0;
    snprintf(d,sizeof d,"trained %d->%d  epochs %d->%d  prediction %s",
             tr0,iris_is_trained(k),ep0,iris_train_epochs_done(k),
             (p0[0]==p1[0])?"same":"MOVED");
    check("E advice restores the instrument completely", same, d); }

  /* F — the collapse detector must fire on a real collapse and stay quiet on a
         mapping that only varies away from the corners.
     NOTE: this test previously sized its arena for 12 hidden units and asked
     for 16, so iris_init correctly returned null and the test read a null
     instrument -- reporting the library broken when the library was right.
     Every instrument here is now checked. */
  { static float scr[65536];
    iris *k=iris_init(A,sizeof A,2,16,3,64,5);
    if(!k){ check("F collapse detector",0,"iris_init refused -- arena too small"); goto done; }
    for(int i=0;i<20;i++){ float in[2]={i/19.0f,(i%4)/3.0f},
      o[3]={0.1f+i*0.04f,0.5f,0.9f-i*0.03f}; iris_record(k,in,o); }
    iris_train_elm(k,1.0f/0.0f,scr,sizeof scr);
    int flagged_collapse = (iris_get_status(k)==IRIS_DIVERGED_STUCK);

    iris *k2=iris_init(A,sizeof A,2,16,3,64,5);
    if(!k2){ check("F collapse detector",0,"iris_init refused"); goto done; }
    /* a bump in the middle: identical at all four corners, varies inside */
    for(int i=0;i<20;i++){ float u=(i%5)/4.0f, v=(i/5)/3.0f;
      float bump=(u>0.2f&&u<0.8f&&v>0.2f&&v<0.8f)?0.4f:0.0f;
      float in[2]={u,v}, o[3]={0.3f+bump,0.5f,0.7f-bump}; iris_record(k2,in,o); }
    iris_train_elm(k2,1e-4f,scr,sizeof scr);
    int quiet_on_bump = (iris_get_status(k2)!=IRIS_DIVERGED_STUCK);
    snprintf(d,sizeof d,"collapse flagged: %s   healthy bump quiet: %s",
             flagged_collapse?"yes":"NO", quiet_on_bump?"yes":"NO -- false alarm");
    check("F collapse detector is right in both directions",
          flagged_collapse && quiet_on_bump, d); }

  /* G — one rule for "did the call work": 0 means it did nothing. Identifiers
         start at 1, so a successful iris_record is naturally non-zero. */
  { iris *k=iris_init(B,sizeof B,1,12,1,32,1);
    float in=0.5f,o=0.5f;
    int good=iris_record(k,&in,&o);
    int bad=0;
    for(int i=0;i<64;i++) bad=iris_record(k,&in,&o);   /* past capacity 32 */
    snprintf(d,sizeof d,"first record returned %d, a full store returned %d", good, bad);
    check("G a refused record returns 0, a good one returns its id",
          good>0 && bad==0, d); }

  /* H — iris_train(k) exists and needs no arguments a beginner cannot explain. */
  { iris *k=iris_init(B,sizeof B,1,12,1,32,1);
    for(int i=0;i<5;i++){ float in=i/4.0f,o=i/4.0f; iris_record(k,&in,&o); }
    int ok=iris_train(k);
    iris *empty=iris_init(A,sizeof A,2,12,3,64,1);
    int refused=iris_train(empty);
    snprintf(d,sizeof d,"trained returned %d, nothing-to-train returned %d", ok, refused);
    check("H iris_train works and refuses with 0", ok!=0 && refused==0, d); }

  /* I — THE REPAIR LOOP. Record a bad take, delete it, retrain: the sequence
         the whole library is built around. The instrument must come back to
         where it was, not keep the deleted take's crater. */
  { float sum_before=0, sum_after=0; int worse=0, runs=0;
    for (unsigned seed=1; seed<=40; ++seed) {
      static unsigned char R[IRIS_ARENA(2,12,1,32)];
      iris *r=iris_init(R,sizeof R,2,12,1,32,seed);
      for(int i=0;i<16;i++){ float u=(i%4)/3.0f, v=(i/4)/3.0f;
        float in[2]={u,v}, o[1]={0.15f+0.7f*0.5f*(u+v)}; iris_record(r,in,o); }
      iris_train(r);
      float b=0; { int n=0; for(int a=0;a<=10;a++) for(int c2=0;c2<=10;c2++){
        float in[2]={a/10.0f,c2/10.0f},got[1];
        float want=0.15f+0.7f*0.5f*(in[0]+in[1]);
        iris_predict(r,in,got); b+=(got[0]-want)*(got[0]-want); n++; } b/=n; }
      float bad_in[2]={0.5f,0.5f}, bad_out[1]={0.95f};
      int id=iris_record(r,bad_in,bad_out); iris_train(r);
      iris_delete_id(r,id); iris_train(r);
      if (iris_get_status(r)!=IRIS_STATUS_OK) continue;
      float a2=0; { int n=0; for(int a=0;a<=10;a++) for(int c2=0;c2<=10;c2++){
        float in[2]={a/10.0f,c2/10.0f},got[1];
        float want=0.15f+0.7f*0.5f*(in[0]+in[1]);
        iris_predict(r,in,got); a2+=(got[0]-want)*(got[0]-want); n++; } a2/=n; }
      runs++; sum_before+=b; sum_after+=a2; if (a2 > b*1.5f) worse++;
    }
    snprintf(d,sizeof d,"%d of %d runs left the gaps worse; mean ratio %.1fx",
             worse, runs, (double)(sum_after/sum_before));
    check("I delete-and-retrain restores the instrument", worse==0, d); }

  /* J — iris_delete_nearest must obey the rule its three siblings obey. */
  { snprintf(d,sizeof d,"index %d  id %d  last %d  nearest %d",
      iris_delete_index(0,0), iris_delete_id(0,1),
      iris_delete_last(0), iris_delete_nearest(0,0));
    check("J every delete reports failure the same way",
          iris_delete_index(0,0)==0 && iris_delete_id(0,1)==0 &&
          iris_delete_last(0)==0    && iris_delete_nearest(0,0)==0, d); }

  /* K — a public function must not read past its own dimensions. */
  { iris *k=iris_init(A,sizeof A,2,12,3,64,1);
    float in[2]={0.1f,0.2f}, o[3]={0.4f,0.5f,0.6f};
    iris_record(k,in,o); iris_fit_ranges(k);
    float bad_in  = iris_norm_in (k, 200, 0.5f);
    float bad_out = iris_norm_out(k, 200, 0.5f);
    float bad_den = iris_denorm_out(k, 200, 0.5f);
    snprintf(d,sizeof d,"norm_in %.1f  norm_out %.1f  denorm_out %.1f (index 200 of 2)",
             bad_in, bad_out, bad_den);
    check("K out-of-range channel index is refused, not computed",
          bad_in==0.0f && bad_out==0.0f && bad_den==0.0f, d); }

done:
  printf("\n  %d of 11 failing\n", fails);
  return fails ? 1 : 0;
}
