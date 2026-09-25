#include "iris.h"
#include <stdio.h>
#include <math.h>
static uint32_t rs=1u;
static float rnd(void){ rs=rs*1664525u+1013904223u; return (float)((rs>>8)&0xFFFF)/65535.0f; }

/* MODE 0 -- every output is its own unrelated function. The worst case, and
   not what an instrument is: nobody maps a hand to eight uncorrelated things.
   MODE 1 -- all outputs are different views of ONE underlying gesture. This is
   what a musical mapping actually is: a posture, moving as a coordinated whole.
   Robotics calls it a synergy. */
static void target(const float*in,int ni,float*out,int no,int mode){
  if(mode==0){
    for(int o=0;o<no;++o){ float a=0;
      for(int i=0;i<ni;++i) a+=sinf((float)(o+1)*1.7f*in[i]+(float)i*0.9f);
      out[o]=0.5f+0.4f*a/(float)ni; }
  } else {
    float lat=0;                       /* one shared latent, then mixed out */
    for(int i=0;i<ni;++i) lat+=sinf(1.7f*in[i]+(float)i*0.9f);
    lat/=(float)ni;
    for(int o=0;o<no;++o)
      out[o]=0.5f+0.4f*lat*cosf((float)o*0.7f);
  }
}
static float trial(int ni,int no,int nh,int nd,int mode,uint32_t seed){
  static unsigned char arena[600000];
  size_t need=iris_size(ni,nh,no,64);
  if(!need||need>sizeof arena) return -1.0f;
  iris*k=iris_init(arena,need,ni,nh,no,64,seed); if(!k) return -1.0f;
  rs=seed;
  for(int d=0;d<nd;++d){ float in[32],out[16];
    for(int i=0;i<ni;++i) in[i]=rnd();
    target(in,ni,out,no,mode); iris_record(k,in,out); }
  if(!iris_train(k)) return -1.0f;
  double se=0; int n=0;
  for(int t=0;t<400;++t){ float in[32],w[16],g[16];
    for(int i=0;i<ni;++i) in[i]=rnd();
    target(in,ni,w,no,mode); iris_predict(k,in,g);
    for(int o=0;o<no;++o){ double d=g[o]-w[o]; se+=d*d; n++; } }
  return (float)sqrt(se/(double)n);
}
static float avg(int ni,int no,int nh,int nd,int mode){
  double s=0;int c=0;
  for(uint32_t x=1;x<=12;++x){ float e=trial(ni,no,nh,nd,mode,x*7919u); if(e>=0){s+=e;c++;} }
  return c?(float)(s/c):-1.0f;
}
int main(void){
  const int ND=12;
  printf("  %d demonstrations, held out on 400 fresh points, 12 seeds.\n",ND);
  printf("  Error is PER OUTPUT CHANNEL, so columns compare directly.\n\n");

  printf("  A. cost of INPUTS (2 outputs, 12 hidden)\n");
  for(int n=1;n<=8;++n) printf("       %d inputs   %.4f\n",n,avg(n,2,12,ND,1));

  printf("\n  B. cost of OUTPUTS when they are UNRELATED (2 inputs, 12 hidden)\n");
  for(int n=1;n<=8;++n) printf("       %d outputs  %.4f\n",n,avg(2,n,12,ND,0));

  printf("\n  C. cost of OUTPUTS when they MOVE TOGETHER (2 inputs, 12 hidden)\n");
  printf("     -- this is what a musical mapping actually is\n");
  for(int n=1;n<=8;++n) printf("       %d outputs  %.4f\n",n,avg(2,n,12,ND,1));

  printf("\n  D. B again, but giving the network more hidden units as outputs grow\n");
  for(int n=1;n<=8;++n) printf("       %d outputs  %.4f  (%d hidden)\n",n,avg(2,n,12+6*n,ND,0),12+6*n);
  return 0;
}
