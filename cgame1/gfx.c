/*
 * Path Master
 *
 * Graphics layer:  one sprite atlas texture, built in memory at startup
 * and drawn with plain OpenGL quads.  Everything is positioned on whole
 * game pixels, and the viewport is an integer multiple of 640x480, so
 * pixels stay sharp.
 *
 * Also holds the hand-made pixel font.  Text is UTF-8.
 */

#include <string.h>

#include <SDL.h>
#include <SDL_opengl.h>

#include "game.h"


#define ATLAS_SIZE   512
#define MAX_SPRITES  512


typedef struct {
        int x, y, w, h;
    } SpriteRect;


static unsigned char atlas[ ATLAS_SIZE * ATLAS_SIZE * 4 ];
static SpriteRect    sprites[ MAX_SPRITES ];
static int           numSprites  = 0;
static int           shelfX      = 0;
static int           shelfY      = 0;
static int           shelfH      = 0;
static GLuint        atlasTex    = 0;
static int           whiteSprite = -1;



int gfxNewSprite( int inW, int inH ) {
    SpriteRect *s;

    if( numSprites >= MAX_SPRITES ) {
        return 0;
        }
    if( shelfX + inW + 1 > ATLAS_SIZE ) {
        shelfX = 0;
        shelfY += shelfH + 1;
        shelfH = 0;
        }
    s = &( sprites[ numSprites ] );
    s->x = shelfX;
    s->y = shelfY;
    s->w = inW;
    s->h = inH;
    shelfX += inW + 1;
    if( inH > shelfH ) {
        shelfH = inH;
        }
    numSprites++;
    return numSprites - 1;
    }



void gfxSetPixel( int inSprite, int inX, int inY, Color inC ) {
    SpriteRect *s = &( sprites[ inSprite ] );
    unsigned char *p;

    if( inX < 0 || inY < 0 || inX >= s->w || inY >= s->h ) {
        return;
        }
    if( s->y + inY >= ATLAS_SIZE ) {
        return;
        }
    p = &( atlas[ ( ( s->y + inY ) * ATLAS_SIZE + s->x + inX ) * 4 ] );
    p[0] = (unsigned char)( ( inC >> 24 ) & 0xFF );
    p[1] = (unsigned char)( ( inC >> 16 ) & 0xFF );
    p[2] = (unsigned char)( ( inC >> 8 ) & 0xFF );
    p[3] = (unsigned char)( inC & 0xFF );
    }



Color gfxGetPixel( int inSprite, int inX, int inY ) {
    SpriteRect *s = &( sprites[ inSprite ] );
    unsigned char *p;

    if( inX < 0 || inY < 0 || inX >= s->w || inY >= s->h ) {
        return 0;
        }
    p = &( atlas[ ( ( s->y + inY ) * ATLAS_SIZE + s->x + inX ) * 4 ] );
    return ( (Color)p[0] << 24 ) | ( (Color)p[1] << 16 ) |
           ( (Color)p[2] << 8 ) | (Color)p[3];
    }



int gfxSpriteW( int inSprite ) {
    return sprites[ inSprite ].w;
    }



int gfxSpriteH( int inSprite ) {
    return sprites[ inSprite ].h;
    }



/* the shared art palette */
Color gfxArtColor( char inC ) {
    switch( inC ) {
        case 'k': return 0x1A1420FFu;
        case 'w': return 0xF4F4F4FFu;
        case 'g': return 0xB0B0C0FFu;
        case 'G': return 0x606078FFu;
        case 'r': return 0xD04040FFu;
        case 'R': return 0x802030FFu;
        case 'o': return 0xE08030FFu;
        case 'y': return 0xF0D040FFu;
        case 'Y': return 0xB08020FFu;
        case 'l': return 0x80D050FFu;
        case 'n': return 0x40A040FFu;
        case 'N': return 0x206030FFu;
        case 'b': return 0x70B0F0FFu;
        case 'B': return 0x3060C0FFu;
        case 'D': return 0x202860FFu;
        case 'p': return 0xA050D0FFu;
        case 'P': return 0x5A2A80FFu;
        case 'm': return 0xE070B0FFu;
        case 's': return 0xF0C090FFu;
        case 'S': return 0xC08860FFu;
        case 't': return 0xA07040FFu;
        case 'T': return 0x603820FFu;
        case 'c': return 0x60E0E0FFu;
        case 'e': return 0xE8E0C0FFu;
        case 'E': return 0xA8A080FFu;
        case '#': return 0xFFFFFFFFu;
        }
    return 0;
    }



