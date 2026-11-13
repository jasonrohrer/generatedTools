/*
 * Path Master
 *
 * Shared declarations for all modules.
 *
 * Pure C89.  Only SDL2 and OpenGL are linked.
 */

#ifndef GAME_H
#define GAME_H


#define SCREEN_W        640
#define SCREEN_H        480

/* the path scene occupies the top of the screen, the wand panel the rest */
#define SCENE_H         200
#define GROUND_Y        172
#define HERO_X          96

#define TICKS_PER_SEC   60
#define TICK_DT         ( 1.0f / 60.0f )


/* 0xRRGGBBAA */
typedef unsigned int Color;

#define COL_WHITE   0xFFFFFFFFu
#define COL_BLACK   0x000000FFu



/* ------------------------------------------------------------------ */
/* gfx.c:  sprite atlas, primitives, Unicode pixel font                */
/* ------------------------------------------------------------------ */

int   gfxNewSprite( int inW, int inH );
void  gfxSetPixel( int inSprite, int inX, int inY, Color inC );
Color gfxGetPixel( int inSprite, int inX, int inY );
int   gfxSpriteW( int inSprite );
int   gfxSpriteH( int inSprite );

/* rows of art separated by '/', each char is a palette color.
   inArtB (may be NULL) is appended below inArtA */
int   gfxSpriteFromArt( const char *inArtA, const char *inArtB );
Color gfxArtColor( char inC );

void  gfxInit( void );
void  gfxUpload( void );
void  gfxFrameBegin( void );
void  gfxFrameEnd( void );
void  gfxAdditive( int inOn );

void  drawSprite( int inSprite, int inX, int inY, int inScale, Color inTint );
void  drawRect( int inX, int inY, int inW, int inH, Color inC );
void  drawFrame( int inX, int inY, int inW, int inH, Color inC );
void  drawLine( int inX1, int inY1, int inX2, int inY2, Color inC );

#define TEXT_LINE_H  11

/* all text is UTF-8.  inY is the top of capital letters.
   returns x just past the end of the text */
int   drawText( int inX, int inY, const char *inText, Color inC );
int   drawTextEx( int inX, int inY, const char *inText, Color inC,
                  int inScale, int inShadow );
void  drawTextCentered( int inCX, int inY, const char *inText, Color inC,
                        int inScale, int inShadow );
int   textWidth( const char *inText );
/* word wraps, obeys '\n', returns y below the last line */
int   drawTextWrap( int inX, int inY, int inW, const char *inText,
                    Color inC );



/* ------------------------------------------------------------------ */
/* art.c:  all pixel art, generated at startup                         */
/* ------------------------------------------------------------------ */

enum {
    ENEMY_SLIME,
    ENEMY_BAT,
    ENEMY_SPIDER,
    ENEMY_SKELETON,
    ENEMY_IMP,
    ENEMY_GOLEM,
    NUM_ENEMY_TYPES
    };

enum {
    ICON_PAUSE,
    ICON_PLAY1,
    ICON_PLAY2,
    ICON_PLAY3,
    ICON_SOUND,
    ICON_MUSIC_OFF,
    ICON_MUTE,
    ICON_FULL,
    ICON_QUIT,
    ICON_HEART,
    ICON_LOCK,
    NUM_ICONS
    };

extern int sprHero[2];
extern int sprEnemy[ NUM_ENEMY_TYPES ][2];
extern int sprItem[16];
extern int sprIcon[ NUM_ICONS ];
extern int sprMountain, sprHill, sprTree[2], sprCloud;
extern int sprGrass, sprPath, sprGlow, sprSocket;

void  artInit( void );
Color itemColor( int inType );
Color itemLightColor( int inType );



/* ------------------------------------------------------------------ */
/* text.c:  every user-visible string, for future translation          */
/* ------------------------------------------------------------------ */

