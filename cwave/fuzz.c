/* fuzz.c -- verify Sequence is bit-exact vs a naive contiguous reference
 * across random insert/delete/splice/effect operations. */
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- naive contiguous reference ---- */
typedef struct { int nch, rate; long n; float *ch[8]; } Ref;

static void ref_init(Ref *r, int nch, int rate){ int c; r->nch=nch; r->rate=rate; r->n=0;
    for(c=0;c<8;c++) r->ch[c]=NULL; }
static void ref_free(Ref *r){ int c; for(c=0;c<8;c++){ if(r->ch[c])free(r->ch[c]); r->ch[c]=NULL;} r->n=0; }
static void ref_resize(Ref *r, long n){ int c; for(c=0;c<r->nch;c++){
    r->ch[c]=realloc(r->ch[c], (n>0?n:1)*sizeof(float)); } r->n=n; }

static void ref_insert(Ref *r, long at, float *const src[], long len){
    int c; long old=r->n;
    for(c=0;c<r->nch;c++){
        float *p=realloc(r->ch[c],(old+len)*sizeof(float));
        memmove(p+at+len, p+at, (old-at)*sizeof(float));
        memcpy(p+at, src[c], len*sizeof(float));
        r->ch[c]=p;
    }
    r->n=old+len;
}
static void ref_delete(Ref *r, long s, long e){
    int c; long tail=r->n-e;
    for(c=0;c<r->nch;c++) memmove(r->ch[c]+s, r->ch[c]+e, tail*sizeof(float));
    r->n = s+tail;
}
static float ref_peak(Ref *r, long s, long e){ int c; long i; float pk=0;
    for(c=0;c<r->nch;c++) for(i=s;i<e;i++){ float a=fabsf(r->ch[c][i]); if(a>pk)pk=a; } return pk; }
static void ref_amplify(Ref *r, long s, long e, float g){ int c; long i;
    for(c=0;c<r->nch;c++) for(i=s;i<e;i++) r->ch[c][i]*=g; }
static void ref_normalize(Ref *r, long s, long e, float peak){ float cur=ref_peak(r,s,e);
    if(cur<=0) return; ref_amplify(r,s,e,peak/cur); }
static void ref_fade(Ref *r, long s, long e, int out){ int c; long i,n=e-s; if(n<=1)return;
    for(c=0;c<r->nch;c++) for(i=s;i<e;i++){ float f=(float)(i-s)/(float)(n-1); if(out)f=1-f; r->ch[c][i]*=f; } }
static void ref_silence(Ref *r, long s, long e){ int c; long i;
    for(c=0;c<r->nch;c++) for(i=s;i<e;i++) r->ch[c][i]=0; }
static void ref_reverse(Ref *r, long s, long e){ int c; long i,j;
    for(c=0;c<r->nch;c++){ i=s; j=e-1; while(i<j){ float t=r->ch[c][i]; r->ch[c][i]=r->ch[c][j]; r->ch[c][j]=t; i++;j--; } } }

/* ---- PRNG ---- */
static unsigned long rng=12345;
static unsigned long rnd(void){ rng = rng*6364136223846793005UL + 1442695040888963407UL; return rng>>16; }
static long rndRange(long lo, long hi){ if(hi<=lo) return lo; return lo + (long)(rnd()%(unsigned long)(hi-lo)); }

/* ---- compare ---- */
static int fails=0;
static void cmp_all(Sequence *seq, Ref *r, const char *tag){
    int c; long i;
    AudioClip flat; memset(&flat,0,sizeof(flat));
    if(seq->numFrames != r->n){ printf("FAIL %s: len seq=%ld ref=%ld\n",tag,seq->numFrames,r->n); fails++; return; }
    /* internal consistency: start[] and block lens */
    { long acc=0; int b; for(b=0;b<seq->numBlocks;b++){ if(seq->start[b]!=acc){ printf("FAIL %s: start[%d]=%ld exp %ld\n",tag,b,seq->start[b],acc); fails++; } acc+=seq->blocks[b]->buf.numFrames; }
      if(acc!=seq->numFrames){ printf("FAIL %s: sum blocks %ld != numFrames %ld\n",tag,acc,seq->numFrames); fails++; } }
    if(r->n==0) return;
    seq_read_range(&flat, seq, 0, seq->numFrames);
    for(c=0;c<r->nch;c++) for(i=0;i<r->n;i++){
        if(flat.channel[c][i] != r->ch[c][i]){
            printf("FAIL %s: sample ch%d[%ld] seq=%.9g ref=%.9g\n",tag,c,i,flat.channel[c][i],r->ch[c][i]); fails++;
            audio_free(&flat); return;
        }
    }
    audio_free(&flat);
    /* col_minmax: raw path exact, bin path conservative */
    for(c=0;c<r->nch;c++){ int t; for(t=0;t<20;t++){
        long f0=rndRange(0,r->n), span=rndRange(1, r->n>4000? 4000: r->n), f1=f0+span;
        double spp = (t&1)? 1.0 : 1000.0;
        float smn,smx; long i2; float tmn,tmx; int got;
        if(f1>r->n) f1=r->n; if(f1<=f0) continue;
        tmn=tmx=r->ch[c][f0]; for(i2=f0;i2<f1;i2++){ float v=r->ch[c][i2]; if(v<tmn)tmn=v; if(v>tmx)tmx=v; }
        got=seq_col_minmax(seq,c,(double)f0,(double)f1,spp,&smn,&smx);
        if(!got){ printf("FAIL %s: minmax got nothing ch%d [%ld,%ld)\n",tag,c,f0,f1); fails++; continue; }
        if(spp<256){ if(smn!=tmn||smx!=tmx){ printf("FAIL %s: raw minmax ch%d [%ld,%ld) seq(%.6g,%.6g) ref(%.6g,%.6g)\n",tag,c,f0,f1,smn,smx,tmn,tmx); fails++; } }
        else { if(smn>tmn+1e-6||smx<tmx-1e-6){ printf("FAIL %s: bin minmax not conservative ch%d\n",tag,c); fails++; } }
    } }
    /* peak */
    { long f0=rndRange(0,r->n), f1=rndRange(f0,r->n+1); float sp=seq_peak(seq,f0,f1), rp=ref_peak(r,f0,f1);
      if(fabsf(sp-rp)>1e-6){ printf("FAIL %s: peak [%ld,%ld) seq=%.6g ref=%.6g\n",tag,f0,f1,sp,rp); fails++; } }
}

