/*
  editor.c

  See editor.h

  Sections:
     state and small helpers
     buffers (open, switch, kill, save)
     cursor movement
     mark / region / kill ring
     typing (electric chars, newline, tab, hungry delete)
     dabbrev completion
     incremental search, query replace
     compile, next-error, debug
     minibuffer and completion lists
     command table (M-x) and key bindings
*/

#include "editor.h"
#include "syntax.h"
#include "indent.h"
#include "format.h"
#include "config.h"
#include "compile.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>


Editor  ed;


typedef void (*CmdFn)( void );


/* sentinel "commands" for lastCommand tracking */
static void cmdSelfInsert( void );
static void cmdKillLine( void );
static void cmdKillWholeLine( void );
static void cmdKillRegion( void );
static void cmdKillWordForward( void );
static void cmdKillWordBackward( void );
static void cmdCopyRegion( void );
static void cmdYank( void );
static void cmdYankPop( void );
static void cmdDabbrev( void );
static void cmdRecenter( void );

static CmdFn  lastCmd       =  NULL;
static CmdFn  thisCmd       =  NULL;
static int    typedInGroup  =  0;
/* shift held during the current movement command */
static int    shiftSelect   =  0;
static int    shiftSelecting =  0;



/* ------------------------------------------------------------------
   State and small helpers
   ------------------------------------------------------------------ */

/* bounded copy that always terminates */
static void scopy( char        *outDest,
                   const char  *inSrc,
                   size_t       inSize ) {
    size_t  i  =  0;

    while( i + 1 < inSize && inSrc[ i ] != '\0' ) {
        outDest[ i ] = inSrc[ i ];
        i ++;
        }
    outDest[ i ] = '\0';
    }



static char *xstrdup( const char  *inS ) {
    char  *s  =  (char *)malloc( strlen( inS ) + 1 );

    strcpy( s, inS );
    return s;
    }



void ed_message( const char  *inFormat,
                 ... ) {
    va_list  ap;

    va_start( ap, inFormat );
    vsprintf( ed.message, inFormat, ap );
    va_end( ap );
    ed.messageIsError = 0;
    }



void ed_errorMessage( const char  *inFormat,
                      ... ) {
    va_list  ap;

    va_start( ap, inFormat );
    vsprintf( ed.message, inFormat, ap );
    va_end( ap );
    ed.messageIsError = 1;
    }



TextBuffer *ed_cur( void ) {
    return ed.buffers[ ed.current ];
    }

#define B()  ( ed.buffers[ ed.current ] )



static void getCur( int  *outLine,
                    int  *outCol ) {
    TextBuffer  *tb  =  B();

    tb_getMarker( tb, tb->cursorMarker, outLine, outCol );
    }



static void setCurNoGoal( int  inLine,
                          int  inCol ) {
    TextBuffer  *tb  =  B();

    tb_clampPos( tb, &inLine, &inCol );
    tb_setMarker( tb, tb->cursorMarker, inLine, inCol );
    tb->cursorLine = inLine;
    tb->cursorCol = inCol;
    }



static void setCur( int  inLine,
                    int  inCol ) {
    setCurNoGoal( inLine, inCol );
    /* a display column, so C-n / C-p keep their place across tabs */
    B()->goalCol = tb_visCol( &( B()->lines[ B()->cursorLine ] ),
                              B()->cursorCol, config.tabWidth );
    }



static void syncCur( void ) {
    TextBuffer  *tb  =  B();

    tb_getMarker( tb, tb->cursorMarker, &( tb->cursorLine ),
                  &( tb->cursorCol ) );
    }



static int checkWritable( void ) {
    if( B()->readOnly ) {
        ed_errorMessage( "Buffer is read-only" );
        return 0;
        }
    return 1;
    }



static int curLine( void ) {
    int  l;
    int  c;

    getCur( &l, &c );
    return l;
    }



static int curCol( void ) {
    int  l;
    int  c;

    getCur( &l, &c );
    return c;
    }



static Line *lineAt( int  inLine ) {
    return &( B()->lines[ inLine ] );
    }



/* the token type at a position (TK_COMMENT, TK_STRING, ...) or 0 if in
   plain code / whitespace.  A position counts as inside a token if it
   is after its first char and before its end (or the token is open). */
static int tokenTypeAt( TextBuffer  *inTB,
                        int          inLine,
                        int          inCol ) {
    static TokenList  tl;
    static int        init  =  0;
    Line             *l     =  &( inTB->lines[ inLine ] );
    int               k;

    if( ! init ) {
        syn_initTokenList( &tl );
        init = 1;
        }
    syn_lexLine( l->text, l->len, syn_getLineState( inTB, inLine ), &tl );
    for( k = 0;
         k < tl.num;
         k ++ ) {
        Token  *t  =  &( tl.tokens[ k ] );

        if( t->type != TK_COMMENT && t->type != TK_STRING
            && t->type != TK_CHAR ) {
            continue;
            }
        if( inCol > t->start
            &&
            ( inCol < t->start + t->len || t->open ) ) {
            return t->type;
            }
        }
    return 0;
    }



/* ------------------------------------------------------------------
   Buffers
   ------------------------------------------------------------------ */

static void touchMRU( int  inIndex ) {
    int  i;
    int  pos  =  -1;

    for( i = 0;
         i < ed.numBuffers;
         i ++ ) {
        if( ed.mru[ i ] == inIndex ) {
            pos = i;
            break;
            }
        }
    if( pos < 0 ) {
        return;
        }
    memmove( &( ed.mru[ 1 ] ), &( ed.mru[ 0 ] ), (size_t)pos * sizeof( int ) );
    ed.mru[ 0 ] = inIndex;
    }



static const char *baseName( const char  *inPath ) {
    const char  *s  =  strrchr( inPath, '/' );

    return s != NULL ? s + 1 : inPath;
    }



static void setUniqueName( TextBuffer  *inTB,
                           const char  *inBase ) {
    char  name[ 512 ];
    int   n  =  1;
    int   clash;
    int   i;

    sprintf( name, "%.480s", inBase );
    do {
        clash = 0;
        for( i = 0;
             i < ed.numBuffers;
             i ++ ) {
            if( ed.buffers[ i ] != inTB && ed.buffers[ i ]->name != NULL
                &&
                strcmp( ed.buffers[ i ]->name, name ) == 0 ) {
                clash = 1;
                }
            }
        if( clash ) {
            n ++;
            sprintf( name, "%.480s<%d>", inBase, n );
            }
        } while( clash );

    free( inTB->name );
    inTB->name = xstrdup( name );
    }



static int addBuffer( TextBuffer  *inTB ) {
    ed.buffers = (TextBuffer **)realloc( ed.buffers,
                                         (size_t)( ed.numBuffers + 1 )
                                         * sizeof( TextBuffer * ) );
    ed.mru = (int *)realloc( ed.mru, (size_t)( ed.numBuffers + 1 )
                             * sizeof( int ) );
    ed.buffers[ ed.numBuffers ] = inTB;
    ed.mru[ ed.numBuffers ] = ed.numBuffers;
    ed.numBuffers ++;
    return ed.numBuffers - 1;
    }



static long fileMTime( const char  *inPath ) {
    struct stat  st;

    if( stat( inPath, &st ) != 0 ) {
        return 0;
        }
    return (long)st.st_mtime;
    }



void ed_switchToBuffer( int  inIndex ) {
    if( inIndex < 0 || inIndex >= ed.numBuffers ) {
        return;
        }
    ed.current = inIndex;
    touchMRU( inIndex );
    syncCur();
    }



/* path as shown / used for %f:  relative to cwd when inside it */
static void displayPath( const char  *inPath,
                         char        *outPath,
                         int          inSize ) {
    char    cwd[ 1024 ];
    size_t  n;

    if( getcwd( cwd, sizeof( cwd ) ) != NULL ) {
        n = strlen( cwd );
        if( strncmp( inPath, cwd, n ) == 0 && inPath[ n ] == '/' ) {
            inPath += n + 1;
            }
        }
    strncpy( outPath, inPath, (size_t)inSize - 1 );
    outPath[ inSize - 1 ] = '\0';
    }



static int findBufferByPath( const char  *inPath ) {
    char  want[ PATH_MAX ];
    char  have[ PATH_MAX ];
    int   i;

    if( realpath( inPath, want ) == NULL ) {
        scopy( want, inPath, sizeof( want ) );
        want[ sizeof( want ) - 1 ] = '\0';
        }
    for( i = 0;
         i < ed.numBuffers;
         i ++ ) {
        TextBuffer  *tb  =  ed.buffers[ i ];

        if( tb->path == NULL ) {
            continue;
            }
        if( realpath( tb->path, have ) == NULL ) {
            scopy( have, tb->path, sizeof( have ) );
            have[ sizeof( have ) - 1 ] = '\0';
            }
        if( strcmp( want, have ) == 0 ) {
            return i;
            }
        }
    return -1;
    }



/* opens (or switches to) a file.  Nonexistent files make a new empty
   buffer with that path.  Returns buffer index or -1. */
static int openFile( const char  *inPath,
                     int          inSwitch ) {
    char         path[ 1024 ];
    TextBuffer  *tb;
    int          tabs  =  0;
    int          idx;
    struct stat  st;

    if( inPath[ 0 ] == '~' && inPath[ 1 ] == '/' && getenv( "HOME" ) ) {
        sprintf( path, "%.500s%.500s", getenv( "HOME" ), inPath + 1 );
        }
    else {
        displayPath( inPath, path, sizeof( path ) );
        }

    if( stat( path, &st ) == 0 && S_ISDIR( st.st_mode ) ) {
        ed_errorMessage( "%s is a directory", path );
        return -1;
        }

    idx = findBufferByPath( path );
    if( idx >= 0 ) {
        if( inSwitch ) {
            ed_switchToBuffer( idx );
            }
        return idx;
        }

    tb = tb_load( path, config.tabWidth, tb_isCPath( path ), &tabs );
    if( tb == NULL ) {
        if( stat( path, &st ) == 0 ) {
            ed_errorMessage( "Can't read %s", path );
            return -1;
            }
        tb = tb_create();
        tb->path = xstrdup( path );
        ed_message( "(New file)" );
        }
    else {
        tb->fileMTime = fileMTime( path );
        if( tabs > 0 ) {
            ed_message( "Expanded %d tab%s to spaces (buffer modified)",
                        tabs, tabs == 1 ? "" : "s" );
            }
        else {
            ed_message( "%s:  %d lines", path, tb->numLines );
            }
        if( access( path, W_OK ) != 0 ) {
            tb->readOnly = 1;
            }
        }
    tb->isC = tb_isCPath( path );
    idx = addBuffer( tb );
    setUniqueName( tb, baseName( path ) );
    if( tb->numLines > 20000 ) {
        /* don't hold up showing the file;  ed_tick does it */
        tb->needsTypeScan = 1;
        }
    else {
        syn_scanTypedefs( tb );
        }

    if( inSwitch ) {
        ed_switchToBuffer( idx );
        }
    return idx;
    }



static int makeScratch( const char  *inName,
                        const char  *inText ) {
    TextBuffer  *tb  =  tb_create();
    int          idx;

    if( inText != NULL ) {
        tb_setText( tb, inText, (int)strlen( inText ) );
        }
    idx = addBuffer( tb );
    setUniqueName( tb, inName );
    return idx;
    }



static int saveBuffer( TextBuffer  *inTB ) {
    if( inTB->path == NULL ) {
        return 0;
        }
    if( ! tb_save( inTB, inTB->path ) ) {
        ed_errorMessage( "Error writing %s", inTB->path );
        return 0;
        }
    inTB->fileMTime = fileMTime( inTB->path );
    inTB->readOnly = 0;
    syn_scanTypedefs( inTB );
    ed_message( "Wrote %s", inTB->path );
    return 1;
    }



int ed_anyModified( void ) {
    int  i;

    for( i = 0;
         i < ed.numBuffers;
         i ++ ) {
        if( ed.buffers[ i ]->path != NULL && tb_isModified( ed.buffers[ i ] ) ) {
            return 1;
            }
        }
    return 0;
    }



static void killBufferNow( int  inIndex ) {
    TextBuffer  *tb  =  ed.buffers[ inIndex ];
    int          i;
    int          k;

    comp_forgetBuffer( tb );
    tb_destroy( tb );

    memmove( &( ed.buffers[ inIndex ] ), &( ed.buffers[ inIndex + 1 ] ),
             (size_t)( ed.numBuffers - inIndex - 1 ) * sizeof( TextBuffer * ) );
    /* fix MRU:  drop entry, renumber */
    k = 0;
    for( i = 0;
         i < ed.numBuffers;
         i ++ ) {
        if( ed.mru[ i ] == inIndex ) {
            continue;
            }
        ed.mru[ k ] = ed.mru[ i ] > inIndex ? ed.mru[ i ] - 1 : ed.mru[ i ];
        k ++;
        }
    ed.numBuffers --;

    if( ed.numBuffers == 0 ) {
        makeScratch( "*scratch*", NULL );
        }
    ed.current = ed.mru[ 0 ];
    syncCur();
    }



/* ------------------------------------------------------------------
   Cursor movement
   ------------------------------------------------------------------ */

static void deactivateMark( void ) {
    B()->markActive = 0;
    }



static void setMarkHere( void ) {
    TextBuffer  *tb  =  B();
    int          l;
    int          c;

    getCur( &l, &c );
    tb_setMarker( tb, tb->markMarker, l, c );
    tb->markActive = 1;
    tb->markSet = 1;
    }



/* called at the start of each movement command */
static void beginMove( void ) {
    if( shiftSelect ) {
        if( ! B()->markActive || ! shiftSelecting ) {
            setMarkHere();
            }
        shiftSelecting = 1;
        }
    else if( shiftSelecting ) {
        deactivateMark();
        shiftSelecting = 0;
        }
    }



static void cmdForwardChar( void ) {
    int  l;
    int  c;

    beginMove();
    getCur( &l, &c );
    if( c < lineAt( l )->len ) {
        c ++;
        }
    else if( l < B()->numLines - 1 ) {
        l ++;
        c = 0;
        }
    setCur( l, c );
    }



static void cmdBackwardChar( void ) {
    int  l;
    int  c;

    beginMove();
    getCur( &l, &c );
    if( c > 0 ) {
        c --;
        }
    else if( l > 0 ) {
        l --;
        c = lineAt( l )->len;
        }
    setCur( l, c );
    }



static void moveLines( int  inDelta ) {
    int  l;
    int  c;

    beginMove();
    getCur( &l, &c );
    l += inDelta;
    if( l < 0 ) {
        l = 0;
        }
    if( l >= B()->numLines ) {
        l = B()->numLines - 1;
        }
    c = tb_colAtVis( lineAt( l ), B()->goalCol, config.tabWidth );
    setCurNoGoal( l, c );
    }



static void cmdNextLine( void ) {
    moveLines( 1 );
    }



static void cmdPrevLine( void ) {
    moveLines( -1 );
    }



static void cmdLineStart( void ) {
    beginMove();
    setCur( curLine(), 0 );
    }



static void cmdLineEnd( void ) {
    beginMove();
    setCur( curLine(), lineAt( curLine() )->len );
    }



static void cmdBackToIndent( void ) {
    beginMove();
    setCur( curLine(), tb_lineIndent( B(), curLine() ) );
    }



static void cmdSmartHome( void ) {
    int  l;
    int  c;
    int  ind;

    beginMove();
    getCur( &l, &c );
    ind = tb_lineIndent( B(), l );
    setCur( l, ( c == ind ) ? 0 : ind );
    }



static int isWordChar( char  inC ) {
    return syn_isIdentChar( inC ) || ( inC & 0x80 );
    }



static void forwardWordPos( int  *ioLine,
                            int  *ioCol ) {
    int  l  =  *ioLine;
    int  c  =  *ioCol;

    /* skip non-word, then word */
    while( 1 ) {
        Line  *ln  =  lineAt( l );

        while( c < ln->len && ! isWordChar( ln->text[ c ] ) ) {
            c ++;
            }
        if( c < ln->len ) {
            break;
            }
        if( l >= B()->numLines - 1 ) {
            break;
            }
        l ++;
        c = 0;
        }
    while( c < lineAt( l )->len && isWordChar( lineAt( l )->text[ c ] ) ) {
        c ++;
        }
    *ioLine = l;
    *ioCol = c;
    }



static void backwardWordPos( int  *ioLine,
                             int  *ioCol ) {
    int  l  =  *ioLine;
    int  c  =  *ioCol;

    while( 1 ) {
        Line  *ln  =  lineAt( l );

        while( c > 0 && ! isWordChar( ln->text[ c - 1 ] ) ) {
            c --;
            }
        if( c > 0 ) {
            break;
            }
        if( l == 0 ) {
            break;
            }
        l --;
        c = lineAt( l )->len;
        }
    while( c > 0 && isWordChar( lineAt( l )->text[ c - 1 ] ) ) {
        c --;
        }
    *ioLine = l;
    *ioCol = c;
    }



static void cmdForwardWord( void ) {
    int  l;
    int  c;

    beginMove();
    getCur( &l, &c );
    forwardWordPos( &l, &c );
    setCur( l, c );
    }



static void cmdBackwardWord( void ) {
    int  l;
    int  c;

    beginMove();
    getCur( &l, &c );
    backwardWordPos( &l, &c );
    setCur( l, c );
    }



static void cmdBufferStart( void ) {
    if( ! shiftSelect && ! B()->markActive ) {
        setMarkHere();
        deactivateMark();
        }
    beginMove();
    setCur( 0, 0 );
    }



static void cmdBufferEnd( void ) {
    int  last  =  B()->numLines - 1;

    if( ! shiftSelect && ! B()->markActive ) {
        setMarkHere();
        deactivateMark();
        }
    beginMove();
    setCur( last, lineAt( last )->len );
    }



