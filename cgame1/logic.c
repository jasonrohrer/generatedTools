/*
 * Path Master
 *
 * The simulation:  the walk, the fights, the wand web and its
 * synergies, loot, leveling.  Knows nothing about video, so it can
 * also be run headless for balance testing (see --sim in main.c).
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "game.h"


Game g;


/* ---- tuning ------------------------------------------------------- */

#define BASE_HP             100.0f
#define BASE_DMG            10.0f
#define BASE_INTERVAL       0.8f
#define WALK_SPEED          46.0f
#define WALK_BETWEEN_WAVES  150.0f
#define FIRE_RANGE_X        420.0f
#define STOP_RANGE_X        270.0f
#define BOLT_SPEED          430.0f
#define BURN_TIME           3.0f
#define CHILL_TIME          2.5f
#define POISON_TIME         4.0f
#define POISON_STACKS       6.0f
#define BARRIER_PERIOD      6.0f
#define DOT_PERIOD          0.5f
#define CHAIN_RANGE         120.0f
#define PLASMA_RANGE        80.0f
#define DROP_CHANCE         0.10f
#define DROP_PITY           0.02f
#define ENRAGE_AFTER        40.0f
#define BOSS_HP             170.0f
#define BOSS_DMG            11.0f
#define STUN_IMMUNE         2.0f

#define SAVE_FILE           "pathMasterBest.txt"


typedef struct {
        float hp;
        float speed;
        float dmg;
        float interval;
        int   xp;
        char  ranged;
        char  flying;
        int   firstWave;
    } EnemyDef;


static const EnemyDef enemyDefs[ NUM_ENEMY_TYPES ] = {
    /* slime */    {  22.0f, 26.0f,  6.0f, 1.0f,  3, 0, 0,  0 },
    /* bat */      {  13.0f, 62.0f,  4.0f, 0.8f,  3, 0, 1,  2 },
    /* spider */   {  10.0f, 78.0f,  3.0f, 0.6f,  2, 0, 0,  4 },
    /* skeleton */ {  42.0f, 30.0f, 10.0f, 1.2f,  6, 0, 0,  6 },
    /* imp */      {  24.0f, 34.0f,  7.0f, 1.9f,  6, 1, 0, 10 },
    /* golem */    { 130.0f, 17.0f, 22.0f, 1.8f, 14, 0, 0, 14 }
    };


const SynPair synPairs[ NUM_SYN ] = {
    { GEM_RUBY,      GEM_SAPPHIRE  },   /* thermal shock */
    { GEM_RUBY,      GEM_TOPAZ     },   /* plasma burst */
    { GEM_RUBY,      GEM_EMERALD   },   /* wildfire */
    { GEM_SAPPHIRE,  GEM_AMETHYST  },   /* shatter */
    { GEM_SAPPHIRE,  GEM_TOPAZ     },   /* superconductor */
    { GEM_TOPAZ,     GEM_QUARTZ    },   /* overclock */
    { GEM_EMERALD,   GEM_ONYX      },   /* leeching venom */
    { GEM_AMETHYST,  GEM_ONYX      },   /* executioner */
    { GEM_DIAMOND,   GEM_AMETHYST  },   /* kaleidoscope */
    { GEM_DIAMOND,   GEM_QUARTZ    },   /* barrage */
    { GEM_GARNET,    GEM_MOONSTONE },   /* bulwark */
    { GEM_CITRINE,   GEM_DIAMOND   },   /* radiance */
    { GEM_GARNET,    GEM_ONYX      },   /* blood pact */
    { GEM_MOONSTONE, GEM_SAPPHIRE  },   /* frost armor */
    { GEM_CITRINE,   GEM_QUARTZ    }    /* zenith */
    };


/* the web is a hexagon of sockets, 2 rings around a center,
   in axial hex coordinates */
const signed char sockQ[ NUM_SOCKETS ] = {
    0,
    1, 0, -1, -1, 0, 1,
    2, 1, 0, -1, -2, -2, -2, -1, 0, 1, 2, 2
    };
const signed char sockR[ NUM_SOCKETS ] = {
    0,
    0, 1, 1, 0, -1, -1,
    0, 1, 2, 2, 2, 1, 0, -1, -2, -2, -2, -1
    };



/* ---- random ------------------------------------------------------- */

static unsigned int rngState = 2463534242u;


void gameSeed( unsigned int inSeed ) {
    rngState = inSeed * 2654435761u + 88172645u;
    if( rngState == 0 ) {
        rngState = 2463534242u;
        }
    rnd();
    rnd();
    }



unsigned int rnd( void ) {
    rngState ^= rngState << 13;
    rngState &= 0xFFFFFFFFu;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    rngState &= 0xFFFFFFFFu;
    return rngState;
    }



float frand( void ) {
    return (float)( rnd() & 0xFFFFFF ) / 16777216.0f;
    }



static int irand( int inN ) {
    if( inN <= 0 ) {
        return 0;
        }
    return (int)( rnd() % (unsigned int)inN );
    }



/* ---- visual effects ----------------------------------------------- */

void addParticle( float inX, float inY, float inVX, float inVY,
                  float inLife, Color inC, int inSize, float inGravity ) {
    Particle *p = &( g.parts[ g.nextPart ] );
    g.nextPart = ( g.nextPart + 1 ) % MAX_PARTICLES;
    p->x = inX;
    p->y = inY;
    p->vx = inVX;
    p->vy = inVY;
    p->life = inLife;
    p->maxLife = inLife;
    p->color = inC;
    p->size = inSize;
    p->gravity = inGravity;
    }



void addBurst( float inX, float inY, int inN, Color inC, float inSpeed ) {
    int i;
    for( i = 0; i < inN; i++ ) {
        float a = frand() * 6.2832f;
        float s = inSpeed * ( 0.3f + 0.7f * frand() );
        addParticle( inX, inY, (float)cos( a ) * s, (float)sin( a ) * s,
                     0.25f + 0.4f * frand(), inC, 1 + irand( 2 ), 120.0f );
        }
    }



void addFloater( float inX, float inY, const char *inText, Color inC,
                 int inBig ) {
    Floater *f = &( g.floats[ g.nextFloat ] );
    g.nextFloat = ( g.nextFloat + 1 ) % MAX_FLOATERS;
    f->x = inX + (float)( irand( 9 ) - 4 );
    f->y = inY;
    f->life = inBig ? 1.1f : 0.8f;
    f->big = inBig;
    f->color = inC;
    strncpy( f->text, inText, sizeof( f->text ) - 1 );
    f->text[ sizeof( f->text ) - 1 ] = '\0';
    }



static void addArc( float inX1, float inY1, float inX2, float inY2 ) {
    Arc *a = &( g.arcs[ g.nextArc ] );
    g.nextArc = ( g.nextArc + 1 ) % MAX_ARCS;
    a->x1 = inX1;
    a->y1 = inY1;
    a->x2 = inX2;
    a->y2 = inY2;
    a->life = 0.18f;
    a->seed = (int)( rnd() & 0xFFFF );
    }