/* measures art made of '/' separated rows */
static void artSize( const char *inArt, int *outW, int *outH ) {
    int w = 0;
    int h = 1;
    int rowW = 0;
    const char *c;

    for( c = inArt; *c != '\0'; c++ ) {
        if( *c == '/' ) {
            h++;
            rowW = 0;
            }
        else {
            rowW++;
            if( rowW > w ) {
                w = rowW;
                }
            }
        }
    *outW = w;
    *outH = h;
    }



static void artPaint( int inSprite, const char *inArt, int inOffX,
                      int inOffY ) {
    int x = 0;
    int y = 0;
    const char *c;

    for( c = inArt; *c != '\0'; c++ ) {
        if( *c == '/' ) {
            y++;
            x = 0;
            }
        else {
            Color col = gfxArtColor( *c );
            if( col != 0 ) {
                gfxSetPixel( inSprite, inOffX + x, inOffY + y, col );
                }
            x++;
            }
        }
    }



int gfxSpriteFromArt( const char *inArtA, const char *inArtB ) {
    int wA, hA;
    int wB = 0;
    int hB = 0;
    int spr;

    artSize( inArtA, &wA, &hA );
    if( inArtB != 0 ) {
        artSize( inArtB, &wB, &hB );
        }
    spr = gfxNewSprite( wA > wB ? wA : wB, hA + hB );
    artPaint( spr, inArtA, 0, 0 );
    if( inArtB != 0 ) {
        artPaint( spr, inArtB, 0, hA );
        }
    return spr;
    }



/* ------------------------------------------------------------------ */
/* The font.                                                           */
/*                                                                     */
/* Each glyph is drawn as rows separated by '/'.  Row 0 is the top of  */
/* a capital letter, row 6 is the baseline, rows 7 and 8 are for       */
/* descenders.  Glyph sprites have 3 spare rows on top for accents.    */
/* ------------------------------------------------------------------ */

#define GLYPH_ABOVE  3
#define GLYPH_H      12
#define MAX_GLYPHS   240


typedef struct {
        int         cp;
        const char *art;
    } GlyphArt;


typedef struct {
        int cp;
        int sprite;
        int w;
    } Glyph;


