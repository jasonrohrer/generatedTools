/* cwave.c -- a fast, stable graphical audio editor.
 *
 * Pure C89.  Window / GL / audio via SDL2, GUI via Dear ImGui (through
 * the C shim in gui.h), waveform drawn with legacy OpenGL.
 *
 *   cwave [file]
 */

/* Request POSIX + C99-library prototypes (snprintf, getcwd, opendir)
 * while still compiling the language as C89. */
#define _XOPEN_SOURCE 700

#include <SDL.h>
#include <SDL_opengl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>

#include "audio.h"
#include "gui.h"

/* -------------------------------------------------------------------- */
/* constants                                                            */
/* -------------------------------------------------------------------- */

#define TRANSPORT_H   96.0f     /* height of bottom transport panel      */
#define OVERVIEW_H    56.0f     /* height of the top overview / scroll bar */
#define MARK_STRIP_H  16.0f     /* caret strip along the top of the wave  */
#define UNDO_LEVELS   32
#define PATH_MAX_LEN  1024
#define MAX_MARKS     512       /* marks that can accumulate              */
#define MARK_SNAP_PX  8.0f      /* selection snaps to a mark within this  */
#define EDGE_PAD_PX   20.0f     /* black scroll-past room beyond start/end */
#define MARK_NUM_COLORS 10      /* distinct caret colors we cycle through  */

/* A palette of 10 visually distinct caret colors.  Each group of marks made
 * together (a selection's two edges, a paste's two seams, or a lone cursor
 * mark) is stamped with the next color in turn, so it is easy to see which
 * carets belong to the same operation.  Cyan/blue hues are deliberately
 * avoided: the waveform itself is blue, so a blue caret would vanish against
 * it.  Instead we use five bright hues (orange, yellow, green, magenta, red)
 * and five darker cousins (tan, dark green, purple, maroon, brown). */
static const float markColors[MARK_NUM_COLORS][3] = {
    { 0.98f, 0.55f, 0.12f },   /* orange     */
    { 0.95f, 0.85f, 0.20f },   /* yellow     */
    { 0.35f, 0.85f, 0.30f },   /* green      */
    { 0.95f, 0.30f, 0.85f },   /* magenta    */
    { 0.95f, 0.25f, 0.20f },   /* red        */
    { 0.80f, 0.66f, 0.42f },   /* tan        */
    { 0.30f, 0.58f, 0.26f },   /* dark green */
    { 0.62f, 0.38f, 0.90f },   /* purple     */
    { 0.72f, 0.16f, 0.30f },   /* maroon     */
    { 0.66f, 0.44f, 0.24f }    /* brown      */
};

/* -------------------------------------------------------------------- */
/* waveform summary pyramid                                             */
/*                                                                      */
/* Rendering a big file by scanning every raw sample per pixel column   */
/* is O(numFrames) per frame, which makes zooming / dragging on large   */
/* files crawl.  Instead we precompute a mip pyramid of (min,max) pairs */
/* once, and render from the coarsest level whose bin fits in a pixel.  */
/* Level 0 bins WF_MIN_BIN samples; each higher level doubles the bin.  */
/* -------------------------------------------------------------------- */

#define WF_MIN_BIN     256
#define WF_MAX_LEVELS  24

typedef struct {
    int    valid;
    int    numChannels;
    long   numFrames;
    int    numLevels;
    long   binSize[WF_MAX_LEVELS];
    long   numBins[WF_MAX_LEVELS];
    float *mn[WF_MAX_LEVELS][AUDIO_MAX_CHANNELS];
    float *mx[WF_MAX_LEVELS][AUDIO_MAX_CHANNELS];
} WaveCache;

static void waveCacheFree( WaveCache *wc )
{
    int L, ch;
    for( L = 0; L < WF_MAX_LEVELS; L++ )
        for( ch = 0; ch < AUDIO_MAX_CHANNELS; ch++ ) {
            if( wc->mn[L][ch] ) free( wc->mn[L][ch] );
            if( wc->mx[L][ch] ) free( wc->mx[L][ch] );
            wc->mn[L][ch] = wc->mx[L][ch] = NULL;
        }
    wc->valid = 0; wc->numLevels = 0;
    wc->numFrames = 0; wc->numChannels = 0;
}

/* (Re)build every coarse level (1..) by combining pairs of bins from the level
 * below.  Frees any existing coarse levels first.  The caller must already
 * have populated level 0 (mn[0]/mx[0], numBins[0], binSize[0]) and set
 * numChannels/numFrames.  This is the cheap part of a build -- O(number of
 * level-0 bins), i.e. O(numFrames/WF_MIN_BIN) -- and is shared by the full
 * build and the incremental tail rebuild.  On allocation failure the whole
 * cache is freed (left invalid). */
static void waveCacheRebuildUpper( WaveCache *wc )
{
    int  nch = wc->numChannels, ch, L;
    long b;

    for( L = 1; L < WF_MAX_LEVELS; L++ )
        for( ch = 0; ch < AUDIO_MAX_CHANNELS; ch++ ) {
            if( wc->mn[L][ch] ) { free( wc->mn[L][ch] ); wc->mn[L][ch] = NULL; }
            if( wc->mx[L][ch] ) { free( wc->mx[L][ch] ); wc->mx[L][ch] = NULL; }
        }

    L = 0;
    while( wc->numBins[L] > 1 && L + 1 < WF_MAX_LEVELS ) {
        long pbins = wc->numBins[L];
        long nbins = ( pbins + 1 ) / 2;
        int  Lp = L + 1;
        wc->binSize[Lp] = wc->binSize[L] * 2;
        wc->numBins[Lp] = nbins;
        for( ch = 0; ch < nch; ch++ ) {
            float *pmn = wc->mn[L][ch], *pmx = wc->mx[L][ch];
            float *omn = (float *)malloc( (size_t)nbins * sizeof(float) );
            float *omx = (float *)malloc( (size_t)nbins * sizeof(float) );
            if( !omn || !omx ) {
                if( omn ) free( omn );
                if( omx ) free( omx );
                waveCacheFree( wc ); return;
            }
            for( b = 0; b < nbins; b++ ) {
                long  a = b * 2, bb = a + 1;
                float mn = pmn[a], mx = pmx[a];
                if( bb < pbins ) {
                    if( pmn[bb] < mn ) mn = pmn[bb];
                    if( pmx[bb] > mx ) mx = pmx[bb];
                }
                omn[b] = mn; omx[b] = mx;
            }
            wc->mn[Lp][ch] = omn; wc->mx[Lp][ch] = omx;
        }
        L = Lp;
    }
    wc->numLevels = L + 1;
    wc->valid = 1;
}

/* Build the pyramid for clip c.  If 'progress' is non-NULL it is filled
 * 0..1000 as the (dominant) level-0 pass proceeds. */
static int waveCacheBuild( WaveCache *wc, const AudioClip *c,
                           volatile int *progress )
{
    int  nch = c->numChannels, ch;
    long n = c->numFrames, bins, b;

    waveCacheFree( wc );
    if( n <= 0 || nch < 1 ) { if( progress ) *progress = 1000; return 0; }

    wc->numChannels = nch;
    wc->numFrames   = n;
    wc->binSize[0]  = WF_MIN_BIN;
    bins = ( n + WF_MIN_BIN - 1 ) / WF_MIN_BIN;
    wc->numBins[0]  = bins;

    for( ch = 0; ch < nch; ch++ ) {
        wc->mn[0][ch] = (float *)malloc( (size_t)bins * sizeof(float) );
        wc->mx[0][ch] = (float *)malloc( (size_t)bins * sizeof(float) );
        if( !wc->mn[0][ch] || !wc->mx[0][ch] ) { waveCacheFree( wc ); return 1; }
    }
    /* level 0 straight from raw samples (the O(numFrames) pass) */
    for( ch = 0; ch < nch; ch++ ) {
        float *s   = c->channel[ch];
        float *omn = wc->mn[0][ch];
        float *omx = wc->mx[0][ch];
        for( b = 0; b < bins; b++ ) {
            long  i0 = b * WF_MIN_BIN, i1 = i0 + WF_MIN_BIN, i;
            float mn, mx;
            if( i1 > n ) i1 = n;
            mn = mx = s[i0];
            for( i = i0 + 1; i < i1; i++ ) {
                float v = s[i];
                if( v < mn ) mn = v;
                if( v > mx ) mx = v;
            }
            omn[b] = mn; omx[b] = mx;
        }
        if( progress ) *progress = (int)( 1000.0 * (double)( ch + 1 ) / nch );
    }
    waveCacheRebuildUpper( wc );
    if( progress ) *progress = 1000;
    return wc->valid ? 0 : 1;
}

/* Incrementally patch the pyramid after a *length-changing* structural edit
 * that left frames [0,from) untouched and rewrote everything from 'from' to the
 * (new) end -- an insert at 'from', a delete/cut starting there, a paste that
 * replaced a tail selection, or an undo/redo splice at 'from'.  Only the
 * level-0 bins from 'from' onward are recomputed from raw samples (the array is
 * realloc'd to the new length, preserving the untouched [0,from) prefix); the
 * cheap coarse levels are then rebuilt.  Cost scales with the changed tail, so
 * an edit at the END of a long file avoids the full O(numFrames) rebuild.
 *
 * Caller guarantees the current level-0 bins covering [0,from) are still valid
 * for clip c.  Falls back to a full build if the cache is not usable. */
static void waveCacheReplaceTail( WaveCache *wc, const AudioClip *c, long from )
{
    int  nch = c->numChannels, ch;
    long n = c->numFrames;
    long newBins, b0, b;

    if( !wc->valid || wc->numChannels != nch ) {
        waveCacheBuild( wc, c, NULL );
        return;
    }
    if( n <= 0 ) { waveCacheFree( wc ); return; }
    if( from < 0 ) from = 0;

    newBins = ( n + WF_MIN_BIN - 1 ) / WF_MIN_BIN;
    b0 = from / WF_MIN_BIN;              /* first level-0 bin that may differ */
    if( b0 > newBins ) b0 = newBins;

    /* resize level-0 arrays to the new bin count; realloc preserves the
     * [0,b0) prefix (unchanged frames), and any growth is filled below */
    for( ch = 0; ch < nch; ch++ ) {
        float *pn = (float *)realloc( wc->mn[0][ch],
                        (size_t)( newBins > 0 ? newBins : 1 ) * sizeof(float) );
        float *px = (float *)realloc( wc->mx[0][ch],
                        (size_t)( newBins > 0 ? newBins : 1 ) * sizeof(float) );
        if( pn ) wc->mn[0][ch] = pn;
        if( px ) wc->mx[0][ch] = px;
        if( !pn || !px ) { waveCacheBuild( wc, c, NULL ); return; }
    }
    wc->numBins[0]  = newBins;
    wc->numChannels = nch;
    wc->numFrames   = n;

    /* recompute the changed level-0 bins [b0, newBins) from raw samples */
    for( ch = 0; ch < nch; ch++ ) {
        float *s   = c->channel[ch];
        float *omn = wc->mn[0][ch];
        float *omx = wc->mx[0][ch];
        for( b = b0; b < newBins; b++ ) {
            long  i0 = b * WF_MIN_BIN, i1 = i0 + WF_MIN_BIN, i;
            float mn, mx;
            if( i1 > n ) i1 = n;
            if( i0 >= n ) break;
            mn = mx = s[i0];
            for( i = i0 + 1; i < i1; i++ ) {
                float v = s[i];
                if( v < mn ) mn = v;
                if( v > mx ) mx = v;
            }
            omn[b] = mn; omx[b] = mx;
        }
    }
    waveCacheRebuildUpper( wc );
}

