/*
  cedit.c

  cedit -- a fast, focused editor for C89 code.

  This file holds main(), the SDL event loop, and all rendering.  The
  editor state and commands are in editor.c;  the document model,
  lexer/highlighter, indenter, formatter, config and compile modules are
  pure C89 with no SDL dependency.

  Headless modes (no window, for scripting and testing):

    cedit --reindent FILE        print FILE re-indented
    cedit --format FILE          print FILE re-indented and auto-formatted
    cedit --check-indent FILE..  list lines whose indentation differs from
                                 what cedit would produce
    cedit --script KEYS FILE     replay keystrokes from the file KEYS
                                 against FILE, then print the buffer
    cedit --write-config         write commented default config files
                                 into ~/.cedit/

  Environment:
    CEDIT_QUIT_AFTER=N           quit after N rendered frames (timing)
*/

#include "editor.h"
#include "gui.h"
#include "config.h"
#include "syntax.h"
#include "indent.h"
#include "format.h"
#include "compile.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>


#define CEDIT_VERSION  "0.1"


/* layout of the last rendered frame, used for mouse hit testing */
typedef struct Layout {
        float  lineH;
        float  charW;
        float  tabTop;
        float  tabBottom;
        float  textTop;
        float  textBottom;
        float  textLeft;
        /* the vertical scroll bar at the right of the text view */
        float  scrollBarX;
        float  thumbTop;
        float  thumbBottom;
        /* scroll range the thumb maps over, in lines */
        float  scrollRange;
        float  compileTop;
        float  compileBottom;
        int    compileFirstLine;
        /* x extents of each tab */
        float *tabX0;
        float *tabX1;
        int    numTabs;
        float  winW;
        float  winH;
    } Layout;

static Layout  layout;

static SDL_Window  *window  =  NULL;



/* ------------------------------------------------------------------
   Headless modes
   ------------------------------------------------------------------ */

static int headlessReindent( const char  *inPath,
                             int          inFormat ) {
    TextBuffer  *tb  =  tb_load( inPath, config.tabWidth, 1, NULL );
    int          i;

    if( tb == NULL ) {
        fprintf( stderr, "cedit:  can't read %s\n", inPath );
        return 1;
        }
    syn_scanTypedefs( tb );
    ind_indentRegion( tb, 0, tb->numLines - 1 );
    if( inFormat ) {
        config.formatOnEnter = 1;
        fmt_formatRegion( tb, 0, tb->numLines - 1 );
        }
    for( i = 0;
         i < tb->numLines;
         i ++ ) {
        if( i == tb->numLines - 1 && tb->lines[ i ].len == 0 ) {
            break;
            }
        printf( "%.*s\n", tb->lines[ i ].len, tb->lines[ i ].text );
        }
    tb_destroy( tb );
    return 0;
    }



static int headlessCheckIndent( int     inNumFiles,
                                char  **inFiles ) {
    int  f;
    int  total  =  0;
    int  diff   =  0;

    for( f = 0;
         f < inNumFiles;
         f ++ ) {
        TextBuffer  *tb  =  tb_load( inFiles[ f ], config.tabWidth, 1, NULL );
        int         *before;
        int          i;

        if( tb == NULL ) {
            fprintf( stderr, "cedit:  can't read %s\n", inFiles[ f ] );
            continue;
            }
        syn_scanTypedefs( tb );
        before = (int *)malloc( (size_t)tb->numLines * sizeof( int ) );
        for( i = 0;
             i < tb->numLines;
             i ++ ) {
            before[ i ] = tb_lineIndent( tb, i );
            }
        ind_indentRegion( tb, 0, tb->numLines - 1 );
        for( i = 0;
             i < tb->numLines;
             i ++ ) {
            if( ! tb_lineIsBlank( tb, i )
                &&
                tb_lineIndent( tb, i ) != before[ i ] ) {
                printf( "%s:%d: indent %d, cedit wants %d\n", inFiles[ f ],
                        i + 1, before[ i ], tb_lineIndent( tb, i ) );
                diff ++;
                }
            }
        total += tb->numLines;
        free( before );
        tb_destroy( tb );
        }
    printf( "%d of %d lines differ\n", diff, total );
    return diff > 0 ? 1 : 0;
    }



/* parses one key token like "C-x", "M-S-Up", "RET", "a" */
static int parseKeyToken( const char  *inTok,
                          int         *outKey,
                          int         *outMods ) {
    const char  *p     =  inTok;
    int          mods  =  0;

    while( p[ 0 ] != '\0' && p[ 1 ] == '-' && p[ 2 ] != '\0' ) {
        if( p[ 0 ] == 'C' ) {
            mods |= MOD_CTRL;
            }
        else if( p[ 0 ] == 'M' ) {
            mods |= MOD_ALT;
            }
        else if( p[ 0 ] == 'S' ) {
            mods |= MOD_SHIFT;
            }
        else {
            break;
            }
        p += 2;
        }
    *outMods = mods;
    if( strcmp( p, "RET" ) == 0 ) {
        *outKey = SDLK_RETURN;
        }
    else if( strcmp( p, "TAB" ) == 0 ) {
        *outKey = SDLK_TAB;
        }
    else if( strcmp( p, "BS" ) == 0 ) {
        *outKey = SDLK_BACKSPACE;
        }
    else if( strcmp( p, "DEL" ) == 0 ) {
        *outKey = SDLK_DELETE;
        }
    else if( strcmp( p, "ESC" ) == 0 ) {
        *outKey = SDLK_ESCAPE;
        }
    else if( strcmp( p, "SPC" ) == 0 ) {
        *outKey = SDLK_SPACE;
        }
    else if( strcmp( p, "Up" ) == 0 ) {
        *outKey = SDLK_UP;
        }
    else if( strcmp( p, "Down" ) == 0 ) {
        *outKey = SDLK_DOWN;
        }
    else if( strcmp( p, "Left" ) == 0 ) {
        *outKey = SDLK_LEFT;
        }
    else if( strcmp( p, "Right" ) == 0 ) {
        *outKey = SDLK_RIGHT;
        }
    else if( strcmp( p, "Home" ) == 0 ) {
        *outKey = SDLK_HOME;
        }
    else if( strcmp( p, "End" ) == 0 ) {
        *outKey = SDLK_END;
        }
    else if( strcmp( p, "PgUp" ) == 0 ) {
        *outKey = SDLK_PAGEUP;
        }
    else if( strcmp( p, "PgDn" ) == 0 ) {
        *outKey = SDLK_PAGEDOWN;
        }
    else if( p[ 0 ] == 'F' && p[ 1 ] >= '1' && p[ 1 ] <= '9' ) {
        *outKey = SDLK_F1 + atoi( p + 1 ) - 1;
        }
    else if( p[ 0 ] != '\0' && p[ 1 ] == '\0' ) {
        *outKey = (unsigned char)p[ 0 ];
        }
    else {
        return 0;
        }
    return 1;
    }



/* replays a key script.  Tokens are whitespace separated;  "quoted text"
   is typed character by character (\n in it presses Enter, \" is a
   quote);  everything else is a key like C-x, M-/, RET, TAB, S-Down.
   Lines starting with # are comments. */
