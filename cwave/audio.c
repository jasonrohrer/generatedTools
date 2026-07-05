/* audio.c -- implementation of the cwave audio module.  Pure C89. */

#include "audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* stb_vorbis is compiled as a separate translation unit; declare the
 * one entry point we use here to avoid pulling its header in. */
extern int stb_vorbis_decode_filename( const char *filename,
                                       int *channels, int *sample_rate,
                                       short **output );

/* -------------------------------------------------------------------- */
/* lifecycle                                                            */
/* -------------------------------------------------------------------- */

int audio_alloc( AudioClip *c, int numChannels, int sampleRate,
                 long numFrames )
{
    int i;
    if( numChannels < 1 ) numChannels = 1;
    if( numChannels > AUDIO_MAX_CHANNELS ) numChannels = AUDIO_MAX_CHANNELS;
    if( numFrames < 0 ) numFrames = 0;

    c->numChannels = numChannels;
    c->sampleRate  = sampleRate;
    c->numFrames   = numFrames;
    for( i = 0; i < AUDIO_MAX_CHANNELS; i++ ) c->channel[i] = NULL;

    for( i = 0; i < numChannels; i++ ) {
        /* allocate at least 1 element so calloc never returns NULL for 0 */
        c->channel[i] = (float *)calloc( (size_t)(numFrames > 0 ? numFrames : 1),
                                         sizeof(float) );
        if( c->channel[i] == NULL ) {
            audio_free( c );
            return 1;
        }
    }
    return 0;
}

void audio_free( AudioClip *c )
{
    int i;
    for( i = 0; i < AUDIO_MAX_CHANNELS; i++ ) {
        if( c->channel[i] ) free( c->channel[i] );
        c->channel[i] = NULL;
    }
    c->numChannels = 0;
    c->numFrames   = 0;
}

int audio_copy( AudioClip *dst, const AudioClip *src )
{
    int i;
    audio_free( dst );
    if( audio_alloc( dst, src->numChannels, src->sampleRate,
                     src->numFrames ) ) return 1;
    for( i = 0; i < src->numChannels; i++ ) {
        if( src->numFrames > 0 )
            memcpy( dst->channel[i], src->channel[i],
                    (size_t)src->numFrames * sizeof(float) );
    }
    return 0;
}

int audio_copy_range( AudioClip *dst, const AudioClip *src,
                      long start, long end )
{
    int i;
    long n;
    if( start < 0 ) start = 0;
    if( end > src->numFrames ) end = src->numFrames;
    if( end < start ) end = start;
    n = end - start;

    audio_free( dst );
    if( audio_alloc( dst, src->numChannels, src->sampleRate, n ) ) return 1;
    for( i = 0; i < src->numChannels; i++ ) {
        if( n > 0 )
            memcpy( dst->channel[i], src->channel[i] + start,
                    (size_t)n * sizeof(float) );
    }
    return 0;
}

/* -------------------------------------------------------------------- */
/* little-endian read/write helpers                                     */
/* -------------------------------------------------------------------- */

static unsigned long rd_u32( const unsigned char *p )
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}
static unsigned int rd_u16( const unsigned char *p )
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}
static void wr_u32( unsigned char *p, unsigned long v )
{
    p[0] = (unsigned char)( v         & 0xFF );
    p[1] = (unsigned char)((v >> 8)  & 0xFF );
    p[2] = (unsigned char)((v >> 16) & 0xFF );
    p[3] = (unsigned char)((v >> 24) & 0xFF );
}
static void wr_u16( unsigned char *p, unsigned int v )
{
    p[0] = (unsigned char)( v        & 0xFF );
    p[1] = (unsigned char)((v >> 8) & 0xFF );
}

/* clamp a float to [-1,1] */
static float clampf( float v )
{
    if( v >  1.0f ) return  1.0f;
    if( v < -1.0f ) return -1.0f;
    return v;
}

/* -------------------------------------------------------------------- */
/* WAV loading                                                          */
/* -------------------------------------------------------------------- */

static void seterr( char *buf, int len, const char *msg )
{
    if( buf && len > 0 ) {
        strncpy( buf, msg, (size_t)(len - 1) );
        buf[len - 1] = '\0';
    }
}