static const GlyphArt glyphArt[] = {
    { ' ',  "" },
    { '!',  "#/#/#/#/#//#" },
    { '"',  "#.#/#.#" },
    { '#',  "/.#.#./#####/.#.#./#####/.#.#." },
    { '$',  "..#../.####/#.#../.###./..#.#/####./..#.." },
    { '%',  "##..#/##.#./...#./..#../.#.../.#.##/#..##" },
    { '&',  ".##../#..#./#.#../.#.../#.#.#/#..#./.##.#" },
    { '\'', "#/#" },
    { '(',  ".#/#./#./#./#./#./.#" },
    { ')',  "#./.#/.#/.#/.#/.#/#." },
    { '*',  "/.#.#./..#../#####/..#../.#.#." },
    { '+',  "/..#../..#../#####/..#../..#.." },
    { ',',  "/////.#/.#/#." },
    { '-',  "///####" },
    { '.',  "//////#" },
    { '/',  "....#/...#./...#./..#../.#.../.#.../#...." },
    { '0',  ".###./#...#/#..##/#.#.#/##..#/#...#/.###." },
    { '1',  "..#../.##../..#../..#../..#../..#../.###." },
    { '2',  ".###./#...#/....#/...#./..#../.#.../#####" },
    { '3',  ".###./#...#/....#/..##./....#/#...#/.###." },
    { '4',  "...#./..##./.#.#./#..#./#####/...#./...#." },
    { '5',  "#####/#..../####./....#/....#/#...#/.###." },
    { '6',  "..##./.#.../#..../####./#...#/#...#/.###." },
    { '7',  "#####/....#/...#./..#../.#.../.#.../.#..." },
    { '8',  ".###./#...#/#...#/.###./#...#/#...#/.###." },
    { '9',  ".###./#...#/#...#/.####/....#/...#./.##.." },
    { ':',  "//#///#" },
    { ';',  "//.#///.#/.#/#." },
    { '<',  "...#/..#./.#../#.../.#../..#./...#" },
    { '=',  "//####//####" },
    { '>',  "#.../.#../..#./...#/..#./.#../#..." },
    { '?',  ".###./#...#/....#/...#./..#..//..#.." },
    { '@',  ".###./#...#/#.###/#.#.#/#.###/#..../.####" },
    { 'A',  ".###./#...#/#...#/#####/#...#/#...#/#...#" },
    { 'B',  "####./#...#/#...#/####./#...#/#...#/####." },
    { 'C',  ".###./#...#/#..../#..../#..../#...#/.###." },
    { 'D',  "####./#...#/#...#/#...#/#...#/#...#/####." },
    { 'E',  "#####/#..../#..../####./#..../#..../#####" },
    { 'F',  "#####/#..../#..../####./#..../#..../#...." },
    { 'G',  ".###./#...#/#..../#.###/#...#/#...#/.####" },
    { 'H',  "#...#/#...#/#...#/#####/#...#/#...#/#...#" },
    { 'I',  "###/.#./.#./.#./.#./.#./###" },
    { 'J',  "..###/...#./...#./...#./...#./#..#./.##.." },
    { 'K',  "#...#/#..#./#.#../##.../#.#../#..#./#...#" },
    { 'L',  "#..../#..../#..../#..../#..../#..../#####" },
    { 'M',  "#...#/##.##/#.#.#/#.#.#/#...#/#...#/#...#" },
    { 'N',  "#...#/##..#/#.#.#/#..##/#...#/#...#/#...#" },
    { 'O',  ".###./#...#/#...#/#...#/#...#/#...#/.###." },
    { 'P',  "####./#...#/#...#/####./#..../#..../#...." },
    { 'Q',  ".###./#...#/#...#/#...#/#.#.#/#..#./.##.#" },
    { 'R',  "####./#...#/#...#/####./#.#../#..#./#...#" },
    { 'S',  ".####/#..../#..../.###./....#/....#/####." },
    { 'T',  "#####/..#../..#../..#../..#../..#../..#.." },
    { 'U',  "#...#/#...#/#...#/#...#/#...#/#...#/.###." },
    { 'V',  "#...#/#...#/#...#/#...#/#...#/.#.#./..#.." },
    { 'W',  "#...#/#...#/#...#/#.#.#/#.#.#/#.#.#/.#.#." },
    { 'X',  "#...#/#...#/.#.#./..#../.#.#./#...#/#...#" },
    { 'Y',  "#...#/#...#/.#.#./..#../..#../..#../..#.." },
    { 'Z',  "#####/....#/...#./..#../.#.../#..../#####" },
    { '[',  "##/#./#./#./#./#./##" },
    { '\\', "#..../.#.../.#.../..#../...#./...#./....#" },
    { ']',  "##/.#/.#/.#/.#/.#/##" },
    { '^',  "..#../.#.#./#...#" },
    { '_',  "//////#####" },
    { '`',  "#./.#" },
    { 'a',  "//.###./....#/.####/#...#/.####" },
    { 'b',  "#..../#..../####./#...#/#...#/#...#/####." },
    { 'c',  "//.###/#.../#.../#.../.###" },
    { 'd',  "....#/....#/.####/#...#/#...#/#...#/.####" },
    { 'e',  "//.###./#...#/#####/#..../.###." },
    { 'f',  "..##/.#../###./.#../.#../.#../.#.." },
    { 'g',  "//.####/#...#/#...#/#...#/.####/....#/.###." },
    { 'h',  "#..../#..../####./#...#/#...#/#...#/#...#" },
    { 'i',  "#//#/#/#/#/#" },
    { 'j',  "..#//..#/..#/..#/..#/..#/..#/##." },
    { 'k',  "#.../#.../#..#/#.#./##../#.#./#..#" },
    { 'l',  "#/#/#/#/#/#/#" },
    { 'm',  "//##.#./#.#.#/#.#.#/#.#.#/#.#.#" },
    { 'n',  "//####./#...#/#...#/#...#/#...#" },
    { 'o',  "//.###./#...#/#...#/#...#/.###." },
    { 'p',  "//####./#...#/#...#/#...#/####./#..../#...." },
    { 'q',  "//.####/#...#/#...#/#...#/.####/....#/....#" },
    { 'r',  "//#.##/##../#.../#.../#..." },
    { 's',  "//.###/#.../.##./...#/###." },
    { 't',  ".#../.#../###./.#../.#../.#../..##" },
    { 'u',  "//#...#/#...#/#...#/#...#/.####" },
    { 'v',  "//#...#/#...#/.#.#./.#.#./..#.." },
    { 'w',  "//#...#/#.#.#/#.#.#/#.#.#/.#.#." },
    { 'x',  "//#...#/.#.#./..#../.#.#./#...#" },
    { 'y',  "//#...#/#...#/#...#/#...#/.####/....#/.###." },
    { 'z',  "//#####/...#./..#../.#.../#####" },
    { '{',  "..#/.#./.#./#../.#./.#./..#" },
    { '|',  "#/#/#/#/#/#/#" },
    { '}',  "#../.#./.#./..#/.#./.#./#.." },
    { '~',  "///.#.../#.#.#/...#." },
    /* a few beyond ASCII */
    { 0xA1,   "#//#/#/#/#/#" },
    { 0xBF,   "..#..//..#../.#.../#..../#...#/.###." },
    { 0xB0,   ".#./#.#/.#." },
    { 0xD7,   "///#.#/.#./#.#" },
    { 0xDF,   ".##./#..#/#..#/#.#./#..#/#..#/#.#." },
    { 0x2014, "///######" },
    { 0x2019, "#/#" },
    { 0x2022, "///##/##" },
    { 0x2026, "//////#.#.#" },
    { 0x2192, "//..#../...#./#####/...#./..#.." }
    };

