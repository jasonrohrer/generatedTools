/*
  textBuffer.c

  See textBuffer.h
*/

#include "textBuffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>



static void *xrealloc( void  *inPtr,
                       size_t inSize ) {
    void  *p  =  realloc( inPtr, inSize );

    if( p == NULL && inSize > 0 ) {
        fprintf( stderr, "cedit:  out of memory\n" );
        exit( 1 );
        }
    return p;
    }



static void lineReserve( Line  *inLine,
                         int    inCap ) {
    int  curCap  =  inLine->cap < 0 ? - inLine->cap : inLine->cap;

    if( curCap < inCap ) {
        int  newCap  =  curCap * 2;

        if( newCap < inCap ) {
            newCap = inCap;
            }
        if( newCap < 16 ) {
            newCap = 16;
            }
        if( inLine->cap < 0 ) {
            /* copy out of the arena */
            char  *t  =  (char *)xrealloc( NULL, (size_t)newCap );

            memcpy( t, inLine->text, (size_t)inLine->len );
            inLine->text = t;
            }
        else {
            inLine->text = (char *)xrealloc( inLine->text, (size_t)newCap );
            }
        inLine->cap = newCap;
        }
    }



static void lineFree( Line  *inLine ) {
    if( inLine->cap > 0 ) {
        free( inLine->text );
        }
    inLine->text = NULL;
    inLine->cap = 0;
    inLine->len = 0;
    }



static void linesReserve( TextBuffer  *inTB,
                          int          inCount ) {
    if( inTB->capLines < inCount ) {
        int  newCap  =  inTB->capLines * 2;

        if( newCap < inCount ) {
            newCap = inCount;
            }
        if( newCap < 64 ) {
            newCap = 64;
            }
        inTB->lines = (Line *)xrealloc( inTB->lines,
                                        (size_t)newCap * sizeof( Line ) );
        inTB->capLines = newCap;
        }
    }



/* inserts inCount empty lines before index inAt */
static void insertEmptyLines( TextBuffer  *inTB,
                              int          inAt,
                              int          inCount ) {
    int  i;

    linesReserve( inTB, inTB->numLines + inCount );

    memmove( &( inTB->lines[ inAt + inCount ] ),
             &( inTB->lines[ inAt ] ),
             (size_t)( inTB->numLines - inAt ) * sizeof( Line ) );

    for( i = 0;
         i < inCount;
         i ++ ) {
        inTB->lines[ inAt + i ].text = NULL;
        inTB->lines[ inAt + i ].len = 0;
        inTB->lines[ inAt + i ].cap = 0;
        }
    inTB->numLines += inCount;
    }



static void removeLines( TextBuffer  *inTB,
                         int          inAt,
                         int          inCount ) {
    int  i;

    for( i = 0;
         i < inCount;
         i ++ ) {
        lineFree( &( inTB->lines[ inAt + i ] ) );
        }
    memmove( &( inTB->lines[ inAt ] ),
             &( inTB->lines[ inAt + inCount ] ),
             (size_t)( inTB->numLines - inAt - inCount ) * sizeof( Line ) );

    inTB->numLines -= inCount;
    }



static void invalidateFrom( TextBuffer  *inTB,
                            int          inLine ) {
    if( inTB->stateValidThrough > inLine ) {
        inTB->stateValidThrough = inLine;
        }
    inTB->changeCount ++;
    }



TextBuffer *tb_create( void ) {
    TextBuffer  *tb  =  (TextBuffer *)xrealloc( NULL, sizeof( TextBuffer ) );

    memset( tb, 0, sizeof( TextBuffer ) );

    linesReserve( tb, 1 );
    tb->numLines = 1;
    tb->lines[ 0 ].text = NULL;
    tb->lines[ 0 ].len = 0;
    tb->lines[ 0 ].cap = 0;

    tb->savedUndoPos = 0;
    tb->nextGroup = 1;
    tb->currentGroup = 0;
    tb->stateValidThrough = 0;
    tb->markMarker = tb_addMarker( tb, 0, 0 );
    tb->cursorMarker = tb_addMarker( tb, 0, 0 );
    tb->markActive = 0;
    tb->isC = 1;

    return tb;
    }