int ed_lineRows( TextBuffer  *inTB,
                 int          inLine ) {
    Line  *l    =  &( inTB->lines[ inLine ] );
    int    len;

    if( ed.wrapCols <= 0 || l->len <= ed.wrapCols / config.tabWidth ) {
        /* can't be wider than the window even if it's all tabs */
        return 1;
        }
    len = tb_visCol( l, l->len, config.tabWidth );
    if( len <= ed.wrapCols ) {
        return 1;
        }
    return ( len + ed.wrapCols - 1 ) / ed.wrapCols;
    }



/* scrolls the view a screen (less two rows) down (inDir 1) or up (-1),
   counting wrapped rows;  the cursor moves along by as many lines */
static void pageScroll( int  inDir ) {
    int  page  =  ed.visibleRows - 2;
    int  top   =  (int)( B()->targetScrollY + 0.5f );
    int  t     =  top;
    int  acc   =  0;

    if( page < 1 ) {
        page = 1;
        }
    if( inDir > 0 ) {
        while( t < B()->numLines - 1
               && acc + ed_lineRows( B(), t ) <= page ) {
            acc += ed_lineRows( B(), t );
            t ++;
            }
        if( t == top && t < B()->numLines - 1 ) {
            t ++;
            }
        }
    else {
        while( t > 0 && acc + ed_lineRows( B(), t - 1 ) <= page ) {
            t --;
            acc += ed_lineRows( B(), t );
            }
        if( t == top && t > 0 ) {
            t --;
            }
        }
    moveLines( t - top );
    B()->targetScrollY = (float)t;
    }



static void cmdPageDown( void ) {
    pageScroll( 1 );
    }



static void cmdPageUp( void ) {
    pageScroll( -1 );
    }



static void cmdRecenter( void ) {
    ed.recenterRequest = 1;
    }



static int lineIsBlankAt( int  inLine ) {
    return tb_lineIsBlank( B(), inLine );
    }



/* M-} and M-{ :  move over runs of non-blank lines */
static void cmdForwardParagraph( void ) {
    int  l  =  curLine();

    beginMove();
    while( l < B()->numLines - 1 && lineIsBlankAt( l ) ) {
        l ++;
        }
    while( l < B()->numLines - 1 && ! lineIsBlankAt( l ) ) {
        l ++;
        }
    setCur( l, 0 );
    }



static void cmdBackwardParagraph( void ) {
    int  l  =  curLine();

    beginMove();
    while( l > 0 && lineIsBlankAt( l ) ) {
        l --;
        }
    while( l > 0 && ! lineIsBlankAt( l ) ) {
        l --;
        }
    setCur( l, 0 );
    }



/* C-M-f / C-M-b:  jump across a bracketed group */
static void cmdForwardSexp( void ) {
    int  l;
    int  c;
    int  ml;
    int  mc;

    beginMove();
    getCur( &l, &c );
    /* skip to next bracket on this line */
    while( c < lineAt( l )->len
           &&
           strchr( "([{", lineAt( l )->text[ c ] ) == NULL ) {
        if( strchr( ")]}", lineAt( l )->text[ c ] ) != NULL ) {
            break;
            }
        c ++;
        }
    if( c < lineAt( l )->len
        &&
        ind_findMatchingBracket( B(), l, c, 100000, &ml, &mc ) ) {
        setCur( ml, mc + 1 );
        }
    else {
        forwardWordPos( &l, &c );
        setCur( l, c );
        }
    }



static void cmdBackwardSexp( void ) {
    int  l;
    int  c;
    int  ml;
    int  mc;

    beginMove();
    getCur( &l, &c );
    while( c > 0
           &&
           lineAt( l )->text[ c - 1 ] == ' ' ) {
        c --;
        }
    if( c > 0
        &&
        strchr( ")]}", lineAt( l )->text[ c - 1 ] ) != NULL
        &&
        ind_findMatchingBracket( B(), l, c - 1, 100000, &ml, &mc ) ) {
        setCur( ml, mc );
        }
    else {
        getCur( &l, &c );
        backwardWordPos( &l, &c );
        setCur( l, c );
        }
    }



static void gotoLine( int  inLine ) {
    if( ! B()->markActive ) {
        setMarkHere();
        deactivateMark();
        }
    setCur( inLine, 0 );
    setCur( inLine, tb_lineIndent( B(), curLine() ) );
    ed.recenterRequest = 1;
    }



/* ------------------------------------------------------------------
   Mark, region, kill ring
   ------------------------------------------------------------------ */

#define KILL_RING_SIZE  32

static char  *killRing[ KILL_RING_SIZE ];
static int    killRingLen[ KILL_RING_SIZE ];
static int    numKills     =  0;
/* for M-y */
static int    yankIndex    =  0;
static int    yankStartLine, yankStartCol;



int ed_getRegion( int  *outStartLine,
                  int  *outStartCol,
                  int  *outEndLine,
                  int  *outEndCol ) {
    TextBuffer  *tb  =  B();
    int          ml;
    int          mc;
    int          cl;
    int          cc;

    if( ! tb->markActive ) {
        return 0;
        }
    tb_getMarker( tb, tb->markMarker, &ml, &mc );
    getCur( &cl, &cc );
    if( tb_comparePos( ml, mc, cl, cc ) <= 0 ) {
        *outStartLine = ml;
        *outStartCol = mc;
        *outEndLine = cl;
        *outEndCol = cc;
        }
    else {
        *outStartLine = cl;
        *outStartCol = cc;
        *outEndLine = ml;
        *outEndCol = mc;
        }
    return 1;
    }



/* region even if not active (mark exists), like emacs C-w */
static void getMarkRegion( int  *outSL,
                           int  *outSC,
                           int  *outEL,
                           int  *outEC ) {
    TextBuffer  *tb      =  B();
    int          active  =  tb->markActive;

    tb->markActive = 1;
    ed_getRegion( outSL, outSC, outEL, outEC );
    tb->markActive = active;
    }



/* PRIMARY selection access (SDL 2.26+;  older SDL has none) */
static char *getPrimaryText( void ) {
#if SDL_VERSION_ATLEAST( 2, 26, 0 )
    return SDL_GetPrimarySelectionText();
#else
    return NULL;
#endif
    }



static void setPrimaryText( const char  *inText ) {
#if SDL_VERSION_ATLEAST( 2, 26, 0 )
    SDL_SetPrimarySelectionText( inText );
#else
    (void)inText;
#endif
    }



static void pushKill( const char  *inText,
                      int          inLen,
                      int          inAppend,
                      int          inPrepend ) {
    if( inAppend && numKills > 0 ) {
        char  *old  =  killRing[ 0 ];
        int    ol   =  killRingLen[ 0 ];
        char  *n    =  (char *)malloc( (size_t)( ol + inLen + 1 ) );

        if( inPrepend ) {
            memcpy( n, inText, (size_t)inLen );
            memcpy( n + inLen, old, (size_t)ol );
            }
        else {
            memcpy( n, old, (size_t)ol );
            memcpy( n + ol, inText, (size_t)inLen );
            }
        n[ ol + inLen ] = '\0';
        free( old );
        killRing[ 0 ] = n;
        killRingLen[ 0 ] = ol + inLen;
        }
    else {
        if( numKills == KILL_RING_SIZE ) {
            free( killRing[ KILL_RING_SIZE - 1 ] );
            numKills --;
            }
        memmove( &( killRing[ 1 ] ), &( killRing[ 0 ] ),
                 (size_t)numKills * sizeof( char * ) );
        memmove( &( killRingLen[ 1 ] ), &( killRingLen[ 0 ] ),
                 (size_t)numKills * sizeof( int ) );
        killRing[ 0 ] = (char *)malloc( (size_t)inLen + 1 );
        memcpy( killRing[ 0 ], inText, (size_t)inLen );
        killRing[ 0 ][ inLen ] = '\0';
        killRingLen[ 0 ] = inLen;
        numKills ++;
        }
    SDL_SetClipboardText( killRing[ 0 ] );
    }



static int lastWasKill( void ) {
    return lastCmd == cmdKillLine || lastCmd == cmdKillWholeLine
           || lastCmd == cmdKillRegion
           || lastCmd == cmdKillWordForward || lastCmd == cmdKillWordBackward
           || lastCmd == cmdCopyRegion;
    }



static void killRange( int  inSL,
                       int  inSC,
                       int  inEL,
                       int  inEC,
                       int  inPrepend ) {
    int    len;
    char  *text;

    text = tb_getText( B(), inSL, inSC, inEL, inEC, &len );
    pushKill( text, len, lastWasKill(), inPrepend );
    free( text );
    tb_delete( B(), inSL, inSC, inEL, inEC );
    setCur( inSL, inSC );
    }



static void cmdSetMark( void ) {
    setMarkHere();
    shiftSelecting = 0;
    ed_message( "Mark set" );
    }



static void cmdExchangePointMark( void ) {
    TextBuffer  *tb  =  B();
    int          ml;
    int          mc;
    int          cl;
    int          cc;

    tb_getMarker( tb, tb->markMarker, &ml, &mc );
    getCur( &cl, &cc );
    tb_setMarker( tb, tb->markMarker, cl, cc );
    setCur( ml, mc );
    tb->markActive = 1;
    }



static void cmdSelectAll( void ) {
    int  last  =  B()->numLines - 1;

    setCur( 0, 0 );
    tb_setMarker( B(), B()->markMarker, last, lineAt( last )->len );
    B()->markSet = 1;
    B()->markActive = 1;
    shiftSelecting = 0;
    }



static void cmdKillRegion( void ) {
    int  sl;
    int  sc;
    int  el;
    int  ec;

    if( ! checkWritable() ) {
        return;
        }
    getMarkRegion( &sl, &sc, &el, &ec );
    killRange( sl, sc, el, ec, 0 );
    deactivateMark();
    }



static void cmdCopyRegion( void ) {
    int    sl;
    int    sc;
    int    el;
    int    ec;
    int    len;
    char  *text;

    getMarkRegion( &sl, &sc, &el, &ec );
    text = tb_getText( B(), sl, sc, el, ec, &len );
    pushKill( text, len, 0, 0 );
    free( text );
    deactivateMark();
    ed_message( "Copied %d characters", len );
    }



static void cmdKillLine( void ) {
    int  l;
    int  c;

    if( ! checkWritable() ) {
        return;
        }
    getCur( &l, &c );
    if( c == lineAt( l )->len ) {
        if( l < B()->numLines - 1 ) {
            killRange( l, c, l + 1, 0, 0 );
            }
        }
    else if( c == 0 ) {
        /* kill-whole-line at column 0 */
        if( l < B()->numLines - 1 ) {
            killRange( l, 0, l + 1, 0, 0 );
            }
        else {
            killRange( l, 0, l, lineAt( l )->len, 0 );
            }
        }
    else {
        killRange( l, c, l, lineAt( l )->len, 0 );
        }
    }



/* kills the cursor's whole line (and its newline) no matter where the
   cursor is in it, without moving first, so repeated kills keep
   appending to the same kill ring entry (emacs kill-whole-line) */
static void cmdKillWholeLine( void ) {
    int  l;
    int  c;

    if( ! checkWritable() ) {
        return;
        }
    getCur( &l, &c );
    if( l < B()->numLines - 1 ) {
        killRange( l, 0, l + 1, 0, 0 );
        }
    else if( l > 0 ) {
        /* last line:  take the newline before it instead */
        killRange( l - 1, lineAt( l - 1 )->len, l, lineAt( l )->len, 0 );
        setCur( l - 1, 0 );
        }
    else {
        killRange( l, 0, l, lineAt( l )->len, 0 );
        }
    }



static void cmdKillWordForward( void ) {
    int  l;
    int  c;
    int  el;
    int  ec;

    if( ! checkWritable() ) {
        return;
        }
    getCur( &l, &c );
    el = l;
    ec = c;
    forwardWordPos( &el, &ec );
    killRange( l, c, el, ec, 0 );
    }



static void cmdKillWordBackward( void ) {
    int  l;
    int  c;
    int  sl;
    int  sc;

    if( ! checkWritable() ) {
        return;
        }
    getCur( &l, &c );
    sl = l;
    sc = c;
    backwardWordPos( &sl, &sc );
    killRange( sl, sc, l, c, 1 );
    }



static void insertAtCursor( const char  *inText,
                            int          inLen ) {
    int  l;
    int  c;
    int  el;
    int  ec;

    getCur( &l, &c );
    tb_insert( B(), l, c, inText, inLen, &el, &ec );
    setCur( el, ec );
    }



static void cmdYank( void ) {
    char  *clip;
    int    l;
    int    c;

    if( ! checkWritable() ) {
        return;
        }
    /* text copied in another program takes priority */
    clip = SDL_GetClipboardText();
    if( clip != NULL && clip[ 0 ] != '\0'
        &&
        ( numKills == 0 || strcmp( clip, killRing[ 0 ] ) != 0 ) ) {
        pushKill( clip, (int)strlen( clip ), 0, 0 );
        }
    if( clip != NULL ) {
        SDL_free( clip );
        }
    if( numKills == 0 ) {
        ed_errorMessage( "Kill ring is empty" );
        return;
        }
    getCur( &l, &c );
    yankStartLine = l;
    yankStartCol = c;
    yankIndex = 0;
    tb_setMarker( B(), B()->markMarker, l, c );
    B()->markSet = 1;
    insertAtCursor( killRing[ 0 ], killRingLen[ 0 ] );
    }



static void cmdYankPop( void ) {
    int  l;
    int  c;

    if( lastCmd != cmdYank && lastCmd != cmdYankPop ) {
        ed_errorMessage( "Previous command was not a yank" );
        return;
        }
    if( numKills < 2 ) {
        return;
        }
    getCur( &l, &c );
    tb_delete( B(), yankStartLine, yankStartCol, l, c );
    yankIndex = ( yankIndex + 1 ) % numKills;
    setCur( yankStartLine, yankStartCol );
    insertAtCursor( killRing[ yankIndex ], killRingLen[ yankIndex ] );
    }



/* deletes the active region (delete-selection-mode);  returns 1 if it
   did */
static int deleteActiveRegion( void ) {
    int  sl;
    int  sc;
    int  el;
    int  ec;

    if( ! ed_getRegion( &sl, &sc, &el, &ec ) ) {
        return 0;
        }
    deactivateMark();
    shiftSelecting = 0;
    if( tb_comparePos( sl, sc, el, ec ) == 0 ) {
        return 0;
        }
    tb_delete( B(), sl, sc, el, ec );
    setCur( sl, sc );
    return 1;
    }



/* ------------------------------------------------------------------
   Indentation helpers
   ------------------------------------------------------------------ */

/* re-indents a line, keeping the cursor on the same text (or moving it
   to the indentation if it was inside it) */
static void reindentLine( int  inLine ) {
    int  l;
    int  c;
    int  oldInd  =  tb_lineIndent( B(), inLine );
    int  want;

    if( ! B()->isC ) {
        /* plain text:  indentation is the user's */
        return;
        }
    want = ind_computeIndent( B(), inLine );
    if( want < 0 ) {
        want = ind_commentIndent( B(), inLine );
        }
    if( want < 0 ) {
        return;
        }
    getCur( &l, &c );
    tb_setLineIndent( B(), inLine, want );
    if( l == inLine ) {
        if( c <= oldInd ) {
            setCur( l, want );
            }
        else {
            setCur( l, c - oldInd + want );
            }
        }
    }



/* indentation for a fresh (empty) line */
static int newLineIndent( int  inLine ) {
    int  want;

    if( ! B()->isC ) {
        /* plain text:  like the previous non-blank line */
        int  p  =  inLine - 1;

        while( p >= 0 && tb_lineIsBlank( B(), p ) ) {
            p --;
            }
        return p >= 0 ? tb_lineIndent( B(), p ) : 0;
        }
    want = ind_computeIndent( B(), inLine );
    if( want < 0 ) {
        want = ind_commentIndent( B(), inLine );
        }
    if( want < 0 && inLine > 0 ) {
        /* comment / macro body:  copy the previous line */
        want = tb_lineIndent( B(), inLine - 1 );
        }
    if( want < 0 ) {
        want = 0;
        }
    return want;
    }



static void deleteTrailingSpace( int  inLine ) {
    Line  *l  =  lineAt( inLine );
    int    e  =  l->len;

    while( e > 0 && ( l->text[ e - 1 ] == ' ' || l->text[ e - 1 ] == '\t' ) ) {
        e --;
        }
    if( e < l->len ) {
        int  cl;
        int  cc;

        getCur( &cl, &cc );
        tb_delete( B(), inLine, e, inLine, l->len );
        if( cl == inLine && cc > e ) {
            setCur( cl, e );
            }
        }
    }



/* format the line (paren padding, splitting argument lists etc.) and
   align declarations around it, keeping the cursor attached to its text.
   inFlags:  FMT_PARTIAL from Tab */
static void formatLineAt( int  inLine,
                          int  inFlags ) {
    int  l;
    int  c;

    if( ! config.formatOnEnter ) {
        return;
        }
    getCur( &l, &c );
    fmt_formatLine( B(), inLine, inFlags, &l, &c );
    setCur( l, c );
    getCur( &l, &c );
    fmt_alignDeclarations( B(), inLine, inFlags, &l, &c );
    setCur( l, c );
    }



/* plain text Enter:  the new (cursor) line gets the previous line's
   leading whitespace, tabs and all (a Makefile recipe line keeps its
   tab);  after a blank line, none (the next target starts at column 0) */
static void plainNewLineIndent( void ) {
    int    l;
    int    c;
    int    p;
    int    n     =  0;
    int    old   =  0;
    Line  *cur;

    getCur( &l, &c );
    p = l - 1;
    cur = lineAt( l );
    while( old < cur->len
           && ( cur->text[ old ] == ' ' || cur->text[ old ] == '\t' ) ) {
        old ++;
        }
    if( old > 0 ) {
        tb_delete( B(), l, 0, l, old );
        }
    if( p >= 0 ) {
        Line  *pl  =  lineAt( p );
        char  *ws;

        while( n < pl->len
               && ( pl->text[ n ] == ' ' || pl->text[ n ] == '\t' ) ) {
            n ++;
            }
        if( n > 0 ) {
            /* copy:  inserting may move the previous line's text */
            ws = (char *)malloc( (size_t)n );
            memcpy( ws, pl->text, (size_t)n );
            tb_insert( B(), l, 0, ws, n, NULL, NULL );
            free( ws );
            }
        }
    setCur( l, n );
    }



