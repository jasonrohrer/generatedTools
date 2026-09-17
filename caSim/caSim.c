/*
  caSim.c

  Headless, automated play-testing simulator for Chessamphetamine (CA).

  Instead of running the graphical game, this program drives the underlying
  CA source modules (chess.h, levels.h, formation.h, playerDeck.h, shop.h,
  sideBoard.h, money.h, hearts.h, unlocks.h, etc.) directly, step-by-step,
  with a simple scripted "player" making the decisions a human would make.

  Each run starts a fresh player from scratch (level 0, 2 hearts, starting
  deck, starting money, default formation) and plays until the player dies.
  Many runs are made per batch, and a histogram of how far players got is
  printed for each batch.

  Batches:
    -- No unlocks
    -- First unlock only         (extra sideboard slot)
    -- First two unlocks only    (+ extra shop slot)
    -- All three unlocks         (+ permanent sale slot)

  Settings files (in the current working directory):
    logOn.ini        contains 1 to turn on verbose logging of every decision
    numPerBatch.ini  number of runs per batch
    maxLevel.ini     (optional) level at which a run is stopped and counted
                     as having reached that level (default 100)

  Pure C89.  stdio.h is the only non-CA header (stdout output, plus
  reading the small .ini settings files).

  Build:  see buildCaSim.sh
*/


#include <stdio.h>


/* make sure the "null" implementation of mingin is what the CA headers
   see (only matters for headers that peek at the platform) */
#undef __linux__
#undef _WIN32


#include "gameSize.h"

#include "maxigin.h"


/* pull the implementation of every CA module into this translation unit,
   exactly like game.c does.  This also gives us direct access to each
   module's static state (formation grid, side board slots, shop slots, rng
   seeds, etc.), which we use to drive the game headlessly. */

#define CHESS_IMPLEMENTATION
#define BOARD_IMPLEMENTATION
#define PARTICLE_SPRITES_IMPLEMENTATION
#define PIECE_SPRITES_IMPLEMENTATION
#define MOVE_ANIM_IMPLEMENTATION
#define MONEY_IMPLEMENTATION
#define NUMBERS_IMPLEMENTATION
#define CHECK_DISPLAY_IMPLEMENTATION
#define UTIL_IMPLEMENTATION
#define COLORS_IMPLEMENTATION
#define FIXED_MATH_IMPLEMENTATION
#define PINCH_IMPLEMENTATION
#define PARTICLE_SYSTEM_IMPLEMENTATION
#define PIECE_DESCRIPTIONS_IMPLEMENTATION
#define NAV_IMPLEMENTATION
#define LEVELS_IMPLEMENTATION
#define DECK_IMPLEMENTATION
#define SHOP_IMPLEMENTATION
#define BUTTON_IMPLEMENTATION
#define HEARTS_IMPLEMENTATION
#define SIDE_BOARD_IMPLEMENTATION
#define DECK_VIEW_IMPLEMENTATION
#define COST_IMPLEMENTATION
#define RARITY_IMPLEMENTATION
#define ROLL_IMPLEMENTATION
#define SLOT_LIFT_IMPLEMENTATION
#define FORMATION_IMPLEMENTATION
#define UNLOCKS_IMPLEMENTATION
#define PLAYER_DECK_IMPLEMENTATION

#include "chess.h"
#include "board.h"
#include "particleSprite.h"
#include "pieceSprites.h"
#include "moveAnim.h"
#include "money.h"
#include "numbers.h"
#include "checkDisplay.h"
#include "util.h"
#include "colors.h"
#include "fixedMath.h"
#include "pinch.h"
#include "particleSystem.h"
#include "pieceDescriptions.h"
#include "nav.h"
#include "levels.h"
#include "deck.h"
#include "shop.h"
#include "button.h"
#include "hearts.h"
#include "sideBoard.h"
#include "deckView.h"
#include "cost.h"
#include "rarity.h"
#include "roll.h"
#include "slotLift.h"
#include "formation.h"
#include "unlocks.h"
#include "playerDeck.h"



/*
  =========================================================================
  maxigin game callbacks

  maxigin's implementation (compiled in maxigin_imp.c) calls these.
  They are never actually invoked, because we never run the engine loop,
  but they must exist for the program to link.
  =========================================================================
*/

void maxiginGame_init( void ) {
    }


void maxiginGame_step( void ) {
    }


void maxiginGame_getNativePixels( unsigned char  *inRGBBuffer ) {
    (void)inRGBBuffer;
    }



/*
  =========================================================================
  Settings
  =========================================================================
*/

/* verbose logging of every decision, board, etc. */
static  char  logOn                    =  0;

static  int   numPerBatch              =  100;

/* a run that reaches this level is stopped, and counted as reaching it */
static  int   maxLevel                 =  100;

#define  SIM_MAX_LEVEL_LIMIT  1000


/* this matches the game.c value */
static  int   simStartingMoney         =  5;

/* shop allowance given each time the shop appears (matches game.c) */
static  int   simShopAllowance         =  3;


/* base random seed.  Run i of every batch uses ( simBaseSeed + i ) */
static  unsigned long  simBaseSeed     =  1000UL;



/*
  =========================================================================
  Player strategy knobs
  =========================================================================
*/

/* the player will pay a sideboard redraw as long as the redraw cost is
   at most this (redraw costs are 1, 2, 3, 6, 10, ...) */
static  int   playerMaxRedrawCost      =  1;

/* player only rerolls the shop if money left after paying for the reroll
   is at least this much (so there's something to buy after) */
static  int   playerRerollMinLeftover  =  4;

/* max shop rerolls per shop visit */
static  int   playerMaxRerolls         =  3;

/* if a new formation spot is on sale, but the player can't afford it,
   the player saves money (skips piece purchases) when the shortfall
   is at most this much (one allowance is 3) */
static  int   playerSaveForSpotMargin  =  3;

/* the player only buys a piece if its value is at least this fraction
   (in percent) of the average value of the pieces already in the deck
   (so a deck full of queens stops buying knights) */
static  int   playerMinBuyValuePercent =  100;

/* the player leaves this much money unspent in the shop, so that
   side board redraws are affordable on the next level */
static  int   playerMoneyReserve       =  0;

/* when placing pieces into formation spots, the player tries this many
   random arrangements (in addition to the standard pawns-forward layout)
   and keeps the one that a shallow look-ahead likes best.
   0 turns this off (standard layout only) */
static  int   playerPlacementTrials    =  12;

/* search depth (plies) of that look-ahead */
static  int   playerPlacementDepth     =  2;



/*
  =========================================================================
  Logging helpers
  =========================================================================
*/

static void logLine( const char  *inString ) {
    if( logOn ) {
        printf( "%s\n",
                inString );
        }
    }


