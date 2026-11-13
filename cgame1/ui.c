/*
 * Path Master
 *
 * Everything the player sees and touches:  the path scene on top, and
 * below it the wand web, the satchel, the crucible, and the info panel.
 * Mouse only.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "game.h"


/* ---- layout ------------------------------------------------------- */

#define PANEL_Y      SCENE_H

#define WEB_CX       124
#define WEB_CY       344
#define WEB_DX       42
#define WEB_DY       36
#define SLOT_HALF    15

#define INV_X        254
#define INV_Y        228
#define INV_STEP     34

#define CRU_X        254
#define CRU_Y        378
#define CRU_W        136
#define CRU_H        44

#define INFO_X       408
#define INFO_Y       212
#define INFO_W       220

#define NUM_BUTTONS  7
#define BUTTON_X0    ( SCREEN_W - NUM_BUTTONS * 17 - 3 )
#define BUTTON_Y     4

#define CARD_W       112
#define CARD_H       152
#define CARD_Y       40

#define COL_TEXT     0xE8E4F0FFu
#define COL_DIM      0x9890A8FFu
#define COL_GOLD     0xF0D040FFu
#define COL_PANEL    0x1C1826FFu
#define COL_PANEL2   0x262234FFu
#define COL_EDGE     0x4A4860FFu


typedef struct {
        Color skyTop, skyBottom;
        Color mountain, hill, tree, grass, path;
    } Biome;


static const Biome biomes[ NUM_BIOMES ] = {
    /* meadow */
    { 0x4A90D8FFu, 0xB8E0F4FFu, 0x8098C8FFu, 0x5AA868FFu, 0x2F8A3FFFu,
      0x4CB050FFu, 0xD0B078FFu },
    /* emberwood */
    { 0xD06838FFu, 0xF4C880FFu, 0xA06858FFu, 0xB87030FFu, 0xC85020FFu,
      0xA88830FFu, 0xB89868FFu },
    /* frostfell */
    { 0x7088C0FFu, 0xD8E8F8FFu, 0xC0D0E8FFu, 0xA0B8D0FFu, 0x5888A0FFu,
      0xD0E0F0FFu, 0xA8B0C0FFu },
    /* ashlands */
    { 0x201420FFu, 0x903830FFu, 0x482838FFu, 0x582430FFu, 0x30181CFFu,
      0x603844FFu, 0x786058FFu }
    };


int  uiSpeed          = 1;
char uiWantFullscreen = 0;
char uiWantQuit       = 0;
char uiFontTest       = 0;

static int   mouseX    = -100;
static int   mouseY    = -100;
static int   dragSlot  = -1;
static int   frame     = 0;
static int   soundMode = 0;
static char  toast[80];
static float toastT    = 0.0f;
/* scene shake offset for this frame */
static int   shakeX    = 0;
static int   shakeY    = 0;



/* ---- helpers ------------------------------------------------------ */

static Color withAlpha( Color inC, int inA ) {
    if( inA < 0 ) {
        inA = 0;
        }
    if( inA > 255 ) {
        inA = 255;
        }
    return ( inC & 0xFFFFFF00u ) | (Color)inA;
    }



