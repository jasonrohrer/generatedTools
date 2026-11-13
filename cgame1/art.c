/*
 * Path Master
 *
 * All pixel art.  Characters are drawn as text art using the shared
 * palette in gfx.c.  Gems are shape masks that get shaded by code.
 * Scenery is generated procedurally in shades of white so that it can
 * be tinted per biome.
 */

#include <math.h>

#include "game.h"


int sprHero[2];
int sprEnemy[ NUM_ENEMY_TYPES ][2];
int sprItem[16];
int sprIcon[ NUM_ICONS ];
int sprMountain, sprHill, sprTree[2], sprCloud;
int sprGrass, sprPath, sprGlow, sprSocket;


#define PI 3.14159265f



static const char *heroBody =
    "......kk......../"
    ".....kppk......./"
    "....kpppk......./"
    "....kppPPk....../"
    "...kpppPPk....../"
    "...kyyyyYk....../"
    "..kpppppPPk...../"
    "kkpppppppPPkk.../"
    ".kkkkkkkkkkk.kk./"
    "..kssskssk..kcck/"
    "..ksssssssk.kcck/"
    "..kwwwwwsk...kk./"
    "..kwwwwwwk...tk./"
    ".kpwwwwwppkkstk./"
    ".kppwwwpppppstk./"
    ".kpppwppPPkk.tk./"
    ".kpppppppPk..tk./"
    ".kpppppppPk..kk./"
    "kppppppppPPk..../"
    "kpppppppPPPk....";

static const char *heroFeetA =
    ".kkTTkkkTTk...../"
    "..kkkk.kkkk.....";

static const char *heroFeetB =
    ".kkkTTkTTkk...../"
    "...kkkkkkk......";



