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
#define TABBAR_H      34.0f     /* height of the open-files tab strip     */
#define MAX_TABS      16        /* how many files can be open at once     */
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
/* playback                                                             */
/* -------------------------------------------------------------------- */

typedef struct {
    Sequence *clip;
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

/* The clip is now a block list, so playback keeps a cursor (block index +
 * offset within that block) instead of a flat sample index.  The cursor is
 * located once per callback via a binary search (cheap -- once per ~1024
 * frames) and then advanced per frame, crossing block boundaries and
 * re-seeking on a loop wrap.  Edits happen under audio_lock so the block
 * pointers cannot change mid-callback. */
static void SDLCALL audio_cb( void *ud, Uint8 *stream, int len )
{
    Player   *p = (Player *)ud;
    float    *out = (float *)stream;
    int       frames = len / (int)( 2 * sizeof(float) );
    Sequence *c = p->clip;
    int       i, bIdx = 0, have = 0, nch = 0;
    long      local = 0;

    if( p->playing && c && c->numFrames > 0 ) {
        seq_locate( c, p->playhead, &bIdx, &local );
        have = ( bIdx < c->numBlocks );
        nch  = c->numChannels;
    }
    for( i = 0; i < frames; i++ ) {
        float L = 0.0f, R = 0.0f;
        if( p->playing && have &&
            p->playhead >= 0 && p->playhead < p->playEnd &&
            p->playhead < c->numFrames ) {
            AudioClip *b = &c->blocks[bIdx]->buf;
            if( nch == 1 ) {
                L = R = b->channel[0][local];
            } else {
                int ch;
                L = b->channel[0][local];
                R = b->channel[1][local];
                for( ch = 2; ch < nch; ch++ ) {
                    float s = b->channel[ch][local] * 0.5f;
                    L += s; R += s;
                }
            }
            L = clamp1( L * p->volume );
            R = clamp1( R * p->volume );
            p->playhead++;
            if( ++local >= b->numFrames ) {   /* advance to next block */
                bIdx++; local = 0;
                have = ( bIdx < c->numBlocks );
            }
            if( p->playhead >= p->playEnd ) {
                if( p->loop ) {
                    p->playhead = p->playStart;
                    seq_locate( c, p->playhead, &bIdx, &local );
                    have = ( bIdx < c->numBlocks );
                } else {
                    p->playing = 0;
                }
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
    int samples, target;
    if( rate <= 0 ) rate = 44100;
    if( audioDev && audioDevRate == rate ) return;
    if( audioDev ) { SDL_CloseAudioDevice( audioDev ); audioDev = 0; }

    /* The callback advances the playhead once per invocation, and the display
       reads it once per video frame -- so the on-screen playhead only moves as
       often as the callback fires (every want.samples/rate seconds).  A fixed
       1024-sample buffer is ~23 ms at 44.1 kHz (smooth) but ~128 ms at 8 kHz
       (visibly jerky).  Pick a buffer that keeps the callback period ~constant
       (~20 ms) across sample rates: the power of two nearest rate/50, clamped
       to [128, 2048].  The callback is a trivial copy, so a small buffer is
       safe. */
    target  = rate / 50;
    samples = 128;
    while( ( samples << 1 ) <= 2048 && ( samples << 1 ) <= target )
        samples <<= 1;
    if( ( samples << 1 ) <= 2048 &&
        ( target - samples ) > ( ( samples << 1 ) - target ) )
        samples <<= 1;

    SDL_memset( &want, 0, sizeof(want) );
    want.freq     = rate;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = (Uint16) samples;
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
/* Besides the sample delta, a record also snapshots the *editor* state
 * (selection + marks) as it was before and after the edit, so undo/redo
 * restore not just the audio but also what was selected and which marks
 * existed.  This makes undoing a cut restore the deleted selection, and
 * undoing a paste remove the marks the paste created -- rather than leaving
 * stale carets and an empty cursor behind.  The mark snapshots are exact
 * copies (malloc'd), so no fragile per-edit mark bookkeeping is needed to
 * reverse them. */
typedef struct {
    long   at;
    long   oldLen;
    long   newLen;
    int    numChannels;
    float *oldData[AUDIO_MAX_CHANNELS];  /* oldLen frames/ch, NULL if 0 */
    float *newData[AUDIO_MAX_CHANNELS];  /* newLen frames/ch, NULL if 0 */

    long   selBeforeS, selBeforeE;       /* selection before / after the edit */
    long   selAfterS,  selAfterE;
    Mark  *marksBefore;                  /* malloc'd copies of the mark table  */
    Mark  *marksAfter;
    int    numMarksBefore, numMarksAfter;
    int    colorNextBefore, colorNextAfter;
} UndoRec;

/* Sample formats a document can be saved as (index also drives the New /
 * Save-As format combos).  Internally all audio is float32; this is purely a
 * "how to encode on save to WAV" preference, defaulted from the loaded file. */
enum { FMT_PCM16 = 0, FMT_PCM24 = 1, FMT_PCM8 = 2, FMT_F32 = 3 };

/* A Doc is one open file / tab.  Everything here is per-document: its audio,
 * marks, selection, view, undo chain, path and save format.  The clipboard is
 * NOT here -- it is shared (see 'ui' below) so audio can be pasted between
 * tabs.  Switching tabs is just changing g_curDoc; nothing is rebuilt. */
typedef struct {
    Sequence  clip;          /* block-list document (owns per-block summaries) */

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

    /* undo / redo (delta records, see UndoRec) */
    UndoRec undo[UNDO_LEVELS];
    UndoRec redo[UNDO_LEVELS];
    int     undoCount;
    int     redoCount;

    char path[PATH_MAX_LEN];
    int  dirty;
    int  fmt;                /* FMT_* save format for this document      */
    int  adoptGeom;          /* empty throwaway tab: paste adopts src     */
                             /* channels/rate; cleared once a real file   */
                             /* loads or the user picks New-dialog geom   */
} Doc;

/* The open documents (tabs).  There is always at least one; the current one
 * is g_docs[g_curDoc].  All the per-document code refers to it through the
 * 'app' macro, so the vast body of editing/rendering logic is unchanged from
 * the single-document version -- it now just operates on whichever tab is
 * current. */
static Doc g_docs[MAX_TABS];
static int g_numDocs = 1;
static int g_curDoc  = 0;

#define app ( g_docs[g_curDoc] )

/* When >= 0, force this tab selected in the tab bar next frame (used to focus
 * a freshly created / opened / newly-current document). */
static int g_forceSelectDoc = -1;

/* Shared, application-global (NOT per-document) UI state. */
static struct {
    AudioClip clipboard;     /* copy/cut buffer -- shared across tabs        */

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
    float browserW, browserH;     /* remembered dialog size this session */

    /* New-file dialog scratch */
    int  newChans;                /* 1..AUDIO_MAX_CHANNELS               */
    int  newRateIdx;              /* index into rate presets             */
    int  newFmt;                  /* FMT_*                               */
} ui;

/* common sample-rate presets offered by the New dialog */
static const int   g_ratePresets[]  = { 8000, 11025, 16000, 22050,
                                        32000, 44100, 48000, 96000 };
static const char *g_rateLabels[]   = { "8000", "11025", "16000", "22050",
                                        "32000", "44100", "48000", "96000" };
#define NUM_RATE_PRESETS ( (int)( sizeof(g_ratePresets) / sizeof(g_ratePresets[0]) ) )

/* labels for the format combos, indexed by FMT_* */
static const char *g_fmtLabels[] = { "16-bit PCM", "24-bit PCM",
                                     "8-bit PCM", "32-bit float" };
#define NUM_FMTS 4

/* -------------------------------------------------------------------- */
/* asynchronous file loading (worker thread + progress bar)             */
/* -------------------------------------------------------------------- */

typedef struct {
    char         path[PATH_MAX_LEN];
    AudioClip    clip;       /* decode target (moved into 'seq' on phase 1) */
    Sequence     seq;        /* block-list built from the decoded clip       */
    volatile int progress;   /* 0..1000 within the current phase          */
    volatile int phase;      /* 0 = decoding file, 1 = building overview   */
    volatile int done;       /* set by the worker when finished            */
    int          error;
    char         err[256];
    int          srcBits;    /* source file's bit depth (for save default) */
    int          srcFloat;   /* source was IEEE float                       */
} LoadJob;

static LoadJob   loadJob;
static pthread_t loadThread;
static int       loading = 0;   /* a load is in flight (main-thread flag)  */

static void *loadWorker( void *arg )
{
    LoadJob *j = (LoadJob *)arg;
    memset( &j->clip, 0, sizeof(j->clip) );
    seq_init( &j->seq );
    j->phase = 0;
    j->progress = 0;
    j->srcBits = 16;
    j->srcFloat = 0;
    if( audio_load_progress( &j->clip, j->path, j->err, sizeof(j->err),
                             &j->progress, &j->srcBits, &j->srcFloat ) ) {
        j->error = 1;
        j->done  = 1;
        return NULL;
    }
    j->phase    = 1;
    j->progress = 0;
    /* split the decoded clip into blocks + per-block summaries (consumes clip) */
    seq_adopt_clip( &j->seq, &j->clip );
    j->progress = 1000;
    j->error = 0;
    j->done  = 1;
    return NULL;
}

/* -------------------------------------------------------------------- */
/* helpers                                                              */
/* -------------------------------------------------------------------- */

static long clipLen( void ) { return app.clip.numFrames; }

static int hasSelection( void ) { return app.selEnd > app.selStart; }

/* NOTE: the overview no longer uses a separate global pyramid that must be
 * rebuilt after edits -- each block in app.clip carries its own min/max
 * summary bins, kept current by the Sequence ops themselves, so structural
 * edits need no pyramid bookkeeping at all (that is what makes an edit at the
 * start of a long file as fast as one at the end). */

/* the range effects act on: the selection, or the whole clip */
static void effectRange( long *s, long *e )
{
    if( hasSelection() ) { *s = app.selStart; *e = app.selEnd; }
    else                 { *s = 0;            *e = clipLen(); }
}

static void setStatus( const char *msg )
{
    strncpy( ui.statusMsg, msg, sizeof(ui.statusMsg) - 1 );
    ui.statusMsg[sizeof(ui.statusMsg) - 1] = '\0';
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
    if( r->marksBefore ) { free( r->marksBefore ); r->marksBefore = NULL; }
    if( r->marksAfter )  { free( r->marksAfter );  r->marksAfter  = NULL; }
    r->oldLen = r->newLen = 0;
}

/* malloc a copy of the current mark table (NULL if empty); caller frees */
static Mark *dupMarks( int *countOut )
{
    Mark *m = NULL;
    *countOut = app.numMarks;
    if( app.numMarks > 0 ) {
        m = (Mark *)malloc( (size_t)app.numMarks * sizeof(Mark) );
        if( m ) memcpy( m, app.marks, (size_t)app.numMarks * sizeof(Mark) );
        else    *countOut = 0;
    }
    return m;
}

/* replace the live mark table with a snapshot (does not take ownership) */
static void restoreMarks( const Mark *m, int count, int colorNext )
{
    if( count > MAX_MARKS ) count = MAX_MARKS;
    if( count > 0 && m ) memcpy( app.marks, m, (size_t)count * sizeof(Mark) );
    app.numMarks      = count;
    app.markColorNext = colorNext;
}

static void clearUndoRedoDoc( Doc *d )
{
    int i;
    for( i = 0; i < d->undoCount; i++ ) undoRecFree( &d->undo[i] );
    for( i = 0; i < d->redoCount; i++ ) undoRecFree( &d->redo[i] );
    d->undoCount = d->redoCount = 0;
}

static void clearUndoRedo( void ) { clearUndoRedoDoc( &app ); }

/* Copy len frames of the current clip starting at 'start' into freshly
 * malloc'd per-channel arrays (dst[ch]).  dst pointers are NULL when len==0. */
static void captureRange( float *dst[AUDIO_MAX_CHANNELS], long start, long len )
{
    int ch, nch = app.clip.numChannels;
    AudioClip tmp;
    for( ch = 0; ch < AUDIO_MAX_CHANNELS; ch++ ) dst[ch] = NULL;
    if( len <= 0 ) return;
    /* flatten the block-list range into a contiguous clip, then take over its
     * per-channel arrays (avoiding a second copy) */
    memset( &tmp, 0, sizeof(tmp) );
    if( seq_read_range( &tmp, &app.clip, start, start + len ) ) return;
    for( ch = 0; ch < nch; ch++ ) {
        dst[ch] = tmp.channel[ch];       /* steal the pointer */
        tmp.channel[ch] = NULL;
    }
    audio_free( &tmp );                  /* frees only the remaining channels */
}

/* Record in progress between beginEdit() and commitEdit(). */
static UndoRec pendingUndo;

/* Snapshot the frames [at, at+oldLen) that are about to be replaced, plus the
 * editor state (selection + marks) as it stands right now -- BEFORE the edit
 * action touches the selection or drops any seam marks.  Call at the very start
 * of an edit action; pair with commitEdit() at the very end (after selection /
 * marks have been updated to their final post-edit values). */
static void beginEdit( long at, long oldLen )
{
    memset( &pendingUndo, 0, sizeof(pendingUndo) );
    pendingUndo.at          = at;
    pendingUndo.oldLen      = oldLen;
    pendingUndo.numChannels = app.clip.numChannels;
    captureRange( pendingUndo.oldData, at, oldLen );

    pendingUndo.selBeforeS     = app.selStart;
    pendingUndo.selBeforeE     = app.selEnd;
    pendingUndo.marksBefore    = dupMarks( &pendingUndo.numMarksBefore );
    pendingUndo.colorNextBefore = app.markColorNext;
}

/* Snapshot the newLen frames now at [at, newLen) and the final editor state,
 * then push the completed record onto the undo stack (clearing redo, dropping
 * the oldest at depth).  Call AFTER the clip, selection and marks are all in
 * their post-edit state. */
static void commitEdit( long newLen )
{
    int i;
    pendingUndo.newLen = newLen;
    captureRange( pendingUndo.newData, pendingUndo.at, newLen );

    pendingUndo.selAfterS      = app.selStart;
    pendingUndo.selAfterE      = app.selEnd;
    pendingUndo.marksAfter     = dupMarks( &pendingUndo.numMarksAfter );
    pendingUndo.colorNextAfter = app.markColorNext;

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
    seq_splice( &app.clip, r.at, r.newLen, r.oldData, r.oldLen );
    audio_unlock();

    /* move the record onto the redo stack (it replays the edit unchanged) */
    if( app.redoCount == UNDO_LEVELS ) {
        undoRecFree( &app.redo[0] );
        for( i = 1; i < UNDO_LEVELS; i++ ) app.redo[i - 1] = app.redo[i];
        app.redoCount--;
    }
    app.redo[app.redoCount++] = r;
    (void)lenChanged;   /* the Sequence keeps its block summaries current */

    /* restore the exact selection + marks that existed before the edit, so
     * a cut's selection comes back and a paste's seam marks disappear */
    app.selStart = r.selBeforeS;
    app.selEnd   = r.selBeforeE;
    restoreMarks( r.marksBefore, r.numMarksBefore, r.colorNextBefore );
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
    seq_splice( &app.clip, r.at, r.oldLen, r.newData, r.newLen );
    audio_unlock();

    if( app.undoCount == UNDO_LEVELS ) {
        undoRecFree( &app.undo[0] );
        for( i = 1; i < UNDO_LEVELS; i++ ) app.undo[i - 1] = app.undo[i];
        app.undoCount--;
    }
    app.undo[app.undoCount++] = r;
    (void)lenChanged;

    /* restore the selection + marks as they were just after the edit */
    app.selStart = r.selAfterS;
    app.selEnd   = r.selAfterE;
    restoreMarks( r.marksAfter, r.numMarksAfter, r.colorNextAfter );
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
    seq_delete_range( &app.clip, s, e );
    audio_unlock();
    marksAdjustDelete( s, e );
    addMark( s, newMarkColor() );     /* mark the seam left behind */
    app.selEnd = app.selStart = s;
    commitEdit( 0 );                  /* record final selection + marks */
    clampView();
    refreshPlayback();
    setStatus( "Deleted selection" );
}

static void actCopy( void )
{
    long s, e;
    if( !hasSelection() ) { setStatus( "No selection to copy" ); return; }
    s = app.selStart; e = app.selEnd;
    seq_read_range( &ui.clipboard, &app.clip, s, e );
    /* mark the copied region's edges so you can see / return to what was
     * grabbed (copy is not an undoable edit, so these are plain annotations) */
    {
        int c = newMarkColor();
        addMark( s, c );
        addMark( e, c );
    }
    setStatus( "Copied selection" );
}

static void actCut( void )
{
    long s, e;
    if( !hasSelection() ) { setStatus( "No selection to cut" ); return; }
    s = app.selStart; e = app.selEnd;
    seq_read_range( &ui.clipboard, &app.clip, s, e );
    beginEdit( s, e - s );
    audio_lock();
    seq_delete_range( &app.clip, s, e );
    audio_unlock();
    marksAdjustDelete( s, e );
    addMark( s, newMarkColor() );     /* mark the seam left behind */
    app.selEnd = app.selStart = s;
    commitEdit( 0 );                  /* record final selection + marks */
    clampView();
    refreshPlayback();
    setStatus( "Cut selection" );
}

static void actPaste( void )
{
    long at, insN, selS, selE;
    int  hadSel, c;
    if( ui.clipboard.numFrames == 0 ) { setStatus( "Clipboard is empty" ); return; }
    /* A throwaway blank tab (startup/after-close, no user-chosen geometry and
     * nothing loaded yet) takes on the clipboard's channel count and rate, so
     * copying a stereo clip into a fresh tab "just works".  A New-dialog doc or
     * a loaded file has adoptGeom cleared, so its geometry is preserved and the
     * clipboard is channel-mapped into it instead. */
    if( clipLen() == 0 && app.adoptGeom &&
        ( app.clip.numChannels != ui.clipboard.numChannels ||
          app.clip.sampleRate  != ui.clipboard.sampleRate ) ) {
        audio_lock();
        seq_set_empty( &app.clip, ui.clipboard.numChannels,
                       ui.clipboard.sampleRate );
        audio_unlock();
        open_audio( app.clip.sampleRate );
    }
    app.adoptGeom = 0;     /* has real content now */
    insN   = ui.clipboard.numFrames;
    hadSel = hasSelection();
    selS   = app.selStart;
    selE   = app.selEnd;
    at     = selS;
    /* one splice: the (optional) replaced selection out, the clipboard in */
    beginEdit( at, hadSel ? ( selE - selS ) : 0 );
    audio_lock();
    if( hadSel ) seq_delete_range( &app.clip, selS, selE );
    seq_insert_clip( &app.clip, at, &ui.clipboard );
    audio_unlock();
    if( hadSel ) marksAdjustDelete( selS, selE );
    marksAdjustInsert( at, insN );
    c = newMarkColor();
    addMark( at,        c );          /* paste start seam */
    addMark( at + insN, c );          /* paste end seam   */
    /* leave the pasted audio UN-selected and drop the cursor at its end so a
     * run of pastes lays clips down end-to-end like train cars; scroll to keep
     * that advancing cursor on screen (following the end of a growing file) */
    app.selStart = app.selEnd = at + insN;
    commitEdit( insN );              /* record final selection + marks */
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
    /* keep only [s,e): drop the tail first (so [0,e) indices stay put), then
     * the head.  Both are block relinks -- no O(n) copy of the kept region. */
    seq_delete_range( &app.clip, e, clipLen() );
    seq_delete_range( &app.clip, 0, s );
    audio_unlock();
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
    commitEdit( clipLen() );          /* record final selection + marks */
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
            float peak = (float)pow( 10.0, (double)ui.dlgNormDb / 20.0 );
            seq_normalize( &app.clip, s, e, peak );
            break;
        }
        case 1: seq_amplify( &app.clip, s, e, ui.dlgGain );          break;
        case 2: seq_fade_in( &app.clip, s, e );                       break;
        case 3: seq_fade_out( &app.clip, s, e );                      break;
        case 4: seq_silence( &app.clip, s, e );                       break;
        case 5: seq_reverse( &app.clip, s, e );                       break;
    }
    audio_unlock();
    /* these length-preserving effects update only the touched blocks' summary
     * bins internally -- no whole-file pyramid work needed */
    /* leave marks at the edges of the processed region so the seam is kept */
    if( onSelection ) {
        int c = newMarkColor();
        addMark( s, c ); addMark( e, c );
    }
    commitEdit( e - s );             /* record final selection + marks */
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

/* which doc index a load / new targets, so finishLoad installs into the right
 * tab even if the user switched tabs while it was decoding.  -1 = none. */
static int loadTargetDoc = -1;

/* Command-line files open one at a time (each load is async and single-flight):
 * they queue here and pumpPendingOpens() kicks off the next once the previous
 * finishes.  A shell wildcard expands to many argv entries, so `cwave *.wav`
 * lands them all as separate tabs. */
static char g_pendingOpen[MAX_TABS][PATH_MAX_LEN];
static int  g_pendingCount   = 0;
static int  g_pendingIdx     = 0;
static int  g_pendingFocused = 0;   /* focused tab 0 once the batch finished */

/* (re)initialize a document to an empty clip with the given geometry, freeing
 * any audio / undo / marks it held.  Safe on a freshly zeroed slot. */
static void docMakeEmpty( Doc *d, int chans, int rate, int fmt )
{
    clearUndoRedoDoc( d );
    seq_free( &d->clip );
    seq_init( &d->clip );
    seq_set_empty( &d->clip, chans, rate );
    d->numMarks        = 0;
    d->markColorNext   = 0;
    d->selStart = d->selEnd = 0;
    d->viewStart       = 0.0;
    d->samplesPerPixel = 1.0;
    d->path[0]         = '\0';
    d->dirty           = 0;
    d->fmt             = fmt;
    d->adoptGeom       = 1;   /* throwaway blank: paste may adopt src geom */
}

/* a "blank" doc is an untitled, unedited, empty tab -- New / Open reuse it
 * instead of piling on another empty tab. */
static int isDocBlank( const Doc *d )
{
    return d->clip.numFrames == 0 && !d->dirty &&
           d->path[0] == '\0' && d->undoCount == 0;
}

/* map a loaded file's sample format to our FMT_* save default */
static int fmtFromSource( int bits, int isFloat )
{
    if( isFloat )     return FMT_F32;
    if( bits == 8 )   return FMT_PCM8;
    if( bits == 24 )  return FMT_PCM24;
    return FMT_PCM16;             /* 16- and 32-bit int fold to 16-bit save */
}

/* Switch the current tab.  If transport was live, keep it live in the tab we
 * jump to -- playing from that tab's cursor/selection -- so a user who opened a
 * folder of samples can hit Play once and flip through them all in turn.  We
 * must stop the callback touching the OLD doc first: player.clip points at a
 * specific Doc's &clip, and this focus change (and any later closeDoc that
 * shifts the array) would otherwise leave it reading moved/freed blocks. */
static void switchTab( int idx )
{
    int wasPlaying;
    if( idx < 0 || idx >= g_numDocs || idx == g_curDoc ) return;
    wasPlaying = player.playing;
    stopPlayback();
    g_curDoc = idx;
    g_forceSelectDoc = idx;   /* keep the tab strip in sync (keyboard switch) */
    open_audio( app.clip.sampleRate );
    if( wasPlaying && clipLen() > 0 ) {
        /* mirror togglePlay: loop a selection (tracking its edges), else play
         * from the cursor to the end.  player.loop is untouched by stop/play,
         * so a checked Loop carries over to the new tab automatically. */
        if( hasSelection() ) {
            playFrom( app.selStart, app.selEnd );
            player.followSel = 1;
        } else {
            playFrom( app.selStart, clipLen() );
            player.followSel = 0;
        }
    }
}

/* close tab 'idx', shifting the rest down.  Never leaves zero tabs: closing
 * the last one replaces it with a fresh empty document. */
static void closeDoc( int idx )
{
    int i;
    if( idx < 0 || idx >= g_numDocs ) return;
    stopPlayback();                       /* player may point into this doc  */
    clearUndoRedoDoc( &g_docs[idx] );
    seq_free( &g_docs[idx].clip );
    for( i = idx; i < g_numDocs - 1; i++ )
        g_docs[i] = g_docs[i + 1];        /* struct copy moves heap pointers */
    g_numDocs--;
    memset( &g_docs[g_numDocs], 0, sizeof(Doc) );
    seq_init( &g_docs[g_numDocs].clip );

    if( g_numDocs == 0 ) {                 /* keep at least one document      */
        g_numDocs = 1;
        docMakeEmpty( &g_docs[0], 1, 44100, FMT_PCM16 );
        g_curDoc = 0;
    } else if( g_curDoc > idx ) {
        g_curDoc--;                        /* current shifted left            */
    } else if( g_curDoc >= g_numDocs ) {
        g_curDoc = g_numDocs - 1;          /* closed the last, current tab    */
    }
    g_forceSelectDoc = g_curDoc;
    open_audio( app.clip.sampleRate );
    setStatus( "Closed tab" );
}

/* create a new empty document (its own tab) with the chosen geometry; reuse a
 * blank current tab rather than stacking another empty one. */
static void createNewDoc( int chans, int rate, int fmt )
{
    int target;
    if( chans < 1 ) chans = 1;
    if( chans > AUDIO_MAX_CHANNELS ) chans = AUDIO_MAX_CHANNELS;
    if( rate  < 1 ) rate = 44100;
    stopPlayback();
    if( isDocBlank( &app ) ) {
        target = g_curDoc;
    } else if( g_numDocs < MAX_TABS ) {
        target = g_numDocs++;
    } else {
        setStatus( "Too many tabs open" );
        strncpy( ui.pendingPopup, "Error", sizeof(ui.pendingPopup) - 1 );
        return;
    }
    docMakeEmpty( &g_docs[target], chans, rate, fmt );
    g_docs[target].adoptGeom = 0;  /* user chose this geometry -- keep it */
    g_curDoc = target;
    g_forceSelectDoc = target;
    open_audio( rate );
    zoomFit();
    setStatus( "New file" );
}

/* open the New-file dialog (populated with the last-used geometry) */
static void newFile( void )
{
    strncpy( ui.pendingPopup, "New", sizeof(ui.pendingPopup) - 1 );
}

/* Kick off a background load into document slot 'target'.  The main loop shows
 * a progress bar and calls finishLoad() when the worker signals done. */
static void startLoad( const char *path, int target )
{
    if( loading ) { setStatus( "Busy loading another file" ); return; }
    stopPlayback();
    memset( &loadJob, 0, sizeof(loadJob) );
    strncpy( loadJob.path, path, PATH_MAX_LEN - 1 );
    loadJob.path[PATH_MAX_LEN - 1] = '\0';
    if( pthread_create( &loadThread, NULL, loadWorker, &loadJob ) != 0 ) {
        setStatus( "Could not start loader thread" );
        return;
    }
    loadTargetDoc = target;
    loading = 1;
    setStatus( "Loading..." );
}

/* Open a file in a tab: reuse a blank current tab, else spawn a new one.  The
 * tab is created (and focused, showing the filename) immediately; its audio
 * fills in when the background decode finishes in finishLoad(). */
static void openFile( const char *path )
{
    int target;
    if( loading ) { setStatus( "Busy loading another file" ); return; }
    if( isDocBlank( &app ) ) {
        target = g_curDoc;
    } else if( g_numDocs < MAX_TABS ) {
        target = g_numDocs++;
        docMakeEmpty( &g_docs[target], 1, 44100, FMT_PCM16 );
    } else {
        setStatus( "Too many tabs open" );
        strncpy( ui.pendingPopup, "Error", sizeof(ui.pendingPopup) - 1 );
        return;
    }
    /* show the filename on the (possibly new) tab while it decodes */
    strncpy( g_docs[target].path, path, PATH_MAX_LEN - 1 );
    g_docs[target].path[PATH_MAX_LEN - 1] = '\0';
    g_curDoc = target;
    g_forceSelectDoc = target;
    startLoad( path, target );
}

/* If command-line files remain queued and no load is in flight, start the next
 * one.  Called each main-loop iteration (and after each load finishes), so a
 * multi-file `cwave a.wav b.wav c.ogg` opens them into successive tabs. */
static void pumpPendingOpens( void )
{
    if( loading ) return;
    if( g_pendingIdx < g_pendingCount ) {
        openFile( g_pendingOpen[g_pendingIdx++] );
        return;
    }
    /* Whole batch loaded: land focus on the first file so the user starts at
     * the top of the list they opened (each openFile focused its own tab, so
     * without this we'd sit on the last one). */
    if( g_pendingCount > 1 && !g_pendingFocused ) {
        g_pendingFocused = 1;
        switchTab( 0 );
    }
}

/* Called on the main thread once the worker has finished. */
static void finishLoad( void )
{
    int target = loadTargetDoc;
    pthread_join( loadThread, NULL );
    loading = 0;
    loadTargetDoc = -1;

    if( target < 0 || target >= g_numDocs ) target = g_curDoc;

    if( loadJob.error ) {
        audio_free( &loadJob.clip );
        seq_free( &loadJob.seq );
        setStatus( loadJob.err );
        strncpy( ui.pendingPopup, "Error", sizeof(ui.pendingPopup) - 1 );
        /* the tab we opened for this load never got audio: drop it if it was
         * freshly created, or reset it if it was a reused blank tab */
        if( g_numDocs > 1 ) closeDoc( target );
        else                docMakeEmpty( &g_docs[target], 1, 44100, FMT_PCM16 );
        return;
    }

    /* install the freshly decoded block-list document into its tab (its
     * per-block summaries are already built -- overview ready, no rebuild) */
    g_curDoc = target;
    audio_lock();
    seq_free( &app.clip );
    app.clip = loadJob.seq;
    audio_unlock();
    seq_init( &loadJob.seq );

    app.fmt = fmtFromSource( loadJob.srcBits, loadJob.srcFloat );
    open_audio( app.clip.sampleRate );

    app.numMarks = 0;
    app.markColorNext = 0;
    app.selStart = app.selEnd = 0;
    snprintf( app.path, PATH_MAX_LEN, "%s", loadJob.path );
    app.dirty = 0;
    app.adoptGeom = 0;          /* real file geometry -- do not adopt on paste */
    clearUndoRedo();
    g_forceSelectDoc = target;
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
        strncpy( ui.pendingPopup, "Error", sizeof(ui.pendingPopup) - 1 );
        return;
    }
    {
        int bits = 16, isFloat = 0;
        switch( app.fmt ) {
            case FMT_PCM8:  bits = 8;  isFloat = 0; break;
            case FMT_PCM24: bits = 24; isFloat = 0; break;
            case FMT_F32:   bits = 32; isFloat = 1; break;
            default:        bits = 16; isFloat = 0; break;
        }
        rc = seq_save_wav_fmt( &app.clip, path, bits, isFloat, err, sizeof(err) );
    }
    if( rc ) {
        setStatus( err );
        strncpy( ui.pendingPopup, "Error", sizeof(ui.pendingPopup) - 1 );
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
    int   px, i;
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
        glColor4f( 0.40f, 0.55f, 0.72f, 1.0f );
        glBegin( GL_LINES );
        for( px = 0; px < (int)w; px++ ) {
            double f0 = (double)px * spp;
            double f1 = f0 + spp;
            float  mn = 0.0f, mx = 0.0f, cmn, cmx;
            int    ch, any = 0;
            for( ch = 0; ch < app.clip.numChannels; ch++ ) {
                if( seq_col_minmax( &app.clip, ch, f0, f1, spp,
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
    float tabsTop = menuH + TABBAR_H;      /* overview sits below the tab strip */
    float top    = tabsTop + OVERVIEW_H;
    float bottom = (float)winH - TRANSPORT_H;
    int   nch    = app.clip.numChannels;
    int   ch;
    float laneH, laneTop, laneAreaH;
    long  n = clipLen();
    AudioClip win;            /* flattened visible window (zoomed-in path)   */
    int   haveWin = 0;
    long  wi0 = 0, wi1 = 0;

    app.ovX = 0.0f;
    app.ovY = tabsTop;
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

    /* zoomed in enough to draw individual samples: flatten just the visible
     * window (all channels) out of the block list once, then index it */
    if( n > 0 && app.samplesPerPixel < 1.0 ) {
        wi0 = (long)app.viewStart - 1;
        wi1 = (long)( app.viewStart + app.samplesPerPixel * app.wfW ) + 2;
        if( wi0 < 0 ) wi0 = 0;
        if( wi1 > n ) wi1 = n;
        memset( &win, 0, sizeof(win) );
        if( wi1 > wi0 && seq_read_range( &win, &app.clip, wi0, wi1 ) == 0 )
            haveWin = 1;
    }

    for( ch = 0; ch < nch; ch++ ) {
        float y0 = laneTop + laneH * (float)ch;
        float yc = y0 + laneH * 0.5f;
        float amp = laneH * 0.45f;
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

        if( n <= 0 ) continue;

        if( app.samplesPerPixel >= 1.0 ) {
            /* peak (min/max) rendering: one vertical line per column,
             * summarized through each block's own bins when zoomed out */
            glColor4f( 0.45f, 0.72f, 0.95f, 1.0f );
            glBegin( GL_LINES );
            for( px = 0; px < (int)app.wfW; px++ ) {
                double f0 = app.viewStart + (double)px * app.samplesPerPixel;
                double f1 = f0 + app.samplesPerPixel;
                float  mn, mx;
                if( f0 >= (double)n ) break;
                if( seq_col_minmax( &app.clip, ch, f0, f1,
                                    app.samplesPerPixel, &mn, &mx ) ) {
                    glVertex2f( (float)px, yc - mx * amp );
                    glVertex2f( (float)px, yc - mn * amp );
                }
            }
            glEnd();
        } else if( haveWin ) {
            /* zoomed in: connect individual samples from the flattened window */
            float *samples = win.channel[ch];
            long i;
            glColor4f( 0.45f, 0.72f, 0.95f, 1.0f );
            glBegin( GL_LINE_STRIP );
            for( i = wi0; i < wi1; i++ ) {
                float x = frameToPixel( i );
                glVertex2f( x, yc - samples[i - wi0] * amp );
            }
            glEnd();
            /* sample dots when very zoomed */
            if( app.samplesPerPixel < 0.25 ) {
                glColor4f( 0.85f, 0.9f, 1.0f, 1.0f );
                glPointSize( 3.0f );
                glBegin( GL_POINTS );
                for( i = wi0; i < wi1; i++ ) {
                    float x = frameToPixel( i );
                    glVertex2f( x, yc - samples[i - wi0] * amp );
                }
                glEnd();
            }
        }
    }
    if( haveWin ) audio_free( &win );

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

/* true if 'name' ends in a supported audio extension (.wav / .ogg, any case) */
static int isAudioName( const char *name )
{
    size_t n = strlen( name );
    char e[5];
    int  i;
    if( n < 4 || name[n - 4] != '.' ) return 0;
    for( i = 0; i < 4; i++ ) {
        char ch = name[n - 4 + i];
        if( ch >= 'A' && ch <= 'Z' ) ch = (char)( ch - 'A' + 'a' );
        e[i] = ch;
    }
    e[4] = '\0';
    return strcmp( e, ".wav" ) == 0 || strcmp( e, ".ogg" ) == 0;
}

static void browserOpen( int save )
{
    ui.browserSave = save;
    if( ui.browserDir[0] == '\0' ) {
        if( getcwd( ui.browserDir, PATH_MAX_LEN ) == NULL )
            strcpy( ui.browserDir, "." );
    }
    ui.browserFile[0] = '\0';
    strncpy( ui.pendingPopup, save ? "Save File" : "Open File",
             sizeof(ui.pendingPopup) - 1 );
}

/* navigate the browser into 'target', resolving it to a clean absolute path
 * (so ".." collapses instead of piling up) when possible. */
static void browserChdir( const char *target )
{
    char resolved[PATH_MAX_LEN];
    if( realpath( target, resolved ) != NULL )
        snprintf( ui.browserDir, PATH_MAX_LEN, "%s", resolved );
    else
        snprintf( ui.browserDir, PATH_MAX_LEN, "%s", target );
}

static void drawFileBrowser( const char *popupId )
{
    DIR *d;
    struct dirent *ent;
    char *names[4096];
    int   count = 0;
    int   i;
    float listH;

    /* remember the dialog size for the rest of the session */
    gui_get_window_size( &ui.browserW, &ui.browserH );

    /* current path (editable, and updated as you navigate directories) */
    gui_input_text( "Dir", ui.browserDir, PATH_MAX_LEN );
    gui_separator();

    /* let the file list grow with the (resizable) dialog: take all the
     * vertical room except a fixed reserve for the Name field + buttons */
    listH = gui_content_avail_h() - 72.0f;
    if( listH < 90.0f ) listH = 90.0f;

    if( gui_begin_child( "files", 0.0f, listH, 1 ) ) {
        d = opendir( ui.browserDir );
        if( d ) {
            while( ( ent = readdir( d ) ) != NULL && count < 4096 ) {
                /* hide "." and every dotfile / dot-directory, but keep ".."
                 * so the user can still walk back up the tree */
                if( ent->d_name[0] == '.' &&
                    strcmp( ent->d_name, ".." ) != 0 ) continue;
                names[count] = (char *)malloc( strlen( ent->d_name ) + 2 );
                strcpy( names[count], ent->d_name );
                count++;
            }
            closedir( d );
            qsort( names, count, sizeof(char *), nameCmp );
            for( i = 0; i < count; i++ ) {
                char label[PATH_MAX_LEN + 8];
                char full[PATH_MAX_LEN];
                DIR *sub;
                int isDir;
                snprintf( full, sizeof(full), "%s/%s",
                          ui.browserDir, names[i] );
                sub = opendir( full );
                isDir = ( sub != NULL );
                if( sub ) closedir( sub );
                /* only show directories and supported audio files */
                if( !isDir && !isAudioName( names[i] ) ) continue;
                snprintf( label, sizeof(label), "%s%s",
                          isDir ? "[DIR] " : "      ", names[i] );
                if( gui_selectable( label, 0 ) ) {
                    if( isDir ) browserChdir( full );
                    else snprintf( ui.browserFile, PATH_MAX_LEN, "%s",
                                   names[i] );
                }
            }
            for( i = 0; i < count; i++ ) free( names[i] );
        } else {
            gui_text( "(cannot open directory)" );
        }
    }
    gui_end_child();

    gui_separator();
    gui_input_text( "Name", ui.browserFile, PATH_MAX_LEN );

    /* when saving, let the user pick the WAV sample format; it is bound to the
     * current document so the choice persists and drives seq_save_wav_fmt */
    if( ui.browserSave )
        gui_combo( "Format", &app.fmt, g_fmtLabels, NUM_FMTS );

    if( gui_button( ui.browserSave ? "Save" : "Open" ) ) {
        char full[PATH_MAX_LEN];
        if( ui.browserFile[0] ) {
            snprintf( full, sizeof(full), "%s/%s",
                      ui.browserDir, ui.browserFile );
            gui_close_current_popup();
            if( ui.browserSave ) saveFile( full );
            else                  openFile( full );
        }
    }
    gui_same_line();
    if( gui_button( "Cancel" ) ) gui_close_current_popup();
    (void)popupId;
}

/* -------------------------------------------------------------------- */
/* GUI: tab bar (open documents)                                        */
/* -------------------------------------------------------------------- */

/* the short display name for a document's tab: the file's base name, or
 * "untitled" for a never-saved document */
static const char *docTitle( const Doc *d )
{
    const char *slash = strrchr( d->path, '/' );
    const char *base  = slash ? slash + 1 : d->path;
    return base[0] ? base : "untitled";
}

/* the strip of open-file tabs directly below the menu bar.  Selecting a tab
 * switches the current document; the little close button on a tab closes it.
 * Layout is one row in a fixed panel of height TABBAR_H (renderWaveform
 * reserves the same room for it). */
static void buildTabBar( int winW, float menuH )
{
    int i;
    int newCur   = g_curDoc;
    int closeReq = -1;

    gui_set_next_window_pos( 0.0f, menuH );
    gui_set_next_window_size( (float)winW, TABBAR_H );
    if( gui_begin( "##tabs", 1 ) ) {
        if( gui_begin_tab_bar( "##doctabs" ) ) {
            for( i = 0; i < g_numDocs; i++ ) {
                char label[PATH_MAX_LEN + 32];
                int  open   = 1;
                int  forced = ( g_forceSelectDoc == i ) ? 1 : 0;
                /* "name *###tabN": text before ### is shown, the id after it
                 * is stable so a name/dirty change doesn't recreate the tab */
                snprintf( label, sizeof(label), "%s%s###tab%d",
                          docTitle( &g_docs[i] ),
                          g_docs[i].dirty ? " *" : "", i );
                if( gui_tab_item( label, &open, forced ) ) newCur = i;
                if( !open ) closeReq = i;
            }
            gui_end_tab_bar();
        }
    }
    gui_end();

    /* A forced SetSelected only takes visual effect next frame, so the
     * previously-selected tab still reports "active" THIS frame and would drag
     * g_curDoc back to it (an issue when we programmatically switch tabs, e.g.
     * focusing tab 0 after a command-line batch loads).  Let the force win over
     * that stale active-tab return, regardless of loop order. */
    if( g_forceSelectDoc >= 0 && g_forceSelectDoc < g_numDocs )
        newCur = g_forceSelectDoc;
    g_forceSelectDoc = -1;
    if( newCur != g_curDoc ) switchTab( newCur );
    if( closeReq >= 0 )      closeDoc( closeReq );
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
        if( gui_menu_item( "Close Tab", "Ctrl+W", 1 ) )
            closeDoc( g_curDoc );
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
                           ui.clipboard.numFrames > 0 ) ) actPaste();
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
            strncpy( ui.pendingPopup, "Normalize", sizeof(ui.pendingPopup)-1 );
        if( gui_menu_item( "Amplify...", NULL, have ) )
            strncpy( ui.pendingPopup, "Amplify", sizeof(ui.pendingPopup)-1 );
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
            strncpy( ui.pendingPopup, "About", sizeof(ui.pendingPopup)-1 );
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
                  ui.statusMsg );
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
    if( ui.pendingPopup[0] ) {
        gui_open_popup( ui.pendingPopup );
        ui.pendingPopup[0] = '\0';
    }

    if( gui_begin_popup_modal( "Normalize" ) ) {
        char lbl[64];
        float lin;
        gui_text( "Peak normalize to target level (dBFS):" );
        gui_slider_float( "dB", &ui.dlgNormDb, -60.0f, 0.0f, "%.1f dB" );
        gui_input_float( "Exact dB", &ui.dlgNormDb );
        if( ui.dlgNormDb >  0.0f ) ui.dlgNormDb =  0.0f;
        if( ui.dlgNormDb < -60.0f ) ui.dlgNormDb = -60.0f;
        lin = (float)pow( 10.0, (double)ui.dlgNormDb / 20.0 );
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
        gui_slider_float( "Gain", &ui.dlgGain, 0.0f, 8.0f, "%.3f" );
        gui_input_float( "Exact", &ui.dlgGain );
        if( gui_button( "Apply" ) ) { applyEffect( 1 ); gui_close_current_popup(); }
        gui_same_line();
        if( gui_button( "Cancel" ) ) gui_close_current_popup();
        gui_end_popup();
    }

    if( gui_begin_popup_modal( "New" ) ) {
        gui_text( "Create a new empty file:" );
        gui_spacing();
        gui_input_int( "Channels", &ui.newChans );
        if( ui.newChans < 1 ) ui.newChans = 1;
        if( ui.newChans > AUDIO_MAX_CHANNELS ) ui.newChans = AUDIO_MAX_CHANNELS;
        if( ui.newRateIdx < 0 ) ui.newRateIdx = 0;
        if( ui.newRateIdx >= NUM_RATE_PRESETS ) ui.newRateIdx = NUM_RATE_PRESETS - 1;
        gui_combo( "Sample rate", &ui.newRateIdx, g_rateLabels, NUM_RATE_PRESETS );
        gui_combo( "Format", &ui.newFmt, g_fmtLabels, NUM_FMTS );
        gui_spacing();
        if( gui_button( "Create" ) ) {
            createNewDoc( ui.newChans, g_ratePresets[ui.newRateIdx], ui.newFmt );
            gui_close_current_popup();
        }
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
        gui_text_colored( 1.0f, 0.5f, 0.5f, ui.statusMsg );
        gui_spacing();
        if( gui_button( "OK" ) ) gui_close_current_popup();
        gui_end_popup();
    }

    /* the file browser is user-resizable; seed it with the last size used
     * this session (SetNextWindowSize with Appearing-cond only applies when
     * the popup opens, so manual resizing sticks) */
    gui_set_next_window_size_appearing( ui.browserW, ui.browserH );
    if( gui_begin_popup_modal_resizable( "Open File" ) ) {
        drawFileBrowser( "Open File" );
        gui_end_popup();
    }
    gui_set_next_window_size_appearing( ui.browserW, ui.browserH );
    if( gui_begin_popup_modal_resizable( "Save File" ) ) {
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
            case SDLK_w: closeDoc( g_curDoc ); break;
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
        /* tab navigation: left/right cycle with wrap-around, up/down jump to
         * the first/last tab.  Transport carries over (switchTab keeps it live)
         * so you can flip through a folder of samples while Play stays on. */
        case SDLK_LEFT:   switchTab( ( g_curDoc - 1 + g_numDocs ) % g_numDocs );
                          break;
        case SDLK_RIGHT:  switchTab( ( g_curDoc + 1 ) % g_numDocs ); break;
        case SDLK_UP:     switchTab( 0 ); break;
        case SDLK_DOWN:   switchTab( g_numDocs - 1 ); break;
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

    memset( g_docs, 0, sizeof(g_docs) );
    memset( &ui, 0, sizeof(ui) );
    memset( &player, 0, sizeof(player) );
    g_numDocs = 1;
    g_curDoc  = 0;
    docMakeEmpty( &g_docs[0], 1, 44100, FMT_PCM16 );

    player.volume = 1.0f;
    player.loop   = 0;
    ui.dlgGain    = 2.0f;
    ui.dlgNormDb  = -1.0f;
    ui.browserW   = 640.0f;
    ui.browserH   = 480.0f;
    ui.newChans   = 1;
    ui.newRateIdx = 5;            /* 44100 in g_ratePresets */
    ui.newFmt     = FMT_PCM16;

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

    /* Queue every command-line path (a shell wildcard expands to many argv
     * entries); pumpPendingOpens() feeds them to the single-flight async loader
     * one at a time, each into its own tab. */
    if( argc > 1 ) {
        int a;
        for( a = 1; a < argc && g_pendingCount < MAX_TABS; a++ ) {
            strncpy( g_pendingOpen[g_pendingCount], argv[a], PATH_MAX_LEN - 1 );
            g_pendingOpen[g_pendingCount][PATH_MAX_LEN - 1] = '\0';
            g_pendingCount++;
        }
    } else {
        setStatus( "Open a WAV or OGG file to begin" );
    }

    while( running ) {
        SDL_Event ev;
        int winW, winH;
        int overGui, keyCapture;

        while( SDL_PollEvent( &ev ) ) {
            gui_process_event( &ev );
            /* "over a GUI window" (menu bar, its dropdown, a dialog, or the
             * transport) rather than "wants capture": a click on empty wave
             * space that merely closes an open menu is NOT over any window, so
             * we still act on it this frame instead of swallowing it -- fixing
             * the old "first click just closes the menu" wart. */
            overGui    = gui_any_window_hovered();
            keyCapture = gui_want_capture_keyboard();

            switch( ev.type ) {
                case SDL_QUIT: running = 0; break;
                case SDL_KEYDOWN:
                    if( !keyCapture ) {
                        int ctrl = ( ev.key.keysym.mod & KMOD_CTRL ) ? 1 : 0;
                        handleKey( ev.key.keysym.sym, ctrl );
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if( !overGui && !loading &&
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
                    if( !overGui ) {
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

        /* feed the next command-line file to the loader when it's free */
        pumpPendingOpens();

        /* live loop: while playing a selection, track edits to its edges.
           Also, when Loop is on, obey a selection made *after* playback
           started -- checking Loop and then selecting redefines the loop
           region immediately, without a Stop/Play cycle.  (Playback may have
           begun with no selection, so followSel is 0; the loop gate covers
           that case.) */
        if( player.playing && hasSelection() &&
            ( player.followSel || player.loop ) ) {
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
        buildTabBar( winW, gui_main_menu_bar_height() );
        buildTransport( winW, winH );
        buildDialogs();
        if( loading ) buildLoadingOverlay( winW, winH );
        gui_render();

        SDL_GL_SwapWindow( window );
    }

    /* if a load is still running at quit, wait for it so we can free */
    if( loading ) { pthread_join( loadThread, NULL );
                    audio_free( &loadJob.clip );
                    seq_free( &loadJob.seq ); }

    stopPlayback();
    {
        int i;
        for( i = 0; i < g_numDocs; i++ ) {
            clearUndoRedoDoc( &g_docs[i] );
            seq_free( &g_docs[i].clip );
        }
    }
    audio_free( &ui.clipboard );
    if( audioDev ) SDL_CloseAudioDevice( audioDev );
    gui_shutdown();
    SDL_GL_DeleteContext( glctx );
    SDL_DestroyWindow( window );
    SDL_Quit();
    return 0;
}