int tb_isCPath( const char  *inPath ) {
    static const char  *exts[]  =  { "c", "h", "cpp", "cc", "cxx", "hpp",
                                     "hh", "hxx", "C", "H", NULL };
    const char         *dot     =  strrchr( inPath, '.' );
    const char         *slash   =  strrchr( inPath, '/' );
    int                 i;

    if( dot == NULL || ( slash != NULL && dot < slash ) ) {
        return 0;
        }
    for( i = 0;
         exts[ i ] != NULL;
         i ++ ) {
        if( strcmp( dot + 1, exts[ i ] ) == 0 ) {
            return 1;
            }
        }
    return 0;
    }



static void freeUndoRange( TextBuffer  *inTB,
                           int          inFrom ) {
    int  i;

    for( i = inFrom;
         i < inTB->numUndo;
         i ++ ) {
        free( inTB->undo[ i ].text );
        }
    if( inFrom < inTB->numUndo ) {
        inTB->numUndo = inFrom;
        }
    }



void tb_destroy( TextBuffer  *inTB ) {
    int  i;

    for( i = 0;
         i < inTB->numLines;
         i ++ ) {
        lineFree( &( inTB->lines[ i ] ) );
        }
    free( inTB->lines );
    free( inTB->arena );
    freeUndoRange( inTB, 0 );
    free( inTB->undo );
    free( inTB->markers );
    free( inTB->lineState );
    free( inTB->path );
    free( inTB->name );
    free( inTB );
    }



/* replaces all contents with inText, which must be a malloc'd block
   that the buffer takes ownership of (lines point into it) */
static void setTextTakeOwnership( TextBuffer  *inTB,
                                  char        *inText,
                                  int          inLen ) {
    int  i;
    int  start;
    int  numLines  =  1;

    for( i = 0;
         i < inTB->numLines;
         i ++ ) {
        lineFree( &( inTB->lines[ i ] ) );
        }
    free( inTB->arena );
    inTB->arena = inText;

    {
        const char  *p    =  inText;
        const char  *end  =  inText + inLen;

        while( p < end
               &&
               ( p = (const char *)memchr( p, '\n', (size_t)( end - p ) ) )
               != NULL ) {
            numLines ++;
            p ++;
            }
        }

    inTB->numLines = 0;
    linesReserve( inTB, numLines );
    inTB->numLines = numLines;

    numLines = 0;
    start = 0;
    while( start <= inLen ) {
        const char  *nl  =  NULL;

        if( start < inLen ) {
            nl = (const char *)memchr( inText + start, '\n',
                                       (size_t)( inLen - start ) );
            }
        i = ( nl == NULL ) ? inLen : (int)( nl - inText );
        {
            Line  *l    =  &( inTB->lines[ numLines ] );
            int    len  =  i - start;

            if( len > 0 ) {
                l->text = inText + start;
                l->len = len;
                l->cap = - len;
                }
            else {
                l->text = NULL;
                l->len = 0;
                l->cap = 0;
                }
            numLines ++;
            start = i + 1;
            }
        }

    for( i = 0;
         i < inTB->numMarkers;
         i ++ ) {
        inTB->markers[ i ].line = 0;
        inTB->markers[ i ].col = 0;
        }
    inTB->cursorLine = 0;
    inTB->cursorCol = 0;
    invalidateFrom( inTB, 0 );
    }



void tb_setText( TextBuffer  *inTB,
                 const char  *inText,
                 int          inLen ) {
    char  *copy  =  (char *)xrealloc( NULL, (size_t)inLen + 1 );

    memcpy( copy, inText, (size_t)inLen );
    setTextTakeOwnership( inTB, copy, inLen );
    }