static void logBoard( BoardState  *inState ) {
    if( logOn ) {
        printf( "%s\n",
                getBoardStateString( inState ) );
        }
    }


static const char *pieceLabel( ChessPiece  inPiece ) {

    ChessPiece  t  =  inPiece & CHESS_TYPE_MASK;

    if( t == noPiece ) {
        return "(empty)";
        }
    return getPieceName( t );
    }



/*
  =========================================================================
  Player deck inspection helpers
  =========================================================================
*/

/* the player's perceived value of a piece type
   (we use the chess AI's own material values) */
static int playerPieceValue( ChessPiece  inPiece ) {
    return pieceValue[ inPiece & CHESS_TYPE_MASK ];
    }



/* average perceived value of all pieces in the player's deck */
static int playerDeckAverageValue( void ) {

    Deck  *d      =  playerDeckGetDrawDeck();
    int    i;
    long   total  =  0;

    if( d->numPieces == 0 ) {
        return 0;
        }

    for( i = 0;
         i < d->numPieces;
         i ++ ) {
        total += playerPieceValue( d->pieces[ i ] );
        }

    return (int)( total / d->numPieces );
    }



/* counts pieces that would be drawn next from the deck (before any
   reshuffle) that are strictly better than inValue */
static int playerDeckCountBetterDrawable( int  inValue ) {

    Deck  *d      =  playerDeckGetDrawDeck();
    int    i;
    int    count  =  0;

    if( d->drawPos >= 0 ) {
        for( i = 0;
             i <= d->drawPos;
             i ++ ) {
            if( playerPieceValue( d->pieces[ i ] ) > inValue ) {
                count ++;
                }
            }
        }
    else {
        /* deck exhausted, next draw reshuffles all present pieces */
        for( i = 0;
             i < d->numPieces;
             i ++ ) {
            if( d->present[ i ]
                &&
                playerPieceValue( d->pieces[ i ] ) > inValue ) {
                count ++;
                }
            }
        }
    return count;
    }



static void logDeckContents( void ) {

    Deck  *d  =  playerDeckGetDrawDeck();
    int    i;
    int    counts[ NUM_CHESS_PIECES ];

    if( ! logOn ) {
        return;
        }

    for( i = 0;
         i < NUM_CHESS_PIECES;
         i ++ ) {
        counts[ i ] = 0;
        }

    for( i = 0;
         i < d->numPieces;
         i ++ ) {
        counts[ d->pieces[ i ] & CHESS_TYPE_MASK ] ++;
        }

    printf( "  Deck (%d pieces, %d ready, avg value %d):",
            d->numPieces,
            playerDeckGetReadyCount(),
            playerDeckAverageValue() );

    for( i = FIRST_CHESS_PIECE;
         i < NUM_CHESS_PIECES;
         i ++ ) {
        if( counts[ i ] > 0 ) {
            printf( " %dx%s",
                    counts[ i ],
                    getPieceName( (ChessPiece)i ) );
            }
        }
    printf( "\n" );
    }



/*
  =========================================================================
  Formation layout
  =========================================================================
*/

/* white's formation rows are the last three rows (5, 6, 7),
   with row 7 being white's back rank */

#define  SIM_KING_ROW  7
#define  SIM_KING_COL  4

/* priority order for non-king formation spots:
   a pawn shield in front of the king, then the rest of the back two rows
   growing outward from the king, then the front row */

#define  SIM_NUM_SPOT_PRIORITIES  23

static  int  simSpotPriorityRow[ SIM_NUM_SPOT_PRIORITIES ] = {
    6, 6, 6,
    7, 7,
    6, 6,
    7, 7,
    5, 5, 5,
    7, 7,
    6, 6,
    5, 5,
    7, 6, 5, 5, 5 };

static  int  simSpotPriorityCol[ SIM_NUM_SPOT_PRIORITIES ] = {
    3, 5, 4,
    3, 5,
    2, 6,
    2, 6,
    3, 5, 4,
    1, 7,
    1, 7,
    2, 6,
    0, 0, 1, 7, 0 };



/* how far forward a piece type wants to be placed
   (lower = more forward) */
static int playerPlacementRank( ChessPiece  inPiece ) {

    switch( inPiece & CHESS_TYPE_MASK ) {
        case pawn:
            return 0;
        case doublingPawn:
            return 1;
        case laserPawn:
            return 2;
        case knight:
            return 3;
        case bishop:
            return 4;
        case rocket:
            return 5;
        case rook:
            return 6;
        case addingRook:
            return 7;
        case laserRook:
            return 8;
        case queen:
            return 9;
        default:
            return 10;
        }
    }



/* step 1: player arranges formation spots, blind to the enemy formation */
static void playerArrangeFormation( void ) {

    int  numSpots  =  formationGetNumNonKingSpots();
    int  y;
    int  x;
    int  i;

    if( numSpots > SIM_NUM_SPOT_PRIORITIES ) {
        numSpots = SIM_NUM_SPOT_PRIORITIES;
        }

    for( y = 0;
         y < BH;
         y ++ ) {
        for( x = 0;
             x < BW;
             x ++ ) {
            formation[ y ][ x ] = 0;
            }
        }

    formation[ SIM_KING_ROW ][ SIM_KING_COL ] = 2;

    for( i = 0;
         i < numSpots;
         i ++ ) {
        formation[ simSpotPriorityRow[ i ] ][ simSpotPriorityCol[ i ] ] = 1;
        }

    /* any newly-purchased spot has now been placed by the player */
    fmNewSpotWaiting = 0;
    formationPickedX = -1;
    formationPickedY = -1;

    if( logOn ) {
        printf( "  Formation (%d spots + king):\n",
                numSpots );
        for( y = BH - 3;
             y < BH;
             y ++ ) {
            printf( "    " );
            for( x = 0;
                 x < BW;
                 x ++ ) {
                char  c  =  '.';
                if( formation[ y ][ x ] == 1 ) {
                    c = 'o';
                    }
                else if( formation[ y ][ x ] == 2 ) {
                    c = 'K';
                    }
                printf( "%c ",
                        c );
                }
            printf( "\n" );
            }
        }
    }



/*
  =========================================================================
  Piece placement, side board swapping, and redrawing
  =========================================================================
*/

/* gathers white non-king pieces sitting in formation spots */
static int gatherFormationPieces( BoardState  *inState,
                                  int          outRows[ BN ],
                                  int          outCols[ BN ],
                                  ChessPiece   outPieces[ BN ] ) {
    int  y;
    int  x;
    int  n  =  0;

    for( y = 0;
         y < BH;
         y ++ ) {
        for( x = 0;
             x < BW;
             x ++ ) {
            if( formationGet( y,
                              x ) == 1 ) {
                outRows  [ n ] = y;
                outCols  [ n ] = x;
                outPieces[ n ] = inState->grid[ y ][ x ];
                n ++;
                }
            }
        }
    return n;
    }