static int load_wav( AudioClip *c, const char *path, char *err, int errLen,
                     volatile int *progress, int *outBits, int *outFloat )
{
    FILE *f;
    unsigned char hdr[12];
    unsigned char chunk[8];
    unsigned char fmt[40];
    int   audioFormat = 1, numCh = 0;
    long  sampleRate = 0;
    int   bitsPerSample = 0;
    long  dataBytes = 0;
    long  dataPos = 0;
    int   haveFmt = 0;
    long  frames, i;
    int   ch;
    unsigned char *raw = NULL;
    int   bytesPerSample;

    f = fopen( path, "rb" );
    if( !f ) { seterr( err, errLen, "Cannot open file" ); return 1; }

    if( fread( hdr, 1, 12, f ) != 12 ||
        memcmp( hdr, "RIFF", 4 ) != 0 || memcmp( hdr + 8, "WAVE", 4 ) != 0 ) {
        seterr( err, errLen, "Not a RIFF/WAVE file" );
        fclose( f ); return 1;
    }

    /* walk chunks */
    while( fread( chunk, 1, 8, f ) == 8 ) {
        unsigned long csize = rd_u32( chunk + 4 );
        if( memcmp( chunk, "fmt ", 4 ) == 0 ) {
            unsigned long toRead = csize < sizeof(fmt) ? csize : sizeof(fmt);
            if( fread( fmt, 1, toRead, f ) != toRead ) {
                seterr( err, errLen, "Truncated fmt chunk" );
                fclose( f ); return 1;
            }
            audioFormat   = (int)rd_u16( fmt );
            numCh         = (int)rd_u16( fmt + 2 );
            sampleRate    = (long)rd_u32( fmt + 4 );
            bitsPerSample = (int)rd_u16( fmt + 14 );
            haveFmt = 1;
            /* skip any remaining fmt bytes */
            if( csize > toRead )
                fseek( f, (long)(csize - toRead), SEEK_CUR );
        } else if( memcmp( chunk, "data", 4 ) == 0 ) {
            dataBytes = (long)csize;
            dataPos   = ftell( f );
            fseek( f, (long)csize, SEEK_CUR );
        } else {
            fseek( f, (long)csize, SEEK_CUR );
        }
        /* chunks are word-aligned */
        if( csize & 1 ) fseek( f, 1, SEEK_CUR );
    }

    if( !haveFmt || dataPos == 0 ) {
        seterr( err, errLen, "Missing fmt or data chunk" );
        fclose( f ); return 1;
    }
    if( numCh < 1 || numCh > AUDIO_MAX_CHANNELS ) {
        seterr( err, errLen, "Unsupported channel count" );
        fclose( f ); return 1;
    }
    /* audioFormat: 1 = PCM int, 3 = IEEE float, 0xFFFE = extensible */
    if( audioFormat != 1 && audioFormat != 3 && audioFormat != (int)0xFFFE ) {
        seterr( err, errLen, "Unsupported WAV encoding (not PCM/float)" );
        fclose( f ); return 1;
    }
    if( bitsPerSample != 8 && bitsPerSample != 16 &&
        bitsPerSample != 24 && bitsPerSample != 32 ) {
        seterr( err, errLen, "Unsupported bit depth" );
        fclose( f ); return 1;
    }

    if( outBits )  *outBits  = bitsPerSample;
    if( outFloat ) *outFloat = ( audioFormat == 3 );

    bytesPerSample = bitsPerSample / 8;
    frames = dataBytes / ( bytesPerSample * numCh );

    if( audio_alloc( c, numCh, (int)sampleRate, frames ) ) {
        seterr( err, errLen, "Out of memory" );
        fclose( f ); return 1;
    }

    raw = (unsigned char *)malloc( (size_t)dataBytes > 0 ? (size_t)dataBytes : 1 );
    if( !raw ) {
        seterr( err, errLen, "Out of memory" );
        audio_free( c ); fclose( f ); return 1;
    }
    fseek( f, dataPos, SEEK_SET );
    /* read in chunks so a large file can report progress (phase 0..400) */
    {
        long got = 0;
        const long CHUNK = 4L * 1024 * 1024;
        while( got < dataBytes ) {
            long want = dataBytes - got;
            size_t rd;
            if( want > CHUNK ) want = CHUNK;
            rd = fread( raw + got, 1, (size_t)want, f );
            got += (long)rd;
            if( progress && dataBytes > 0 )
                *progress = (int)( 400.0 * (double)got / (double)dataBytes );
            if( rd < (size_t)want ) break; /* tolerate short read */
        }
    }
    fclose( f );

    for( i = 0; i < frames; i++ ) {
        if( progress && ( i & 0x3FFFF ) == 0 && frames > 0 )
            *progress = 400 + (int)( 600.0 * (double)i / (double)frames );
        for( ch = 0; ch < numCh; ch++ ) {
            unsigned char *p = raw + ( i * numCh + ch ) * bytesPerSample;
            float v = 0.0f;
            if( audioFormat == 3 ) {
                /* IEEE float */
                if( bitsPerSample == 32 ) {
                    float fv;
                    memcpy( &fv, p, 4 );
                    v = fv;
                } else if( bitsPerSample == 64 ) {
                    v = 0.0f; /* not reached (blocked above) */
                }
            } else {
                /* integer PCM */
                if( bitsPerSample == 8 ) {
                    /* unsigned 8-bit, bias 128 */
                    v = ( (float)p[0] - 128.0f ) / 128.0f;
                } else if( bitsPerSample == 16 ) {
                    short s = (short)rd_u16( p );
                    v = (float)s / 32768.0f;
                } else if( bitsPerSample == 24 ) {
                    long s = (long)p[0] | ((long)p[1] << 8) | ((long)p[2] << 16);
                    if( s & 0x800000L ) s |= ~0xFFFFFFL; /* sign extend */
                    v = (float)s / 8388608.0f;
                } else if( bitsPerSample == 32 ) {
                    long s = (long)rd_u32( p );
                    v = (float)( (double)( (int)s ) / 2147483648.0 );
                }
            }
            c->channel[ch][i] = v;
        }
    }
    free( raw );
    if( progress ) *progress = 1000;
    return 0;
}

/* -------------------------------------------------------------------- */
/* OGG loading (via stb_vorbis)                                         */
/* -------------------------------------------------------------------- */

static int load_ogg( AudioClip *c, const char *path, char *err, int errLen,
                     volatile int *progress )
{
    int   channels = 0, rate = 0;
    short *data = NULL;
    int   frames;
    long  i;
    int   ch;

    if( progress ) *progress = 100;
    frames = stb_vorbis_decode_filename( path, &channels, &rate, &data );
    if( frames < 0 || data == NULL ) {
        seterr( err, errLen, "Failed to decode OGG file" );
        return 1;
    }
    if( channels < 1 || channels > AUDIO_MAX_CHANNELS ) {
        seterr( err, errLen, "Unsupported OGG channel count" );
        free( data );
        return 1;
    }
    if( audio_alloc( c, channels, rate, frames ) ) {
        seterr( err, errLen, "Out of memory" );
        free( data );
        return 1;
    }
    for( i = 0; i < frames; i++ ) {
        if( progress && ( i & 0x3FFFF ) == 0 && frames > 0 )
            *progress = 100 + (int)( 900.0 * (double)i / (double)frames );
        for( ch = 0; ch < channels; ch++ )
            c->channel[ch][i] = (float)data[i * channels + ch] / 32768.0f;
    }
    free( data );
    if( progress ) *progress = 1000;
    return 0;
}

/* -------------------------------------------------------------------- */

static int has_ext( const char *path, const char *ext )
{
    size_t lp = strlen( path ), le = strlen( ext );
    size_t k;
    if( lp < le ) return 0;
    for( k = 0; k < le; k++ ) {
        char a = path[lp - le + k];
        char b = ext[k];
        if( a >= 'A' && a <= 'Z' ) a = (char)( a - 'A' + 'a' );
        if( a != b ) return 0;
    }
    return 1;
}

int audio_load_progress( AudioClip *c, const char *path, char *err, int errLen,
                         volatile int *progress, int *outBits, int *outFloat )
{
    if( has_ext( path, ".ogg" ) ) {
        /* OGG decodes to 16-bit-equivalent samples; default save to 16-bit PCM */
        if( outBits )  *outBits  = 16;
        if( outFloat ) *outFloat = 0;
        return load_ogg( c, path, err, errLen, progress );
    }
    /* default to WAV */
    return load_wav( c, path, err, errLen, progress, outBits, outFloat );
}