/* Incrementally refresh only the pyramid bins covering frames [start,end).
 * Used after an in-place edit (fade, normalize, silence, ...) that does NOT
 * change the clip length: the geometry (bin counts / sizes) is unchanged, so
 * we recompute the handful of affected level-0 bins from the raw samples and
 * propagate the change up the pyramid.  Cost scales with the edited span, not
 * with the whole file -- fading the tail of a long file is now instant.
 *
 * Caller must guarantee wc was built for a clip with the SAME numFrames and
 * numChannels as c (i.e. a length-preserving edit). */
static void waveCacheUpdateRange( WaveCache *wc, const AudioClip *c,
                                  long start, long end )
{
    int  nch = c->numChannels, ch, L;
    long n = c->numFrames;
    long b0, b1, b;

    if( !wc->valid || wc->numLevels < 1 ) return;
    if( start < 0 ) start = 0;
    if( end > n )   end = n;
    if( end <= start ) return;

    /* level 0: recompute affected bins straight from raw samples */
    b0 = start / WF_MIN_BIN;
    b1 = ( end - 1 ) / WF_MIN_BIN;      /* inclusive */
    if( b1 >= wc->numBins[0] ) b1 = wc->numBins[0] - 1;
    for( ch = 0; ch < nch; ch++ ) {
        float *s   = c->channel[ch];
        float *omn = wc->mn[0][ch];
        float *omx = wc->mx[0][ch];
        for( b = b0; b <= b1; b++ ) {
            long  i0 = b * WF_MIN_BIN, i1 = i0 + WF_MIN_BIN, i;
            float mn, mx;
            if( i1 > n ) i1 = n;
            if( i0 >= n ) break;
            mn = mx = s[i0];
            for( i = i0 + 1; i < i1; i++ ) {
                float v = s[i];
                if( v < mn ) mn = v;
                if( v > mx ) mx = v;
            }
            omn[b] = mn; omx[b] = mx;
        }
    }

    /* propagate the dirty bin range up through the coarser levels */
    for( L = 1; L < wc->numLevels; L++ ) {
        long pbins = wc->numBins[L - 1];
        long nb    = wc->numBins[L];
        long ub0 = b0 / 2, ub1 = b1 / 2;
        if( ub1 >= nb ) ub1 = nb - 1;
        for( ch = 0; ch < nch; ch++ ) {
            float *pmn = wc->mn[L - 1][ch], *pmx = wc->mx[L - 1][ch];
            float *omn = wc->mn[L][ch],     *omx = wc->mx[L][ch];
            for( b = ub0; b <= ub1; b++ ) {
                long  a = b * 2, bb = a + 1;
                float mn = pmn[a], mx = pmx[a];
                if( bb < pbins ) {
                    if( pmn[bb] < mn ) mn = pmn[bb];
                    if( pmx[bb] > mx ) mx = pmx[bb];
                }
                omn[b] = mn; omx[b] = mx;
            }
        }
        b0 = ub0; b1 = ub1;
    }
}

/* largest level whose bin fits within 'spp' samples-per-pixel, or -1 to
 * scan raw samples (when zoomed in enough that raw is already cheap). */
static int waveCacheLevel( const WaveCache *wc, double spp )
{
    int L, best = -1;
    if( !wc->valid ) return -1;
    for( L = 0; L < wc->numLevels; L++ ) {
        if( (double)wc->binSize[L] <= spp ) best = L;
        else break;
    }
    return best;
}

/* min/max of channel ch over frame span [f0,f1); via cache level or raw.
 * Returns 1 if any data covered. */
static int colMinMax( const AudioClip *c, const WaveCache *wc, int level,
                      int ch, double f0, double f1, float *mn, float *mx )
{
    long n = c->numFrames;
    long i0 = (long)f0, i1 = (long)f1;
    if( i0 < 0 ) i0 = 0;
    if( i1 > n ) i1 = n;
    if( i1 <= i0 ) i1 = i0 + 1;
    if( i0 >= n ) return 0;

    if( level >= 0 ) {
        long   bs = wc->binSize[level];
        long   b0 = i0 / bs, b1 = ( i1 + bs - 1 ) / bs, b;
        long   nb = wc->numBins[level];
        float *pmn = wc->mn[level][ch], *pmx = wc->mx[level][ch];
        float  lo = 0.0f, hi = 0.0f;
        int    any = 0;
        if( b1 > nb ) b1 = nb;
        for( b = b0; b < b1; b++ ) {
            if( !any ) { lo = pmn[b]; hi = pmx[b]; any = 1; }
            else {
                if( pmn[b] < lo ) lo = pmn[b];
                if( pmx[b] > hi ) hi = pmx[b];
            }
        }
        if( !any ) return 0;
        *mn = lo; *mx = hi; return 1;
    } else {
        float *s = c->channel[ch];
        long   i;
        float  lo = s[i0], hi = s[i0];
        for( i = i0 + 1; i < i1 && i < n; i++ ) {
            float v = s[i];
            if( v < lo ) lo = v;
            if( v > hi ) hi = v;
        }
        *mn = lo; *mx = hi; return 1;
    }
}

/* -------------------------------------------------------------------- */
/* playback                                                             */
/* -------------------------------------------------------------------- */

typedef struct {
    AudioClip *clip;
    long  playhead;
    long  playStart;
    long  playEnd;
    int   playing;
    int   loop;
    int   followSel;   /* live-track the selection as loop/play range     */
    float volume;
} Player;

static Player       player;
static SDL_AudioDeviceID audioDev = 0;
static int          audioDevRate  = 0;

static float clamp1( float v )
{
    if( v >  1.0f ) return  1.0f;
    if( v < -1.0f ) return -1.0f;
    return v;
}

static void SDLCALL audio_cb( void *ud, Uint8 *stream, int len )
{
    Player *p = (Player *)ud;
    float *out = (float *)stream;
    int frames = len / (int)( 2 * sizeof(float) );
    int i;
    for( i = 0; i < frames; i++ ) {
        float L = 0.0f, R = 0.0f;
        if( p->playing && p->clip && p->clip->numFrames > 0 &&
            p->playhead >= 0 && p->playhead < p->playEnd &&
            p->playhead < p->clip->numFrames ) {
            AudioClip *c = p->clip;
            long h = p->playhead;
            if( c->numChannels == 1 ) {
                L = R = c->channel[0][h];
            } else {
                int ch;
                L = c->channel[0][h];
                R = c->channel[1][h];
                for( ch = 2; ch < c->numChannels; ch++ ) {
                    float s = c->channel[ch][h] * 0.5f;
                    L += s; R += s;
                }
            }
            L = clamp1( L * p->volume );
            R = clamp1( R * p->volume );
            p->playhead++;
            if( p->playhead >= p->playEnd ) {
                if( p->loop ) p->playhead = p->playStart;
                else          p->playing  = 0;
            }
        }
        out[2 * i]     = L;
        out[2 * i + 1] = R;
    }
}

static void audio_lock( void )   { if( audioDev ) SDL_LockAudioDevice( audioDev ); }
static void audio_unlock( void ) { if( audioDev ) SDL_UnlockAudioDevice( audioDev ); }

static void open_audio( int rate )
{
    SDL_AudioSpec want, have;
    if( rate <= 0 ) rate = 44100;
    if( audioDev && audioDevRate == rate ) return;
    if( audioDev ) { SDL_CloseAudioDevice( audioDev ); audioDev = 0; }

    SDL_memset( &want, 0, sizeof(want) );
    want.freq     = rate;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = 1024;
    want.callback = audio_cb;
    want.userdata = &player;

    audioDev = SDL_OpenAudioDevice( NULL, 0, &want, &have,
                                    SDL_AUDIO_ALLOW_SAMPLES_CHANGE );
    if( audioDev ) {
        audioDevRate = rate;
        SDL_PauseAudioDevice( audioDev, 0 );
    }
}

/* -------------------------------------------------------------------- */
/* application state                                                    */
/* -------------------------------------------------------------------- */

/* A mark is a labelled point in the audio that tracks its sample as edits
 * insert/remove frames around it.  Marks are created manually (at the
 * cursor or around a selection) and automatically at the seams of edits
 * (cut points, paste boundaries, processed-region edges), so you never
 * lose where an operation happened.  Rendered as carets above the wave. */
typedef struct {
    long frame;      /* sample position it points at            */
    int  selected;   /* highlighted / targeted for deletion     */
    int  color;      /* index into markColors (per-group hue)   */
} Mark;

/* An undo record is a self-reversing "splice": it stores the frames that
 * occupied [at, at+oldLen) before the edit (oldData) and the frames that
 * occupy [at, at+newLen) after it (newData).  Reversing the edit splices
 * oldData back in; replaying it splices newData in.  Crucially only the
 * edited span is copied, so an undo of a small edit on a huge file costs
 * the size of the edit -- NOT a full-clip snapshot as before.  (A length-
 * changing structural edit, e.g. trim, still stores the affected spans,
 * which for trim is the whole clip -- that case is inherently O(n).) */
typedef struct {
    long   at;
    long   oldLen;
    long   newLen;
    int    numChannels;
    float *oldData[AUDIO_MAX_CHANNELS];  /* oldLen frames/ch, NULL if 0 */
    float *newData[AUDIO_MAX_CHANNELS];  /* newLen frames/ch, NULL if 0 */
} UndoRec;

typedef struct {
    AudioClip clip;
    AudioClip clipboard;

    /* marks (see Mark) */
    Mark marks[MAX_MARKS];
    int  numMarks;
    int  markColorNext;      /* next hue to hand out for a new mark group */

    /* selection (frames); selEnd == selStart means "no selection", the
     * cursor sits at selStart */
    long selStart;
    long selEnd;

    /* view */
    double viewStart;        /* first visible frame                     */
    double samplesPerPixel;  /* zoom                                    */

    /* waveform pixel region for this frame (updated each render) */
    float  wfX, wfY, wfW, wfH;

    /* overview / scroll bar pixel region (updated each render) */
    float  ovX, ovY, ovW, ovH;

    /* waveform summary pyramid, rebuilt when waveGen changes */
    WaveCache wave;
    long      waveGen;      /* bumped whenever the clip data mutates      */
    long      waveBuiltGen; /* generation the current cache was built for */

    /* undo / redo (delta records, see UndoRec) */
    UndoRec undo[UNDO_LEVELS];
    UndoRec redo[UNDO_LEVELS];
    int     undoCount;
    int     redoCount;

    char path[PATH_MAX_LEN];
    int  dirty;

    /* pending dialog to open (handled next frame): name or "" */
    char  pendingPopup[64];
    /* dialog scratch values */
    float dlgGain;
    float dlgNormDb;              /* normalize target in dBFS            */
    char  statusMsg[256];

    /* file browser */
    int  browserSave;             /* 0 = open, 1 = save                  */
    char browserDir[PATH_MAX_LEN];
    char browserFile[PATH_MAX_LEN];
} App;

static App app;

/* -------------------------------------------------------------------- */
/* asynchronous file loading (worker thread + progress bar)             */
/* -------------------------------------------------------------------- */

typedef struct {
    char         path[PATH_MAX_LEN];
    AudioClip    clip;
    WaveCache    cache;
    volatile int progress;   /* 0..1000 within the current phase          */
    volatile int phase;      /* 0 = decoding file, 1 = building overview   */
    volatile int done;       /* set by the worker when finished            */
    int          error;
    char         err[256];
} LoadJob;

static LoadJob   loadJob;
static pthread_t loadThread;
static int       loading = 0;   /* a load is in flight (main-thread flag)  */