static Color mixColor( Color inA, Color inB, float inT ) {
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



static float pulse( float inSpeed ) {
    return 0.5f + 0.5f * (float)sin( (float)frame * inSpeed );
    }



static int posMod( int inA, int inB ) {
    int m = inA % inB;
    if( m < 0 ) {
        m += inB;
        }
    return m;
    }



static void fmtNum( char *outBuf, float inV ) {
    float r = (float)floor( inV + 0.5f );
    float d = inV - r;
    if( d < 0.0f ) {
        d = -d;
        }
    if( d < 0.05f ) {
        sprintf( outBuf, "%d", (int)r );
        }
    else {
        sprintf( outBuf, "%.1f", inV );
        }
    }



static void setToast( const char *inText ) {
    strncpy( toast, inText, sizeof( toast ) - 1 );
    toast[ sizeof( toast ) - 1 ] = '\0';
    toastT = 2.0f;
    }



void uiSlotPos( int inSlot, int *outX, int *outY ) {
    if( inSlot < NUM_SOCKETS ) {
        *outX = WEB_CX + WEB_DX * sockQ[ inSlot ]
            + ( WEB_DX / 2 ) * sockR[ inSlot ];
        *outY = WEB_CY + WEB_DY * sockR[ inSlot ];
        }
    else if( inSlot < SLOT_CRUCIBLE ) {
        int i = inSlot - SLOT_INV0;
        *outX = INV_X + INV_STEP / 2 + ( i % 4 ) * INV_STEP;
        *outY = INV_Y + INV_STEP / 2 + ( i / 4 ) * INV_STEP;
        }
    else {
        *outX = CRU_X + CRU_W / 2;
        *outY = CRU_Y + CRU_H / 2;
        }
    }



static int slotAt( int inX, int inY ) {
    int i, x, y;

    if( inX >= CRU_X && inX < CRU_X + CRU_W &&
        inY >= CRU_Y && inY < CRU_Y + CRU_H ) {
        return SLOT_CRUCIBLE;
        }
    for( i = 0; i < SLOT_CRUCIBLE; i++ ) {
        uiSlotPos( i, &x, &y );
        if( inX >= x - SLOT_HALF && inX <= x + SLOT_HALF &&
            inY >= y - SLOT_HALF && inY <= y + SLOT_HALF ) {
            return i;
            }
        }
    return -1;
    }



static int cardAt( int inX, int inY ) {
    int i;
    for( i = 0; i < 3; i++ ) {
        int x = 142 + i * 122;
        if( inX >= x && inX < x + CARD_W &&
            inY >= CARD_Y && inY < CARD_Y + CARD_H ) {
            return i;
            }
        }
    return -1;
    }



static int buttonAt( int inX, int inY ) {
    int i;
    for( i = 0; i < NUM_BUTTONS; i++ ) {
        int x = BUTTON_X0 + i * 17;
        if( inX >= x && inX < x + 15 &&
            inY >= BUTTON_Y && inY < BUTTON_Y + 15 ) {
            return i;
            }
        }
    return -1;
    }



static int againButtonHit( int inX, int inY ) {
    return inX >= 260 && inX < 380 && inY >= 128 && inY < 152;
    }



/* ---- mouse -------------------------------------------------------- */

void uiMouseMove( int inX, int inY ) {
    mouseX = inX;
    mouseY = inY;
    }



void uiMouseDown( void ) {
    int b = buttonAt( mouseX, mouseY );
    int slot;

    if( b >= 0 ) {
        sndPlay( SND_CLICK );
        switch( b ) {
            case 0: uiSpeed = 0; break;
            case 1: uiSpeed = 1; break;
            case 2: uiSpeed = 2; break;
            case 3: uiSpeed = 4; break;
            case 4:
                soundMode = ( soundMode + 1 ) % 3;
                sndSetMode( soundMode );
                break;
            case 5: uiWantFullscreen = 1; break;
            case 6: uiWantQuit = 1; break;
            }
        return;
        }

    if( g.state == STATE_TITLE ) {
        gameStartRun();
        if( uiSpeed == 0 ) {
            uiSpeed = 1;
            }
        sndPlay( SND_CARD );
        return;
        }
    if( g.state == STATE_DEAD ) {
        if( againButtonHit( mouseX, mouseY ) ) {
            gameStartRun();
            if( uiSpeed == 0 ) {
                uiSpeed = 1;
                }
            sndPlay( SND_CARD );
            }
        return;
        }
    if( g.state == STATE_CARDS ) {
        int c = cardAt( mouseX, mouseY );
        if( c >= 0 ) {
            if( ! gamePickCard( c ) ) {
                setToast( T( TXT_SATCHEL_FULL ) );
                sndPlay( SND_ERROR );
                }
            return;
            }
        }

    slot = slotAt( mouseX, mouseY );
    if( slot >= 0 && slot < SLOT_CRUCIBLE &&
        g.slots[ slot ].type != ITEM_NONE && ! g.reserved[ slot ] ) {
        dragSlot = slot;
        sndPlay( SND_CLICK );
        }
    }



void uiMouseUp( void ) {
    int target, result;
    Item moved;

    if( dragSlot < 0 ) {
        return;
        }
    target = slotAt( mouseX, mouseY );
    moved = g.slots[ dragSlot ];
    if( target >= 0 && target != dragSlot ) {
        result = gameMoveItem( dragSlot, target );
        switch( result ) {
            case MOVE_OK:
                sndPlay( SND_PLACE );
                break;
            case MOVE_MERGE: {
                int x, y;
                uiSlotPos( target, &x, &y );
                addBurst( (float)x, (float)y, 30,
                          itemLightColor( moved.type ), 110.0f );
                sndPlay( SND_MERGE );
                break;
                }
            case MOVE_UNLOCK: {
                int x, y;
                uiSlotPos( target, &x, &y );
                addBurst( (float)x, (float)y, 30, 0xFFFFFFFFu, 110.0f );
                sndPlay( SND_UNLOCK );
                break;
                }
            case MOVE_DISSOLVE: {
                int x, y;
                uiSlotPos( target, &x, &y );
                addBurst( (float)x, (float)y, 24,
                          itemLightColor( moved.type ), 90.0f );
                sndPlay( SND_DISSOLVE );
                break;
                }
            default:
                if( target < NUM_SOCKETS && ! g.unlocked[ target ] ) {
                    setToast( T( TXT_NEED_SHARD ) );
                    }
                else if( moved.type == ITEM_SHARD &&
                         target < NUM_SOCKETS ) {
                    setToast( T( TXT_CANT_PLACE ) );
                    }
                sndPlay( SND_ERROR );
                break;
            }
        }
    dragSlot = -1;
    }



/* ---- the path scene ----------------------------------------------- */

static void drawBackground( void ) {
    int stage = g.wave / WAVES_PER_STAGE;
    const Biome *b = &( biomes[ stage % NUM_BIOMES ] );
    int scroll = (int)g.scroll;
    int i, off, k;

    for( i = 0; i < 12; i++ ) {
        drawRect( 0, i * 14, SCREEN_W, 14,
                  mixColor( b->skyTop, b->skyBottom, (float)i / 11.0f ) );
        }

    for( i = 0; i < 4; i++ ) {
        int x = posMod( i * 190 + i * i * 23 - scroll / 16, SCREEN_W + 80 )
            - 80;
        drawSprite( sprCloud, x, 16 + ( i * 37 ) % 40, 2,
                    withAlpha( mixColor( b->skyBottom, COL_WHITE, 0.6f ),
                               200 ) );
        }

    off = posMod( scroll / 10, 512 );
    drawSprite( sprMountain, -off, 14, 2, b->mountain );
    drawSprite( sprMountain, 512 - off, 14, 2, b->mountain );
    drawSprite( sprMountain, 1024 - off, 14, 2, b->mountain );

    off = posMod( scroll / 4, 512 );
    drawSprite( sprHill, -off, 84, 2, b->hill );
    drawSprite( sprHill, 512 - off, 84, 2, b->hill );
    drawSprite( sprHill, 1024 - off, 84, 2, b->hill );

    /* trees stand at fixed spots in their own parallax layer */
    off = ( scroll * 6 ) / 10;
    for( k = off / 70 - 1; k <= ( off + SCREEN_W ) / 70 + 1; k++ ) {
        int h = posMod( k * 7919 + k * k * 31, 97 );
        int x = k * 70 - off + h % 30;
        if( h % 4 == 0 ) {
            continue;
            }
        drawSprite( sprTree[ ( h / 7 ) % 2 ], x, 162 - 68 + shakeY, 2,
                    ( h % 3 == 0 ) ? mixColor( b->tree, b->hill, 0.35f )
                                   : b->tree );
        }

    /* grass, then the path */
    drawRect( 0, 158, SCREEN_W, 10, b->grass );
    off = posMod( scroll, 64 );
    for( i = -1; i < SCREEN_W / 64 + 2; i++ ) {
        drawSprite( sprGrass, i * 64 - off, 153 + shakeY, 1, b->grass );
        drawSprite( sprPath, i * 64 - off, 166 + shakeY, 1, b->path );
        }
    drawRect( 0, 190 + shakeY, SCREEN_W, 12,
              mixColor( b->grass, COL_BLACK, 0.25f ) );
    for( i = -1; i < SCREEN_W / 64 + 2; i++ ) {
        drawSprite( sprGrass, i * 64 - posMod( scroll + 20, 64 ),
                    186 + shakeY, 1,
                    mixColor( b->grass, COL_BLACK, 0.25f ) );
        }
    }



static void drawHero( void ) {
    int walkFrame = g.walking ? ( (int)( g.walkAnim * 6.0f ) & 1 ) : 0;
    int x = HERO_X - 16 + shakeX;
    int y = GROUND_Y - 44 + shakeY - ( g.walking ? walkFrame : 0 );
    int tipX = x + 27;
    int tipY = y + 20;

    if( g.state == STATE_DEAD ) {
        return;
        }
    /* shadow */
    drawRect( x + 2, GROUND_Y - 1 + shakeY, 22, 2, 0x00000050u );
    drawSprite( sprHero[ walkFrame ], x, y, 2, COL_WHITE );

    gfxAdditive( 1 );
    if( g.hurtFlash > 0.0f ) {
        drawSprite( sprHero[ walkFrame ], x, y, 2, 0xFF2020A0u );
        }
    /* the wand's gem glows, and flares on a cast */
    drawSprite( sprGlow, tipX - 8, tipY - 8, 1,
                withAlpha( 0x80F0FFFFu, 90 + (int)( 60.0f * pulse( 0.1f ) ) ) );
    if( g.castAnim > 0.0f ) {
        drawSprite( sprGlow, tipX - 16, tipY - 16, 2, 0xFFFFFFC0u );
        }
    if( g.barrier > 0.0f ) {
        int a = 40 + (int)( 30.0f * pulse( 0.08f ) );
        drawSprite( sprGlow, x - 8, y - 4, 3, withAlpha( 0x70A8FFFFu, a ) );
        }
    gfxAdditive( 0 );
    }



static void drawEnemies( void ) {
    static const Color tierTint[5] = {
        0xFFFFFFFFu, 0xFFD8B0FFu, 0xC8D8FFFFu, 0xFFB0B0FFu, 0xD0FFD0FFu
        };
    int stage = g.wave / WAVES_PER_STAGE;
    int i;

    for( i = 0; i < MAX_ENEMIES; i++ ) {
        Enemy *e = &( g.enemies[i] );
        int spr, w, h, x, y;
        Color tint;

        if( ! e->alive ) {
            continue;
            }
        spr = sprEnemy[ (int)e->type ][ (int)( e->anim * 5.0f ) & 1 ];
        w = gfxSpriteW( spr ) * e->scale;
        h = gfxSpriteH( spr ) * e->scale;
        x = (int)e->x - w / 2 + shakeX - ( e->lunge > 0.0f ? 5 : 0 );
        if( e->flying ) {
            y = (int)e->y - h / 2 + shakeY;
            }
        else {
            y = GROUND_Y - h + shakeY;
            }

        tint = tierTint[ stage % 5 ];
        if( e->stunT > 0.0f ) {
            tint = 0x70E0FFFFu;
            }
        else if( e->chillT > 0.0f ) {
            tint = mixColor( tint, 0x80B0FFFFu, 0.6f );
            }
        else if( e->poisonT > 0.0f ) {
            tint = mixColor( tint, 0x90FF90FFu, 0.5f );
            }

        drawRect( (int)e->x - w / 3 + shakeX, GROUND_Y - 1 + shakeY,
                  ( w * 2 ) / 3, 2, 0x00000050u );
        drawSprite( spr, x, y, e->scale, tint );
        if( e->flash > 0.0f ) {
            gfxAdditive( 1 );
            drawSprite( spr, x, y, e->scale, 0xFFFFFFC0u );
            gfxAdditive( 0 );
            }
        if( ! e->boss && e->hp < e->maxHp ) {
            int bw = 8 * e->scale + 4;
            int bx = (int)e->x - bw / 2 + shakeX;
            int fill = (int)( (float)( bw - 2 ) * e->hp / e->maxHp );
            drawRect( bx, y - 6, bw, 4, 0x000000C0u );
            drawRect( bx + 1, y - 5, fill > 0 ? fill : 1, 2, 0xE04040FFu );
            }
        }
    }



static void drawBoltsAndEffects( void ) {
    int i;

    gfxAdditive( 1 );
    for( i = 0; i < MAX_BOLTS; i++ ) {
        Bolt *b = &( g.bolts[i] );
        if( ! b->alive ) {
            continue;
            }
        drawSprite( sprGlow, (int)b->x - 8, (int)b->drawY - 8, 1,
                    b->color );
        drawRect( (int)b->x - 1, (int)b->drawY - 1, 3, 3, 0xFFFFFFFFu );
        }
    for( i = 0; i < MAX_ARCS; i++ ) {
        Arc *a = &( g.arcs[i] );
        int seg;
        int px, py;
        unsigned int h;
        if( a->life <= 0.0f ) {
            continue;
            }
        /* jagged lightning between the two points */
        h = (unsigned int)a->seed;
        px = (int)a->x1;
        py = (int)a->y1;
        for( seg = 1; seg <= 5; seg++ ) {
            float t = (float)seg / 5.0f;
            int nx = (int)( a->x1 + ( a->x2 - a->x1 ) * t );
            int ny = (int)( a->y1 + ( a->y2 - a->y1 ) * t );
            if( seg < 5 ) {
                h = h * 1103515245u + 12345u;
                ny += (int)( ( h >> 16 ) % 15 ) - 7;
                }
            drawLine( px, py, nx, ny, 0xFFF8A0FFu );
            drawLine( px, py + 1, nx, ny + 1, 0xFFE040A0u );
            px = nx;
            py = ny;
            }
        }
    for( i = 0; i < MAX_PARTICLES; i++ ) {
        Particle *p = &( g.parts[i] );
        if( p->life > 0.0f ) {
            drawRect( (int)p->x, (int)p->y, p->size, p->size,
                      withAlpha( p->color,
                                 (int)( 255.0f * p->life / p->maxLife ) ) );
            }
        }
    gfxAdditive( 0 );

    for( i = 0; i < MAX_FLOATERS; i++ ) {
        Floater *f = &( g.floats[i] );
        if( f->life > 0.0f ) {
            int a = f->life > 0.3f ? 255 : (int)( f->life / 0.3f * 255.0f );
            drawTextCentered( (int)f->x, (int)f->y, f->text,
                              withAlpha( f->color, a ), f->big ? 2 : 1, 1 );
            }
        }
    }



static void drawBar( int inX, int inY, int inW, int inH, float inFrac,
                     Color inC ) {
    int fill;
    if( inFrac < 0.0f ) {
        inFrac = 0.0f;
        }
    if( inFrac > 1.0f ) {
        inFrac = 1.0f;
        }
    fill = (int)( (float)( inW - 2 ) * inFrac );
    drawRect( inX, inY, inW, inH, 0x000000D0u );
    drawRect( inX + 1, inY + 1, fill, inH - 2, inC );
    drawRect( inX + 1, inY + 1, fill, 1, mixColor( inC, COL_WHITE, 0.4f ) );
    }



static void drawHud( void ) {
    static const int icons[ NUM_BUTTONS ] = {
        ICON_PAUSE, ICON_PLAY1, ICON_PLAY2, ICON_PLAY3, ICON_SOUND,
        ICON_FULL, ICON_QUIT
        };
    char buf[96];
    int i;
    int hover = buttonAt( mouseX, mouseY );

    for( i = 0; i < NUM_BUTTONS; i++ ) {
        int x = BUTTON_X0 + i * 17;
        int icon = icons[i];
        int on = ( i == 0 && uiSpeed == 0 ) || ( i == 1 && uiSpeed == 1 ) ||
                 ( i == 2 && uiSpeed == 2 ) || ( i == 3 && uiSpeed == 4 );
        if( i == 4 ) {
            icon = soundMode == 0 ? ICON_SOUND
                 : ( soundMode == 1 ? ICON_MUSIC_OFF : ICON_MUTE );
            }
        drawRect( x, BUTTON_Y, 15, 15, on ? 0x504890E0u : 0x18141FC0u );
        drawFrame( x, BUTTON_Y, 15, 15,
                   hover == i ? COL_WHITE : ( on ? 0xA098E0FFu
                                                 : 0x6A6480FFu ) );
        drawSprite( sprIcon[ icon ],
                    x + ( 15 - gfxSpriteW( sprIcon[ icon ] ) ) / 2,
                    BUTTON_Y + ( 15 - gfxSpriteH( sprIcon[ icon ] ) ) / 2
                    - ( gfxSpriteH( sprIcon[ icon ] ) > 7 ? 1 : 0 ),
                    1, on || hover == i ? COL_WHITE : 0xB0A8C8FFu );
        }

    if( g.state == STATE_TITLE ) {
        return;
        }

    /* life, with the barrier laid over it */
    drawBar( 6, 5, 134, 12, g.hp / g.wand.maxHp, 0xD83848FFu );
    if( g.barrier > 0.0f ) {
        float frac = g.barrier / g.wand.maxHp;
        if( frac > 1.0f ) {
            frac = 1.0f;
            }
        drawRect( 7, 13, (int)( 132.0f * frac ), 3, 0x80B8FFFFu );
        }
    drawSprite( sprIcon[ ICON_HEART ], 9, 7, 1, COL_WHITE );
    sprintf( buf, "%d / %d", (int)( g.hp + 0.99f ), (int)g.wand.maxHp );
    drawTextCentered( 78, 8, buf, COL_WHITE, 1, 1 );

    drawBar( 6, 19, 134, 7, (float)g.xp / (float)g.xpNext, 0xE8C030FFu );
    sprintf( buf, T( TXT_LV_FMT ), g.level );
    drawTextEx( 144, 18, buf, COL_GOLD, 1, 1 );

    /* where we are on the path */
    if( g.state != STATE_CARDS ) {
        int stage = g.wave / WAVES_PER_STAGE;
        int wave = g.wave % WAVES_PER_STAGE;
        sprintf( buf, "%s %d-%d", T( TXT_BIOME0 + stage % NUM_BIOMES ),
                 stage + 1, wave + 1 );
        drawTextCentered( SCREEN_W / 2, 5, buf, COL_WHITE, 1, 1 );
        for( i = 0; i < WAVES_PER_STAGE; i++ ) {
            int px = SCREEN_W / 2 - WAVES_PER_STAGE * 5 + i * 10 + 2;
            int boss = ( i == WAVES_PER_STAGE - 1 );
            Color c = 0x00000090u;
            if( i < wave ) {
                c = 0xF0D040FFu;
                }
            else if( i == wave ) {
                c = withAlpha( boss ? 0xFF5050FFu : COL_WHITE,
                               140 + (int)( 115.0f * pulse( 0.12f ) ) );
                }
            else if( boss ) {
                c = 0x903030FFu;
                }
            if( boss ) {
                drawRect( px - 1, 16, 8, 8, c );
                }
            else {
                drawRect( px, 17, 6, 6, c );
                }
            }
        }

    /* a boss gets a big bar of its own */
    for( i = 0; i < MAX_ENEMIES; i++ ) {
        Enemy *e = &( g.enemies[i] );
        if( e->alive && e->boss && g.state != STATE_DEAD ) {
            drawTextCentered( SCREEN_W / 2, 30, T( TXT_BOSS0 + e->type ),
                              0xFF9090FFu, 1, 1 );
            drawBar( SCREEN_W / 2 - 110, 41, 220, 8, e->hp / e->maxHp,
                     0xC02838FFu );
            break;
            }
        }

    if( g.best > 0 ) {
        sprintf( buf, T( TXT_BEST ), g.best );
        drawTextEx( 6, 30, buf, 0xFFFFFFA0u, 1, 1 );
        }

    if( g.bannerT > 0.0f && g.state == STATE_PLAY ) {
        int a = g.bannerT > 0.5f ? 255 : (int)( g.bannerT * 510.0f );
        drawTextCentered( SCREEN_W / 2, 64, g.banner,
                          withAlpha( g.bannerColor, a ), 2, 1 );
        }

    if( uiSpeed == 0 && g.state == STATE_PLAY ) {
        drawTextCentered( SCREEN_W / 2, 100, T( TXT_PAUSED ),
                          withAlpha( COL_WHITE,
                                     120 + (int)( 135.0f * pulse( 0.08f ) ) ),
                          2, 1 );
        }
    }



/* ---- items and the info panel ------------------------------------- */

static void drawItem( Item inItem, int inCX, int inCY, int inScale ) {
    int spr;
    if( inItem.type == ITEM_NONE ) {
        return;
        }
    spr = sprItem[ inItem.type ];
    drawSprite( spr, inCX - ( gfxSpriteW( spr ) * inScale ) / 2,
                inCY - ( gfxSpriteH( spr ) * inScale ) / 2, inScale,
                COL_WHITE );
    if( inItem.type != ITEM_SHARD && inScale == 2 ) {
        char buf[8];
        sprintf( buf, "%d", (int)inItem.level );
        drawTextEx( inCX + 7, inCY + 5, buf, COL_WHITE, 1, 1 );
        }
    }



static void gemDescription( Item inItem, float inPower, char *outBuf ) {
    char a[16], b[16];
    float fa, fb;
    gemEffect( inItem.type, inPower, &fa, &fb );
    fmtNum( a, fa );
    fmtNum( b, fb );
    sprintf( outBuf, T( TXT_GEM_DESC0 + inItem.type - 1 ), a, b );
    }



static const char *roman( int inRank ) {
    switch( inRank ) {
        case 1: return "I";
        case 2: return "II";
        case 3: return "III";
        }
    return "";
    }



/* full details of one item.  inSocket is -1 when not in the web */
static void drawItemInfo( Item inItem, int inSocket ) {
    char buf[200];
    char num[16];
    int x = INFO_X;
    int y = INFO_Y;
    int i, tx;
    float power = (float)inItem.level;

    if( inItem.type == ITEM_SHARD ) {
        drawText( x, y, T( TXT_SHARD_NAME ), COL_WHITE );
        drawTextWrap( x, y + 16, INFO_W, T( TXT_SHARD_DESC ), COL_TEXT );
        return;
        }

    if( inSocket >= 0 ) {
        power = g.wand.power[ inSocket ];
        }
    tx = drawText( x, y, T( TXT_GEM_NAME0 + inItem.type - 1 ),
                   itemLightColor( inItem.type ) );
    sprintf( buf, T( TXT_LV_FMT ), (int)inItem.level );
    tx = drawText( tx + 8, y, buf, COL_TEXT );
    if( inSocket >= 0 && power > (float)inItem.level + 0.01f ) {
        fmtNum( num, power );
        sprintf( buf, T( TXT_POWER_FMT ), num );
        drawText( tx + 10, y, buf, COL_GOLD );
        }
    y += 15;

    gemDescription( inItem, power, buf );
    y = drawTextWrap( x, y, INFO_W, buf, COL_TEXT ) + 3;

    if( inItem.level < MAX_GEM_LEVEL ) {
        sprintf( buf, T( TXT_FUSE_HINT ), (int)inItem.level,
                 T( TXT_GEM_NAME0 + inItem.type - 1 ) );
        }
    else {
        sprintf( buf, "%s", T( TXT_MAX_LEVEL ) );
        }
    y = drawTextWrap( x, y, INFO_W, buf, COL_DIM );
    if( inItem.type != GEM_OPAL ) {
        sprintf( buf, T( TXT_RESONANCE ),
                 T( TXT_GEM_NAME0 + inItem.type - 1 ) );
        y = drawTextWrap( x, y, INFO_W, buf, COL_DIM );
        }
    y += 5;

    for( i = 0; i < NUM_SYN; i++ ) {
        int partner = 0;
        int rank = g.wand.syn[i];
        if( synPairs[i].a == inItem.type ) {
            partner = synPairs[i].b;
            }
        else if( synPairs[i].b == inItem.type ) {
            partner = synPairs[i].a;
            }
        if( partner == 0 ) {
            continue;
            }
        drawSprite( sprItem[ partner ], x, y - 2, 1, COL_WHITE );
        tx = drawText( x + 14, y, "+", COL_DIM );
        tx = drawText( tx + 2, y, T( TXT_GEM_NAME0 + partner - 1 ),
                       itemLightColor( partner ) );
        tx = drawText( tx + 2, y, ":", COL_DIM );
        tx = drawText( tx + 4, y, T( TXT_SYN_NAME0 + i ),
                       rank > 0 ? COL_GOLD : COL_TEXT );
        if( rank > 0 ) {
            drawText( tx + 4, y, roman( rank ), COL_GOLD );
            }
        y += TEXT_LINE_H + 1;
        fmtNum( num, synValue( i, rank > 0 ? rank : 1 ) );
        sprintf( buf, T( TXT_SYN_DESC0 + i ), num );
        y = drawTextWrap( x + 14, y, INFO_W - 14, buf,
                          rank > 0 ? COL_TEXT : COL_DIM ) + 3;
        }
    }



static int statRow;

static void statLine( const char *inLabel, const char *inValue ) {
    int col = statRow % 2;
    int x = INFO_X + col * 114;
    int y = INFO_Y + 16 + ( statRow / 2 ) * 12;
    drawText( x, y, inLabel, COL_DIM );
    drawText( x + 104 - textWidth( inValue ), y, inValue, COL_TEXT );
    statRow++;
    }



static void drawWandInfo( void ) {
    WandStats *w = &( g.wand );
    char buf[64];
    char num[16], num2[16];
    int i, y;
    int any = 0;
    int socketed = 0;

    for( i = 0; i < NUM_SOCKETS; i++ ) {
        if( g.slots[i].type != ITEM_NONE ) {
            socketed++;
            }
        }

    if( uiFontTest ) {
        /* shows that text beyond ASCII works, for future translations */
        drawTextWrap( INFO_X, INFO_Y, INFO_W,
                      "\xC3\x80 \xC3\x89 \xC3\x8E \xC3\x95 \xC3\x9C "
                      "\xC3\x87 \xC3\x91 \xC3\xA0 \xC3\xA9 \xC3\xAE "
                      "\xC3\xB5 \xC3\xBC \xC3\xA7 \xC3\xB1 \xC3\x9F "
                      "\xC2\xBF \xC2\xA1 \xE2\x80\x94 \xE2\x86\x92\n"
                      "\xC2\xBFQu\xC3\xA9 gema eliges, ni\xC3\xB1o?\n"
                      "Le ma\xC3\xAEtre du chemin a d\xC3\xA9j\xC3\xA0 "
                      "gagn\xC3\xA9.\n"
                      "Gr\xC3\xBC\xC3\x9F""e, Zauberer!  "
                      "F\xC3\xBCnf \xC3\x84pfel.\n"
                      "The quick brown fox jumps over the lazy dog.\n"
                      "PACK MY BOX WITH FIVE DOZEN LIQUOR JUGS\n"
                      "0123456789 !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~",
                      COL_TEXT );
        return;
        }
    if( g.state == STATE_TITLE || ( socketed == 0 && g.wave == 0 ) ) {
        drawText( INFO_X, INFO_Y, T( TXT_TITLE ), COL_GOLD );
        drawTextWrap( INFO_X, INFO_Y + 16, INFO_W, T( TXT_HOWTO ),
                      COL_TEXT );
        return;
        }

    drawText( INFO_X, INFO_Y, T( TXT_WAND ), COL_GOLD );
    statRow = 0;

    sprintf( buf, "%.1f", w->dmg );
    statLine( T( TXT_STAT_DAMAGE ), buf );
    sprintf( buf, "%.2f", 1.0f / w->interval );
    statLine( T( TXT_STAT_RATE ), buf );
    if( w->critChance > 0.0f ) {
        fmtNum( num2, w->critMult );
        sprintf( buf, "%d%% \xC3\x97%s", (int)( w->critChance * 100.0f
                                                + 0.5f ), num2 );
        statLine( T( TXT_STAT_CRIT ), buf );
        }
    if( w->extraShots > 0.0f ) {
        fmtNum( num, 1.0f + w->extraShots );
        statLine( T( TXT_STAT_BOLTS ), num );
        }
    if( w->burnDps > 0.0f ) {
        fmtNum( num, w->burnDps * w->dotMult );
        statLine( T( TXT_STAT_BURN ), num );
        }
    if( w->poisonDps > 0.0f ) {
        fmtNum( num, w->poisonDps * w->dotMult );
        statLine( T( TXT_STAT_POISON ), num );
        }
    if( w->chill > 0.0f ) {
        sprintf( buf, "%d%%", (int)( w->chill * 100.0f + 0.5f ) );
        statLine( T( TXT_STAT_CHILL ), buf );
        }
    if( w->chains > 0 ) {
        sprintf( buf, "%d", w->chains );
        statLine( T( TXT_STAT_CHAIN ), buf );
        }
    if( w->lifesteal > 0.0f ) {
        sprintf( buf, "%d%%", (int)( w->lifesteal * 100.0f + 0.5f ) );
        statLine( T( TXT_STAT_STEAL ), buf );
        }
    if( w->regen > 0.0f ) {
        fmtNum( num, w->regen );
        statLine( T( TXT_STAT_REGEN ), num );
        }
    if( w->barrier > 0.0f ) {
        fmtNum( num, w->barrier );
        statLine( T( TXT_STAT_BARRIER ), num );
        }

    y = INFO_Y + 16 + ( ( statRow + 1 ) / 2 ) * 12 + 8;
    drawText( INFO_X, y, T( TXT_SYNERGIES ), COL_GOLD );
    y += 15;
    for( i = 0; i < NUM_SYN; i++ ) {
        int tx;
        if( w->syn[i] <= 0 ) {
            continue;
            }
        any = 1;
        drawSprite( sprItem[ synPairs[i].a ], INFO_X, y - 2, 1, COL_WHITE );
        drawSprite( sprItem[ synPairs[i].b ], INFO_X + 12, y - 2, 1,
                    COL_WHITE );
        tx = drawText( INFO_X + 28, y, T( TXT_SYN_NAME0 + i ), COL_TEXT );
        drawText( tx + 4, y, roman( w->syn[i] ), COL_GOLD );
        y += 13;
        }
    if( ! any ) {
        drawTextWrap( INFO_X, y, INFO_W, T( TXT_NO_SYNERGY ), COL_DIM );
        }
    }



/* ---- the wand panel ----------------------------------------------- */

/* which synergy two gem types make, or -1 */
static int synergyOf( int inA, int inB ) {
    int i;
    for( i = 0; i < NUM_SYN; i++ ) {
        if( ( synPairs[i].a == inA && synPairs[i].b == inB ) ||
            ( synPairs[i].a == inB && synPairs[i].b == inA ) ) {
            return i;
            }
        }
    return -1;
    }



static void drawThread( int inX1, int inY1, int inX2, int inY2, Color inC,
                        int inThick ) {
    drawLine( inX1, inY1, inX2, inY2, inC );
    if( inThick ) {
        drawLine( inX1 + 1, inY1, inX2 + 1, inY2, inC );
        drawLine( inX1, inY1 + 1, inX2, inY2 + 1, inC );
        }
    }



static void drawWeb( void ) {
    int a, b;
    Item dragged;

    dragged.type = ITEM_NONE;
    dragged.level = 0;
    if( dragSlot >= 0 ) {
        dragged = g.slots[ dragSlot ];
        }

    /* the wand's handle */
    drawRect( WEB_CX - 4, WEB_CY + 2 * WEB_DY, 8, 70, 0x603820FFu );
    drawRect( WEB_CX - 4, WEB_CY + 2 * WEB_DY, 2, 70, 0xA07040FFu );
    drawRect( WEB_CX + 2, WEB_CY + 2 * WEB_DY, 2, 70, 0x402010FFu );
    drawRect( WEB_CX - 6, WEB_CY + 2 * WEB_DY + 20, 12, 4, 0xB08020FFu );
    drawRect( WEB_CX - 6, WEB_CY + 2 * WEB_DY + 20, 12, 1, 0xF0D040FFu );

    /* threads */
    for( a = 0; a < NUM_SOCKETS; a++ ) {
        for( b = a + 1; b < NUM_SOCKETS; b++ ) {
            int x1, y1, x2, y2;
            Item *ia = &( g.slots[a] );
            Item *ib = &( g.slots[b] );
            Color c = 0x343040FFu;
            int thick = 0;

            if( ! socketsAdjacent( a, b ) ) {
                continue;
                }
            uiSlotPos( a, &x1, &y1 );
            uiSlotPos( b, &x2, &y2 );
            if( g.unlocked[a] && g.unlocked[b] ) {
                c = 0x7C7C98FFu;
                if( ia->type != ITEM_NONE && ib->type != ITEM_NONE ) {
                    float p = pulse( 0.1f );
                    if( synergyOf( ia->type, ib->type ) >= 0 ) {
                        c = mixColor( itemLightColor( ia->type ),
                                      itemLightColor( ib->type ), p );
                        thick = 1;
                        }
                    else if( ia->type == ib->type &&
                             ia->type != GEM_OPAL ) {
                        c = mixColor( itemColor( ia->type ), COL_WHITE,
                                      p * 0.5f );
                        thick = 1;
                        }
                    else if( ia->type == GEM_OPAL ||
                             ib->type == GEM_OPAL ) {
                        c = mixColor( 0xB0C0D0FFu, 0xFFC0E0FFu, p );
                        thick = 1;
                        }
                    }
                }
            drawThread( x1, y1, x2, y2, c, thick );
            }
        }

    /* sockets and their gems */
    for( a = 0; a < NUM_SOCKETS; a++ ) {
        int x, y;
        Color tint = 0xC8D0E8FFu;
        uiSlotPos( a, &x, &y );

        if( ! g.unlocked[a] ) {
            int can = socketUnlockable( a );
            tint = can ? 0x7068A0FFu : 0x484458FFu;
            if( can && dragged.type == ITEM_SHARD ) {
                tint = mixColor( 0x9080C0FFu, 0xFFE880FFu, pulse( 0.2f ) );
                }
            drawSprite( sprSocket, x - 15, y - 15, 2, tint );
            drawSprite( sprIcon[ ICON_LOCK ], x - 3, y - 4, 1,
                        can ? 0xFFFFFFE0u : 0xFFFFFF60u );
            continue;
            }
        if( dragged.type != ITEM_NONE && dragged.type != ITEM_SHARD &&
            a != dragSlot ) {
            Item *it = &( g.slots[a] );
            if( it->type == ITEM_NONE ) {
                tint = mixColor( tint, 0x80FF90FFu, 0.3f + 0.4f * pulse( 0.15f ) );
                }
            }
        drawSprite( sprSocket, x - 15, y - 15, 2, tint );
        if( a != dragSlot ) {
            drawItem( g.slots[a], x, y, 2 );
            }
        }
    }



static void drawSatchel( void ) {
    int i, x, y;
    char buf[64];
    Item dragged;

    dragged.type = ITEM_NONE;
    dragged.level = 0;
    if( dragSlot >= 0 ) {
        dragged = g.slots[ dragSlot ];
        }

    for( i = SLOT_INV0; i < SLOT_CRUCIBLE; i++ ) {
        uiSlotPos( i, &x, &y );
        drawRect( x - 15, y - 15, 30, 30, 0x100E18FFu );
        drawFrame( x - 15, y - 15, 30, 30, COL_EDGE );
        if( i != dragSlot ) {
            drawItem( g.slots[i], x, y, 2 );
            }
        }

    /* the crucible */
    {
        int hot = ( dragged.type != ITEM_NONE );
        Color edge = hot ? mixColor( 0xC06020FFu, 0xFFD060FFu,
                                     pulse( 0.15f ) )
                         : 0x804828FFu;
        drawRect( CRU_X, CRU_Y, CRU_W, CRU_H, 0x1A100CFFu );
        drawFrame( CRU_X, CRU_Y, CRU_W, CRU_H, edge );
        drawFrame( CRU_X + 1, CRU_Y + 1, CRU_W - 2, CRU_H - 2,
                   withAlpha( edge, 110 ) );
        drawTextCentered( CRU_X + CRU_W / 2, CRU_Y + 9, T( TXT_CRUCIBLE ),
                          hot ? 0xFFD060FFu : 0xC08050FFu, 1, 0 );
        if( hot ) {
            sprintf( buf, T( TXT_CRUCIBLE_HINT ), dissolveXp( dragged ) );
            drawTextCentered( CRU_X + CRU_W / 2, CRU_Y + 25, buf,
                              COL_GOLD, 1, 0 );
            }
        else {
            /* embers */
            for( i = 0; i < 9; i++ ) {
                int ex = CRU_X + 18 + i * 12 + ( ( frame / 9 + i * 5 ) % 5 );
                int ey = CRU_Y + 34 - ( ( frame / 5 + i * 7 ) % 10 );
                drawRect( ex, ey, 2, 2,
                          withAlpha( 0xFF9030FFu,
                                     80 + ( ( frame + i * 40 ) % 90 ) ) );
                }
            }
        }

    /* highlight twins of a dragged gem, since they can fuse */
    if( dragged.type != ITEM_NONE && dragged.type != ITEM_SHARD &&
        dragged.level < MAX_GEM_LEVEL ) {
        for( i = 0; i < SLOT_CRUCIBLE; i++ ) {
            Item *it = &( g.slots[i] );
            if( i == dragSlot || it->type != dragged.type ||
                it->level != dragged.level ) {
                continue;
                }
            if( i < NUM_SOCKETS && ! g.unlocked[i] ) {
                continue;
                }
            uiSlotPos( i, &x, &y );
            drawFrame( x - 16, y - 16, 32, 32,
                       withAlpha( COL_WHITE,
                                  100 + (int)( 155.0f * pulse( 0.25f ) ) ) );
            drawFrame( x - 17, y - 17, 34, 34,
                       withAlpha( itemLightColor( dragged.type ),
                                  100 + (int)( 155.0f * pulse( 0.25f ) ) ) );
            }
        }
    }



static void drawPanel( void ) {
    int hover = slotAt( mouseX, mouseY );
    int card = ( g.state == STATE_CARDS ) ? cardAt( mouseX, mouseY ) : -1;
    int i;

    drawRect( 0, PANEL_Y, SCREEN_W, SCREEN_H - PANEL_Y, COL_PANEL );
    drawRect( 0, PANEL_Y, SCREEN_W, 2, 0x0A0810FFu );
    drawRect( 0, PANEL_Y + 2, SCREEN_W, 1, 0x5A5470FFu );

    drawRect( 4, 206, 240, 270, COL_PANEL2 );
    drawFrame( 4, 206, 240, 270, COL_EDGE );
    drawRect( 248, 206, 148, 270, COL_PANEL2 );
    drawFrame( 248, 206, 148, 270, COL_EDGE );
    drawRect( 400, 206, 236, 270, COL_PANEL2 );
    drawFrame( 400, 206, 236, 270, COL_EDGE );

    drawText( 10, 212, T( TXT_WAND ), COL_GOLD );
    drawText( 254, 212, T( TXT_SATCHEL ), COL_GOLD );

    drawWeb();
    drawSatchel();

    /* the info panel follows the mouse */
    if( dragSlot >= 0 ) {
        drawItemInfo( g.slots[ dragSlot ],
                      dragSlot < NUM_SOCKETS ? dragSlot : -1 );
        }
    else if( card >= 0 ) {
        Card *c = &( g.cards[ card ] );
        if( c->kind == CARD_HEART ) {
            drawText( INFO_X, INFO_Y, T( TXT_HEART_NAME ), 0xFF8090FFu );
            drawTextWrap( INFO_X, INFO_Y + 16, INFO_W, T( TXT_HEART_DESC ),
                          COL_TEXT );
            }
        else {
            drawItemInfo( c->item, -1 );
            }
        }
    else if( hover >= 0 && hover < SLOT_CRUCIBLE &&
             g.slots[ hover ].type != ITEM_NONE ) {
        drawItemInfo( g.slots[ hover ],
                      hover < NUM_SOCKETS ? hover : -1 );
        }
    else if( hover >= 0 && hover < NUM_SOCKETS &&
             g.state != STATE_TITLE ) {
        const char *t = T( TXT_EMPTY_SOCKET );
        if( ! g.unlocked[ hover ] ) {
            t = socketUnlockable( hover ) ? T( TXT_LOCKED_SOCKET )
                                          : T( TXT_LOCKED_FAR );
            }
        drawTextWrap( INFO_X, INFO_Y, INFO_W, t, COL_TEXT );
        }
    else {
        drawWandInfo();
        }

    /* loot in flight to the satchel */
    for( i = 0; i < MAX_FLYERS; i++ ) {
        Flyer *f = &( g.flyers[i] );
        if( f->alive ) {
            int tx, ty;
            float t = f->t * f->t * ( 3.0f - 2.0f * f->t );
            float x, y;
            uiSlotPos( f->slot, &tx, &ty );
            x = f->x + ( (float)tx - f->x ) * t;
            y = f->y + ( (float)ty - f->y ) * t
                - 50.0f * (float)sin( 3.14159f * f->t );
            gfxAdditive( 1 );
            drawSprite( sprGlow, (int)x - 16, (int)y - 16, 2,
                        withAlpha( itemLightColor( f->item.type ), 150 ) );
            gfxAdditive( 0 );
            drawItem( f->item, (int)x, (int)y, 2 );
            }
        }

    /* sparks that belong to the panel (fusing, unlocking) */
    gfxAdditive( 1 );
    for( i = 0; i < MAX_PARTICLES; i++ ) {
        Particle *p = &( g.parts[i] );
        if( p->life > 0.0f && p->y >= (float)PANEL_Y ) {
            drawRect( (int)p->x, (int)p->y, p->size + 1, p->size + 1,
                      withAlpha( p->color,
                                 (int)( 255.0f * p->life / p->maxLife ) ) );
            }
        }
    gfxAdditive( 0 );

    if( dragSlot >= 0 ) {
        drawItem( g.slots[ dragSlot ], mouseX, mouseY, 2 );
        }
    }



/* ---- overlays ----------------------------------------------------- */

static void drawCards( void ) {
    int i;
    int hover = cardAt( mouseX, mouseY );
    char buf[200];

    drawRect( 0, 0, SCREEN_W, SCENE_H, 0x08061090u );
    drawTextCentered( SCREEN_W / 2, 6, T( TXT_LEVEL_UP ),
                      mixColor( COL_GOLD, COL_WHITE, pulse( 0.15f ) ), 2, 1 );
    drawTextCentered( SCREEN_W / 2, 26, T( TXT_CHOOSE ), COL_TEXT, 1, 1 );

    for( i = 0; i < 3; i++ ) {
        Card *c = &( g.cards[i] );
        int x = 142 + i * 122;
        int y = CARD_Y - ( hover == i ? 2 : 0 );
        Color accent = c->kind == CARD_HEART ? 0xFF8090FFu
                                             : itemLightColor( c->item.type );
        const char *name;

        drawRect( x + 3, y + 3, CARD_W, CARD_H, 0x00000080u );
        drawRect( x, y, CARD_W, CARD_H, 0x241E34FFu );
        drawRect( x + 2, y + 2, CARD_W - 4, 56,
                  mixColor( 0x241E34FFu, accent, 0.22f ) );
        drawFrame( x, y, CARD_W, CARD_H,
                   hover == i ? COL_WHITE : mixColor( accent, COL_BLACK,
                                                      0.3f ) );
        if( hover == i ) {
            drawFrame( x - 1, y - 1, CARD_W + 2, CARD_H + 2, accent );
            }

        if( c->kind == CARD_HEART ) {
            drawSprite( sprIcon[ ICON_HEART ], x + CARD_W / 2 - 21, y + 9, 6,
                        COL_WHITE );
            name = T( TXT_HEART_NAME );
            sprintf( buf, "%s", T( TXT_HEART_DESC ) );
            }
        else if( c->item.type == ITEM_SHARD ) {
            drawItem( c->item, x + CARD_W / 2, y + 30, 4 );
            name = T( TXT_SHARD_NAME );
            sprintf( buf, "%s", T( TXT_SHARD_SHORT ) );
            }
        else {
            gfxAdditive( 1 );
            drawSprite( sprGlow, x + CARD_W / 2 - 24, y + 6, 3,
                        withAlpha( accent, 90 ) );
            gfxAdditive( 0 );
            drawItem( c->item, x + CARD_W / 2, y + 30, 4 );
            name = T( TXT_GEM_NAME0 + c->item.type - 1 );
            gemDescription( c->item, (float)c->item.level, buf );
            }
        drawTextCentered( x + CARD_W / 2, y + 62, name, accent, 1, 1 );
        if( c->kind == CARD_ITEM && c->item.type != ITEM_SHARD ) {
            char lv[24];
            sprintf( lv, T( TXT_LV_FMT ), (int)c->item.level );
            drawTextCentered( x + CARD_W / 2, y + 74, lv,
                              c->item.level > 1 ? COL_GOLD : COL_DIM, 1, 0 );
            }
        drawTextWrap( x + 7, y + 88, CARD_W - 13, buf, COL_TEXT );
        }
    }



static void drawTitle( void ) {
    char buf[64];
    int bob = (int)( 3.0f * sin( (float)frame * 0.05f ) );

    drawRect( 0, 0, SCREEN_W, SCENE_H, 0x08061060u );
    drawTextCentered( SCREEN_W / 2 + 3, 43 + bob, T( TXT_TITLE ),
                      0x3A1860FFu, 6, 0 );
    drawTextCentered( SCREEN_W / 2, 40 + bob, T( TXT_TITLE ),
                      mixColor( 0xF0D040FFu, 0xFFF8C0FFu, pulse( 0.07f ) ),
                      6, 0 );
    if( ( frame / 30 ) % 2 == 0 ) {
        drawTextCentered( SCREEN_W / 2, 112, T( TXT_CLICK_START ),
                          COL_WHITE, 2, 1 );
        }
    if( g.best > 0 ) {
        sprintf( buf, T( TXT_BEST ), g.best );
        drawTextCentered( SCREEN_W / 2, 140, buf, COL_GOLD, 1, 1 );
        }
    }



static void drawDead( void ) {
    char buf[120];
    int hot = againButtonHit( mouseX, mouseY );

    drawRect( 0, 0, SCREEN_W, SCENE_H, 0x100008A0u );
    drawTextCentered( SCREEN_W / 2, 36, T( TXT_DEAD_TITLE ), 0xFF6060FFu,
                      4, 1 );
    sprintf( buf, T( TXT_DEAD_STATS ), g.wave + 1, g.level, g.kills );
    drawTextCentered( SCREEN_W / 2, 84, buf, COL_TEXT, 1, 1 );
    if( g.newBest ) {
        drawTextCentered( SCREEN_W / 2, 100, T( TXT_NEW_BEST ),
                          mixColor( COL_GOLD, COL_WHITE, pulse( 0.2f ) ),
                          1, 1 );
        }
    else {
        sprintf( buf, T( TXT_BEST ), g.best );
        drawTextCentered( SCREEN_W / 2, 100, buf, COL_DIM, 1, 1 );
        }
    drawRect( 260, 128, 120, 24, hot ? 0x6050A0FFu : 0x3A3060FFu );
    drawFrame( 260, 128, 120, 24, hot ? COL_WHITE : 0xA098E0FFu );
    drawTextCentered( SCREEN_W / 2, 136, T( TXT_WALK_AGAIN ), COL_WHITE,
                      1, 1 );
    }



void uiDraw( void ) {
    frame++;
    shakeX = 0;
    shakeY = 0;
    if( g.shake > 0.5f ) {
        int amp = (int)g.shake;
        shakeX = ( ( frame * 7 ) % ( 2 * amp + 1 ) ) - amp;
        shakeY = ( ( frame * 13 ) % ( amp + 1 ) ) - amp / 2;
        }

    drawBackground();
    drawEnemies();
    drawHero();
    drawBoltsAndEffects();

    if( g.state == STATE_TITLE ) {
        drawTitle();
        }
    else if( g.state == STATE_CARDS ) {
        drawCards();
        }
    else if( g.state == STATE_DEAD ) {
        drawDead();
        }
    drawHud();

    if( toastT > 0.0f ) {
        int a = toastT > 0.4f ? 255 : (int)( toastT / 0.4f * 255.0f );
        int w = textWidth( toast ) + 16;
        toastT -= 1.0f / 60.0f;
        drawRect( SCREEN_W / 2 - w / 2, 178, w, 16,
                  withAlpha( 0x300810FFu, ( a * 220 ) / 255 ) );
        drawTextCentered( SCREEN_W / 2, 182, toast,
                          withAlpha( 0xFFC0C0FFu, a ), 1, 0 );
        }

    drawPanel();
    }