int audio_load( AudioClip *c, const char *path, char *err, int errLen )
{
    return audio_load_progress( c, path, err, errLen, NULL, NULL, NULL );
}

/* -------------------------------------------------------------------- */
/* WAV saving                                                           */
/* -------------------------------------------------------------------- */

static int write_wav_header( FILE *f, int numCh, int sampleRate,
                             int bitsPerSample, int isFloat, long dataBytes )
{
    unsigned char h[44];
    long byteRate = (long)sampleRate * numCh * ( bitsPerSample / 8 );
    int  blockAlign = numCh * ( bitsPerSample / 8 );

    memcpy( h, "RIFF", 4 );
    wr_u32( h + 4, (unsigned long)( 36 + dataBytes ) );
    memcpy( h + 8, "WAVE", 4 );
    memcpy( h + 12, "fmt ", 4 );
    wr_u32( h + 16, 16 );
    wr_u16( h + 20, isFloat ? 3u : 1u );
    wr_u16( h + 22, (unsigned int)numCh );
    wr_u32( h + 24, (unsigned long)sampleRate );
    wr_u32( h + 28, (unsigned long)byteRate );
    wr_u16( h + 32, (unsigned int)blockAlign );
    wr_u16( h + 34, (unsigned int)bitsPerSample );
    memcpy( h + 36, "data", 4 );
    wr_u32( h + 40, (unsigned long)dataBytes );

    return fwrite( h, 1, 44, f ) == 44 ? 0 : 1;
}

int audio_save_wav( const AudioClip *c, const char *path,
                    char *err, int errLen )
{
    FILE *f;
    long  i;
    int   ch;
    long  dataBytes = c->numFrames * c->numChannels * 2;

    f = fopen( path, "wb" );
    if( !f ) { seterr( err, errLen, "Cannot create file" ); return 1; }

    if( write_wav_header( f, c->numChannels, c->sampleRate, 16, 0,
                          dataBytes ) ) {
        seterr( err, errLen, "Write error" ); fclose( f ); return 1;
    }
    for( i = 0; i < c->numFrames; i++ ) {
        for( ch = 0; ch < c->numChannels; ch++ ) {
            unsigned char b[2];
            float v = clampf( c->channel[ch][i] );
            int s = (int)( v * 32767.0f + ( v >= 0.0f ? 0.5f : -0.5f ) );
            if( s > 32767 ) s = 32767;
            if( s < -32768 ) s = -32768;
            wr_u16( b, (unsigned int)( s & 0xFFFF ) );
            fwrite( b, 1, 2, f );
        }
    }
    fclose( f );
    return 0;
}

int audio_save_wav_float( const AudioClip *c, const char *path,
                          char *err, int errLen )
{
    FILE *f;
    long  i;
    int   ch;
    long  dataBytes = c->numFrames * c->numChannels * 4;

    f = fopen( path, "wb" );
    if( !f ) { seterr( err, errLen, "Cannot create file" ); return 1; }

    if( write_wav_header( f, c->numChannels, c->sampleRate, 32, 1,
                          dataBytes ) ) {
        seterr( err, errLen, "Write error" ); fclose( f ); return 1;
    }
    for( i = 0; i < c->numFrames; i++ ) {
        for( ch = 0; ch < c->numChannels; ch++ ) {
            float v = c->channel[ch][i];
            fwrite( &v, 4, 1, f );
        }
    }
    fclose( f );
    return 0;
}

/* -------------------------------------------------------------------- */
/* effects                                                              */
/* -------------------------------------------------------------------- */

static void clip_range( const AudioClip *c, long *start, long *end )
{
    if( *start < 0 ) *start = 0;
    if( *end > c->numFrames ) *end = c->numFrames;
    if( *end < *start ) *end = *start;
}

float audio_peak( const AudioClip *c, long start, long end )
{
    long i;
    int  ch;
    float peak = 0.0f;
    clip_range( c, &start, &end );
    for( ch = 0; ch < c->numChannels; ch++ ) {
        float *s = c->channel[ch];
        for( i = start; i < end; i++ ) {
            float a = s[i] < 0.0f ? -s[i] : s[i];
            if( a > peak ) peak = a;
        }
    }
    return peak;
}

void audio_normalize( AudioClip *c, long start, long end, float peak )
{
    float cur = audio_peak( c, start, end );
    float gain;
    if( cur <= 0.0f ) return;
    gain = peak / cur;
    audio_amplify( c, start, end, gain );
}

void audio_amplify( AudioClip *c, long start, long end, float gain )
{
    long i;
    int  ch;
    clip_range( c, &start, &end );
    for( ch = 0; ch < c->numChannels; ch++ ) {
        float *s = c->channel[ch];
        for( i = start; i < end; i++ ) s[i] *= gain;
    }
}

void audio_fade_in( AudioClip *c, long start, long end )
{
    long i, n;
    int  ch;
    clip_range( c, &start, &end );
    n = end - start;
    if( n <= 1 ) return;
    for( ch = 0; ch < c->numChannels; ch++ ) {
        float *s = c->channel[ch];
        for( i = start; i < end; i++ )
            s[i] *= (float)( i - start ) / (float)( n - 1 );
    }
}

void audio_fade_out( AudioClip *c, long start, long end )
{
    long i, n;
    int  ch;
    clip_range( c, &start, &end );
    n = end - start;
    if( n <= 1 ) return;
    for( ch = 0; ch < c->numChannels; ch++ ) {
        float *s = c->channel[ch];
        for( i = start; i < end; i++ )
            s[i] *= 1.0f - (float)( i - start ) / (float)( n - 1 );
    }
}

void audio_silence( AudioClip *c, long start, long end )
{
    long i;
    int  ch;
    clip_range( c, &start, &end );
    for( ch = 0; ch < c->numChannels; ch++ ) {
        float *s = c->channel[ch];
        for( i = start; i < end; i++ ) s[i] = 0.0f;
    }
}

void audio_reverse( AudioClip *c, long start, long end )
{
    long i, j;
    int  ch;
    clip_range( c, &start, &end );
    for( ch = 0; ch < c->numChannels; ch++ ) {
        float *s = c->channel[ch];
        i = start; j = end - 1;
        while( i < j ) {
            float t = s[i]; s[i] = s[j]; s[j] = t;
            i++; j--;
        }
    }
}