static void *loadWorker( void *arg )
{
    LoadJob *j = (LoadJob *)arg;
    memset( &j->clip,  0, sizeof(j->clip) );
    memset( &j->cache, 0, sizeof(j->cache) );
    j->phase = 0;
    j->progress = 0;
    if( audio_load_progress( &j->clip, j->path, j->err, sizeof(j->err),
                             &j->progress ) ) {
        j->error = 1;
        j->done  = 1;
        return NULL;
    }
    j->phase    = 1;
    j->progress = 0;
    waveCacheBuild( &j->cache, &j->clip, &j->progress );
    j->error = 0;
    j->done  = 1;
    return NULL;
}

/* -------------------------------------------------------------------- */
/* helpers                                                              */
/* -------------------------------------------------------------------- */

static long clipLen( void ) { return app.clip.numFrames; }

static int hasSelection( void ) { return app.selEnd > app.selStart; }

/* mark the waveform sample data as changed so the overview pyramid is
 * rebuilt (fully) before the next render.  Use for structural edits that
 * change the clip length. */
static void bumpWave( void ) { app.waveGen++; }

/* Fast path for a length-preserving in-place edit of frames [start,end):
 * patch just the affected pyramid bins instead of rebuilding the whole
 * thing.  Falls back to a full rebuild if the cache geometry no longer
 * matches the clip (e.g. first edit before any cache was built). */
static void waveUpdateRange( long start, long end )
{
    if( app.wave.valid &&
        app.waveBuiltGen     == app.waveGen &&   /* cache current, no rebuild pending */
        app.wave.numFrames   == app.clip.numFrames &&
        app.wave.numChannels == app.clip.numChannels ) {
        waveCacheUpdateRange( &app.wave, &app.clip, start, end );
        /* keep the generation counters in lock-step so renderWaveform does
         * NOT trigger a redundant full rebuild this frame */
        app.waveGen++;
        app.waveBuiltGen = app.waveGen;
    } else {
        bumpWave();
    }
}

/* Fast path for a length-CHANGING structural edit (insert / delete / cut /
 * paste / undo / redo) whose change begins at frame 'from' and runs to the new
 * end, leaving [0,from) untouched: patch only the affected tail of the pyramid
 * instead of rebuilding the whole file.  This is what makes paste / delete at
 * the end of a long file instant (the buffer op itself is already ~O(1); the
 * old bumpWave() full rebuild was the real cost).  Falls back to a deferred
 * full rebuild when the cache is not in a patchable state. */
static void waveReplaceTail( long from )
{
    if( app.wave.valid &&
        app.waveBuiltGen     == app.waveGen &&   /* cache current, no rebuild pending */
        app.wave.numChannels == app.clip.numChannels ) {
        waveCacheReplaceTail( &app.wave, &app.clip, from );
        /* keep generation counters in lock-step so renderWaveform does NOT
         * trigger a redundant full rebuild this frame */
        app.waveGen++;
        app.waveBuiltGen = app.waveGen;
    } else {
        bumpWave();
    }
}

/* the range effects act on: the selection, or the whole clip */
static void effectRange( long *s, long *e )
{
    if( hasSelection() ) { *s = app.selStart; *e = app.selEnd; }
    else                 { *s = 0;            *e = clipLen(); }
}

static void setStatus( const char *msg )
{
    strncpy( app.statusMsg, msg, sizeof(app.statusMsg) - 1 );
    app.statusMsg[sizeof(app.statusMsg) - 1] = '\0';
}

/* -------------------------------------------------------------------- */
/* marks                                                                */
/* -------------------------------------------------------------------- */

/* hand out the next caret hue, advancing the cycle.  All marks belonging to
 * one operation (a selection's two edges, a paste's two seams) should share a
 * single value from this so they read as a group. */
static int newMarkColor( void )
{
    int c = app.markColorNext % MARK_NUM_COLORS;
    app.markColorNext++;
    return c;
}

/* add a mark at 'frame' with hue 'color' (deduped against an existing mark at
 * the same frame).  Silently ignored if the mark table is full. */
static void addMark( long frame, int color )
{
    int i;
    if( frame < 0 ) frame = 0;
    if( frame > clipLen() ) frame = clipLen();
    for( i = 0; i < app.numMarks; i++ )
        if( app.marks[i].frame == frame ) return;   /* already marked */
    if( app.numMarks >= MAX_MARKS ) return;
    app.marks[app.numMarks].frame    = frame;
    app.marks[app.numMarks].selected = 0;
    app.marks[app.numMarks].color    = color;
    app.numMarks++;
}

/* drop the mark at index i */
static void removeMarkAt( int i )
{
    int k;
    if( i < 0 || i >= app.numMarks ) return;
    for( k = i; k < app.numMarks - 1; k++ ) app.marks[k] = app.marks[k + 1];
    app.numMarks--;
}

/* delete all marks flagged selected */
static void deleteSelectedMarks( void )
{
    int i = 0, removed = 0;
    while( i < app.numMarks ) {
        if( app.marks[i].selected ) { removeMarkAt( i ); removed++; }
        else i++;
    }
    if( removed ) setStatus( "Deleted selected marks" );
    else          setStatus( "No marks selected" );
}

/* delete every mark whose frame lies within the current selection [s,e].
 * Lets you drag a region and drop all its carets at once, without having to
 * click each one individually first. */
static void clearMarksInSelection( void )
{
    long s = app.selStart, e = app.selEnd;
    int  i = 0, removed = 0;
    while( i < app.numMarks ) {
        long f = app.marks[i].frame;
        if( f >= s && f <= e ) { removeMarkAt( i ); removed++; }
        else i++;
    }
    if( removed ) setStatus( "Cleared marks in selection" );
    else          setStatus( "No marks in selection" );
}

static void clearAllMarks( void )
{
    app.numMarks = 0;
    setStatus( "Cleared all marks" );
}

/* shift marks after an insertion of 'len' frames at 'at' */
static void marksAdjustInsert( long at, long len )
{
    int i;
    for( i = 0; i < app.numMarks; i++ )
        if( app.marks[i].frame >= at ) app.marks[i].frame += len;
}

/* shift marks after deletion of frames [s,e); marks inside collapse to s */
static void marksAdjustDelete( long s, long e )
{
    int i;
    long span = e - s;
    for( i = 0; i < app.numMarks; i++ ) {
        long f = app.marks[i].frame;
        if( f <= s )      continue;
        else if( f >= e ) app.marks[i].frame = f - span;
        else              app.marks[i].frame = s;
    }
}

/* clamp all marks into [0,clipLen]; used after undo/redo where the exact
 * pre-edit geometry is not tracked */
static void marksClampToClip( void )
{
    int i = 0;
    long n = clipLen();
    while( i < app.numMarks ) {
        if( app.marks[i].frame > n ) app.marks[i].frame = n;
        i++;
    }
}

/* stop playback safely (under audio lock) */
static void stopPlayback( void )
{
    audio_lock();
    player.playing = 0;
    audio_unlock();
}

/* After an edit shifts / resizes the clip, keep any in-progress playback
 * alive and valid instead of stopping it: re-clamp the loop range and
 * playhead (under the audio lock) so the callback keeps producing sound
 * rather than running off the end of moved data.  When looping a selection
 * (followSel) the loop re-syncs to the edited selection.  No-op when idle. */
static void refreshPlayback( void )
{
    long n = clipLen();
    audio_lock();
    if( player.playing ) {
        if( player.followSel && hasSelection() ) {
            player.playStart = app.selStart;
            player.playEnd   = app.selEnd;
        }
        if( player.playStart < 0 ) player.playStart = 0;
        if( player.playStart > n ) player.playStart = n;
        if( player.playEnd   > n ) player.playEnd   = n;
        if( player.playEnd <= player.playStart ) player.playEnd = n;
        if( player.playhead <  player.playStart ||
            player.playhead >= player.playEnd )
            player.playhead = player.playStart;
    }
    audio_unlock();
}

/* -------------------------------------------------------------------- */
/* undo / redo -- delta records (see UndoRec)                           */
/* -------------------------------------------------------------------- */

/* free the sample copies held by an undo record and reset it to empty */
static void undoRecFree( UndoRec *r )
{
    int ch;
    for( ch = 0; ch < AUDIO_MAX_CHANNELS; ch++ ) {
        if( r->oldData[ch] ) free( r->oldData[ch] );
        if( r->newData[ch] ) free( r->newData[ch] );
        r->oldData[ch] = r->newData[ch] = NULL;
    }
    r->oldLen = r->newLen = 0;
}

static void clearUndoRedo( void )
{
    int i;
    for( i = 0; i < app.undoCount; i++ ) undoRecFree( &app.undo[i] );
    for( i = 0; i < app.redoCount; i++ ) undoRecFree( &app.redo[i] );
    app.undoCount = app.redoCount = 0;
}

/* Copy len frames of the current clip starting at 'start' into freshly
 * malloc'd per-channel arrays (dst[ch]).  dst pointers are NULL when len==0. */
static void captureRange( float *dst[AUDIO_MAX_CHANNELS], long start, long len )
{
    int ch, nch = app.clip.numChannels;
    for( ch = 0; ch < AUDIO_MAX_CHANNELS; ch++ ) dst[ch] = NULL;
    if( len <= 0 ) return;
    for( ch = 0; ch < nch; ch++ ) {
        dst[ch] = (float *)malloc( (size_t)len * sizeof(float) );
        if( dst[ch] )
            memcpy( dst[ch], app.clip.channel[ch] + start,
                    (size_t)len * sizeof(float) );
    }
}

/* Record in progress between beginEdit() and commitEdit(). */
static UndoRec pendingUndo;

/* Snapshot the frames [at, at+oldLen) that are about to be replaced.  Call
 * BEFORE mutating the clip; pair with commitEdit() AFTER the mutation. */
static void beginEdit( long at, long oldLen )
{
    memset( &pendingUndo, 0, sizeof(pendingUndo) );
    pendingUndo.at          = at;
    pendingUndo.oldLen      = oldLen;
    pendingUndo.numChannels = app.clip.numChannels;
    captureRange( pendingUndo.oldData, at, oldLen );
}

/* Snapshot the newLen frames now at [at, newLen) and push the completed
 * record onto the undo stack (clearing redo, dropping the oldest at depth). */
static void commitEdit( long newLen )
{
    int i;
    pendingUndo.newLen = newLen;
    captureRange( pendingUndo.newData, pendingUndo.at, newLen );

    for( i = 0; i < app.redoCount; i++ ) undoRecFree( &app.redo[i] );
    app.redoCount = 0;

    if( app.undoCount == UNDO_LEVELS ) {
        undoRecFree( &app.undo[0] );
        for( i = 1; i < UNDO_LEVELS; i++ ) app.undo[i - 1] = app.undo[i];
        app.undoCount--;
    }
    app.undo[app.undoCount] = pendingUndo;    /* record owns the data now */
    app.undoCount++;
    memset( &pendingUndo, 0, sizeof(pendingUndo) );
    app.dirty = 1;
}

static void clampView( void );