static void logSideBoard( void ) {
    int  i;

    if( ! logOn ) {
        return;
        }

    printf( "  Side board (%d slots):",
            sbNumSlots );
    for( i = 0;
         i < sbNumSlots;
         i ++ ) {
        printf( " %s",
                pieceLabel( sideBoard[ i ] ) );
        }
    printf( "\n" );
    }



/* swaps side board pieces onto the board whenever a side board piece
   is better than the weakest piece on the board */
static void playerSwapFromSideBoard( BoardState  *inState ) {

    int         rows  [ BN ];
    int         cols  [ BN ];
    ChessPiece  pieces[ BN ];

    char  swapped  =  1;

    while( swapped ) {

        int  n           =  gatherFormationPieces( inState,
                                                   rows,
                                                   cols,
                                                   pieces );
        int  i;
        int  weakestI    =  -1;
        int  weakestVal  =  0;
        int  bestSB      =  -1;
        int  bestSBVal   =  0;

        swapped = 0;

        for( i = 0;
             i < n;
             i ++ ) {
            int  v  =  playerPieceValue( pieces[ i ] );

            if( weakestI == -1
                ||
                v < weakestVal ) {
                weakestI   = i;
                weakestVal = v;
                }
            }

        if( weakestI == -1 ) {
            return;
            }

        for( i = 0;
             i < sbNumSlots;
             i ++ ) {
            int  v  =  playerPieceValue( sideBoard[ i ] );

            if( sideBoard[ i ] != noPiece
                &&
                v > bestSBVal ) {
                bestSB    = i;
                bestSBVal = v;
                }
            }

        if( bestSB != -1
            &&
            bestSBVal > weakestVal ) {

            ChessPiece  newPiece;

            /* same as the game's swapMarkedPieces */
            sbPickedIndex = bestSB;

            newPiece = sideBoardSwap( pieces[ weakestI ] );

            if( newPiece != noPiece ) {

                inState->grid[ rows[ weakestI ] ][ cols[ weakestI ] ] =
                    newPiece | CHESS_WHITE;

                if( logOn ) {
                    printf( "  Swap: side board %s <-> board %s at (%d,%d)\n",
                            pieceLabel( newPiece ),
                            pieceLabel( pieces[ weakestI ] ),
                            rows[ weakestI ],
                            cols[ weakestI ] );
                    }
                swapped = 1;
                }
            sideBoardClearPick();
            }
        }
    }



/* the value of the weakest white non-king piece in the formation
   (a formation spot that came up empty counts as value 0) */
static int playerWeakestFormationValue( BoardState  *inState ) {

    int         rows  [ BN ];
    int         cols  [ BN ];
    ChessPiece  pieces[ BN ];
    int         n           =  gatherFormationPieces( inState,
                                                      rows,
                                                      cols,
                                                      pieces );
    int         i;
    int         weakestVal  =  -1;

    for( i = 0;
         i < n;
         i ++ ) {
        int  v  =  playerPieceValue( pieces[ i ] );

        if( weakestVal == -1
            ||
            v < weakestVal ) {
            weakestVal = v;
            }
        }
    return weakestVal;
    }



/* pays for side board redraws while it seems worth it */
static void playerConsiderRedraws( BoardState  *inState,
                                   int          inDrawCost ) {

    while( 1 ) {
        int  cost        =  costGet( inDrawCost );
        int  weakestVal  =  playerWeakestFormationValue( inState );
        int  numBetter;

        if( weakestVal == -1 ) {
            /* no formation spots at all */
            return;
            }

        numBetter = playerDeckCountBetterDrawable( weakestVal );

        if( numBetter == 0 ) {
            logLine( "  No better pieces left to draw, not redrawing" );
            return;
            }

        if( cost > playerMaxRedrawCost ) {
            if( logOn ) {
                printf( "  Redraw cost %d is above player limit %d, "
                        "not redrawing\n",
                        cost,
                        playerMaxRedrawCost );
                }
            return;
            }

        if( moneyGetTotal() < cost ) {
            if( logOn ) {
                printf( "  Can't afford redraw cost %d with %d money\n",
                        cost,
                        moneyGetTotal() );
                }
            return;
            }

        if( ! sideBoardIsRedrawHelpful() ) {
            logLine( "  Redraw would not be helpful, not redrawing" );
            return;
            }

        /* same as the game's draw button handling */
        moneyAdd( - cost );
        costIncrement( inDrawCost );
        sideBoardRedraw();
        playerDeckJustRefreshed();

        if( logOn ) {
            printf( "  Redraw side board for %d (money now %d)\n",
                    cost,
                    moneyGetTotal() );
            }
        logSideBoard();

        playerSwapFromSideBoard( inState );
        }
    }



/* rng for the player's random placement trials (seeded per run) */
static  MaxiginRand  placementRand;


/* scores a candidate starting position from white's point of view
   using the chess AI's own look-ahead (higher is better for white) */
static int playerEvaluatePlacement( BoardState  *inState ) {

    Move        m;
    Captured    c;
    BoardState  newState;
    int         score  =  0;
    BoardState  copy   =  *inState;

    if( ! getGreedyDepthMove( &copy,
                              1,
                              &m,
                              &c,
                              &newState,
                              &score,
                              - MAX_SCORE - SCORE_SCALE,
                              MAX_SCORE + SCORE_SCALE,
                              playerPlacementDepth,
                              0 ) ) {
        return - MAX_SCORE;
        }
    return score;
    }



/* rearranges the pieces among the formation spots:
   pawn-like pieces up front, big pieces in back, then tries random
   arrangements and keeps the one with the best look-ahead score */