static void setBanner( const char *inText, Color inC ) {
    strncpy( g.banner, inText, sizeof( g.banner ) - 1 );
    g.banner[ sizeof( g.banner ) - 1 ] = '\0';
    g.bannerT = 3.0f;
    g.bannerColor = inC;
    }



/* ---- the wand web ------------------------------------------------- */

int socketsAdjacent( int inA, int inB ) {
    int dq = sockQ[ inA ] - sockQ[ inB ];
    int dr = sockR[ inA ] - sockR[ inB ];
    int ds = dq + dr;
    if( dq < 0 ) {
        dq = -dq;
        }
    if( dr < 0 ) {
        dr = -dr;
        }
    if( ds < 0 ) {
        ds = -ds;
        }
    return ( dq + dr + ds ) == 2;
    }



int socketUnlockable( int inSocket ) {
    int i;
    if( inSocket < 0 || inSocket >= NUM_SOCKETS ||
        g.unlocked[ inSocket ] ) {
        return 0;
        }
    for( i = 0; i < NUM_SOCKETS; i++ ) {
        if( g.unlocked[i] && socketsAdjacent( i, inSocket ) ) {
            return 1;
            }
        }
    return 0;
    }



int lockedSocketCount( void ) {
    int i;
    int n = 0;
    for( i = 0; i < NUM_SOCKETS; i++ ) {
        if( ! g.unlocked[i] ) {
            n++;
            }
        }
    return n;
    }



void gemEffect( int inType, float inPower, float *outA, float *outB ) {
    float p = inPower;
    float a = 0.0f;
    float b = 0.0f;

    switch( inType ) {
        case GEM_RUBY:
            a = 4.0f * p;
            b = 1.5f * p;
            break;
        case GEM_SAPPHIRE:
            a = 100.0f * ( 1.0f - (float)pow( 0.8, p ) );
            if( a > 75.0f ) {
                a = 75.0f;
                }
            b = 1.5f * p;
            break;
        case GEM_TOPAZ:
            a = (float)(int)( ( p + 1.0f ) / 2.0f );
            b = 45.0f + 8.0f * p;
            if( b > 100.0f ) {
                b = 100.0f;
                }
            break;
        case GEM_EMERALD:
            a = 1.5f * p;
            break;
        case GEM_AMETHYST:
            a = 100.0f * ( 1.0f - (float)pow( 0.9, p ) );
            break;
        case GEM_DIAMOND:
            a = 0.5f * p;
            break;
        case GEM_ONYX:
            a = 3.0f * p;
            if( a > 30.0f ) {
                a = 30.0f;
                }
            break;
        case GEM_QUARTZ:
            a = 12.0f * p;
            break;
        case GEM_GARNET:
            a = 20.0f * p;
            b = 0.4f * p;
            break;
        case GEM_OPAL:
            a = 0.5f * p;
            break;
        case GEM_CITRINE:
            a = 15.0f * p;
            break;
        case GEM_MOONSTONE:
            a = 8.0f * p;
            break;
        }
    *outA = a;
    *outB = b;
    }



float synValue( int inSyn, int inRank ) {
    float r = (float)inRank;
    switch( inSyn ) {
        case SYN_THERMAL:    return 25.0f * r;
        case SYN_PLASMA:     return 25.0f * r;
        case SYN_WILDFIRE:   return 30.0f * r;
        case SYN_SHATTER:    return 0.5f * r;
        case SYN_SUPERCOND:  return r;
        case SYN_OVERCLOCK:  return 15.0f * r;
        case SYN_LEECH:      return 8.0f * r;
        case SYN_EXECUTE:    return 8.0f * r;
        case SYN_KALEIDO:    return 50.0f * r;
        case SYN_BARRAGE:    return r;
        case SYN_BULWARK:    return 50.0f * r;
        case SYN_RADIANCE:   return 10.0f * r;
        case SYN_BLOODPACT:  return 8.0f * r;
        case SYN_FROSTARMOR: return 50.0f * r;
        case SYN_ZENITH:     return 10.0f * r;
        }
    return 0.0f;
    }