static void doUndo( void )
{
    UndoRec r;
    int     i, lenChanged;
    if( app.undoCount == 0 ) { setStatus( "Nothing to undo" ); return; }

    r = app.undo[--app.undoCount];          /* struct copy; owns the data */
    lenChanged = ( r.oldLen != r.newLen );

    /* reverse the edit: at 'at', replace the newLen frames with oldData */
    audio_lock();
    audio_splice( &app.clip, r.at, r.newLen, r.oldData, r.oldLen );
    audio_unlock();

    /* move the record onto the redo stack (it replays the edit unchanged) */
    if( app.redoCount == UNDO_LEVELS ) {
        undoRecFree( &app.redo[0] );
        for( i = 1; i < UNDO_LEVELS; i++ ) app.redo[i - 1] = app.redo[i];
        app.redoCount--;
    }
    app.redo[app.redoCount++] = r;

    /* length-preserving -> patch the touched span; length-changing -> patch
     * only the rewritten tail from r.at (both avoid a full rebuild) */
    if( lenChanged ) waveReplaceTail( r.at );
    else             waveUpdateRange( r.at, r.at + r.oldLen );

    if( app.selStart > clipLen() ) app.selStart = clipLen();
    if( app.selEnd   > clipLen() ) app.selEnd   = clipLen();
    marksClampToClip();
    clampView();
    refreshPlayback();
    setStatus( "Undo" );
}

static void doRedo( void )
{
    UndoRec r;
    int     i, lenChanged;
    if( app.redoCount == 0 ) { setStatus( "Nothing to redo" ); return; }

    r = app.redo[--app.redoCount];
    lenChanged = ( r.oldLen != r.newLen );

    /* replay the edit: at 'at', replace the oldLen frames with newData */
    audio_lock();
    audio_splice( &app.clip, r.at, r.oldLen, r.newData, r.newLen );
    audio_unlock();

    if( app.undoCount == UNDO_LEVELS ) {
        undoRecFree( &app.undo[0] );
        for( i = 1; i < UNDO_LEVELS; i++ ) app.undo[i - 1] = app.undo[i];
        app.undoCount--;
    }
    app.undo[app.undoCount++] = r;

    if( lenChanged ) waveReplaceTail( r.at );
    else             waveUpdateRange( r.at, r.at + r.newLen );

    if( app.selStart > clipLen() ) app.selStart = clipLen();
    if( app.selEnd   > clipLen() ) app.selEnd   = clipLen();
    marksClampToClip();
    clampView();
    refreshPlayback();
    setStatus( "Redo" );
}

/* -------------------------------------------------------------------- */
/* view management                                                      */
/* -------------------------------------------------------------------- */

static void clampView( void )
{
    long n = clipLen();
    if( app.samplesPerPixel < 0.01 ) app.samplesPerPixel = 0.01;
    if( n <= 0 ) { app.viewStart = 0; return; }
    {
        double maxSpp = (double)n; /* at least whole file fits-ish */
        if( app.samplesPerPixel > maxSpp && maxSpp > 0 )
            app.samplesPerPixel = maxSpp;
    }
    {
        /* allow scrolling EDGE_PAD_PX pixels past either end so the very first
         * and last samples can be pulled clear of the window edge; the extra
         * room shows up as a black region (drawn in renderWaveform) marking
         * that you have reached the boundary of the data. */
        double pad     = (double)EDGE_PAD_PX * app.samplesPerPixel;
        double visible = app.samplesPerPixel * ( app.wfW > 1 ? app.wfW : 1000.0 );
        double minStart = -pad;
        double maxStart = (double)n - visible + pad;
        if( maxStart < minStart ) maxStart = minStart;
        if( app.viewStart < minStart ) app.viewStart = minStart;
        if( app.viewStart > maxStart ) app.viewStart = maxStart;
    }
}

/* scroll (without changing zoom) so that 'frame' sits comfortably inside the
 * visible window, leaving a small screen-space margin so it is not jammed
 * against an edge.  Used e.g. after a paste so the cursor at the end of the
 * newly-inserted audio stays on screen -- repeated pastes keep scrolling to
 * follow the growing end of the file. */
static void ensureFrameVisible( long frame )
{
    double spp    = app.samplesPerPixel;
    double w      = ( app.wfW > 1 ? app.wfW : 1000.0 );
    double margin = 40.0 * spp;              /* 40px of breathing room       */
    double visible = spp * w;
    double lo = app.viewStart + margin;
    double hi = app.viewStart + visible - margin;
    if( (double)frame < lo ) app.viewStart = (double)frame - margin;
    else if( (double)frame > hi ) app.viewStart = (double)frame - visible + margin;
    clampView();
}

static void zoomFit( void )
{
    long n = clipLen();
    float w = app.wfW > 1 ? app.wfW : 1000.0f;
    if( n <= 0 ) { app.viewStart = 0; app.samplesPerPixel = 1; return; }
    app.viewStart = 0;
    app.samplesPerPixel = (double)n / (double)w;
    if( app.samplesPerPixel < 0.01 ) app.samplesPerPixel = 0.01;
}

static void zoomSelection( void )
{
    float w = app.wfW > 1 ? app.wfW : 1000.0f;
    if( !hasSelection() ) return;
    app.viewStart = app.selStart;
    app.samplesPerPixel = (double)( app.selEnd - app.selStart ) / (double)w;
    if( app.samplesPerPixel < 0.01 ) app.samplesPerPixel = 0.01;
    clampView();
}

/* convert a pixel x within the waveform region to a frame index */
static long pixelToFrame( float px )
{
    double f = app.viewStart + (double)( px - app.wfX ) * app.samplesPerPixel;
    long fr = (long)( f + 0.5 );
    if( fr < 0 ) fr = 0;
    if( fr > clipLen() ) fr = clipLen();
    return fr;
}

/* convert a frame to a pixel x (may be off-screen) */
static float frameToPixel( long fr )
{
    return app.wfX + (float)( ( (double)fr - app.viewStart ) /
                              app.samplesPerPixel );
}

/* If a mark sits within MARK_SNAP_PX screen pixels of frame 'f', return
 * that mark's frame so the selection edge snaps to it; else return f. */
static long snapFrameToMark( long f )
{
    int   i, best = -1;
    float fpx = frameToPixel( f );
    float bestDist = MARK_SNAP_PX;
    for( i = 0; i < app.numMarks; i++ ) {
        float d = frameToPixel( app.marks[i].frame ) - fpx;
        if( d < 0 ) d = -d;
        if( d <= bestDist ) { bestDist = d; best = i; }
    }
    return ( best >= 0 ) ? app.marks[best].frame : f;
}

/* -------------------------------------------------------------------- */
/* transport control                                                    */
/* -------------------------------------------------------------------- */

static void playFrom( long from, long to )
{
    open_audio( app.clip.sampleRate );
    if( clipLen() == 0 ) return;
    if( from < 0 ) from = 0;
    if( to > clipLen() ) to = clipLen();
    if( to <= from ) to = clipLen();
    audio_lock();
    player.clip      = &app.clip;
    player.playStart = from;
    player.playEnd   = to;
    player.playhead  = from;
    player.playing   = 1;
    player.followSel = 0;
    audio_unlock();
}

static void togglePlay( void )
{
    if( player.playing ) { stopPlayback(); return; }
    if( hasSelection() ) {
        playFrom( app.selStart, app.selEnd );
        player.followSel = 1;   /* let selection edits move the loop live */
    } else {
        playFrom( app.selStart, clipLen() );
        player.followSel = 0;
    }
}

static long playheadFrame( void )
{
    long h;
    audio_lock();
    h = player.playing ? player.playhead : -1;
    audio_unlock();
    return h;
}

/* -------------------------------------------------------------------- */
/* editing actions                                                      */
/* -------------------------------------------------------------------- */

static void actSelectAll( void )
{
    app.selStart = 0;
    app.selEnd   = clipLen();
}

static void actSelectNone( void )
{
    app.selEnd = app.selStart;
}

/* create marks manually: around the selection, or at the bare cursor */
static void actAddMark( void )
{
    int c = newMarkColor();
    if( clipLen() <= 0 ) return;
    if( hasSelection() ) {
        addMark( app.selStart, c );
        addMark( app.selEnd,   c );
        setStatus( "Marked selection edges" );
    } else {
        addMark( app.selStart, c );
        setStatus( "Added mark at cursor" );
    }
}

static void actDelete( void )
{
    long s, e;
    if( !hasSelection() ) { setStatus( "No selection to delete" ); return; }
    s = app.selStart; e = app.selEnd;
    beginEdit( s, e - s );
    audio_lock();
    audio_delete_range( &app.clip, s, e );
    audio_unlock();
    commitEdit( 0 );
    waveReplaceTail( s );             /* only frames from s changed */
    marksAdjustDelete( s, e );
    addMark( s, newMarkColor() );     /* mark the seam left behind */
    app.selEnd = app.selStart = s;
    clampView();
    refreshPlayback();
    setStatus( "Deleted selection" );
}

static void actCopy( void )
{
    long s, e;
    if( !hasSelection() ) { setStatus( "No selection to copy" ); return; }
    s = app.selStart; e = app.selEnd;
    audio_copy_range( &app.clipboard, &app.clip, s, e );
    setStatus( "Copied selection" );
}

static void actCut( void )
{
    long s, e;
    if( !hasSelection() ) { setStatus( "No selection to cut" ); return; }
    s = app.selStart; e = app.selEnd;
    audio_copy_range( &app.clipboard, &app.clip, s, e );
    beginEdit( s, e - s );
    audio_lock();
    audio_delete_range( &app.clip, s, e );
    audio_unlock();
    commitEdit( 0 );
    waveReplaceTail( s );             /* only frames from s changed */
    marksAdjustDelete( s, e );
    addMark( s, newMarkColor() );     /* mark the seam left behind */
    app.selEnd = app.selStart = s;
    clampView();
    refreshPlayback();
    setStatus( "Cut selection" );
}

static void actPaste( void )
{
    long at, insN, selS, selE;
    int  hadSel, c;
    if( app.clipboard.numFrames == 0 ) { setStatus( "Clipboard is empty" ); return; }
    insN   = app.clipboard.numFrames;
    hadSel = hasSelection();
    selS   = app.selStart;
    selE   = app.selEnd;
    at     = selS;
    /* one splice: the (optional) replaced selection out, the clipboard in */
    beginEdit( at, hadSel ? ( selE - selS ) : 0 );
    audio_lock();
    if( hadSel ) audio_delete_range( &app.clip, selS, selE );
    audio_insert( &app.clip, at, &app.clipboard );
    audio_unlock();
    commitEdit( insN );
    waveReplaceTail( at );            /* frames [0,at) unchanged; tail rewritten */
    if( hadSel ) marksAdjustDelete( selS, selE );
    marksAdjustInsert( at, insN );
    c = newMarkColor();
    addMark( at,        c );          /* paste start seam */
    addMark( at + insN, c );          /* paste end seam   */
    /* leave the pasted audio UN-selected and drop the cursor at its end so a
     * run of pastes lays clips down end-to-end like train cars; scroll to keep
     * that advancing cursor on screen (following the end of a growing file) */
    app.selStart = app.selEnd = at + insN;
    clampView();
    ensureFrameVisible( at + insN );
    refreshPlayback();
    setStatus( "Pasted" );
}

static void actTrim( void )
{
    long s, e;
    if( !hasSelection() ) { setStatus( "No selection to trim to" ); return; }
    s = app.selStart; e = app.selEnd;
    /* trim discards everything outside [s,e); the undo record therefore has
     * to hold the whole pre-trim clip (inherently O(n) -- unavoidable) */
    beginEdit( 0, clipLen() );
    audio_lock();
    audio_trim_to_range( &app.clip, s, e );
    audio_unlock();
    commitEdit( clipLen() );
    bumpWave();
    /* remap marks into the kept range [s,e); drop any that fall outside */
    {
        int i = 0;
        while( i < app.numMarks ) {
            long f = app.marks[i].frame;
            if( f < s || f > e ) removeMarkAt( i );
            else { app.marks[i].frame = f - s; i++; }
        }
    }
    app.selStart = 0;
    app.selEnd   = clipLen();
    zoomFit();
    refreshPlayback();
    setStatus( "Trimmed to selection" );
}