static void playerPlacePieces( BoardState  *inState ) {

    int         rows  [ BN ];
    int         cols  [ BN ];
    ChessPiece  pieces[ BN ];
    int         n     =  gatherFormationPieces( inState,
                                                rows,
                                                cols,
                                                pieces );
    int         i;
    int         j;

    /* sort spots front-to-back (lower row first), center-out */
    for( i = 0;
         i < n - 1;
         i ++ ) {
        for( j = i + 1;
             j < n;
             j ++ ) {
            int  di  =  rows[ i ] * 100
                        + ( cols[ i ] > SIM_KING_COL ?
                            cols[ i ] - SIM_KING_COL :
                            SIM_KING_COL - cols[ i ] );
            int  dj  =  rows[ j ] * 100
                        + ( cols[ j ] > SIM_KING_COL ?
                            cols[ j ] - SIM_KING_COL :
                            SIM_KING_COL - cols[ j ] );
            if( dj < di ) {
                int  t;
                t = rows[ i ];  rows[ i ] = rows[ j ];  rows[ j ] = t;
                t = cols[ i ];  cols[ i ] = cols[ j ];  cols[ j ] = t;
                }
            }
        }

    /* sort pieces by placement rank (most forward first) */
    for( i = 0;
         i < n - 1;
         i ++ ) {
        for( j = i + 1;
             j < n;
             j ++ ) {
            if( playerPlacementRank( pieces[ j ] )
                <
                playerPlacementRank( pieces[ i ] ) ) {
                ChessPiece  t  =  pieces[ i ];
                pieces[ i ] = pieces[ j ];
                pieces[ j ] = t;
                }
            }
        }

    for( i = 0;
         i < n;
         i ++ ) {
        inState->grid[ rows[ i ] ][ cols[ i ] ] = pieces[ i ];
        }

    /* now try some random arrangements, and keep whichever one the
       chess AI's own shallow look-ahead scores best for white
       (i.e., the layout that sets up the best immediate trades) */
    if( playerPlacementTrials > 0
        &&
        n > 1 ) {

        static  int          order[ BN ];

        MaxiginRand  savedChessRand  =  chessRand;

        int          bestScore;
        int          t;
        ChessPiece   bestPieces[ BN ];

        bestScore = playerEvaluatePlacement( inState );

        for( i = 0;
             i < n;
             i ++ ) {
            bestPieces[ i ] = pieces[ i ];
            }

        if( logOn ) {
            printf( "  Standard placement scores %d\n",
                    bestScore );
            }

        for( t = 0;
             t < playerPlacementTrials;
             t ++ ) {

            int  score;

            for( i = 0;
                 i < n;
                 i ++ ) {
                order[ i ] = i;
                }
            maxigin_shuffle( &placementRand,
                             n,
                             order );

            for( i = 0;
                 i < n;
                 i ++ ) {
                inState->grid[ rows[ i ] ][ cols[ i ] ] =
                    pieces[ order[ i ] ];
                }

            score = playerEvaluatePlacement( inState );

            if( score > bestScore ) {
                bestScore = score;
                for( i = 0;
                     i < n;
                     i ++ ) {
                    bestPieces[ i ] = pieces[ order[ i ] ];
                    }
                if( logOn ) {
                    printf( "  Random placement %d scores better: %d\n",
                            t,
                            score );
                    }
                }
            }

        for( i = 0;
             i < n;
             i ++ ) {
            inState->grid[ rows[ i ] ][ cols[ i ] ] = bestPieces[ i ];
            }

        /* leave the match's random stream untouched by our trials */
        chessRand = savedChessRand;
        }
    }



/*
  =========================================================================
  The match
  =========================================================================
*/

enum{
    matchWhiteCheckmate = 0,
    matchWhiteOverrunWin,
    matchWhiteTrappedWin,
    matchBlackCheckmate,
    matchBlackOverrunWin,
    matchBlackTrappedWin,
    NUM_MATCH_RESULTS };


static  const char  *matchResultNames[ NUM_MATCH_RESULTS ] = {
    "player checkmated enemy",
    "player won by overrun",
    "player won (enemy trapped)",
    "player was checkmated",
    "player lost by overrun",
    "player lost (trapped)" };


static char matchIsPlayerWin( int  inResult ) {
    return inResult == matchWhiteCheckmate
        || inResult == matchWhiteOverrunWin
        || inResult == matchWhiteTrappedWin;
    }



/* runs the AI-vs-AI chess match, mirroring game.c
   returns one of the match result codes, and sets outNumMoves */
static int runMatch( BoardState  *inState,
                     int         *outNumMoves ) {

    int  noScoreMoveCount  =  0;
    int  numMoves          =  0;

    while( 1 ) {
        Move        m;
        Captured    c;
        BoardState  postMoveState;
        int         loserColor;

        if( ! getChessMove( inState,
                            &m,
                            &c,
                            &postMoveState ) ) {

            /* trapped condition */
            *outNumMoves = numMoves;

            if( inState->nextToMove == CHESS_BLACK ) {
                return matchWhiteTrappedWin;
                }
            else {
                return matchBlackTrappedWin;
                }
            }

        numMoves ++;

        if( logOn ) {
            int  i;

            printf( "  Move %d: %s %s (%d,%d) -> (%d,%d)",
                    numMoves,
                    ( inState->nextToMove == CHESS_WHITE ? "White" : "Black" ),
                    pieceLabel( inState->grid[ m.startPos[0] ]
                                              [ m.startPos[1] ] ),
                    m.startPos[0],
                    m.startPos[1],
                    m.endPos[0],
                    m.endPos[1] );

            for( i = 0;
                 i < c.num;
                 i ++ ) {
                printf( " captures %s%s",
                        ( ( c.pieces[i].p & CHESS_COLOR_MASK ) == CHESS_BLACK ?
                          "black " : "white " ),
                        pieceLabel( c.pieces[i].p ) );
                }
            printf( "\n" );
            }

        /* money for captures, like moveAnim.h */
        {
            int  i;
            for( i = 0;
                 i < c.num;
                 i ++ ) {
                moneyAddCapture( c.pieces[i].p );
                }
        }

        if( isCheckmate( &postMoveState,
                         &loserColor ) ) {

            applyMove( inState,
                       &m,
                       &postMoveState );
            logBoard( inState );

            *outNumMoves = numMoves;

            if( loserColor == CHESS_BLACK ) {
                return matchWhiteCheckmate;
                }
            else {
                return matchBlackCheckmate;
                }
            }
        else if( c.num == 0 ) {

            int  noScoreLimit  =  50;

            if( ! isForcedCheckmatePossible( &postMoveState ) ) {
                noScoreLimit = 20;
                }

            noScoreMoveCount ++;

            if( noScoreMoveCount > noScoreLimit ) {

                int  newScore  =  getScore( &postMoveState );

                applyMove( inState,
                           &m,
                           &postMoveState );
                logBoard( inState );

                if( logOn ) {
                    printf( "  Overrun after %d moves with no capture, "
                            "score %d\n",
                            noScoreMoveCount,
                            newScore );
                    }

                *outNumMoves = numMoves;

                if( newScore >= 0 ) {
                    /* tie counts as a win for white */
                    return matchWhiteOverrunWin;
                    }
                else {
                    return matchBlackOverrunWin;
                    }
                }
            }
        else {
            noScoreMoveCount = 0;
            }

        applyMove( inState,
                   &m,
                   &postMoveState );

        logBoard( inState );
        }
    }



/*
  =========================================================================
  The shop
  =========================================================================
*/