/* the core of Enter */
static void newlineAndIndent( void ) {
    int  l;
    int  c;
    int  state;

    getCur( &l, &c );
    state = syn_getLineState( B(), l );

    if( B()->isC && state == 0 && tokenTypeAt( B(), l, c ) == 0 ) {
        if( config.reindentOnEnter && ! tb_lineIsBlank( B(), l ) ) {
            reindentLine( l );
            }
        formatLineAt( l, 0 );
        }
    getCur( &l, &c );

    insertAtCursor( "\n", 1 );
    /* the old line:  no trailing spaces, and blank lines get no indent */
    deleteTrailingSpace( l );

    if( ! B()->isC ) {
        plainNewLineIndent();
        return;
        }

    getCur( &l, &c );
    {
        int  want  =  newLineIndent( l );
        /* text that moved down with the cursor keeps its indent decided
           by the engine */
        int  oldInd  =  tb_lineIndent( B(), l );

        tb_setLineIndent( B(), l, want );
        setCur( l, want );
        (void)oldInd;
        if( ! tb_lineIsBlank( B(), l ) ) {
            reindentLine( l );
            setCur( l, tb_lineIndent( B(), l ) );
            }
        }
    }



static void cmdNewline( void ) {
    if( ! checkWritable() ) {
        return;
        }
    deleteActiveRegion();
    newlineAndIndent();
    }



static void cmdOpenLine( void ) {
    int  l;
    int  c;

    if( ! checkWritable() ) {
        return;
        }
    getCur( &l, &c );
    insertAtCursor( "\n", 1 );
    setCur( l, c );
    }



static void cmdIndentRegionLines( int  inFirst,
                                  int  inLast ) {
    int  l;
    int  c;

    getCur( &l, &c );
    ind_indentRegion( B(), inFirst, inLast );
    syncCur();
    getCur( &l, &c );
    if( c > lineAt( l )->len ) {
        c = lineAt( l )->len;
        }
    setCur( l, c );
    ed_message( "Indented %d line%s", inLast - inFirst + 1,
                inLast == inFirst ? "" : "s" );
    }



/* Tab in plain text inserts a real tab character, like emacs:
   Makefiles and other formats need them. */
static void plainTab( void ) {
    insertAtCursor( "\t", 1 );
    }



/* the C commands do nothing in a plain text buffer */
static int checkC( void ) {
    if( ! B()->isC ) {
        ed_message( "Not a C buffer (%s)", B()->name );
        return 0;
        }
    return 1;
    }



static void cmdTab( void ) {
    int  sl;
    int  sc;
    int  el;
    int  ec;
    int  l;
    int  c;

    if( ! checkWritable() ) {
        return;
        }
    if( ! B()->isC ) {
        if( ed_getRegion( &sl, &sc, &el, &ec ) && sl != el ) {
            /* plain text region:  a tab at the start of each line */
            if( ec == 0 && el > sl ) {
                el --;
                }
            for( l = sl;
                 l <= el;
                 l ++ ) {
                if( ! tb_lineIsBlank( B(), l ) ) {
                    tb_insert( B(), l, 0, "\t", 1, NULL, NULL );
                    }
                }
            syncCur();
            return;
            }
        plainTab();
        return;
        }
    if( ed_getRegion( &sl, &sc, &el, &ec ) && sl != el ) {
        if( ec == 0 && el > sl ) {
            el --;
            }
        cmdIndentRegionLines( sl, el );
        return;
        }
    getCur( &l, &c );
    reindentLine( l );
    getCur( &l, &c );
    if( c < tb_lineIndent( B(), l ) ) {
        setCur( l, tb_lineIndent( B(), l ) );
        getCur( &l, &c );
        }
    if( ! tb_lineIsBlank( B(), l ) && syn_getLineState( B(), l ) == 0
        && tokenTypeAt( B(), l, c ) == 0 ) {
        /* tidy the line like Enter does;  on an unfinished declaration
           ("int  ") also jump to where the name will line up */
        formatLineAt( l, FMT_PARTIAL );
        }
    if( tb_lineIsBlank( B(), l ) ) {
        /* blank line:  still put cursor where code would go */
        int  want  =  newLineIndent( l );

        tb_setLineIndent( B(), l, want );
        setCur( l, want );
        }
    }



/* finds the extent of the top-level construct around a line */
static void topLevelExtent( int   inLine,
                            int  *outFirst,
                            int  *outLast ) {
    int  first  =  inLine;
    int  last   =  inLine;
    int  n      =  B()->numLines;

    while( first > 0 ) {
        Line  *l  =  lineAt( first );

        if( l->len > 0 && l->text[ 0 ] != ' ' && l->text[ 0 ] != '}'
            && l->text[ 0 ] != '#'
            &&
            ( syn_getLineState( B(), first ) & LEX_IN_COMMENT ) == 0 ) {
            break;
            }
        first --;
        }
    last = inLine + 1;
    while( last < n ) {
        Line  *l  =  lineAt( last );

        if( l->len > 0 && l->text[ 0 ] != ' ' && l->text[ 0 ] != '}'
            &&
            ( syn_getLineState( B(), last ) & LEX_IN_COMMENT ) == 0 ) {
            break;
            }
        last ++;
        }
    last --;
    *outFirst = first;
    *outLast = last;
    }



static void cmdIndentRegion( void ) {
    int  sl;
    int  sc;
    int  el;
    int  ec;

    if( ! checkC() ) {
        return;
        }
    if( ! checkWritable() ) {
        return;
        }
    if( ed_getRegion( &sl, &sc, &el, &ec ) ) {
        if( ec == 0 && el > sl ) {
            el --;
            }
        }
    else {
        /* no region:  the current function / top-level block */
        topLevelExtent( curLine(), &sl, &el );
        }
    cmdIndentRegionLines( sl, el );
    }



static void cmdIndentBuffer( void ) {
    if( ! checkC() ) {
        return;
        }
    if( ! checkWritable() ) {
        return;
        }
    cmdIndentRegionLines( 0, B()->numLines - 1 );
    }



static void cmdFormatRegion( void ) {
    int  sl;
    int  sc;
    int  el;
    int  ec;
    int  savedFlag  =  config.formatOnEnter;

    if( ! checkC() ) {
        return;
        }
    if( ! checkWritable() ) {
        return;
        }
    if( ! ed_getRegion( &sl, &sc, &el, &ec ) ) {
        topLevelExtent( curLine(), &sl, &el );
        }
    config.formatOnEnter = 1;
    fmt_formatRegion( B(), sl, el );
    config.formatOnEnter = savedFlag;
    syncCur();
    setCur( curLine(), curCol() );
    ed_message( "Formatted lines %d-%d", sl + 1, el + 1 );
    }



static void cmdFormatBuffer( void ) {
    int  savedFlag  =  config.formatOnEnter;

    if( ! checkC() ) {
        return;
        }
    if( ! checkWritable() ) {
        return;
        }
    config.formatOnEnter = 1;
    fmt_formatRegion( B(), 0, B()->numLines - 1 );
    config.formatOnEnter = savedFlag;
    syncCur();
    setCur( curLine(), curCol() );
    ed_message( "Formatted buffer" );
    }



static void cmdAlignDeclarations( void ) {
    int  l;
    int  c;
    int  saved  =  config.alignDeclarations;

    if( ! checkC() ) {
        return;
        }
    if( ! checkWritable() ) {
        return;
        }
    getCur( &l, &c );
    config.alignDeclarations = 1;
    if( ! fmt_alignDeclarations( B(), l, 0, &l, &c ) ) {
        ed_message( "No declaration to align here" );
        }
    config.alignDeclarations = saved;
    setCur( l, c );
    }



/* M-i:  insert spaces up to the next column where a token starts on the
   previous non-blank, non-comment line (for hand-aligning tables) */
static void cmdAlignToNextColumn( void ) {
    int  l;
    int  c;
    int  p;
    int  target  =  -1;

    if( ! checkWritable() ) {
        return;
        }
    getCur( &l, &c );
    for( p = l - 1;
         p >= 0;
         p -- ) {
        Line  *pl   =  lineAt( p );
        int    ind  =  tb_lineIndent( B(), p );

        if( ind == pl->len || pl->text[ ind ] == '/' ) {
            continue;
            }
        break;
        }
    if( p >= 0 ) {
        Line  *pl         =  lineAt( p );
        int    i;
        int    prevSpace  =  1;
        int    cur        =  c;

        if( c < lineAt( l )->len && lineAt( l )->text[ c ] == '*' ) {
            cur = c + 1;
            }
        for( i = 0;
             i < pl->len;
             i ++ ) {
            char  ch  =  pl->text[ i ];

            if( ch == ' ' || ch == '*' ) {
                prevSpace = 1;
                }
            else {
                if( prevSpace && i > cur ) {
                    target = i;
                    break;
                    }
                prevSpace = 0;
                }
            }
        if( target > cur ) {
            char  sp[ 256 ];
            int   n  =  target - cur;

            if( n > 255 ) {
                n = 255;
                }
            memset( sp, ' ', (size_t)n );
            insertAtCursor( sp, n );
            return;
            }
        }
    ed_message( "No further column on previous line" );
    }



/* ------------------------------------------------------------------
   Typing
   ------------------------------------------------------------------ */

/* paren depth at a position, counting from the start of its line plus
   the context at line start */
static int insideParens( int  inLine,
                         int  inCol ) {
    static TokenList  tl;
    static int        init   =  0;
    LineContext       ctx;
    Line             *l      =  lineAt( inLine );
    int               depth  =  0;
    int               k;

    if( ! init ) {
        syn_initTokenList( &tl );
        init = 1;
        }
    ind_getLineContext( B(), inLine, &ctx );
    syn_lexLine( l->text, l->len, syn_getLineState( B(), inLine ), &tl );
    for( k = 0;
         k < tl.num && tl.tokens[ k ].start < inCol;
         k ++ ) {
        Token  *t  =  &( tl.tokens[ k ] );

        if( t->type == TK_PUNCT && t->len == 1 ) {
            if( t->c == '(' ) {
                depth ++;
                }
            else if( t->c == ')' ) {
                depth --;
                }
            }
        }
    return depth > 0 || ( ctx.inParen && depth >= 0 );
    }



/* is the '{' at (line, col) the start of an initializer list or enum? */
static int braceIsList( int  inLine,
                        int  inCol ) {
    Line  *l  =  lineAt( inLine );
    int    i  =  inCol - 1;

    while( i >= 0 && l->text[ i ] == ' ' ) {
        i --;
        }
    if( i >= 0 && ( l->text[ i ] == '=' || l->text[ i ] == ','
                    || l->text[ i ] == '{' ) ) {
        return 1;
        }
    if( i < 0 && inLine > 0 ) {
        /* brace at line start:  look at end of previous line */
        Line  *p  =  lineAt( inLine - 1 );
        int    j  =  p->len - 1;

        while( j >= 0 && p->text[ j ] == ' ' ) {
            j --;
            }
        if( j >= 0 && ( p->text[ j ] == '=' || p->text[ j ] == ',' ) ) {
            return 1;
            }
        }
    /* enum */
    for( i = 0;
         i + 4 <= inCol;
         i ++ ) {
        if( strncmp( l->text + i, "enum", 4 ) == 0
            &&
            ( i == 0 || ! syn_isIdentChar( l->text[ i - 1 ] ) )
            &&
            ( i + 4 >= l->len || ! syn_isIdentChar( l->text[ i + 4 ] ) ) ) {
            return 1;
            }
        }
    return 0;
    }



/* is the '{' at (line, col) a struct/union/typedef body or a do-block,
   whose '}' is followed by more of the statement ( } Foo;  } while(x); ) */
static int braceContinuesStatement( int  inLine,
                                    int  inCol ) {
    Line        *l      =  lineAt( inLine );
    const char  *words[ 5 ];
    int          w;
    int          i;

    words[ 0 ] = "struct";
    words[ 1 ] = "union";
    words[ 2 ] = "typedef";
    words[ 3 ] = "do";
    words[ 4 ] = NULL;

    /* a ')' right before the brace means a function or control body */
    i = inCol - 1;
    while( i >= 0 && l->text[ i ] == ' ' ) {
        i --;
        }
    if( i >= 0 && l->text[ i ] == ')' ) {
        return 0;
        }
    for( w = 0;
         words[ w ] != NULL;
         w ++ ) {
        int  n  =  (int)strlen( words[ w ] );

        for( i = 0;
             i + n <= inCol;
             i ++ ) {
            if( strncmp( l->text + i, words[ w ], (size_t)n ) == 0
                &&
                ( i == 0 || ! syn_isIdentChar( l->text[ i - 1 ] ) )
                &&
                ( i + n >= l->len || ! syn_isIdentChar( l->text[ i + n ] ) ) ) {
                return 1;
                }
            }
        }
    return 0;
    }



static int restOfLineBlank( int  inLine,
                            int  inCol ) {
    Line  *l  =  lineAt( inLine );
    int    i;

    for( i = inCol;
         i < l->len;
         i ++ ) {
        if( l->text[ i ] != ' ' ) {
            return 0;
            }
        }
    return 1;
    }



/* electric behavior after a character was inserted at (l, c - 1) */
static void electric( char  inC ) {
    int  l;
    int  c;
    int  inCode;

    if( ! B()->isC ) {
        return;
        }
    getCur( &l, &c );
    inCode = ( syn_getLineState( B(), l ) == 0
               && tokenTypeAt( B(), l, c - 1 ) == 0
               && tokenTypeAt( B(), l, c ) == 0 );
    if( ! inCode ) {
        return;
        }

    if( inC == ';' && config.cleanupBraceSemi
        && c == tb_lineIndent( B(), l ) + 1 && restOfLineBlank( l, c )
        && l > 0 ) {
        /* ";" on an empty line right after "}" :  join them  "};" */
        int    p   =  l - 1;
        Line  *pl  =  lineAt( p );
        int    e   =  pl->len;

        while( e > 0 && pl->text[ e - 1 ] == ' ' ) {
            e --;
            }
        if( e > 0 && pl->text[ e - 1 ] == '}' ) {
            tb_delete( B(), p, e, l, c - 1 );
            setCur( p, e + 1 );
            getCur( &l, &c );
            }
        }

    if( config.electricChars
        &&
        ( inC == '{' || inC == '}' || inC == ':' || inC == '#'
          || inC == ';' || inC == ')' || inC == ']' ) ) {
        /* only reindent when this char is the first thing on the line, or
           it is a ; or : (statement ends and case labels) */
        int  ind    =  tb_lineIndent( B(), l );
        int  first  =  ( c - 1 == ind );

        if( first || inC == ':' || inC == ';' || inC == '{' ) {
            reindentLine( l );
            getCur( &l, &c );
            }
        }

    if( config.electricChars && isalpha( (unsigned char)inC ) ) {
        /* "else" as the first word on a line jumps into place */
        int    ind  =  tb_lineIndent( B(), l );
        Line  *ln   =  lineAt( l );

        if( c - ind == 4 && strncmp( ln->text + ind, "else", 4 ) == 0
            &&
            ( c >= ln->len || ! syn_isIdentChar( ln->text[ c ] ) ) ) {
            reindentLine( l );
            getCur( &l, &c );
            }
        }

    if( config.autoNewline && restOfLineBlank( l, c ) ) {
        if( inC == '{' && ! insideParens( l, c - 1 )
            && ! braceIsList( l, c - 1 ) ) {
            newlineAndIndent();
            }
        else if( inC == ';' && ! insideParens( l, c - 1 ) ) {
            newlineAndIndent();
            }
        else if( inC == '}' ) {
            int  ml;
            int  mc;

            if( ind_findMatchingBracket( B(), l, c - 1, 20000, &ml, &mc )
                &&
                ! braceIsList( ml, mc )
                &&
                ! braceContinuesStatement( ml, mc ) ) {
                newlineAndIndent();
                }
            }
        else if( inC == ':' ) {
            /* hanging colon after case labels, like emacs */
            Line  *ln   =  lineAt( l );
            int    ind  =  tb_lineIndent( B(), l );

            if( ( ln->len - ind >= 4
                  && strncmp( ln->text + ind, "case", 4 ) == 0
                  && ! syn_isIdentChar( ln->text[ ind + 4 ] ) )
                ||
                ( ln->len - ind >= 7
                  && strncmp( ln->text + ind, "default", 7 ) == 0
                  && ! syn_isIdentChar( ln->text[ ind + 7 ] ) ) ) {
                newlineAndIndent();
                }
            }
        }
    }



static void selfInsert( const char  *inText,
                        int          inLen ) {
    if( ! checkWritable() ) {
        return;
        }
    deleteActiveRegion();
    insertAtCursor( inText, inLen );
    if( inLen == 1 ) {
        electric( inText[ 0 ] );
        }
    }



static void cmdSelfInsert( void ) {
    /* placeholder identity for lastCmd tracking */
    }



/* hungry delete helpers */
static int isSpaceAt( int  inLine,
                      int  inCol ) {
    return inCol < lineAt( inLine )->len
           && lineAt( inLine )->text[ inCol ] == ' ';
    }