static void applyEffect( int which )
{
    long s, e;
    int  onSelection = hasSelection();
    effectRange( &s, &e );
    if( e <= s ) { setStatus( "Nothing to process" ); return; }
    beginEdit( s, e - s );
    audio_lock();
    switch( which ) {
        case 0: {
            /* normalize target is specified in dBFS */
            float peak = (float)pow( 10.0, (double)app.dlgNormDb / 20.0 );
            audio_normalize( &app.clip, s, e, peak );
            break;
        }
        case 1: audio_amplify( &app.clip, s, e, app.dlgGain );        break;
        case 2: audio_fade_in( &app.clip, s, e );                     break;
        case 3: audio_fade_out( &app.clip, s, e );                    break;
        case 4: audio_silence( &app.clip, s, e );                     break;
        case 5: audio_reverse( &app.clip, s, e );                     break;
    }
    audio_unlock();
    commitEdit( e - s );
    /* these effects preserve length, so patch only the edited span of the
     * overview pyramid rather than rebuilding the whole (possibly huge) file */
    waveUpdateRange( s, e );
    /* leave marks at the edges of the processed region so the seam is kept */
    if( onSelection ) {
        int c = newMarkColor();
        addMark( s, c ); addMark( e, c );
    }
    refreshPlayback();
    switch( which ) {
        case 0: setStatus( "Normalized" ); break;
        case 1: setStatus( "Amplified" ); break;
        case 2: setStatus( "Faded in" ); break;
        case 3: setStatus( "Faded out" ); break;
        case 4: setStatus( "Silenced" ); break;
        case 5: setStatus( "Reversed" ); break;
    }
}

/* -------------------------------------------------------------------- */
/* file operations                                                      */
/* -------------------------------------------------------------------- */

static void newFile( void )
{
    stopPlayback();
    audio_lock();
    audio_free( &app.clip );
    audio_alloc( &app.clip, 1, 44100, 0 );
    audio_unlock();
    bumpWave();
    clearUndoRedo();       /* stale deltas would splice into the empty clip */
    app.numMarks = 0;
    app.markColorNext = 0;
    app.selStart = app.selEnd = 0;
    app.path[0] = '\0';
    app.dirty = 0;
    zoomFit();
    setStatus( "New empty file" );
}

/* Kick off a background load.  The main loop shows a progress bar and
 * calls finishLoad() when the worker signals done. */
static void startLoad( const char *path )
{
    if( loading ) return;   /* one load at a time */
    stopPlayback();
    memset( &loadJob, 0, sizeof(loadJob) );
    strncpy( loadJob.path, path, PATH_MAX_LEN - 1 );
    loadJob.path[PATH_MAX_LEN - 1] = '\0';
    if( pthread_create( &loadThread, NULL, loadWorker, &loadJob ) != 0 ) {
        setStatus( "Could not start loader thread" );
        return;
    }
    loading = 1;
    setStatus( "Loading..." );
}

/* Called on the main thread once the worker has finished. */
static void finishLoad( void )
{
    pthread_join( loadThread, NULL );
    loading = 0;

    if( loadJob.error ) {
        audio_free( &loadJob.clip );
        waveCacheFree( &loadJob.cache );
        setStatus( loadJob.err );
        strncpy( app.pendingPopup, "Error", sizeof(app.pendingPopup) - 1 );
        return;
    }

    /* swap the freshly decoded clip + its prebuilt overview into place */
    audio_lock();
    audio_free( &app.clip );
    app.clip = loadJob.clip;
    audio_unlock();
    memset( &loadJob.clip, 0, sizeof(loadJob.clip) );

    waveCacheFree( &app.wave );
    app.wave = loadJob.cache;
    memset( &loadJob.cache, 0, sizeof(loadJob.cache) );
    app.waveGen      = app.waveGen + 1;
    app.waveBuiltGen = app.waveGen;   /* cache already matches, no rebuild */

    open_audio( app.clip.sampleRate );

    app.numMarks = 0;
    app.markColorNext = 0;
    app.selStart = app.selEnd = 0;
    strncpy( app.path, loadJob.path, PATH_MAX_LEN - 1 );
    app.path[PATH_MAX_LEN - 1] = '\0';
    app.dirty = 0;
    clearUndoRedo();
    zoomFit();
    setStatus( "Loaded file" );
}

static int endsWithOgg( const char *p )
{
    size_t n = strlen( p );
    return n >= 4 &&
           ( p[n-4] == '.' ) &&
           ( p[n-3] == 'o' || p[n-3] == 'O' ) &&
           ( p[n-2] == 'g' || p[n-2] == 'G' ) &&
           ( p[n-1] == 'g' || p[n-1] == 'G' );
}

static void saveFile( const char *path )
{
    char err[256];
    int rc;
    if( endsWithOgg( path ) ) {
        setStatus( "OGG export not supported (decode only) - save as WAV" );
        strncpy( app.pendingPopup, "Error", sizeof(app.pendingPopup) - 1 );
        return;
    }
    rc = audio_save_wav( &app.clip, path, err, sizeof(err) );
    if( rc ) {
        setStatus( err );
        strncpy( app.pendingPopup, "Error", sizeof(app.pendingPopup) - 1 );
        return;
    }
    strncpy( app.path, path, PATH_MAX_LEN - 1 );
    app.path[PATH_MAX_LEN - 1] = '\0';
    app.dirty = 0;
    setStatus( "Saved WAV" );
}

/* -------------------------------------------------------------------- */
/* waveform rendering (legacy OpenGL)                                   */
/* -------------------------------------------------------------------- */

static void drawRect( float x0, float y0, float x1, float y1,
                      float r, float g, float b, float a )
{
    glColor4f( r, g, b, a );
    glBegin( GL_QUADS );
        glVertex2f( x0, y0 );
        glVertex2f( x1, y0 );
        glVertex2f( x1, y1 );
        glVertex2f( x0, y1 );
    glEnd();
}

static void drawVLine( float x, float y0, float y1,
                       float r, float g, float b, float a )
{
    glColor4f( r, g, b, a );
    glBegin( GL_LINES );
        glVertex2f( x, y0 );
        glVertex2f( x, y1 );
    glEnd();
}

/* pixel-space orthographic projection, y increasing downward */
static void setPixelProjection( int winW, int winH )
{
    glViewport( 0, 0, winW, winH );
    glMatrixMode( GL_PROJECTION );
    glLoadIdentity();
    glOrtho( 0.0, (double)winW, (double)winH, 0.0, -1.0, 1.0 );
    glMatrixMode( GL_MODELVIEW );
    glLoadIdentity();
    glDisable( GL_TEXTURE_2D );
    glEnable( GL_BLEND );
    glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
}

/* the mini overview / scroll bar under the menus: whole file at a glance
 * with a draggable box marking the region shown in the main view. */
static void renderOverview( int winW )
{
    float x0 = app.ovX, y0 = app.ovY, w = app.ovW, h = app.ovH;
    float yc = y0 + h * 0.5f;
    float amp = h * 0.42f;
    long  n = clipLen();
    int   level, px, i;
    double spp;
    (void)winW;

    /* background + frame */
    drawRect( x0, y0, x0 + w, y0 + h, 0.06f, 0.07f, 0.09f, 1.0f );
    glColor4f( 0.30f, 0.32f, 0.36f, 1.0f );
    glBegin( GL_LINES );
        glVertex2f( x0, yc ); glVertex2f( x0 + w, yc );
    glEnd();

    if( n > 0 && w > 1.0f ) {
        spp   = (double)n / (double)w;
        level = waveCacheLevel( &app.wave, spp );
        glColor4f( 0.40f, 0.55f, 0.72f, 1.0f );
        glBegin( GL_LINES );
        for( px = 0; px < (int)w; px++ ) {
            double f0 = (double)px * spp;
            double f1 = f0 + spp;
            float  mn = 0.0f, mx = 0.0f, cmn, cmx;
            int    ch, any = 0;
            for( ch = 0; ch < app.clip.numChannels; ch++ ) {
                if( colMinMax( &app.clip, &app.wave, level, ch, f0, f1,
                               &cmn, &cmx ) ) {
                    if( !any ) { mn = cmn; mx = cmx; any = 1; }
                    else { if( cmn < mn ) mn = cmn; if( cmx > mx ) mx = cmx; }
                }
            }
            if( any ) {
                glVertex2f( x0 + (float)px, yc - mx * amp );
                glVertex2f( x0 + (float)px, yc - mn * amp );
            }
        }
        glEnd();

        /* selection tint in the overview -- deliberately more solid than the
         * main view's selection so the region is easy to spot at a glance
         * (edge lines are omitted here to keep the mini bar uncluttered) */
        if( hasSelection() ) {
            float sx0 = x0 + (float)( (double)app.selStart / (double)n ) * w;
            float sx1 = x0 + (float)( (double)app.selEnd   / (double)n ) * w;
            if( sx1 < sx0 + 1.0f ) sx1 = sx0 + 1.0f;
            drawRect( sx0, y0, sx1, y0 + h, 0.95f, 0.85f, 0.20f, 0.32f );
        }

        /* visible-window box */
        {
            double visible = app.samplesPerPixel * ( app.wfW > 1 ? app.wfW : 1 );
            float bx0 = x0 + (float)( app.viewStart / (double)n ) * w;
            float bx1 = x0 + (float)( ( app.viewStart + visible ) / (double)n ) * w;
            if( bx0 < x0 ) bx0 = x0;
            if( bx1 > x0 + w ) bx1 = x0 + w;
            if( bx1 < bx0 + 2.0f ) bx1 = bx0 + 2.0f;
            drawRect( bx0, y0, bx1, y0 + h, 0.95f, 0.95f, 1.0f, 0.14f );
            drawVLine( bx0, y0, y0 + h, 0.9f, 0.95f, 1.0f, 0.9f );
            drawVLine( bx1, y0, y0 + h, 0.9f, 0.95f, 1.0f, 0.9f );
        }

        /* cursor position (yellow) when there is no selection, so the bare
         * caret is locatable in the whole-file view too */
        if( !hasSelection() ) {
            float cx = x0 + (float)( (double)app.selStart / (double)n ) * w;
            drawVLine( cx, y0, y0 + h, 0.95f, 0.9f, 0.3f, 0.9f );
        }

        /* live playhead (green), mirroring the main view's playhead so you can
         * see where playback is within the whole file */
        {
            long ph = playheadFrame();
            if( ph >= 0 ) {
                float gx = x0 + (float)( (double)ph / (double)n ) * w;
                drawVLine( gx, y0, y0 + h, 0.3f, 1.0f, 0.4f, 0.95f );
            }
        }

        /* tiny mark carets along the top edge, in each mark's hue, so the
         * whole-file position of every mark is visible even when zoomed in */
        for( i = 0; i < app.numMarks; i++ ) {
            const float *col = markColors[ app.marks[i].color % MARK_NUM_COLORS ];
            float mx = x0 + (float)( (double)app.marks[i].frame /
                                     (double)n ) * w;
            glColor4f( col[0], col[1], col[2], 0.95f );
            glBegin( GL_TRIANGLES );
                glVertex2f( mx - 3.0f, y0 + 1.0f );
                glVertex2f( mx + 3.0f, y0 + 1.0f );
                glVertex2f( mx,        y0 + 6.0f );
            glEnd();
        }
    }

    /* bottom divider */
    glColor4f( 0.25f, 0.27f, 0.30f, 1.0f );
    glBegin( GL_LINES );
        glVertex2f( x0, y0 + h ); glVertex2f( x0 + w, y0 + h );
    glEnd();
}