static const char *enemyArt[ NUM_ENEMY_TYPES ][2] = {
    /* slime */
    {
    "/////"
    ".....kkkkkk...../"
    "...kkllllllkk.../"
    "..kllwllllnnnk../"
    ".kllwlllllnnnnk./"
    ".klllllllllnnnk./"
    "kllkwlllkwllnnnk/"
    "kllkklllkkllnnnk/"
    "klllllllllllnnnk/"
    "kllllllllllnnnNk/"
    "knnnnnnnnnnnnNNk/"
    ".kkkkkkkkkkkkkk.",
    "///////"
    "....kkkkkkkk..../"
    "..kkllllllllkk../"
    ".kllwlllllnnnnk./"
    "kllkwlllkwllnnnk/"
    "kllkklllkkllnnnk/"
    "klllllllllllnnnk/"
    "kllllllllllnnnNk/"
    "knnnnnnnnnnnnNNk/"
    ".kkkkkkkkkkkkkk."
    },
    /* bat */
    {
    "/"
    "k..............k/"
    "kk............kk/"
    "kPk..........kPk/"
    "kPPk..k..k..kPPk/"
    "kPPPkkPkkPkkPPPk/"
    "kPpPPPPPPPPPPpPk/"
    ".kPpPPrPPrPPpPk./"
    ".kPPpPPPPPPpPPk./"
    "..kkkPPwwPPkkk../"
    ".....kPPPPk...../"
    "......kkkk......///",
    "////"
    "......k..k....../"
    "..kkkkPkkPkkkk../"
    ".kPPPPPPPPPPPPk./"
    "kPpPPPrPPrPPPpPk/"
    "kPpkPPPPPPPPkpPk/"
    "kPk.kPPwwPPk.kPk/"
    "kk...kPPPPk...kk/"
    "k.....kkkk.....k///"
    },
    /* spider */
    {
    "//////"
    "..k..k....k..k../"
    ".k.k.kkkkkk.k.k./"
    ".k..kGGGGGGk..k./"
    "k..kGrGrGGGGk..k/"
    "k.kkGGGGGGGGkk.k/"
    "..k.kGGGGGGk.k../"
    ".k...kkkkkk...k./"
    ".k..k......k..k./"
    "k...k......k...k",
    "//////"
    ".k...k....k...k./"
    ".k.k.kkkkkk.k.k./"
    "k...kGGGGGGk...k/"
    "k..kGrGrGGGGk..k/"
    "..kkGGGGGGGGkk../"
    ".k..kGGGGGGk..k./"
    ".k...kkkkkk...k./"
    "k...k......k...k/"
    "...k........k..."
    },
    /* skeleton */
    {
    ".....kkkkkk...../"
    "....keeeeeek..../"
    "....kkekkeek..../"
    "....keeeeeek..../"
    ".....kekekk...../"
    ".g....kek......./"
    ".g..kkeeekk...../"
    ".g.keEeeeEek..../"
    ".gkek.keek.kek../"
    "kkkk.keEeEk.kek./"
    ".t...keeeek..kk./"
    "......kEEk....../"
    ".....keekek...../"
    ".....kek.kek..../"
    ".....kek.kek..../"
    "....kkek.kekk...",
    ".....kkkkkk...../"
    "....keeeeeek..../"
    "....kkekkeek..../"
    "....keeeeeek..../"
    ".....kekekk...../"
    "g.....kek......./"
    "g...kkeeekk...../"
    "g..keEeeeEek..../"
    "g.kek.keek.kek../"
    "kkk..keEeEk.kek./"
    "t....keeeek..kk./"
    "......kEEk....../"
    "......kekek...../"
    ".....kek.kek..../"
    "....kek...kek.../"
    "...kkek...kekk.."
    },
    /* imp */
    {
    "...k........k.../"
    "..krk......krk../"
    "..krrkkkkkkrrk../"
    "...krrrrrrrrk.../"
    "..krrrrrrrrrrk../"
    "..krywkrrywkrk../"
    "..krrkkrrkkrrk../"
    "..krrrrrrrrrrk../"
    "...krrwwwwrrk.../"
    "....krrrrrrk..../"
    "..kkkRRRRRRkkk../"
    ".krkkRRRRRRkkrk./"
    ".kk.kRRRRRRk.kk./"
    "....kRRkkRRk..../"
    "....krk..krk..../"
    "...kkkk..kkkk...",
    "...k........k.../"
    "..krk......krk../"
    "..krrkkkkkkrrk../"
    "...krrrrrrrrk.../"
    "..krrrrrrrrrrk../"
    "..krywkrrywkrk../"
    "..krrkkrrkkrrk../"
    "..krrrrrrrrrrk../"
    "...krrwwwwrrk.../"
    ".kk.krrrrrrk.kk./"
    ".krkkRRRRRRkkrk./"
    "..kkkRRRRRRkkk../"
    "....kRRRRRRk..../"
    "....kRRkkRRk..../"
    "...krk....krk.../"
    "..kkkk....kkkk.."
    },
    /* golem */
    {
    "....kkkkkkkk..../"
    "...kggggggggk.../"
    "...kgckggckGk.../"
    "...kgggggggGk.../"
    ".kkkkgGGGGgkkkk./"
    "kgggkkkkkkkkgggk/"
    "kggGkggggggkgGGk/"
    "kgGGkgggggGkgGGk/"
    "kgGGkggggGGkgGGk/"
    "kgGGkgggGGGkgGGk/"
    ".kkkkgGGGGGkkkk./"
    "...kggGkkgGGk.../"
    "...kggGk.kgGk.../"
    "...kgGGk.kgGk.../"
    "..kkgGGk.kgGGk../"
    "..kkkkkk.kkkkk..",
    "/"
    "....kkkkkkkk..../"
    "...kggggggggk.../"
    "...kgckggckGk.../"
    "...kgggggggGk.../"
    ".kkkkgGGGGgkkkk./"
    "kgggkkkkkkkkgggk/"
    "kggGkggggggkgGGk/"
    "kgGGkgggggGkgGGk/"
    "kgGGkggggGGkgGGk/"
    ".kkkkgGGGGGkkkk./"
    "...kggGkkgGGk.../"
    "...kggGk.kgGk.../"
    "..kkgGGk.kgGGk../"
    "..kkkkkk.kkkkk.."
    }
    };