TextBuffer *tb_load( const char  *inPath,
                     int          inTabWidth,
                     int          inExpandTabs,
                     int         *outTabsExpanded ) {
    FILE        *f;
    long         size;
    char        *raw;
    char        *clean;
    long         i;
    long         n          =  0;
    long         col        =  0;
    int          numTabs    =  0;
    int          numCR      =  0;
    TextBuffer  *tb;

    if( outTabsExpanded != NULL ) {
        *outTabsExpanded = 0;
        }

    f = fopen( inPath, "rb" );
    if( f == NULL ) {
        return NULL;
        }
    fseek( f, 0, SEEK_END );
    size = ftell( f );
    fseek( f, 0, SEEK_SET );
    if( size < 0 ) {
        fclose( f );
        return NULL;
        }

    raw = (char *)xrealloc( NULL, (size_t)size + 1 );
    if( size > 0 && fread( raw, 1, (size_t)size, f ) != (size_t)size ) {
        fclose( f );
        free( raw );
        return NULL;
        }
    fclose( f );

    if( size > 0
        &&
        ( ! inExpandTabs || memchr( raw, '\t', (size_t)size ) == NULL )
        &&
        memchr( raw, '\r', (size_t)size ) == NULL ) {
        /* the common case:  nothing to clean, use the read buffer as is */
        clean = raw;
        n = size;
        }
    else {
        for( i = 0;
             i < size;
             i ++ ) {
            if( raw[ i ] == '\t' && inExpandTabs ) {
                numTabs ++;
                }
            }
        if( inTabWidth < 1 ) {
            inTabWidth = 4;
            }

        clean = (char *)xrealloc(
            NULL, (size_t)( size + numTabs * inTabWidth + 1 ) );

        for( i = 0;
             i < size;
             i ++ ) {
            char  c  =  raw[ i ];

            if( c == '\t' && inExpandTabs ) {
                int  spaces  =  inTabWidth - (int)( col % inTabWidth );

                while( spaces > 0 ) {
                    clean[ n ++ ] = ' ';
                    col ++;
                    spaces --;
                    }
                }
            else if( c == '\r' && i + 1 < size && raw[ i + 1 ] == '\n' ) {
                /* drop DOS line endings */
                numCR ++;
                }
            else {
                clean[ n ++ ] = c;
                if( c == '\n' ) {
                    col = 0;
                    }
                else {
                    col ++;
                    }
                }
            }
        free( raw );
        }

    /* final newline is implied by our line model */
    if( n > 0 && clean[ n - 1 ] == '\n' ) {
        n --;
        }

    tb = tb_create();
    setTextTakeOwnership( tb, clean, (int)n );

    tb->path = (char *)xrealloc( NULL, strlen( inPath ) + 1 );
    strcpy( tb->path, inPath );

    if( numTabs > 0 || numCR > 0 ) {
        /* not equal to the saved state */
        tb->savedUndoPos = -1;
        }
    if( outTabsExpanded != NULL ) {
        *outTabsExpanded = numTabs;
        }
    return tb;
    }



int tb_save( TextBuffer  *inTB,
             const char  *inPath ) {
    FILE  *f;
    int    i;
    int    ok  =  1;

    f = fopen( inPath, "wb" );
    if( f == NULL ) {
        return 0;
        }
    for( i = 0;
         i < inTB->numLines;
         i ++ ) {
        Line  *l  =  &( inTB->lines[ i ] );

        /* every line ends with a newline (load strips exactly one final
           newline, so this round-trips, and a file that lacked one gets
           it, like emacs's require-final-newline).  A trailing empty line
           is a real blank line at the end of the file.  Only a totally
           empty buffer writes an empty file. */
        if( inTB->numLines == 1 && l->len == 0 ) {
            break;
            }
        if( l->len > 0
            &&
            fwrite( l->text, 1, (size_t)l->len, f ) != (size_t)l->len ) {
            ok = 0;
            }
        if( fputc( '\n', f ) == EOF ) {
            ok = 0;
            }
        }
    if( fclose( f ) != 0 ) {
        ok = 0;
        }
    if( ok ) {
        inTB->savedUndoPos = inTB->undoPos;
        }
    return ok;
    }



int tb_isModified( TextBuffer  *inTB ) {
    return inTB->savedUndoPos != inTB->undoPos;
    }