static void cmdBackspace( void ) {
    int  l;
    int  c;

    if( ! checkWritable() ) {
        return;
        }
    if( deleteActiveRegion() ) {
        return;
        }
    getCur( &l, &c );
    if( l == 0 && c == 0 ) {
        return;
        }
    if( config.hungryDelete && B()->isC
        &&
        ( c == 0 || lineAt( l )->text[ c - 1 ] == ' ' ) ) {
        int  sl  =  l;
        int  sc  =  c;

        while( 1 ) {
            if( sc > 0 && lineAt( sl )->text[ sc - 1 ] == ' ' ) {
                sc --;
                }
            else if( sc == 0 && sl > 0 ) {
                sl --;
                sc = lineAt( sl )->len;
                }
            else {
                break;
                }
            }
        tb_delete( B(), sl, sc, l, c );
        setCur( sl, sc );
        return;
        }
    if( c > 0 ) {
        /* delete a whole UTF-8 sequence */
        int  s  =  c - 1;

        while( s > 0 && ( lineAt( l )->text[ s ] & 0xC0 ) == 0x80 ) {
            s --;
            }
        tb_delete( B(), l, s, l, c );
        setCur( l, s );
        }
    else {
        int  pc  =  lineAt( l - 1 )->len;

        tb_delete( B(), l - 1, pc, l, 0 );
        setCur( l - 1, pc );
        }
    }



static void cmdBackspaceSingle( void ) {
    int  saved  =  config.hungryDelete;

    config.hungryDelete = 0;
    cmdBackspace();
    config.hungryDelete = saved;
    }



static void cmdDeleteChar( void ) {
    int  l;
    int  c;

    if( ! checkWritable() ) {
        return;
        }
    if( deleteActiveRegion() ) {
        return;
        }
    getCur( &l, &c );
    if( config.hungryDelete
        &&
        ( c == lineAt( l )->len || isSpaceAt( l, c ) ) ) {
        int  el  =  l;
        int  ec  =  c;

        while( 1 ) {
            if( isSpaceAt( el, ec ) ) {
                ec ++;
                }
            else if( ec == lineAt( el )->len && el < B()->numLines - 1 ) {
                el ++;
                ec = 0;
                }
            else {
                break;
                }
            }
        tb_delete( B(), l, c, el, ec );
        setCur( l, c );
        return;
        }
    if( c < lineAt( l )->len ) {
        int  e  =  c + 1;

        while( e < lineAt( l )->len
               && ( lineAt( l )->text[ e ] & 0xC0 ) == 0x80 ) {
            e ++;
            }
        tb_delete( B(), l, c, l, e );
        }
    else if( l < B()->numLines - 1 ) {
        tb_delete( B(), l, c, l + 1, 0 );
        }
    setCur( l, c );
    }



static void cmdUndo( void ) {
    if( tb_undo( B() ) ) {
        setCur( B()->cursorLine, B()->cursorCol );
        deactivateMark();
        ed_message( "Undo" );
        }
    else {
        ed_errorMessage( "No further undo information" );
        }
    }



static void cmdRedo( void ) {
    if( tb_redo( B() ) ) {
        setCur( B()->cursorLine, B()->cursorCol );
        deactivateMark();
        ed_message( "Redo" );
        }
    else {
        ed_errorMessage( "Nothing to redo" );
        }
    }



/* ------------------------------------------------------------------
   Dabbrev:  expand the word before the cursor from words found
   earlier in this buffer, then later, then in other buffers.
   Repeating M-/ cycles.
   ------------------------------------------------------------------ */

static char  **dabCands     =  NULL;
static int     dabNum       =  0;
static int     dabIndex     =  0;
static int     dabLine;
static int     dabStartCol;
static int     dabInsertedLen;
static char    dabPrefix[ 256 ];



static void dabClear( void ) {
    int  i;

    for( i = 0;
         i < dabNum;
         i ++ ) {
        free( dabCands[ i ] );
        }
    free( dabCands );
    dabCands = NULL;
    dabNum = 0;
    }



static void dabAdd( const char  *inWord,
                    int          inLen ) {
    int  i;

    if( inLen == (int)strlen( dabPrefix ) ) {
        return;
        }
    for( i = 0;
         i < dabNum;
         i ++ ) {
        if( (int)strlen( dabCands[ i ] ) == inLen
            &&
            strncmp( dabCands[ i ], inWord, (size_t)inLen ) == 0 ) {
            return;
            }
        }
    dabCands = (char **)realloc( dabCands, (size_t)( dabNum + 1 )
                                 * sizeof( char * ) );
    dabCands[ dabNum ] = (char *)malloc( (size_t)inLen + 1 );
    memcpy( dabCands[ dabNum ], inWord, (size_t)inLen );
    dabCands[ dabNum ][ inLen ] = '\0';
    dabNum ++;
    }



/* scans one line for words starting with prefix;  inReverse lists them
   right-to-left.  inLimitCol (or -1):  only words ending before it (for
   reverse) or starting after it (forward) */
static void dabScanLine( TextBuffer  *inTB,
                         int          inLine,
                         int          inReverse,
                         int          inLimitCol ) {
    Line  *l     =  &( inTB->lines[ inLine ] );
    int    plen  =  (int)strlen( dabPrefix );
    int    starts[ 512 ];
    int    lens[ 512 ];
    int    n     =  0;
    int    i     =  0;
    int    k;

    while( i < l->len && n < 512 ) {
        if( syn_isIdentChar( l->text[ i ] )
            &&
            ( i == 0 || ! syn_isIdentChar( l->text[ i - 1 ] ) ) ) {
            int  s  =  i;

            while( i < l->len && syn_isIdentChar( l->text[ i ] ) ) {
                i ++;
                }
            if( i - s > plen
                &&
                strncmp( l->text + s, dabPrefix, (size_t)plen ) == 0 ) {
                int  ok  =  1;

                if( inLimitCol >= 0 ) {
                    if( inReverse && i > inLimitCol ) {
                        ok = 0;
                        }
                    if( ! inReverse && s < inLimitCol ) {
                        ok = 0;
                        }
                    }
                if( ok ) {
                    starts[ n ] = s;
                    lens[ n ] = i - s;
                    n ++;
                    }
                }
            }
        else {
            i ++;
            }
        }
    for( k = 0;
         k < n;
         k ++ ) {
        int  j  =  inReverse ? n - 1 - k : k;

        dabAdd( l->text + starts[ j ], lens[ j ] );
        }
    }



static void dabCollect( void ) {
    TextBuffer  *tb   =  B();
    int          i;
    int          b;
    int          max  =  200;

    /* backward from cursor */
    dabScanLine( tb, dabLine, 1, dabStartCol );
    for( i = dabLine - 1;
         i >= 0 && dabNum < max;
         i -- ) {
        dabScanLine( tb, i, 1, -1 );
        }
    /* forward */
    dabScanLine( tb, dabLine, 0, dabStartCol + (int)strlen( dabPrefix ) );
    for( i = dabLine + 1;
         i < tb->numLines && dabNum < max;
         i ++ ) {
        dabScanLine( tb, i, 0, -1 );
        }
    /* other buffers, most recently used first */
    for( b = 1;
         b < ed.numBuffers && dabNum < max;
         b ++ ) {
        TextBuffer  *o  =  ed.buffers[ ed.mru[ b ] ];

        for( i = 0;
             i < o->numLines && dabNum < max;
             i ++ ) {
            dabScanLine( o, i, 0, -1 );
            }
        }
    }



static void cmdDabbrev( void ) {
    int  l;
    int  c;

    if( ! checkWritable() ) {
        return;
        }
    getCur( &l, &c );

    if( lastCmd != cmdDabbrev ) {
        int  s  =  c;
        int  plen;

        while( s > 0 && syn_isIdentChar( lineAt( l )->text[ s - 1 ] ) ) {
            s --;
            }
        plen = c - s;
        if( plen == 0 || plen > 200 ) {
            ed_errorMessage( "No word to complete" );
            return;
            }
        memcpy( dabPrefix, lineAt( l )->text + s, (size_t)plen );
        dabPrefix[ plen ] = '\0';
        dabClear();
        dabLine = l;
        dabStartCol = s;
        dabInsertedLen = plen;
        dabIndex = -1;
        dabCollect();
        if( dabNum == 0 ) {
            ed_errorMessage( "No expansion for \"%s\"", dabPrefix );
            return;
            }
        }
    if( dabNum == 0 ) {
        return;
        }

    dabIndex ++;
    if( dabIndex >= dabNum ) {
        /* wrapped:  back to the original prefix */
        tb_delete( B(), dabLine, dabStartCol, dabLine,
                   dabStartCol + dabInsertedLen );
        tb_insert( B(), dabLine, dabStartCol, dabPrefix,
                   (int)strlen( dabPrefix ), NULL, NULL );
        dabInsertedLen = (int)strlen( dabPrefix );
        setCur( dabLine, dabStartCol + dabInsertedLen );
        dabIndex = -1;
        ed_message( "No further expansions (back to \"%s\")", dabPrefix );
        return;
        }
    tb_delete( B(), dabLine, dabStartCol, dabLine,
               dabStartCol + dabInsertedLen );
    tb_insert( B(), dabLine, dabStartCol, dabCands[ dabIndex ],
               (int)strlen( dabCands[ dabIndex ] ), NULL, NULL );
    dabInsertedLen = (int)strlen( dabCands[ dabIndex ] );
    setCur( dabLine, dabStartCol + dabInsertedLen );
    ed_message( "Expansion %d of %d  (M-/ again for next)", dabIndex + 1,
                dabNum );
    }



/* ------------------------------------------------------------------
   Search
   ------------------------------------------------------------------ */

static int hasUpper( const char  *inS ) {
    while( *inS != '\0' ) {
        if( isupper( (unsigned char)*inS ) ) {
            return 1;
            }
        inS ++;
        }
    return 0;
    }



static int matchAt( Line        *inL,
                    int          inCol,
                    const char  *inText,
                    int          inLen,
                    int          inFold ) {
    int  i;

    if( inCol + inLen > inL->len ) {
        return 0;
        }
    for( i = 0;
         i < inLen;
         i ++ ) {
        char  a  =  inL->text[ inCol + i ];
        char  b  =  inText[ i ];

        if( inFold ) {
            a = (char)tolower( (unsigned char)a );
            b = (char)tolower( (unsigned char)b );
            }
        if( a != b ) {
            return 0;
            }
        }
    return 1;
    }



/* finds inText starting at/after (line, col) going forward, or strictly
   before it going backward.  No wrap.  Returns 1 if found. */
static int findText( TextBuffer  *inTB,
                     const char  *inText,
                     int          inLine,
                     int          inCol,
                     int          inForward,
                     int         *outLine,
                     int         *outCol ) {
    int  len   =  (int)strlen( inText );
    int  fold  =  ! hasUpper( inText );
    int  l     =  inLine;
    int  c;

    if( len == 0 ) {
        return 0;
        }
    if( inForward ) {
        for( l = inLine;
             l < inTB->numLines;
             l ++ ) {
            Line  *ln  =  &( inTB->lines[ l ] );

            for( c = ( l == inLine ? inCol : 0 );
                 c + len <= ln->len;
                 c ++ ) {
                if( matchAt( ln, c, inText, len, fold ) ) {
                    *outLine = l;
                    *outCol = c;
                    return 1;
                    }
                }
            }
        }
    else {
        for( l = inLine;
             l >= 0;
             l -- ) {
            Line  *ln  =  &( inTB->lines[ l ] );

            c = ( l == inLine ? inCol - 1 : ln->len - len );
            if( c > ln->len - len ) {
                c = ln->len - len;
                }
            for( ;
                 c >= 0;
                 c -- ) {
                if( matchAt( ln, c, inText, len, fold ) ) {
                    *outLine = l;
                    *outCol = c;
                    return 1;
                    }
                }
            }
        }
    return 0;
    }



int ed_searchHighlight( const char  **outText,
                        int          *outCaseFold ) {
    if( ( ed.mbMode == MB_ISEARCH && ed.searchText[ 0 ] != '\0' ) ) {
        *outText = ed.searchText;
        *outCaseFold = ! hasUpper( ed.searchText );
        return 1;
        }
    if( ed.mbMode == MB_QUERY_REPLACE ) {
        *outText = ed.replaceFrom;
        *outCaseFold = ! hasUpper( ed.replaceFrom );
        return 1;
        }
    return 0;
    }



static void updateSearchPrompt( void ) {
    sprintf( ed.mbPrompt, "%s%sI-search%s: ",
             ed.searchFailing ? "Failing " : "",
             ed.searchWrapped ? "Wrapped " : "",
             ed.searchForward ? "" : " backward" );
    strcpy( ed.mbText, ed.searchText );
    ed.mbCursor = (int)strlen( ed.mbText );
    }



/* searches from (line,col) in the current direction, updates match */
static void isearchFrom( int  inLine,
                         int  inCol,
                         int  inWrapIfFail ) {
    int  fl;
    int  fc;
    int  found;

    found = findText( B(), ed.searchText, inLine, inCol, ed.searchForward,
                      &fl, &fc );
    if( ! found && inWrapIfFail ) {
        if( ed.searchForward ) {
            found = findText( B(), ed.searchText, 0, 0, 1, &fl, &fc );
            }
        else {
            int  last  =  B()->numLines - 1;

            found = findText( B(), ed.searchText, last,
                              lineAt( last )->len + 1, 0, &fl, &fc );
            }
        if( found ) {
            ed.searchWrapped = 1;
            }
        }
    if( found ) {
        ed.searchFailing = 0;
        ed.matchLine = fl;
        ed.matchCol = fc;
        ed.matchLen = (int)strlen( ed.searchText );
        if( ed.searchForward ) {
            setCur( fl, fc + ed.matchLen );
            }
        else {
            setCur( fl, fc );
            }
        }
    else {
        ed.searchFailing = 1;
        }
    updateSearchPrompt();
    }



static void cmdISearch( int  inForward ) {
    int  l;
    int  c;

    if( ed.mbMode == MB_ISEARCH ) {
        /* repeat */
        if( ed.searchText[ 0 ] == '\0' ) {
            strcpy( ed.searchText, ed.lastSearch );
            }
        if( ed.searchForward != inForward ) {
            ed.searchForward = inForward;
            }
        if( ed.searchText[ 0 ] == '\0' ) {
            updateSearchPrompt();
            return;
            }
        if( ed.searchFailing ) {
            isearchFrom( inForward ? 0 : B()->numLines - 1,
                         inForward ? 0 : INT_MAX / 2, 1 );
            }
        else if( inForward ) {
            isearchFrom( ed.matchLine, ed.matchCol + 1, 0 );
            }
        else {
            isearchFrom( ed.matchLine, ed.matchCol, 0 );
            }
        return;
        }
    getCur( &l, &c );
    ed.mbMode = MB_ISEARCH;
    ed.searchForward = inForward;
    ed.searchFailing = 0;
    ed.searchWrapped = 0;
    ed.searchStartLine = l;
    ed.searchStartCol = c;
    ed.matchLine = l;
    ed.matchCol = c;
    ed.matchLen = 0;
    ed.searchText[ 0 ] = '\0';
    ed.numCands = 0;
    updateSearchPrompt();
    }



static void isearchEnd( int  inCancel ) {
    if( ed.searchText[ 0 ] != '\0' ) {
        strcpy( ed.lastSearch, ed.searchText );
        }
    ed.mbMode = MB_NONE;
    if( inCancel ) {
        setCur( ed.searchStartLine, ed.searchStartCol );
        ed_message( "Quit" );
        }
    else {
        TextBuffer  *tb  =  B();

        if( ed.searchStartLine != tb->cursorLine
            || ed.searchStartCol != tb->cursorCol ) {
            tb_setMarker( tb, tb->markMarker, ed.searchStartLine,
                          ed.searchStartCol );
            tb->markActive = 0;
            tb->markSet = 1;
            ed_message( "Mark saved where search started" );
            }
        }
    }



/* returns 1 if the key was consumed by isearch */
static int isearchKey( int  inKey,
                       int  inMods ) {
    if( inKey == SDLK_BACKSPACE ) {
        int  n  =  (int)strlen( ed.searchText );

        if( n > 0 ) {
            ed.searchText[ n - 1 ] = '\0';
            }
        ed.searchWrapped = 0;
        if( ed.searchText[ 0 ] == '\0' ) {
            setCur( ed.searchStartLine, ed.searchStartCol );
            ed.searchFailing = 0;
            updateSearchPrompt();
            }
        else {
            isearchFrom( ed.searchStartLine, ed.searchStartCol, 0 );
            }
        return 1;
        }
    if( inKey == SDLK_RETURN || inKey == SDLK_KP_ENTER ) {
        isearchEnd( 0 );
        return 1;
        }
    if( ( inMods & MOD_CTRL ) && inKey == SDLK_g ) {
        isearchEnd( 1 );
        return 1;
        }
    if( inKey == SDLK_ESCAPE ) {
        isearchEnd( 0 );
        return 1;
        }
    if( ( inMods & MOD_CTRL ) && inKey == SDLK_s ) {
        cmdISearch( 1 );
        return 1;
        }
    if( ( inMods & MOD_CTRL ) && inKey == SDLK_r ) {
        cmdISearch( 0 );
        return 1;
        }
    if( ( inMods & MOD_CTRL ) && inKey == SDLK_w ) {
        /* pull the rest of the word at point into the search string */
        int    l;
        int    c;
        Line  *ln;
        int    n  =  (int)strlen( ed.searchText );

        getCur( &l, &c );
        ln = lineAt( l );
        while( c < ln->len && ! isWordChar( ln->text[ c ] ) && n < 250 ) {
            ed.searchText[ n ++ ] = ln->text[ c ++ ];
            }
        while( c < ln->len && isWordChar( ln->text[ c ] ) && n < 250 ) {
            ed.searchText[ n ++ ] = ln->text[ c ++ ];
            }
        ed.searchText[ n ] = '\0';
        isearchFrom( ed.matchLine, ed.matchCol, 0 );
        return 1;
        }
    /* modifier keys alone don't end the search */
    if( inKey == SDLK_LSHIFT || inKey == SDLK_RSHIFT || inKey == SDLK_LCTRL
        || inKey == SDLK_RCTRL || inKey == SDLK_LALT || inKey == SDLK_RALT ) {
        return 1;
        }
    /* printable keys arrive via text input */
    if( ! ( inMods & ( MOD_CTRL | MOD_ALT ) )
        && inKey >= 32 && inKey < 127 ) {
        return 2;
        }
    /* anything else ends the search and runs normally */
    isearchEnd( 0 );
    return 0;
    }