/* recomputes every wand stat from the gems in the web */
void gameRecalc( void ) {
    WandStats *w = &( g.wand );
    float typePower[ NUM_ITEM_TYPES ];
    float a, b;
    float dmgFlat = 0.0f;
    float dmgMult = 1.0f;
    float speedMult = 1.0f;
    int s, t, i;

    memset( w, 0, sizeof( WandStats ) );
    for( i = 0; i < NUM_ITEM_TYPES; i++ ) {
        typePower[i] = 0.0f;
        }

    /* power of each socketed gem:  its level, plus resonance with
       touching gems of its own kind, plus touching opals */
    for( s = 0; s < NUM_SOCKETS; s++ ) {
        Item *it = &( g.slots[s] );
        float p;
        if( ! g.unlocked[s] || it->type == ITEM_NONE ) {
            continue;
            }
        p = (float)it->level;
        for( t = 0; t < NUM_SOCKETS; t++ ) {
            Item *o = &( g.slots[t] );
            if( t == s || ! g.unlocked[t] || o->type == ITEM_NONE ||
                ! socketsAdjacent( s, t ) ) {
                continue;
                }
            if( o->type == it->type && it->type != GEM_OPAL ) {
                p += 0.25f * (float)it->level;
                }
            if( o->type == GEM_OPAL && it->type != GEM_OPAL ) {
                p += 0.5f * (float)o->level;
                }
            /* count synergy pairs once */
            if( t > s ) {
                for( i = 0; i < NUM_SYN; i++ ) {
                    if( ( synPairs[i].a == it->type &&
                          synPairs[i].b == o->type ) ||
                        ( synPairs[i].b == it->type &&
                          synPairs[i].a == o->type ) ) {
                        if( w->syn[i] < MAX_SYN_RANK ) {
                            w->syn[i]++;
                            }
                        }
                    }
                }
            }
        w->power[s] = p;
        typePower[ it->type ] += p;
        }

    w->critMult = 2.0f;
    w->extraFrac = 0.6f;
    w->dmgTakenMult = 1.0f;
    w->dotMult = 1.0f;
    w->maxHp = BASE_HP + (float)g.bonusHp;

    if( typePower[ GEM_RUBY ] > 0 ) {
        gemEffect( GEM_RUBY, typePower[ GEM_RUBY ], &a, &b );
        w->burnDps = a;
        dmgFlat += b;
        }
    if( typePower[ GEM_SAPPHIRE ] > 0 ) {
        gemEffect( GEM_SAPPHIRE, typePower[ GEM_SAPPHIRE ], &a, &b );
        w->chill = a / 100.0f;
        dmgFlat += b;
        }
    if( typePower[ GEM_TOPAZ ] > 0 ) {
        gemEffect( GEM_TOPAZ, typePower[ GEM_TOPAZ ], &a, &b );
        w->chains = (int)a;
        w->chainFrac = b / 100.0f;
        }
    if( typePower[ GEM_EMERALD ] > 0 ) {
        gemEffect( GEM_EMERALD, typePower[ GEM_EMERALD ], &a, &b );
        w->poisonDps = a;
        }
    if( typePower[ GEM_AMETHYST ] > 0 ) {
        gemEffect( GEM_AMETHYST, typePower[ GEM_AMETHYST ], &a, &b );
        w->critChance = a / 100.0f;
        }
    if( typePower[ GEM_DIAMOND ] > 0 ) {
        gemEffect( GEM_DIAMOND, typePower[ GEM_DIAMOND ], &a, &b );
        w->extraShots = a;
        }
    if( typePower[ GEM_ONYX ] > 0 ) {
        gemEffect( GEM_ONYX, typePower[ GEM_ONYX ], &a, &b );
        w->lifesteal = a / 100.0f;
        }
    if( typePower[ GEM_QUARTZ ] > 0 ) {
        gemEffect( GEM_QUARTZ, typePower[ GEM_QUARTZ ], &a, &b );
        speedMult += a / 100.0f;
        }
    if( typePower[ GEM_GARNET ] > 0 ) {
        gemEffect( GEM_GARNET, typePower[ GEM_GARNET ], &a, &b );
        w->maxHp += a;
        w->regen = b;
        }
    if( typePower[ GEM_CITRINE ] > 0 ) {
        gemEffect( GEM_CITRINE, typePower[ GEM_CITRINE ], &a, &b );
        dmgMult += a / 100.0f;
        }
    if( typePower[ GEM_MOONSTONE ] > 0 ) {
        gemEffect( GEM_MOONSTONE, typePower[ GEM_MOONSTONE ], &a, &b );
        w->barrier = a;
        }

    /* synergies */
    w->thermal = synValue( SYN_THERMAL, w->syn[ SYN_THERMAL ] ) / 100.0f;
    w->plasma = synValue( SYN_PLASMA, w->syn[ SYN_PLASMA ] ) / 100.0f;
    w->dotMult += synValue( SYN_WILDFIRE, w->syn[ SYN_WILDFIRE ] )
        / 100.0f;
    w->shatter = synValue( SYN_SHATTER, w->syn[ SYN_SHATTER ] );
    w->chains += w->syn[ SYN_SUPERCOND ];
    speedMult += synValue( SYN_OVERCLOCK, w->syn[ SYN_OVERCLOCK ] )
        / 100.0f;
    w->leech = synValue( SYN_LEECH, w->syn[ SYN_LEECH ] ) / 100.0f;
    w->execute = synValue( SYN_EXECUTE, w->syn[ SYN_EXECUTE ] ) / 100.0f;
    w->critMult += synValue( SYN_KALEIDO, w->syn[ SYN_KALEIDO ] ) / 100.0f;
    w->barrage = w->syn[ SYN_BARRAGE ];
    if( w->syn[ SYN_BULWARK ] > 0 ) {
        w->barrier *= 1.0f
            + synValue( SYN_BULWARK, w->syn[ SYN_BULWARK ] ) / 100.0f;
        w->dmgTakenMult -= 0.08f * (float)w->syn[ SYN_BULWARK ];
        }
    if( w->syn[ SYN_RADIANCE ] > 0 ) {
        w->extraFrac = 1.0f;
        dmgMult += synValue( SYN_RADIANCE, w->syn[ SYN_RADIANCE ] )
            / 100.0f;
        }
    dmgMult += synValue( SYN_BLOODPACT, w->syn[ SYN_BLOODPACT ] ) / 100.0f
        * ( w->maxHp / 100.0f );
    w->frostArmor = synValue( SYN_FROSTARMOR, w->syn[ SYN_FROSTARMOR ] )
        / 100.0f;
    dmgMult += synValue( SYN_ZENITH, w->syn[ SYN_ZENITH ] ) / 100.0f;
    speedMult += synValue( SYN_ZENITH, w->syn[ SYN_ZENITH ] ) / 100.0f;

    w->dmg = ( BASE_DMG + dmgFlat ) * dmgMult;
    w->interval = BASE_INTERVAL / speedMult;

    if( g.hp > w->maxHp ) {
        g.hp = w->maxHp;
        }
    if( g.barrier > w->barrier ) {
        g.barrier = w->barrier;
        }
    }



/* ---- experience and loot ------------------------------------------ */


static void healHero( float inAmount ) {
    g.hp += inAmount;
    if( g.hp > g.wand.maxHp ) {
        g.hp = g.wand.maxHp;
        }
    }


static int xpForLevel( int inLevel ) {
    return 8 + 4 * inLevel + ( inLevel * inLevel ) / 2;
    }



static void addXp( int inXp ) {
    g.xp += inXp;
    while( g.xp >= g.xpNext ) {
        g.xp -= g.xpNext;
        g.level++;
        g.xpNext = xpForLevel( g.level );
        g.pendingCards++;
        healHero( g.wand.maxHp * 0.10f );
        }
    }



int dissolveXp( Item inItem ) {
    int base = 4 + g.wave / 2;
    int i;
    if( inItem.type == ITEM_NONE ) {
        return 0;
        }
    if( inItem.type == ITEM_SHARD ) {
        return base * 3;
        }
    for( i = 1; i < inItem.level; i++ ) {
        base *= 2;
        }
    return base;
    }



static int randomGemType( void ) {
    /* often favor a kind that the player already owns,
       so that fusing stays within reach */
    if( frand() < 0.4f ) {
        int tries;
        for( tries = 0; tries < 8; tries++ ) {
            Item *it = &( g.slots[ irand( SLOT_CRUCIBLE ) ] );
            if( it->type >= 1 && it->type <= NUM_GEM_TYPES ) {
                return it->type;
                }
            }
        }
    return 1 + irand( NUM_GEM_TYPES );
    }



static int randomGemLevel( void ) {
    int stage = g.wave / WAVES_PER_STAGE;
    float p2 = 0.04f + 0.07f * (float)stage;
    float p3 = stage >= 2 ? 0.03f * (float)( stage - 1 ) : 0.0f;
    float r = frand();

    if( p2 > 0.45f ) {
        p2 = 0.45f;
        }
    if( p3 > 0.15f ) {
        p3 = 0.15f;
        }
    if( r < p3 ) {
        return 3;
        }
    if( r < p3 + p2 ) {
        return 2;
        }
    return 1;
    }



static int freeInvSlot( void ) {
    int i;
    for( i = SLOT_INV0; i < SLOT_CRUCIBLE; i++ ) {
        if( g.slots[i].type == ITEM_NONE && ! g.reserved[i] ) {
            return i;
            }
        }
    return -1;
    }



/* an item pops out in the scene and flies to the satchel */
static void dropItem( Item inItem, float inX, float inY ) {
    int slot = freeInvSlot();
    int i;

    if( slot < 0 ) {
        char buf[32];
        int xp = dissolveXp( inItem );
        sprintf( buf, T( TXT_SATCHEL_FULL_XP ), xp );
        addFloater( inX, inY - 10.0f, buf, 0xF0D040FFu, 0 );
        addXp( xp );
        return;
        }
    for( i = 0; i < MAX_FLYERS; i++ ) {
        Flyer *f = &( g.flyers[i] );
        if( ! f->alive ) {
            f->alive = 1;
            f->item = inItem;
            f->x = inX;
            f->y = inY;
            f->t = 0.0f;
            f->slot = slot;
            g.reserved[ slot ] = 1;
            return;
            }
        }
    /* no flyer free:  just put it there */
    g.slots[ slot ] = inItem;
    }