static void logShop( void ) {
    int  i;

    if( ! logOn ) {
        return;
        }

    printf( "  Shop (money %d, reroll cost %d):",
            moneyGetTotal(),
            costGet( shopRerollCost ) );

    for( i = 0;
         i < shopNumVisibleSlots;
         i ++ ) {
        if( shopItems[ i ] == noPiece ) {
            printf( " [sold]" );
            }
        else {
            printf( " [%s $%d%s]",
                    pieceLabel( shopItems[ i ] ),
                    shopSlotPrices[ i ],
                    ( shopIsOnSale[ i ] ?
                      ( shopIsPermaSale[ i ] ? " perma-sale" : " sale" ) :
                      "" ) );
            }
        }

    if( formationHasRoomForNewSpot()
        &&
        ! newSpotBought
        &&
        newSpotAvail ) {
        printf( " [new formation spot $%d]",
                costGet( newFormationSpotCost ) );
        }
    printf( "\n" );
    }



static char shopSpotForSale( void ) {
    return formationHasRoomForNewSpot()
        && ! newSpotBought
        && newSpotAvail;
    }



/* buys a new formation spot, same as shopStep does */
static void shopBuySpot( void ) {

    int  cost  =  costGet( newFormationSpotCost );

    moneyAdd( - cost );
    formationAddNewSpot();
    costIncrement( newFormationSpotCost );
    newSpotBought = 1;

    if( logOn ) {
        printf( "  Bought new formation spot for %d (money now %d, "
                "%d spots)\n",
                cost,
                moneyGetTotal(),
                formationGetNumNonKingSpots() );
        }
    }



/* buys the piece in a shop slot, same as shopStep does */
static void shopBuyPiece( int  inSlot ) {

    int  price  =  shopSlotPrices[ inSlot ];

    moneyAdd( - price );

    playerDeckAddPiece( shopItems[ inSlot ] );
    playerDeckReshuffle();

    shopSetNewSpotAvail();

    if( logOn ) {
        printf( "  Bought %s for %d (money now %d)\n",
                pieceLabel( shopItems[ inSlot ] ),
                price,
                moneyGetTotal() );
        }

    shopItems[ inSlot ] = noPiece;
    }



/* the player's shopping session
   returns number of pieces bought */
static int playerShop( void ) {

    int   numRerolls  =  0;
    int   numBought   =  0;
    char  done        =  0;

    logShop();
    logDeckContents();

    while( ! done ) {

        char  boughtSomething  =  0;
        char  saving           =  0;
        int   deckAvg          =  playerDeckAverageValue();
        int   minBuyValue      =  ( deckAvg * playerMinBuyValuePercent ) / 100;

        /* 1. formation spot first */
        if( shopSpotForSale() ) {

            int  spotCost  =  costGet( newFormationSpotCost );

            if( moneyGetTotal() - playerMoneyReserve >= spotCost ) {
                shopBuySpot();
                boughtSomething = 1;
                }
            else if( spotCost - ( moneyGetTotal() - playerMoneyReserve )
                     <= playerSaveForSpotMargin ) {
                saving = 1;
                if( logOn ) {
                    printf( "  Saving up for formation spot (%d, have %d)\n",
                            spotCost,
                            moneyGetTotal() );
                    }
                }
            }

        /* 2. best affordable piece worth adding to the deck */
        if( ! saving ) {

            int  bestSlot  =  -1;
            int  bestVal   =  0;
            int  i;

            for( i = 0;
                 i < shopNumVisibleSlots;
                 i ++ ) {

                ChessPiece  p  =  shopItems[ i ];
                int         v;

                if( p == noPiece ) {
                    continue;
                    }
                if( shopSlotPrices[ i ]
                    >
                    moneyGetTotal() - playerMoneyReserve ) {
                    continue;
                    }

                v = playerPieceValue( p );

                if( v < minBuyValue ) {
                    continue;
                    }

                /* prefer higher value, then lower price */
                if( bestSlot == -1
                    ||
                    v > bestVal
                    ||
                    ( v == bestVal
                      &&
                      shopSlotPrices[ i ] < shopSlotPrices[ bestSlot ] ) ) {
                    bestSlot = i;
                    bestVal  = v;
                    }
                }

            if( bestSlot != -1 ) {
                shopBuyPiece( bestSlot );
                numBought ++;
                boughtSomething = 1;
                }
            }

        if( boughtSomething ) {
            /* look again with our new money situation */
            continue;
            }

        /* 3. nothing (more) to buy... consider a reroll */
        if( ! saving
            &&
            numRerolls < playerMaxRerolls
            &&
            moneyGetTotal() - playerMoneyReserve - costGet( shopRerollCost )
            >= playerRerollMinLeftover ) {

            int  rerollCost  =  costGet( shopRerollCost );

            moneyAdd( - rerollCost );
            costIncrement( shopRerollCost );
            shopInternalReroll();
            numRerolls ++;

            if( logOn ) {
                printf( "  Rerolled shop for %d (money now %d)\n",
                        rerollCost,
                        moneyGetTotal() );
                }
            logShop();
            }
        else {
            done = 1;
            }
        }

    if( logOn ) {
        printf( "  Done shopping, money left %d\n",
                moneyGetTotal() );
        }
    logDeckContents();

    return numBought;
    }



/*
  =========================================================================
  Statistics
  =========================================================================
*/

typedef struct BatchStats {

        int   numRuns;

        /* how many runs died at (i.e., reached at most) each level */
        int   reachedHistogram[ SIM_MAX_LEVEL_LIMIT + 1 ];

        long  totalMaxLevel;

        int   numReachedMaxLevel;

        /* matches by result type */
        long  matchResultCounts[ NUM_MATCH_RESULTS ];

        long  totalMoves;
        long  totalMatches;

        long  totalSpotsAtDeath;
        long  totalDeckSizeAtDeath;
        long  totalMoneySpent;

        /* runs where the level-8/16/32 checkmate unlock got earned
           during the run (beyond what they started with) */
        int   numEarnedUnlock[ NUM_UNLOCKS ];

        /* per-level match outcomes */
        int   levelMatches      [ SIM_MAX_LEVEL_LIMIT + 1 ];
        int   levelCheckmateWins[ SIM_MAX_LEVEL_LIMIT + 1 ];
        int   levelOtherWins    [ SIM_MAX_LEVEL_LIMIT + 1 ];

    } BatchStats;



static void batchStatsClear( BatchStats  *inStats ) {
    int  i;

    inStats->numRuns             = 0;
    inStats->totalMaxLevel       = 0;
    inStats->numReachedMaxLevel  = 0;
    inStats->totalMoves          = 0;
    inStats->totalMatches        = 0;
    inStats->totalSpotsAtDeath   = 0;
    inStats->totalDeckSizeAtDeath = 0;
    inStats->totalMoneySpent     = 0;

    for( i = 0;
         i <= SIM_MAX_LEVEL_LIMIT;
         i ++ ) {
        inStats->reachedHistogram  [ i ] = 0;
        inStats->levelMatches      [ i ] = 0;
        inStats->levelCheckmateWins[ i ] = 0;
        inStats->levelOtherWins    [ i ] = 0;
        }
    for( i = 0;
         i < NUM_MATCH_RESULTS;
         i ++ ) {
        inStats->matchResultCounts[ i ] = 0;
        }
    for( i = 0;
         i < NUM_UNLOCKS;
         i ++ ) {
        inStats->numEarnedUnlock[ i ] = 0;
        }
    }