/* draw the mark carets in the top strip plus faint guide lines down the
 * waveform.  stripTop is app.wfY; the carets live in the first
 * MARK_STRIP_H pixels, guides run to the bottom of the wave area. */
static void renderMarks( void )
{
    int   i;
    float x0 = app.wfX, x1 = app.wfX + app.wfW;
    float stripTop = app.wfY;
    float stripBot = app.wfY + MARK_STRIP_H;
    float waveBot  = app.wfY + app.wfH;

    for( i = 0; i < app.numMarks; i++ ) {
        float px = frameToPixel( app.marks[i].frame );
        int   sel = app.marks[i].selected;
        float hw  = 5.0f;
        const float *col = markColors[ app.marks[i].color % MARK_NUM_COLORS ];
        float r = col[0], g = col[1], b = col[2];
        if( px < x0 - hw || px > x1 + hw ) continue;

        /* faint vertical guide the height of the wave, in the mark's hue */
        drawVLine( px, stripBot, waveBot, r, g, b, sel ? 0.5f : 0.22f );

        /* caret: downward triangle sitting in the strip, tip at the line.
         * a selected caret is brightened toward white so it reads as picked */
        if( sel ) glColor4f( 0.5f + r * 0.5f, 0.5f + g * 0.5f,
                             0.5f + b * 0.5f, 1.0f );
        else      glColor4f( r, g, b, 1.0f );
        glBegin( GL_TRIANGLES );
            glVertex2f( px - hw, stripTop + 1.0f );
            glVertex2f( px + hw, stripTop + 1.0f );
            glVertex2f( px,      stripBot - 2.0f );
        glEnd();
        /* outline selected carets so they read as picked */
        if( sel ) {
            glColor4f( 1.0f, 1.0f, 1.0f, 0.9f );
            glBegin( GL_LINE_LOOP );
                glVertex2f( px - hw, stripTop + 1.0f );
                glVertex2f( px + hw, stripTop + 1.0f );
                glVertex2f( px,      stripBot - 2.0f );
            glEnd();
        }
    }
}

static void renderWaveform( int winW, int winH )
{
    float menuH  = gui_main_menu_bar_height();
    float top    = menuH + OVERVIEW_H;
    float bottom = (float)winH - TRANSPORT_H;
    int   nch    = app.clip.numChannels;
    int   ch;
    float laneH, laneTop, laneAreaH;
    long  n = clipLen();

    /* rebuild the overview pyramid if the sample data changed */
    if( !loading && app.waveGen != app.waveBuiltGen ) {
        waveCacheBuild( &app.wave, &app.clip, NULL );
        app.waveBuiltGen = app.waveGen;
    }

    app.ovX = 0.0f;
    app.ovY = menuH;
    app.ovW = (float)winW;
    app.ovH = OVERVIEW_H;

    app.wfX = 0.0f;
    app.wfY = top;
    app.wfW = (float)winW;
    app.wfH = bottom - top;
    if( app.wfH < 10.0f ) app.wfH = 10.0f;
    if( nch < 1 ) nch = 1;
    /* reserve a thin strip along the top for the mark carets */
    laneTop   = app.wfY + MARK_STRIP_H;
    laneAreaH = app.wfH - MARK_STRIP_H;
    if( laneAreaH < 10.0f ) laneAreaH = 10.0f;
    laneH = laneAreaH / (float)nch;

    setPixelProjection( winW, winH );

    /* overall background of waveform area */
    drawRect( app.wfX, app.wfY, app.wfX + app.wfW, app.wfY + app.wfH,
              0.10f, 0.11f, 0.13f, 1.0f );

    for( ch = 0; ch < nch; ch++ ) {
        float y0 = laneTop + laneH * (float)ch;
        float yc = y0 + laneH * 0.5f;
        float amp = laneH * 0.45f;
        float *samples = app.clip.channel[ch];
        int   px;

        /* lane separator */
        if( ch > 0 ) {
            glColor4f( 0.25f, 0.27f, 0.30f, 1.0f );
            glBegin( GL_LINES );
                glVertex2f( app.wfX, y0 );
                glVertex2f( app.wfX + app.wfW, y0 );
            glEnd();
        }

        /* center line */
        glColor4f( 0.30f, 0.32f, 0.36f, 1.0f );
        glBegin( GL_LINES );
            glVertex2f( app.wfX, yc );
            glVertex2f( app.wfX + app.wfW, yc );
        glEnd();

        if( n <= 0 || samples == NULL ) continue;

        if( app.samplesPerPixel >= 1.0 ) {
            /* peak (min/max) rendering: one vertical line per column,
             * summarized through the pyramid when zoomed out on a big file */
            int level = waveCacheLevel( &app.wave, app.samplesPerPixel );
            glColor4f( 0.45f, 0.72f, 0.95f, 1.0f );
            glBegin( GL_LINES );
            for( px = 0; px < (int)app.wfW; px++ ) {
                double f0 = app.viewStart + (double)px * app.samplesPerPixel;
                double f1 = f0 + app.samplesPerPixel;
                float  mn, mx;
                if( f0 >= (double)n ) break;
                if( colMinMax( &app.clip, &app.wave, level, ch,
                               f0, f1, &mn, &mx ) ) {
                    glVertex2f( (float)px, yc - mx * amp );
                    glVertex2f( (float)px, yc - mn * amp );
                }
            }
            glEnd();
        } else {
            /* zoomed in: connect individual samples */
            long i0 = (long)app.viewStart - 1;
            long i1 = (long)( app.viewStart +
                              app.samplesPerPixel * app.wfW ) + 2;
            long i;
            if( i0 < 0 ) i0 = 0;
            if( i1 > n ) i1 = n;
            glColor4f( 0.45f, 0.72f, 0.95f, 1.0f );
            glBegin( GL_LINE_STRIP );
            for( i = i0; i < i1; i++ ) {
                float x = frameToPixel( i );
                glVertex2f( x, yc - samples[i] * amp );
            }
            glEnd();
            /* sample dots when very zoomed */
            if( app.samplesPerPixel < 0.25 ) {
                glColor4f( 0.85f, 0.9f, 1.0f, 1.0f );
                glPointSize( 3.0f );
                glBegin( GL_POINTS );
                for( i = i0; i < i1; i++ ) {
                    float x = frameToPixel( i );
                    glVertex2f( x, yc - samples[i] * amp );
                }
                glEnd();
            }
        }
    }

    /* black "beyond the data" margins: when scrolled to the very start or end
     * the pad frames outside [0,n] show up as solid black, giving a clear
     * visual signal that this is the boundary of the file.  Painted over the
     * waveform (which clamps to the edge sample there) but under the
     * selection / cursor / marks. */
    if( n > 0 ) {
        float lx = frameToPixel( 0 );
        float rx = frameToPixel( n );
        if( lx > app.wfX )
            drawRect( app.wfX, app.wfY, lx, app.wfY + app.wfH,
                      0.0f, 0.0f, 0.0f, 1.0f );
        if( rx < app.wfX + app.wfW )
            drawRect( rx, app.wfY, app.wfX + app.wfW, app.wfY + app.wfH,
                      0.0f, 0.0f, 0.0f, 1.0f );
    }

    /* selection overlay */
    if( n > 0 ) {
        float sx0 = frameToPixel( app.selStart );
        float sx1 = frameToPixel( app.selEnd );
        if( hasSelection() ) {
            if( sx0 < app.wfX ) sx0 = app.wfX;
            if( sx1 > app.wfX + app.wfW ) sx1 = app.wfX + app.wfW;
            drawRect( sx0, app.wfY, sx1, app.wfY + app.wfH,
                      0.9f, 0.9f, 0.3f, 0.16f );
            drawVLine( sx0, app.wfY, app.wfY + app.wfH, 0.95f, 0.9f, 0.3f, 0.9f );
            drawVLine( sx1, app.wfY, app.wfY + app.wfH, 0.95f, 0.9f, 0.3f, 0.9f );
        } else {
            /* just the cursor */
            drawVLine( sx0, app.wfY, app.wfY + app.wfH, 0.95f, 0.9f, 0.3f, 0.9f );
        }
    }

    /* playhead */
    {
        long h = playheadFrame();
        if( h >= 0 ) {
            float x = frameToPixel( h );
            if( x >= app.wfX && x <= app.wfX + app.wfW )
                drawVLine( x, app.wfY, app.wfY + app.wfH,
                           0.3f, 1.0f, 0.4f, 0.95f );
        }
    }

    /* mark carets + guides on top of the wave */
    renderMarks();

    /* overview / scroll bar across the top */
    renderOverview( winW );
}

/* -------------------------------------------------------------------- */
/* mouse interaction on the waveform                                    */
/* -------------------------------------------------------------------- */

static int   dragging = 0;
static long  dragAnchor = 0;

static int inWaveform( int mx, int my )
{
    return (float)mx >= app.wfX && (float)mx <= app.wfX + app.wfW &&
           (float)my >= app.wfY && (float)my <= app.wfY + app.wfH;
}

/* a click within the caret strip picks the nearest mark (shift toggles /
 * multi-selects); an empty click clears the mark selection */
static void handleMarkStripDown( int mx, int shift )
{
    int   i, k, best = -1;
    float bestDist = 7.0f;   /* pixel pick radius */
    for( i = 0; i < app.numMarks; i++ ) {
        float d = frameToPixel( app.marks[i].frame ) - (float)mx;
        if( d < 0 ) d = -d;
        if( d <= bestDist ) { bestDist = d; best = i; }
    }
    if( best < 0 ) {
        if( !shift )
            for( k = 0; k < app.numMarks; k++ ) app.marks[k].selected = 0;
        return;
    }
    if( shift ) {
        app.marks[best].selected = !app.marks[best].selected;
    } else {
        for( k = 0; k < app.numMarks; k++ ) app.marks[k].selected = 0;
        app.marks[best].selected = 1;
    }
}

static void handleWaveMouseDown( int mx, int my, int shift )
{
    long f;
    if( !inWaveform( mx, my ) ) return;
    /* the top strip belongs to the mark carets, not the audio selection */
    if( (float)my <= app.wfY + MARK_STRIP_H ) {
        handleMarkStripDown( mx, shift );
        return;
    }
    f = snapFrameToMark( pixelToFrame( (float)mx ) );
    if( shift && hasSelection() ) {
        /* extend selection from the nearer edge */
        if( f < app.selStart ) app.selStart = f;
        else                   app.selEnd = f;
        dragAnchor = ( f < ( app.selStart + app.selEnd ) / 2 )
                     ? app.selEnd : app.selStart;
    } else {
        dragAnchor = f;
        app.selStart = app.selEnd = f;
    }
    dragging = 1;
}

static void handleWaveMouseMove( int mx )
{
    long f;
    if( !dragging ) return;
    f = snapFrameToMark( pixelToFrame( (float)mx ) );
    if( f < dragAnchor ) { app.selStart = f; app.selEnd = dragAnchor; }
    else                 { app.selStart = dragAnchor; app.selEnd = f; }
}

static void handleWheel( int mx, int my, float wheelY )
{
    double anchorFrame;
    double factor;
    if( !inWaveform( mx, my ) ) return;
    anchorFrame = app.viewStart + (double)( mx - app.wfX ) * app.samplesPerPixel;
    factor = ( wheelY > 0 ) ? 0.8 : 1.25;
    app.samplesPerPixel *= factor;
    if( app.samplesPerPixel < 0.01 ) app.samplesPerPixel = 0.01;
    /* keep the frame under the cursor fixed */
    app.viewStart = anchorFrame - (double)( mx - app.wfX ) * app.samplesPerPixel;
    clampView();
}