static void isearchText( const char  *inText ) {
    int  n  =  (int)strlen( ed.searchText );
    int  k  =  (int)strlen( inText );

    if( n + k >= (int)sizeof( ed.searchText ) - 1 ) {
        return;
        }
    strcat( ed.searchText, inText );
    if( ed.searchFailing ) {
        updateSearchPrompt();
        return;
        }
    if( ed.searchForward ) {
        isearchFrom( ed.matchLen > 0 ? ed.matchLine : ed.searchStartLine,
                     ed.matchLen > 0 ? ed.matchCol : ed.searchStartCol, 0 );
        }
    else {
        isearchFrom( ed.matchLen > 0 ? ed.matchLine : ed.searchStartLine,
                     ( ed.matchLen > 0 ? ed.matchCol : ed.searchStartCol )
                     + (int)strlen( ed.searchText ), 0 );
        }
    }



/* ---- query replace ---- */

static void startPrompt( int          inMode,
                         const char  *inPrompt,
                         const char  *inInitial );

static void queryReplaceNext( void ) {
    int  l;
    int  c;
    int  fl;
    int  fc;

    getCur( &l, &c );
    if( findText( B(), ed.replaceFrom, l, c, 1, &fl, &fc ) ) {
        ed.matchLine = fl;
        ed.matchCol = fc;
        ed.matchLen = (int)strlen( ed.replaceFrom );
        setCur( fl, fc + ed.matchLen );
        sprintf( ed.mbPrompt, "Query replacing %.100s with %.100s:  "
                 "(y n ! . q) ", ed.replaceFrom, ed.replaceTo );
        ed.mbText[ 0 ] = '\0';
        ed.mbCursor = 0;
        ed.mbMode = MB_QUERY_REPLACE;
        }
    else {
        ed.mbMode = MB_NONE;
        ed_message( "Replaced %d occurrence%s", ed.replaceCount,
                    ed.replaceCount == 1 ? "" : "s" );
        }
    }



static void replaceCurrentMatch( void ) {
    int  tl  =  (int)strlen( ed.replaceTo );

    tb_continueGroup( B() );
    tb_delete( B(), ed.matchLine, ed.matchCol, ed.matchLine,
               ed.matchCol + ed.matchLen );
    tb_insert( B(), ed.matchLine, ed.matchCol, ed.replaceTo, tl, NULL,
               NULL );
    setCur( ed.matchLine, ed.matchCol + tl );
    ed.replaceCount ++;
    }



static void queryReplaceKey( const char  *inText ) {
    char  k  =  inText[ 0 ];

    if( k == 'y' || k == ' ' ) {
        replaceCurrentMatch();
        queryReplaceNext();
        }
    else if( k == 'n' ) {
        queryReplaceNext();
        }
    else if( k == '!' ) {
        replaceCurrentMatch();
        while( 1 ) {
            int  l;
            int  c;
            int  fl;
            int  fc;

            getCur( &l, &c );
            if( ! findText( B(), ed.replaceFrom, l, c, 1, &fl, &fc ) ) {
                break;
                }
            ed.matchLine = fl;
            ed.matchCol = fc;
            replaceCurrentMatch();
            }
        queryReplaceNext();
        }
    else if( k == '.' ) {
        replaceCurrentMatch();
        ed.mbMode = MB_NONE;
        ed_message( "Replaced %d occurrence%s", ed.replaceCount,
                    ed.replaceCount == 1 ? "" : "s" );
        }
    else if( k == 'q' ) {
        ed.mbMode = MB_NONE;
        ed_message( "Replaced %d occurrence%s", ed.replaceCount,
                    ed.replaceCount == 1 ? "" : "s" );
        }
    }



static void cmdQueryReplace( void ) {
    if( ! checkWritable() ) {
        return;
        }
    startPrompt( MB_REPLACE_FROM, "Query replace: ", ed.replaceFrom );
    }



/* ------------------------------------------------------------------
   Compile, next-error, debug
   ------------------------------------------------------------------ */

static void saveAllQuiet( void ) {
    int  i;

    for( i = 0;
         i < ed.numBuffers;
         i ++ ) {
        TextBuffer  *tb  =  ed.buffers[ i ];

        if( tb->path != NULL && tb_isModified( tb ) ) {
            saveBuffer( tb );
            }
        }
    }



static void runCompile( const char  *inCommand ) {
    if( comp_isRunning() ) {
        comp_kill();
        /* reap it */
        while( comp_isRunning() ) {
            comp_poll();
            SDL_Delay( 5 );
            }
        }
    if( config.saveBeforeCompile ) {
        saveAllQuiet();
        }
    if( ed.compileBuffer == NULL ) {
        ed.compileBuffer = tb_create();
        ed.compileBuffer->name = xstrdup( "*compile*" );
        ed.compileBuffer->readOnly = 1;
        }
    scopy( config.compileCommand, inCommand, sizeof( config.compileCommand ) );
    if( ! comp_start( inCommand, ed.compileBuffer ) ) {
        ed_errorMessage( "Could not start compile" );
        return;
        }
    ed.showCompile = 1;
    ed.currentError = -1;
    ed.compileHighlightLine = -1;
    ed.compileScroll = 0;
    ed.compileFollow = 1;
    ed_message( "Compiling:  %s", inCommand );
    }



static void cmdCompile( void ) {
    startPrompt( MB_COMPILE, "Compile command: ", config.compileCommand );
    }



static void cmdRecompile( void ) {
    runCompile( config.compileCommand );
    }



static void cmdKillCompile( void ) {
    if( comp_isRunning() ) {
        comp_kill();
        ed_message( "Killed compilation" );
        }
    else {
        ed_message( "No compilation running" );
        }
    }



static void cmdToggleCompilePane( void ) {
    if( ed.compileBuffer == NULL ) {
        ed_message( "No compilation yet" );
        return;
        }
    ed.showCompile = ! ed.showCompile;
    }



static void cmdHideCompilePane( void ) {
    ed.showCompile = 0;
    }



static void visitError( int  inIndex ) {
    CompileError  *e  =  comp_getError( inIndex );
    int            idx;
    int            l;
    int            c;
    int            i;
    TextBuffer    *tb;

    if( e == NULL ) {
        return;
        }
    if( e->buffer == NULL ) {
        idx = openFile( e->file, 1 );
        if( idx < 0 ) {
            ed_errorMessage( "Can't open %s", e->file );
            return;
            }
        tb = ed.buffers[ idx ];
        /* attach markers for every error in this file at once, so they
           all track later edits */
        for( i = 0;
             i < comp_numErrors();
             i ++ ) {
            CompileError  *o  =  comp_getError( i );

            if( o->buffer == NULL && strcmp( o->file, e->file ) == 0 ) {
                o->buffer = tb;
                o->marker = tb_addMarker( tb, o->line - 1,
                                          o->col > 0 ? o->col - 1 : 0 );
                }
            }
        }
    tb = e->buffer;
    for( idx = 0;
         idx < ed.numBuffers;
         idx ++ ) {
        if( ed.buffers[ idx ] == tb ) {
            break;
            }
        }
    if( idx == ed.numBuffers ) {
        return;
        }
    ed_switchToBuffer( idx );
    tb_getMarker( tb, e->marker, &l, &c );
    if( ! B()->markActive ) {
        setMarkHere();
        deactivateMark();
        }
    setCur( l, c );
    ed.recenterRequest = 1;
    ed.currentError = inIndex;
    ed.compileHighlightLine = e->outLine;
    ed.compileFollow = 0;
    ed.showCompile = 1;
    {
        Line  *ol  =  &( ed.compileBuffer->lines[ e->outLine ] );
        int    ne;
        int    nw;

        comp_countErrors( &ne, &nw );
        ed_message( "%.*s", ol->len > 900 ? 900 : ol->len, ol->text );
        ed.messageIsError = ( e->kind == CE_ERROR );
        }
    }



void ed_visitErrorAtOutputLine( int  inOutLine ) {
    int  i;

    for( i = 0;
         i < comp_numErrors();
         i ++ ) {
        CompileError  *e  =  comp_getError( i );

        if( inOutLine >= e->outLine
            &&
            inOutLine <= e->outLine + e->numDetailLines ) {
            visitError( i );
            return;
            }
        }
    }



static void stepError( int  inDir ) {
    int  i  =  ed.currentError;
    int  n  =  comp_numErrors();

    if( n == 0 ) {
        ed_errorMessage( comp_isRunning() ? "No errors yet" : "No errors" );
        return;
        }
    while( 1 ) {
        i += inDir;
        if( i < 0 || i >= n ) {
            ed_errorMessage( inDir > 0 ? "No more errors" :
                             "No previous errors" );
            return;
            }
        /* skip notes, like emacs's compilation-skip-threshold */
        if( comp_getError( i )->kind != CE_NOTE ) {
            break;
            }
        }
    visitError( i );
    }



static void cmdNextError( void ) {
    stepError( 1 );
    }



static void cmdPrevError( void ) {
    stepError( -1 );
    }



static void cmdFirstError( void ) {
    ed.currentError = -1;
    stepError( 1 );
    }



static void launchDebugger( void ) {
    char         full[ PATH_MAX ];
    char        *cmd;
    TextBuffer  *tb  =  B();

    if( tb->path == NULL ) {
        ed_errorMessage( "Buffer has no file" );
        return;
        }
    if( realpath( tb->path, full ) == NULL ) {
        scopy( full, tb->path, sizeof( full ) );
        full[ sizeof( full ) - 1 ] = '\0';
        }
    cmd = comp_expandDebugCommand( ed.debugCommand, baseName( tb->path ),
                                   full, curLine() + 1, ed.debugProgram );
    if( comp_launchDetached( cmd ) ) {
        ed_message( "Launched:  %.900s", cmd );
        }
    else {
        ed_errorMessage( "Could not launch:  %.900s", cmd );
        }
    free( cmd );
    }



static void defaultDebugProgram( char  *outProg ) {
    char  cwd[ 1024 ];

    if( getcwd( cwd, sizeof( cwd ) ) != NULL ) {
        sprintf( outProg, "./%.500s", baseName( cwd ) );
        }
    else {
        strcpy( outProg, "./a.out" );
        }
    }



static void cmdDebug( void ) {
    if( B()->path == NULL ) {
        ed_errorMessage( "Buffer has no file to set a breakpoint in" );
        return;
        }
    if( ed.debugCommand[ 0 ] == '\0' ) {
        startPrompt( MB_DEBUG_COMMAND,
                     "Debug command (%f file, %l line, %e program): ",
                     "xterm -e gdb -ex 'break %f:%l' -ex run %e" );
        return;
        }
    if( strstr( ed.debugCommand, "%e" ) != NULL
        && ed.debugProgram[ 0 ] == '\0' ) {
        char  prog[ 600 ];

        defaultDebugProgram( prog );
        startPrompt( MB_DEBUG_PROGRAM, "Program to debug: ", prog );
        return;
        }
    launchDebugger();
    }



static void cmdSetDebugProgram( void ) {
    char  prog[ 600 ];

    if( ed.debugProgram[ 0 ] != '\0' ) {
        strcpy( prog, ed.debugProgram );
        }
    else {
        defaultDebugProgram( prog );
        }
    startPrompt( MB_DEBUG_PROGRAM, "Program to debug: ", prog );
    /* don't launch after choosing */
    ed.yesNoAction = 1;
    }



static void cmdSetDebugCommand( void ) {
    startPrompt( MB_DEBUG_COMMAND,
                 "Debug command (%f file, %l line, %e program): ",
                 ed.debugCommand[ 0 ] != '\0' ? ed.debugCommand :
                 "xterm -e gdb -ex 'break %f:%l' -ex run %e" );
    ed.yesNoAction = 1;
    }



/* writes compile/debug settings into ./cedit.conf, keeping other lines */
static void cmdSaveFolderConfig( void ) {
    FILE        *f;
    char       **lines  =  NULL;
    int          n      =  0;
    char         buf[ 4096 ];
    int          i;
    const char  *keys[ 3 ];
    const char  *vals[ 3 ];
    int          done[ 3 ]  =  { 0, 0, 0 };
    int          k;

    keys[ 0 ] = "compileCommand";
    vals[ 0 ] = config.compileCommand;
    keys[ 1 ] = "debugCommand";
    vals[ 1 ] = ed.debugCommand;
    keys[ 2 ] = "debugProgram";
    vals[ 2 ] = ed.debugProgram;

    f = fopen( "cedit.conf", "r" );
    if( f != NULL ) {
        while( fgets( buf, sizeof( buf ), f ) != NULL ) {
            lines = (char **)realloc( lines, (size_t)( n + 1 )
                                      * sizeof( char * ) );
            lines[ n ] = xstrdup( buf );
            n ++;
            }
        fclose( f );
        }
    f = fopen( "cedit.conf", "w" );
    if( f == NULL ) {
        ed_errorMessage( "Can't write ./cedit.conf" );
        return;
        }
    for( i = 0;
         i < n;
         i ++ ) {
        char  *p        =  lines[ i ];
        int    replaced  =  0;

        while( *p == ' ' ) {
            p ++;
            }
        for( k = 0;
             k < 3;
             k ++ ) {
            size_t  kl  =  strlen( keys[ k ] );

            if( strncmp( p, keys[ k ], kl ) == 0
                &&
                ( p[ kl ] == ' ' || p[ kl ] == '=' ) ) {
                if( ! done[ k ] && vals[ k ][ 0 ] != '\0' ) {
                    fprintf( f, "%s = %s\n", keys[ k ], vals[ k ] );
                    }
                done[ k ] = 1;
                replaced = 1;
                }
            }
        if( ! replaced ) {
            fputs( lines[ i ], f );
            }
        free( lines[ i ] );
        }
    free( lines );
    for( k = 0;
         k < 3;
         k ++ ) {
        if( ! done[ k ] && vals[ k ][ 0 ] != '\0' ) {
            fprintf( f, "%s = %s\n", keys[ k ], vals[ k ] );
            }
        }
    fclose( f );
    ed_message( "Saved compile/debug settings to ./cedit.conf" );
    }



/* ------------------------------------------------------------------
   Minibuffer
   ------------------------------------------------------------------ */

static void clearCands( void ) {
    int  i;

    for( i = 0;
         i < ed.numCands;
         i ++ ) {
        free( ed.cands[ i ] );
        }
    ed.numCands = 0;
    ed.candSel = -1;
    ed.candScroll = 0;
    }



static void addCand( const char  *inS ) {
    if( ed.numCands >= ed.capCands ) {
        ed.capCands = ed.capCands * 2 + 64;
        ed.cands = (char **)realloc( ed.cands, (size_t)ed.capCands
                                     * sizeof( char * ) );
        }
    ed.cands[ ed.numCands ] = xstrdup( inS );
    ed.numCands ++;
    }



static int compareStrings( const void  *inA,
                           const void  *inB ) {
    return strcmp( *(char * const *)inA, *(char * const *)inB );
    }



/* case-insensitive substring */
static int containsFold( const char  *inHay,
                         const char  *inNeedle ) {
    size_t  n  =  strlen( inNeedle );
    size_t  h  =  strlen( inHay );
    size_t  i;
    size_t  j;

    if( n == 0 ) {
        return 1;
        }
    for( i = 0;
         i + n <= h;
         i ++ ) {
        for( j = 0;
             j < n;
             j ++ ) {
            if( tolower( (unsigned char)inHay[ i + j ] )
                != tolower( (unsigned char)inNeedle[ j ] ) ) {
                break;
                }
            }
        if( j == n ) {
            return 1;
            }
        }
    return 0;
    }



static void expandHome( const char  *inPath,
                        char        *outPath,
                        int          inSize ) {
    if( inPath[ 0 ] == '~' && ( inPath[ 1 ] == '/' || inPath[ 1 ] == '\0' )
        && getenv( "HOME" ) != NULL ) {
        sprintf( outPath, "%.500s%.500s", getenv( "HOME" ), inPath + 1 );
        }
    else {
        strncpy( outPath, inPath, (size_t)inSize - 1 );
        outPath[ inSize - 1 ] = '\0';
        }
    }



/* file name candidates for the path in mbText */
static void fileCandidates( void ) {
    char            path[ 1100 ];
    char            dir[ 1100 ];
    const char     *base;
    char           *slash;
    DIR            *d;
    struct dirent  *de;
    size_t          blen;

    clearCands();
    expandHome( ed.mbText, path, sizeof( path ) );
    slash = strrchr( path, '/' );
    if( slash != NULL ) {
        size_t  dl  =  (size_t)( slash - path ) + 1;

        memcpy( dir, path, dl );
        dir[ dl ] = '\0';
        base = slash + 1;
        }
    else {
        strcpy( dir, "./" );
        base = path;
        }
    blen = strlen( base );

    d = opendir( dir );
    if( d == NULL ) {
        return;
        }
    while( ( de = readdir( d ) ) != NULL ) {
        char         full[ 2300 ];
        char         shown[ 600 ];
        struct stat  st;

        if( strcmp( de->d_name, "." ) == 0 || strcmp( de->d_name, ".." ) == 0 ) {
            continue;
            }
        if( strncmp( de->d_name, base, blen ) != 0 ) {
            continue;
            }
        /* hide dotfiles and backups unless asked for */
        if( de->d_name[ 0 ] == '.' && base[ 0 ] != '.' ) {
            continue;
            }
        if( de->d_name[ strlen( de->d_name ) - 1 ] == '~' ) {
            continue;
            }
        {
            size_t  nl  =  strlen( de->d_name );

            if( nl > 2 && strcmp( de->d_name + nl - 2, ".o" ) == 0 ) {
                continue;
                }
            }
        sprintf( full, "%s%s", dir, de->d_name );
        if( stat( full, &st ) == 0 && S_ISDIR( st.st_mode ) ) {
            sprintf( shown, "%.500s/", de->d_name );
            }
        else {
            sprintf( shown, "%.500s", de->d_name );
            }
        addCand( shown );
        }
    closedir( d );
    qsort( ed.cands, (size_t)ed.numCands, sizeof( char * ), compareStrings );
    }



