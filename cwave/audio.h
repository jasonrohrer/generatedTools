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

/* Compute the peak absolute sample over the range (for metering). */
float audio_peak( const AudioClip *c, long start, long end );

#endif /* CWAVE_AUDIO_H */