static int headlessScript( const char  *inScript,
                           const char  *inFile ) {
    FILE         *f  =  fopen( inScript, "r" );
    char          line[ 4096 ];
    char         *argv[ 2 ];
    TextBuffer   *tb;
    int           i;

    if( f == NULL ) {
        fprintf( stderr, "cedit:  can't read %s\n", inScript );
        return 1;
        }
    argv[ 0 ] = (char *)"cedit";
    argv[ 1 ] = (char *)inFile;
    ed_init( inFile != NULL ? 2 : 1, argv );

    while( fgets( line, sizeof( line ), f ) != NULL ) {
        char  *p  =  line;

        if( line[ 0 ] == '#' ) {
            continue;
            }
        while( *p != '\0' ) {
            while( *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ) {
                p ++;
                }
            if( *p == '\0' ) {
                break;
                }
            if( *p == '"' ) {
                p ++;
                while( *p != '\0' && *p != '"' ) {
                    char  s[ 2 ];

                    if( *p == '\\' && p[ 1 ] == 'n' ) {
                        ed_keyDown( SDLK_RETURN, 0 );
                        p += 2;
                        continue;
                        }
                    if( *p == '\\' && p[ 1 ] != '\0' ) {
                        p ++;
                        }
                    s[ 0 ] = *p;
                    s[ 1 ] = '\0';
                    if( ! ed_keyDown( (unsigned char)*p, 0 ) ) {
                        ed_textInput( s );
                        }
                    p ++;
                    }
                if( *p == '"' ) {
                    p ++;
                    }
                }
            else {
                char  tok[ 64 ];
                int   n  =  0;
                int   key;
                int   mods;

                while( *p != '\0' && *p != ' ' && *p != '\t' && *p != '\n'
                       && *p != '\r' && n < 63 ) {
                    tok[ n ++ ] = *p ++;
                    }
                tok[ n ] = '\0';
                if( parseKeyToken( tok, &key, &mods ) ) {
                    ed_keyDown( key, mods );
                    }
                else {
                    fprintf( stderr, "cedit:  bad key token %s\n", tok );
                    }
                }
            while( comp_isRunning() ) {
                ed_tick();
                SDL_Delay( 5 );
                }
            }
        }
    fclose( f );

    tb = ed_cur();
    for( i = 0;
         i < tb->numLines;
         i ++ ) {
        printf( "%.*s\n", tb->lines[ i ].len, tb->lines[ i ].text );
        }
    fprintf( stderr, "cursor %d:%d  message: %s\n", tb->cursorLine + 1,
             tb->cursorCol, ed.message );
    return 0;
    }



/* ------------------------------------------------------------------
   Rendering helpers
   ------------------------------------------------------------------ */

static unsigned int UC( int  inWhich ) {
    return config.uiColor[ inWhich ];
    }



static void drawString( float         inX,
                        float         inY,
                        unsigned int  inColor,
                        const char   *inS ) {
    gui_text( inX, inY, inColor, inS, (int)strlen( inS ) );
    }



/* draws a run of bytes of one line at character cells, handling UTF-8
   (a multi-byte char is drawn at the cell of its first byte) */
static void drawCells( float         inX,
                       float         inY,
                       float         inCharW,
                       unsigned int  inColor,
                       const char   *inText,
                       int           inLen ) {
    int  i;
    int  start  =  0;

    for( i = 0;
         i <= inLen;
         i ++ ) {
        if( i == inLen || ( inText[ i ] & 0x80 ) ) {
            if( i > start ) {
                gui_text( inX + (float)start * inCharW, inY, inColor,
                          inText + start, i - start );
                }
            if( i < inLen ) {
                int  n  =  1;

                while( i + n < inLen && ( inText[ i + n ] & 0xC0 ) == 0x80 ) {
                    n ++;
                    }
                gui_text( inX + (float)i * inCharW, inY, inColor, inText + i,
                          n );
                i += n - 1;
                start = i + 1;
                }
            }
        }
    }



static int numDigits( int  inN ) {
    int  d  =  1;

    while( inN >= 10 ) {
        inN /= 10;
        d ++;
        }
    return d;
    }



/* width of the line number gutter for a buffer.  At least 4 digits, so
   the text doesn't jump sideways as a small file grows */
static float gutterWidth( TextBuffer  *inTB,
                          float        inCharW ) {
    int  d;

    if( ! config.showLineNumbers ) {
        return inCharW * 0.5f;
        }
    d = numDigits( inTB->numLines );
    if( d < 4 ) {
        d = 4;
        }
    return (float)( d + 2 ) * inCharW;
    }



static float scrollBarWidth( float  inCharW ) {
    float  w  =  (float)floor( inCharW * 1.25f );

    if( ! config.showScrollBar ) {
        return 0;
        }
    if( w < 10 ) {
        w = 10;
        }
    return w;
    }



static int matchAtFold( Line        *inL,
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
            if( a >= 'A' && a <= 'Z' ) {
                a = (char)( a - 'A' + 'a' );
                }
            if( b >= 'A' && b <= 'Z' ) {
                b = (char)( b - 'A' + 'a' );
                }
            }
        if( a != b ) {
            return 0;
            }
        }
    return 1;
    }



/* ------------------------------------------------------------------
   Visual rows (line wrapping)

   With wrapLines on, a line longer than the window shows on several
   rows, each ed.wrapCols columns wide (a line exactly as wide as the
   window still takes one row:  its end-of-line cursor is a bar at the
   edge).  scrollY stays a line number:  its fraction is the part of the
   top line's rows scrolled off, so smooth scrolling still works.
   ------------------------------------------------------------------ */

#define SEG_END_MAX  0x7FFFFFFF

/* display column of byte c of a line (tabs are wider than one cell);
   view rows, scrolling and drawing all work in display columns */
#define VCOL( l, c )  tb_visCol( (l), (c), config.tabWidth )


/* one row of the text view:  a line, or one piece of a wrapped line */
typedef struct ViewRow {
        int    line;
        /* display columns [ segStart, segEnd ) show on this row;  the
           last row of a line has segEnd = SEG_END_MAX */
        int    segStart;
        int    segEnd;
        float  y;
    } ViewRow;

#define MAX_VIEW_ROWS  2048

/* the rows of the last frame, for mouse hit testing */
static ViewRow  viewRows[ MAX_VIEW_ROWS ];
static int      numViewRows  =  0;



static int lineRows( TextBuffer  *inTB,
                     int          inLine ) {
    return ed_lineRows( inTB, inLine );
    }



/* which of its line's rows a column is on */
static int rowOfCol( TextBuffer  *inTB,
                     int          inLine,
                     int          inCol ) {
    int  n  =  lineRows( inTB, inLine );
    int  r;

    if( n == 1 ) {
        return 0;
        }
    r = VCOL( &( inTB->lines[ inLine ] ), inCol ) / ed.wrapCols;
    if( r >= n ) {
        r = n - 1;
        }
    return r;
    }



/* the latest top line that leaves at least inRowsAbove rows above row
   inSub of inLine */
static int topWithRowsAbove( TextBuffer  *inTB,
                             int          inLine,
                             int          inSub,
                             int          inRowsAbove ) {
    int  top  =  inLine;
    int  acc  =  inSub;

    while( top > 0 && acc < inRowsAbove ) {
        top --;
        acc += lineRows( inTB, top );
        }
    return top;
    }



/* the earliest top line that keeps row inSub of inLine at or above view
   row inMaxRow */
static int topWithRowAtMost( TextBuffer  *inTB,
                             int          inLine,
                             int          inSub,
                             int          inMaxRow ) {
    int  top  =  inLine;
    int  acc  =  inSub;

    while( top > 0 && acc + lineRows( inTB, top - 1 ) <= inMaxRow ) {
        top --;
        acc += lineRows( inTB, top );
        }
    return top;
    }



/* the last line whose rows all fit in inRows rows below top line inTop */
static int lastLineFitting( TextBuffer  *inTB,
                            int          inTop,
                            int          inRows ) {
    int  line  =  inTop;
    int  acc   =  lineRows( inTB, inTop );

    while( line + 1 < inTB->numLines
           && acc + lineRows( inTB, line + 1 ) <= inRows ) {
        line ++;
        acc += lineRows( inTB, line );
        }
    return line;
    }



/* lays out the visible rows from the buffer's scroll position */
static void buildViewRows( TextBuffer  *inTB,
                           float        inTop,
                           float        inBottom,
                           float        inLineH ) {
    int    line  =  (int)floor( inTB->scrollY );
    float  frac  =  inTB->scrollY - (float)line;
    float  y;

    numViewRows = 0;
    if( line < 0 ) {
        line = 0;
        frac = 0;
        }
    if( line >= inTB->numLines ) {
        return;
        }
    y = inTop - frac * (float)lineRows( inTB, line ) * inLineH;

    while( line < inTB->numLines && y < inBottom
           && numViewRows < MAX_VIEW_ROWS ) {
        int  n  =  lineRows( inTB, line );
        int  r;

        for( r = 0;
             r < n && y < inBottom && numViewRows < MAX_VIEW_ROWS;
             r ++ ) {
            if( y + inLineH > inTop ) {
                ViewRow  *v  =  &( viewRows[ numViewRows ] );

                v->line = line;
                v->segStart = r * ed.wrapCols;
                v->segEnd = r == n - 1 ? SEG_END_MAX : ( r + 1 ) * ed.wrapCols;
                v->y = y;
                numViewRows ++;
                }
            y += inLineH;
            }
        line ++;
        }
    }