static void bufferCandidates( void ) {
    int  i;

    clearCands();
    /* most recently used first, skipping the current buffer */
    for( i = 1;
         i < ed.numBuffers;
         i ++ ) {
        TextBuffer  *tb  =  ed.buffers[ ed.mru[ i ] ];

        if( containsFold( tb->name, ed.mbText ) ) {
            addCand( tb->name );
            }
        }
    if( containsFold( B()->name, ed.mbText ) ) {
        addCand( B()->name );
        }
    }



static void commandCandidates( void );

static void refreshCandidates( void ) {
    switch( ed.mbMode ) {
        case MB_FIND_FILE:
        case MB_SAVE_AS:
        case MB_DEBUG_PROGRAM:
            fileCandidates();
            break;
        case MB_SWITCH_BUFFER:
            bufferCandidates();
            break;
        case MB_COMMAND:
            commandCandidates();
            break;
        default:
            clearCands();
            break;
        }
    }



static void startPrompt( int          inMode,
                         const char  *inPrompt,
                         const char  *inInitial ) {
    ed.mbMode = inMode;
    scopy( ed.mbPrompt, inPrompt, sizeof( ed.mbPrompt ) );
    scopy( ed.mbText, inInitial, sizeof( ed.mbText ) );
    ed.mbText[ sizeof( ed.mbText ) - 1 ] = '\0';
    ed.mbCursor = (int)strlen( ed.mbText );
    ed.yesNoAction = 0;
    ed.message[ 0 ] = '\0';
    refreshCandidates();
    }



static void cancelPrompt( void ) {
    ed.mbMode = MB_NONE;
    clearCands();
    ed_message( "Quit" );
    }



static void cmdFindFile( void ) {
    char  dir[ 1100 ];

    dir[ 0 ] = '\0';
    if( B()->path != NULL ) {
        char  shown[ 1024 ];
        char  *slash;

        displayPath( B()->path, shown, sizeof( shown ) );
        slash = strrchr( shown, '/' );
        if( slash != NULL ) {
            slash[ 1 ] = '\0';
            strcpy( dir, shown );
            }
        }
    startPrompt( MB_FIND_FILE, "Find file: ", dir );
    }



static void cmdSwitchBuffer( void ) {
    char  prompt[ 600 ];

    if( ed.numBuffers > 1 ) {
        sprintf( prompt, "Switch to buffer (default %.500s): ",
                 ed.buffers[ ed.mru[ 1 ] ]->name );
        }
    else {
        strcpy( prompt, "Switch to buffer: " );
        }
    startPrompt( MB_SWITCH_BUFFER, prompt, "" );
    }



static void cmdNextBuffer( void ) {
    ed_switchToBuffer( ( ed.current + 1 ) % ed.numBuffers );
    }



static void cmdPrevBuffer( void ) {
    ed_switchToBuffer( ( ed.current + ed.numBuffers - 1 ) % ed.numBuffers );
    }



static void cmdLastBuffer( void ) {
    if( ed.numBuffers > 1 ) {
        ed_switchToBuffer( ed.mru[ 1 ] );
        }
    }



static void cmdSave( void ) {
    if( B()->path == NULL ) {
        startPrompt( MB_SAVE_AS, "Write file: ", "" );
        return;
        }
    if( ! tb_isModified( B() ) ) {
        ed_message( "(No changes need to be saved)" );
        return;
        }
    saveBuffer( B() );
    }



static void cmdSaveAs( void ) {
    startPrompt( MB_SAVE_AS, "Write file: ",
                 B()->path != NULL ? B()->path : "" );
    }



static void cmdSaveAll( void ) {
    int  i;
    int  n  =  0;

    for( i = 0;
         i < ed.numBuffers;
         i ++ ) {
        TextBuffer  *tb  =  ed.buffers[ i ];

        if( tb->path != NULL && tb_isModified( tb ) && saveBuffer( tb ) ) {
            n ++;
            }
        }
    ed_message( n == 0 ? "(No files need saving)" : "Saved %d file%s", n,
                n == 1 ? "" : "s" );
    }



#define YN_KILL_BUFFER  1
#define YN_QUIT         2
#define YN_REVERT       3

static void cmdKillBuffer( void ) {
    if( B()->path != NULL && tb_isModified( B() ) ) {
        char  p[ 600 ];

        sprintf( p, "Buffer %.500s modified;  kill anyway? (y or n) ",
                 B()->name );
        startPrompt( MB_YES_NO, p, "" );
        ed.yesNoAction = YN_KILL_BUFFER;
        return;
        }
    killBufferNow( ed.current );
    }



void ed_requestQuit( void ) {
    if( ed_anyModified() ) {
        startPrompt( MB_YES_NO,
                     "Modified buffers exist;  quit anyway? (y or n) ", "" );
        ed.yesNoAction = YN_QUIT;
        return;
        }
    ed.wantQuit = 1;
    }



static void cmdQuit( void ) {
    ed_requestQuit();
    }



static void revertBuffer( void ) {
    TextBuffer  *tb  =  B();
    TextBuffer  *fresh;
    int          l;
    int          c;

    fresh = tb_load( tb->path, config.tabWidth, tb->isC, NULL );
    if( fresh == NULL ) {
        ed_errorMessage( "Can't read %s", tb->path );
        return;
        }
    getCur( &l, &c );
    {
        /* swap contents, keep the TextBuffer object (errors point at it) */
        char  *text;
        int    len;

        text = tb_getText( fresh, 0, 0, fresh->numLines - 1,
                           fresh->lines[ fresh->numLines - 1 ].len, &len );
        tb_setText( tb, text, len );
        free( text );
        tb_clearUndo( tb );
        tb->savedUndoPos = fresh->savedUndoPos == -1 ? -1 : 0;
        tb->fileMTime = fileMTime( tb->path );
        tb_destroy( fresh );
        }
    setCur( l, c );
    ed_message( "Reverted %s", tb->path );
    }



static void cmdRevert( void ) {
    if( B()->path == NULL ) {
        return;
        }
    if( tb_isModified( B() ) ) {
        startPrompt( MB_YES_NO, "Discard changes and revert? (y or n) ", "" );
        ed.yesNoAction = YN_REVERT;
        return;
        }
    revertBuffer();
    }



static void cmdGotoLine( void ) {
    startPrompt( MB_GOTO_LINE, "Goto line: ", "" );
    }



static void cmdExecuteCommand( void ) {
    startPrompt( MB_COMMAND, "M-x ", "" );
    }



static void cmdCancel( void ) {
    deactivateMark();
    shiftSelecting = 0;
    ed.prefixCtrlX = 0;
    ed.prefixCtrlC = 0;
    ed.prefixMetaG = 0;
    ed_message( "Quit" );
    }



static void cmdFontBigger( void ) {
    ed.fontSize += 1.0f;
    if( ed.fontSize > 96 ) {
        ed.fontSize = 96;
        }
    ed_message( "Font size %d", (int)ed.fontSize );
    }



static void cmdFontSmaller( void ) {
    ed.fontSize -= 1.0f;
    if( ed.fontSize < 6 ) {
        ed.fontSize = 6;
        }
    ed_message( "Font size %d", (int)ed.fontSize );
    }



static void cmdFontReset( void ) {
    ed.fontSize = (float)config.fontSize;
    ed_message( "Font size %d", (int)ed.fontSize );
    }



static void cmdReloadConfig( void ) {
    config_load();
    ed.fontSize = (float)config.fontSize;
    {
        int  i;

        for( i = 0;
             i < ed.numBuffers;
             i ++ ) {
            syn_scanTypedefs( ed.buffers[ i ] );
            }
        }
    if( config_lastWarning()[ 0 ] != '\0' ) {
        ed_errorMessage( "%s", config_lastWarning() );
        }
    else {
        ed_message( "Reloaded configuration" );
        }
    }



static void cmdWriteDefaultConfig( void ) {
    char  msg[ 1024 ];

    config_writeDefaults( config_homeDir(), msg, sizeof( msg ) );
    ed_message( "%s", msg );
    }



static void cmdRescanTypes( void ) {
    int  i;

    syn_clearScannedTypes();
    for( i = 0;
         i < ed.numBuffers;
         i ++ ) {
        syn_scanTypedefs( ed.buffers[ i ] );
        }
    ed_message( "Rescanned typedefs in %d buffers", ed.numBuffers );
    }



static void cmdToggleLineNumbers( void ) {
    config.showLineNumbers = ! config.showLineNumbers;
    }



static void cmdToggleAutoNewline( void ) {
    config.autoNewline = ! config.autoNewline;
    ed_message( "Auto-newline %s", config.autoNewline ? "on" : "off" );
    }



static void cmdToggleHungry( void ) {
    config.hungryDelete = ! config.hungryDelete;
    ed_message( "Hungry delete %s", config.hungryDelete ? "on" : "off" );
    }



static void cmdToggleFormat( void ) {
    config.formatOnEnter = ! config.formatOnEnter;
    ed_message( "Format as you type %s", config.formatOnEnter ? "on" : "off" );
    }



static void cmdCountWords( void ) {
    int  sl;
    int  sc;
    int  el;
    int  ec;
    long words  =  0;
    long chars  =  0;
    int  l;

    if( ! ed_getRegion( &sl, &sc, &el, &ec ) ) {
        sl = 0;
        sc = 0;
        el = B()->numLines - 1;
        ec = lineAt( el )->len;
        }
    for( l = sl;
         l <= el;
         l ++ ) {
        Line  *ln  =  lineAt( l );
        int    s   =  l == sl ? sc : 0;
        int    e   =  l == el ? ec : ln->len;
        int    i;
        int    inWord  =  0;

        for( i = s;
             i < e;
             i ++ ) {
            int  w  =  isalnum( (unsigned char)ln->text[ i ] );

            if( w && ! inWord ) {
                words ++;
                }
            inWord = w;
            }
        chars += e - s + ( l < el ? 1 : 0 );
        }
    ed_message( "%s has %d lines, %ld words, %ld characters",
                B()->markActive ? "Region" : "Buffer", el - sl + 1, words,
                chars );
    }



static void cmdHelp( void );



/* ------------------------------------------------------------------
   Command table
   ------------------------------------------------------------------ */

typedef struct NamedCommand {
        const char  *name;
        CmdFn        fn;
        const char  *keys;
    } NamedCommand;


static NamedCommand  commands[]  =  {
    { "find-file", cmdFindFile, "C-x C-f" },
    { "save-buffer", cmdSave, "C-x C-s" },
    { "save-some-buffers", cmdSaveAll, "C-x s" },
    { "write-file", cmdSaveAs, "C-x C-w" },
    { "switch-to-buffer", cmdSwitchBuffer, "C-x b" },
    { "next-buffer", cmdNextBuffer, "C-Tab  C-PgDn" },
    { "previous-buffer", cmdPrevBuffer, "C-S-Tab  C-PgUp" },
    { "last-buffer", cmdLastBuffer, "C-x C-b" },
    { "kill-buffer", cmdKillBuffer, "C-x k" },
    { "revert-buffer", cmdRevert, "" },
    { "goto-line", cmdGotoLine, "M-g g" },
    { "compile", cmdCompile, "C-c C-c" },
    { "recompile", cmdRecompile, "F5" },
    { "kill-compilation", cmdKillCompile, "" },
    { "kill-whole-line", cmdKillWholeLine, "C-S-k  C-S-Backspace" },
    { "next-error", cmdNextError, "C-x `  M-g n  F4" },
    { "previous-error", cmdPrevError, "M-g p  S-F4" },
    { "first-error", cmdFirstError, "" },
    { "toggle-compile-pane", cmdToggleCompilePane, "C-c C-o" },
    { "hide-compile-pane", cmdHideCompilePane, "C-x 1" },
    { "debug", cmdDebug, "C-c C-d  F9" },
    { "set-debug-program", cmdSetDebugProgram, "" },
    { "set-debug-command", cmdSetDebugCommand, "" },
    { "save-folder-config", cmdSaveFolderConfig, "" },
    { "indent-region", cmdIndentRegion, "C-M-\\" },
    { "indent-buffer", cmdIndentBuffer, "" },
    { "format-region", cmdFormatRegion, "C-c C-f" },
    { "format-buffer", cmdFormatBuffer, "" },
    { "align-declarations", cmdAlignDeclarations, "C-c C-a" },
    { "align-to-next-column", cmdAlignToNextColumn, "M-i" },
    { "query-replace", cmdQueryReplace, "M-%" },
    { "dabbrev-expand", cmdDabbrev, "M-/" },
    { "undo", cmdUndo, "C-/  C-_  C-z" },
    { "redo", cmdRedo, "C-?  C-S-z" },
    { "mark-whole-buffer", cmdSelectAll, "C-x h" },
    { "count-words", cmdCountWords, "M-=" },
    { "reload-config", cmdReloadConfig, "" },
    { "write-default-config", cmdWriteDefaultConfig, "" },
    { "rescan-types", cmdRescanTypes, "" },
    { "toggle-line-numbers", cmdToggleLineNumbers, "" },
    { "toggle-auto-newline", cmdToggleAutoNewline, "" },
    { "toggle-hungry-delete", cmdToggleHungry, "" },
    { "toggle-format-as-you-type", cmdToggleFormat, "" },
    { "font-bigger", cmdFontBigger, "C-=" },
    { "font-smaller", cmdFontSmaller, "C--" },
    { "font-reset", cmdFontReset, "C-0" },
    { "help", cmdHelp, "F1  C-h b" },
    { "quit", cmdQuit, "C-x C-c" },
    { NULL, NULL, NULL }
    };



static void commandCandidates( void ) {
    int  i;

    clearCands();
    for( i = 0;
         commands[ i ].name != NULL;
         i ++ ) {
        if( containsFold( commands[ i ].name, ed.mbText ) ) {
            addCand( commands[ i ].name );
            }
        }
    }



static const char  *helpLines[]  =  {
    "cedit -- a C89 editor",
    "=====================",
    "",
    "Keys are emacs-style.  C- is Ctrl, M- is Alt, S- is Shift.",
    "Shift + any movement key selects.  Typing replaces the selection.",
    "",
    "Moving",
    "  C-f C-b C-n C-p / arrows     char / line",
    "  M-f M-b / C-Left C-Right     word",
    "  C-a C-e / Home End           line start / end (Home toggles indent)",
    "  M-m                          back to indentation",
    "  C-v M-v / PgDn PgUp          page",
    "  M-< M-> / C-Home C-End       buffer start / end",
    "  M-{ M-}                      paragraph (blank-line separated)",
    "  C-M-f C-M-b                  across a ( ) [ ] { } group",
    "  C-l                          recenter",
    "  M-g g                        goto line",
    "",
    "Editing",
    "  Tab                          re-indent + format line (or indent",
    "                               region);  on \"int  \" jumps to where",
    "                               the name lines up in the table",
    "  C-M-\\                        re-indent region, or current function",
    "  Enter                        newline, indent, format the line left",
    "  C-d Del / Backspace          delete (hungry:  eats all whitespace)",
    "  S-Backspace                  delete a single char",
    "  C-k                          kill line",
    "  C-S-k / C-S-Backspace        kill whole line (appends when repeated)",
    "  M-d / C-Backspace M-Backsp.  kill word forward / backward",
    "  C-Space                      set mark",
    "  C-w M-w C-y M-y              kill region, copy, yank, yank-pop",
    "  C-x C-x  C-x h               exchange point/mark, select all",
    "  C-/ C-_ C-z  /  C-? C-S-z    undo / redo",
    "  M-/                          complete word from buffers (repeat cycles)",
    "  M-i                          align to next column of previous line",
    "  C-c C-a                      align declarations into a table",
    "  C-c C-f                      format region / current function",
    "  C-o                          open line",
    "",
    "Searching",
    "  C-s C-r                      incremental search (C-w adds word)",
    "  M-%                          query replace (y n ! . q)",
    "",
    "Files and buffers",
    "  C-x C-f                      find file (Tab completes)",
    "  C-x C-s  C-x s  C-x C-w      save, save all, save as",
    "  C-x b                        switch buffer (default: previous)",
    "  C-x C-b                      flip to previous buffer",
    "  C-Tab C-S-Tab / C-PgDn PgUp  next / previous buffer",
    "  M-1 .. M-9                   buffer by position in tab bar",
    "  C-x k                        kill buffer",
    "  C-x C-c                      quit",
    "",
    "Compile and debug",
    "  C-c C-c                      compile (edit the command)",
    "  F5                           recompile",
    "  C-x `  M-g n  F4             next error",
    "  M-g p  S-F4                  previous error",
    "  C-c C-o  C-x 1               toggle / hide compile pane",
    "  C-c C-d  F9                  debug at the current line",
    "",
    "Other",
    "  M-x                          run any command by name",
    "  C-= C-- C-0                  font size",
    "  C-g                          cancel",
    "",
    "Configuration (key = value files, later ones override):",
    "  ~/.cedit/colors.conf  ~/.cedit/style.conf  ~/.cedit/cedit.conf",
    "  ./cedit.conf          (per folder:  compileCommand, debugCommand, ...)",
    "  M-x write-default-config writes commented defaults to ~/.cedit/",
    "  M-x save-folder-config saves the compile/debug commands to ./cedit.conf",
    "",
    "All commands (M-x name):",
    NULL };



