/*
  format.c

  See format.h
*/

#include "format.h"
#include "syntax.h"
#include "indent.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>


/* ------------------------------------------------------------------
   Space edits on a single line
   ------------------------------------------------------------------ */

#define MAX_EDITS  512

/* replace del chars at col with ins spaces followed by stars '*'s */
typedef struct SpaceEdit {
        int  col;
        int  del;
        int  ins;
        int  stars;
    } SpaceEdit;


typedef struct EditList {
        SpaceEdit  e[ MAX_EDITS ];
        int        num;
    } EditList;



static void addEditStars( EditList  *inList,
                          int        inCol,
                          int        inDel,
                          int        inIns,
                          int        inStars ) {
    if( inList->num < MAX_EDITS ) {
        inList->e[ inList->num ].col = inCol;
        inList->e[ inList->num ].del = inDel;
        inList->e[ inList->num ].ins = inIns;
        inList->e[ inList->num ].stars = inStars;
        inList->num ++;
        }
    }



static void addEdit( EditList  *inList,
                     int        inCol,
                     int        inDel,
                     int        inIns ) {
    if( inDel == inIns ) {
        return;
        }
    addEditStars( inList, inCol, inDel, inIns, 0 );
    }



static int compareEdits( const void  *inA,
                         const void  *inB ) {
    const SpaceEdit  *a  =  (const SpaceEdit *)inA;
    const SpaceEdit  *b  =  (const SpaceEdit *)inB;

    return b->col - a->col;
    }



/* applies edits right to left;  returns mapped cursor col */
static int applyEdits( TextBuffer  *inTB,
                       int          inLine,
                       EditList    *inList,
                       int          inCursorCol ) {
    char  buf[ 256 ];
    int   i;
    int   cursor  =  inCursorCol;

    qsort( inList->e, (size_t)inList->num, sizeof( SpaceEdit ),
           compareEdits );

    for( i = 0;
         i < inList->num;
         i ++ ) {
        SpaceEdit  *e    =  &( inList->e[ i ] );
        int         n    =  0;
        int         j;

        if( i > 0 && e->col == inList->e[ i - 1 ].col ) {
            /* two edits at one spot;  skip the second */
            continue;
            }
        if( e->del > 0 ) {
            tb_delete( inTB, inLine, e->col, inLine, e->col + e->del );
            }
        for( j = 0;
             j < e->ins && n < (int)sizeof( buf ) - 32;
             j ++ ) {
            buf[ n ++ ] = ' ';
            }
        for( j = 0;
             j < e->stars && n < (int)sizeof( buf );
             j ++ ) {
            buf[ n ++ ] = '*';
            }
        if( n > 0 ) {
            tb_insert( inTB, inLine, e->col, buf, n, NULL, NULL );
            }

        if( cursor > e->col ) {
            if( cursor >= e->col + e->del ) {
                cursor += n - e->del;
                }
            else {
                cursor = e->col + n;
                }
            }
        }
    inList->num = 0;
    return cursor;
    }



/* ------------------------------------------------------------------
   Token helpers
   ------------------------------------------------------------------ */

static int isP( Token  *inT,
                char    inC ) {
    return inT->type == TK_PUNCT && inT->len == 1 && inT->c == inC;
    }



static int tokEnd( Token  *inT ) {
    return inT->start + inT->len;
    }



/* keywords that may appear in a declaration's type */
static int isDeclKeyword( int  inKW ) {
    switch( inKW ) {
        case KW_CHAR: case KW_DOUBLE: case KW_FLOAT: case KW_INT:
        case KW_LONG: case KW_SHORT: case KW_SIGNED: case KW_UNSIGNED:
        case KW_VOID: case KW_CONST: case KW_VOLATILE: case KW_STATIC:
        case KW_EXTERN: case KW_REGISTER: case KW_AUTO: case KW_STRUCT:
        case KW_UNION: case KW_ENUM: case KW_TYPEDEF:
            return 1;
        default:
            return 0;
        }
    }



/* is the paren group tokens (inOpen, inClose) a cast like (int) or
   (const char *) or (Deck*)? */