static int rowHasCol( ViewRow  *inRow,
                      int       inCol ) {
    return inCol >= inRow->segStart && inCol < inRow->segEnd;
    }



/* ------------------------------------------------------------------
   Main text view
   ------------------------------------------------------------------ */

static void updateScroll( TextBuffer  *inTB,
                          int          inRows,
                          int          inCols,
                          float        inDT ) {
    int    cl      =  inTB->cursorLine;
    int    cc      =  VCOL( &( inTB->lines[ cl ] ), inTB->cursorCol );
    int    margin  =  config.scrollMargin;
    float  maxTop  =  (float)( inTB->numLines - 1 );
    int    sub     =  rowOfCol( inTB, cl, inTB->cursorCol );
    float  t;
    float  latest;
    float  earliest;

    if( margin > inRows / 2 - 1 ) {
        margin = inRows / 2 - 1;
        }
    if( margin < 0 ) {
        margin = 0;
        }

    if( ed.recenterRequest ) {
        inTB->targetScrollY = (float)topWithRowsAbove( inTB, cl, sub,
                                                       inRows / 2 );
        ed.recenterRequest = 0;
        }
    t = inTB->targetScrollY;
    /* keep the cursor's row margin rows from the top and bottom
       (counting wrapped rows) */
    latest = (float)topWithRowsAbove( inTB, cl, sub, margin );
    earliest = (float)topWithRowAtMost( inTB, cl, sub,
                                        inRows - 1 - margin );
    if( t > latest ) {
        t = latest;
        }
    if( t < earliest ) {
        t = earliest;
        }
    if( t > maxTop ) {
        t = maxTop;
        }
    if( t < 0 ) {
        t = 0;
        }
    /* whole lines only for the resting position */
    t = (float)floor( t + 0.5 );
    inTB->targetScrollY = t;

    if( config.smoothScroll
        &&
        fabs( t - inTB->scrollY ) < (float)inRows * 3.0f ) {
        float  k  =  inDT * 20.0f;

        if( k > 1.0f ) {
            k = 1.0f;
            }
        inTB->scrollY += ( t - inTB->scrollY ) * k;
        if( fabs( t - inTB->scrollY ) < 0.02f ) {
            inTB->scrollY = t;
            }
        }
    else {
        inTB->scrollY = t;
        }

    /* horizontal */
    if( ed.wrapCols > 0 ) {
        inTB->scrollX = 0;
        return;
        }
    if( (float)cc < inTB->scrollX ) {
        inTB->scrollX = (float)( cc - inCols / 4 );
        }
    /* a cursor right after the last visible column still shows (as a
       bar at the edge), so an 80 column window can edit 80 column lines */
    if( (float)cc > inTB->scrollX + (float)inCols ) {
        inTB->scrollX = (float)( cc - inCols + inCols / 4 );
        }
    if( inTB->scrollX < 0 ) {
        inTB->scrollX = 0;
        }
    }



static int isAnimating( void ) {
    TextBuffer  *tb  =  ed_cur();

    return tb->scrollY != tb->targetScrollY;
    }



/* the scroll range:  the top line can go from 0 to numLines - 1 */
static float scrollRangeOf( TextBuffer  *inTB,
                            int          inRows ) {
    return (float)( inTB->numLines - 1 + inRows );
    }



static void drawScrollBar( TextBuffer  *inTB,
                           float        inX0,
                           float        inX1,
                           float        inTop,
                           float        inBottom,
                           int          inRows,
                           int          inCursorLine,
                           int          inErrLine ) {
    float  trackH   =  inBottom - inTop;
    float  range    =  scrollRangeOf( inTB, inRows );
    float  perLine  =  trackH / (float)inTB->numLines;
    float  thumbH;
    float  thumbY;
    float  lines;
    float  pad      =  2;

    layout.scrollBarX = inX0;
    layout.scrollRange = range;
    if( inX1 <= inX0 ) {
        layout.thumbTop = layout.thumbBottom = -1;
        return;
        }
    gui_rect( inX0, inTop, inX1, inBottom, UC( UI_SCROLLBAR_BG ) );

    thumbH = trackH * (float)inRows / range;
    if( thumbH < 24 ) {
        thumbH = 24;
        }
    if( thumbH > trackH ) {
        thumbH = trackH;
        }
    lines = range - (float)inRows;
    if( lines < 1 ) {
        lines = 1;
        }
    thumbY = inTop + ( trackH - thumbH ) * inTB->scrollY / lines;
    if( thumbY + thumbH > inBottom ) {
        thumbY = inBottom - thumbH;
        }
    layout.thumbTop = thumbY;
    layout.thumbBottom = thumbY + thumbH;

    /* ticks for the cursor line and the current compile error */
    if( inErrLine >= 0 ) {
        float  y  =  inTop + perLine * (float)inErrLine;

        gui_rect( inX0, y - 1, inX1, y + 2, UC( UI_COMPILE_ERROR ) );
        }

    gui_rect( inX0 + pad, thumbY + pad, inX1 - pad, thumbY + thumbH - pad,
              UC( UI_SCROLLBAR_THUMB ) );

    {
        float  y  =  inTop + perLine * ( (float)inCursorLine + 0.5f );

        gui_rect( inX0, y - 1, inX1, y + 1, UC( UI_CURSOR ) );
        }
    }



/* fills columns [ inS, inE ) of a line, clipped to view row inRow */
static void spanRect( ViewRow       *inRow,
                      int            inS,
                      int            inE,
                      float          inTextX,
                      float          inCharW,
                      int            inScrollX,
                      float          inLineH,
                      unsigned int   inColor ) {
    if( inS < inRow->segStart ) {
        inS = inRow->segStart;
        }
    if( inE > inRow->segEnd ) {
        inE = inRow->segEnd;
        }
    if( inE <= inS ) {
        return;
        }
    gui_rect( inTextX + (float)( inS - inRow->segStart - inScrollX ) * inCharW,
              inRow->y,
              inTextX + (float)( inE - inRow->segStart - inScrollX ) * inCharW,
              inRow->y + inLineH, inColor );
    }



static int isBlankChar( char  inC ) {
    return inC == ' ' || inC == '\t';
    }



/* the hooked arrow in the gutter of a wrapped line's continuation row,
   like the one emacs puts in its fringe */
static void drawWrapArrow( float  inGutterW,
                           float  inY,
                           float  inCharW,
                           float  inLineH ) {
    unsigned int  col    =  UC( UI_WRAP_MARK );
    float         w      =  inCharW * 1.1f;
    float         x1     =  inGutterW - inCharW * 0.3f;
    float         x0     =  x1 - w;
    float         yTop   =  inY + inLineH * 0.2f;
    float         yMid   =  inY + inLineH * 0.6f;
    float         head   =  inCharW * 0.35f;

    if( x0 < 1 ) {
        x0 = 1;
        }
    gui_line( x0, yTop, x0, yMid, col, 1.5f );
    gui_line( x0, yMid, x1, yMid, col, 1.5f );
    gui_line( x1 - head, yMid - head, x1, yMid, col, 1.5f );
    gui_line( x1 - head, yMid + head, x1, yMid, col, 1.5f );
    }