/* 11 x 11 gem silhouettes, one distinct shape per gem type */
static const char *gemMask[ NUM_GEM_TYPES ] = {
    /* ruby:  cut stone */
    "..#######../.#########./###########/###########/###########/"
    ".#########./..#######../...#####.../....###..../.....#.....",
    /* sapphire:  teardrop */
    ".....#...../....###..../....###..../...#####.../..#######../"
    ".#########./.#########./.#########./.#########./..#######../"
    "...#####...",
    /* topaz:  rhombus */
    ".....#...../....###..../...#####.../..#######../.#########./"
    "###########/.#########./..#######../...#####.../....###..../"
    ".....#.....",
    /* emerald:  step cut */
    "..#######../.#########./.#########./.#########./.#########./"
    ".#########./.#########./.#########./.#########./.#########./"
    "..#######..",
    /* amethyst:  point down */
    "###########/###########/.#########./.#########./..#######../"
    "..#######../...#####.../...#####.../....###..../....###..../"
    ".....#.....",
    /* diamond:  prism, point up */
    ".....#...../....###..../....###..../...#####.../...#####.../"
    "..#######../..#######../.#########./.#########./###########/"
    "###########",
    /* onyx:  square */
    "/.#########./.#########./.#########./.#########./.#########./"
    ".#########./.#########./.#########./.#########.",
    /* quartz:  tall crystal */
    "....##...../...####..../...####..../..######.../..######.../"
    "..######.../..######.../..######.../..######.../...####..../"
    "....##.....",
    /* garnet:  heart */
    "/.###...###./#####.#####/###########/###########/###########/"
    ".#########./..#######../...#####.../....###..../.....#.....",
    /* opal:  round */
    "...#####.../.#########./.#########./###########/###########/"
    "###########/###########/###########/.#########./.#########./"
    "...#####...",
    /* citrine:  sun */
    ".....#...../.#...#...#./..#.###.#../...#####.../..#######../"
    "###########/..#######../...#####.../..#.###.#../.#...#...#./"
    ".....#.....",
    /* moonstone:  crescent */
    "...#####.../..#######../.#####...../#####....../####......./"
    "####......./####......./#####....../.#####...../..#######../"
    "...#####..."
    };


/* dark, middle, light for each item type */
static const Color gemRamp[ NUM_ITEM_TYPES ][3] = {
    { 0, 0, 0 },
    { 0x801020FFu, 0xE02848FFu, 0xFF8090FFu },
    { 0x103080FFu, 0x2868E8FFu, 0x80B8FFFFu },
    { 0x806000FFu, 0xE8B800FFu, 0xFFF080FFu },
    { 0x0A5020FFu, 0x20B850FFu, 0x90F8A0FFu },
    { 0x401070FFu, 0x9848D8FFu, 0xD8A8FFFFu },
    { 0x507088FFu, 0xB0E0F0FFu, 0xFFFFFFFFu },
    { 0x101018FFu, 0x40405AFFu, 0x8888A8FFu },
    { 0x804060FFu, 0xF090C0FFu, 0xFFD0E8FFu },
    { 0x501010FFu, 0xA02828FFu, 0xE06858FFu },
    { 0x607080FFu, 0xD0E0E0FFu, 0xFFFFFFFFu },
    { 0x904000FFu, 0xF08020FFu, 0xFFD070FFu },
    { 0x405080FFu, 0xA0B0E0FFu, 0xE0F0FFFFu },
    { 0x606078FFu, 0xD8D8E8FFu, 0xFFFFFFFFu }
    };


static const char *shardArt =
    "......kk.../"
    ".....kwwk../"
    "....kwwgk../"
    "...kwwwgk../"
    "..kwwwggkk./"
    ".kwwwgwwwgk/"
    ".kwggkgwggk/"
    "kwwgk.kwgk./"
    "kwgk..kgk../"
    "kgk....k.../"
    "kk.........";


static const char *iconArt[ NUM_ICONS ] = {
    /* pause */
    "/.##.##./.##.##./.##.##./.##.##./.##.##./.##.##.",
    /* play 1 */
    "/..#..../..##.../..###../..####./..###../..##.../..#....",
    /* play 2 */
    "/#...#.../##..##../###.###./#######./###.###./##..##../#...#...",
    /* play 3 */
    "/#..#..#../##.##.##./#########/##.##.##./#..#..#..",
    /* sound on */
    "/...#..#./..##...#/####.#.#/####.#.#/####.#.#/..##...#/...#..#.",
    /* music off */
    "/...#..../..##..../####.##./####..../####.##./..##..../...#....",
    /* mute */
    "/...#..../..##.#.#/####..#./####.#.#/####..../..##..../...#....",
    /* fullscreen */
    "###.###/#.....#/#.....#/......./#.....#/#.....#/###.###",
    /* quit */
    "/#.....#/.#...#./..#.#../...#.../..#.#../.#...#./#.....#",
    /* heart */
    ".kk.kk./krrkrrk/krwrrrk/krrrrrk/.krrrk./..krk../...k...",
    /* lock */
    "..ggg../.g...g./.g...g./YYYYYYY/YyyyyyY/YyykyyY/YyyyyyY/YYYYYYY"
    };