static int isCast( const char  *inText,
                   Token       *inToks,
                   int          inOpen,
                   int          inClose ) {
    int  k;
    int  sawType   =  0;
    int  sawStar   =  0;
    int  numIdent  =  0;

    if( inClose - inOpen < 2 ) {
        return 0;
        }
    /* a paren right after a name is a call or a declarator, not a cast */
    if( inOpen > 0 ) {
        Token  *p  =  &( inToks[ inOpen - 1 ] );

        if( p->type == TK_IDENT || isP( p, ')' ) || isP( p, ']' )
            ||
            ( p->type == TK_KEYWORD && p->kw == KW_SIZEOF ) ) {
            return 0;
            }
        }
    for( k = inOpen + 1;
         k < inClose;
         k ++ ) {
        Token  *t  =  &( inToks[ k ] );

        if( isP( t, '*' ) ) {
            if( ! sawType && numIdent == 0 ) {
                return 0;
                }
            sawStar = 1;
            continue;
            }
        if( sawStar ) {
            return 0;
            }
        if( t->type == TK_KEYWORD ) {
            if( ! isDeclKeyword( t->kw ) ) {
                return 0;
                }
            sawType = 1;
            continue;
            }
        if( t->type == TK_IDENT ) {
            numIdent ++;
            if( syn_isTypeName( inText + t->start, t->len ) ) {
                sawType = 1;
                }
            continue;
            }
        return 0;
        }
    if( sawType ) {
        return 1;
        }
    /* unknown single identifier:  (Deck *) or (MyType) style */
    if( numIdent == 1 ) {
        Token  *t  =  &( inToks[ inOpen + 1 ] );
        char    c  =  inText[ t->start ];

        if( sawStar ) {
            return 1;
            }
        if( c >= 'A' && c <= 'Z' ) {
            /* but not an ALL_CAPS macro/constant */
            int  i;

            for( i = 0;
                 i < t->len;
                 i ++ ) {
                char  d  =  inText[ t->start + i ];

                if( d >= 'a' && d <= 'z' ) {
                    return 1;
                    }
                }
            }
        }
    return 0;
    }



/* ------------------------------------------------------------------
   Declaration rows (a type, a name, maybe an initializer)
   ------------------------------------------------------------------ */

typedef struct DeclRow {
        int  line;
        int  typeStart;
        int  typeEnd;
        int  numStars;
        int  nameStart;
        /* end of name including any [ ] dimensions */
        int  nameEnd;
        /* column of '=' or -1 */
        int  eqStart;
        int  valueStart;
        /* column of the token after the name (';' ',' ')' '=') */
        int  afterName;
    } DeclRow;



/* parses toks[ inFrom .. inTo ) as  type-words stars name [dims].
   Returns 1 on success. */
static int parseDeclSpan( const char  *inText,
                          Token       *inToks,
                          int          inFrom,
                          int          inTo,
                          DeclRow     *outRow ) {
    int  k  =  inTo - 1;
    int  nameTok;
    int  firstStar;
    int  numTypeToks;
    int  i;

    (void)inText;
    if( inTo - inFrom < 2 ) {
        return 0;
        }
    /* skip trailing [ ] groups */
    while( k >= inFrom && isP( &( inToks[ k ] ), ']' ) ) {
        int  depth  =  0;

        while( k >= inFrom ) {
            if( isP( &( inToks[ k ] ), ']' ) ) {
                depth ++;
                }
            else if( isP( &( inToks[ k ] ), '[' ) ) {
                depth --;
                if( depth == 0 ) {
                    break;
                    }
                }
            k --;
            }
        k --;
        }
    if( k < inFrom || inToks[ k ].type != TK_IDENT ) {
        return 0;
        }
    nameTok = k;
    k --;
    firstStar = nameTok;
    while( k >= inFrom && isP( &( inToks[ k ] ), '*' ) ) {
        firstStar = k;
        k --;
        }
    numTypeToks = k - inFrom + 1;
    if( numTypeToks < 1 ) {
        return 0;
        }
    for( i = inFrom;
         i <= k;
         i ++ ) {
        Token  *t  =  &( inToks[ i ] );

        if( t->type == TK_IDENT ) {
            continue;
            }
        if( t->type == TK_KEYWORD && isDeclKeyword( t->kw ) ) {
            continue;
            }
        return 0;
        }
    /* "x y" where neither is a type keyword and there are 3+ idents is
       unusual;  but "unsigned long int x" is fine.  Accept. */

    outRow->typeStart = inToks[ inFrom ].start;
    outRow->typeEnd = tokEnd( &( inToks[ k ] ) );
    outRow->numStars = nameTok - firstStar;
    outRow->nameStart = inToks[ nameTok ].start;
    outRow->nameEnd = tokEnd( &( inToks[ inTo - 1 ] ) );
    outRow->eqStart = -1;
    outRow->valueStart = -1;
    outRow->afterName = -1;
    return 1;
    }