int audio_delete_range( AudioClip *c, long start, long end )
{
    long tail;
    int  ch;
    clip_range( c, &start, &end );
    if( end <= start ) return 0;
    tail = c->numFrames - end;
    for( ch = 0; ch < c->numChannels; ch++ ) {
        /* slide the tail down over the gap in one move; leave the allocation
         * as-is (just shrink the logical length) so this stays O(tail) */
        memmove( c->channel[ch] + start, c->channel[ch] + end,
                 (size_t)tail * sizeof(float) );
    }
    c->numFrames = start + tail;
    return 0;
}

int audio_trim_to_range( AudioClip *c, long start, long end )
{
    AudioClip tmp;
    int i;
    /* tmp must be a valid (empty) clip before audio_copy_range frees it */
    memset( &tmp, 0, sizeof(tmp) );
    clip_range( c, &start, &end );
    if( audio_copy_range( &tmp, c, start, end ) ) return 1;
    /* move tmp into c */
    audio_free( c );
    c->numChannels = tmp.numChannels;
    c->sampleRate  = tmp.sampleRate;
    c->numFrames   = tmp.numFrames;
    for( i = 0; i < AUDIO_MAX_CHANNELS; i++ ) c->channel[i] = tmp.channel[i];
    return 0;
}

int audio_insert( AudioClip *c, long at, const AudioClip *ins )
{
    long tail, newFrames, insN;
    int  ch, nch = c->numChannels;

    if( at < 0 ) at = 0;
    if( at > c->numFrames ) at = c->numFrames;
    if( ins->numFrames == 0 ) return 0;

    /* if c is empty, adopt the inserted clip's channel count / rate */
    if( c->numFrames == 0 && c->numChannels == 0 ) {
        return audio_copy( c, ins );
    }

    insN      = ins->numFrames;
    tail      = c->numFrames - at;
    newFrames = c->numFrames + insN;

    /* grow each channel in place, shift its tail up to open a gap, and copy
     * the inserted data in.  O(tail + insN) rather than rebuilding the clip;
     * pasting at the end (tail == 0) is just a realloc + a small copy. */
    for( ch = 0; ch < nch; ch++ ) {
        int    sch = ( ch < ins->numChannels ) ? ch : 0;
        float *p   = (float *)realloc( c->channel[ch],
                                       (size_t)newFrames * sizeof(float) );
        if( !p ) return 1;
        memmove( p + at + insN, p + at, (size_t)tail * sizeof(float) );
        memcpy( p + at, ins->channel[sch], (size_t)insN * sizeof(float) );
        c->channel[ch] = p;
    }
    c->numFrames = newFrames;
    return 0;
}

int audio_splice( AudioClip *c, long at, long removeLen,
                  float *const ins[], long insLen )
{
    long tail, newFrames;
    int  ch, nch = c->numChannels;

    if( at < 0 ) at = 0;
    if( at > c->numFrames ) at = c->numFrames;
    if( removeLen < 0 ) removeLen = 0;
    if( at + removeLen > c->numFrames ) removeLen = c->numFrames - at;
    if( insLen < 0 ) insLen = 0;
    tail      = c->numFrames - at - removeLen;   /* frames after the edit */
    newFrames = c->numFrames - removeLen + insLen;

    if( removeLen == insLen ) {
        /* pure in-place overwrite: no length change, no reallocation */
        for( ch = 0; ch < nch; ch++ )
            if( insLen > 0 )
                memcpy( c->channel[ch] + at, ins[ch],
                        (size_t)insLen * sizeof(float) );
        return 0;
    }

    if( newFrames > c->numFrames ) {
        /* net growth: enlarge, slide the tail up, drop in the new data */
        for( ch = 0; ch < nch; ch++ ) {
            float *p = (float *)realloc( c->channel[ch],
                          (size_t)( newFrames > 0 ? newFrames : 1 ) *
                          sizeof(float) );
            if( !p ) return 1;
            memmove( p + at + insLen, p + at + removeLen,
                     (size_t)tail * sizeof(float) );
            if( insLen > 0 )
                memcpy( p + at, ins[ch], (size_t)insLen * sizeof(float) );
            c->channel[ch] = p;
        }
    } else {
        /* net shrink: slide the tail down over the gap; keep the allocation */
        for( ch = 0; ch < nch; ch++ ) {
            float *p = c->channel[ch];
            memmove( p + at + insLen, p + at + removeLen,
                     (size_t)tail * sizeof(float) );
            if( insLen > 0 )
                memcpy( p + at, ins[ch], (size_t)insLen * sizeof(float) );
        }
    }
    c->numFrames = newFrames;
    return 0;
}

/* ==================================================================== */
/* Sequence -- block-list document (see audio.h for the rationale)      */
/* ==================================================================== */

static long ceil_div( long a, long b ) { return ( a + b - 1 ) / b; }

/* ---- Block ---- */

static void block_free( Block *b )
{
    int ch;
    if( !b ) return;
    audio_free( &b->buf );
    for( ch = 0; ch < AUDIO_MAX_CHANNELS; ch++ ) {
        if( b->mn[ch] ) free( b->mn[ch] );
        if( b->mx[ch] ) free( b->mx[ch] );
        b->mn[ch] = b->mx[ch] = NULL;
    }
    b->numBins = 0;
    free( b );
}

/* (re)compute a block's per-bin min/max summary from its samples */
static int block_bins( Block *b )
{
    int  nch = b->buf.numChannels, ch;
    long len = b->buf.numFrames, nb = ceil_div( len, SEQ_BIN ), bi;

    for( ch = 0; ch < AUDIO_MAX_CHANNELS; ch++ ) {
        if( b->mn[ch] ) { free( b->mn[ch] ); b->mn[ch] = NULL; }
        if( b->mx[ch] ) { free( b->mx[ch] ); b->mx[ch] = NULL; }
    }
    b->numBins = nb;
    if( nb <= 0 ) return 0;
    for( ch = 0; ch < nch; ch++ ) {
        float *s   = b->buf.channel[ch];
        float *omn = (float *)malloc( (size_t)nb * sizeof(float) );
        float *omx = (float *)malloc( (size_t)nb * sizeof(float) );
        if( !omn || !omx ) { if( omn ) free( omn ); if( omx ) free( omx );
                             return 1; }
        for( bi = 0; bi < nb; bi++ ) {
            long  i0 = bi * SEQ_BIN, i1 = i0 + SEQ_BIN, i;
            float mn, mx;
            if( i1 > len ) i1 = len;
            mn = mx = s[i0];
            for( i = i0 + 1; i < i1; i++ ) {
                float v = s[i];
                if( v < mn ) mn = v;
                if( v > mx ) mx = v;
            }
            omn[bi] = mn; omx[bi] = mx;
        }
        b->mn[ch] = omn; b->mx[ch] = omx;
    }
    return 0;
}