#define NUM_GLYPH_ART  ( (int)( sizeof( glyphArt ) / sizeof( glyphArt[0] ) ) )


static Glyph glyphs[ MAX_GLYPHS ];
static int   numGlyphs = 0;
/* direct index for code points below 256 */
static short glyphLow[ 256 ];



static Glyph *findGlyph( int inCP ) {
    int i;

    if( inCP >= 0 && inCP < 256 ) {
        if( glyphLow[ inCP ] >= 0 ) {
            return &( glyphs[ glyphLow[ inCP ] ] );
            }
        return 0;
        }
    for( i = 0; i < numGlyphs; i++ ) {
        if( glyphs[i].cp == inCP ) {
            return &( glyphs[i] );
            }
        }
    return 0;
    }



static Glyph *addGlyph( int inCP, int inW ) {
    Glyph *gl;

    if( numGlyphs >= MAX_GLYPHS ) {
        return 0;
        }
    gl = &( glyphs[ numGlyphs ] );
    gl->cp = inCP;
    gl->w = inW;
    gl->sprite = gfxNewSprite( inW, GLYPH_H );
    if( inCP >= 0 && inCP < 256 ) {
        glyphLow[ inCP ] = (short)numGlyphs;
        }
    numGlyphs++;
    return gl;
    }



