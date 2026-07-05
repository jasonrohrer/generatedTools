#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec*1000.0 + t.tv_nsec/1e6; }
static void mk(AudioClip*c,long n){ int ch; long i; audio_alloc(c,2,44100,n);
    for(ch=0;ch<2;ch++)for(i=0;i<n;i++)c->channel[ch][i]=0.1f*(float)ch; }
int main(void){
    long N=44100L*60*28; AudioClip ins; Sequence s; int t;
    long pos[3]; const char*nm[3]={"start","middle","end"};
    mk(&ins,44100);
    pos[0]=0; pos[1]=N/2; pos[2]=N;
    printf("=== single 1s paste into 28-min stereo sequence ===\n");
    for(t=0;t<3;t++){
        AudioClip big; double t0,t1;
        seq_init(&s); mk(&big,N); seq_adopt_clip(&s,&big);
        t0=ms(); seq_insert_clip(&s,pos[t],&ins); t1=ms();
        printf("paste @ %-7s: %8.2f ms   (blocks=%d)\n",nm[t],t1-t0,s.numBlocks);
        seq_free(&s);
    }
    printf("=== single 1s delete at each position ===\n");
    for(t=0;t<3;t++){
        AudioClip big; double t0,t1; long at=pos[t]; if(at>=N) at=N-44100;
        seq_init(&s); mk(&big,N); seq_adopt_clip(&s,&big);
        t0=ms(); seq_delete_range(&s,at,at+44100); t1=ms();
        printf("delete @ %-7s: %8.2f ms\n",nm[t],t1-t0);
        seq_free(&s);
    }
    printf("=== train-car: 20000 small pastes at the end (empty start) ===\n");
    { AudioClip small; double t0,t1; int i; long worst=0; double worstMs=0;
      mk(&small,2000); seq_init(&s);
      t0=ms();
      for(i=0;i<20000;i++){ double a=ms(); seq_insert_clip(&s,s.numFrames,&small);
        double d=ms()-a; if(d>worstMs){worstMs=d; worst=i;} }
      t1=ms();
      printf("total=%.1f ms  avg=%.4f ms/paste  worst=%.3f ms @#%ld  finalBlocks=%d frames=%ld\n",
             t1-t0,(t1-t0)/20000.0,worstMs,worst,s.numBlocks,s.numFrames);
      audio_free(&small); seq_free(&s);
    }
    audio_free(&ins);
    return 0;
}