/* create a block of 'len' frames, channel c copied from src[map(c)]+off */
static Block *block_make( int nch, int rate, float *const src[],
                          const int *srcMap, long off, long len )
{
    Block *b = (Block *)calloc( 1, sizeof(Block) );
    int    ch;
    if( !b ) return NULL;
    if( audio_alloc( &b->buf, nch, rate, len ) ) { free( b ); return NULL; }
    for( ch = 0; ch < nch; ch++ ) {
        int sc = srcMap ? srcMap[ch] : ch;
        if( len > 0 )
            memcpy( b->buf.channel[ch], src[sc] + off,
                    (size_t)len * sizeof(float) );
    }
    if( block_bins( b ) ) { block_free( b ); return NULL; }
    return b;
}

/* ---- Sequence array housekeeping ---- */

void seq_init( Sequence *s ) { memset( s, 0, sizeof(*s) ); }

void seq_free( Sequence *s )
{
    int i;
    for( i = 0; i < s->numBlocks; i++ ) block_free( s->blocks[i] );
    if( s->blocks ) free( s->blocks );
    if( s->start )  free( s->start );
    memset( s, 0, sizeof(*s) );
}

static int seq_reserve( Sequence *s, int n )
{
    int    nc;
    Block **nb;
    long  *ns;
    if( n <= s->capBlocks ) return 0;
    nc = s->capBlocks ? s->capBlocks : 8;
    while( nc < n ) nc *= 2;
    nb = (Block **)realloc( s->blocks, (size_t)nc * sizeof(Block *) );
    if( !nb ) return 1;
    s->blocks = nb;
    ns = (long *)realloc( s->start, (size_t)( nc + 1 ) * sizeof(long) );
    if( !ns ) return 1;
    s->start = ns;
    s->capBlocks = nc;
    return 0;
}

/* recompute start[] prefix sums and numFrames */
static void seq_reindex( Sequence *s )
{
    long acc = 0;
    int  i;
    for( i = 0; i < s->numBlocks; i++ ) {
        s->start[i] = acc;
        acc += s->blocks[i]->buf.numFrames;
    }
    s->start[s->numBlocks] = acc;
    s->numFrames = acc;
}

