/* audio.h -- audio clip representation, file IO, and effects for cwave.
 *
 * Pure C89.  All sample data is stored as 32-bit float in the range
 * [-1, 1], planar (one array per channel).  This makes per-channel
 * effects and waveform drawing simple, and mixing to stereo cheap.
 */
#ifndef CWAVE_AUDIO_H
#define CWAVE_AUDIO_H

#define AUDIO_MAX_CHANNELS 8

typedef struct {
    int   numChannels;              /* 1..AUDIO_MAX_CHANNELS            */
    int   sampleRate;               /* e.g. 44100                       */
    long  numFrames;                /* samples per channel              */
    float *channel[AUDIO_MAX_CHANNELS]; /* each numFrames floats        */
} AudioClip;

/* ---- lifecycle -------------------------------------------------------- */

/* Allocate a clip with the given geometry.  Samples are zeroed.
 * Returns 0 on success, non-zero on allocation failure. */
int  audio_alloc( AudioClip *c, int numChannels, int sampleRate,
                  long numFrames );

/* Free channel data and reset the struct to empty. */
void audio_free( AudioClip *c );

/* Deep copy src into dst (dst is overwritten; any prior dst data freed).
 * Returns 0 on success. */
int  audio_copy( AudioClip *dst, const AudioClip *src );

/* Copy a frame range [start,end) of all channels into dst.
 * Returns 0 on success. */
int  audio_copy_range( AudioClip *dst, const AudioClip *src,
                       long start, long end );

/* ---- file IO ---------------------------------------------------------- */

/* Load a file, auto-detecting WAV or OGG by extension/content.
 * On success fills c and returns 0.  On failure returns non-zero and
 * writes a message into errBuf (errBufLen bytes). */
int  audio_load( AudioClip *c, const char *path,
                 char *errBuf, int errBufLen );

/* Same as audio_load, but if 'progress' is non-NULL it is updated with a
 * 0..1000 completion value as the file is read (for a progress bar in a
 * worker thread).  Safe to read 'progress' concurrently from another
 * thread while this runs.  If 'outBits'/'outFloat' are non-NULL they receive
 * the source file's sample format (bits-per-sample and 1 if IEEE float), so a
 * loaded document can default its save format to match what it came from. */
int  audio_load_progress( AudioClip *c, const char *path,
                          char *errBuf, int errBufLen,
                          volatile int *progress,
                          int *outBits, int *outFloat );

/* Save the clip to a WAV file (16-bit PCM).  Returns 0 on success. */
int  audio_save_wav( const AudioClip *c, const char *path,
                     char *errBuf, int errBufLen );

/* Save the clip to a 32-bit float WAV file.  Returns 0 on success. */
int  audio_save_wav_float( const AudioClip *c, const char *path,
                           char *errBuf, int errBufLen );

/* ---- effects (operate in place on frame range [start,end)) ------------ */

/* Peak-normalize the range so the loudest sample hits +/-peak. */
void audio_normalize( AudioClip *c, long start, long end, float peak );

/* Multiply the range by gain. */
void audio_amplify( AudioClip *c, long start, long end, float gain );

/* Linear fade from 0 -> 1 (in) or 1 -> 0 (out) across the range. */
void audio_fade_in( AudioClip *c, long start, long end );
void audio_fade_out( AudioClip *c, long start, long end );

/* Zero the range. */
void audio_silence( AudioClip *c, long start, long end );

/* Reverse the range. */
void audio_reverse( AudioClip *c, long start, long end );

/* Delete the range [start,end), shifting later frames left.  The clip
 * shrinks.  Returns 0 on success. */
int  audio_delete_range( AudioClip *c, long start, long end );

/* Keep only the range [start,end); discard everything outside (trim). */
int  audio_trim_to_range( AudioClip *c, long start, long end );

/* Insert clip 'ins' into c at frame 'at' (channels are matched/mixed as
 * needed).  Returns 0 on success. */
int  audio_insert( AudioClip *c, long at, const AudioClip *ins );

/* Replace frames [at, at+removeLen) with insLen frames taken from ins[ch]
 * (one array per channel; ins may be NULL only when insLen == 0).  This is
 * the single primitive behind delete (insLen 0), insert (removeLen 0),
 * in-place overwrite (removeLen == insLen -- no reallocation) and the
 * undo/redo of any of those.  Cost is O(tail + insLen): the frames after the
 * edit are shifted at most once, so it never scans the whole clip.  Channel
 * count and sample rate are preserved.  Returns 0 on success. */
int  audio_splice( AudioClip *c, long at, long removeLen,
                   float *const ins[], long insLen );

/* Compute the peak absolute sample over the range (for metering). */
float audio_peak( const AudioClip *c, long start, long end );