static TokenList  lineToks;
static int        lineToksInit  =  0;


/* lexes a line into lineToks, dropping comments.  Returns number of
   significant tokens, or -1 if the line isn't plain code.  If outComment
   is non-NULL, gets 1 if there was a comment anywhere but at the end. */
static int lexCode( TextBuffer  *inTB,
                    int          inLine,
                    int         *outInnerComment ) {
    Line  *l  =  &( inTB->lines[ inLine ] );
    int    state;
    int    k;
    int    n  =  0;
    int    sawComment  =  0;

    if( ! lineToksInit ) {
        syn_initTokenList( &lineToks );
        lineToksInit = 1;
        }
    if( outInnerComment != NULL ) {
        *outInnerComment = 0;
        }
    state = syn_getLineState( inTB, inLine );
    if( state != 0 ) {
        return -1;
        }
    syn_lexLine( l->text, l->len, state, &lineToks );

    for( k = 0;
         k < lineToks.num;
         k ++ ) {
        Token  *t  =  &( lineToks.tokens[ k ] );

        if( t->inPreproc ) {
            return -1;
            }
        if( t->type == TK_COMMENT ) {
            sawComment = 1;
            continue;
            }
        if( sawComment && outInnerComment != NULL ) {
            *outInnerComment = 1;
            }
        lineToks.tokens[ n ] = *t;
        n ++;
        }
    lineToks.num = n;
    return n;
    }



/* parses a whole line as a single declaration */
static int parseDeclLine( TextBuffer  *inTB,
                          int          inLine,
                          DeclRow     *outRow ) {
    Line   *l      =  &( inTB->lines[ inLine ] );
    int     inner;
    int     n      =  lexCode( inTB, inLine, &inner );
    Token  *toks   =  lineToks.tokens;
    int     k;
    int     depth  =  0;
    int     stop   =  -1;

    if( n < 3 || inner ) {
        return 0;
        }
    if( ! isP( &( toks[ n - 1 ] ), ';' ) ) {
        return 0;
        }
    for( k = 0;
         k < n;
         k ++ ) {
        Token  *t  =  &( toks[ k ] );

        if( isP( t, '[' ) ) {
            depth ++;
            }
        else if( isP( t, ']' ) ) {
            depth --;
            }
        else if( depth == 0 && ( isP( t, '=' ) || isP( t, ';' ) ) ) {
            stop = k;
            break;
            }
        else if( depth == 0
                 &&
                 ( isP( t, '(' ) || isP( t, ',' ) || isP( t, '{' )
                   || ( t->type == TK_PUNCT && ! isP( t, '*' ) ) ) ) {
            return 0;
            }
        }
    if( stop < 0 ) {
        return 0;
        }
    if( ! parseDeclSpan( l->text, toks, 0, stop, outRow ) ) {
        return 0;
        }
    outRow->line = inLine;
    outRow->afterName = toks[ stop ].start;

    if( isP( &( toks[ stop ] ), '=' ) ) {
        /* initializer must be simple:  no top level comma, no braces */
        int  d  =  0;

        if( stop + 1 >= n - 1 ) {
            return 0;
            }
        for( k = stop + 1;
             k < n - 1;
             k ++ ) {
            Token  *t  =  &( toks[ k ] );

            if( isP( t, '(' ) || isP( t, '[' ) ) {
                d ++;
                }
            else if( isP( t, ')' ) || isP( t, ']' ) ) {
                d --;
                }
            else if( isP( t, '{' ) || isP( t, '}' ) ) {
                return 0;
                }
            else if( d == 0 && isP( t, ',' ) ) {
                return 0;
                }
            }
        outRow->eqStart = toks[ stop ].start;
        outRow->valueStart = toks[ stop + 1 ].start;
        }
    else if( stop != n - 1 ) {
        return 0;
        }
    return 1;
    }



int fmt_isDeclarationLine( TextBuffer  *inTB,
                           int          inLine ) {
    DeclRow  row;

    return parseDeclLine( inTB, inLine, &row );
    }