static void drawTextView( float  inTop,
                          float  inBottom,
                          float  inW,
                          float  inDT ) {
    static unsigned char  *classes     =  NULL;
    static int             classesCap  =  0;
    /* a line with tabs, expanded to display columns for drawing */
    static char           *expText     =  NULL;
    static unsigned char  *expClasses  =  NULL;
    static int             expCap      =  0;
    TextBuffer            *tb          =  ed_cur();
    float                  lineH       =  layout.lineH;
    float                  charW       =  layout.charW;
    float                  gutterW;
    float                  sbW;
    float                  textX;
    int                    rows;
    int                    cols;
    int                    i;
    int                    sl          =  0;
    int                    sc          =  0;
    int                    el          =  0;
    int                    ec          =  0;
    int                    hasRegion;
    int                    cl;
    int                    cc;
    int                    ccv;
    int                    scrollXi;
    const char            *searchText  =  NULL;
    int                    searchFold  =  0;
    int                    searchLen   =  0;
    int                    parenL1     =  -1;
    int                    parenC1     =  -1;
    int                    parenL2     =  -1;
    int                    parenC2     =  -1;
    int                    errLine     =  -1;
    int                    markL;
    int                    markC;
    int                    lastLexed   =  -1;
    ViewRow               *curRow      =  NULL;

    gutterW = gutterWidth( tb, charW );
    sbW = scrollBarWidth( charW );
    /* everything below draws the text area;  the scroll bar gets the
       strip to the right of it */
    inW -= sbW;
    textX = gutterW;
    layout.textLeft = textX;
    layout.textTop = inTop;
    layout.textBottom = inBottom;

    rows = (int)( ( inBottom - inTop ) / lineH );
    if( rows < 1 ) {
        rows = 1;
        }
    cols = (int)( ( inW - textX ) / charW );
    if( cols < 1 ) {
        cols = 1;
        }
    ed.visibleRows = rows;
    ed.visibleCols = cols;
    ed.wrapCols = config.wrapLines ? cols : 0;

    updateScroll( tb, rows, cols, inDT );

    cl = tb->cursorLine;
    cc = tb->cursorCol;
    ccv = VCOL( &( tb->lines[ cl ] ), cc );
    scrollXi = (int)tb->scrollX;
    buildViewRows( tb, inTop, inBottom, lineH );
    for( i = 0;
         i < numViewRows;
         i ++ ) {
        if( viewRows[ i ].line == cl
            && rowHasCol( &( viewRows[ i ] ), ccv ) ) {
            curRow = &( viewRows[ i ] );
            }
        }

    hasRegion = ed_getRegion( &sl, &sc, &el, &ec );
    if( ed_searchHighlight( &searchText, &searchFold ) ) {
        searchLen = (int)strlen( searchText );
        }

    /* bracket matching (C only:  in prose, an apostrophe would look like
       a char literal to the scanner) */
    if( tb->isC ) {
        Line  *l  =  &( tb->lines[ cl ] );

        if( cc < l->len && strchr( "([{", l->text[ cc ] ) != NULL ) {
            parenL1 = cl;
            parenC1 = cc;
            }
        else if( cc > 0 && strchr( ")]}", l->text[ cc - 1 ] ) != NULL ) {
            parenL1 = cl;
            parenC1 = cc - 1;
            }
        else if( cc < l->len && strchr( ")]}", l->text[ cc ] ) != NULL ) {
            parenL1 = cl;
            parenC1 = cc;
            }
        if( parenL1 >= 0
            &&
            ! ind_findMatchingBracket( tb, parenL1, parenC1, 3000, &parenL2,
                                       &parenC2 ) ) {
            parenL2 = -2;
            }
        }

    /* current compile error location in this buffer */
    if( ed.currentError >= 0 ) {
        CompileError  *e  =  comp_getError( ed.currentError );

        if( e != NULL && e->buffer == tb ) {
            int  ecol;

            tb_getMarker( tb, e->marker, &errLine, &ecol );
            }
        }

    tb_getMarker( tb, tb->markMarker, &markL, &markC );

    gui_rect( 0, inTop, inW, inBottom, UC( UI_BACKGROUND ) );
    gui_rect( 0, inTop, gutterW, inBottom, UC( UI_GUTTER_BG ) );

    gui_pushClip( textX, inTop, inW, inBottom );

    /* x of column c on view row r */
#define COL_X( r, c )  ( textX + (float)( (c) - (r)->segStart - scrollXi ) \
                                 * charW )

    /* backgrounds */
    for( i = 0;
         i < numViewRows;
         i ++ ) {
        ViewRow  *r     =  &( viewRows[ i ] );
        int       line  =  r->line;
        float     y     =  r->y;
        Line     *l     =  &( tb->lines[ line ] );

        if( line == cl && config.highlightCurrentLine ) {
            gui_rect( textX, y, inW, y + lineH, UC( UI_CURRENT_LINE ) );
            }
        if( line == errLine ) {
            gui_rect( textX, y, inW, y + lineH, UC( UI_ERROR_LINE ) );
            }

        if( config.showTrailingSpace && l->len > 0
            && isBlankChar( l->text[ l->len - 1 ] )
            && ! ( line == cl && cc == l->len ) ) {
            int  s  =  l->len;

            while( s > 0 && isBlankChar( l->text[ s - 1 ] ) ) {
                s --;
                }
            spanRect( r, VCOL( l, s ), VCOL( l, l->len ), textX, charW,
                      scrollXi, lineH, UC( UI_TRAILING_SPACE ) );
            }

        if( hasRegion && line >= sl && line <= el ) {
            int  s  =  ( line == sl ) ? VCOL( l, sc ) : 0;
            int  e  =  VCOL( l, l->len ) + 1;

            if( line == el ) {
                e = VCOL( l, ec );
                }

            spanRect( r, s, e, textX, charW, scrollXi, lineH,
                      UC( UI_SELECTION ) );
            }

        if( searchLen > 0 ) {
            int  c;

            for( c = 0;
                 c + searchLen <= l->len;
                 c ++ ) {
                if( matchAtFold( l, c, searchText, searchLen, searchFold ) ) {
                    int  cur  =  ( line == ed.matchLine && c == ed.matchCol );

                    spanRect( r, VCOL( l, c ), VCOL( l, c + searchLen ),
                              textX, charW, scrollXi, lineH,
                              UC( cur ? UI_SEARCH_CURRENT
                                  : UI_SEARCH_MATCH ) );
                    c += searchLen - 1;
                    }
                }
            }

        if( line == parenL1 || line == parenL2 ) {
            int           pc   =  line == parenL1 ? parenC1 : parenC2;
            unsigned int  col  =  parenL2 == -2 ? UC( UI_MISMATCH_PAREN )
                                  : UC( UI_MATCH_PAREN );

            if( parenL2 != -2 || line == parenL1 ) {
                spanRect( r, VCOL( l, pc ), VCOL( l, pc ) + 1, textX, charW,
                          scrollXi, lineH, col );
                }
            if( line == parenL1 && line == parenL2 ) {
                spanRect( r, VCOL( l, parenC2 ), VCOL( l, parenC2 ) + 1,
                          textX, charW, scrollXi, lineH, col );
                }
            }

        if( tb->markSet && line == markL && rowHasCol( r, VCOL( l, markC ) )
            && ! ( markL == cl && markC == cc ) ) {
            /* visible mark */
            int  mv  =  VCOL( l, markC );

            gui_rect( COL_X( r, mv ), y, COL_X( r, mv + 1 ), y + lineH,
                      UC( UI_MARK ) );
            gui_rect( COL_X( r, mv ), y + lineH - 2,
                      COL_X( r, mv + 1 ), y + lineH, UC( UI_CURSOR ) );
            }
        }

    /* current column band and ruler */
    if( config.highlightCurrentColumn ) {
        float  x  =  textX + (float)( ccv - scrollXi ) * charW;

        if( curRow != NULL ) {
            x = COL_X( curRow, ccv );
            }
        gui_rect( x, inTop, x + charW, inBottom, UC( UI_CURRENT_COLUMN ) );
        }
    if( config.showRuler && config.fillColumn > 0 ) {
        float  x  =  textX + (float)( config.fillColumn - scrollXi ) * charW;

        gui_line( x, inTop, x, inBottom, UC( UI_RULER ), 1.0f );
        }

    /* text */
    for( i = 0;
         i < numViewRows;
         i ++ ) {
        ViewRow  *r     =  &( viewRows[ i ] );
        int       line  =  r->line;
        float     y     =  r->y;
        Line     *l     =  &( tb->lines[ line ] );
        int       s;
        int       e;
        int       c;
        const char           *text;
        const unsigned char  *cls8;
        int                   len;

        if( l->len == 0 ) {
            continue;
            }
        if( line != lastLexed ) {
            if( classesCap < l->len ) {
                classesCap = l->len * 2 + 256;
                classes = (unsigned char *)realloc( classes,
                                                    (size_t)classesCap );
                }
            syn_highlightLine( tb, line, classes );
            lastLexed = line;
            }
        text = l->text;
        cls8 = classes;
        len = l->len;
        if( memchr( l->text, '\t', (size_t)l->len ) != NULL ) {
            /* tabs:  draw from a copy expanded to display columns */
            int  v  =  VCOL( l, l->len );
            int  j;

            if( expCap < v ) {
                expCap = v * 2 + 256;
                expText = (char *)realloc( expText, (size_t)expCap );
                expClasses = (unsigned char *)realloc( expClasses,
                                                       (size_t)expCap );
                }
            v = 0;
            for( j = 0;
                 j < l->len;
                 j ++ ) {
                int  w  =  1;

                if( l->text[ j ] == '\t' ) {
                    w = config.tabWidth - v % config.tabWidth;
                    }
                while( w > 0 ) {
                    expText[ v ] = l->text[ j ] == '\t' ? ' ' : l->text[ j ];
                    expClasses[ v ] = classes[ j ];
                    v ++;
                    w --;
                    }
                }
            text = expText;
            cls8 = expClasses;
            len = v;
            }

        s = r->segStart + scrollXi;
        e = s + cols + 1;
        if( e > r->segEnd ) {
            e = r->segEnd;
            }
        if( e > len ) {
            e = len;
            }
        c = s;
        while( c < e ) {
            int  k    =  c;
            int  cls  =  cls8[ c ];

            if( text[ c ] == ' ' ) {
                c ++;
                continue;
                }
            while( k < e && cls8[ k ] == cls && text[ k ] != ' ' ) {
                k ++;
                }
            drawCells( COL_X( r, c ), y, charW, config.syntaxColor[ cls ],
                       text + c, k - c );
            c = k;
            }
        }

    /* cursor */
    if( curRow != NULL ) {
        float  y  =  curRow->y;
        float  x  =  COL_X( curRow, ccv );
        Line  *l  =  &( tb->lines[ cl ] );
        /* the block covers the whole tab it sits on, like emacs */
        float  w  =  charW;

        if( cc < l->len && l->text[ cc ] == '\t' ) {
            w = (float)( VCOL( l, cc + 1 ) - ccv ) * charW;
            }

        if( x + charW > inW + 0.5f ) {
            /* the column just past the last full one (the end of a line
               exactly as wide as the window):  a bar at the edge */
            x = inW - charW * 0.35f;
            }
        if( ed.mbMode == MB_NONE || ed.mbMode == MB_ISEARCH
            || ed.mbMode == MB_QUERY_REPLACE ) {
            gui_rect( x, y, x + w, y + lineH, UC( UI_CURSOR ) );
            if( cc < l->len && ! isBlankChar( l->text[ cc ] ) ) {
                int  n  =  1;

                while( cc + n < l->len
                       && ( l->text[ cc + n ] & 0xC0 ) == 0x80 ) {
                    n ++;
                    }
                gui_text( x, y, UC( UI_CURSOR_TEXT ), l->text + cc, n );
                }
            }
        else {
            gui_rectOutline( x, y, x + charW, y + lineH, UC( UI_CURSOR ),
                             1.0f );
            }
        }