/* ---- combat ------------------------------------------------------- */

static int plasmaDepth = 0;

static void damageEnemy( Enemy *inE, float inDmg, int inCrit, Color inC );



static void killEnemy( Enemy *inE ) {
    int stage = g.wave / WAVES_PER_STAGE;
    Item it;
    Color blood = 0xFFFFFFFFu;

    inE->alive = 0;
    g.kills++;
    addXp( inE->xp );

    switch( inE->type ) {
        case ENEMY_SLIME:    blood = 0x80D050FFu; break;
        case ENEMY_BAT:      blood = 0xA050D0FFu; break;
        case ENEMY_SPIDER:   blood = 0x8080A0FFu; break;
        case ENEMY_SKELETON: blood = 0xE8E0C0FFu; break;
        case ENEMY_IMP:      blood = 0xD04040FFu; break;
        case ENEMY_GOLEM:    blood = 0xB0B0C0FFu; break;
        }
    addBurst( inE->x, inE->y, inE->boss ? 60 : 12, blood,
              inE->boss ? 160.0f : 90.0f );
    sndPlay( inE->boss ? SND_BOSS_DIE : SND_KILL );

    if( inE->boss ) {
        g.shake = 10.0f;
        it.type = ITEM_SHARD;
        it.level = 1;
        if( lockedSocketCount() > 0 ) {
            dropItem( it, inE->x, inE->y );
            }
        it.type = (unsigned char)randomGemType();
        it.level = (unsigned char)( 2 + ( stage >= 3 ? 1 : 0 ) );
        dropItem( it, inE->x + 10.0f, inE->y );
        healHero( g.wand.maxHp * 0.3f );
        }
    else {
        float chance = DROP_CHANCE + DROP_PITY * (float)g.dropPity;
        if( frand() < chance ) {
            g.dropPity = 0;
            if( lockedSocketCount() > 0 && frand() < 0.06f ) {
                it.type = ITEM_SHARD;
                it.level = 1;
                }
            else {
                it.type = (unsigned char)randomGemType();
                it.level = (unsigned char)randomGemLevel();
                }
            dropItem( it, inE->x, inE->y );
            }
        else {
            g.dropPity++;
            }
        }

    /* plasma burst:  burning foes explode */
    if( g.wand.plasma > 0.0f && inE->burnT > 0.0f && ! inE->boss &&
        plasmaDepth < 3 ) {
        int i;
        float boom = inE->maxHp * g.wand.plasma;
        plasmaDepth++;
        addBurst( inE->x, inE->y, 24, 0xFFA030FFu, 170.0f );
        sndPlay( SND_EXPLODE );
        for( i = 0; i < MAX_ENEMIES; i++ ) {
            Enemy *o = &( g.enemies[i] );
            float dx = o->x - inE->x;
            if( o->alive && dx > -PLASMA_RANGE && dx < PLASMA_RANGE ) {
                damageEnemy( o, boom, 0, 0xFFA030FFu );
                }
            }
        plasmaDepth--;
        }
    }



static void damageEnemy( Enemy *inE, float inDmg, int inCrit, Color inC ) {
    char buf[20];
    float limit;

    if( ! inE->alive ) {
        return;
        }
    inE->hp -= inDmg;
    inE->flash = 0.12f;
    if( inDmg >= 0.95f ) {
        sprintf( buf, inCrit ? "%d!" : "%d", (int)( inDmg + 0.5f ) );
        addFloater( inE->x, inE->y - 8.0f * (float)inE->scale, buf,
                    inCrit ? 0xFFF060FFu : inC, inCrit );
        }
    limit = inE->maxHp * g.wand.execute * ( inE->boss ? 0.5f : 1.0f );
    if( inE->hp > 0.0f && inE->hp < limit ) {
        inE->hp = 0.0f;
        addBurst( inE->x, inE->y, 10, 0x9848D8FFu, 120.0f );
        }
    if( inE->hp <= 0.0f ) {
        killEnemy( inE );
        }
    }



static Enemy *nearestEnemy( float inX, float inY, float inMaxDist,
                            int inSkipChain ) {
    Enemy *best = 0;
    float bestD = inMaxDist * inMaxDist;
    int i;

    for( i = 0; i < MAX_ENEMIES; i++ ) {
        Enemy *e = &( g.enemies[i] );
        float dx, dy, d;
        if( ! e->alive ) {
            continue;
            }
        if( inSkipChain && e->chainId == g.chainCounter ) {
            continue;
            }
        dx = e->x - inX;
        dy = e->y - inY;
        d = dx * dx + dy * dy;
        if( d < bestD ) {
            bestD = d;
            best = e;
            }
        }
    return best;
    }



/* a bolt (or an arc of one) lands */
static void hitEnemy( Enemy *inE, float inBaseDmg, int inChainsLeft,
                      Color inC ) {
    WandStats *w = &( g.wand );
    float dmg = inBaseDmg;
    float ex = inE->x;
    float ey = inE->y;
    int crit = 0;

    if( w->thermal > 0.0f &&
        ( inE->burnT > 0.0f || inE->chillT > 0.0f ) ) {
        dmg *= 1.0f + w->thermal;
        }
    if( frand() < w->critChance ) {
        crit = 1;
        dmg *= w->critMult;
        }
    if( w->burnDps > 0.0f ) {
        if( w->burnDps > inE->burnDps ) {
            inE->burnDps = w->burnDps;
            }
        inE->burnT = BURN_TIME;
        }
    if( w->chill > 0.0f ) {
        if( w->chill > inE->chill ) {
            inE->chill = w->chill;
            }
        inE->chillT = CHILL_TIME;
        /* a foe that just thawed can not be frozen again right away */
        if( crit && w->shatter > 0.0f && inE->stunT <= -STUN_IMMUNE ) {
            inE->stunT = w->shatter * ( inE->boss ? 0.5f : 1.0f );
            addBurst( ex, ey, 8, 0xB0E8FFFFu, 80.0f );
            }
        }
    if( w->poisonDps > 0.0f ) {
        inE->poisonDps += w->poisonDps;
        if( inE->poisonDps > w->poisonDps * POISON_STACKS ) {
            inE->poisonDps = w->poisonDps * POISON_STACKS;
            }
        inE->poisonT = POISON_TIME;
        }
    if( w->lifesteal > 0.0f ) {
        healHero( ( dmg < inE->hp ? dmg : inE->hp ) * w->lifesteal );
        }
    inE->chainId = g.chainCounter;
    sndPlay( crit ? SND_CRIT : SND_HIT );
    addBurst( ex, ey, crit ? 8 : 4, inC, 70.0f );
    damageEnemy( inE, dmg, crit, inC );

    if( inChainsLeft > 0 ) {
        Enemy *next = nearestEnemy( ex, ey, CHAIN_RANGE, 1 );
        if( next != 0 ) {
            addArc( ex, ey, next->x, next->y );
            sndPlay( SND_ZAP );
            hitEnemy( next, inBaseDmg * w->chainFrac, inChainsLeft - 1,
                      0xFFF080FFu );
            }
        }
    }