/* -------------------------------------------------------------------- */
/* overview / scroll bar interaction                                    */
/* -------------------------------------------------------------------- */

static int    ovDragging = 0;
static double ovGrabOffset = 0.0;   /* frames from view start to grab point */

static int inOverview( int mx, int my )
{
    return (float)mx >= app.ovX && (float)mx <= app.ovX + app.ovW &&
           (float)my >= app.ovY && (float)my <= app.ovY + app.ovH;
}

static double overviewFrame( int mx )
{
    long n = clipLen();
    double f;
    if( app.ovW <= 1.0f || n <= 0 ) return 0.0;
    f = (double)( mx - app.ovX ) / (double)app.ovW * (double)n;
    if( f < 0 ) f = 0;
    if( f > (double)n ) f = (double)n;
    return f;
}

static void handleOverviewMouseDown( int mx, int my )
{
    double visible, f, vEnd;
    if( !inOverview( mx, my ) || clipLen() <= 0 ) return;
    visible = app.samplesPerPixel * ( app.wfW > 1 ? app.wfW : 1 );
    f       = overviewFrame( mx );
    vEnd    = app.viewStart + visible;
    if( f >= app.viewStart && f <= vEnd ) {
        /* grabbed inside the box: pan, keeping the grab point steady */
        ovGrabOffset = f - app.viewStart;
    } else {
        /* clicked elsewhere: center the view there */
        app.viewStart = f - visible * 0.5;
        ovGrabOffset  = visible * 0.5;
        clampView();
    }
    ovDragging = 1;
}

static void handleOverviewMouseMove( int mx )
{
    if( !ovDragging ) return;
    app.viewStart = overviewFrame( mx ) - ovGrabOffset;
    clampView();
}

/* -------------------------------------------------------------------- */
/* time formatting                                                      */
/* -------------------------------------------------------------------- */

static void fmtTime( long frames, int rate, char *buf, int buflen )
{
    double sec = ( rate > 0 ) ? (double)frames / (double)rate : 0.0;
    int m = (int)( sec / 60.0 );
    double s = sec - m * 60.0;
    snprintf( buf, buflen, "%d:%06.3f", m, s );
}

/* -------------------------------------------------------------------- */
/* file browser (simple, POSIX dirent)                                  */
/* -------------------------------------------------------------------- */

static int nameCmp( const void *a, const void *b )
{
    return strcmp( *(const char * const *)a, *(const char * const *)b );
}

static void browserOpen( int save )
{
    app.browserSave = save;
    if( app.browserDir[0] == '\0' ) {
        if( getcwd( app.browserDir, PATH_MAX_LEN ) == NULL )
            strcpy( app.browserDir, "." );
    }
    app.browserFile[0] = '\0';
    strncpy( app.pendingPopup, save ? "Save File" : "Open File",
             sizeof(app.pendingPopup) - 1 );
}

static void drawFileBrowser( const char *popupId )
{
    DIR *d;
    struct dirent *ent;
    char *names[4096];
    int   count = 0;
    int   i;

    gui_input_text( "Dir", app.browserDir, PATH_MAX_LEN );
    gui_separator();

    if( gui_begin_child( "files", 0.0f, 260.0f, 1 ) ) {
        d = opendir( app.browserDir );
        if( d ) {
            while( ( ent = readdir( d ) ) != NULL && count < 4096 ) {
                if( strcmp( ent->d_name, "." ) == 0 ) continue;
                names[count] = (char *)malloc( strlen( ent->d_name ) + 2 );
                strcpy( names[count], ent->d_name );
                count++;
            }
            closedir( d );
            qsort( names, count, sizeof(char *), nameCmp );
            for( i = 0; i < count; i++ ) {
                char label[PATH_MAX_LEN + 8];
                char full[PATH_MAX_LEN];
                struct dirent *dummy;
                DIR *sub;
                int isDir;
                (void)dummy;
                snprintf( full, sizeof(full), "%s/%s",
                          app.browserDir, names[i] );
                sub = opendir( full );
                isDir = ( sub != NULL );
                if( sub ) closedir( sub );
                snprintf( label, sizeof(label), "%s%s",
                          isDir ? "[DIR] " : "      ", names[i] );
                if( gui_selectable( label, 0 ) ) {
                    if( isDir ) {
                        snprintf( app.browserDir, PATH_MAX_LEN, "%s", full );
                    } else {
                        snprintf( app.browserFile, PATH_MAX_LEN, "%s",
                                  names[i] );
                    }
                }
            }
            for( i = 0; i < count; i++ ) free( names[i] );
        } else {
            gui_text( "(cannot open directory)" );
        }
    }
    gui_end_child();

    gui_separator();
    gui_input_text( "Name", app.browserFile, PATH_MAX_LEN );

    if( gui_button( app.browserSave ? "Save" : "Open" ) ) {
        char full[PATH_MAX_LEN];
        if( app.browserFile[0] ) {
            snprintf( full, sizeof(full), "%s/%s",
                      app.browserDir, app.browserFile );
            gui_close_current_popup();
            if( app.browserSave ) saveFile( full );
            else                  startLoad( full );
        }
    }
    gui_same_line();
    if( gui_button( "Cancel" ) ) gui_close_current_popup();
    (void)popupId;
}

/* -------------------------------------------------------------------- */
/* GUI: menu bar, transport, dialogs                                    */
/* -------------------------------------------------------------------- */

static void buildMenuBar( void )
{
    if( !gui_begin_main_menu_bar() ) return;

    if( gui_begin_menu( "File" ) ) {
        if( gui_menu_item( "New", "Ctrl+N", 1 ) ) newFile();
        if( gui_menu_item( "Open...", "Ctrl+O", 1 ) ) browserOpen( 0 );
        if( gui_menu_item( "Save", "Ctrl+S", app.path[0] != 0 ) )
            saveFile( app.path );
        if( gui_menu_item( "Save As...", NULL, clipLen() > 0 ) )
            browserOpen( 1 );
        gui_separator();
        if( gui_menu_item( "Quit", "Ctrl+Q", 1 ) ) {
            SDL_Event q; q.type = SDL_QUIT; SDL_PushEvent( &q );
        }
        gui_end_menu();
    }

    if( gui_begin_menu( "Edit" ) ) {
        if( gui_menu_item( "Undo", "Ctrl+Z", app.undoCount > 0 ) ) doUndo();
        if( gui_menu_item( "Redo", "Ctrl+Y", app.redoCount > 0 ) ) doRedo();
        gui_separator();
        if( gui_menu_item( "Cut", "Ctrl+X", hasSelection() ) ) actCut();
        if( gui_menu_item( "Copy", "Ctrl+C", hasSelection() ) ) actCopy();
        if( gui_menu_item( "Paste", "Ctrl+V",
                           app.clipboard.numFrames > 0 ) ) actPaste();
        if( gui_menu_item( "Delete", "Del", hasSelection() ) ) actDelete();
        if( gui_menu_item( "Trim to Selection", NULL, hasSelection() ) )
            actTrim();
        gui_separator();
        if( gui_menu_item( "Select All", "Ctrl+A", clipLen() > 0 ) )
            actSelectAll();
        if( gui_menu_item( "Select None", NULL, hasSelection() ) )
            actSelectNone();
        gui_end_menu();
    }

    if( gui_begin_menu( "Effect" ) ) {
        int have = clipLen() > 0;
        if( gui_menu_item( "Normalize...", NULL, have ) )
            strncpy( app.pendingPopup, "Normalize", sizeof(app.pendingPopup)-1 );
        if( gui_menu_item( "Amplify...", NULL, have ) )
            strncpy( app.pendingPopup, "Amplify", sizeof(app.pendingPopup)-1 );
        gui_separator();
        if( gui_menu_item( "Fade In", NULL, have ) ) applyEffect( 2 );
        if( gui_menu_item( "Fade Out", NULL, have ) ) applyEffect( 3 );
        if( gui_menu_item( "Silence", NULL, have ) ) applyEffect( 4 );
        if( gui_menu_item( "Reverse", NULL, have ) ) applyEffect( 5 );
        gui_end_menu();
    }

    if( gui_begin_menu( "Marks" ) ) {
        int have  = clipLen() > 0;
        int anyMk = app.numMarks > 0;
        int anySel = 0, i;
        for( i = 0; i < app.numMarks; i++ )
            if( app.marks[i].selected ) { anySel = 1; break; }
        if( gui_menu_item(
                hasSelection() ? "Mark Selection Edges" : "Add Mark at Cursor",
                "M", have ) )
            actAddMark();
        gui_separator();
        if( gui_menu_item( "Delete Selected Marks", NULL, anySel ) )
            deleteSelectedMarks();
        if( gui_menu_item( "Clear Marks in Selection", NULL,
                           anyMk && hasSelection() ) )
            clearMarksInSelection();
        if( gui_menu_item( "Clear All Marks", NULL, anyMk ) )
            clearAllMarks();
        gui_end_menu();
    }

    if( gui_begin_menu( "View" ) ) {
        if( gui_menu_item( "Zoom In", "+", 1 ) )
            { app.samplesPerPixel *= 0.5; clampView(); }
        if( gui_menu_item( "Zoom Out", "-", 1 ) )
            { app.samplesPerPixel *= 2.0; clampView(); }
        if( gui_menu_item( "Fit in Window", "F", 1 ) ) zoomFit();
        if( gui_menu_item( "Zoom to Selection", NULL, hasSelection() ) )
            zoomSelection();
        gui_end_menu();
    }

    if( gui_begin_menu( "Help" ) ) {
        if( gui_menu_item( "About cwave", NULL, 1 ) )
            strncpy( app.pendingPopup, "About", sizeof(app.pendingPopup)-1 );
        gui_end_menu();
    }

    gui_end_main_menu_bar();
}

static void buildTransport( int winW, int winH )
{
    char buf[256];
    char t1[32], t2[32];
    long selFrames = app.selEnd - app.selStart;

    gui_set_next_window_pos( 0.0f, (float)winH - TRANSPORT_H );
    gui_set_next_window_size( (float)winW, TRANSPORT_H );
    if( gui_begin( "##transport", 1 ) ) {

        if( gui_button( player.playing ? " Stop " : " Play " ) )
            togglePlay();
        gui_same_line();
        if( gui_button( "|< Start" ) ) {
            stopPlayback();
            app.selStart = app.selEnd = 0;
            app.viewStart = 0;
        }
        gui_same_line();
        if( gui_button( "End >|" ) ) {
            stopPlayback();
            app.selStart = app.selEnd = clipLen();
            app.viewStart = clipLen();   /* clampView scrolls back to the end */
            clampView();
        }
        gui_same_line();
        if( gui_button( "Play All" ) ) { stopPlayback(); playFrom( 0, clipLen() ); }
        gui_same_line();
        gui_checkbox( "Loop", &player.loop );
        gui_same_line();
        gui_slider_float( "Vol", &player.volume, 0.0f, 2.0f, "%.2f" );

        gui_spacing();

        fmtTime( app.selStart, app.clip.sampleRate, t1, sizeof(t1) );
        if( hasSelection() ) {
            fmtTime( app.selEnd, app.clip.sampleRate, t2, sizeof(t2) );
            snprintf( buf, sizeof(buf),
                "Sel: %s - %s  (%.3fs, %ld frames)   |   Length: ",
                t1, t2,
                (double)selFrames / ( app.clip.sampleRate > 0 ?
                                      app.clip.sampleRate : 1 ),
                selFrames );
        } else {
            snprintf( buf, sizeof(buf), "Cursor: %s   |   Length: ", t1 );
        }
        gui_text( buf );
        gui_same_line();
        fmtTime( clipLen(), app.clip.sampleRate, t1, sizeof(t1) );
        snprintf( buf, sizeof(buf), "%s  |  %d ch  %d Hz",
                  t1, app.clip.numChannels, app.clip.sampleRate );
        gui_text( buf );

        /* status line */
        snprintf( buf, sizeof(buf), "%s%s   %s",
                  app.path[0] ? app.path : "(untitled)",
                  app.dirty ? " *" : "",
                  app.statusMsg );
        gui_text_colored( 0.7f, 0.85f, 1.0f, buf );
    }
    gui_end();
}