static void cmdHelp( void ) {
    char  *text;
    size_t cap  =  16384;
    int    i;
    int    idx;

    for( i = 0;
         i < ed.numBuffers;
         i ++ ) {
        if( ed.buffers[ i ]->name != NULL
            && strcmp( ed.buffers[ i ]->name, "*help*" ) == 0 ) {
            ed_switchToBuffer( i );
            return;
            }
        }
    for( i = 0;
         helpLines[ i ] != NULL;
         i ++ ) {
        cap += strlen( helpLines[ i ] ) + 1;
        }
    text = (char *)malloc( cap );
    text[ 0 ] = '\0';
    for( i = 0;
         helpLines[ i ] != NULL;
         i ++ ) {
        strcat( text, helpLines[ i ] );
        strcat( text, "\n" );
        }
    for( i = 0;
         commands[ i ].name != NULL;
         i ++ ) {
        sprintf( text + strlen( text ), "  %-28s %s\n", commands[ i ].name,
                 commands[ i ].keys );
        }
    idx = makeScratch( "*help*", text );
    ed.buffers[ idx ]->isC = 0;
    free( text );
    ed.buffers[ idx ]->readOnly = 1;
    ed_switchToBuffer( idx );
    setCur( 0, 0 );
    }



/* ------------------------------------------------------------------
   Minibuffer input
   ------------------------------------------------------------------ */

static void mbInsert( const char  *inText ) {
    int  n  =  (int)strlen( inText );
    int  l  =  (int)strlen( ed.mbText );

    if( l + n >= MB_TEXT_MAX - 1 ) {
        return;
        }
    memmove( ed.mbText + ed.mbCursor + n, ed.mbText + ed.mbCursor,
             (size_t)( l - ed.mbCursor + 1 ) );
    memcpy( ed.mbText + ed.mbCursor, inText, (size_t)n );
    ed.mbCursor += n;
    refreshCandidates();
    }



static void mbComplete( void ) {
    /* longest common prefix of candidates (prefix-matching ones for
       files;  for buffers/commands, if just one candidate take it) */
    int   i;
    int   lcp;

    if( ed.numCands == 0 ) {
        ed_errorMessage( "[No match]" );
        return;
        }
    if( ed.mbMode == MB_FIND_FILE || ed.mbMode == MB_SAVE_AS
        || ed.mbMode == MB_DEBUG_PROGRAM ) {
        char  *slash  =  strrchr( ed.mbText, '/' );
        int    baseAt =  slash != NULL ? (int)( slash - ed.mbText ) + 1 : 0;

        lcp = (int)strlen( ed.cands[ 0 ] );
        for( i = 1;
             i < ed.numCands;
             i ++ ) {
            int  k  =  0;

            while( k < lcp && ed.cands[ i ][ k ] == ed.cands[ 0 ][ k ] ) {
                k ++;
                }
            lcp = k;
            }
        if( baseAt + lcp < MB_TEXT_MAX - 1 ) {
            memcpy( ed.mbText + baseAt, ed.cands[ 0 ], (size_t)lcp );
            ed.mbText[ baseAt + lcp ] = '\0';
            ed.mbCursor = (int)strlen( ed.mbText );
            }
        refreshCandidates();
        if( ed.numCands > 1 ) {
            ed.candSel = -1;
            }
        return;
        }
    /* buffers / commands:  common prefix of candidates that start with
       the typed text, else the first candidate */
    if( ed.numCands == 1 ) {
        strcpy( ed.mbText, ed.cands[ 0 ] );
        }
    else {
        lcp = (int)strlen( ed.cands[ 0 ] );
        for( i = 1;
             i < ed.numCands;
             i ++ ) {
            int  k  =  0;

            while( k < lcp && ed.cands[ i ][ k ] == ed.cands[ 0 ][ k ] ) {
                k ++;
                }
            lcp = k;
            }
        if( lcp > (int)strlen( ed.mbText ) ) {
            memcpy( ed.mbText, ed.cands[ 0 ], (size_t)lcp );
            ed.mbText[ lcp ] = '\0';
            }
        }
    ed.mbCursor = (int)strlen( ed.mbText );
    refreshCandidates();
    }



static void runNamedCommand( const char  *inName ) {
    int  i;

    for( i = 0;
         commands[ i ].name != NULL;
         i ++ ) {
        if( strcmp( commands[ i ].name, inName ) == 0 ) {
            tb_beginGroup( B() );
            commands[ i ].fn();
            syncCur();
            return;
            }
        }
    ed_errorMessage( "No command named %s", inName );
    }



static void mbAccept( void ) {
    int    mode  =  ed.mbMode;
    char   text[ MB_TEXT_MAX ];
    char   path[ 1100 ];

    if( ed.candSel >= 0 && ed.candSel < ed.numCands ) {
        if( mode == MB_FIND_FILE || mode == MB_SAVE_AS
            || mode == MB_DEBUG_PROGRAM ) {
            char  *slash  =  strrchr( ed.mbText, '/' );
            int    baseAt =  slash != NULL ?
                             (int)( slash - ed.mbText ) + 1 : 0;

            sprintf( ed.mbText + baseAt, "%.900s", ed.cands[ ed.candSel ] );
            ed.mbCursor = (int)strlen( ed.mbText );
            }
        else {
            strcpy( ed.mbText, ed.cands[ ed.candSel ] );
            }
        }
    strcpy( text, ed.mbText );

    /* a directory chosen in find-file:  descend instead of opening */
    if( mode == MB_FIND_FILE || mode == MB_DEBUG_PROGRAM ) {
        size_t  n  =  strlen( text );

        if( n > 0 && text[ n - 1 ] == '/' && ed.candSel >= 0 ) {
            ed.mbCursor = (int)n;
            refreshCandidates();
            return;
            }
        }

    ed.mbMode = MB_NONE;
    clearCands();
    ed.message[ 0 ] = '\0';

    switch( mode ) {
        case MB_FIND_FILE:
            if( text[ 0 ] == '\0' ) {
                return;
                }
            expandHome( text, path, sizeof( path ) );
            openFile( path, 1 );
            break;
        case MB_SAVE_AS:
            if( text[ 0 ] == '\0' ) {
                return;
                }
            expandHome( text, path, sizeof( path ) );
            free( B()->path );
            B()->path = xstrdup( path );
            B()->isC = tb_isCPath( path );
            setUniqueName( B(), baseName( path ) );
            B()->readOnly = 0;
            saveBuffer( B() );
            break;
        case MB_SWITCH_BUFFER: {
            int  i;
            int  found  =  -1;

            if( text[ 0 ] == '\0' ) {
                cmdLastBuffer();
                return;
                }
            for( i = 0;
                 i < ed.numBuffers;
                 i ++ ) {
                if( strcmp( ed.buffers[ i ]->name, text ) == 0 ) {
                    found = i;
                    }
                }
            if( found < 0 ) {
                /* first fuzzy candidate */
                strcpy( ed.mbText, text );
                bufferCandidates();
                if( ed.numCands > 0 ) {
                    for( i = 0;
                         i < ed.numBuffers;
                         i ++ ) {
                        if( strcmp( ed.buffers[ i ]->name,
                                    ed.cands[ 0 ] ) == 0 ) {
                            found = i;
                            }
                        }
                    }
                clearCands();
                }
            if( found >= 0 ) {
                ed_switchToBuffer( found );
                }
            else {
                ed_errorMessage( "No buffer named %s", text );
                }
            break;
            }
        case MB_COMMAND: {
            int  i;

            for( i = 0;
                 commands[ i ].name != NULL;
                 i ++ ) {
                if( strcmp( commands[ i ].name, text ) == 0 ) {
                    break;
                    }
                }
            if( commands[ i ].name == NULL ) {
                strcpy( ed.mbText, text );
                commandCandidates();
                if( ed.numCands > 0 ) {
                    strcpy( text, ed.cands[ 0 ] );
                    }
                clearCands();
                }
            runNamedCommand( text );
            break;
            }
        case MB_GOTO_LINE: {
            int  n  =  atoi( text );

            if( n >= 1 ) {
                gotoLine( n - 1 );
                }
            break;
            }
        case MB_COMPILE:
            if( text[ 0 ] != '\0' ) {
                runCompile( text );
                }
            break;
        case MB_DEBUG_COMMAND:
            scopy( ed.debugCommand, text, sizeof( ed.debugCommand ) );
            if( ed.yesNoAction == 0 ) {
                cmdDebug();
                }
            else {
                ed_message( "Debug command set (M-x save-folder-config "
                            "to keep it)" );
                }
            break;
        case MB_DEBUG_PROGRAM:
            scopy( ed.debugProgram, text, sizeof( ed.debugProgram ) );
            if( ed.yesNoAction == 0 ) {
                cmdDebug();
                }
            else {
                ed_message( "Debug program set to %s", text );
                }
            break;
        case MB_REPLACE_FROM:
            if( text[ 0 ] == '\0' ) {
                return;
                }
            scopy( ed.replaceFrom, text, sizeof( ed.replaceFrom ) );
            {
                char  p[ 400 ];

                sprintf( p, "Query replace %.200s with: ", ed.replaceFrom );
                startPrompt( MB_REPLACE_TO, p, "" );
                }
            break;
        case MB_REPLACE_TO:
            scopy( ed.replaceTo, text, sizeof( ed.replaceTo ) );
            ed.replaceCount = 0;
            tb_beginGroup( B() );
            queryReplaceNext();
            break;
        default:
            break;
        }
    }



static void yesNoAnswer( int  inYes ) {
    int  action  =  ed.yesNoAction;

    ed.mbMode = MB_NONE;
    if( ! inYes ) {
        ed_message( "Cancelled" );
        return;
        }
    switch( action ) {
        case YN_KILL_BUFFER:
            killBufferNow( ed.current );
            break;
        case YN_QUIT:
            ed.wantQuit = 1;
            break;
        case YN_REVERT:
            revertBuffer();
            break;
        default:
            break;
        }
    }



static int mbKey( int  inKey,
                  int  inMods ) {
    int  len  =  (int)strlen( ed.mbText );
    int  ctrl =  ( inMods & MOD_CTRL ) != 0;
    int  alt  =  ( inMods & MOD_ALT ) != 0;

    if( ed.mbMode == MB_YES_NO ) {
        if( inKey == SDLK_y ) {
            yesNoAnswer( 1 );
            }
        else if( inKey == SDLK_n || inKey == SDLK_ESCAPE
                 || ( ctrl && inKey == SDLK_g ) ) {
            yesNoAnswer( 0 );
            }
        return 1;
        }
    if( ed.mbMode == MB_QUERY_REPLACE ) {
        if( inKey == SDLK_ESCAPE || inKey == SDLK_RETURN
            || ( ctrl && inKey == SDLK_g ) ) {
            queryReplaceKey( "q" );
            return 1;
            }
        if( inKey == SDLK_DELETE || inKey == SDLK_BACKSPACE ) {
            queryReplaceKey( "n" );
            return 1;
            }
        /* y n ! . q arrive as text input */
        return 0;
        }

    if( ( ctrl && inKey == SDLK_g ) || inKey == SDLK_ESCAPE ) {
        cancelPrompt();
        return 1;
        }
    if( inKey == SDLK_RETURN || inKey == SDLK_KP_ENTER ) {
        mbAccept();
        return 1;
        }
    if( inKey == SDLK_TAB ) {
        mbComplete();
        return 1;
        }
    if( inKey == SDLK_DOWN || ( ctrl && inKey == SDLK_n ) ) {
        if( ed.numCands > 0 ) {
            ed.candSel = ( ed.candSel + 1 ) % ed.numCands;
            }
        return 1;
        }
    if( inKey == SDLK_UP || ( ctrl && inKey == SDLK_p ) ) {
        if( ed.numCands > 0 ) {
            ed.candSel = ed.candSel <= 0 ? ed.numCands - 1 : ed.candSel - 1;
            }
        return 1;
        }
    if( inKey == SDLK_BACKSPACE ) {
        if( ctrl || alt ) {
            /* delete back to previous '/' or word boundary */
            int  p  =  ed.mbCursor;

            if( p > 0 && ed.mbText[ p - 1 ] == '/' ) {
                p --;
                }
            while( p > 0 && ed.mbText[ p - 1 ] != '/'
                   && ed.mbText[ p - 1 ] != ' ' ) {
                p --;
                }
            memmove( ed.mbText + p, ed.mbText + ed.mbCursor,
                     (size_t)( len - ed.mbCursor + 1 ) );
            ed.mbCursor = p;
            }
        else if( ed.mbCursor > 0 ) {
            memmove( ed.mbText + ed.mbCursor - 1, ed.mbText + ed.mbCursor,
                     (size_t)( len - ed.mbCursor + 1 ) );
            ed.mbCursor --;
            }
        refreshCandidates();
        return 1;
        }
    if( inKey == SDLK_DELETE || ( ctrl && inKey == SDLK_d ) ) {
        if( ed.mbCursor < len ) {
            memmove( ed.mbText + ed.mbCursor, ed.mbText + ed.mbCursor + 1,
                     (size_t)( len - ed.mbCursor ) );
            refreshCandidates();
            }
        return 1;
        }
    if( inKey == SDLK_LEFT || ( ctrl && inKey == SDLK_b ) ) {
        if( ed.mbCursor > 0 ) {
            ed.mbCursor --;
            }
        return 1;
        }
    if( inKey == SDLK_RIGHT || ( ctrl && inKey == SDLK_f ) ) {
        if( ed.mbCursor < len ) {
            ed.mbCursor ++;
            }
        return 1;
        }
    if( inKey == SDLK_HOME || ( ctrl && inKey == SDLK_a ) ) {
        ed.mbCursor = 0;
        return 1;
        }
    if( inKey == SDLK_END || ( ctrl && inKey == SDLK_e ) ) {
        ed.mbCursor = len;
        return 1;
        }
    if( ctrl && inKey == SDLK_k ) {
        ed.mbText[ ed.mbCursor ] = '\0';
        refreshCandidates();
        return 1;
        }
    if( ctrl && inKey == SDLK_y ) {
        char  *clip  =  SDL_GetClipboardText();

        if( clip != NULL ) {
            char  *nl  =  strchr( clip, '\n' );

            if( nl != NULL ) {
                *nl = '\0';
                }
            mbInsert( clip );
            SDL_free( clip );
            }
        return 1;
        }
    return 0;
    }



/* ------------------------------------------------------------------
   Key dispatch
   ------------------------------------------------------------------ */

typedef struct KeyBinding {
        int    mods;
        int    key;
        CmdFn  fn;
    } KeyBinding;


#define C   MOD_CTRL
#define M   MOD_ALT
#define S   MOD_SHIFT


static void cmdMarkOrCancel( void );


static KeyBinding  globalKeys[]  =  {
    { C, SDLK_f, cmdForwardChar },
    { C, SDLK_b, cmdBackwardChar },
    { C, SDLK_n, cmdNextLine },
    { C, SDLK_p, cmdPrevLine },
    { C, SDLK_a, cmdLineStart },
    { C, SDLK_e, cmdLineEnd },
    { M, SDLK_m, cmdBackToIndent },
    { M, SDLK_f, cmdForwardWord },
    { M, SDLK_b, cmdBackwardWord },
    { C, SDLK_RIGHT, cmdForwardWord },
    { C, SDLK_LEFT, cmdBackwardWord },
    { M, SDLK_RIGHT, cmdForwardWord },
    { M, SDLK_LEFT, cmdBackwardWord },
    { C, SDLK_UP, cmdBackwardParagraph },
    { C, SDLK_DOWN, cmdForwardParagraph },
    { 0, SDLK_RIGHT, cmdForwardChar },
    { 0, SDLK_LEFT, cmdBackwardChar },
    { 0, SDLK_DOWN, cmdNextLine },
    { 0, SDLK_UP, cmdPrevLine },
    { 0, SDLK_HOME, cmdSmartHome },
    { 0, SDLK_END, cmdLineEnd },
    { C, SDLK_HOME, cmdBufferStart },
    { C, SDLK_END, cmdBufferEnd },
    { 0, SDLK_PAGEDOWN, cmdPageDown },
    { 0, SDLK_PAGEUP, cmdPageUp },
    { C, SDLK_v, cmdPageDown },
    { M, SDLK_v, cmdPageUp },
    { C, SDLK_l, cmdRecenter },
    { M | S, SDLK_COMMA, cmdBufferStart },
    { M | S, SDLK_PERIOD, cmdBufferEnd },
    { M, SDLK_LESS, cmdBufferStart },
    { M, SDLK_GREATER, cmdBufferEnd },
    { M | S, SDLK_RIGHTBRACKET, cmdForwardParagraph },
    { M | S, SDLK_LEFTBRACKET, cmdBackwardParagraph },
    { C | M, SDLK_f, cmdForwardSexp },
    { C | M, SDLK_b, cmdBackwardSexp },

    { 0, SDLK_RETURN, cmdNewline },
    { 0, SDLK_KP_ENTER, cmdNewline },
    { 0, SDLK_TAB, cmdTab },
    { 0, SDLK_BACKSPACE, cmdBackspace },
    { S, SDLK_BACKSPACE, cmdBackspaceSingle },
    { 0, SDLK_DELETE, cmdDeleteChar },
    { C, SDLK_d, cmdDeleteChar },
    { C, SDLK_k, cmdKillLine },
    { C | S, SDLK_k, cmdKillWholeLine },
    { C | S, SDLK_BACKSPACE, cmdKillWholeLine },
    { M, SDLK_d, cmdKillWordForward },
    { C, SDLK_DELETE, cmdKillWordForward },
    { C, SDLK_BACKSPACE, cmdKillWordBackward },
    { M, SDLK_BACKSPACE, cmdKillWordBackward },
    { C, SDLK_SPACE, cmdMarkOrCancel },
    { C, SDLK_w, cmdKillRegion },
    { M, SDLK_w, cmdCopyRegion },
    { C, SDLK_y, cmdYank },
    { M, SDLK_y, cmdYankPop },
    { C, SDLK_SLASH, cmdUndo },
    { C, SDLK_UNDERSCORE, cmdUndo },
    { C | S, SDLK_MINUS, cmdUndo },
    { C, SDLK_z, cmdUndo },
    { C | S, SDLK_SLASH, cmdRedo },
    { C | S, SDLK_z, cmdRedo },
    { M, SDLK_SLASH, cmdDabbrev },
    { M, SDLK_i, cmdAlignToNextColumn },
    { C | M, SDLK_BACKSLASH, cmdIndentRegion },
    { C, SDLK_o, cmdOpenLine },
    { C, SDLK_g, cmdCancel },
    { 0, SDLK_ESCAPE, cmdCancel },
    { M | S, SDLK_5, cmdQueryReplace },
    { M, SDLK_PERCENT, cmdQueryReplace },
    { M, SDLK_x, cmdExecuteCommand },
    { M, SDLK_EQUALS, cmdCountWords },

    { C, SDLK_TAB, cmdNextBuffer },
    { C | S, SDLK_TAB, cmdPrevBuffer },
    { C, SDLK_PAGEDOWN, cmdNextBuffer },
    { C, SDLK_PAGEUP, cmdPrevBuffer },

    { 0, SDLK_F1, cmdHelp },
    { 0, SDLK_F4, cmdNextError },
    { S, SDLK_F4, cmdPrevError },
    { 0, SDLK_F5, cmdRecompile },
    { 0, SDLK_F9, cmdDebug },

    { C, SDLK_EQUALS, cmdFontBigger },
    { C | S, SDLK_EQUALS, cmdFontBigger },
    { C, SDLK_PLUS, cmdFontBigger },
    { C, SDLK_KP_PLUS, cmdFontBigger },
    { C, SDLK_MINUS, cmdFontSmaller },
    { C, SDLK_KP_MINUS, cmdFontSmaller },
    { C, SDLK_0, cmdFontReset },
    { 0, 0, NULL }
    };