/* lines up rows (each on its own line;  all rows of a table share the
   same typeStart).  inEqSpaces < 0 means these are parameters (no
   initializers and no ';' to tighten). */
static int alignRows( TextBuffer  *inTB,
                      DeclRow     *inRows,
                      int          inNumRows,
                      int          inTypeGap,
                      int          inEqSpaces,
                      int         *ioCursorLine,
                      int         *ioCursorCol ) {
    static EditList  edits;
    int              typeW    =  0;
    int              starMax  =  0;
    int              nameCol;
    int              eqCol    =  -1;
    int              i;
    int              changed  =  0;

    for( i = 0;
         i < inNumRows;
         i ++ ) {
        DeclRow  *r  =  &( inRows[ i ] );

        if( r->typeEnd - r->typeStart > typeW ) {
            typeW = r->typeEnd - r->typeStart;
            }
        if( r->numStars > starMax ) {
            starMax = r->numStars;
            }
        }
    nameCol = inRows[ 0 ].typeStart + typeW + inTypeGap + starMax;

    for( i = 0;
         i < inNumRows && inEqSpaces >= 0;
         i ++ ) {
        DeclRow  *r  =  &( inRows[ i ] );

        if( r->eqStart >= 0 ) {
            int  c  =  nameCol + ( r->nameEnd - r->nameStart ) + inEqSpaces;

            if( c > eqCol ) {
                eqCol = c;
                }
            }
        }

    for( i = 0;
         i < inNumRows;
         i ++ ) {
        DeclRow  *r           =  &( inRows[ i ] );
        Line     *l           =  &( inTB->lines[ r->line ] );
        int       newNameEnd  =  nameCol + ( r->nameEnd - r->nameStart );
        int       wantGap     =  nameCol - r->numStars - r->typeEnd;
        int       spanOK      =  1;
        int       j;

        edits.num = 0;

        /* the span between type and name must be only spaces and stars */
        for( j = r->typeEnd;
             j < r->nameStart;
             j ++ ) {
            if( l->text[ j ] != ' ' && l->text[ j ] != '*' ) {
                spanOK = 0;
                }
            }
        if( ! spanOK || wantGap < 1 ) {
            continue;
            }

        if( inEqSpaces >= 0 ) {
            if( r->eqStart >= 0 ) {
                addEdit( &edits, r->eqStart + 1,
                         r->valueStart - ( r->eqStart + 1 ), inEqSpaces );
                addEdit( &edits, r->nameEnd, r->eqStart - r->nameEnd,
                         eqCol - newNameEnd );
                }
            else if( r->afterName >= 0 ) {
                addEdit( &edits, r->nameEnd, r->afterName - r->nameEnd, 0 );
                }
            }

        /* type-to-name span:  already right? */
        {
            int  same  =  ( r->nameStart - r->typeEnd
                            == wantGap + r->numStars );

            for( j = 0;
                 same && j < wantGap + r->numStars;
                 j ++ ) {
                char  want  =  ( j < wantGap ) ? ' ' : '*';

                if( l->text[ r->typeEnd + j ] != want ) {
                    same = 0;
                    }
                }
            if( ! same ) {
                addEditStars( &edits, r->typeEnd, r->nameStart - r->typeEnd,
                              wantGap, r->numStars );
                }
            }

        if( edits.num > 0 ) {
            int  onCursor  =  ioCursorLine != NULL
                              &&
                              ioCursorCol != NULL
                              &&
                              *ioCursorLine == r->line;
            int  c         =  onCursor ? *ioCursorCol : 0;

            c = applyEdits( inTB, r->line, &edits, c );
            if( onCursor ) {
                *ioCursorCol = c;
                }
            changed = 1;
            }
        }
    return changed;
    }