static Color lerpColor( Color inA, Color inB, float inT ) {
    Color out = 0;
    int shift;
    for( shift = 0; shift <= 24; shift += 8 ) {
        float a = (float)( ( inA >> shift ) & 0xFF );
        float b = (float)( ( inB >> shift ) & 0xFF );
        int v = (int)( a + ( b - a ) * inT + 0.5f );
        out |= ( (Color)v & 0xFF ) << shift;
        }
    return out;
    }



static Color gray( int inV, int inA ) {
    return ( (Color)inV << 24 ) | ( (Color)inV << 16 ) |
           ( (Color)inV << 8 ) | (Color)inA;
    }



static int maskAt( const char *inMask, int inX, int inY ) {
    int x = 0;
    int y = 0;
    const char *c;

    if( inX < 0 || inY < 0 ) {
        return 0;
        }
    for( c = inMask; *c != '\0'; c++ ) {
        if( *c == '/' ) {
            y++;
            x = 0;
            }
        else {
            if( x == inX && y == inY ) {
                return *c == '#';
                }
            x++;
            }
        }
    return 0;
    }



/* turns a silhouette into a shaded, faceted gem */
static int makeGem( int inType ) {
    const char *mask = gemMask[ inType - 1 ];
    const Color *ramp = gemRamp[ inType ];
    int spr = gfxNewSprite( 11, 11 );
    int x, y;

    for( y = 0; y < 11; y++ ) {
        for( x = 0; x < 11; x++ ) {
            Color c;
            int edge;
            float t;

            if( ! maskAt( mask, x, y ) ) {
                continue;
                }
            edge = ! maskAt( mask, x - 1, y ) || ! maskAt( mask, x + 1, y )
                || ! maskAt( mask, x, y - 1 ) || ! maskAt( mask, x, y + 1 );
            if( edge ) {
                /* rim is lit from the upper left */
                if( ! maskAt( mask, x + 1, y ) ||
                    ! maskAt( mask, x, y + 1 ) ) {
                    c = lerpColor( ramp[0], 0x000000FFu, 0.35f );
                    }
                else {
                    c = ramp[0];
                    }
                }
            else {
                t = (float)( x + y ) / 18.0f;
                /* facet bands */
                if( ( ( x + 2 * y ) / 5 ) % 2 == 0 ) {
                    t -= 0.18f;
                    }
                if( t < 0.33f ) {
                    c = ramp[2];
                    }
                else if( t < 0.72f ) {
                    c = ramp[1];
                    }
                else {
                    c = lerpColor( ramp[1], ramp[0], 0.6f );
                    }
                /* glint */
                if( ! maskAt( mask, x - 2, y ) &&
                    ! maskAt( mask, x, y - 2 ) ) {
                    c = 0xFFFFFFFFu;
                    }
                }
            if( inType == GEM_OPAL && ! edge ) {
                /* opal fire */
                int h = ( x * 7 + y * 13 ) % 11;
                if( h == 0 ) {
                    c = 0xFF90C0FFu;
                    }
                else if( h == 4 ) {
                    c = 0x70E0D0FFu;
                    }
                else if( h == 7 ) {
                    c = 0xB0A0FFFFu;
                    }
                }
            gfxSetPixel( spr, x, y, c );
            }
        }
    return spr;
    }



Color itemColor( int inType ) {
    if( inType <= 0 || inType >= NUM_ITEM_TYPES ) {
        return COL_WHITE;
        }
    if( inType == GEM_ONYX ) {
        return 0x8080A8FFu;
        }
    return gemRamp[ inType ][1];
    }



Color itemLightColor( int inType ) {
    if( inType <= 0 || inType >= NUM_ITEM_TYPES ) {
        return COL_WHITE;
        }
    if( inType == GEM_ONYX ) {
        return 0xB0B0D0FFu;
        }
    return gemRamp[ inType ][2];
    }



