/* THE GAIN SWEEP the header says is missing.
   iris_train_elm_ex is public and takes gain_w and gain_b separately, so the
   sweep IS reachable through the shipped API. Held-out error on fresh points
   the solve never saw, over several target shapes and many seeds. */
#define IRIS_IMPLEMENTATION
#include "iris.h"
#include <stdio.h>
#include <math.h>
#define NI 2
#define NO 2
static unsigned char A[400000];
static uint32_t rs=1u;
static float rnd(void){ rs=rs*1664525u+1013904223u; return (float)((rs>>8)&0xFFFF)/65535.0f; }

static void target(const float*in,float*out,int shape){
  float a=in[0], b=in[1];
  switch(shape){
    case 0: out[0]=0.5f+0.4f*sinf(3.0f*a)*cosf(2.0f*b);      out[1]=0.5f+0.3f*(a-b);        break;
    case 1: out[0]=(a>0.5f)?0.85f:0.15f;                     out[1]=0.5f+0.4f*b;            break; /* cliff */
    case 2: out[0]=0.1f+0.8f*a*a;                            out[1]=0.9f-0.8f*b*b;          break;
    default:out[0]=0.5f+0.35f*sinf(8.0f*a);                  out[1]=0.5f+0.35f*cosf(8.0f*b);break; /* periodic */
  }
}
static float trial(int nh,int nex,float gain,int shape,uint32_t seed){
  static unsigned char scr[IRIS_ELM_SCRATCH(64,NO)];
  size_t need=iris_size(NI,nh,NO,64); if(!need||need>sizeof A) return -1.0f;
  iris*k=iris_init(A,need,NI,nh,NO,64,seed); if(!k) return -1.0f;
  rs=seed;
  for(int i=0;i<nex;++i){ float in[NI],o[NO];
    in[0]=rnd(); in[1]=rnd(); target(in,o,shape); iris_record(k,in,o); }
  if(iris_train_elm_ex(k,1e-4f,gain,gain,scr,sizeof scr)<0) return -1.0f;
  double se=0; int n=0;
  for(int t=0;t<300;++t){ float in[NI],w[NO],g[NO];
    in[0]=rnd(); in[1]=rnd(); target(in,w,shape); iris_predict(k,in,g);
    for(int o=0;o<NO;++o){ double d=g[o]-w[o]; se+=d*d; n++; } }
  return (float)sqrt(se/n);
}
int main(void){
  const float mult[]={0.25f,0.5f,0.75f,1.0f,1.5f,2.0f,3.0f,4.0f,6.0f,8.0f};
  const int widths[]={12,24,48};
  printf("  gain = M / sqrt(n_in), n_in=2. Held out on 300 fresh points.\n");
  printf("  4 target shapes x 16 seeds x {8,20,50} demonstrations, per cell.\n\n");
  printf("      M      nh=12     nh=24     nh=48     mean\n");
  float best=1e9f; float bestM=0;
  for(unsigned m=0;m<sizeof mult/sizeof*mult;++m){
    float gain = mult[m]/sqrtf((float)NI);
    printf("  %5.2f", (double)mult[m]);
    double allsum=0; int alln=0;
    for(unsigned w=0;w<3;++w){
      double sum=0; int cnt=0;
      for(int shape=0;shape<4;++shape)
        for(uint32_t sd=1;sd<=16;++sd)
          for(int nex=8;nex<=50;nex+=21){
            float e=trial(widths[w],nex,gain,shape,sd*7919u);
            if(e>=0){ sum+=e; cnt++; allsum+=e; alln++; } }
      printf("   %7.4f", cnt?sum/cnt:-1.0);
    }
    double mean = alln?allsum/alln:1e9;
    printf("   %7.4f%s\n", mean, (mult[m]==2.0f)?"   <- shipped":"");
    if(mean<best){best=(float)mean;bestM=mult[m];}
  }
  printf("\n  lowest held-out error at M = %.2f (%.4f); shipped is M = 2.00\n",
         (double)bestM,(double)best);
  return 0; }