static void fill_clip(AudioClip *c, int nch, int rate, long n){
    int ch; long i;
    audio_alloc(c, nch, rate, n);
    for(ch=0;ch<nch;ch++) for(i=0;i<n;i++) c->channel[ch][i] = (float)((double)(rnd()%20001)/10000.0 - 1.0);
}

int main(int argc, char **argv){
    int nch = 2, iter, ops = argc>1? atoi(argv[1]):3000;
    if(argc>2) rng = (unsigned long)atol(argv[2]);
    Sequence seq; Ref ref;
    AudioClip init; memset(&init,0,sizeof(init));
    seq_init(&seq); ref_init(&ref,nch,44100);
    /* seed with ~600k frames so we cross many blocks */
    fill_clip(&init, nch, 44100, 600000);
    { int c; long i; ref_resize(&ref, init.numFrames);
      for(c=0;c<nch;c++) memcpy(ref.ch[c], init.channel[c], init.numFrames*sizeof(float)); }
    { AudioClip tmp; memset(&tmp,0,sizeof(tmp)); audio_copy(&tmp,&init); seq_adopt_clip(&seq,&tmp); }
    audio_free(&init);
    cmp_all(&seq,&ref,"init");

    for(iter=0; iter<ops && fails==0; iter++){
        int op = (int)(rnd()%9);
        long n = seq.numFrames;
        char tag[64]; sprintf(tag,"iter%d op%d",iter,op);
        if(op==0){ /* insert */
            long at=rndRange(0,n+1), len=rndRange(1,50000);
            AudioClip ins; memset(&ins,0,sizeof(ins)); fill_clip(&ins,nch,44100,len);
            seq_insert_clip(&seq,at,&ins);
            ref_insert(&ref,at,ins.channel,len);
            audio_free(&ins);
        } else if(op==1){ /* delete */
            if(n<2) continue; { long s=rndRange(0,n-1), e=rndRange(s+1,n+1);
            seq_delete_range(&seq,s,e); ref_delete(&ref,s,e); }
        } else if(op==2){ /* splice replace */
            if(n<2) continue; { long at=rndRange(0,n-1), rem=rndRange(0,n-at+1), len=rndRange(0,30000);
            AudioClip ins; float *pin[8]; int c; memset(&ins,0,sizeof(ins));
            if(len>0){ fill_clip(&ins,nch,44100,len); for(c=0;c<nch;c++) pin[c]=ins.channel[c]; }
            seq_splice(&seq,at,rem, len>0?pin:NULL, len);
            if(rem>0) ref_delete(&ref,at,at+rem);
            if(len>0) ref_insert(&ref,at,ins.channel,len);
            if(len>0) audio_free(&ins);
            }
        } else if(op==3){ long s=rndRange(0,n), e=rndRange(s,n+1); float g=(float)((rnd()%400)/100.0);
            seq_amplify(&seq,s,e,g); ref_amplify(&ref,s,e,g);
        } else if(op==4){ long s=rndRange(0,n), e=rndRange(s,n+1);
            seq_fade_in(&seq,s,e); ref_fade(&ref,s,e,0);
        } else if(op==5){ long s=rndRange(0,n), e=rndRange(s,n+1);
            seq_fade_out(&seq,s,e); ref_fade(&ref,s,e,1);
        } else if(op==6){ long s=rndRange(0,n), e=rndRange(s,n+1);
            seq_silence(&seq,s,e); ref_silence(&ref,s,e);
        } else if(op==7){ long s=rndRange(0,n), e=rndRange(s,n+1);
            seq_reverse(&seq,s,e); ref_reverse(&ref,s,e);
        } else if(op==8){ long s=rndRange(0,n), e=rndRange(s,n+1); float pk=(float)((rnd()%100)/100.0);
            seq_normalize(&seq,s,e,pk); ref_normalize(&ref,s,e,pk);
        }
        cmp_all(&seq,&ref,tag);
    }
    printf("blocks=%d frames=%ld fails=%d\n", seq.numBlocks, seq.numFrames, fails);
    seq_free(&seq); ref_free(&ref);
    printf(fails? "==== FUZZ FAILED ====\n" : "==== FUZZ OK ====\n");
    return fails?1:0;
}