/* builds a Latin-1 accented letter from its base letter and a mark */
static void addAccented( int inCP, int inBase, char inMark ) {
    Glyph *base = findGlyph( inBase );
    Glyph *gl;
    const char *mark = "";
    int markW, markH;
    int lower = ( inBase >= 'a' && inBase <= 'z' );
    int markY;
    int w, x, y, off;

    if( base == 0 ) {
        return;
        }
    switch( inMark ) {
        case '`':  mark = "#./.#";     break;
        case '\'': mark = ".#/#.";     break;
        case '^':  mark = ".#./#.#";   break;
        case '~':  mark = ".#.#/#.#."; break;
        case '"':  mark = "#.#";       break;
        case 'o':  mark = ".#./#.#";   break;
        case ',':  mark = ".#/#.";     break;
        }
    artSize( mark, &markW, &markH );
    if( mark[0] == '\0' ) {
        markW = 0;
        markH = 0;
        }
    w = base->w > markW ? base->w : markW;
    gl = addGlyph( inCP, w );
    if( gl == 0 ) {
        return;
        }
    /* addGlyph may not move glyphs, so base is still valid */
    off = ( w - base->w ) / 2;
    for( y = 0; y < GLYPH_H; y++ ) {
        for( x = 0; x < base->w; x++ ) {
            Color c = gfxGetPixel( base->sprite, x, y );
            /* accented i and j lose their dots */
            if( lower && y < GLYPH_ABOVE + 2 ) {
                c = 0;
                }
            if( c != 0 ) {
                gfxSetPixel( gl->sprite, x + off, y, c );
                }
            }
        }
    if( inMark == ',' ) {
        markY = GLYPH_ABOVE + 7;
        }
    else if( lower ) {
        markY = GLYPH_ABOVE + 1 - markH;
        }
    else {
        markY = GLYPH_ABOVE - 1 - markH;
        }
    artPaint( gl->sprite, mark, ( w - markW ) / 2, markY );
    }



static void buildFont( void ) {
    /* Latin-1 letters 0xC0 to 0xDF as base letter plus mark */
    static const char *bases = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs";
    static const char *marks = "`'^~\"o ,`'^\"`'^\" ~`'^~\"  `'^\"'  ";
    int i;

    for( i = 0; i < 256; i++ ) {
        glyphLow[i] = -1;
        }
    for( i = 0; i < NUM_GLYPH_ART; i++ ) {
        int w, h;
        Glyph *gl;
        artSize( glyphArt[i].art, &w, &h );
        if( w == 0 ) {
            w = 3;
            }
        gl = addGlyph( glyphArt[i].cp, w );
        if( gl != 0 ) {
            artPaint( gl->sprite, glyphArt[i].art, 0, GLYPH_ABOVE );
            }
        }
    for( i = 0; i < 32; i++ ) {
        int cp = 0xC0 + i;
        if( cp == 0xD7 || cp == 0xDF ) {
            continue;
            }
        addAccented( cp, bases[i], marks[i] );
        addAccented( cp + 0x20, bases[i] + ( 'a' - 'A' ), marks[i] );
        }
    addAccented( 0xFF, 'y', '"' );
    }



/* decodes one UTF-8 code point and advances the pointer */
static int utf8Next( const char **inOutText ) {
    const unsigned char *s = (const unsigned char *)( *inOutText );
    int cp;
    int extra;

    if( s[0] < 0x80 ) {
        cp = s[0];
        extra = 0;
        }
    else if( ( s[0] & 0xE0 ) == 0xC0 ) {
        cp = s[0] & 0x1F;
        extra = 1;
        }
    else if( ( s[0] & 0xF0 ) == 0xE0 ) {
        cp = s[0] & 0x0F;
        extra = 2;
        }
    else if( ( s[0] & 0xF8 ) == 0xF0 ) {
        cp = s[0] & 0x07;
        extra = 3;
        }
    else {
        cp = '?';
        extra = 0;
        }
    s++;
    while( extra > 0 && ( s[0] & 0xC0 ) == 0x80 ) {
        cp = ( cp << 6 ) | ( s[0] & 0x3F );
        s++;
        extra--;
        }
    *inOutText = (const char *)s;
    return cp;
    }