/* centered progress panel shown while a file loads in the background */
static void buildLoadingOverlay( int winW, int winH )
{
    char  base[PATH_MAX_LEN];
    char  msg[PATH_MAX_LEN + 32];
    const char *p = loadJob.path;
    const char *slash = strrchr( p, '/' );
    float w = 420.0f, h = 96.0f;
    int   ph = loadJob.phase;
    int   pr = loadJob.progress;
    float frac;

    strncpy( base, slash ? slash + 1 : p, sizeof(base) - 1 );
    base[sizeof(base) - 1] = '\0';

    /* combined fraction: decode is phase 0 (0..0.7), overview 1 (0.7..1.0) */
    if( ph == 0 ) frac = ( (float)pr / 1000.0f ) * 0.7f;
    else          frac = 0.7f + ( (float)pr / 1000.0f ) * 0.3f;
    if( frac < 0.0f ) frac = 0.0f;
    if( frac > 1.0f ) frac = 1.0f;

    gui_set_next_window_pos( ( (float)winW - w ) * 0.5f,
                             ( (float)winH - h ) * 0.5f );
    gui_set_next_window_size( w, h );
    if( gui_begin( "##loading", 1 ) ) {
        snprintf( msg, sizeof(msg), "Loading %s", base );
        gui_text( msg );
        gui_text( ph == 0 ? "Reading audio data..."
                          : "Building overview..." );
        gui_progress_bar( frac, NULL );
    }
    gui_end();
}

static void buildDialogs( void )
{
    /* open any pending popup */
    if( app.pendingPopup[0] ) {
        gui_open_popup( app.pendingPopup );
        app.pendingPopup[0] = '\0';
    }

    if( gui_begin_popup_modal( "Normalize" ) ) {
        char lbl[64];
        float lin;
        gui_text( "Peak normalize to target level (dBFS):" );
        gui_slider_float( "dB", &app.dlgNormDb, -60.0f, 0.0f, "%.1f dB" );
        gui_input_float( "Exact dB", &app.dlgNormDb );
        if( app.dlgNormDb >  0.0f ) app.dlgNormDb =  0.0f;
        if( app.dlgNormDb < -60.0f ) app.dlgNormDb = -60.0f;
        lin = (float)pow( 10.0, (double)app.dlgNormDb / 20.0 );
        snprintf( lbl, sizeof(lbl), "= %.4f peak amplitude", lin );
        gui_text( lbl );
        gui_spacing();
        if( gui_button( "Apply" ) ) { applyEffect( 0 ); gui_close_current_popup(); }
        gui_same_line();
        if( gui_button( "Cancel" ) ) gui_close_current_popup();
        gui_end_popup();
    }

    if( gui_begin_popup_modal( "Amplify" ) ) {
        gui_text( "Gain multiplier:" );
        gui_slider_float( "Gain", &app.dlgGain, 0.0f, 8.0f, "%.3f" );
        gui_input_float( "Exact", &app.dlgGain );
        if( gui_button( "Apply" ) ) { applyEffect( 1 ); gui_close_current_popup(); }
        gui_same_line();
        if( gui_button( "Cancel" ) ) gui_close_current_popup();
        gui_end_popup();
    }

    if( gui_begin_popup_modal( "About" ) ) {
        gui_text( "cwave -- a fast, stable audio editor" );
        gui_text( "" );
        gui_text( "Pure C89 + SDL2 + OpenGL + Dear ImGui." );
        gui_text( "WAV read/write, OGG read." );
        gui_text( "" );
        gui_text( "Space: play/stop   F: fit   wheel: zoom" );
        gui_text( "Drag: select   Shift+click: extend   M: mark" );
        gui_text( "Click a caret to select a mark; selection" );
        gui_text( "snaps to nearby marks while dragging." );
        gui_spacing();
        if( gui_button( "Close" ) ) gui_close_current_popup();
        gui_end_popup();
    }

    if( gui_begin_popup_modal( "Error" ) ) {
        gui_text_colored( 1.0f, 0.5f, 0.5f, app.statusMsg );
        gui_spacing();
        if( gui_button( "OK" ) ) gui_close_current_popup();
        gui_end_popup();
    }

    if( gui_begin_popup_modal( "Open File" ) ) {
        drawFileBrowser( "Open File" );
        gui_end_popup();
    }
    if( gui_begin_popup_modal( "Save File" ) ) {
        drawFileBrowser( "Save File" );
        gui_end_popup();
    }
}

/* -------------------------------------------------------------------- */
/* keyboard shortcuts                                                   */
/* -------------------------------------------------------------------- */

static void handleKey( SDL_Keycode key, int ctrl )
{
    if( ctrl ) {
        switch( key ) {
            case SDLK_n: newFile(); break;
            case SDLK_o: browserOpen( 0 ); break;
            case SDLK_s:
                if( app.path[0] ) saveFile( app.path );
                else              browserOpen( 1 );
                break;
            case SDLK_z: doUndo(); break;
            case SDLK_y: doRedo(); break;
            case SDLK_x: actCut(); break;
            case SDLK_c: actCopy(); break;
            case SDLK_v: actPaste(); break;
            case SDLK_a: actSelectAll(); break;
            case SDLK_q: { SDL_Event q; q.type = SDL_QUIT;
                           SDL_PushEvent( &q ); } break;
            default: break;
        }
        return;
    }
    switch( key ) {
        case SDLK_SPACE:  togglePlay(); break;
        case SDLK_DELETE:
        case SDLK_BACKSPACE: actDelete(); break;
        case SDLK_f:      zoomFit(); break;
        case SDLK_m:      actAddMark(); break;
        case SDLK_HOME:   app.selStart = app.selEnd = 0;
                          app.viewStart = 0; break;
        case SDLK_END:    app.selStart = app.selEnd = clipLen();
                          app.viewStart = clipLen(); clampView(); break;
        case SDLK_EQUALS:
        case SDLK_PLUS:   app.samplesPerPixel *= 0.5; clampView(); break;
        case SDLK_MINUS:  app.samplesPerPixel *= 2.0; clampView(); break;
        case SDLK_ESCAPE: actSelectNone(); break;
        default: break;
    }
}

/* -------------------------------------------------------------------- */
/* main                                                                 */
/* -------------------------------------------------------------------- */

int main( int argc, char **argv )
{
    SDL_Window   *window;
    SDL_GLContext glctx;
    int running = 1;

    memset( &app, 0, sizeof(app) );
    memset( &player, 0, sizeof(player) );
    player.volume = 1.0f;
    player.loop   = 0;
    app.samplesPerPixel = 1.0;
    app.dlgGain = 2.0f;
    app.dlgNormDb = -1.0f;
    audio_alloc( &app.clip, 1, 44100, 0 );

    if( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_AUDIO ) != 0 ) {
        fprintf( stderr, "SDL_Init failed: %s\n", SDL_GetError() );
        return 1;
    }

    SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
    SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 0 );

    window = SDL_CreateWindow( "cwave",
                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                1100, 640,
                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                SDL_WINDOW_ALLOW_HIGHDPI );
    if( !window ) {
        fprintf( stderr, "CreateWindow failed: %s\n", SDL_GetError() );
        SDL_Quit();
        return 1;
    }
    glctx = SDL_GL_CreateContext( window );
    if( !glctx ) {
        fprintf( stderr, "GL context failed: %s\n", SDL_GetError() );
        SDL_DestroyWindow( window );
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent( window, glctx );
    SDL_GL_SetSwapInterval( 1 ); /* vsync */

    if( gui_init( window, glctx ) ) {
        fprintf( stderr, "GUI init failed\n" );
        return 1;
    }

    open_audio( 44100 );

    if( argc > 1 ) startLoad( argv[1] );
    else           setStatus( "Open a WAV or OGG file to begin" );

    while( running ) {
        SDL_Event ev;
        int winW, winH;
        int mouseCapture, keyCapture;

        while( SDL_PollEvent( &ev ) ) {
            gui_process_event( &ev );
            mouseCapture = gui_want_capture_mouse();
            keyCapture   = gui_want_capture_keyboard();

            switch( ev.type ) {
                case SDL_QUIT: running = 0; break;
                case SDL_KEYDOWN:
                    if( !keyCapture ) {
                        int ctrl = ( ev.key.keysym.mod & KMOD_CTRL ) ? 1 : 0;
                        handleKey( ev.key.keysym.sym, ctrl );
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if( !mouseCapture && !loading &&
                        ev.button.button == SDL_BUTTON_LEFT ) {
                        int shift = ( SDL_GetModState() & KMOD_SHIFT ) ? 1 : 0;
                        if( inOverview( ev.button.x, ev.button.y ) )
                            handleOverviewMouseDown( ev.button.x, ev.button.y );
                        else
                            handleWaveMouseDown( ev.button.x, ev.button.y, shift );
                    }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if( ev.button.button == SDL_BUTTON_LEFT ) {
                        dragging = 0;
                        ovDragging = 0;
                    }
                    break;
                case SDL_MOUSEMOTION:
                    if( ovDragging )    handleOverviewMouseMove( ev.motion.x );
                    else if( dragging ) handleWaveMouseMove( ev.motion.x );
                    break;
                case SDL_MOUSEWHEEL:
                    if( !mouseCapture ) {
                        int mx, my;
                        SDL_GetMouseState( &mx, &my );
                        handleWheel( mx, my, (float)ev.wheel.y );
                    }
                    break;
                default: break;
            }
        }

        /* a background load just finished: swap in the result */
        if( loading && loadJob.done ) finishLoad();

        /* live loop: while playing a selection, track edits to its edges */
        if( player.playing && player.followSel && hasSelection() ) {
            audio_lock();
            player.playStart = app.selStart;
            player.playEnd   = app.selEnd;
            if( player.playhead < player.playStart ||
                player.playhead >= player.playEnd )
                player.playhead = player.playStart;
            audio_unlock();
        }

        SDL_GetWindowSize( window, &winW, &winH );

        /* --- render waveform (raw GL) first, imgui on top --- */
        glClearColor( 0.08f, 0.09f, 0.10f, 1.0f );
        glClear( GL_COLOR_BUFFER_BIT );
        renderWaveform( winW, winH );

        /* --- build and render the GUI --- */
        gui_new_frame( window );
        buildMenuBar();
        buildTransport( winW, winH );
        buildDialogs();
        if( loading ) buildLoadingOverlay( winW, winH );
        gui_render();

        SDL_GL_SwapWindow( window );
    }

    /* if a load is still running at quit, wait for it so we can free */
    if( loading ) { pthread_join( loadThread, NULL );
                    audio_free( &loadJob.clip );
                    waveCacheFree( &loadJob.cache ); }

    stopPlayback();
    waveCacheFree( &app.wave );
    if( audioDev ) SDL_CloseAudioDevice( audioDev );
    gui_shutdown();
    SDL_GL_DeleteContext( glctx );
    SDL_DestroyWindow( window );
    SDL_Quit();
    return 0;
}