enum {
    TXT_TITLE,
    TXT_CLICK_START,
    TXT_BEST,
    TXT_HOWTO,
    TXT_WAND,
    TXT_SATCHEL,
    TXT_CRUCIBLE,
    TXT_CRUCIBLE_HINT,
    TXT_LEVEL_UP,
    TXT_CHOOSE,
    TXT_SATCHEL_FULL,
    TXT_DEAD_TITLE,
    TXT_DEAD_STATS,
    TXT_NEW_BEST,
    TXT_WALK_AGAIN,
    TXT_STAGE_FMT,
    TXT_BOSS_FMT,
    TXT_LV_FMT,
    TXT_POWER_FMT,
    TXT_FUSE_HINT,
    TXT_MAX_LEVEL,
    TXT_SYNERGIES,
    TXT_NO_SYNERGY,
    TXT_EMPTY_SOCKET,
    TXT_LOCKED_SOCKET,
    TXT_LOCKED_FAR,
    TXT_SHARD_NAME,
    TXT_SHARD_DESC,
    TXT_SHARD_SHORT,
    TXT_HEART_NAME,
    TXT_HEART_DESC,
    TXT_STAT_DAMAGE,
    TXT_STAT_RATE,
    TXT_STAT_CRIT,
    TXT_STAT_BURN,
    TXT_STAT_CHILL,
    TXT_STAT_POISON,
    TXT_STAT_CHAIN,
    TXT_STAT_BOLTS,
    TXT_STAT_STEAL,
    TXT_STAT_REGEN,
    TXT_STAT_BARRIER,
    TXT_SATCHEL_FULL_XP,
    TXT_PAUSED,
    TXT_CANT_PLACE,
    TXT_NEED_SHARD,
    TXT_RESONANCE,
    TXT_BIOME0,
    TXT_BIOME1,
    TXT_BIOME2,
    TXT_BIOME3,
    TXT_BOSS0,
    TXT_BOSS1,
    TXT_BOSS2,
    TXT_BOSS3,
    TXT_BOSS4,
    TXT_BOSS5,
    TXT_GEM_NAME0,
    TXT_GEM_DESC0 = TXT_GEM_NAME0 + 12,
    TXT_SYN_NAME0 = TXT_GEM_DESC0 + 12,
    TXT_SYN_DESC0 = TXT_SYN_NAME0 + 15,
    NUM_TXT       = TXT_SYN_DESC0 + 15
    };

const char *T( int inID );



/* ------------------------------------------------------------------ */
/* sound.c:  tiny synthesizer                                          */
/* ------------------------------------------------------------------ */

enum {
    SND_SHOOT,
    SND_HIT,
    SND_CRIT,
    SND_KILL,
    SND_HURT,
    SND_PICKUP,
    SND_PLACE,
    SND_MERGE,
    SND_UNLOCK,
    SND_LEVELUP,
    SND_CARD,
    SND_DISSOLVE,
    SND_DEATH,
    SND_BOSS,
    SND_ERROR,
    SND_CLICK,
    SND_ZAP,
    SND_EXPLODE,
    SND_BOSS_DIE,
    NUM_SNDS
    };

void sndInit( void );
void sndPlay( int inSound );
/* 0 = everything, 1 = effects only, 2 = silence */
void sndSetMode( int inMode );



/* ------------------------------------------------------------------ */
/* logic.c:  the whole simulation, independent of video                */
/* ------------------------------------------------------------------ */

enum {
    ITEM_NONE = 0,
    GEM_RUBY,
    GEM_SAPPHIRE,
    GEM_TOPAZ,
    GEM_EMERALD,
    GEM_AMETHYST,
    GEM_DIAMOND,
    GEM_ONYX,
    GEM_QUARTZ,
    GEM_GARNET,
    GEM_OPAL,
    GEM_CITRINE,
    GEM_MOONSTONE,
    ITEM_SHARD,
    NUM_ITEM_TYPES
    };

#define NUM_GEM_TYPES   12
#define MAX_GEM_LEVEL   5

enum {
    SYN_THERMAL,
    SYN_PLASMA,
    SYN_WILDFIRE,
    SYN_SHATTER,
    SYN_SUPERCOND,
    SYN_OVERCLOCK,
    SYN_LEECH,
    SYN_EXECUTE,
    SYN_KALEIDO,
    SYN_BARRAGE,
    SYN_BULWARK,
    SYN_RADIANCE,
    SYN_BLOODPACT,
    SYN_FROSTARMOR,
    SYN_ZENITH,
    NUM_SYN
    };

#define MAX_SYN_RANK    3

/* slots:  wand sockets first, then the satchel, then the crucible */
#define NUM_SOCKETS     19
#define INV_SIZE        16
#define SLOT_INV0       NUM_SOCKETS
#define SLOT_CRUCIBLE   ( NUM_SOCKETS + INV_SIZE )
#define NUM_SLOTS       ( SLOT_CRUCIBLE + 1 )

#define WAVES_PER_STAGE 8
#define NUM_BIOMES      4

#define MAX_ENEMIES     40
#define MAX_BOLTS       64
#define MAX_PARTICLES   300
#define MAX_FLOATERS    48
#define MAX_ARCS        16
#define MAX_FLYERS      12

enum {
    STATE_TITLE,
    STATE_PLAY,
    STATE_CARDS,
    STATE_DEAD
    };

enum {
    MOVE_FAIL = 0,
    MOVE_OK,
    MOVE_MERGE,
    MOVE_UNLOCK,
    MOVE_DISSOLVE
    };

enum {
    CARD_ITEM,
    CARD_HEART
    };


typedef struct {
        unsigned char type;
        unsigned char level;
    } Item;


typedef struct {
        int  kind;
        Item item;
    } Card;