#undef COL_X

    gui_popClip();

    /* line numbers, and wrap arrows on continuation rows */
    gui_pushClip( 0, inTop, gutterW, inBottom );
    for( i = 0;
         i < numViewRows;
         i ++ ) {
        ViewRow  *r     =  &( viewRows[ i ] );
        int       line  =  r->line;
        float     y     =  r->y;
        char      num[ 32 ];
        int       n;

        if( r->segStart > 0 ) {
            drawWrapArrow( gutterW, y, charW, lineH );
            continue;
            }
        if( ! config.showLineNumbers ) {
            continue;
            }
        n = sprintf( num, "%d", line + 1 );
        gui_text( gutterW - (float)( n + 1 ) * charW, y,
                  UC( line == cl ? UI_LINE_NUMBER_CURRENT
                      : UI_LINE_NUMBER ), num, n );
        }
    gui_popClip();

    drawScrollBar( tb, inW, inW + sbW, inTop, inBottom, rows, cl, errLine );
    }



/* ------------------------------------------------------------------
   Chrome:  tab bar, compile pane, status line, minibuffer
   ------------------------------------------------------------------ */

/* tab label:  "3 name" (number is the M-3 shortcut) with a * when
   modified */
static void tabLabel( int    inIndex,
                      char  *outLabel ) {
    TextBuffer  *tb   =  ed.buffers[ inIndex ];
    int          mod  =  tb_isModified( tb ) && tb->path != NULL;

    if( inIndex < 9 ) {
        sprintf( outLabel, "%d %s%.500s", inIndex + 1, mod ? "*" : "",
                 tb->name );
        }
    else {
        sprintf( outLabel, "%s%.500s", mod ? "*" : "", tb->name );
        }
    }



static void drawTabBar( float  inTop,
                        float  inH,
                        float  inW ) {
    float  charW     =  layout.charW;
    float  total     =  0;
    float  x;
    float  offset    =  0;
    float  activeX1  =  0;
    int    i;
    char   label[ 600 ];

    layout.tabX0 = (float *)realloc( layout.tabX0,
                                     (size_t)( ed.numBuffers + 1 )
                                     * sizeof( float ) );
    layout.tabX1 = (float *)realloc( layout.tabX1,
                                     (size_t)( ed.numBuffers + 1 )
                                     * sizeof( float ) );
    layout.numTabs = ed.numBuffers;
    layout.tabTop = inTop;
    layout.tabBottom = inTop + inH;

    gui_rect( 0, inTop, inW, inTop + inH, UC( UI_TAB_BG ) );

    /* scroll the bar so the active tab is visible */
    for( i = 0;
         i < ed.numBuffers;
         i ++ ) {
        float  w;

        tabLabel( i, label );
        w = (float)( strlen( label ) + 3 ) * charW;
        if( i == ed.current ) {
            activeX1 = total + w;
            }
        total += w + 2;
        }
    if( activeX1 > inW ) {
        offset = activeX1 - inW + charW * 4;
        }

    x = - offset;
    for( i = 0;
         i < ed.numBuffers;
         i ++ ) {
        int    act  =  ( i == ed.current );
        float  w;

        tabLabel( i, label );
        w = (float)( strlen( label ) + 3 ) * charW;
        layout.tabX0[ i ] = x;
        layout.tabX1[ i ] = x + w;
        if( x + w > 0 && x < inW ) {
            gui_rect( x, inTop + 3, x + w, inTop + inH,
                      UC( act ? UI_TAB_ACTIVE_BG : UI_TAB_BG ) );
            if( ! act ) {
                gui_line( x + w, inTop + 6, x + w, inTop + inH - 3,
                          UC( UI_TAB_FG ), 1.0f );
                }
            drawString( x + charW * 1.5f,
                        inTop + 3 + ( inH - 3 - layout.lineH ) / 2,
                        UC( act ? UI_TAB_ACTIVE_FG : UI_TAB_FG ), label );
            }
        x += w + 2;
        }
    }