void tb_clampPos( TextBuffer  *inTB,
                  int         *ioLine,
                  int         *ioCol ) {
    if( *ioLine < 0 ) {
        *ioLine = 0;
        *ioCol = 0;
        }
    if( *ioLine >= inTB->numLines ) {
        *ioLine = inTB->numLines - 1;
        *ioCol = inTB->lines[ *ioLine ].len;
        }
    if( *ioCol < 0 ) {
        *ioCol = 0;
        }
    if( *ioCol > inTB->lines[ *ioLine ].len ) {
        *ioCol = inTB->lines[ *ioLine ].len;
        }
    }



int tb_comparePos( int  inLineA,
                   int  inColA,
                   int  inLineB,
                   int  inColB ) {
    if( inLineA < inLineB ) {
        return -1;
        }
    if( inLineA > inLineB ) {
        return 1;
        }
    if( inColA < inColB ) {
        return -1;
        }
    if( inColA > inColB ) {
        return 1;
        }
    return 0;
    }



/* ------------------------------------------------------------------
   Undo recording
   ------------------------------------------------------------------ */

static void pushUndo( TextBuffer  *inTB,
                      int          inType,
                      int          inLine,
                      int          inCol,
                      const char  *inText,
                      int          inLen ) {
    UndoRecord  *r;

    /* new edit kills redo history */
    if( inTB->undoPos < inTB->numUndo ) {
        if( inTB->savedUndoPos > inTB->undoPos ) {
            inTB->savedUndoPos = -1;
            }
        freeUndoRange( inTB, inTB->undoPos );
        }

    if( inTB->currentGroup == 0 ) {
        tb_beginGroup( inTB );
        }

    /* coalesce consecutive typing into a single record */
    if( inTB->numUndo > 0 && inTB->savedUndoPos != inTB->numUndo ) {
        UndoRecord  *p  =  &( inTB->undo[ inTB->numUndo - 1 ] );

        if( inType == UNDO_INSERT
            &&
            p->type == UNDO_INSERT
            &&
            p->group == inTB->currentGroup
            &&
            p->line == inLine
            &&
            p->col + p->len == inCol
            &&
            memchr( p->text, '\n', (size_t)p->len ) == NULL
            &&
            memchr( inText, '\n', (size_t)inLen ) == NULL ) {

            p->text = (char *)xrealloc( p->text, (size_t)( p->len + inLen ) );
            memcpy( p->text + p->len, inText, (size_t)inLen );
            p->len += inLen;
            return;
            }
        }

    if( inTB->numUndo >= inTB->capUndo ) {
        inTB->capUndo = inTB->capUndo * 2 + 64;
        inTB->undo = (UndoRecord *)xrealloc(
            inTB->undo, (size_t)inTB->capUndo * sizeof( UndoRecord ) );
        }
    r = &( inTB->undo[ inTB->numUndo ] );
    r->type = inType;
    r->line = inLine;
    r->col = inCol;
    r->len = inLen;
    r->text = (char *)xrealloc( NULL, (size_t)inLen + 1 );
    memcpy( r->text, inText, (size_t)inLen );
    r->group = inTB->currentGroup;
    r->cursorLine = inTB->cursorLine;
    r->cursorCol = inTB->cursorCol;

    inTB->numUndo ++;
    inTB->undoPos = inTB->numUndo;
    }



void tb_beginGroup( TextBuffer  *inTB ) {
    inTB->currentGroup = inTB->nextGroup;
    inTB->nextGroup ++;
    }



void tb_continueGroup( TextBuffer  *inTB ) {
    if( inTB->undoPos > 0
        &&
        inTB->undoPos == inTB->numUndo ) {
        inTB->currentGroup = inTB->undo[ inTB->undoPos - 1 ].group;
        }
    else {
        tb_beginGroup( inTB );
        }
    }



void tb_clearUndo( TextBuffer  *inTB ) {
    freeUndoRange( inTB, 0 );
    inTB->undoPos = 0;
    inTB->savedUndoPos = 0;
    }



/* ------------------------------------------------------------------
   Raw edits (no undo recording)
   ------------------------------------------------------------------ */

