/*
 * Path Master
 *
 * A tiny software synthesizer:  a handful of chip-style voices for
 * sound effects, and a quiet generated tune that loops underneath.
 * Everything is synthesized, there are no sound files.
 */

#include <string.h>

#include <SDL.h>

#include "game.h"


#define SAMPLE_RATE   22050
#define MAX_VOICES    20

enum {
    WAVE_SQUARE,
    WAVE_PULSE,
    WAVE_TRIANGLE,
    WAVE_NOISE
    };


typedef struct {
        int   active;
        int   wave;
        int   delay;
        int   left;
        int   total;
        float phase;
        float freq;
        float slide;
        float vol;
    } Voice;


/* one note of a sound effect */
typedef struct {
        int   wave;
        float freq;
        /* end frequency, slides linearly */
        float endFreq;
        float seconds;
        float vol;
        float delay;
    } Note;


typedef struct {
        int  numNotes;
        Note notes[4];
    } SoundDef;


static const SoundDef soundDefs[ NUM_SNDS ] = {
    /* shoot */    { 1, { { WAVE_PULSE, 880, 440, 0.07f, 0.10f, 0 } } },
    /* hit */      { 1, { { WAVE_NOISE, 500, 200, 0.05f, 0.10f, 0 } } },
    /* crit */     { 2, { { WAVE_NOISE, 900, 200, 0.08f, 0.14f, 0 },
                          { WAVE_SQUARE, 1320, 1760, 0.08f, 0.08f, 0 } } },
    /* kill */     { 1, { { WAVE_TRIANGLE, 330, 110, 0.14f, 0.22f, 0 } } },
    /* hurt */     { 1, { { WAVE_SQUARE, 160, 80, 0.12f, 0.16f, 0 } } },
    /* pickup */   { 2, { { WAVE_TRIANGLE, 660, 660, 0.06f, 0.2f, 0 },
                          { WAVE_TRIANGLE, 990, 990, 0.10f, 0.2f, 0.06f } } },
    /* place */    { 1, { { WAVE_TRIANGLE, 300, 420, 0.07f, 0.3f, 0 } } },
    /* merge */    { 3, { { WAVE_PULSE, 523, 523, 0.07f, 0.12f, 0 },
                          { WAVE_PULSE, 784, 784, 0.07f, 0.12f, 0.07f },
                          { WAVE_PULSE, 1047, 1047, 0.16f, 0.12f, 0.14f } } },
    /* unlock */   { 3, { { WAVE_TRIANGLE, 392, 392, 0.09f, 0.25f, 0 },
                          { WAVE_TRIANGLE, 523, 523, 0.09f, 0.25f, 0.09f },
                          { WAVE_TRIANGLE, 784, 784, 0.2f, 0.25f, 0.18f } } },
    /* level up */ { 4, { { WAVE_PULSE, 523, 523, 0.09f, 0.12f, 0 },
                          { WAVE_PULSE, 659, 659, 0.09f, 0.12f, 0.09f },
                          { WAVE_PULSE, 784, 784, 0.09f, 0.12f, 0.18f },
                          { WAVE_PULSE, 1047, 1047, 0.3f, 0.12f, 0.27f } } },
    /* card */     { 2, { { WAVE_TRIANGLE, 523, 523, 0.08f, 0.25f, 0 },
                          { WAVE_TRIANGLE, 784, 784, 0.15f, 0.25f, 0.08f } } },
    /* dissolve */ { 1, { { WAVE_NOISE, 1200, 150, 0.25f, 0.10f, 0 } } },
    /* death */    { 3, { { WAVE_SQUARE, 330, 165, 0.25f, 0.14f, 0 },
                          { WAVE_SQUARE, 247, 123, 0.25f, 0.14f, 0.25f },
                          { WAVE_SQUARE, 165, 55, 0.7f, 0.14f, 0.5f } } },
    /* boss */     { 2, { { WAVE_SQUARE, 110, 110, 0.3f, 0.14f, 0 },
                          { WAVE_SQUARE, 104, 104, 0.5f, 0.14f, 0.3f } } },
    /* error */    { 1, { { WAVE_SQUARE, 140, 120, 0.12f, 0.10f, 0 } } },
    /* click */    { 1, { { WAVE_TRIANGLE, 700, 700, 0.03f, 0.2f, 0 } } },
    /* zap */      { 1, { { WAVE_PULSE, 1800, 600, 0.05f, 0.05f, 0 } } },
    /* explode */  { 1, { { WAVE_NOISE, 400, 60, 0.3f, 0.2f, 0 } } },
    /* boss die */ { 3, { { WAVE_NOISE, 500, 50, 0.6f, 0.22f, 0 },
                          { WAVE_PULSE, 392, 784, 0.2f, 0.10f, 0.1f },
                          { WAVE_PULSE, 784, 1568, 0.4f, 0.10f, 0.3f } } }
    };