static void drawCompilePane( float  inTop,
                             float  inBottom,
                             float  inW ) {
    TextBuffer  *cb      =  ed.compileBuffer;
    float        lineH   =  layout.lineH;
    float        charW   =  layout.charW;
    int          rows;
    int          first;
    int          i;
    char         header[ 1200 ];
    int          ne;
    int          nw;

    layout.compileTop = inTop;
    layout.compileBottom = inBottom;

    gui_rect( 0, inTop, inW, inBottom, UC( UI_COMPILE_BG ) );
    gui_rect( 0, inTop, inW, inTop + lineH + 2, UC( UI_STATUS_BG ) );

    comp_countErrors( &ne, &nw );
    if( comp_isRunning() ) {
        sprintf( header, " *compile*   running...   %d error%s, %d warning%s",
                 ne, ne == 1 ? "" : "s", nw, nw == 1 ? "" : "s" );
        }
    else {
        sprintf( header, " *compile*   %s   %d error%s, %d warning%s   "
                 "(F4 next, S-F4 prev, C-x 1 hides)",
                 comp_exitStatus() == 0 ? "finished" : "exited abnormally",
                 ne, ne == 1 ? "" : "s", nw, nw == 1 ? "" : "s" );
        }
    drawString( 0, inTop + 1, UC( UI_STATUS_FG ), header );

    inTop += lineH + 2;
    rows = (int)( ( inBottom - inTop ) / lineH );
    if( rows < 1 ) {
        return;
        }
    ed.compileRows = rows;

    if( ed.compileFollow ) {
        ed.compileScroll = cb->numLines - rows;
        }
    else if( ed.compileHighlightLine >= 0 ) {
        if( ed.compileHighlightLine < ed.compileScroll
            ||
            ed.compileHighlightLine >= ed.compileScroll + rows ) {
            ed.compileScroll = ed.compileHighlightLine - 1;
            }
        }
    if( ed.compileScroll > cb->numLines - rows ) {
        ed.compileScroll = cb->numLines - rows;
        }
    if( ed.compileScroll < 0 ) {
        ed.compileScroll = 0;
        }
    first = ed.compileScroll;
    layout.compileFirstLine = first;

    gui_pushClip( 0, inTop, inW, inBottom );
    for( i = 0;
         i < rows && first + i < cb->numLines;
         i ++ ) {
        int           line  =  first + i;
        Line         *l     =  &( cb->lines[ line ] );
        float         y     =  inTop + (float)i * lineH;
        unsigned int  col   =  UC( UI_COMPILE_FG );
        int           k;

        if( line == ed.compileHighlightLine ) {
            gui_rect( 0, y, inW, y + lineH, UC( UI_COMPILE_CURRENT ) );
            }
        for( k = 0;
             k < comp_numErrors();
             k ++ ) {
            CompileError  *e  =  comp_getError( k );

            if( e->outLine == line ) {
                col = e->kind == CE_ERROR ? UC( UI_COMPILE_ERROR )
                      : ( e->kind == CE_WARNING ? UC( UI_COMPILE_WARNING )
                          : UC( UI_COMPILE_FG ) );
                break;
                }
            if( e->outLine > line ) {
                break;
                }
            }
        gui_text( charW * 0.5f, y, col, l->text, l->len );
        }
    gui_popClip();
    }



static void drawStatus( float  inTop,
                        float  inH,
                        float  inW ) {
    TextBuffer  *tb  =  ed_cur();
    char         s[ 2048 ];
    char         pos[ 64 ];
    int          mod  =  tb_isModified( tb ) && tb->path != NULL;
    float        x;

    gui_rect( 0, inTop, inW, inTop + inH, UC( UI_STATUS_BG ) );

    if( tb->scrollY <= 0 ) {
        strcpy( pos, "Top" );
        }
    else if( lastLineFitting( tb, (int)tb->scrollY, ed.visibleRows )
             >= tb->numLines - 1 ) {
        strcpy( pos, "Bot" );
        }
    else {
        sprintf( pos, "%d%%", (int)( 100.0 * tb->scrollY
                                     / (double)tb->numLines ) );
        }

    sprintf( s, " %s%s ", tb->readOnly ? "%%" : ( mod ? "**" : "--" ),
             "" );
    x = 0;
    drawString( x, inTop + 2, mod ? UC( UI_STATUS_MODIFIED )
                : UC( UI_STATUS_FG ), s );
    x += (float)strlen( s ) * layout.charW;

    sprintf( s, " %.500s", tb->name );
    drawString( x, inTop + 2, UC( UI_STATUS_FG ), s );
    x += (float)( strlen( s ) + 3 ) * layout.charW;
    if( x < layout.charW * 28 ) {
        x = layout.charW * 28;
        }

    sprintf( s, "%-4s L%-6d C%-4d  %d lines   %s%s%s", pos,
             tb->cursorLine + 1, tb->cursorCol, tb->numLines,
             ! tb->isC ? "(Text" : config.autoNewline ? "(C/a" : "(C",
             tb->isC && config.hungryDelete ? "h" : "",
             ")" );
    drawString( x, inTop + 2, UC( UI_STATUS_FG ), s );
    x += (float)( strlen( s ) + 3 ) * layout.charW;

    if( comp_isRunning() ) {
        drawString( x, inTop + 2, UC( UI_STATUS_MODIFIED ), "[compiling]" );
        }
    else if( comp_numErrors() > 0 ) {
        int  ne;
        int  nw;

        comp_countErrors( &ne, &nw );
        sprintf( s, "[%d err %d warn]", ne, nw );
        drawString( x, inTop + 2, ne > 0 ? UC( UI_COMPILE_ERROR )
                    : UC( UI_STATUS_FG ), s );
        }
    }



static void drawMinibuffer( float  inTop,
                            float  inH,
                            float  inW ) {
    float  charW  =  layout.charW;
    float  y      =  inTop + 2;

    gui_rect( 0, inTop, inW, inTop + inH, UC( UI_MINIBUF_BG ) );

    if( ed.mbMode != MB_NONE ) {
        float  x  =  charW * 0.5f;
        float  cx;

        drawString( x, y, UC( UI_PROMPT ), ed.mbPrompt );
        x += (float)strlen( ed.mbPrompt ) * charW;
        drawString( x, y, UC( UI_MINIBUF_FG ), ed.mbText );
        cx = x + (float)ed.mbCursor * charW;
        if( ed.mbMode != MB_QUERY_REPLACE && ed.mbMode != MB_YES_NO
            && ed.mbMode != MB_ISEARCH ) {
            gui_rect( cx, y, cx + charW, y + layout.lineH, UC( UI_CURSOR ) );
            if( ed.mbCursor < (int)strlen( ed.mbText ) ) {
                gui_text( cx, y, UC( UI_CURSOR_TEXT ), ed.mbText + ed.mbCursor,
                          1 );
                }
            }
        else {
            gui_rect( cx, y, cx + charW, y + layout.lineH, UC( UI_CURSOR ) );
            }
        }
    else if( ed.message[ 0 ] != '\0' ) {
        drawString( charW * 0.5f, y, ed.messageIsError
                    ? UC( UI_MESSAGE_ERROR ) : UC( UI_MINIBUF_FG ),
                    ed.message );
        }
    }



/* completion candidates, drawn as a list growing up from the minibuffer */
static void drawCandidates( float  inBottom,
                            float  inW,
                            float  inMaxH ) {
    float  lineH  =  layout.lineH;
    float  charW  =  layout.charW;
    int    maxRows;
    int    rows;
    int    i;
    float  top;

    if( ed.numCands == 0 || ed.mbMode == MB_NONE ) {
        return;
        }
    maxRows = (int)( inMaxH / lineH ) - 1;
    if( maxRows > 15 ) {
        maxRows = 15;
        }
    if( maxRows < 1 ) {
        return;
        }
    rows = ed.numCands < maxRows ? ed.numCands : maxRows;

    if( ed.candSel >= 0 ) {
        if( ed.candSel < ed.candScroll ) {
            ed.candScroll = ed.candSel;
            }
        if( ed.candSel >= ed.candScroll + rows ) {
            ed.candScroll = ed.candSel - rows + 1;
            }
        }
    if( ed.candScroll > ed.numCands - rows ) {
        ed.candScroll = ed.numCands - rows;
        }
    if( ed.candScroll < 0 ) {
        ed.candScroll = 0;
        }

    top = inBottom - (float)rows * lineH - 4;
    if( ed.numCands > rows ) {
        top -= lineH;
        }
    gui_rect( 0, top, inW, inBottom, UC( UI_LIST_BG ) );
    gui_line( 0, top, inW, top, UC( UI_STATUS_BG ), 1.0f );
    for( i = 0;
         i < rows;
         i ++ ) {
        int    k  =  ed.candScroll + i;
        float  y  =  top + 2 + (float)i * lineH;

        if( k == ed.candSel ) {
            gui_rect( 0, y, inW, y + lineH, UC( UI_LIST_SELECTED ) );
            }
        drawString( charW, y, UC( UI_LIST_FG ), ed.cands[ k ] );
        }
    if( ed.numCands > rows ) {
        char  more[ 128 ];

        sprintf( more, "  [%d of %d shown;  type to narrow, Tab completes, "
                 "Up/Down choose]", rows, ed.numCands );
        drawString( charW, top + 2 + (float)rows * lineH, UC( UI_TAB_FG ),
                    more );
        }
    }



