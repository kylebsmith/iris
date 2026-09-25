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
static unsigned char C[IRIS_ARENA(1,12,1,32)];   /* second arena for test P */
static void demos(iris *k,int n){ for(int i=0;i<n;i++){
  float in[2]={i/(float)(n-1),(i%3)/2.0f}, o[3]={0.2f+i*0.02f,0.5f,0.8f-i*0.02f};
  iris_record(k,in,o);} }

int main(void){
  char d[160];

  /* A — iris_save must not write past what iris_save_size asked for. */
  { iris *k=iris_init(A,sizeof A,2,12,3,64,7); demos(k,8);
    iris_train_converge(k,0,0,0);
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
    /* forge a file whose stored input range is exactly zero-width. The
       ranges follow w1,b1,w2,b2, which start at offset 48; every value is
       little-endian (iris.h, PART 9). */
    { const size_t off=48+4*((size_t)12*2 + 12 + (size_t)3*12 + 3);
      const float v[4]={100.0f,0.0f,                     /* in_lo */
                        100.0f,1.0f};                    /* in_hi: channel 0 zero-width */
      for(int j=0;j<4;j++){ union{float f; uint32_t u;} w; w.f=v[j];
        for(int b=0;b<4;b++) f[off+4*(size_t)j+(size_t)b]=(unsigned char)(w.u>>(8*b)); }
      uint32_t c=iris_internal_crc32(f,n-sizeof(uint32_t));
      for(int b=0;b<4;b++) f[n-4+(size_t)b]=(unsigned char)(c>>(8*b)); }
    static unsigned char C2[IRIS_ARENA(2,12,3,64)];
    iris *k2=iris_init(C2,sizeof C2,2,12,3,64,7);
    int ok=iris_load(k2,f,n);
    /* Finite is not enough: the guard substitutes the range centre, so a dead
       instrument also looks finite. Ask whether it still RESPONDS. */
    float lo=1e30f, hi=-1e30f;
    if(ok) for(int t=0;t<=10;t++){
      float q[2]={100.0f, t/10.0f}, out[3]; iris_predict(k2,q,out);
      if(out[0]<lo) lo=out[0];
      if(out[0]>hi) hi=out[0]; }
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
    iris_train_elm(k,1e6f,scr,sizeof scr);
    int flagged_collapse = (iris_get_status(k)==IRIS_SOLVE_COLLAPSED);

    iris *k2=iris_init(A,sizeof A,2,16,3,64,5);
    if(!k2){ check("F collapse detector",0,"iris_init refused"); goto done; }
    /* a bump in the middle: identical at all four corners, varies inside */
    for(int i=0;i<20;i++){ float u=(i%5)/4.0f, v=(i/5)/3.0f;
      float bump=(u>0.2f&&u<0.8f&&v>0.2f&&v<0.8f)?0.4f:0.0f;
      float in[2]={u,v}, o[3]={0.3f+bump,0.5f,0.7f-bump}; iris_record(k2,in,o); }
    iris_train_elm(k2,1e-4f,scr,sizeof scr);
    int quiet_on_bump = (iris_get_status(k2)!=IRIS_SOLVE_COLLAPSED);
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
        iris_predict(r,in,got); b+=(got[0]-want)*(got[0]-want); n++; }
        b/=n; }
      float bad_in[2]={0.5f,0.5f}, bad_out[1]={0.95f};
      int id=iris_record(r,bad_in,bad_out); iris_train(r);
      iris_delete_id(r,id); iris_train(r);
      if (iris_get_status(r)!=IRIS_STATUS_OK) continue;
      float a2=0; { int n=0; for(int a=0;a<=10;a++) for(int c2=0;c2<=10;c2++){
        float in[2]={a/10.0f,c2/10.0f},got[1];
        float want=0.15f+0.7f*0.5f*(in[0]+in[1]);
        iris_predict(r,in,got); a2+=(got[0]-want)*(got[0]-want); n++; }
        a2/=n; }
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
    iris_record(k,in,o); iris_internal_fit_ranges(k);
    float bad_in  = iris_internal_norm_in (k, 200, 0.5f);
    float bad_out = iris_internal_norm_out(k, 200, 0.5f);
    float bad_den = iris_internal_denorm_out(k, 200, 0.5f);
    snprintf(d,sizeof d,"norm_in %.1f  norm_out %.1f  denorm_out %.1f (index 200 of 2)",
             bad_in, bad_out, bad_den);
    check("K out-of-range channel index is refused, not computed",
          bad_in==0.0f && bad_out==0.0f && bad_den==0.0f, d); }

  /* M — D5: a refused record must say WHICH of its three reasons applied. */
  { iris *k=iris_init(B,sizeof B,1,12,1,32,1);
    float in=0.5f,o=0.5f, nan=0.0f/0.0f;
    iris_record(k,&in,&o);
    int s_full=0, s_nan=0;
    iris_record(k,&nan,&o); s_nan=(int)iris_get_status(k);
    iris *k2=iris_init(A,sizeof A,2,12,3,64,1);
    { float i2[2]={0.1f,0.2f},o2[3]={0.3f,0.4f,0.5f};
      for(int i=0;i<80;i++) iris_record(k2,i2,o2);
      s_full=(int)iris_get_status(k2); }
    snprintf(d,sizeof d,"poisoned reading -> status %d, full store -> status %d", s_nan, s_full);
    check("M a refused record distinguishes its reasons",
          s_nan==IRIS_NAN_TRAPPED && s_full==IRIS_STORE_FULL, d); }

  /* N — D16: a non-positive slice budget must do nothing, not 2,000 epochs. */
  { iris *k=iris_init(B,sizeof B,1,12,1,32,1);
    for(int i=0;i<6;i++){ float in=i/5.0f,o=i/5.0f; iris_record(k,&in,&o); }
    iris_train_begin(k,20000);
    int before=iris_train_epochs_done(k);
    iris_train_slice(k,0);
    int after=iris_train_epochs_done(k);
    snprintf(d,sizeof d,"slice(0) advanced the epoch count by %d", after-before);
    check("N a zero slice budget does nothing", after==before, d); }

  /* O — D14: training an instrument with nothing in it must not report success. */
  { iris *k=iris_init(B,sizeof B,1,12,1,32,1);
    for(int i=0;i<6;i++){ float in=i/5.0f,o=i/5.0f; iris_record(k,&in,&o); }
    iris_train(k);
    iris_clear(k);
    int r=iris_train(k);
    snprintf(d,sizeof d,"iris_train on an emptied instrument returned %d", r);
    check("O training nothing reports failure", r==0, d); }

  /* P — no pair of flipped bits may get a file past the loader. Exhaustive
     over every two-bit corruption of a 1-in/1-out file, the smallest legal
     shape, where a changed version word or example count is cheapest to make
     add up; this asserts 0 accepted. */
  { iris *k=iris_init(B,sizeof B,1,12,1,32,1234u);
    for(int i=0;i<6;i++){ float in=i*0.15f,o=(float)((i*3)%5)/5.0f; iris_record(k,&in,&o); }
    iris_train(k);
    static unsigned char blob[512], sc[512];
    size_t n=iris_save(k,blob,sizeof blob);
    long acc=0;
    size_t bits=n*8;
    for(size_t x=0;x<bits;++x) for(size_t y=x+1;y<bits;++y){
      memcpy(sc,blob,n);
      sc[x/8]^=(unsigned char)(1u<<(x%8));
      sc[y/8]^=(unsigned char)(1u<<(y%8));
      iris *c=iris_init(C,sizeof C,1,12,1,32,7u);
      if(c && iris_load(c,sc,n)) acc++; }
    snprintf(d,sizeof d,"%ld of %ld two-bit corruptions accepted",
             acc,(long)(bits*(bits-1)/2));
    check("P no two-bit corruption survives on a 1-in/1-out file", acc==0, d); }

  /* Q-U — five properties the mutation harness proved had NO working check.
     The harness itself was broken (it grepped its own output for "failing", a
     word this file printed unconditionally), so its 100% score was decoration.
     Fixed, the honest score was 70%: six survivors. Five are below. The sixth,
     "ignore an unsizeable shape in iris_init", is UNREACHABLE on a 64-bit host
     by construction -- iris_init validates every dimension against its maximum
     before calling iris_internal_bytes, so the product cannot overflow a 64-bit
     size_t and `need` is never 0. It fires only where size_t is 16 bits. Not
     writing a test that cannot fail. */

  /* Q — the arena bound. An arena one byte short must be refused. */
  { size_t need = iris_size(1,12,1,32);
    iris *small = iris_init(B, need - 1, 1,12,1,32, 1u);
    iris *exact = iris_init(B, need,     1,12,1,32, 1u);
    snprintf(d,sizeof d,"need %u: one byte short %s, exact %s",
             (unsigned)need, small?"ACCEPTED":"refused", exact?"accepted":"REFUSED");
    check("Q an arena one byte short is refused", !small && exact, d); }

  /* R — the hidden-width floor of 8. The width is frozen into the file. */
  { iris *lo = iris_init(B,sizeof B,1,4,1,32,1u);
    iris *ok = iris_init(B,sizeof B,1,8,1,32,1u);
    snprintf(d,sizeof d,"n_hid 4 %s, n_hid 8 %s",
             lo?"ACCEPTED":"refused", ok?"accepted":"REFUSED");
    check("R the hidden-width floor of 8 holds", !lo && ok, d); }

  /* S — the shuffle buffer must be filled at init. It indexes the examples;
     left unset it reads whatever the caller's memory held, so this hands it
     memory that is deliberately not zero and compares against a clean run. */
  { memset(B, 0xA5, sizeof B);
    iris *k1 = iris_init(B,sizeof B,1,12,1,32,4242u);
    for(int i=0;i<8;i++){ float in=i/7.0f,o=(float)((i*3)%5)/5.0f; iris_record(k1,&in,&o); }
    iris_train(k1);
    float q=0.42f, a1; iris_predict(k1,&q,&a1);
    memset(C, 0x00, sizeof C);
    iris *k2 = iris_init(C,sizeof C,1,12,1,32,4242u);
    for(int i=0;i<8;i++){ float in=i/7.0f,o=(float)((i*3)%5)/5.0f; iris_record(k2,&in,&o); }
    iris_train(k2);
    float a2; iris_predict(k2,&q,&a2);
    snprintf(d,sizeof d,"dirty arena %.7f, clean arena %.7f",(double)a1,(double)a2);
    check("S init does not inherit the caller's memory", a1==a2, d); }

  /* T — the smoothing setting must survive a save and load. */
  { iris *k1=iris_init(B,sizeof B,1,12,1,32,1u);
    for(int i=0;i<6;i++){ float in=i/5.0f,o=i/5.0f; iris_record(k1,&in,&o); }
    iris_train(k1);
    iris_set_smoothing(k1, 0.7f);
    static unsigned char blob[512];
    size_t n=iris_save(k1,blob,sizeof blob);
    iris *k2=iris_init(C,sizeof C,1,12,1,32,9u);
    int ok = n && iris_load(k2,blob,n);
    float got = ok ? iris_get_smoothing(k2) : -1.0f;
    snprintf(d,sizeof d,"set 0.700, read back %.3f",(double)got);
    check("T smoothing survives save and load", ok && got > 0.69f && got < 0.71f, d); }

  /* U — iris_train must report the trainer's refusal, not its own optimism.
     iris_record and iris_load both refuse a not-a-number, so this writes one
     straight into the store: the state an IRIS_NO_GUARDS build, whose
     iris_record stores whatever it is given, can reach. */
  { iris *k1=iris_init(B,sizeof B,1,12,1,32,1u);
    for(int i=0;i<6;i++){ float in=i/5.0f,o=i/5.0f; iris_record(k1,&in,&o); }
    iris_train(k1);
    k1->ex[2*2] = __builtin_nanf("");               /* demonstration 2's input */
    int trained = iris_train(k1);
    snprintf(d,sizeof d,"iris_train over a stored not-a-number returned %d", trained);
    check("U iris_train reports the trainer's refusal", trained == 0, d); }

done:
  /* Do not print the word "failing" when nothing failed. Any tool that reads
     this output -- tools/mutate.sh did -- cannot tell the two apart otherwise.
     The exit code below is the real answer; this line is for humans. */
  if (fails) printf("\n  %d of 20 FAILING\n", fails);
  else       printf("\n  all 20 pass\n");
  return fails ? 1 : 0;
}