static void adjustMarkersInsert( TextBuffer  *inTB,
                                 int          inLine,
                                 int          inCol,
                                 int          inEndLine,
                                 int          inEndCol ) {
    int  i;

    for( i = 0;
         i < inTB->numMarkers;
         i ++ ) {
        Marker  *m  =  &( inTB->markers[ i ] );

        if( ! m->inUse ) {
            continue;
            }
        if( m->line == inLine && m->col > inCol ) {
            m->col = inEndCol + ( m->col - inCol );
            m->line = inEndLine;
            }
        else if( m->line > inLine ) {
            m->line += inEndLine - inLine;
            }
        }
    }



static void adjustMarkersDelete( TextBuffer  *inTB,
                                 int          inLine,
                                 int          inCol,
                                 int          inEndLine,
                                 int          inEndCol ) {
    int  i;

    for( i = 0;
         i < inTB->numMarkers;
         i ++ ) {
        Marker  *m  =  &( inTB->markers[ i ] );

        if( ! m->inUse ) {
            continue;
            }
        if( tb_comparePos( m->line, m->col, inLine, inCol ) <= 0 ) {
            continue;
            }
        if( tb_comparePos( m->line, m->col, inEndLine, inEndCol ) <= 0 ) {
            m->line = inLine;
            m->col = inCol;
            }
        else if( m->line == inEndLine ) {
            m->col = inCol + ( m->col - inEndCol );
            m->line = inLine;
            }
        else {
            m->line -= inEndLine - inLine;
            }
        }
    }



static void rawInsert( TextBuffer  *inTB,
                       int          inLine,
                       int          inCol,
                       const char  *inText,
                       int          inLen,
                       int         *outLine,
                       int         *outCol ) {
    int    numNewlines  =  0;
    int    i;
    Line  *l;

    for( i = 0;
         i < inLen;
         i ++ ) {
        if( inText[ i ] == '\n' ) {
            numNewlines ++;
            }
        }

    if( numNewlines == 0 ) {
        l = &( inTB->lines[ inLine ] );
        lineReserve( l, l->len + inLen );
        memmove( l->text + inCol + inLen,
                 l->text + inCol,
                 (size_t)( l->len - inCol ) );
        memcpy( l->text + inCol, inText, (size_t)inLen );
        l->len += inLen;
        *outLine = inLine;
        *outCol = inCol + inLen;
        }
    else {
        char  *tail;
        int    tailLen;
        int    start;
        int    curLine;

        l = &( inTB->lines[ inLine ] );
        tailLen = l->len - inCol;
        tail = (char *)xrealloc( NULL, (size_t)tailLen + 1 );
        memcpy( tail, l->text + inCol, (size_t)tailLen );
        l->len = inCol;

        insertEmptyLines( inTB, inLine + 1, numNewlines );

        curLine = inLine;
        start = 0;
        for( i = 0;
             i <= inLen;
             i ++ ) {
            if( i == inLen || inText[ i ] == '\n' ) {
                int  segLen  =  i - start;

                l = &( inTB->lines[ curLine ] );
                lineReserve( l, l->len + segLen );
                memcpy( l->text + l->len, inText + start, (size_t)segLen );
                l->len += segLen;
                start = i + 1;
                if( i < inLen ) {
                    curLine ++;
                    }
                }
            }
        *outLine = curLine;
        *outCol = l->len;

        lineReserve( l, l->len + tailLen );
        memcpy( l->text + l->len, tail, (size_t)tailLen );
        l->len += tailLen;
        free( tail );
        }

    adjustMarkersInsert( inTB, inLine, inCol, *outLine, *outCol );
    invalidateFrom( inTB, inLine );
    }