static SDL_AudioDeviceID device = 0;
static Voice         voices[ MAX_VOICES ];
static int           soundMode = 0;
static unsigned int  noiseState = 12345u;
static unsigned int  lastPlayed[ NUM_SNDS ];
static unsigned int  sampleClock = 0;

/* music state */
static int   musicStep = 0;
static int   musicSamplesLeft = 0;
static Voice musicBass;
static Voice musicLead;



static float voiceSample( Voice *inV ) {
    float out = 0.0f;
    float env;

    if( inV->delay > 0 ) {
        inV->delay--;
        return 0.0f;
        }
    if( inV->left <= 0 ) {
        inV->active = 0;
        return 0.0f;
        }
    inV->phase += inV->freq / (float)SAMPLE_RATE;
    if( inV->phase >= 1.0f ) {
        inV->phase -= 1.0f;
        if( inV->wave == WAVE_NOISE ) {
            noiseState = noiseState * 1103515245u + 12345u;
            }
        }
    switch( inV->wave ) {
        case WAVE_SQUARE:
            out = inV->phase < 0.5f ? 1.0f : -1.0f;
            break;
        case WAVE_PULSE:
            out = inV->phase < 0.25f ? 1.0f : -1.0f;
            break;
        case WAVE_TRIANGLE:
            out = inV->phase < 0.5f ? 4.0f * inV->phase - 1.0f
                                    : 3.0f - 4.0f * inV->phase;
            break;
        case WAVE_NOISE:
            out = ( ( noiseState >> 16 ) & 1 ) ? 1.0f : -1.0f;
            break;
        }
    inV->freq += inV->slide;
    /* quick attack, straight decay */
    env = (float)inV->left / (float)inV->total;
    if( inV->total - inV->left < 40 ) {
        env *= (float)( inV->total - inV->left ) / 40.0f;
        }
    inV->left--;
    return out * env * inV->vol;
    }



static void startVoice( Voice *inV, int inWave, float inFreq,
                        float inEndFreq, float inSeconds, float inVol,
                        float inDelay ) {
    inV->active = 1;
    inV->wave = inWave;
    inV->phase = 0.0f;
    inV->total = (int)( inSeconds * SAMPLE_RATE );
    if( inV->total < 1 ) {
        inV->total = 1;
        }
    inV->left = inV->total;
    inV->delay = (int)( inDelay * SAMPLE_RATE );
    /* noise is clocked much faster than its nominal pitch */
    if( inWave == WAVE_NOISE ) {
        inFreq *= 8.0f;
        inEndFreq *= 8.0f;
        }
    inV->freq = inFreq;
    inV->slide = ( inEndFreq - inFreq ) / (float)inV->total;
    inV->vol = inVol;
    }