static KeyBinding  ctrlXKeys[]  =  {
    { C, SDLK_f, cmdFindFile },
    { C, SDLK_s, cmdSave },
    { 0, SDLK_s, cmdSaveAll },
    { C, SDLK_w, cmdSaveAs },
    { 0, SDLK_b, cmdSwitchBuffer },
    { C, SDLK_b, cmdLastBuffer },
    { 0, SDLK_k, cmdKillBuffer },
    { C, SDLK_c, cmdQuit },
    { C, SDLK_x, cmdExchangePointMark },
    { 0, SDLK_h, cmdSelectAll },
    { 0, SDLK_1, cmdHideCompilePane },
    { 0, SDLK_BACKQUOTE, cmdNextError },
    { 0, SDLK_RIGHT, cmdNextBuffer },
    { 0, SDLK_LEFT, cmdPrevBuffer },
    { 0, SDLK_u, cmdUndo },
    { 0, 0, NULL }
    };


static KeyBinding  ctrlCKeys[]  =  {
    { C, SDLK_c, cmdCompile },
    { C, SDLK_d, cmdDebug },
    { C, SDLK_o, cmdToggleCompilePane },
    { C, SDLK_f, cmdFormatRegion },
    { C, SDLK_a, cmdAlignDeclarations },
    { C, SDLK_k, cmdKillCompile },
    { 0, 0, NULL }
    };


static KeyBinding  metaGKeys[]  =  {
    { 0, SDLK_g, cmdGotoLine },
    { M, SDLK_g, cmdGotoLine },
    { 0, SDLK_n, cmdNextError },
    { M, SDLK_n, cmdNextError },
    { 0, SDLK_p, cmdPrevError },
    { M, SDLK_p, cmdPrevError },
    { 0, 0, NULL }
    };

#undef C
#undef M
#undef S



static void cmdMarkOrCancel( void ) {
    cmdSetMark();
    }



static int isMovement( CmdFn  inFn ) {
    return inFn == cmdForwardChar || inFn == cmdBackwardChar
           || inFn == cmdNextLine || inFn == cmdPrevLine
           || inFn == cmdLineStart || inFn == cmdLineEnd
           || inFn == cmdSmartHome || inFn == cmdBackToIndent
           || inFn == cmdForwardWord || inFn == cmdBackwardWord
           || inFn == cmdBufferStart || inFn == cmdBufferEnd
           || inFn == cmdPageDown || inFn == cmdPageUp
           || inFn == cmdForwardParagraph || inFn == cmdBackwardParagraph
           || inFn == cmdForwardSexp || inFn == cmdBackwardSexp;
    }



static CmdFn lookupKey( KeyBinding  *inTable,
                        int          inKey,
                        int          inMods,
                        int          inIgnoreShift ) {
    int  i;

    for( i = 0;
         inTable[ i ].fn != NULL;
         i ++ ) {
        int  m  =  inMods;

        if( inIgnoreShift ) {
            m &= ~ MOD_SHIFT;
            }
        if( inTable[ i ].key == inKey && inTable[ i ].mods == m ) {
            return inTable[ i ].fn;
            }
        }
    return NULL;
    }



static void runCommand( CmdFn  inFn ) {
    thisCmd = inFn;
    if( inFn != cmdUndo && inFn != cmdRedo ) {
        tb_beginGroup( B() );
        }
    syncCur();
    inFn();
    syncCur();
    lastCmd = thisCmd;
    if( inFn != cmdSelfInsert ) {
        typedInGroup = 0;
        }
    }



int ed_keyDown( int  inKey,
                int  inMods ) {
    CmdFn  fn       =  NULL;
    int    shifted  =  ( inMods & MOD_SHIFT ) != 0;

    /* ignore bare modifier presses */
    if( inKey == SDLK_LSHIFT || inKey == SDLK_RSHIFT || inKey == SDLK_LCTRL
        || inKey == SDLK_RCTRL || inKey == SDLK_LALT || inKey == SDLK_RALT
        || inKey == SDLK_LGUI || inKey == SDLK_RGUI || inKey == SDLK_CAPSLOCK ) {
        return 0;
        }

    if( ed.mbMode == MB_ISEARCH ) {
        int  r  =  isearchKey( inKey, inMods );

        if( r == 2 ) {
            /* printable:  arrives as text input */
            return 0;
            }
        if( r ) {
            return 1;
            }
        }
    else if( ed.mbMode != MB_NONE ) {
        return mbKey( inKey, inMods );
        }

    ed.message[ 0 ] = '\0';
    ed.messageIsError = 0;

    if( ed.prefixCtrlX || ed.prefixCtrlC || ed.prefixMetaG ) {
        KeyBinding  *table  =  ed.prefixCtrlX ? ctrlXKeys :
                               ( ed.prefixCtrlC ? ctrlCKeys : metaGKeys );
        const char  *name   =  ed.prefixCtrlX ? "C-x" :
                               ( ed.prefixCtrlC ? "C-c" : "M-g" );

        ed.prefixCtrlX = 0;
        ed.prefixCtrlC = 0;
        ed.prefixMetaG = 0;
        fn = lookupKey( table, inKey, inMods, 0 );
        if( fn == NULL ) {
            fn = lookupKey( table, inKey, inMods, 1 );
            }
        if( fn == NULL ) {
            if( ( inMods & MOD_CTRL ) && inKey == SDLK_g ) {
                ed_message( "Quit" );
                }
            else {
                ed_errorMessage( "%s %s%s is undefined", name,
                                 ( inMods & MOD_CTRL ) ? "C-" : "",
                                 SDL_GetKeyName( (SDL_Keycode)inKey ) );
                }
            return 1;
            }
        shiftSelect = 0;
        runCommand( fn );
        return 1;
        }

    if( inMods == MOD_CTRL && inKey == SDLK_x ) {
        ed.prefixCtrlX = 1;
        ed_message( "C-x-" );
        return 1;
        }
    if( inMods == MOD_CTRL && inKey == SDLK_c ) {
        ed.prefixCtrlC = 1;
        ed_message( "C-c-" );
        return 1;
        }
    if( inMods == MOD_ALT && inKey == SDLK_g ) {
        ed.prefixMetaG = 1;
        ed_message( "M-g-" );
        return 1;
        }
    if( inMods == MOD_CTRL && inKey == SDLK_h ) {
        /* C-h b / C-h k etc:  all show help */
        ed.prefixCtrlX = 0;
        runCommand( cmdHelp );
        return 1;
        }
    if( inMods == MOD_CTRL && ( inKey == SDLK_s || inKey == SDLK_r ) ) {
        cmdISearch( inKey == SDLK_s );
        return 1;
        }
    if( inMods == MOD_ALT && inKey >= SDLK_1 && inKey <= SDLK_9 ) {
        int  n  =  inKey - SDLK_1;

        if( n < ed.numBuffers ) {
            ed_switchToBuffer( n );
            }
        return 1;
        }

    fn = lookupKey( globalKeys, inKey, inMods, 0 );
    shiftSelect = 0;
    if( fn == NULL && shifted ) {
        /* shift + movement = select */
        fn = lookupKey( globalKeys, inKey, inMods, 1 );
        if( fn != NULL && isMovement( fn ) ) {
            shiftSelect = 1;
            }
        else {
            fn = NULL;
            }
        }
    if( fn == NULL ) {
        if( inMods & ( MOD_CTRL | MOD_ALT ) ) {
            ed_errorMessage( "%s%s%s is undefined",
                             ( inMods & MOD_CTRL ) ? "C-" : "",
                             ( inMods & MOD_ALT ) ? "M-" : "",
                             SDL_GetKeyName( (SDL_Keycode)inKey ) );
            return 1;
            }
        return 0;
        }
    if( ! isMovement( fn ) && fn != cmdCancel ) {
        shiftSelecting = 0;
        }
    runCommand( fn );
    return 1;
    }



void ed_textInput( const char  *inText ) {
    if( ed.mbMode == MB_ISEARCH ) {
        isearchText( inText );
        return;
        }
    if( ed.mbMode == MB_QUERY_REPLACE ) {
        queryReplaceKey( inText );
        return;
        }
    if( ed.mbMode == MB_YES_NO ) {
        if( inText[ 0 ] == 'y' || inText[ 0 ] == 'Y' ) {
            yesNoAnswer( 1 );
            }
        else if( inText[ 0 ] == 'n' || inText[ 0 ] == 'N' ) {
            yesNoAnswer( 0 );
            }
        return;
        }
    if( ed.mbMode != MB_NONE ) {
        mbInsert( inText );
        return;
        }
    if( ed.prefixCtrlX || ed.prefixCtrlC || ed.prefixMetaG ) {
        /* handled by keydown */
        return;
        }

    thisCmd = cmdSelfInsert;
    if( lastCmd == cmdSelfInsert && typedInGroup < 20
        && inText[ 0 ] != ' ' ) {
        tb_continueGroup( B() );
        }
    else {
        tb_beginGroup( B() );
        typedInGroup = 0;
        }
    typedInGroup ++;
    ed.message[ 0 ] = '\0';
    shiftSelecting = 0;
    syncCur();
    selfInsert( inText, (int)strlen( inText ) );
    syncCur();
    lastCmd = cmdSelfInsert;
    }



/* ------------------------------------------------------------------
   Mouse
   ------------------------------------------------------------------ */

void ed_mouseSetCursor( int  inLine,
                        int  inCol,
                        int  inExtend ) {
    if( ed.mbMode == MB_ISEARCH ) {
        isearchEnd( 0 );
        }
    if( inExtend ) {
        if( ! B()->markActive ) {
            setMarkHere();
            }
        }
    else {
        deactivateMark();
        }
    shiftSelecting = 0;
    lastCmd = NULL;
    setCur( inLine, inCol );
    }



void ed_mouseSelectWord( int  inLine,
                         int  inCol ) {
    Line  *l;
    int    s;
    int    e;

    tb_clampPos( B(), &inLine, &inCol );
    l = lineAt( inLine );
    s = inCol;
    e = inCol;
    while( s > 0 && isWordChar( l->text[ s - 1 ] ) ) {
        s --;
        }
    while( e < l->len && isWordChar( l->text[ e ] ) ) {
        e ++;
        }
    setCur( inLine, s );
    setMarkHere();
    setCur( inLine, e );
    }



/* middle click:  pastes the X11 PRIMARY selection (the text last
   selected in any program) at the click, like emacs mouse-2 */
void ed_mousePastePrimary( int  inLine,
                           int  inCol ) {
    char  *text;

    if( ed.mbMode == MB_ISEARCH ) {
        isearchEnd( 0 );
        }
    if( ! checkWritable() ) {
        return;
        }
    text = getPrimaryText();
    if( text == NULL ) {
        return;
        }
    if( text[ 0 ] != '\0' ) {
        tb_beginGroup( B() );
        ed_mouseSetCursor( inLine, inCol, 0 );
        insertAtCursor( text, (int)strlen( text ) );
        syncCur();
        }
    SDL_free( text );
    }



/* ------------------------------------------------------------------
   The X11 PRIMARY selection

   Like other X programs (and emacs), whatever region is active is
   offered as the PRIMARY selection (middle-click paste elsewhere),
   separately from the clipboard that M-w / C-w / C-y use.
   ------------------------------------------------------------------ */

static TextBuffer     *primaryBuffer  =  NULL;
static unsigned long   primaryChange  =  0;
static int             primarySL      =  -1;
static int             primarySC      =  -1;
static int             primaryEL      =  -1;
static int             primaryEC      =  -1;



static void updatePrimarySelection( void ) {
    int    sl;
    int    sc;
    int    el;
    int    ec;
    int    len;
    char  *text;

    if( ed.numBuffers == 0 || ! ed_getRegion( &sl, &sc, &el, &ec ) ) {
        return;
        }
    if( sl == el && sc == ec ) {
        return;
        }
    if( B() == primaryBuffer && B()->changeCount == primaryChange
        && sl == primarySL && sc == primarySC
        && el == primaryEL && ec == primaryEC ) {
        return;
        }
    primaryBuffer = B();
    primaryChange = B()->changeCount;
    primarySL = sl;
    primarySC = sc;
    primaryEL = el;
    primaryEC = ec;

    text = tb_getText( B(), sl, sc, el, ec, &len );
    setPrimaryText( text );
    free( text );
    }



/* ------------------------------------------------------------------
   Init and tick
   ------------------------------------------------------------------ */

int ed_tick( void ) {
    int  changed  =  0;
    int  i;

    updatePrimarySelection();

    for( i = 0;
         i < ed.numBuffers;
         i ++ ) {
        if( ed.buffers[ i ]->needsTypeScan ) {
            ed.buffers[ i ]->needsTypeScan = 0;
            syn_scanTypedefs( ed.buffers[ i ] );
            /* one per tick */
            return 1;
            }
        }

    if( comp_isRunning() ) {
        changed = comp_poll();
        if( changed && ! comp_isRunning() ) {
            int  ne;
            int  nw;

            comp_countErrors( &ne, &nw );
            if( comp_exitStatus() == 0 ) {
                ed_message( "Compilation finished:  %d error%s, %d warning%s",
                            ne, ne == 1 ? "" : "s", nw, nw == 1 ? "" : "s" );
                }
            else {
                ed_errorMessage( "Compilation failed:  %d error%s, "
                                 "%d warning%s  (F4 / C-x ` for next)",
                                 ne, ne == 1 ? "" : "s", nw,
                                 nw == 1 ? "" : "s" );
                }
            }
        }
    return changed;
    }



/* checks open files for changes on disk (call when window gains focus) */
void ed_checkFilesOnDisk( void );

void ed_checkFilesOnDisk( void ) {
    int  i;
    int  cur  =  ed.current;

    for( i = 0;
         i < ed.numBuffers;
         i ++ ) {
        TextBuffer  *tb  =  ed.buffers[ i ];
        long         mt;

        if( tb->path == NULL || tb->fileMTime == 0 ) {
            continue;
            }
        mt = fileMTime( tb->path );
        if( mt != 0 && mt != tb->fileMTime ) {
            if( ! tb_isModified( tb ) ) {
                ed.current = i;
                revertBuffer();
                ed.current = cur;
                syncCur();
                ed_message( "%s changed on disk;  reverted", tb->name );
                }
            else {
                tb->fileMTime = mt;
                ed_errorMessage( "%s changed on disk, and you have unsaved "
                                 "edits!  (M-x revert-buffer to discard "
                                 "yours)", tb->name );
                }
            }
        }
    }



void ed_init( int     inArgc,
              char  **inArgv ) {
    int  i;
    int  firstFile  =  -1;
    int  gotoLineNum  =  0;

    memset( &ed, 0, sizeof( ed ) );
    ed.current = 0;
    ed.compileHighlightLine = -1;
    ed.currentError = -1;
    ed.fontSize = (float)config.fontSize;
    ed.visibleRows = 30;
    scopy( ed.debugCommand, config.debugCommand, sizeof( ed.debugCommand ) );
    scopy( ed.debugProgram, config.debugProgram, sizeof( ed.debugProgram ) );

    for( i = 1;
         i < inArgc;
         i ++ ) {
        int  idx;

        if( inArgv[ i ][ 0 ] == '+' ) {
            /* +N  goto line, like emacs/vi */
            gotoLineNum = atoi( inArgv[ i ] + 1 );
            continue;
            }
        if( inArgv[ i ][ 0 ] == '-' ) {
            continue;
            }
        idx = openFile( inArgv[ i ], 0 );
        if( idx >= 0 && firstFile < 0 ) {
            firstFile = idx;
            }
        }
    if( ed.numBuffers == 0 ) {
        makeScratch( "*scratch*", NULL );
        firstFile = 0;
        ed_message( "Welcome to cedit.  F1 for help.  C-x C-f to open "
                    "a file." );
        }
    if( firstFile < 0 ) {
        firstFile = 0;
        }
    for( i = 0;
         i < ed.numBuffers;
         i ++ ) {
        ed.mru[ i ] = i;
        }
    ed_switchToBuffer( firstFile );
    if( gotoLineNum > 0 ) {
        gotoLine( gotoLineNum - 1 );
        }
    if( ed.numBuffers > 1 || B()->path != NULL ) {
        ed_message( "F1 for help" );
        }
    if( config_lastWarning()[ 0 ] != '\0' ) {
        ed_errorMessage( "%s", config_lastWarning() );
        }
    }