static void rawDelete( TextBuffer  *inTB,
                       int          inStartLine,
                       int          inStartCol,
                       int          inEndLine,
                       int          inEndCol ) {
    Line  *l  =  &( inTB->lines[ inStartLine ] );

    if( inStartLine == inEndLine ) {
        memmove( l->text + inStartCol,
                 l->text + inEndCol,
                 (size_t)( l->len - inEndCol ) );
        l->len -= inEndCol - inStartCol;
        }
    else {
        Line  *e        =  &( inTB->lines[ inEndLine ] );
        int    tailLen  =  e->len - inEndCol;

        lineReserve( l, inStartCol + tailLen );
        /* l may not move, but e's pointer is stable;  re-fetch anyway */
        e = &( inTB->lines[ inEndLine ] );
        memcpy( l->text + inStartCol, e->text + inEndCol, (size_t)tailLen );
        l->len = inStartCol + tailLen;

        removeLines( inTB, inStartLine + 1, inEndLine - inStartLine );
        }

    adjustMarkersDelete( inTB, inStartLine, inStartCol, inEndLine, inEndCol );
    invalidateFrom( inTB, inStartLine );
    }



/* ------------------------------------------------------------------
   Public edits
   ------------------------------------------------------------------ */

void tb_insert( TextBuffer  *inTB,
                int          inLine,
                int          inCol,
                const char  *inText,
                int          inLen,
                int         *outLine,
                int         *outCol ) {
    int  endLine;
    int  endCol;

    tb_clampPos( inTB, &inLine, &inCol );

    if( inLen <= 0 ) {
        if( outLine != NULL ) {
            *outLine = inLine;
            }
        if( outCol != NULL ) {
            *outCol = inCol;
            }
        return;
        }

    pushUndo( inTB, UNDO_INSERT, inLine, inCol, inText, inLen );
    rawInsert( inTB, inLine, inCol, inText, inLen, &endLine, &endCol );

    if( outLine != NULL ) {
        *outLine = endLine;
        }
    if( outCol != NULL ) {
        *outCol = endCol;
        }
    }



void tb_delete( TextBuffer  *inTB,
                int          inStartLine,
                int          inStartCol,
                int          inEndLine,
                int          inEndCol ) {
    char  *text;
    int    len;

    tb_clampPos( inTB, &inStartLine, &inStartCol );
    tb_clampPos( inTB, &inEndLine, &inEndCol );

    if( tb_comparePos( inStartLine, inStartCol, inEndLine, inEndCol ) > 0 ) {
        int  t;

        t = inStartLine;
        inStartLine = inEndLine;
        inEndLine = t;
        t = inStartCol;
        inStartCol = inEndCol;
        inEndCol = t;
        }
    if( tb_comparePos( inStartLine, inStartCol, inEndLine, inEndCol ) == 0 ) {
        return;
        }

    text = tb_getText( inTB, inStartLine, inStartCol, inEndLine, inEndCol,
                       &len );
    pushUndo( inTB, UNDO_DELETE, inStartLine, inStartCol, text, len );
    free( text );

    rawDelete( inTB, inStartLine, inStartCol, inEndLine, inEndCol );
    }



char *tb_getText( TextBuffer  *inTB,
                  int          inStartLine,
                  int          inStartCol,
                  int          inEndLine,
                  int          inEndCol,
                  int         *outLen ) {
    int    total  =  0;
    int    i;
    char  *out;
    int    n      =  0;

    tb_clampPos( inTB, &inStartLine, &inStartCol );
    tb_clampPos( inTB, &inEndLine, &inEndCol );

    if( tb_comparePos( inStartLine, inStartCol, inEndLine, inEndCol ) > 0 ) {
        out = (char *)xrealloc( NULL, 1 );
        out[ 0 ] = '\0';
        if( outLen != NULL ) {
            *outLen = 0;
            }
        return out;
        }

    for( i = inStartLine;
         i <= inEndLine;
         i ++ ) {
        total += inTB->lines[ i ].len + 1;
        }
    out = (char *)xrealloc( NULL, (size_t)total + 1 );

    for( i = inStartLine;
         i <= inEndLine;
         i ++ ) {
        Line  *l  =  &( inTB->lines[ i ] );
        int    s  =  0;
        int    e  =  l->len;

        if( i == inStartLine ) {
            s = inStartCol;
            }
        if( i == inEndLine ) {
            e = inEndCol;
            }
        if( e > s ) {
            memcpy( out + n, l->text + s, (size_t)( e - s ) );
            n += e - s;
            }
        if( i != inEndLine ) {
            out[ n ++ ] = '\n';
            }
        }
    out[ n ] = '\0';
    if( outLen != NULL ) {
        *outLen = n;
        }
    return out;
    }