/*
  =========================================================================
  One full run (one player, from scratch until death)
  =========================================================================
*/

/* handle to the side board redraw cost, set up like game.c does */
static  int  simDrawCost  =  -1;


static void simSeedEverything( unsigned long  inSeed ) {
    chessSeed( inSeed );
    maxigin_randSeed( &levelsRand,
                      inSeed * 7UL + 1UL );
    maxigin_randSeed( &deckRand,
                      inSeed * 13UL + 2UL );
    maxigin_randSeed( &rollRand,
                      inSeed * 17UL + 3UL );
    maxigin_randSeed( &shopRand,
                      inSeed * 19UL + 4UL );
    maxigin_randSeed( &placementRand,
                      inSeed * 23UL + 5UL );
    }



/* resets the shop's variance-reducing roll state, so that runs
   are independent of each other */
static void simResetRolls( void ) {
    int  i;

    for( i = FIRST_ROLLABLE_RARITY;
         i < NUM_RARITIES;
         i ++ ) {
        rollReset( &( rarityRolls[ i ] ) );
        rollPoolReset( rarityPools[ i ] );
        }
    rollReset( &shopOnSaleRoll );
    }



/* returns the highest level reached (the level the player died on,
   or maxLevel if they reached it) */
static int runOneGame( unsigned long  inSeed,
                       BatchStats    *inStats ) {

    BoardState  boardState;
    int         currentLevel  =  0;
    char        alive         =  1;
    int         moneySpent    =  0;
    char        startedWithUnlock[ NUM_UNLOCKS ];
    int         i;

    for( i = 0;
         i < NUM_UNLOCKS;
         i ++ ) {
        startedWithUnlock[ i ] = unlockEnabled[ i ];
        }

    simSeedEverything( inSeed );
    simResetRolls();

    /* new game reset, same as game.c */
    costFullReset( simDrawCost );
    moneyForce( simStartingMoney );
    heartsReset();
    playerDeckSetupFresh();
    formationBackToStart();
    shopReset();

    if( logOn ) {
        printf( "\n========================================\n"
                "New run, seed %lu, unlocks %d%d%d\n"
                "========================================\n",
                inSeed,
                unlockEnabled[ 0 ],
                unlockEnabled[ 1 ],
                unlockEnabled[ 2 ] );
        }

    while( alive ) {

        int  matchResult;
        int  numMoves;
        int  moneyBefore;
        int  moneyAfter;

        if( currentLevel >= maxLevel ) {
            if( logOn ) {
                printf( "Reached max level %d, stopping run\n",
                        maxLevel );
                }
            inStats->numReachedMaxLevel ++;
            break;
            }

        if( logOn ) {
            printf( "\n--- Level %d  (hearts %d, money %d) ---\n",
                    currentLevel,
                    heartsCount,
                    moneyGetTotal() );
            }

        /* 1. arrange formation blind */
        playerArrangeFormation();

        /* 2 & 3. draw player pieces into formation, fill side board,
           then reveal enemy formation */
        getLevel( currentLevel,
                  &boardState,
                  CHESS_WHITE );

        sideBoardRedraw();
        playerDeckJustRefreshed();

        getLevel( currentLevel,
                  &boardState,
                  CHESS_BLACK );

        if( logOn ) {
            printf( "  Enemy formation revealed, initial draw:\n" );
            logBoard( &boardState );
            logSideBoard();
            logDeckContents();
            }

        /* 4. swap pieces around, maybe redraw */
        playerSwapFromSideBoard( &boardState );
        playerConsiderRedraws( &boardState,
                               simDrawCost );
        playerPlacePieces( &boardState );

        if( logOn ) {
            printf( "  Final formation for the match:\n" );
            logBoard( &boardState );
            }

        /* "spin": pieces on board go back to deck as played,
           side board pieces go back as unplayed */
        {
            int  y;
            int  x;

            for( y = 0;
                 y < BH;
                 y ++ ) {
                for( x = 0;
                     x < BW;
                     x ++ ) {

                    ChessPiece  p  =  boardState.grid[y][x];
                    ChessPiece  t  =  p & CHESS_TYPE_MASK;
                    ChessPiece  c  =  p & CHESS_COLOR_MASK;

                    if( t != noPiece
                        &&
                        t != king
                        &&
                        c == CHESS_WHITE ) {
                        playerDeckReturnPiecePlayed( p );
                        }
                    }
                }
            sideBoardReturnPieces();
        }

        /* 5. the match */
        moneyBefore = moneyGetTotal();

        matchResult = runMatch( &boardState,
                                &numMoves );

        moneyAfter = moneyGetTotal();

        inStats->matchResultCounts[ matchResult ] ++;
        inStats->totalMoves += numMoves;
        inStats->totalMatches ++;

        inStats->levelMatches[ currentLevel ] ++;
        if( matchResult == matchWhiteCheckmate ) {
            inStats->levelCheckmateWins[ currentLevel ] ++;
            }
        else if( matchIsPlayerWin( matchResult ) ) {
            inStats->levelOtherWins[ currentLevel ] ++;
            }

        if( logOn ) {
            printf( "  Match result: %s after %d moves, earned %d money\n",
                    matchResultNames[ matchResult ],
                    numMoves,
                    moneyAfter - moneyBefore );
            }

        /* 6. hearts and unlocks */
        if( matchResult == matchWhiteCheckmate ) {
            heartsGain();
            unlocksBeatLevel( currentLevel );
            }
        else if( ! matchIsPlayerWin( matchResult ) ) {
            heartsLose();
            }

        if( logOn ) {
            printf( "  Hearts now %d\n",
                    heartsCount );
            }

        if( heartsIsDead() ) {
            alive = 0;
            if( logOn ) {
                printf( "DEAD on level %d\n",
                        currentLevel );
                }
            break;
            }

        /* 7. the shop */
        playerDeckReshuffle();
        shopReroll();
        moneyAdd( simShopAllowance );

        moneyBefore = moneyGetTotal();

        playerShop();

        moneySpent += moneyBefore - moneyGetTotal();

        /* 8. next level */
        costResetIncrement( simDrawCost );
        costLevelIncrement( simDrawCost );
        playerDeckReshuffle();
        currentLevel ++;
        }

    inStats->numRuns ++;
    inStats->totalMaxLevel += currentLevel;
    inStats->reachedHistogram[ currentLevel ] ++;
    inStats->totalSpotsAtDeath   += formationGetNumNonKingSpots();
    inStats->totalDeckSizeAtDeath += playerDeckGetSize();
    inStats->totalMoneySpent     += moneySpent;

    for( i = 0;
         i < NUM_UNLOCKS;
         i ++ ) {
        if( unlockEnabled[ i ]
            &&
            ! startedWithUnlock[ i ] ) {
            inStats->numEarnedUnlock[ i ] ++;
            }
        }

    return currentLevel;
    }