/* ====================================================================== */
/* Sequence -- a block-list ("piece list") document                       */
/*                                                                        */
/* The editable audio document is stored NOT as one giant contiguous      */
/* buffer per channel but as an ordered list of bounded blocks, each      */
/* owning its own contiguous samples AND its own min/max summary bins.    */
/* Structural edits (insert / delete / cut / paste) only split the two    */
/* boundary blocks (a bounded copy) and relink the small block-pointer    */
/* array, so their cost is O(edit size + numBlocks) -- independent of     */
/* WHERE in the file the edit happens (the whole point: a paste at the    */
/* start of a 28-minute file is as fast as one at the end).  Because each */
/* block keeps its own summary bins, the waveform overview needs no       */
/* O(numFrames) pyramid rebuild after an edit either -- unchanged blocks  */
/* keep their bins; only split/new blocks recompute (bounded).            */
/* ====================================================================== */

#define SEQ_BIN          256        /* frames summarized per level-0 bin   */
#ifndef SEQ_BLOCK_FRAMES            /* overridable so tests can force tiny  */
#define SEQ_BLOCK_FRAMES ( 1 << 18 )/* max frames per block (~1 MB / ch)   */
#endif

typedef struct {
    AudioClip buf;                          /* this block's samples        */
    long      numBins;                      /* ceil(buf.numFrames/SEQ_BIN) */
    float    *mn[AUDIO_MAX_CHANNELS];       /* per-bin min, numBins each    */
    float    *mx[AUDIO_MAX_CHANNELS];       /* per-bin max                  */
} Block;

typedef struct {
    int     numChannels;
    int     sampleRate;
    long    numFrames;                      /* total across all blocks      */
    Block **blocks;
    long   *start;                          /* start[i] = abs frame of blk i*/
    int     numBlocks, capBlocks;
} Sequence;

/* ---- lifecycle ---- */
void seq_init( Sequence *s );               /* zero an empty sequence       */
void seq_free( Sequence *s );
/* Adopt an already-loaded contiguous clip's samples into a fresh sequence
 * (splitting into blocks + computing bins).  'src' is consumed: its data is
 * moved into the sequence and src is left empty.  Returns 0 on success. */
int  seq_adopt_clip( Sequence *s, AudioClip *src );
/* Reset to an empty clip with the given geometry (numFrames==0). */
int  seq_set_empty( Sequence *s, int numChannels, int sampleRate );

/* ---- reads ---- */
float seq_sample( const Sequence *s, int ch, long frame );
/* Locate the block containing absolute 'frame' (0<=frame<numFrames): returns
 * the block index in *bIdx and the offset within that block in *local.  For
 * frame>=numFrames returns bIdx==numBlocks.  Used by the playback cursor. */
void  seq_locate( const Sequence *s, long frame, int *bIdx, long *local );
/* Flatten frames [start,end) of every channel into a contiguous clip. */
int   seq_read_range( AudioClip *dst, const Sequence *s, long start, long end );
/* Min/max of channel ch over the absolute frame span [f0,f1); uses block
 * summary bins when spp>=SEQ_BIN, raw samples otherwise.  Returns 1 if any
 * data was covered. */
int   seq_col_minmax( const Sequence *s, int ch, double f0, double f1,
                      double spp, float *mn, float *mx );

/* ---- structural edits ---- */
int  seq_delete_range( Sequence *s, long start, long end );
/* Insert the contiguous clip 'ins' at frame 'at'. */
int  seq_insert_clip( Sequence *s, long at, const AudioClip *ins );
/* Replace [at,at+removeLen) with insLen frames taken from ins[ch] (ins may be
 * NULL when insLen==0).  The single primitive behind undo/redo. */
int  seq_splice( Sequence *s, long at, long removeLen,
                 float *const ins[], long insLen );

/* ---- length-preserving effects on [start,end) ---- */
float seq_peak( const Sequence *s, long start, long end );
void  seq_normalize( Sequence *s, long start, long end, float peak );
void  seq_amplify( Sequence *s, long start, long end, float gain );
void  seq_fade_in( Sequence *s, long start, long end );
void  seq_fade_out( Sequence *s, long start, long end );
void  seq_silence( Sequence *s, long start, long end );
void  seq_reverse( Sequence *s, long start, long end );

/* Save the whole sequence to a 16-bit PCM WAV. */
int   seq_save_wav( const Sequence *s, const char *path,
                    char *errBuf, int errBufLen );

/* Save the whole sequence to a WAV in the requested sample format:
 * bits is 8, 16, 24 or 32; isFloat!=0 means 32-bit IEEE float (bits must be
 * 32).  8-bit is written as unsigned PCM (WAV convention), 16/24/32 as signed
 * little-endian PCM.  Returns 0 on success. */
int   seq_save_wav_fmt( const Sequence *s, const char *path,
                        int bits, int isFloat,
                        char *errBuf, int errBufLen );

#endif /* CWAVE_AUDIO_H */