static void hurtHero( float inDmg, Enemy *inSource ) {
    WandStats *w = &( g.wand );
    char buf[20];
    float dmg = inDmg * w->dmgTakenMult;

    if( g.barrier > 0.0f ) {
        if( g.barrier >= dmg ) {
            g.barrier -= dmg;
            dmg = 0.0f;
            }
        else {
            dmg -= g.barrier;
            g.barrier = 0.0f;
            }
        addBurst( (float)HERO_X + 4.0f, (float)GROUND_Y - 22.0f, 5,
                  0xA0C8FFFFu, 60.0f );
        }
    if( dmg > 0.0f ) {
        g.hp -= dmg;
        g.hurtFlash = 0.2f;
        if( g.shake < 3.0f ) {
            g.shake = 3.0f;
            }
        sprintf( buf, "-%d", (int)( dmg + 0.5f ) );
        addFloater( (float)HERO_X, (float)GROUND_Y - 50.0f, buf,
                    0xFF5050FFu, 0 );
        sndPlay( SND_HURT );
        }
    if( inSource != 0 && inSource->alive && w->frostArmor > 0.0f ) {
        if( inSource->chill < 0.5f ) {
            inSource->chill = 0.5f;
            }
        inSource->chillT = CHILL_TIME;
        damageEnemy( inSource, w->dmg * w->frostArmor, 0, 0x80B8FFFFu );
        }
    if( g.hp <= 0.0f && g.state == STATE_PLAY ) {
        FILE *f;
        g.hp = 0.0f;
        g.state = STATE_DEAD;
        g.shake = 8.0f;
        addBurst( (float)HERO_X, (float)GROUND_Y - 20.0f, 50, 0xA050D0FFu,
                  150.0f );
        sndPlay( SND_DEATH );
        if( g.wave + 1 > g.best ) {
            g.best = g.wave + 1;
            g.newBest = 1;
            if( ! g.noSave ) {
                f = fopen( SAVE_FILE, "w" );
                if( f != 0 ) {
                    fprintf( f, "%d\n", g.best );
                    fclose( f );
                    }
                }
            }
        }
    }



static void spawnBolt( int inHostile, float inX, float inY, int inTarget,
                       float inDmg, float inArc, Color inC ) {
    int i;
    for( i = 0; i < MAX_BOLTS; i++ ) {
        Bolt *b = &( g.bolts[i] );
        if( ! b->alive ) {
            b->alive = 1;
            b->hostile = (char)inHostile;
            b->x = inX;
            b->y = inY;
            b->drawY = inY;
            b->startDist = 0.0f;
            b->arc = inArc;
            b->dmg = inDmg;
            b->target = inTarget;
            b->color = inC;
            return;
            }
        }
    }



static void heroFire( void ) {
    WandStats *w = &( g.wand );
    int order[ MAX_ENEMIES ];
    int n = 0;
    int i, j, shots;
    Color col = 0xC0A0FFFFu;
    int tries;

    /* targets, nearest first */
    for( i = 0; i < MAX_ENEMIES; i++ ) {
        if( g.enemies[i].alive && g.enemies[i].x < FIRE_RANGE_X ) {
            j = n;
            while( j > 0 &&
                   g.enemies[ order[ j - 1 ] ].x > g.enemies[i].x ) {
                order[j] = order[ j - 1 ];
                j--;
                }
            order[j] = i;
            n++;
            }
        }
    if( n == 0 ) {
        return;
        }

    shots = 1 + (int)w->extraShots;
    if( frand() < w->extraShots - (float)(int)w->extraShots ) {
        shots++;
        }
    g.atkCount++;
    if( w->barrage > 0 && g.atkCount % 4 == 0 ) {
        shots += w->barrage;
        }

    /* bolts take the colors of the socketed gems, in turn */
    for( tries = 0; tries < NUM_SOCKETS; tries++ ) {
        Item *it;
        g.boltColorIdx = ( g.boltColorIdx + 1 ) % NUM_SOCKETS;
        it = &( g.slots[ g.boltColorIdx ] );
        if( g.unlocked[ g.boltColorIdx ] && it->type != ITEM_NONE ) {
            col = itemLightColor( it->type );
            break;
            }
        }

    for( i = 0; i < shots; i++ ) {
        float dmg = w->dmg * ( 0.9f + 0.2f * frand() );
        float arc = 0.0f;
        if( i > 0 ) {
            dmg *= w->extraFrac;
            arc = (float)( ( i + 1 ) / 2 ) * 14.0f
                * ( ( i % 2 ) ? 1.0f : -1.0f );
            }
        spawnBolt( 0, (float)HERO_X + 12.0f, (float)GROUND_Y - 24.0f,
                   order[ i % n ], dmg, arc, col );
        }
    g.castAnim = 0.15f;
    sndPlay( SND_SHOOT );
    }



static void updateBolts( void ) {
    int i;

    for( i = 0; i < MAX_BOLTS; i++ ) {
        Bolt *b = &( g.bolts[i] );
        float tx, ty, dx, dy, d, step;

        if( ! b->alive ) {
            continue;
            }
        if( b->hostile ) {
            tx = (float)HERO_X + 6.0f;
            ty = (float)GROUND_Y - 20.0f;
            step = 170.0f * TICK_DT;
            }
        else {
            Enemy *e = &( g.enemies[ b->target ] );
            if( ! e->alive ) {
                /* target fell:  find another, or fizzle */
                Enemy *o = nearestEnemy( b->x, b->y, 400.0f, 0 );
                if( o == 0 ) {
                    b->alive = 0;
                    addBurst( b->x, b->drawY, 3, b->color, 40.0f );
                    continue;
                    }
                b->target = (int)( o - g.enemies );
                e = o;
                }
            tx = e->x;
            ty = e->y;
            step = BOLT_SPEED * TICK_DT;
            }
        dx = tx - b->x;
        dy = ty - b->y;
        d = (float)sqrt( dx * dx + dy * dy );
        if( b->startDist < d ) {
            b->startDist = d;
            }
        if( d <= step + 4.0f ) {
            b->alive = 0;
            if( b->hostile ) {
                hurtHero( b->dmg, 0 );
                addBurst( tx, ty, 6, 0xFF8030FFu, 70.0f );
                }
            else {
                g.chainCounter++;
                hitEnemy( &( g.enemies[ b->target ] ), b->dmg,
                          g.wand.chains, b->color );
                }
            continue;
            }
        b->x += dx / d * step;
        b->y += dy / d * step;
        b->drawY = b->y;
        if( b->startDist > 1.0f ) {
            b->drawY = b->y - b->arc
                * (float)sin( 3.14159f * ( 1.0f - d / b->startDist ) );
            }
        if( ( g.tick & 1 ) == 0 ) {
            addParticle( b->x, b->drawY, -30.0f + 20.0f * frand(),
                         -10.0f + 20.0f * frand(), 0.25f, b->color, 1,
                         0.0f );
            }
        }
    }