static void renderFrame( float  inDT ) {
    int    w;
    int    h;
    float  lineH;
    float  tabH;
    float  statusH;
    float  miniH;
    float  compileH  =  0;
    float  textBottom;

    SDL_GetWindowSize( window, &w, &h );
    layout.winW = (float)w;
    layout.winH = (float)h;

    gui_newFrame();

    if( gui_fontSize() != ed.fontSize ) {
        gui_setFontSize( ed.fontSize );
        }
    layout.charW = gui_charWidth();
    lineH = (float)floor( ed.fontSize * 1.2f ) + (float)config.lineSpacing;
    layout.lineH = lineH;

    tabH = lineH + 6;
    statusH = lineH + 4;
    miniH = lineH + 4;

    if( ed.showCompile && ed.compileBuffer != NULL ) {
        int  rows  =  (int)( ( (float)h * 0.3f ) / lineH );

        if( rows < 4 ) {
            rows = 4;
            }
        if( rows > 14 ) {
            rows = 14;
            }
        compileH = (float)( rows + 1 ) * lineH + 2;
        }
    textBottom = (float)h - miniH - statusH - compileH;

    drawTabBar( 0, tabH, (float)w );
    drawTextView( tabH, textBottom, (float)w, inDT );
    drawStatus( textBottom, statusH, (float)w );
    if( compileH > 0 ) {
        drawCompilePane( textBottom + statusH, (float)h - miniH, (float)w );
        }
    else {
        layout.compileTop = layout.compileBottom = -1;
        }
    drawMinibuffer( (float)h - miniH, miniH, (float)w );
    drawCandidates( (float)h - miniH, (float)w, textBottom - tabH );

    gui_render();
    SDL_GL_SwapWindow( window );
    }



/* ------------------------------------------------------------------
   Mouse
   ------------------------------------------------------------------ */

/* scrolls the text view so inTop is the top line, dragging the cursor
   along to stay on screen, like emacs.  inImmediate skips the smooth
   scrolling animation (scroll bar dragging). */
static void scrollViewTo( float  inTop,
                          int    inImmediate ) {
    TextBuffer  *tb   =  ed_cur();
    float        mxs  =  (float)( tb->numLines - 1 );
    int          t;
    int          last;

    if( inTop > mxs ) {
        inTop = mxs;
        }
    if( inTop < 0 ) {
        inTop = 0;
        }
    tb->targetScrollY = inTop;
    if( inImmediate ) {
        tb->scrollY = inTop;
        }
    t = (int)( inTop + 0.5f );
    last = lastLineFitting( tb, t, ed.visibleRows - config.scrollMargin );
    if( tb->cursorLine < t + config.scrollMargin ) {
        ed_mouseSetCursor( t + config.scrollMargin, 0, 0 );
        }
    else if( tb->cursorLine > last ) {
        ed_mouseSetCursor( last, 0, 0 );
        }
    }



/* the scroll bar thumb's top edge was dragged to inThumbY */
static void scrollBarDrag( float  inThumbY ) {
    float  trackH  =  layout.textBottom - layout.textTop;
    float  thumbH  =  layout.thumbBottom - layout.thumbTop;
    float  free    =  trackH - thumbH;
    float  lines   =  layout.scrollRange - (float)ed.visibleRows;
    float  f;

    if( free <= 0 || lines <= 0 ) {
        return;
        }
    f = ( inThumbY - layout.textTop ) / free;
    if( f < 0 ) {
        f = 0;
        }
    if( f > 1 ) {
        f = 1;
        }
    scrollViewTo( (float)floor( f * lines + 0.5f ), 1 );
    }


/* 1 if a window position is over the text (not the scroll bar) */
static int inTextArea( int  inX,
                       int  inY ) {
    float  x  =  (float)inX;
    float  y  =  (float)inY;

    if( y < layout.textTop || y >= layout.textBottom ) {
        return 0;
        }
    return layout.thumbTop < 0 || x < layout.scrollBarX;
    }



static void mouseToTextPos( int   inX,
                            int   inY,
                            int  *outLine,
                            int  *outCol ) {
    TextBuffer  *tb  =  ed_cur();
    float        fx  =  ( (float)inX - layout.textLeft ) / layout.charW
                        + (float)(int)tb->scrollX;
    int          c   =  (int)floor( fx );
    ViewRow     *r;
    int          i;
    int          l;

    if( numViewRows == 0 ) {
        *outLine = 0;
        *outCol = 0;
        return;
        }
    /* the row under the mouse (the first / last one above / below) */
    r = &( viewRows[ 0 ] );
    for( i = 0;
         i < numViewRows;
         i ++ ) {
        if( (float)inY >= viewRows[ i ].y ) {
            r = &( viewRows[ i ] );
            }
        }
    l = r->line;
    if( l >= tb->numLines ) {
        l = tb->numLines - 1;
        }
    if( c < 0 ) {
        c = 0;
        fx = 0;
        }
    /* fx, c:  display column (and the one under the mouse) */
    c += r->segStart;
    fx += (float)r->segStart;
    if( r->segEnd != SEG_END_MAX && c >= r->segEnd ) {
        /* past the end of a wrapped row:  its last char */
        c = r->segEnd - 1;
        }
    {
        /* the char whose cell is under the mouse;  the right half of a
           cell means the gap after it */
        Line  *ln  =  &( tb->lines[ l ] );
        int    b   =  tb_colAtVis( ln, c, config.tabWidth );

        if( b < ln->len ) {
            int  v0  =  VCOL( ln, b );
            int  v1  =  VCOL( ln, b + 1 );

            if( fx - (float)v0 >= (float)( v1 - v0 ) * 0.5f
                && ! ( r->segEnd != SEG_END_MAX && v1 >= r->segEnd ) ) {
                b ++;
                }
            }
        *outLine = l;
        *outCol = b;
        }
    }



/* ------------------------------------------------------------------
   main
   ------------------------------------------------------------------ */

static void usage( void ) {
    printf( "cedit %s -- a C89 code editor\n\n"
            "usage:  cedit [+LINE] [FILE...]\n"
            "        cedit --reindent FILE\n"
            "        cedit --format FILE\n"
            "        cedit --check-indent FILE...\n"
            "        cedit --script KEYS [FILE]\n"
            "        cedit --write-config\n\n"
            "Press F1 inside cedit for keys.\n", CEDIT_VERSION );
    }



static void setTitle( void ) {
    static char  last[ 600 ]  =  "";
    char         title[ 600 ];
    TextBuffer  *tb  =  ed_cur();

    sprintf( title, "cedit: %.500s", tb->name );
    if( strcmp( title, last ) != 0 ) {
        SDL_SetWindowTitle( window, title );
        strcpy( last, title );
        }
    }