static Glyph *glyphFor( int inCP ) {
    Glyph *gl = findGlyph( inCP );
    if( gl == 0 ) {
        gl = findGlyph( '?' );
        }
    return gl;
    }



/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

void gfxInit( void ) {
    int x, y;

    memset( atlas, 0, sizeof( atlas ) );
    whiteSprite = gfxNewSprite( 4, 4 );
    for( y = 0; y < 4; y++ ) {
        for( x = 0; x < 4; x++ ) {
            gfxSetPixel( whiteSprite, x, y, COL_WHITE );
            }
        }
    buildFont();
    }



void gfxUpload( void ) {
    glGenTextures( 1, &atlasTex );
    glBindTexture( GL_TEXTURE_2D, atlasTex );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP );
    glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
    glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, ATLAS_SIZE, ATLAS_SIZE, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, atlas );
    }



void gfxFrameBegin( void ) {
    glMatrixMode( GL_PROJECTION );
    glLoadIdentity();
    glOrtho( 0, SCREEN_W, SCREEN_H, 0, -1, 1 );
    glMatrixMode( GL_MODELVIEW );
    glLoadIdentity();
    glDisable( GL_DEPTH_TEST );
    glEnable( GL_TEXTURE_2D );
    glBindTexture( GL_TEXTURE_2D, atlasTex );
    glEnable( GL_BLEND );
    glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
    glBegin( GL_QUADS );
    }



void gfxFrameEnd( void ) {
    glEnd();
    }



void gfxAdditive( int inOn ) {
    glEnd();
    if( inOn ) {
        glBlendFunc( GL_SRC_ALPHA, GL_ONE );
        }
    else {
        glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
        }
    glBegin( GL_QUADS );
    }



static void setColor( Color inC ) {
    glColor4ub( (GLubyte)( ( inC >> 24 ) & 0xFF ),
                (GLubyte)( ( inC >> 16 ) & 0xFF ),
                (GLubyte)( ( inC >> 8 ) & 0xFF ),
                (GLubyte)( inC & 0xFF ) );
    }



void drawSprite( int inSprite, int inX, int inY, int inScale,
                 Color inTint ) {
    SpriteRect *s = &( sprites[ inSprite ] );
    float u0 = (float)s->x / ATLAS_SIZE;
    float v0 = (float)s->y / ATLAS_SIZE;
    float u1 = (float)( s->x + s->w ) / ATLAS_SIZE;
    float v1 = (float)( s->y + s->h ) / ATLAS_SIZE;
    int w = s->w * inScale;
    int h = s->h * inScale;

    setColor( inTint );
    glTexCoord2f( u0, v0 );
    glVertex2i( inX, inY );
    glTexCoord2f( u1, v0 );
    glVertex2i( inX + w, inY );
    glTexCoord2f( u1, v1 );
    glVertex2i( inX + w, inY + h );
    glTexCoord2f( u0, v1 );
    glVertex2i( inX, inY + h );
    }



void drawRect( int inX, int inY, int inW, int inH, Color inC ) {
    SpriteRect *s = &( sprites[ whiteSprite ] );
    float u = ( (float)s->x + 2.0f ) / ATLAS_SIZE;
    float v = ( (float)s->y + 2.0f ) / ATLAS_SIZE;

    setColor( inC );
    glTexCoord2f( u, v );
    glVertex2i( inX, inY );
    glVertex2i( inX + inW, inY );
    glVertex2i( inX + inW, inY + inH );
    glVertex2i( inX, inY + inH );
    }