static void updateEnemies( void ) {
    int i;
    int dotTick = 0;
    float dotHeal = 0.0f;
    /* a fight that drags on gets more and more dangerous, so that no
       build can stall forever */
    float enrage = 1.0f;

    g.waveTime += TICK_DT;
    if( g.waveTime > ENRAGE_AFTER ) {
        enrage += ( g.waveTime - ENRAGE_AFTER ) * 0.08f;
        }

    g.dotTimer -= TICK_DT;
    if( g.dotTimer <= 0.0f ) {
        g.dotTimer += DOT_PERIOD;
        dotTick = 1;
        }

    for( i = 0; i < MAX_ENEMIES; i++ ) {
        Enemy *e = &( g.enemies[i] );
        float speed;

        if( ! e->alive ) {
            continue;
            }
        if( g.walking ) {
            e->x -= WALK_SPEED * TICK_DT;
            }
        if( e->flash > 0.0f ) {
            e->flash -= TICK_DT;
            }
        if( e->lunge > 0.0f ) {
            e->lunge -= TICK_DT;
            }

        /* afflictions */
        if( e->burnT > 0.0f ) {
            e->burnT -= TICK_DT;
            if( ( ( g.tick + i ) % 6 ) == 0 ) {
                addParticle( e->x + (float)( irand( 13 ) - 6 ), e->y,
                             0.0f, -40.0f, 0.4f, 0xFF9030FFu, 2, 0.0f );
                }
            if( e->burnT <= 0.0f ) {
                e->burnDps = 0.0f;
                }
            }
        if( e->poisonT > 0.0f ) {
            e->poisonT -= TICK_DT;
            if( ( ( g.tick + i ) % 9 ) == 0 ) {
                addParticle( e->x + (float)( irand( 13 ) - 6 ),
                             e->y - 4.0f, 0.0f, -25.0f, 0.5f, 0x60E060FFu,
                             2, 0.0f );
                }
            if( e->poisonT <= 0.0f ) {
                e->poisonDps = 0.0f;
                }
            }
        if( e->chillT > 0.0f ) {
            e->chillT -= TICK_DT;
            if( e->chillT <= 0.0f ) {
                e->chill = 0.0f;
                }
            }
        if( dotTick && ( e->burnDps > 0.0f || e->poisonDps > 0.0f ) ) {
            float dot = ( e->burnDps + e->poisonDps ) * DOT_PERIOD
                * g.wand.dotMult;
            dotHeal += ( dot < e->hp ? dot : e->hp ) * g.wand.leech;
            damageEnemy( e, dot, 0,
                         e->burnDps > e->poisonDps ? 0xFF9030FFu
                                                   : 0x60E060FFu );
            if( ! e->alive ) {
                continue;
                }
            }

        /* below zero, this counts the time since the foe thawed */
        if( e->stunT > -100.0f ) {
            e->stunT -= TICK_DT;
            }
        if( e->stunT > 0.0f ) {
            continue;
            }

        speed = e->speed * ( 1.0f - e->chill );
        e->anim += TICK_DT * ( 1.0f - e->chill );

        if( e->flying ) {
            e->y = (float)GROUND_Y - 8.0f * (float)e->scale - 26.0f
                + 6.0f * (float)sin( e->anim * 6.0f );
            }

        if( e->x > e->stopX ) {
            e->x -= speed * TICK_DT;
            /* far-off foes hurry until they are on stage */
            if( e->x > (float)SCREEN_W + 30.0f ) {
                e->x -= 40.0f * TICK_DT;
                }
            }
        else {
            e->atkTimer -= TICK_DT * ( 1.0f - e->chill * 0.5f );
            if( e->atkTimer <= 0.0f ) {
                e->atkTimer = e->atkInterval;
                e->lunge = 0.15f;
                if( e->ranged ) {
                    spawnBolt( 1, e->x - 8.0f, e->y, 0, e->dmg * enrage,
                               0.0f, 0xFF7030FFu );
                    }
                else {
                    hurtHero( e->dmg * enrage, e );
                    }
                }
            }
        }

    if( dotHeal > 0.0f ) {
        healHero( dotHeal );
        }
    }



static void spawnEnemy( int inType, float inX, int inBoss ) {
    const EnemyDef *def = &( enemyDefs[ inType ] );
    float lvl = (float)g.wave;
    float hpMult = ( 1.0f + 0.08f * lvl ) * (float)pow( 1.08, lvl );
    float dmgMult = (float)pow( 1.05, lvl );
    int i;

    for( i = 0; i < MAX_ENEMIES; i++ ) {
        Enemy *e = &( g.enemies[i] );
        if( e->alive ) {
            continue;
            }
        memset( e, 0, sizeof( Enemy ) );
        e->alive = 1;
        e->type = (char)inType;
        e->boss = (char)inBoss;
        e->ranged = def->ranged;
        e->flying = def->flying;
        e->scale = inBoss ? 4 : 2;
        e->x = inX;
        e->y = (float)GROUND_Y - 8.0f * (float)e->scale;
        e->maxHp = ( inBoss ? BOSS_HP : def->hp ) * hpMult;
        e->hp = e->maxHp;
        e->speed = ( inBoss ? 20.0f : def->speed )
            * ( 0.9f + 0.2f * frand() );
        e->dmg = ( inBoss ? BOSS_DMG : def->dmg ) * dmgMult;
        e->atkInterval = inBoss ? 1.3f : def->interval;
        e->atkTimer = def->interval * 0.5f;
        e->xp = (int)( (float)( inBoss ? 30 : def->xp )
                       * ( 1.0f + 0.08f * lvl ) );
        e->anim = frand() * 3.0f;
        e->chainId = -1;
        e->stunT = -100.0f;
        if( e->ranged ) {
            e->stopX = (float)HERO_X + 150.0f + (float)irand( 60 );
            }
        else {
            e->stopX = (float)HERO_X + 16.0f + 5.0f * (float)e->scale
                + (float)irand( 14 );
            }
        return;
        }
    }



static void spawnWave( void ) {
    int stage = g.wave / WAVES_PER_STAGE;
    int w = g.wave % WAVES_PER_STAGE;
    int pool[ NUM_ENEMY_TYPES ];
    int poolN = 0;
    int i, n;
    float x = (float)SCREEN_W + 30.0f;
    char buf[64];

    for( i = 0; i < NUM_ENEMY_TYPES; i++ ) {
        if( enemyDefs[i].firstWave <= g.wave ) {
            pool[ poolN ] = i;
            poolN++;
            }
        }

    if( w == WAVES_PER_STAGE - 1 ) {
        int bossType = stage % NUM_ENEMY_TYPES;
        spawnEnemy( bossType, x + 20.0f, 1 );
        sprintf( buf, T( TXT_BOSS_FMT ), T( TXT_BOSS0 + bossType ) );
        setBanner( buf, 0xFF6060FFu );
        sndPlay( SND_BOSS );
        n = 2 + stage;
        if( n > 7 ) {
            n = 7;
            }
        x += 70.0f;
        }
    else {
        n = 2 + ( w + 1 ) / 2 + stage;
        if( n > 14 ) {
            n = 14;
            }
        }

    for( i = 0; i < n; i++ ) {
        int type = pool[ irand( poolN ) ];
        /* newest kinds show up a little more */
        if( frand() < 0.25f ) {
            type = pool[ poolN - 1 ];
            }
        spawnEnemy( type, x, 0 );
        x += 22.0f + (float)irand( 34 );
        if( type == ENEMY_SPIDER ) {
            /* spiders come in pairs */
            spawnEnemy( type, x, 0 );
            x += 18.0f;
            }
        if( type == ENEMY_GOLEM ) {
            i++;
            }
        }
    g.waveActive = 1;
    g.waveTime = 0.0f;
    }