int fmt_alignDeclarations( TextBuffer  *inTB,
                           int          inLine,
                           int         *ioCursorLine,
                           int         *ioCursorCol ) {
    static DeclRow  *rows     =  NULL;
    static int       rowCap   =  0;
    int              first    =  inLine;
    int              last     =  inLine;
    int              indent;
    int              i;
    int              n        =  0;
    DeclRow          row;

    if( ! config.alignDeclarations ) {
        return 0;
        }
    if( ! parseDeclLine( inTB, inLine, &row ) ) {
        return 0;
        }
    indent = row.typeStart;

    while( first > 0
           &&
           parseDeclLine( inTB, first - 1, &row )
           &&
           row.typeStart == indent ) {
        first --;
        }
    while( last < inTB->numLines - 1
           &&
           parseDeclLine( inTB, last + 1, &row )
           &&
           row.typeStart == indent ) {
        last ++;
        }

    if( rowCap < last - first + 1 ) {
        rowCap = last - first + 1 + 16;
        rows = (DeclRow *)realloc( rows, (size_t)rowCap * sizeof( DeclRow ) );
        }
    for( i = first;
         i <= last;
         i ++ ) {
        if( parseDeclLine( inTB, i, &( rows[ n ] ) ) ) {
            n ++;
            }
        }
    if( n == 0 ) {
        return 0;
        }
    return alignRows( inTB, rows, n, config.declTypeSpaces,
                      config.declEqualsSpaces, ioCursorLine, ioCursorCol );
    }



/* ------------------------------------------------------------------
   Function parameter lists
   ------------------------------------------------------------------ */

/* aligns the parameter list whose '(' is token inOpenTok on inLine (in
   lineToks).  Handles both one-line lists and one-param-per-line
   tables. */
static void formatParamList( TextBuffer  *inTB,
                             int          inLine,
                             int          inOpenCol,
                             int         *ioCursorLine,
                             int         *ioCursorCol ) {
    int       closeLine;
    int       closeCol;
    DeclRow   rows[ 64 ];
    int       numRows  =  0;
    int       li;

    if( config.paramTypeSpaces <= 0 ) {
        return;
        }
    if( ! ind_findMatchingBracket( inTB, inLine, inOpenCol, 64,
                                   &closeLine, &closeCol ) ) {
        /* list not closed yet:  format through the current line */
        closeLine = -1;
        }

    for( li = inLine;
         li <= ( closeLine >= 0 ? closeLine : inLine );
         li ++ ) {
        int     n     =  lexCode( inTB, li, NULL );
        Token  *toks  =  lineToks.tokens;
        int     k     =  0;
        int     start;
        int     numOnLine  =  0;
        DeclRow lineRows[ 32 ];
        int     r;

        if( n < 0 ) {
            return;
            }
        if( li == inLine ) {
            while( k < n && ! ( isP( &( toks[ k ] ), '(' )
                                && toks[ k ].start == inOpenCol ) ) {
                k ++;
                }
            k ++;
            }
        start = k;
        while( k <= n ) {
            if( k == n || isP( &( toks[ k ] ), ',' )
                ||
                ( li == closeLine && isP( &( toks[ k ] ), ')' )
                  && toks[ k ].start == closeCol ) ) {
                if( k > start ) {
                    DeclRow  *dr  =  &( lineRows[ numOnLine ] );

                    if( k == n && li != closeLine ) {
                        /* param continues on next line?  only accept if
                           this ends with a comma (handled above) */
                        return;
                        }
                    if( numOnLine >= 32
                        ||
                        ! parseDeclSpan( inTB->lines[ li ].text, toks, start,
                                         k, dr ) ) {
                        /* void, ..., function pointer, K&R names:  leave
                           the whole list alone */
                        if( k - start == 1
                            &&
                            ( toks[ start ].type == TK_KEYWORD
                              || isP( &( toks[ start ] ), '.' ) ) ) {
                            /* (void) */
                            }
                        else if( toks[ start ].type == TK_PUNCT
                                 && toks[ start ].c == '.' ) {
                            }
                        else {
                            return;
                            }
                        }
                    else {
                        dr->line = li;
                        dr->afterName = k < n ? toks[ k ].start : -1;
                        numOnLine ++;
                        }
                    }
                if( k == n
                    ||
                    ( li == closeLine && isP( &( toks[ k ] ), ')' )
                      && toks[ k ].start == closeCol ) ) {
                    break;
                    }
                start = k + 1;
                }
            else if( isP( &( toks[ k ] ), '(' ) ) {
                /* function pointer param:  leave alone */
                return;
                }
            k ++;
            }

        if( numOnLine == 1 && numRows < 64 ) {
            rows[ numRows ] = lineRows[ 0 ];
            numRows ++;
            }
        else if( numOnLine > 1 ) {
            /* several params on one line:  just fix each type/name gap,
               right to left */
            for( r = numOnLine - 1;
                 r >= 0;
                 r -- ) {
                alignRows( inTB, &( lineRows[ r ] ), 1,
                           config.paramTypeSpaces, -1,
                           ioCursorLine, ioCursorCol );
                }
            }
        }

    if( numRows == 1 ) {
        alignRows( inTB, rows, 1, config.paramTypeSpaces, -1,
                   ioCursorLine, ioCursorCol );
        }
    else if( numRows > 1 ) {
        /* table:  all rows must start at the same column */
        int  i;

        for( i = 1;
             i < numRows;
             i ++ ) {
            if( rows[ i ].typeStart != rows[ 0 ].typeStart ) {
                return;
                }
            }
        alignRows( inTB, rows, numRows, config.paramTypeSpaces, -1,
                   ioCursorLine, ioCursorCol );
        }
    }