/* ------------------------------------------------------------------
   Undo / redo
   ------------------------------------------------------------------ */

int tb_undo( TextBuffer  *inTB ) {
    int  group;
    int  curLine  =  0;
    int  curCol   =  0;

    if( inTB->undoPos <= 0 ) {
        return 0;
        }
    group = inTB->undo[ inTB->undoPos - 1 ].group;

    while( inTB->undoPos > 0
           &&
           inTB->undo[ inTB->undoPos - 1 ].group == group ) {
        UndoRecord  *r  =  &( inTB->undo[ inTB->undoPos - 1 ] );
        int          el;
        int          ec;

        if( r->type == UNDO_INSERT ) {
            /* figure out end of the inserted text */
            int  i;

            el = r->line;
            ec = r->col;
            for( i = 0;
                 i < r->len;
                 i ++ ) {
                if( r->text[ i ] == '\n' ) {
                    el ++;
                    ec = 0;
                    }
                else {
                    ec ++;
                    }
                }
            rawDelete( inTB, r->line, r->col, el, ec );
            }
        else {
            rawInsert( inTB, r->line, r->col, r->text, r->len, &el, &ec );
            }
        curLine = r->cursorLine;
        curCol = r->cursorCol;
        inTB->undoPos --;
        }

    inTB->cursorLine = curLine;
    inTB->cursorCol = curCol;
    tb_clampPos( inTB, &( inTB->cursorLine ), &( inTB->cursorCol ) );
    inTB->goalCol = inTB->cursorCol;
    /* next edit starts a fresh group */
    inTB->currentGroup = 0;
    return 1;
    }



int tb_redo( TextBuffer  *inTB ) {
    int  group;
    int  el  =  0;
    int  ec  =  0;

    if( inTB->undoPos >= inTB->numUndo ) {
        return 0;
        }
    group = inTB->undo[ inTB->undoPos ].group;

    while( inTB->undoPos < inTB->numUndo
           &&
           inTB->undo[ inTB->undoPos ].group == group ) {
        UndoRecord  *r  =  &( inTB->undo[ inTB->undoPos ] );

        if( r->type == UNDO_INSERT ) {
            rawInsert( inTB, r->line, r->col, r->text, r->len, &el, &ec );
            }
        else {
            int  i;

            el = r->line;
            ec = r->col;
            for( i = 0;
                 i < r->len;
                 i ++ ) {
                if( r->text[ i ] == '\n' ) {
                    el ++;
                    ec = 0;
                    }
                else {
                    ec ++;
                    }
                }
            rawDelete( inTB, r->line, r->col, el, ec );
            el = r->line;
            ec = r->col;
            }
        inTB->undoPos ++;
        }
    inTB->cursorLine = el;
    inTB->cursorCol = ec;
    tb_clampPos( inTB, &( inTB->cursorLine ), &( inTB->cursorCol ) );
    inTB->goalCol = inTB->cursorCol;
    inTB->currentGroup = 0;
    return 1;
    }



/* ------------------------------------------------------------------
   Markers
   ------------------------------------------------------------------ */

int tb_addMarker( TextBuffer  *inTB,
                  int          inLine,
                  int          inCol ) {
    int  i;

    for( i = 0;
         i < inTB->numMarkers;
         i ++ ) {
        if( ! inTB->markers[ i ].inUse ) {
            break;
            }
        }
    if( i == inTB->numMarkers ) {
        inTB->numMarkers ++;
        inTB->markers = (Marker *)xrealloc(
            inTB->markers, (size_t)inTB->numMarkers * sizeof( Marker ) );
        }
    inTB->markers[ i ].inUse = 1;
    inTB->markers[ i ].line = inLine;
    inTB->markers[ i ].col = inCol;
    return i;
    }