void drawFrame( int inX, int inY, int inW, int inH, Color inC ) {
    drawRect( inX, inY, inW, 1, inC );
    drawRect( inX, inY + inH - 1, inW, 1, inC );
    drawRect( inX, inY + 1, 1, inH - 2, inC );
    drawRect( inX + inW - 1, inY + 1, 1, inH - 2, inC );
    }



/* a chunky Bresenham line, one game pixel at a time */
void drawLine( int inX1, int inY1, int inX2, int inY2, Color inC ) {
    int dx = inX2 > inX1 ? inX2 - inX1 : inX1 - inX2;
    int dy = inY2 > inY1 ? inY2 - inY1 : inY1 - inY2;
    int sx = inX1 < inX2 ? 1 : -1;
    int sy = inY1 < inY2 ? 1 : -1;
    int err = dx - dy;
    int x = inX1;
    int y = inY1;
    int guard = 0;

    while( guard < 2000 ) {
        int e2 = 2 * err;
        drawRect( x, y, 1, 1, inC );
        if( x == inX2 && y == inY2 ) {
            break;
            }
        if( e2 > -dy ) {
            err -= dy;
            x += sx;
            }
        if( e2 < dx ) {
            err += dx;
            y += sy;
            }
        guard++;
        }
    }



static int drawRun( int inX, int inY, const char *inText, int inLen,
                    Color inC, int inScale, int inDraw ) {
    const char *s = inText;
    const char *end = inText + inLen;
    int x = inX;

    while( s < end && *s != '\0' ) {
        int cp = utf8Next( &s );
        Glyph *gl = glyphFor( cp );
        if( gl == 0 ) {
            continue;
            }
        if( inDraw ) {
            drawSprite( gl->sprite, x, inY - GLYPH_ABOVE * inScale,
                        inScale, inC );
            }
        x += ( gl->w + 1 ) * inScale;
        }
    return x;
    }



int textWidth( const char *inText ) {
    int w = drawRun( 0, 0, inText, (int)strlen( inText ), 0, 1, 0 );
    return w > 0 ? w - 1 : 0;
    }



int drawTextEx( int inX, int inY, const char *inText, Color inC,
                int inScale, int inShadow ) {
    int len = (int)strlen( inText );
    if( inShadow ) {
        drawRun( inX + inScale, inY + inScale, inText, len,
                 0x00000000u | ( ( inC & 0xFF ) * 3 / 4 ), inScale, 1 );
        }
    return drawRun( inX, inY, inText, len, inC, inScale, 1 );
    }



int drawText( int inX, int inY, const char *inText, Color inC ) {
    return drawTextEx( inX, inY, inText, inC, 1, 0 );
    }



void drawTextCentered( int inCX, int inY, const char *inText, Color inC,
                       int inScale, int inShadow ) {
    int w = textWidth( inText ) * inScale;
    drawTextEx( inCX - w / 2, inY, inText, inC, inScale, inShadow );
    }



int drawTextWrap( int inX, int inY, int inW, const char *inText,
                  Color inC ) {
    const char *s = inText;
    int x = inX;
    int y = inY;
    int spaceW = 4;

    while( *s != '\0' ) {
        const char *wordEnd;
        int w;

        if( *s == '\n' ) {
            x = inX;
            y += TEXT_LINE_H;
            s++;
            continue;
            }
        if( *s == ' ' ) {
            x += spaceW;
            s++;
            continue;
            }
        wordEnd = s;
        while( *wordEnd != '\0' && *wordEnd != ' ' && *wordEnd != '\n' ) {
            wordEnd++;
            }
        w = drawRun( 0, 0, s, (int)( wordEnd - s ), 0, 1, 0 );
        if( x + w - 1 > inX + inW && x > inX ) {
            x = inX;
            y += TEXT_LINE_H;
            }
        drawRun( x, y, s, (int)( wordEnd - s ), inC, 1, 1 );
        x += w;
        s = wordEnd;
        }
    return y + TEXT_LINE_H;
    }