/* ------------------------------------------------------------------
   Line formatting
   ------------------------------------------------------------------ */

int fmt_formatLine( TextBuffer  *inTB,
                    int          inLine,
                    int          inCursorCol ) {
    static EditList  edits;
    int              n;
    Token           *toks;
    Line            *l;
    int              k;
    int              parenStack[ 128 ];
    int              numParens   =  0;
    int              cursor      =  inCursorCol;
    int              paramOpenCol  =  -1;

    if( ! config.formatOnEnter ) {
        return inCursorCol;
        }
    if( inLine < 0 || inLine >= inTB->numLines ) {
        return inCursorCol;
        }

    n = lexCode( inTB, inLine, NULL );
    if( n <= 0 ) {
        return inCursorCol;
        }
    l = &( inTB->lines[ inLine ] );
    toks = lineToks.tokens;
    edits.num = 0;

    /* comments were dropped from toks;  gaps that contain a comment must
       not be touched, so check the raw text between tokens */
    for( k = 0;
         k < n;
         k ++ ) {
        Token  *t     =  &( toks[ k ] );
        Token  *next  =  k + 1 < n ? &( toks[ k + 1 ] ) : NULL;
        int     gap   =  0;
        int     clean =  1;

        if( next != NULL ) {
            int  j;

            gap = next->start - tokEnd( t );
            for( j = tokEnd( t );
                 j < next->start;
                 j ++ ) {
                if( l->text[ j ] != ' ' ) {
                    clean = 0;
                    }
                }
            }

        if( isP( t, '(' ) || isP( t, '[' ) ) {
            if( numParens < 128 ) {
                parenStack[ numParens ] = k;
                numParens ++;
                }
            }

        if( next == NULL || ! clean ) {
            if( isP( t, ')' ) || isP( t, ']' ) ) {
                if( numParens > 0 ) {
                    numParens --;
                    }
                }
            continue;
            }

        /* after an opener */
        if( isP( t, '(' ) && ! isP( next, ')' ) ) {
            int  close  =  -1;
            int  depth  =  0;
            int  j;

            for( j = k;
                 j < n;
                 j ++ ) {
                if( isP( &( toks[ j ] ), '(' ) ) {
                    depth ++;
                    }
                else if( isP( &( toks[ j ] ), ')' ) ) {
                    depth --;
                    if( depth == 0 ) {
                        close = j;
                        break;
                        }
                    }
                }
            if( close >= 0 && isCast( l->text, toks, k, close ) ) {
                if( ! config.castPadding && gap == 1 ) {
                    addEdit( &edits, tokEnd( t ), 1, 0 );
                    }
                else if( config.castPadding && gap == 0 ) {
                    addEdit( &edits, tokEnd( t ), 0, 1 );
                    }
                }
            else if( config.parenPadding && gap == 0 ) {
                addEdit( &edits, tokEnd( t ), 0, 1 );
                }
            }
        else if( isP( t, '[' ) && ! isP( next, ']' ) ) {
            if( config.bracketPadding && gap == 0 ) {
                addEdit( &edits, tokEnd( t ), 0, 1 );
                }
            }

        /* before a closer */
        if( isP( next, ')' ) && ! isP( t, '(' ) ) {
            int  open  =  numParens > 0 ? parenStack[ numParens - 1 ] : -1;

            if( open >= 0 && isP( &( toks[ open ] ), '(' )
                &&
                isCast( l->text, toks, open, k + 1 ) ) {
                if( ! config.castPadding && gap == 1 ) {
                    addEdit( &edits, tokEnd( t ), 1, 0 );
                    }
                else if( config.castPadding && gap == 0 ) {
                    addEdit( &edits, tokEnd( t ), 0, 1 );
                    }
                }
            else if( config.parenPadding && gap == 0 ) {
                addEdit( &edits, tokEnd( t ), 0, 1 );
                }
            }
        else if( isP( next, ']' ) && ! isP( t, '[' ) ) {
            if( config.bracketPadding && gap == 0 ) {
                addEdit( &edits, tokEnd( t ), 0, 1 );
                }
            }

        if( isP( t, ')' ) || isP( t, ']' ) ) {
            if( numParens > 0 ) {
                numParens --;
                }
            }

        /* after a comma */
        if( isP( t, ',' ) && config.commaSpace && gap == 0 ) {
            addEdit( &edits, tokEnd( t ), 0, 1 );
            }
        }

    cursor = applyEdits( inTB, inLine, &edits, cursor );

    /* function parameter list opened on this line? */
    if( config.paramTypeSpaces > 0 ) {
        LineContext  ctx;

        n = lexCode( inTB, inLine, NULL );
        toks = lineToks.tokens;
        ind_getLineContext( inTB, inLine, &ctx );

        if( ctx.braceDepth == 0 && ! ctx.inParen && ! ctx.inList ) {
            /* look for  type-words name (  at the start of the line */
            for( k = 1;
                 k < n;
                 k ++ ) {
                if( isP( &( toks[ k ] ), '(' ) ) {
                    if( toks[ k - 1 ].type == TK_IDENT && k >= 2 ) {
                        int  j;
                        int  ok  =  1;

                        for( j = 0;
                             j < k - 1;
                             j ++ ) {
                            Token  *t  =  &( toks[ j ] );

                            if( ! ( t->type == TK_IDENT
                                    || isP( t, '*' )
                                    || ( t->type == TK_KEYWORD
                                         && isDeclKeyword( t->kw ) ) ) ) {
                                ok = 0;
                                }
                            }
                        if( ok ) {
                            paramOpenCol = toks[ k ].start;
                            }
                        }
                    break;
                    }
                if( isP( &( toks[ k ] ), '=' ) ) {
                    break;
                    }
                }
            if( paramOpenCol >= 0 ) {
                int  cl  =  inLine;
                int  cc  =  cursor;

                formatParamList( inTB, inLine, paramOpenCol, &cl, &cc );
                cursor = cc;
                }
            }
        else if( ctx.inParamList ) {
            /* continuation line of a parameter list:  find its opener */
            int  li;

            for( li = inLine - 1;
                 li >= 0 && li > inLine - 64;
                 li -- ) {
                int  m  =  lexCode( inTB, li, NULL );
                int  depth  =  0;
                int  j;
                int  found  =  -1;

                if( m < 0 ) {
                    break;
                    }
                for( j = m - 1;
                     j >= 0;
                     j -- ) {
                    Token  *t  =  &( lineToks.tokens[ j ] );

                    if( isP( t, ')' ) ) {
                        depth ++;
                        }
                    else if( isP( t, '(' ) ) {
                        if( depth == 0 ) {
                            found = t->start;
                            break;
                            }
                        depth --;
                        }
                    }
                if( found >= 0 ) {
                    int  cl  =  inLine;
                    int  cc  =  cursor;

                    formatParamList( inTB, li, found, &cl, &cc );
                    cursor = cc;
                    break;
                    }
                }
            }
        }

    return cursor;
    }



void fmt_formatRegion( TextBuffer  *inTB,
                       int          inFirstLine,
                       int          inLastLine ) {
    int  i;

    if( inLastLine >= inTB->numLines ) {
        inLastLine = inTB->numLines - 1;
        }
    for( i = inFirstLine;
         i <= inLastLine;
         i ++ ) {
        fmt_formatLine( inTB, i, 0 );
        }
    for( i = inFirstLine;
         i <= inLastLine;
         i ++ ) {
        if( fmt_isDeclarationLine( inTB, i ) ) {
            fmt_alignDeclarations( inTB, i, NULL, NULL );
            /* skip the rest of this run */
            while( i + 1 <= inLastLine
                   &&
                   fmt_isDeclarationLine( inTB, i + 1 ) ) {
                i ++;
                }
            }
        }
    }