typedef struct {
        float dmg;
        float interval;
        float critChance;
        float critMult;
        float burnDps;
        float chill;
        int   chains;
        float chainFrac;
        float poisonDps;
        float extraShots;
        float extraFrac;
        float lifesteal;
        float maxHp;
        float regen;
        float barrier;
        float dmgTakenMult;
        float dotMult;
        float thermal;
        float plasma;
        float shatter;
        float execute;
        int   barrage;
        float leech;
        float frostArmor;
        int   syn[ NUM_SYN ];
        /* per socket results, for display */
        float power[ NUM_SOCKETS ];
    } WandStats;


typedef struct {
        char  alive;
        char  type;
        char  boss;
        char  ranged;
        char  flying;
        int   scale;
        float x, y;
        float hp, maxHp;
        float speed;
        float dmg;
        float atkInterval, atkTimer;
        float burnDps, burnT;
        float chill, chillT;
        float poisonDps, poisonT;
        float stunT;
        float flash;
        float anim;
        float lunge;
        float stopX;
        int   xp;
        int   chainId;
    } Enemy;


typedef struct {
        char  alive;
        char  hostile;
        float x, y;
        float drawY;
        float startDist;
        float arc;
        float dmg;
        int   target;
        Color color;
    } Bolt;


typedef struct {
        float x, y, vx, vy;
        float life, maxLife;
        float gravity;
        int   size;
        Color color;
    } Particle;


typedef struct {
        float x, y;
        float life;
        int   big;
        Color color;
        char  text[20];
    } Floater;


typedef struct {
        float x1, y1, x2, y2;
        float life;
        int   seed;
    } Arc;


typedef struct {
        char  alive;
        Item  item;
        float x, y;
        float t;
        int   slot;
    } Flyer;


typedef struct {
        int       state;
        int       tick;
        int       runTicks;

        Item      slots[ NUM_SLOTS ];
        char      unlocked[ NUM_SOCKETS ];
        char      reserved[ NUM_SLOTS ];
        WandStats wand;

        float     hp;
        float     barrier;
        float     barrierTimer;
        int       bonusHp;

        int       level;
        int       xp;
        int       xpNext;
        int       pendingCards;
        Card      cards[3];

        int       wave;
        char      waveActive;
        float     walkLeft;
        float     waveTime;
        float     scroll;
        char      walking;
        float     walkAnim;

        float     atkTimer;
        int       atkCount;
        float     castAnim;
        float     hurtFlash;
        float     dotTimer;
        float     shake;
        int       boltColorIdx;
        int       chainCounter;
        int       dropPity;

        int       kills;
        int       best;
        char      newBest;
        char      noSave;

        char      banner[64];
        float     bannerT;
        Color     bannerColor;

        Enemy     enemies[ MAX_ENEMIES ];
        Bolt      bolts[ MAX_BOLTS ];
        Particle  parts[ MAX_PARTICLES ];
        Floater   floats[ MAX_FLOATERS ];
        Arc       arcs[ MAX_ARCS ];
        Flyer     flyers[ MAX_FLYERS ];
        int       nextPart;
        int       nextFloat;
        int       nextArc;
    } Game;


typedef struct {
        int a, b;
    } SynPair;


extern Game g;
extern const SynPair synPairs[ NUM_SYN ];
extern const signed char sockQ[ NUM_SOCKETS ];
extern const signed char sockR[ NUM_SOCKETS ];

void  gameSeed( unsigned int inSeed );
unsigned int rnd( void );
float frand( void );

void  gameInit( void );
void  gameStartRun( void );
void  gameTick( void );
void  gameRecalc( void );
int   gameMoveItem( int inFrom, int inTo );
int   gamePickCard( int inCard );
void  gameAiStep( void );

int   socketsAdjacent( int inA, int inB );
/* locked, but touching an unlocked socket */
int   socketUnlockable( int inSocket );
int   lockedSocketCount( void );
/* effect numbers of a gem type at a given power */
void  gemEffect( int inType, float inPower, float *outA, float *outB );
/* effect number of a synergy at a given rank */
float synValue( int inSyn, int inRank );
int   dissolveXp( Item inItem );

void  addParticle( float inX, float inY, float inVX, float inVY,
                   float inLife, Color inC, int inSize, float inGravity );
void  addBurst( float inX, float inY, int inN, Color inC, float inSpeed );
void  addFloater( float inX, float inY, const char *inText, Color inC,
                  int inBig );



/* ------------------------------------------------------------------ */
/* ui.c:  drawing and mouse control                                    */
/* ------------------------------------------------------------------ */

/* 0 = paused, else ticks per frame */
extern int  uiSpeed;
extern char uiWantFullscreen;
extern char uiWantQuit;
/* set by --fonttest, shows a page of sample text */
extern char uiFontTest;

void uiMouseMove( int inX, int inY );
void uiMouseDown( void );
void uiMouseUp( void );
void uiDraw( void );
void uiSlotPos( int inSlot, int *outX, int *outY );


#endif