/*
  =========================================================================
  Batch reporting
  =========================================================================
*/

static void printBatchReport( const char  *inBatchName,
                              BatchStats  *inStats ) {

    int   i;
    int   highest      =  0;
    int   cumulative   =  0;
    int   maxCount     =  1;
    int   medianLevel  =  0;
    long  totalRuns    =  inStats->numRuns;

    printf( "\n==========================================================\n" );
    printf( "Batch: %s  (%d runs)\n",
            inBatchName,
            inStats->numRuns );
    printf( "==========================================================\n" );

    if( totalRuns == 0 ) {
        printf( "No runs.\n" );
        return;
        }

    for( i = 0;
         i <= SIM_MAX_LEVEL_LIMIT;
         i ++ ) {
        if( inStats->reachedHistogram[ i ] > 0 ) {
            highest = i;
            if( inStats->reachedHistogram[ i ] > maxCount ) {
                maxCount = inStats->reachedHistogram[ i ];
                }
            }
        }

    printf( "\nHighest level reached (level the player died on):\n\n" );
    printf( "  Level   Runs   %%     Reached at least this level\n" );

    for( i = 0;
         i <= highest;
         i ++ ) {

        int  runsHere      =  inStats->reachedHistogram[ i ];
        int  reachedAtLeast;
        int  barLen;
        int  b;

        reachedAtLeast = (int)totalRuns - cumulative;

        if( cumulative < ( totalRuns + 1 ) / 2
            &&
            cumulative + runsHere >= ( totalRuns + 1 ) / 2 ) {
            medianLevel = i;
            }

        cumulative += runsHere;

        barLen = ( runsHere * 40 ) / maxCount;

        printf( "  %3d%s  %5d  %5.1f  %5.1f%%  ",
                i,
                ( i == maxLevel ? "+" : " " ),
                runsHere,
                ( 100.0 * runsHere ) / (double)totalRuns,
                ( 100.0 * reachedAtLeast ) / (double)totalRuns );

        for( b = 0;
             b < barLen;
             b ++ ) {
            printf( "#" );
            }
        printf( "\n" );
        }

    printf( "\nAverage highest level reached:  %.2f\n",
            (double)inStats->totalMaxLevel / (double)totalRuns );
    printf( "Median highest level reached:   %d\n",
            medianLevel );

    if( inStats->numReachedMaxLevel > 0 ) {
        printf( "Runs stopped at max level %d:  %d\n",
                maxLevel,
                inStats->numReachedMaxLevel );
        }

    {
        int  thresholds[ 4 ]  =  { 8, 16, 32, 64 };
        int  t;

        printf( "\n" );
        for( t = 0;
             t < 4;
             t ++ ) {
            int  count  =  0;
            for( i = thresholds[ t ];
                 i <= SIM_MAX_LEVEL_LIMIT;
                 i ++ ) {
                count += inStats->reachedHistogram[ i ];
                }
            printf( "Reached level %2d:  %5.1f%%   (%d runs)\n",
                    thresholds[ t ],
                    ( 100.0 * count ) / (double)totalRuns,
                    count );
            }
    }

    printf( "\nUnlocks earned during run (checkmate at/after level):\n" );
    for( i = 0;
         i < NUM_UNLOCKS;
         i ++ ) {
        printf( "  level %2d unlock:  %5.1f%%   (%d runs)\n",
                unlockAtLevel[ i ],
                ( 100.0 * inStats->numEarnedUnlock[ i ] ) / (double)totalRuns,
                inStats->numEarnedUnlock[ i ] );
        }

    printf( "\nMatch results (%ld matches, avg %.1f moves each):\n",
            inStats->totalMatches,
            ( inStats->totalMatches > 0 ?
              (double)inStats->totalMoves / (double)inStats->totalMatches :
              0.0 ) );
    for( i = 0;
         i < NUM_MATCH_RESULTS;
         i ++ ) {
        printf( "  %-30s %5.1f%%   (%ld)\n",
                matchResultNames[ i ],
                ( inStats->totalMatches > 0 ?
                  ( 100.0 * (double)inStats->matchResultCounts[ i ] )
                  / (double)inStats->totalMatches :
                  0.0 ),
                inStats->matchResultCounts[ i ] );
        }

    printf( "\nPer-level match outcomes (levels with at least 5 matches):\n" );
    printf( "  Level  Matches  Checkmate%%  OverrunWin%%  Loss%%\n" );
    for( i = 0;
         i <= highest;
         i ++ ) {
        int  n  =  inStats->levelMatches[ i ];

        if( n < 5 ) {
            continue;
            }
        printf( "  %3d%s  %7d  %9.1f%%  %10.1f%%  %5.1f%%\n",
                i,
                ( i == maxLevel ? "+" : " " ),
                n,
                ( 100.0 * inStats->levelCheckmateWins[ i ] ) / (double)n,
                ( 100.0 * inStats->levelOtherWins[ i ] ) / (double)n,
                ( 100.0 * ( n
                            - inStats->levelCheckmateWins[ i ]
                            - inStats->levelOtherWins[ i ] ) ) / (double)n );
        }

    printf( "\nAt death, average:  %.1f formation spots (non-king), "
            "%.1f deck pieces, %.1f money spent over run\n",
            (double)inStats->totalSpotsAtDeath / (double)totalRuns,
            (double)inStats->totalDeckSizeAtDeath / (double)totalRuns,
            (double)inStats->totalMoneySpent / (double)totalRuns );
    }



/*
  =========================================================================
  Batches
  =========================================================================
*/

/* sets up permanent unlocks for the start of a run
   inNumUnlocks is how many of the unlocks (in order) to grant */
static void simSetupUnlocks( int  inNumUnlocks ) {
    int  i;

    unlocksHardReset();

    for( i = 0;
         i < inNumUnlocks;
         i ++ ) {
        /* each call grants at most one unlock, the next locked one
           whose level requirement is met */
        unlocksBeatLevel( unlockAtLevel[ i ] );
        }
    }