/* find the block containing absolute 'frame' (0<=frame<numFrames) */
void seq_locate( const Sequence *s, long frame, int *bIdx, long *local )
{
    int lo = 0, hi = s->numBlocks - 1, res = 0;
    if( frame < 0 ) frame = 0;
    if( s->numBlocks == 0 || frame >= s->numFrames ) {
        *bIdx = s->numBlocks; *local = 0; return;
    }
    while( lo <= hi ) {
        int mid = ( lo + hi ) / 2;
        if( s->start[mid] <= frame ) { res = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    *bIdx = res; *local = frame - s->start[res];
}

/* ensure a block boundary exists exactly at absFrame; return the index of the
 * block that then STARTS at absFrame (numBlocks if absFrame==numFrames).
 * Returns -1 on allocation failure. */
static int seq_split_at( Sequence *s, long absFrame )
{
    int   bIdx, i;
    long  local;
    Block *b, *b0, *b1;

    if( absFrame <= 0 ) return 0;
    if( absFrame >= s->numFrames ) return s->numBlocks;
    seq_locate( s, absFrame, &bIdx, &local );
    if( local == 0 ) return bIdx;                 /* already a boundary */

    b  = s->blocks[bIdx];
    b0 = block_make( b->buf.numChannels, b->buf.sampleRate,
                     b->buf.channel, NULL, 0, local );
    b1 = block_make( b->buf.numChannels, b->buf.sampleRate,
                     b->buf.channel, NULL, local, b->buf.numFrames - local );
    if( !b0 || !b1 ) { if( b0 ) block_free( b0 ); if( b1 ) block_free( b1 );
                       return -1; }
    if( seq_reserve( s, s->numBlocks + 1 ) ) { block_free( b0 );
                                               block_free( b1 ); return -1; }
    for( i = s->numBlocks - 1; i >= bIdx + 1; i-- )
        s->blocks[i + 1] = s->blocks[i];
    s->blocks[bIdx]     = b0;
    s->blocks[bIdx + 1] = b1;
    s->numBlocks++;
    block_free( b );
    seq_reindex( s );
    return bIdx + 1;
}

/* insert 'count' ready blocks at index idx (array takes ownership) */
static int seq_insert_blocks( Sequence *s, int idx, Block **nb, int count )
{
    int i;
    if( count <= 0 ) return 0;
    if( seq_reserve( s, s->numBlocks + count ) ) return 1;
    for( i = s->numBlocks - 1; i >= idx; i-- )
        s->blocks[i + count] = s->blocks[i];
    for( i = 0; i < count; i++ ) s->blocks[idx + i] = nb[i];
    s->numBlocks += count;
    seq_reindex( s );
    return 0;
}

/* remove and free 'count' blocks starting at idx */
static void seq_remove_blocks( Sequence *s, int idx, int count )
{
    int i;
    if( count <= 0 ) return;
    for( i = idx; i < idx + count; i++ ) block_free( s->blocks[i] );
    for( i = idx + count; i < s->numBlocks; i++ )
        s->blocks[i - count] = s->blocks[i];
    s->numBlocks -= count;
    seq_reindex( s );
}

/* merge adjacent blocks around indices [lo,hi] whose combined length fits in
 * SEQ_BLOCK_FRAMES, so repeated small edits (e.g. train-car pastes) do not
 * fragment the list into countless tiny blocks. */
static void seq_coalesce_around( Sequence *s, int lo, int hi )
{
    int i;
    if( lo < 0 ) lo = 0;
    if( hi > s->numBlocks - 1 ) hi = s->numBlocks - 1;
    i = lo;
    while( i >= 0 && i < s->numBlocks - 1 && i <= hi ) {
        Block *a = s->blocks[i], *b = s->blocks[i + 1];
        long la = a->buf.numFrames, lb = b->buf.numFrames;
        if( la + lb <= SEQ_BLOCK_FRAMES ) {
            /* build a merged block from a followed by b */
            int    nch = a->buf.numChannels, ch;
            Block *m = (Block *)calloc( 1, sizeof(Block) );
            if( !m ) return;
            if( audio_alloc( &m->buf, nch, a->buf.sampleRate, la + lb ) ) {
                free( m ); return;
            }
            for( ch = 0; ch < nch; ch++ ) {
                if( la > 0 ) memcpy( m->buf.channel[ch], a->buf.channel[ch],
                                     (size_t)la * sizeof(float) );
                if( lb > 0 ) memcpy( m->buf.channel[ch] + la,
                                     b->buf.channel[ch],
                                     (size_t)lb * sizeof(float) );
            }
            if( block_bins( m ) ) { block_free( m ); return; }
            block_free( a ); block_free( b );
            s->blocks[i] = m;
            { int k; for( k = i + 2; k < s->numBlocks; k++ )
                         s->blocks[k - 1] = s->blocks[k]; }
            s->numBlocks--;
            hi--;
            seq_reindex( s );
            /* stay at i to keep merging into this block */
        } else {
            i++;
        }
    }
}

/* ---- Sequence lifecycle ---- */

int seq_set_empty( Sequence *s, int numChannels, int sampleRate )
{
    int i;
    for( i = 0; i < s->numBlocks; i++ ) block_free( s->blocks[i] );
    s->numBlocks   = 0;
    s->numFrames   = 0;
    s->numChannels = numChannels;
    s->sampleRate  = sampleRate;
    if( s->start && s->capBlocks > 0 ) s->start[0] = 0;
    return 0;
}

int seq_adopt_clip( Sequence *s, AudioClip *src )
{
    long len = src->numFrames, off;
    int  count, k, ch;

    seq_free( s );
    s->numChannels = src->numChannels;
    s->sampleRate  = src->sampleRate;
    if( len <= 0 ) { audio_free( src ); return 0; }

    count = (int)ceil_div( len, SEQ_BLOCK_FRAMES );
    if( seq_reserve( s, count ) ) return 1;
    for( k = 0, off = 0; k < count; k++, off += SEQ_BLOCK_FRAMES ) {
        long   bl = len - off;
        Block *b;
        if( bl > SEQ_BLOCK_FRAMES ) bl = SEQ_BLOCK_FRAMES;
        b = (Block *)calloc( 1, sizeof(Block) );
        if( !b ) return 1;
        if( audio_alloc( &b->buf, src->numChannels, src->sampleRate, bl ) ) {
            free( b ); return 1;
        }
        for( ch = 0; ch < src->numChannels; ch++ )
            memcpy( b->buf.channel[ch], src->channel[ch] + off,
                    (size_t)bl * sizeof(float) );
        if( block_bins( b ) ) { block_free( b ); return 1; }
        s->blocks[s->numBlocks++] = b;
    }
    seq_reindex( s );
    audio_free( src );
    return 0;
}

/* ---- reads ---- */

float seq_sample( const Sequence *s, int ch, long frame )
{
    int  bIdx;
    long local;
    if( frame < 0 || frame >= s->numFrames ) return 0.0f;
    seq_locate( s, frame, &bIdx, &local );
    if( bIdx >= s->numBlocks ) return 0.0f;
    return s->blocks[bIdx]->buf.channel[ch][local];
}

int seq_read_range( AudioClip *dst, const Sequence *s, long start, long end )
{
    int  i, ch;
    long n;
    if( start < 0 ) start = 0;
    if( end > s->numFrames ) end = s->numFrames;
    if( end < start ) end = start;
    n = end - start;
    audio_free( dst );   /* release any prior contents so reuse does not leak */
    if( audio_alloc( dst, s->numChannels, s->sampleRate, n ) ) return 1;
    if( n <= 0 ) return 0;
    { int b; long loc; seq_locate( s, start, &b, &loc ); i = b; }
    for( ; i < s->numBlocks && s->start[i] < end; i++ ) {
        long bs = s->start[i], be = s->start[i + 1];
        long lo = start > bs ? start : bs;
        long hi = end   < be ? end   : be;
        long srcLoc = lo - bs, dstOff = lo - start, cnt = hi - lo;
        Block *b = s->blocks[i];
        if( cnt <= 0 ) continue;
        for( ch = 0; ch < s->numChannels; ch++ )
            memcpy( dst->channel[ch] + dstOff, b->buf.channel[ch] + srcLoc,
                    (size_t)cnt * sizeof(float) );
    }
    return 0;
}

/* overwrite [start,end) from contiguous src (length end-start), then refresh
 * the summary bins of every block the write touched */
static void seq_write_range( Sequence *s, long start, long end,
                             const AudioClip *src )
{
    int  i, ch;
    if( start < 0 ) start = 0;
    if( end > s->numFrames ) end = s->numFrames;
    if( end <= start ) return;
    { int b; long loc; seq_locate( s, start, &b, &loc ); i = b; }
    for( ; i < s->numBlocks && s->start[i] < end; i++ ) {
        long bs = s->start[i], be = s->start[i + 1];
        long lo = start > bs ? start : bs;
        long hi = end   < be ? end   : be;
        long dstLoc = lo - bs, srcOff = lo - start, cnt = hi - lo;
        Block *b = s->blocks[i];
        if( cnt <= 0 ) continue;
        for( ch = 0; ch < s->numChannels; ch++ )
            memcpy( b->buf.channel[ch] + dstLoc, src->channel[ch] + srcOff,
                    (size_t)cnt * sizeof(float) );
        block_bins( b );
    }
}

int seq_col_minmax( const Sequence *s, int ch, double f0, double f1,
                    double spp, float *mn, float *mx )
{
    long i0 = (long)f0, i1 = (long)f1, n = s->numFrames;
    int  i, any = 0;
    float lo = 0.0f, hi = 0.0f;
    if( i0 < 0 ) i0 = 0;
    if( i1 > n ) i1 = n;
    if( i1 <= i0 ) i1 = i0 + 1;
    if( i0 >= n ) return 0;

    if( spp < (double)SEQ_BIN ) {
        /* zoomed in enough that scanning raw samples is cheap */
        int  b; long loc;
        seq_locate( s, i0, &b, &loc );
        for( i = b; i < s->numBlocks && s->start[i] < i1; i++ ) {
            long bs = s->start[i], be = s->start[i + 1];
            long a = ( i0 > bs ? i0 : bs ) - bs;
            long z = ( i1 < be ? i1 : be ) - bs;
            float *smp = s->blocks[i]->buf.channel[ch];
            long j;
            for( j = a; j < z; j++ ) {
                float v = smp[j];
                if( !any ) { lo = hi = v; any = 1; }
                else { if( v < lo ) lo = v; if( v > hi ) hi = v; }
            }
        }
    } else {
        /* summarize through the block bins */
        int  b; long loc;
        seq_locate( s, i0, &b, &loc );
        for( i = b; i < s->numBlocks && s->start[i] < i1; i++ ) {
            Block *blk = s->blocks[i];
            long bs = s->start[i], be = s->start[i + 1];
            long a = ( i0 > bs ? i0 : bs ) - bs;   /* block-local frame span */
            long z = ( i1 < be ? i1 : be ) - bs;
            long b0 = a / SEQ_BIN, b1 = ( z + SEQ_BIN - 1 ) / SEQ_BIN, k;
            float *pmn = blk->mn[ch], *pmx = blk->mx[ch];
            if( b1 > blk->numBins ) b1 = blk->numBins;
            for( k = b0; k < b1; k++ ) {
                if( !any ) { lo = pmn[k]; hi = pmx[k]; any = 1; }
                else { if( pmn[k] < lo ) lo = pmn[k];
                       if( pmx[k] > hi ) hi = pmx[k]; }
            }
        }
    }
    if( !any ) return 0;
    *mn = lo; *mx = hi; return 1;
}

/* ---- structural edits ---- */

int seq_delete_range( Sequence *s, long start, long end )
{
    int i0, i1;
    if( start < 0 ) start = 0;
    if( end > s->numFrames ) end = s->numFrames;
    if( end <= start ) return 0;
    i0 = seq_split_at( s, start );
    if( i0 < 0 ) return 1;
    i1 = seq_split_at( s, end );
    if( i1 < 0 ) return 1;
    seq_remove_blocks( s, i0, i1 - i0 );
    seq_coalesce_around( s, i0 - 1, i0 );
    return 0;
}

int seq_insert_clip( Sequence *s, long at, const AudioClip *ins )
{
    long   len = ins->numFrames, off;
    int    idx, count, k, ch;
    int    map[AUDIO_MAX_CHANNELS];
    Block **nb;

    if( len <= 0 ) return 0;
    if( s->numFrames == 0 && s->numBlocks == 0 ) {
        s->numChannels = ins->numChannels;
        s->sampleRate  = ins->sampleRate;
    }
    for( ch = 0; ch < s->numChannels; ch++ )
        map[ch] = ch < ins->numChannels ? ch : 0;

    if( at < 0 ) at = 0;
    if( at > s->numFrames ) at = s->numFrames;
    idx = seq_split_at( s, at );
    if( idx < 0 ) return 1;

    count = (int)ceil_div( len, SEQ_BLOCK_FRAMES );
    nb = (Block **)malloc( (size_t)count * sizeof(Block *) );
    if( !nb ) return 1;
    for( k = 0, off = 0; k < count; k++, off += SEQ_BLOCK_FRAMES ) {
        long bl = len - off;
        if( bl > SEQ_BLOCK_FRAMES ) bl = SEQ_BLOCK_FRAMES;
        nb[k] = block_make( s->numChannels, s->sampleRate,
                            ins->channel, map, off, bl );
        if( !nb[k] ) { int j; for( j = 0; j < k; j++ ) block_free( nb[j] );
                       free( nb ); return 1; }
    }
    if( seq_insert_blocks( s, idx, nb, count ) ) {
        int j; for( j = 0; j < count; j++ ) block_free( nb[j] );
        free( nb ); return 1;
    }
    free( nb );
    seq_coalesce_around( s, idx - 1, idx + count );
    return 0;
}

int seq_splice( Sequence *s, long at, long removeLen,
                float *const ins[], long insLen )
{
    if( at < 0 ) at = 0;
    if( at > s->numFrames ) at = s->numFrames;
    if( removeLen < 0 ) removeLen = 0;
    if( at + removeLen > s->numFrames ) removeLen = s->numFrames - at;
    if( removeLen > 0 ) seq_delete_range( s, at, at + removeLen );
    if( insLen > 0 && ins ) {
        AudioClip tmp;
        int ch;
        memset( &tmp, 0, sizeof(tmp) );
        tmp.numChannels = s->numChannels;
        tmp.sampleRate  = s->sampleRate;
        tmp.numFrames   = insLen;
        for( ch = 0; ch < s->numChannels; ch++ )
            tmp.channel[ch] = ins[ch];               /* borrow, do not free */
        return seq_insert_clip( s, at, &tmp );
    }
    return 0;
}

/* ---- effects ---- */

float seq_peak( const Sequence *s, long start, long end )
{
    int   i;
    float peak = 0.0f;
    if( start < 0 ) start = 0;
    if( end > s->numFrames ) end = s->numFrames;
    if( end <= start ) return 0.0f;
    { int b; long loc; seq_locate( s, start, &b, &loc ); i = b; }
    for( ; i < s->numBlocks && s->start[i] < end; i++ ) {
        long bs = s->start[i], be = s->start[i + 1];
        long a = ( start > bs ? start : bs ) - bs;
        long z = ( end   < be ? end   : be ) - bs;
        float p = audio_peak( &s->blocks[i]->buf, a, z );
        if( p > peak ) peak = p;
    }
    return peak;
}

void seq_amplify( Sequence *s, long start, long end, float gain )
{
    int i;
    if( start < 0 ) start = 0;
    if( end > s->numFrames ) end = s->numFrames;
    if( end <= start ) return;
    { int b; long loc; seq_locate( s, start, &b, &loc ); i = b; }
    for( ; i < s->numBlocks && s->start[i] < end; i++ ) {
        long bs = s->start[i], be = s->start[i + 1];
        long a = ( start > bs ? start : bs ) - bs;
        long z = ( end   < be ? end   : be ) - bs;
        audio_amplify( &s->blocks[i]->buf, a, z, gain );
        block_bins( s->blocks[i] );
    }
}

void seq_normalize( Sequence *s, long start, long end, float peak )
{
    float cur = seq_peak( s, start, end );
    if( cur <= 0.0f ) return;
    seq_amplify( s, start, end, peak / cur );
}

void seq_silence( Sequence *s, long start, long end )
{
    int i;
    if( start < 0 ) start = 0;
    if( end > s->numFrames ) end = s->numFrames;
    if( end <= start ) return;
    { int b; long loc; seq_locate( s, start, &b, &loc ); i = b; }
    for( ; i < s->numBlocks && s->start[i] < end; i++ ) {
        long bs = s->start[i], be = s->start[i + 1];
        long a = ( start > bs ? start : bs ) - bs;
        long z = ( end   < be ? end   : be ) - bs;
        audio_silence( &s->blocks[i]->buf, a, z );
        block_bins( s->blocks[i] );
    }
}

/* fade factor is position-dependent across the whole [start,end); compute it
 * from the GLOBAL frame index while walking each block */
static void seq_fade( Sequence *s, long start, long end, int fadeOut )
{
    long n;
    int  i, ch;
    if( start < 0 ) start = 0;
    if( end > s->numFrames ) end = s->numFrames;
    if( end <= start ) return;
    n = end - start;
    if( n <= 1 ) return;
    { int b; long loc; seq_locate( s, start, &b, &loc ); i = b; }
    for( ; i < s->numBlocks && s->start[i] < end; i++ ) {
        Block *blk = s->blocks[i];
        long bs = s->start[i], be = s->start[i + 1];
        long lo = ( start > bs ? start : bs );
        long hi = ( end   < be ? end   : be );
        long j;
        for( ch = 0; ch < s->numChannels; ch++ ) {
            float *smp = blk->buf.channel[ch];
            for( j = lo; j < hi; j++ ) {
                float f = (float)( j - start ) / (float)( n - 1 );
                if( fadeOut ) f = 1.0f - f;
                smp[j - bs] *= f;
            }
        }
        block_bins( blk );
    }
}

void seq_fade_in ( Sequence *s, long start, long end ) { seq_fade( s, start, end, 0 ); }
void seq_fade_out( Sequence *s, long start, long end ) { seq_fade( s, start, end, 1 ); }

void seq_reverse( Sequence *s, long start, long end )
{
    AudioClip tmp;
    if( start < 0 ) start = 0;
    if( end > s->numFrames ) end = s->numFrames;
    if( end <= start ) return;
    memset( &tmp, 0, sizeof(tmp) );
    if( seq_read_range( &tmp, s, start, end ) ) return;
    audio_reverse( &tmp, 0, tmp.numFrames );
    seq_write_range( s, start, end, &tmp );
    audio_free( &tmp );
}

/* ---- save ---- */

/* Encode one float sample [-1,1] into 'out' (byteWidth bytes) in the given
 * WAV format.  8-bit is unsigned (bias 128); 16/24/32-int are signed LE;
 * 32-float is raw IEEE. */
static void encode_sample( unsigned char *out, float v, int bits, int isFloat )
{
    if( isFloat ) {
        memcpy( out, &v, 4 );
        return;
    }
    v = clampf( v );
    switch( bits ) {
        case 8: {
            int s = (int)( v * 127.0f + ( v >= 0.0f ? 0.5f : -0.5f ) ) + 128;
            if( s > 255 ) s = 255;
            if( s < 0 )   s = 0;
            out[0] = (unsigned char)s;
            break;
        }
        case 24: {
            long s = (long)( v * 8388607.0 + ( v >= 0.0f ? 0.5 : -0.5 ) );
            if( s >  8388607L ) s =  8388607L;
            if( s < -8388608L ) s = -8388608L;
            out[0] = (unsigned char)(  s        & 0xFF );
            out[1] = (unsigned char)( (s >> 8)  & 0xFF );
            out[2] = (unsigned char)( (s >> 16) & 0xFF );
            break;
        }
        case 32: {
            double d = (double)v * 2147483647.0;
            long s;
            if( d >  2147483647.0 ) d =  2147483647.0;
            if( d < -2147483648.0 ) d = -2147483648.0;
            s = (long)( d + ( v >= 0.0f ? 0.5 : -0.5 ) );
            out[0] = (unsigned char)(  s        & 0xFF );
            out[1] = (unsigned char)( (s >> 8)  & 0xFF );
            out[2] = (unsigned char)( (s >> 16) & 0xFF );
            out[3] = (unsigned char)( (s >> 24) & 0xFF );
            break;
        }
        default: {   /* 16 */
            int s = (int)( v * 32767.0f + ( v >= 0.0f ? 0.5f : -0.5f ) );
            if( s >  32767 ) s =  32767;
            if( s < -32768 ) s = -32768;
            out[0] = (unsigned char)(  s       & 0xFF );
            out[1] = (unsigned char)( (s >> 8) & 0xFF );
            break;
        }
    }
}

int seq_save_wav_fmt( const Sequence *s, const char *path,
                      int bits, int isFloat, char *err, int errLen )
{
    FILE *f;
    int   i, ch;
    int   byteWidth;
    long  dataBytes;

    if( isFloat ) bits = 32;
    if( bits != 8 && bits != 16 && bits != 24 && bits != 32 ) bits = 16;
    byteWidth = bits / 8;
    dataBytes = s->numFrames * s->numChannels * byteWidth;

    f = fopen( path, "wb" );
    if( !f ) { seterr( err, errLen, "Cannot create file" ); return 1; }
    if( write_wav_header( f, s->numChannels, s->sampleRate, bits, isFloat,
                          dataBytes ) ) {
        seterr( err, errLen, "Write error" ); fclose( f ); return 1;
    }
    for( i = 0; i < s->numBlocks; i++ ) {
        Block *b = s->blocks[i];
        long   fr, nf = b->buf.numFrames;
        for( fr = 0; fr < nf; fr++ ) {
            for( ch = 0; ch < s->numChannels; ch++ ) {
                unsigned char bb[4];
                encode_sample( bb, b->buf.channel[ch][fr], bits, isFloat );
                fwrite( bb, 1, (size_t)byteWidth, f );
            }
        }
    }
    fclose( f );
    return 0;
}

int seq_save_wav( const Sequence *s, const char *path, char *err, int errLen )
{
    return seq_save_wav_fmt( s, path, 16, 0, err, errLen );
}