static void makeCards( void ) {
    int i, j;
    int haveShard = 0;
    int haveHeart = 0;

    for( i = 0; i < 3; i++ ) {
        Card *c = &( g.cards[i] );
        c->kind = CARD_ITEM;
        c->item.level = 1;
        if( ! haveShard && lockedSocketCount() > 0 && frand() < 0.24f ) {
            c->item.type = ITEM_SHARD;
            haveShard = 1;
            }
        else if( ! haveHeart && frand() < 0.13f ) {
            c->kind = CARD_HEART;
            c->item.type = ITEM_NONE;
            haveHeart = 1;
            }
        else {
            int tries = 0;
            int dup = 1;
            while( dup && tries < 20 ) {
                c->item.type = (unsigned char)randomGemType();
                dup = 0;
                for( j = 0; j < i; j++ ) {
                    if( g.cards[j].kind == CARD_ITEM &&
                        g.cards[j].item.type == c->item.type ) {
                        dup = 1;
                        }
                    }
                tries++;
                }
            c->item.level = (unsigned char)randomGemLevel();
            }
        }
    g.state = STATE_CARDS;
    sndPlay( SND_LEVELUP );
    }



int gamePickCard( int inCard ) {
    Card *c;

    if( g.state != STATE_CARDS || inCard < 0 || inCard > 2 ) {
        return 0;
        }
    c = &( g.cards[ inCard ] );
    if( c->kind == CARD_HEART ) {
        g.bonusHp += 20;
        gameRecalc();
        healHero( g.wand.maxHp * 0.4f );
        }
    else {
        int slot = freeInvSlot();
        if( slot < 0 ) {
            return 0;
            }
        g.slots[ slot ] = c->item;
        }
    g.pendingCards--;
    g.state = STATE_PLAY;
    sndPlay( SND_CARD );
    return 1;
    }



int gameMoveItem( int inFrom, int inTo ) {
    Item *a, *b;
    Item tmp;

    if( inFrom < 0 || inFrom >= SLOT_CRUCIBLE || inTo < 0 ||
        inTo >= NUM_SLOTS || inFrom == inTo ) {
        return MOVE_FAIL;
        }
    if( g.state != STATE_PLAY && g.state != STATE_CARDS ) {
        return MOVE_FAIL;
        }
    a = &( g.slots[ inFrom ] );
    b = &( g.slots[ inTo ] );
    if( a->type == ITEM_NONE || g.reserved[ inFrom ] ||
        g.reserved[ inTo ] ) {
        return MOVE_FAIL;
        }

    if( inTo == SLOT_CRUCIBLE ) {
        addXp( dissolveXp( *a ) );
        a->type = ITEM_NONE;
        gameRecalc();
        return MOVE_DISSOLVE;
        }

    if( inTo < NUM_SOCKETS && ! g.unlocked[ inTo ] ) {
        if( a->type == ITEM_SHARD && socketUnlockable( inTo ) ) {
            g.unlocked[ inTo ] = 1;
            a->type = ITEM_NONE;
            gameRecalc();
            return MOVE_UNLOCK;
            }
        return MOVE_FAIL;
        }
    if( a->type == ITEM_SHARD && inTo < NUM_SOCKETS ) {
        return MOVE_FAIL;
        }

    if( a->type != ITEM_SHARD && b->type == a->type &&
        b->level == a->level && a->level < MAX_GEM_LEVEL ) {
        b->level++;
        a->type = ITEM_NONE;
        gameRecalc();
        return MOVE_MERGE;
        }

    if( b->type == ITEM_SHARD && inFrom < NUM_SOCKETS ) {
        return MOVE_FAIL;
        }
    tmp = *a;
    *a = *b;
    *b = tmp;
    gameRecalc();
    return MOVE_OK;
    }



/* ---- the main tick ------------------------------------------------ */

static void updateEffects( void ) {
    int i;

    for( i = 0; i < MAX_PARTICLES; i++ ) {
        Particle *p = &( g.parts[i] );
        if( p->life > 0.0f ) {
            p->life -= TICK_DT;
            p->x += p->vx * TICK_DT;
            p->y += p->vy * TICK_DT;
            p->vy += p->gravity * TICK_DT;
            }
        }
    for( i = 0; i < MAX_FLOATERS; i++ ) {
        Floater *f = &( g.floats[i] );
        if( f->life > 0.0f ) {
            f->life -= TICK_DT;
            f->y -= 22.0f * TICK_DT;
            }
        }
    for( i = 0; i < MAX_ARCS; i++ ) {
        if( g.arcs[i].life > 0.0f ) {
            g.arcs[i].life -= TICK_DT;
            }
        }
    for( i = 0; i < MAX_FLYERS; i++ ) {
        Flyer *f = &( g.flyers[i] );
        if( f->alive ) {
            f->t += TICK_DT / 0.7f;
            if( f->t >= 1.0f ) {
                f->alive = 0;
                g.slots[ f->slot ] = f->item;
                g.reserved[ f->slot ] = 0;
                sndPlay( SND_PICKUP );
                }
            }
        }
    if( g.bannerT > 0.0f ) {
        g.bannerT -= TICK_DT;
        }
    if( g.shake > 0.0f ) {
        g.shake -= 20.0f * TICK_DT;
        }
    if( g.hurtFlash > 0.0f ) {
        g.hurtFlash -= TICK_DT;
        }
    if( g.castAnim > 0.0f ) {
        g.castAnim -= TICK_DT;
        }
    }