void tb_removeMarker( TextBuffer  *inTB,
                      int          inID ) {
    if( inID >= 0 && inID < inTB->numMarkers ) {
        inTB->markers[ inID ].inUse = 0;
        }
    }



void tb_setMarker( TextBuffer  *inTB,
                   int          inID,
                   int          inLine,
                   int          inCol ) {
    if( inID >= 0 && inID < inTB->numMarkers ) {
        tb_clampPos( inTB, &inLine, &inCol );
        inTB->markers[ inID ].line = inLine;
        inTB->markers[ inID ].col = inCol;
        }
    }



void tb_getMarker( TextBuffer  *inTB,
                   int          inID,
                   int         *outLine,
                   int         *outCol ) {
    if( inID >= 0 && inID < inTB->numMarkers ) {
        *outLine = inTB->markers[ inID ].line;
        *outCol = inTB->markers[ inID ].col;
        tb_clampPos( inTB, outLine, outCol );
        }
    else {
        *outLine = 0;
        *outCol = 0;
        }
    }



/* ------------------------------------------------------------------
   Helpers
   ------------------------------------------------------------------ */

int tb_lineIndent( TextBuffer  *inTB,
                   int          inLine ) {
    Line  *l  =  &( inTB->lines[ inLine ] );
    int    i  =  0;

    while( i < l->len && l->text[ i ] == ' ' ) {
        i ++;
        }
    return i;
    }



int tb_lineIsBlank( TextBuffer  *inTB,
                    int          inLine ) {
    Line  *l  =  &( inTB->lines[ inLine ] );
    int    i;

    for( i = 0;
         i < l->len;
         i ++ ) {
        if( l->text[ i ] != ' ' && l->text[ i ] != '\t' ) {
            return 0;
            }
        }
    return 1;
    }



int tb_visCol( const Line  *inL,
               int          inCol,
               int          inTabWidth ) {
    int  i;
    int  v  =  0;

    if( inCol > inL->len ) {
        inCol = inL->len;
        }
    if( inCol <= 0 ) {
        return 0;
        }
    if( memchr( inL->text, '\t', (size_t)inCol ) == NULL ) {
        return inCol;
        }
    if( inTabWidth < 1 ) {
        inTabWidth = 1;
        }
    for( i = 0;
         i < inCol;
         i ++ ) {
        if( inL->text[ i ] == '\t' ) {
            v += inTabWidth - v % inTabWidth;
            }
        else {
            v ++;
            }
        }
    return v;
    }



int tb_colAtVis( const Line  *inL,
                 int          inVis,
                 int          inTabWidth ) {
    int  i;
    int  v  =  0;

    if( inVis <= 0 ) {
        return 0;
        }
    if( inL->len == 0
        ||
        memchr( inL->text, '\t', (size_t)inL->len ) == NULL ) {
        return inVis < inL->len ? inVis : inL->len;
        }
    if( inTabWidth < 1 ) {
        inTabWidth = 1;
        }
    for( i = 0;
         i < inL->len;
         i ++ ) {
        int  w  =  1;

        if( inL->text[ i ] == '\t' ) {
            w = inTabWidth - v % inTabWidth;
            }
        if( inVis < v + w ) {
            return i;
            }
        v += w;
        }
    return inL->len;
    }



int tb_setLineIndent( TextBuffer  *inTB,
                      int          inLine,
                      int          inIndent ) {
    int  cur  =  tb_lineIndent( inTB, inLine );

    if( inIndent < 0 ) {
        inIndent = 0;
        }
    if( cur == inIndent ) {
        return 0;
        }
    if( cur > inIndent ) {
        tb_delete( inTB, inLine, 0, inLine, cur - inIndent );
        }
    else {
        char  spaces[ 256 ];
        int   need  =  inIndent - cur;

        memset( spaces, ' ', sizeof( spaces ) );
        while( need > 0 ) {
            int  n  =  need;

            if( n > (int)sizeof( spaces ) ) {
                n = (int)sizeof( spaces );
                }
            tb_insert( inTB, inLine, 0, spaces, n, NULL, NULL );
            need -= n;
            }
        }
    return inIndent - cur;
    }
