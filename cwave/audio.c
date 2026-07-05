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
                     volatile int *progress )
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
                         volatile int *progress )
{
    if( has_ext( path, ".ogg" ) )
        return load_ogg( c, path, err, errLen, progress );
    /* default to WAV */
    return load_wav( c, path, err, errLen, progress );
}

int audio_load( AudioClip *c, const char *path, char *err, int errLen )
{
    return audio_load_progress( c, path, err, errLen, NULL );
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
    long tail, newFrames, i;
    int  ch;
    clip_range( c, &start, &end );
    if( end <= start ) return 0;
    tail = c->numFrames - end;
    newFrames = start + tail;
    for( ch = 0; ch < c->numChannels; ch++ ) {
        float *s = c->channel[ch];
        for( i = 0; i < tail; i++ ) s[start + i] = s[end + i];
        /* leave allocation as-is; just shrink logical length */
    }
    c->numFrames = newFrames;
    (void)start;
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
    AudioClip out;
    long newFrames, i;
    int  ch;
    int  nch = c->numChannels;

    if( at < 0 ) at = 0;
    if( at > c->numFrames ) at = c->numFrames;
    if( ins->numFrames == 0 ) return 0;

    /* if c is empty, adopt the inserted clip's channel count / rate */
    if( c->numFrames == 0 && c->numChannels == 0 ) {
        return audio_copy( c, ins );
    }

    newFrames = c->numFrames + ins->numFrames;
    if( audio_alloc( &out, nch, c->sampleRate, newFrames ) ) return 1;

    for( ch = 0; ch < nch; ch++ ) {
        /* head */
        for( i = 0; i < at; i++ )
            out.channel[ch][i] = c->channel[ch][i];
        /* inserted (use source channel ch, or channel 0 if source is mono) */
        for( i = 0; i < ins->numFrames; i++ ) {
            int sch = ( ch < ins->numChannels ) ? ch : 0;
            out.channel[ch][at + i] = ins->channel[sch][i];
        }
        /* tail */
        for( i = at; i < c->numFrames; i++ )
            out.channel[ch][ins->numFrames + i] = c->channel[ch][i];
    }

    audio_free( c );
    c->numChannels = out.numChannels;
    c->sampleRate  = out.sampleRate;
    c->numFrames   = out.numFrames;
    for( i = 0; i < AUDIO_MAX_CHANNELS; i++ ) c->channel[i] = out.channel[i];
    return 0;
}