void gameTick( void ) {
    int i;
    int anyEnemy = 0;
    float nearest = 100000.0f;

    g.tick++;
    updateEffects();

    if( g.state == STATE_TITLE ) {
        g.walking = 1;
        g.scroll += WALK_SPEED * TICK_DT;
        g.walkAnim += TICK_DT;
        return;
        }
    if( g.state != STATE_PLAY ) {
        return;
        }
    if( g.pendingCards > 0 ) {
        makeCards();
        return;
        }

    g.runTicks++;

    for( i = 0; i < MAX_ENEMIES; i++ ) {
        if( g.enemies[i].alive ) {
            anyEnemy = 1;
            if( g.enemies[i].x < nearest ) {
                nearest = g.enemies[i].x;
                }
            }
        }

    /* wave flow */
    if( g.waveActive && ! anyEnemy ) {
        char buf[64];
        g.waveActive = 0;
        g.wave++;
        g.walkLeft = WALK_BETWEEN_WAVES;
        /* a breather */
        healHero( g.wand.maxHp * 0.10f );
        if( g.wave % WAVES_PER_STAGE == 0 ) {
            int stage = g.wave / WAVES_PER_STAGE;
            sprintf( buf, T( TXT_STAGE_FMT ), stage + 1,
                     T( TXT_BIOME0 + stage % NUM_BIOMES ) );
            setBanner( buf, 0xFFFFFFFFu );
            g.walkLeft = WALK_BETWEEN_WAVES * 1.6f;
            }
        }

    g.walking = ( nearest > STOP_RANGE_X );
    if( g.walking ) {
        g.scroll += WALK_SPEED * TICK_DT;
        g.walkAnim += TICK_DT;
        if( ! g.waveActive ) {
            g.walkLeft -= WALK_SPEED * TICK_DT;
            if( g.walkLeft <= 0.0f ) {
                spawnWave();
                }
            }
        }

    /* the hero */
    if( g.wand.regen > 0.0f ) {
        healHero( g.wand.regen * TICK_DT );
        }
    if( g.wand.barrier > 0.0f ) {
        g.barrierTimer -= TICK_DT;
        if( g.barrierTimer <= 0.0f ) {
            g.barrierTimer = BARRIER_PERIOD;
            g.barrier = g.wand.barrier;
            }
        }
    else {
        g.barrier = 0.0f;
        }
    if( g.atkTimer > 0.0f ) {
        g.atkTimer -= TICK_DT;
        }
    if( g.atkTimer <= 0.0f && anyEnemy && nearest < FIRE_RANGE_X ) {
        g.atkTimer += g.wand.interval;
        if( g.atkTimer < 0.0f ) {
            g.atkTimer = 0.0f;
            }
        heroFire();
        }

    updateBolts();
    updateEnemies();
    }



void gameInit( void ) {
    FILE *f;
    int best = 0;
    char noSave = g.noSave;

    memset( &g, 0, sizeof( Game ) );
    g.noSave = noSave;
    if( ! g.noSave ) {
        f = fopen( SAVE_FILE, "r" );
        if( f != 0 ) {
            if( fscanf( f, "%d", &best ) != 1 ) {
                best = 0;
                }
            fclose( f );
            }
        }
    g.best = best;
    g.state = STATE_TITLE;
    g.unlocked[0] = 1;
    g.unlocked[1] = 1;
    g.unlocked[2] = 1;
    g.unlocked[3] = 1;
    gameRecalc();
    g.hp = g.wand.maxHp;
    }



void gameStartRun( void ) {
    int best = g.best;
    char noSave = g.noSave;
    float scroll = g.scroll;
    char buf[64];
    int a, b;

    memset( &g, 0, sizeof( Game ) );
    g.best = best;
    g.noSave = noSave;
    g.scroll = scroll;
    g.state = STATE_PLAY;
    g.unlocked[0] = 1;
    g.unlocked[1] = 1;
    g.unlocked[2] = 1;
    g.unlocked[3] = 1;

    /* a ruby, and two other gems that differ */
    g.slots[ SLOT_INV0 ].type = GEM_RUBY;
    g.slots[ SLOT_INV0 ].level = 1;
    a = 2 + irand( NUM_GEM_TYPES - 1 );
    if( a == GEM_OPAL ) {
        a = GEM_QUARTZ;
        }
    do {
        b = 2 + irand( NUM_GEM_TYPES - 1 );
        } while( b == a || b == GEM_OPAL );
    g.slots[ SLOT_INV0 + 1 ].type = (unsigned char)a;
    g.slots[ SLOT_INV0 + 1 ].level = 1;
    g.slots[ SLOT_INV0 + 2 ].type = (unsigned char)b;
    g.slots[ SLOT_INV0 + 2 ].level = 1;

    g.level = 1;
    g.xpNext = xpForLevel( 1 );
    g.walkLeft = 230.0f;
    g.barrierTimer = 1.0f;
    gameRecalc();
    g.hp = g.wand.maxHp;

    sprintf( buf, T( TXT_STAGE_FMT ), 1, T( TXT_BIOME0 ) );
    setBanner( buf, 0xFFFFFFFFu );
    }



/* ---- a simple scripted player, for testing and balance ------------ */

static int aiFindMerge( int inFrom ) {
    Item *a = &( g.slots[ inFrom ] );
    int i;
    for( i = 0; i < SLOT_CRUCIBLE; i++ ) {
        Item *b = &( g.slots[i] );
        if( i != inFrom && ! g.reserved[i] && b->type == a->type &&
            b->level == a->level && a->level < MAX_GEM_LEVEL &&
            ( i >= NUM_SOCKETS || g.unlocked[i] ) ) {
            return i;
            }
        }
    return -1;
    }



static void aiDissolveOne( void ) {
    int i;
    int worst = -1;
    for( i = SLOT_INV0; i < SLOT_CRUCIBLE; i++ ) {
        Item *it = &( g.slots[i] );
        if( it->type == ITEM_NONE || g.reserved[i] ) {
            continue;
            }
        if( it->type == ITEM_SHARD && lockedSocketCount() > 0 ) {
            continue;
            }
        if( worst < 0 || it->level < g.slots[ worst ].level ) {
            worst = i;
            }
        }
    if( worst >= 0 ) {
        gameMoveItem( worst, SLOT_CRUCIBLE );
        }
    }



void gameAiStep( void ) {
    int i, s;
    int freeCount = 0;

    if( g.state == STATE_CARDS ) {
        int pick = irand( 3 );
        if( freeInvSlot() < 0 ) {
            aiDissolveOne();
            }
        for( i = 0; i < 3; i++ ) {
            if( g.cards[i].kind == CARD_HEART &&
                g.hp < g.wand.maxHp * 0.5f ) {
                pick = i;
                }
            }
        for( i = 0; i < 3; i++ ) {
            if( g.cards[i].kind == CARD_ITEM &&
                g.cards[i].item.type == ITEM_SHARD ) {
                pick = i;
                }
            }
        gamePickCard( pick );
        return;
        }
    if( g.state != STATE_PLAY ) {
        return;
        }

    for( i = SLOT_INV0; i < SLOT_CRUCIBLE; i++ ) {
        Item *it = &( g.slots[i] );
        int target;
        int lowest = -1;

        if( g.reserved[i] ) {
            continue;
            }
        if( it->type == ITEM_NONE ) {
            freeCount++;
            continue;
            }
        if( it->type == ITEM_SHARD ) {
            for( s = 0; s < NUM_SOCKETS; s++ ) {
                if( socketUnlockable( s ) ) {
                    gameMoveItem( i, s );
                    return;
                    }
                }
            continue;
            }
        target = aiFindMerge( i );
        if( target >= 0 ) {
            gameMoveItem( i, target );
            return;
            }
        for( s = 0; s < NUM_SOCKETS; s++ ) {
            if( g.unlocked[s] && g.slots[s].type == ITEM_NONE ) {
                gameMoveItem( i, s );
                return;
                }
            if( g.unlocked[s] &&
                ( lowest < 0 ||
                  g.slots[s].level < g.slots[ lowest ].level ) ) {
                lowest = s;
                }
            }
        /* trade up */
        if( lowest >= 0 && g.slots[ lowest ].level < it->level ) {
            gameMoveItem( i, lowest );
            return;
            }
        }
    if( freeCount < 3 ) {
        aiDissolveOne();
        }
    }