static void runBatch( const char  *inBatchName,
                      int          inNumUnlocks,
                      int          inBatchIndex,
                      BatchStats  *inStats ) {

    int  i;
    int  progressStep  =  numPerBatch / 10;

    if( progressStep < 1 ) {
        progressStep = 1;
        }

    batchStatsClear( inStats );

    printf( "\n>>> Starting batch: %s  (%d runs)\n",
            inBatchName,
            numPerBatch );
    fflush( stdout );

    for( i = 0;
         i < numPerBatch;
         i ++ ) {

        unsigned long  seed;
        int            reached;

        simSetupUnlocks( inNumUnlocks );

        /* every batch uses the same sequence of seeds, so the batches
           see the same enemy formations, deck shuffles and shop rolls
           (until their decisions diverge).  This makes differences
           between batches much less noisy than independent seeds would */
        (void)inBatchIndex;
        seed = simBaseSeed + (unsigned long)i;

        reached = runOneGame( seed,
                              inStats );

        if( logOn ) {
            printf( "Run %d of batch '%s' reached level %d\n",
                    i + 1,
                    inBatchName,
                    reached );
            }
        else if( ( i + 1 ) % progressStep == 0
                 ||
                 i + 1 == numPerBatch ) {
            printf( "    ... %d / %d runs done (running average level "
                    "reached %.2f)\n",
                    i + 1,
                    numPerBatch,
                    (double)inStats->totalMaxLevel
                    / (double)inStats->numRuns );
            fflush( stdout );
            }
        }

    /* leave no permanent unlocks behind */
    unlocksHardReset();
    }



/*
  =========================================================================
  Settings file reading
  =========================================================================
*/

/* reads the first integer found in a file
   returns inDefault if the file can't be read or contains no number */
static int readIntFromFile( const char  *inFileName,
                            int          inDefault ) {

    FILE  *f      =  fopen( inFileName,
                            "r" );
    int    c;
    int    val    =  0;
    char   found  =  0;
    char   neg    =  0;

    if( f == NULL ) {
        return inDefault;
        }

    while( ( c = fgetc( f ) ) != EOF ) {

        if( c >= '0'
            &&
            c <= '9' ) {
            val = val * 10 + ( c - '0' );
            found = 1;
            }
        else if( c == '-'
                 &&
                 ! found ) {
            neg = 1;
            }
        else if( found ) {
            break;
            }
        }

    fclose( f );

    if( ! found ) {
        return inDefault;
        }

    if( neg ) {
        val = -val;
        }

    return val;
    }



/*
  =========================================================================
  main
  =========================================================================
*/

static void simInitModules( void ) {

    /* these init functions are called exactly once.
       They try to load sprites and sounds, which silently fail
       (returning -1 handles) under the null mingin implementation.
       Everything else they do (rng seeding, cost setup, etc.) works. */

    unlocksInit( 0,
                 -1 );
    chessInit();
    moneyInit( simStartingMoney,
               -1 );
    rollInit();
    formationInit( 0,
                   0 );
    levelsInit();
    deckInit();
    playerDeckInit();
    rarityInit();
    shopInit( 0,
              0,
              0,
              MAXIGIN_GAME_NATIVE_W / 2,
              MAXIGIN_GAME_NATIVE_H / 2 );
    heartsInit();
    sideBoardInit( 0,
                   0,
                   0 );

    /* redraw costs are 1, 2, 3, 6, 10, etc (same as game.c) */
    simDrawCost = costInit( 1,
                            1,
                            -1,
                            -1,
                            1,
                            0,
                            -1,
                            -1,
                            0 );
    }



int main( void ) {

    static  BatchStats  stats[ 4 ];

    static  const char  *batchNames[ 4 ] = {
        "No unlocks",
        "First unlock only (extra side board slot)",
        "First two unlocks (+ extra shop slot)",
        "All three unlocks (+ permanent sale slot)" };

    int  b;

    logOn       = (char)( readIntFromFile( "logOn.ini",
                                           0 ) == 1 );
    numPerBatch = readIntFromFile( "numPerBatch.ini",
                                   100 );
    maxLevel    = readIntFromFile( "maxLevel.ini",
                                   100 );

    if( numPerBatch < 1 ) {
        numPerBatch = 1;
        }
    if( maxLevel < 1 ) {
        maxLevel = 1;
        }
    if( maxLevel > SIM_MAX_LEVEL_LIMIT ) {
        maxLevel = SIM_MAX_LEVEL_LIMIT;
        }

    printf( "caSim: Chessamphetamine headless play-testing simulator\n" );
    printf( "  logOn = %d,  numPerBatch = %d,  maxLevel = %d\n",
            logOn,
            numPerBatch,
            maxLevel );
    printf( "  player knobs:  maxRedrawCost = %d,  rerollMinLeftover = %d,  "
            "maxRerolls = %d,  saveForSpotMargin = %d,  "
            "minBuyValuePercent = %d,  moneyReserve = %d,  "
            "placementTrials = %d,  placementDepth = %d\n",
            playerMaxRedrawCost,
            playerRerollMinLeftover,
            playerMaxRerolls,
            playerSaveForSpotMargin,
            playerMinBuyValuePercent,
            playerMoneyReserve,
            playerPlacementTrials,
            playerPlacementDepth );
    fflush( stdout );

    simInitModules();

    for( b = 0;
         b < 4;
         b ++ ) {
        runBatch( batchNames[ b ],
                  b,
                  b,
                  &( stats[ b ] ) );

        /* report each batch as soon as it's done, since a full run
           can take a while */
        printBatchReport( batchNames[ b ],
                          &( stats[ b ] ) );
        fflush( stdout );
        }

    /* summary table */
    printf( "\n==========================================================\n" );
    printf( "Summary\n" );
    printf( "==========================================================\n" );
    printf( "  %-44s %8s %8s %8s %8s %8s\n",
            "Batch",
            "AvgLvl",
            ">=8",
            ">=16",
            ">=32",
            ">=64" );
    for( b = 0;
         b < 4;
         b ++ ) {
        int  thresholds[ 4 ]  =  { 8, 16, 32, 64 };
        int  counts    [ 4 ];
        int  t;
        int  i;

        for( t = 0;
             t < 4;
             t ++ ) {
            counts[ t ] = 0;
            for( i = thresholds[ t ];
                 i <= SIM_MAX_LEVEL_LIMIT;
                 i ++ ) {
                counts[ t ] += stats[ b ].reachedHistogram[ i ];
                }
            }

        printf( "  %-44s %8.2f %7.1f%% %7.1f%% %7.1f%% %7.1f%%\n",
                batchNames[ b ],
                (double)stats[ b ].totalMaxLevel
                / (double)stats[ b ].numRuns,
                ( 100.0 * counts[ 0 ] ) / (double)stats[ b ].numRuns,
                ( 100.0 * counts[ 1 ] ) / (double)stats[ b ].numRuns,
                ( 100.0 * counts[ 2 ] ) / (double)stats[ b ].numRuns,
                ( 100.0 * counts[ 3 ] ) / (double)stats[ b ].numRuns );
        }

    return 0;
    }