/* a gentle minor-key loop:  bass on the beat, a wandering arpeggio */
static void musicAdvance( void ) {
    /* A minor, F, C, G:  root notes and chord tones in Hz */
    static const float roots[4] = { 110.0f, 87.31f, 130.81f, 98.0f };
    static const float chords[4][4] = {
        { 220.0f, 261.63f, 329.63f, 440.0f },
        { 174.61f, 220.0f, 261.63f, 349.23f },
        { 261.63f, 329.63f, 392.0f, 523.25f },
        { 196.0f, 246.94f, 293.66f, 392.0f }
        };
    static const signed char arp[16] = {
        0, -1, 1, 2, -1, 1, 3, -1, 2, -1, 1, 0, -1, 2, 1, -1
        };
    int bar = ( musicStep / 16 ) % 4;
    int step = musicStep % 16;
    /* every other pass through the chords, the arpeggio rests more */
    int sparse = ( ( musicStep / 64 ) % 2 ) == 1;

    if( step == 0 || step == 6 || step == 10 ) {
        startVoice( &musicBass, WAVE_TRIANGLE, roots[ bar ], roots[ bar ],
                    0.32f, 0.16f, 0.0f );
        }
    if( arp[ step ] >= 0 && !( sparse && ( step % 4 ) == 2 ) ) {
        float f = chords[ bar ][ (int)arp[ step ] ] * 2.0f;
        startVoice( &musicLead, WAVE_PULSE, f, f, 0.22f, 0.035f, 0.0f );
        }
    musicStep++;
    }



static void audioCallback( void *inUser, Uint8 *inStream, int inLen ) {
    Sint16 *out = (Sint16 *)inStream;
    int n = inLen / 2;
    int i, v;

    (void)inUser;

    for( i = 0; i < n; i++ ) {
        float mix = 0.0f;

        if( soundMode == 0 ) {
            if( musicSamplesLeft <= 0 ) {
                musicAdvance();
                /* 16th notes at 96 beats per minute */
                musicSamplesLeft = SAMPLE_RATE * 60 / ( 96 * 4 );
                }
            musicSamplesLeft--;
            if( musicBass.active ) {
                mix += voiceSample( &musicBass );
                }
            if( musicLead.active ) {
                mix += voiceSample( &musicLead );
                }
            }
        for( v = 0; v < MAX_VOICES; v++ ) {
            if( voices[v].active ) {
                mix += voiceSample( &( voices[v] ) );
                }
            }
        if( soundMode == 2 ) {
            mix = 0.0f;
            }
        if( mix > 1.0f ) {
            mix = 1.0f;
            }
        if( mix < -1.0f ) {
            mix = -1.0f;
            }
        out[i] = (Sint16)( mix * 30000.0f );
        sampleClock++;
        }
    }



void sndInit( void ) {
    SDL_AudioSpec want, have;

    memset( voices, 0, sizeof( voices ) );
    memset( lastPlayed, 0, sizeof( lastPlayed ) );
    memset( &want, 0, sizeof( want ) );
    want.freq = SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = audioCallback;
    device = SDL_OpenAudioDevice( 0, 0, &want, &have, 0 );
    if( device != 0 ) {
        SDL_PauseAudioDevice( device, 0 );
        }
    }



void sndSetMode( int inMode ) {
    soundMode = inMode;
    }



void sndPlay( int inSound ) {
    const SoundDef *def;
    int n, v;

    if( device == 0 || soundMode == 2 || inSound < 0 ||
        inSound >= NUM_SNDS ) {
        return;
        }
    SDL_LockAudioDevice( device );
    /* the same effect many times at once is just noise:  space it out */
    if( sampleClock - lastPlayed[ inSound ] < SAMPLE_RATE / 25 &&
        lastPlayed[ inSound ] != 0 ) {
        SDL_UnlockAudioDevice( device );
        return;
        }
    lastPlayed[ inSound ] = sampleClock + 1;
    def = &( soundDefs[ inSound ] );
    for( n = 0; n < def->numNotes; n++ ) {
        const Note *note = &( def->notes[n] );
        for( v = 0; v < MAX_VOICES; v++ ) {
            if( ! voices[v].active ) {
                startVoice( &( voices[v] ), note->wave, note->freq,
                            note->endFreq, note->seconds, note->vol,
                            note->delay );
                break;
                }
            }
        }
    SDL_UnlockAudioDevice( device );
    }