int main( int     inArgc,
          char  **inArgv ) {
    SDL_GLContext  glContext;
    int            running       =  1;
    Uint32         lastTicks;
    int            framesLeft    =  -1;
    /* 1 = selecting text, 2 = dragging the scroll bar thumb */
    int            dragging      =  0;
    float          thumbGrab     =  0;
    int            suppressText  =  0;
    int            winW;
    int            winH;
    int            dirtyFrames   =  2;

    config_load();

    if( inArgc >= 2 ) {
        if( strcmp( inArgv[ 1 ], "--help" ) == 0
            || strcmp( inArgv[ 1 ], "-h" ) == 0 ) {
            usage();
            return 0;
            }
        if( strcmp( inArgv[ 1 ], "--version" ) == 0 ) {
            printf( "cedit %s\n", CEDIT_VERSION );
            return 0;
            }
        if( strcmp( inArgv[ 1 ], "--reindent" ) == 0 && inArgc >= 3 ) {
            return headlessReindent( inArgv[ 2 ], 0 );
            }
        if( strcmp( inArgv[ 1 ], "--format" ) == 0 && inArgc >= 3 ) {
            return headlessReindent( inArgv[ 2 ], 1 );
            }
        if( strcmp( inArgv[ 1 ], "--check-indent" ) == 0 ) {
            return headlessCheckIndent( inArgc - 2, inArgv + 2 );
            }
        if( strcmp( inArgv[ 1 ], "--script" ) == 0 && inArgc >= 3 ) {
            return headlessScript( inArgv[ 2 ],
                                   inArgc >= 4 ? inArgv[ 3 ] : NULL );
            }
        if( strcmp( inArgv[ 1 ], "--write-config" ) == 0 ) {
            char  msg[ 1024 ];

            config_writeDefaults( config_homeDir(), msg, sizeof( msg ) );
            printf( "%s\n", msg );
            return 0;
            }
        }

    if( getenv( "CEDIT_QUIT_AFTER" ) != NULL ) {
        framesLeft = atoi( getenv( "CEDIT_QUIT_AFTER" ) );
        if( framesLeft < 1 ) {
            framesLeft = 1;
            }
        }

    if( SDL_Init( SDL_INIT_VIDEO ) != 0 ) {
        fprintf( stderr, "cedit:  SDL_Init failed:  %s\n", SDL_GetError() );
        return 1;
        }
    SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
    SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 0 );

    {
        float  fs     =  (float)config.fontSize;
        float  charW  =  fs * 0.6f;
        float  lineH  =  (float)floor( fs * 1.2f ) + (float)config.lineSpacing;

        winW = (int)( charW * (float)( config.windowCols + 8 ) );
        winH = (int)( lineH * (float)( config.windowRows + 3 ) + 18 );
        }

    window = SDL_CreateWindow( "cedit", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, winW, winH,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE );
    if( window == NULL ) {
        fprintf( stderr, "cedit:  can't create window:  %s\n",
                 SDL_GetError() );
        return 1;
        }
    glContext = SDL_GL_CreateContext( window );
    if( glContext == NULL ) {
        fprintf( stderr, "cedit:  can't create GL context:  %s\n",
                 SDL_GetError() );
        return 1;
        }
    SDL_GL_MakeCurrent( window, glContext );
    SDL_GL_SetSwapInterval( 1 );

    if( ! gui_init( window, glContext, config.fontPath,
                    (float)config.fontSize ) ) {
        fprintf( stderr, "cedit:  GUI init failed\n" );
        return 1;
        }

    ed_init( inArgc, inArgv );
    SDL_StartTextInput();

    /* now that the real font metrics are known, size the window so
       exactly windowCols columns of text fit beside the gutter and the
       scroll bar (the guess above can't know the font's advance) */
    {
        float  charW  =  gui_charWidth();
        int    w      =  (int)ceil( gutterWidth( ed_cur(), charW )
                                    + charW * (float)config.windowCols
                                    + scrollBarWidth( charW ) );

        SDL_GetWindowSize( window, &winW, &winH );
        if( w != winW ) {
            SDL_SetWindowSize( window, w, winH );
            }
        }

    lastTicks = SDL_GetTicks();
    /* show the first frame as soon as possible */
    renderFrame( 0 );

    while( running ) {
        SDL_Event  e;
        int        gotEvent;
        int        timeout   =  1000;
        Uint32     now;
        float      dt;

        if( isAnimating() || comp_isRunning() || dirtyFrames > 0 ) {
            timeout = 15;
            }
        gotEvent = SDL_WaitEventTimeout( &e, timeout );

        while( gotEvent ) {
            gui_processEvent( &e );

            switch( e.type ) {
                case SDL_QUIT:
                    ed_requestQuit();
                    break;

                case SDL_WINDOWEVENT:
                    if( e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ) {
                        extern void ed_checkFilesOnDisk( void );

                        ed_checkFilesOnDisk();
                        }
                    break;

                case SDL_KEYDOWN: {
                    int  mods  =  0;
                    SDL_Keymod  m  =  (SDL_Keymod)e.key.keysym.mod;

                    if( m & KMOD_CTRL ) {
                        mods |= MOD_CTRL;
                        }
                    if( m & KMOD_ALT ) {
                        mods |= MOD_ALT;
                        }
                    if( m & KMOD_SHIFT ) {
                        mods |= MOD_SHIFT;
                        }
                    suppressText = ed_keyDown( e.key.keysym.sym, mods )
                                   || ( mods & ( MOD_CTRL | MOD_ALT ) ) != 0;
                    break;
                    }

                case SDL_TEXTINPUT:
                    if( ! suppressText
                        && (unsigned char)e.text.text[ 0 ] >= 32 ) {
                        ed_textInput( e.text.text );
                        }
                    break;

                case SDL_MOUSEBUTTONDOWN:
                    if( e.button.button == SDL_BUTTON_LEFT ) {
                        float  mx  =  (float)e.button.x;
                        float  my  =  (float)e.button.y;

                        if( my < layout.tabBottom ) {
                            int  i;

                            for( i = 0;
                                 i < layout.numTabs;
                                 i ++ ) {
                                if( mx >= layout.tabX0[ i ]
                                    && mx < layout.tabX1[ i ] ) {
                                    ed_switchToBuffer( i );
                                    }
                                }
                            }
                        else if( my >= layout.textTop
                                 && my < layout.textBottom
                                 && layout.thumbTop >= 0
                                 && mx >= layout.scrollBarX ) {
                            /* scroll bar:  grab the thumb, or jump so the
                               thumb centers on the click, then drag */
                            if( my >= layout.thumbTop
                                && my < layout.thumbBottom ) {
                                thumbGrab = my - layout.thumbTop;
                                }
                            else {
                                thumbGrab = ( layout.thumbBottom
                                              - layout.thumbTop ) / 2;
                                scrollBarDrag( my - thumbGrab );
                                }
                            dragging = 2;
                            }
                        else if( my >= layout.textTop
                                 && my < layout.textBottom ) {
                            int  l;
                            int  c;
                            int  shift  =  ( SDL_GetModState() & KMOD_SHIFT )
                                           != 0;

                            mouseToTextPos( e.button.x, e.button.y, &l, &c );
                            if( e.button.clicks == 2 ) {
                                ed_mouseSelectWord( l, c );
                                }
                            else {
                                ed_mouseSetCursor( l, c, shift );
                                dragging = 1;
                                }
                            }
                        else if( my >= layout.compileTop
                                 && my < layout.compileBottom
                                 && ed.compileBuffer != NULL ) {
                            int  row  =  (int)( ( my - layout.compileTop
                                                  - layout.lineH - 2 )
                                                / layout.lineH );

                            if( row >= 0 ) {
                                ed_visitErrorAtOutputLine(
                                    layout.compileFirstLine + row );
                                }
                            }
                        }
                    else if( e.button.button == SDL_BUTTON_MIDDLE
                             && inTextArea( e.button.x, e.button.y ) ) {
                        int  l;
                        int  c;

                        mouseToTextPos( e.button.x, e.button.y, &l, &c );
                        ed_mousePastePrimary( l, c );
                        }
                    break;

                case SDL_MOUSEBUTTONUP:
                    if( e.button.button == SDL_BUTTON_LEFT ) {
                        dragging = 0;
                        }
                    break;

                case SDL_MOUSEMOTION:
                    if( dragging == 2 ) {
                        scrollBarDrag( (float)e.motion.y - thumbGrab );
                        }
                    else if( dragging ) {
                        int  l;
                        int  c;

                        mouseToTextPos( e.motion.x, e.motion.y, &l, &c );
                        ed_mouseSetCursor( l, c, 1 );
                        }
                    break;

                case SDL_MOUSEWHEEL: {
                    int  mx;
                    int  my;

                    SDL_GetMouseState( &mx, &my );
                    if( (float)my >= layout.compileTop
                        && (float)my < layout.compileBottom ) {
                        ed.compileFollow = 0;
                        ed.compileScroll -= e.wheel.y * 3;
                        }
                    else {
                        scrollViewTo( ed_cur()->targetScrollY
                                      - (float)( e.wheel.y * 3 ), 0 );
                        }
                    break;
                    }

                default:
                    break;
                }
            dirtyFrames = 2;
            gotEvent = SDL_PollEvent( &e );
            }

        if( ed_tick() ) {
            dirtyFrames = 2;
            }
        if( ed.wantQuit ) {
            break;
            }

        now = SDL_GetTicks();
        dt = (float)( now - lastTicks ) / 1000.0f;
        if( dt > 0.1f ) {
            dt = 0.1f;
            }
        lastTicks = now;

        setTitle();
        renderFrame( dt );
        if( dirtyFrames > 0 ) {
            dirtyFrames --;
            }

        if( framesLeft > 0 ) {
            framesLeft --;
            if( framesLeft == 0 ) {
                break;
                }
            }
        }

    if( comp_isRunning() ) {
        comp_kill();
        }
    gui_shutdown();
    SDL_GL_DeleteContext( glContext );
    SDL_DestroyWindow( window );
    SDL_Quit();
    return 0;
    }