static void fillCircle( int inSprite, float inCX, float inCY, float inR,
                        Color inC ) {
    int x, y;
    int w = gfxSpriteW( inSprite );
    int h = gfxSpriteH( inSprite );

    for( y = 0; y < h; y++ ) {
        for( x = 0; x < w; x++ ) {
            float dx = (float)x + 0.5f - inCX;
            float dy = (float)y + 0.5f - inCY;
            if( dx * dx + dy * dy <= inR * inR ) {
                gfxSetPixel( inSprite, x, y, inC );
                }
            }
        }
    }



static void makeScenery( void ) {
    int x, y, i;

    /* mountains:  jagged, tileable, lit from the left */
    sprMountain = gfxNewSprite( 256, 64 );
    for( x = 0; x < 256; x++ ) {
        float fx = (float)x;
        float h0 = 12.0f
            + 30.0f * (float)fabs( sin( PI * 3.0f * fx / 256.0f + 0.7f ) )
            + 12.0f * (float)fabs( sin( PI * 7.0f * fx / 256.0f + 2.0f ) )
            + 3.0f * (float)sin( PI * 34.0f * fx / 256.0f );
        float h1 = 12.0f
            + 30.0f * (float)fabs( sin( PI * 3.0f * ( fx + 1 ) / 256.0f
                                        + 0.7f ) )
            + 12.0f * (float)fabs( sin( PI * 7.0f * ( fx + 1 ) / 256.0f
                                        + 2.0f ) )
            + 3.0f * (float)sin( PI * 34.0f * ( fx + 1 ) / 256.0f );
        int h = (int)h0;
        for( y = 64 - h; y < 64; y++ ) {
            int depth = y - ( 64 - h );
            int v = h1 < h0 ? 0xB0 : 0xE0;
            if( depth < 5 && h > 34 ) {
                v = 0xFF;
                }
            else if( depth > 16 ) {
                v = 0xD0;
                }
            gfxSetPixel( sprMountain, x, y, gray( v, 255 ) );
            }
        }

    /* rolling hills */
    sprHill = gfxNewSprite( 256, 40 );
    for( x = 0; x < 256; x++ ) {
        float fx = (float)x;
        int h = (int)( 20.0f
            + 10.0f * (float)sin( PI * 4.0f * fx / 256.0f )
            + 6.0f * (float)sin( PI * 10.0f * fx / 256.0f + 1.0f ) );
        for( y = 40 - h; y < 40; y++ ) {
            int depth = y - ( 40 - h );
            gfxSetPixel( sprHill, x, y,
                         gray( depth < 2 ? 0xFF : 0xD8, 255 ) );
            }
        }

    /* a round tree */
    sprTree[0] = gfxNewSprite( 22, 34 );
    for( y = 18; y < 34; y++ ) {
        gfxSetPixel( sprTree[0], 10, y, gray( 0x50, 255 ) );
        gfxSetPixel( sprTree[0], 11, y, gray( 0x40, 255 ) );
        }
    fillCircle( sprTree[0], 11.0f, 12.0f, 10.5f, gray( 0x90, 255 ) );
    fillCircle( sprTree[0], 10.0f, 11.0f, 9.0f, gray( 0xC0, 255 ) );
    fillCircle( sprTree[0], 8.0f, 8.0f, 5.0f, gray( 0xF0, 255 ) );
    fillCircle( sprTree[0], 14.0f, 14.0f, 2.5f, gray( 0xA0, 255 ) );

    /* a pine */
    sprTree[1] = gfxNewSprite( 22, 34 );
    for( y = 26; y < 34; y++ ) {
        gfxSetPixel( sprTree[1], 10, y, gray( 0x50, 255 ) );
        gfxSetPixel( sprTree[1], 11, y, gray( 0x40, 255 ) );
        }
    for( i = 0; i < 3; i++ ) {
        int top = i * 8;
        for( y = 0; y < 13; y++ ) {
            int half = 1 + ( y * ( 4 + i * 2 ) ) / 12;
            for( x = 11 - half; x < 11 + half; x++ ) {
                int v = x < 10 ? 0xE8 : 0xB0;
                if( y > 10 ) {
                    v = 0x88;
                    }
                gfxSetPixel( sprTree[1], x, top + y, gray( v, 255 ) );
                }
            }
        }

    /* a cloud */
    sprCloud = gfxNewSprite( 40, 14 );
    fillCircle( sprCloud, 10.0f, 9.0f, 5.0f, gray( 0xFF, 200 ) );
    fillCircle( sprCloud, 19.0f, 6.5f, 6.5f, gray( 0xFF, 200 ) );
    fillCircle( sprCloud, 29.0f, 9.0f, 5.0f, gray( 0xFF, 200 ) );
    for( y = 9; y < 14; y++ ) {
        for( x = 6; x < 34; x++ ) {
            gfxSetPixel( sprCloud, x, y, gray( y > 11 ? 0xD8 : 0xFF, 200 ) );
            }
        }

    /* grass fringe, tileable */
    sprGrass = gfxNewSprite( 64, 6 );
    for( x = 0; x < 64; x++ ) {
        int blade = ( x * 37 + ( x / 3 ) * 11 ) % 5;
        int top = blade < 2 ? 0 : ( blade < 4 ? 1 : 2 );
        for( y = top; y < 6; y++ ) {
            gfxSetPixel( sprGrass, x, y,
                         gray( y < top + 2 ? 0xFF : 0xC8, 255 ) );
            }
        }

    /* the dirt path, tileable, with pebbles */
    sprPath = gfxNewSprite( 64, 24 );
    for( y = 0; y < 24; y++ ) {
        for( x = 0; x < 64; x++ ) {
            int h = ( x * 73 + y * 151 + ( x * y ) * 7 ) % 97;
            int v = 0xE0 - y * 2;
            if( h < 4 ) {
                v -= 0x30;
                }
            else if( h > 93 ) {
                v += 0x1F;
                }
            gfxSetPixel( sprPath, x, y, gray( v, 255 ) );
            }
        }

    /* soft round glow for bolts and magic */
    sprGlow = gfxNewSprite( 16, 16 );
    for( y = 0; y < 16; y++ ) {
        for( x = 0; x < 16; x++ ) {
            float dx = (float)x - 7.5f;
            float dy = (float)y - 7.5f;
            float d = (float)sqrt( dx * dx + dy * dy ) / 8.0f;
            if( d < 1.0f ) {
                /* banded, to stay looking like pixel art */
                int band = (int)( ( 1.0f - d ) * 4.0f );
                gfxSetPixel( sprGlow, x, y, gray( 255, 40 + band * 55 ) );
                }
            }
        }

    /* a round socket for the wand web */
    sprSocket = gfxNewSprite( 15, 15 );
    for( y = 0; y < 15; y++ ) {
        for( x = 0; x < 15; x++ ) {
            float dx = (float)x - 7.0f;
            float dy = (float)y - 7.0f;
            float d = (float)sqrt( dx * dx + dy * dy );
            if( d <= 7.4f ) {
                int v;
                if( d > 6.3f ) {
                    v = 0x38;
                    }
                else if( d > 4.8f ) {
                    v = ( dx + dy < 0 ) ? 0xFF : 0xA0;
                    }
                else {
                    v = 0x30 + (int)( d * 4.0f );
                    }
                gfxSetPixel( sprSocket, x, y, gray( v, 255 ) );
                }
            }
        }
    }



void artInit( void ) {
    int i;

    sprHero[0] = gfxSpriteFromArt( heroBody, heroFeetA );
    sprHero[1] = gfxSpriteFromArt( heroBody, heroFeetB );

    for( i = 0; i < NUM_ENEMY_TYPES; i++ ) {
        sprEnemy[i][0] = gfxSpriteFromArt( enemyArt[i][0], 0 );
        sprEnemy[i][1] = gfxSpriteFromArt( enemyArt[i][1], 0 );
        }

    sprItem[0] = 0;
    for( i = 1; i <= NUM_GEM_TYPES; i++ ) {
        sprItem[i] = makeGem( i );
        }
    sprItem[ ITEM_SHARD ] = gfxSpriteFromArt( shardArt, 0 );

    for( i = 0; i < NUM_ICONS; i++ ) {
        sprIcon[i] = gfxSpriteFromArt( iconArt[i], 0 );
        }

    makeScenery();
    }
