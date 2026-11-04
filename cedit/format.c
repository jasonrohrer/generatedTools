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



/* =  +=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>= */
static int isAssignOp( const char  *inText,
                       Token       *inT ) {
    const char  *p  =  inText + inT->start;

    if( inT->type != TK_PUNCT ) {
        return 0;
        }
    if( inT->len == 1 ) {
        return p[ 0 ] == '=';
        }
    if( inT->len == 2 ) {
        return p[ 1 ] == '=' && strchr( "+-*/%&|^", p[ 0 ] ) != NULL;
        }
    if( inT->len == 3 ) {
        return ( p[ 0 ] == '<' || p[ 0 ] == '>' ) && p[ 1 ] == p[ 0 ]
               && p[ 2 ] == '=';
        }
    return 0;
    }



/* <  >  <=  >=  ==  != */
static int isCompareOp( const char  *inText,
                        Token       *inT ) {
    const char  *p  =  inText + inT->start;

    if( inT->type != TK_PUNCT ) {
        return 0;
        }
    if( inT->len == 1 ) {
        return p[ 0 ] == '<' || p[ 0 ] == '>';
        }
    if( inT->len == 2 ) {
        return p[ 1 ] == '=' && strchr( "<>=!", p[ 0 ] ) != NULL;
        }
    return 0;
    }



/* ++ or -- */
static int isIncDec( const char  *inText,
                     Token       *inT ) {
    const char  *p  =  inText + inT->start;

    return inT->type == TK_PUNCT && inT->len == 2
           && ( p[ 0 ] == '+' || p[ 0 ] == '-' ) && p[ 1 ] == p[ 0 ];
    }



/* &&  || */
static int isBoolOp( const char  *inText,
                     Token       *inT ) {
    const char  *p  =  inText + inT->start;

    return inT->type == TK_PUNCT && inT->len == 2
           && ( p[ 0 ] == '&' || p[ 0 ] == '|' ) && p[ 1 ] == p[ 0 ];
    }



/* ------------------------------------------------------------------
   Lexing lines
   ------------------------------------------------------------------ */

/* lineToks is the main scratch list;  auxToks is for helpers that look
   at other lines while lineToks is still in use */
static TokenList  lineToks;
static TokenList  auxToks;
static int        toksInit  =  0;


/* lexes a line into inList, dropping comments.  Returns number of
   significant tokens, or -1 if the line isn't plain code.  If
   outInnerComment is non-NULL, gets 1 if there was a comment anywhere but
   at the end. */
static int lexCodeInto( TextBuffer  *inTB,
                        int          inLine,
                        TokenList   *inList,
                        int         *outInnerComment ) {
    Line  *l           =  &( inTB->lines[ inLine ] );
    int    state;
    int    k;
    int    n           =  0;
    int    sawComment  =  0;

    if( ! toksInit ) {
        syn_initTokenList( &lineToks );
        syn_initTokenList( &auxToks );
        toksInit = 1;
        }
    if( outInnerComment != NULL ) {
        *outInnerComment = 0;
        }
    state = syn_getLineState( inTB, inLine );
    if( state != 0 ) {
        return -1;
        }
    syn_lexLine( l->text, l->len, state, inList );

    for( k = 0;
         k < inList->num;
         k ++ ) {
        Token  *t  =  &( inList->tokens[ k ] );

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
        inList->tokens[ n ] = *t;
        n ++;
        }
    inList->num = n;
    return n;
    }



static int lexCode( TextBuffer  *inTB,
                    int          inLine,
                    int         *outInnerComment ) {
    return lexCodeInto( inTB, inLine, &lineToks, outInnerComment );
    }



/* ------------------------------------------------------------------
   The cursor, while a public entry point runs

   Edits made with tb_insert / tb_delete carry a marker along, so the
   cursor rides in a temporary marker.  Space edits on one line map it
   more precisely by hand (applyEditsCur).
   ------------------------------------------------------------------ */

static int  cursorMarker  =  -1;
/* the line where Tab was pressed on an unfinished declaration
   ("int  " -> show where the name will go), or -1 */
static int  partialLine   =  -1;
/* nesting depth of beginCursor */
static int  cursorDepth   =  0;



static void beginCursor( TextBuffer  *inTB,
                         int          inFlags,
                         int         *ioCursorLine,
                         int         *ioCursorCol ) {
    cursorDepth ++;
    if( cursorDepth > 1 ) {
        return;
        }
    cursorMarker = -1;
    partialLine = -1;
    if( ioCursorLine != NULL && ioCursorCol != NULL && *ioCursorLine >= 0
        && *ioCursorLine < inTB->numLines ) {
        cursorMarker = tb_addMarker( inTB, *ioCursorLine, *ioCursorCol );
        if( inFlags & FMT_PARTIAL ) {
            partialLine = *ioCursorLine;
            }
        }
    }



static void endCursor( TextBuffer  *inTB,
                       int         *ioCursorLine,
                       int         *ioCursorCol ) {
    cursorDepth --;
    if( cursorDepth > 0 ) {
        return;
        }
    if( cursorMarker >= 0 ) {
        tb_getMarker( inTB, cursorMarker, ioCursorLine, ioCursorCol );
        tb_removeMarker( inTB, cursorMarker );
        }
    cursorMarker = -1;
    partialLine = -1;
    }



static void getCursor( TextBuffer  *inTB,
                       int         *outLine,
                       int         *outCol ) {
    if( cursorMarker >= 0 ) {
        tb_getMarker( inTB, cursorMarker, outLine, outCol );
        }
    else {
        *outLine = -1;
        *outCol = 0;
        }
    }



static void setCursor( TextBuffer  *inTB,
                       int          inLine,
                       int          inCol ) {
    if( cursorMarker >= 0 && inLine >= 0 ) {
        tb_setMarker( inTB, cursorMarker, inLine, inCol );
        }
    }



static void applyEditsCur( TextBuffer  *inTB,
                           int          inLine,
                           EditList    *inList ) {
    int  l;
    int  c;

    getCursor( inTB, &l, &c );
    c = applyEdits( inTB, inLine, inList, l == inLine ? c : 0 );
    if( l == inLine ) {
        setCursor( inTB, l, c );
        }
    }



/* is the cursor on inLine, with nothing but spaces after it? */
static int cursorAtLineEnd( TextBuffer  *inTB,
                            int          inLine ) {
    Line  *l  =  &( inTB->lines[ inLine ] );
    int    cl;
    int    cc;
    int    i;

    getCursor( inTB, &cl, &cc );
    if( cl != inLine ) {
        return 0;
        }
    for( i = cc;
         i < l->len;
         i ++ ) {
        if( l->text[ i ] != ' ' ) {
            return 0;
            }
        }
    return 1;
    }



/* re-indents a line that the formatter created or moved, keeping the
   cursor out of the indentation */
static void reindentNewLine( TextBuffer  *inTB,
                             int          inLine ) {
    int  cl;
    int  cc;

    ind_indentLine( inTB, inLine );
    getCursor( inTB, &cl, &cc );
    if( cl == inLine && cc < tb_lineIndent( inTB, inLine ) ) {
        setCursor( inTB, cl, tb_lineIndent( inTB, inLine ) );
        }
    }



/* ------------------------------------------------------------------
   Declaration rows (a type, a name, maybe an initializer)
   ------------------------------------------------------------------ */

typedef struct DeclRow {
        int  line;
        /* last line of the declaration (an initializer like a function
           call can continue onto following lines), or -1 */
        int  lastLine;
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
        /* 1 if this is an unfinished row with no name yet (Tab) */
        int  partial;
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
    outRow->lastLine = -1;
    outRow->partial = 0;
    return 1;
    }



/* parses toks[ inFrom .. inTo ) as an unfinished declaration:  type
   words and maybe stars, but no name yet ("unsigned int", "Deck *").
   The name goes at the end of the line (inLineLen). */
static int parsePartialSpan( const char  *inText,
                             int          inLineLen,
                             Token       *inToks,
                             int          inFrom,
                             int          inTo,
                             DeclRow     *outRow ) {
    int     k      =  inTo - 1;
    int     stars  =  0;
    int     i;
    Token  *first;

    while( k >= inFrom && isP( &( inToks[ k ] ), '*' ) ) {
        stars ++;
        k --;
        }
    if( k < inFrom ) {
        return 0;
        }
    for( i = inFrom;
         i <= k;
         i ++ ) {
        Token  *t  =  &( inToks[ i ] );

        if( t->type != TK_IDENT
            && ! ( t->type == TK_KEYWORD && isDeclKeyword( t->kw ) ) ) {
            return 0;
            }
        }
    /* it must start like a type:  a type keyword, a known type name, or
       a Capitalized name (not a variable about to be assigned) */
    first = &( inToks[ inFrom ] );
    if( first->type == TK_IDENT
        && ! syn_isTypeName( inText + first->start, first->len )
        && ! ( inText[ first->start ] >= 'A'
               && inText[ first->start ] <= 'Z' ) ) {
        return 0;
        }
    /* "Deck" right at the cursor might be a name being typed;  "Deck "
       or "Deck *" or "unsigned" is clearly a type */
    if( stars == 0 && inToks[ k ].type == TK_IDENT
        && tokEnd( &( inToks[ k ] ) ) == inLineLen ) {
        return 0;
        }
    outRow->typeStart = first->start;
    outRow->typeEnd = tokEnd( &( inToks[ k ] ) );
    outRow->numStars = stars;
    outRow->nameStart = inLineLen;
    outRow->nameEnd = inLineLen;
    outRow->eqStart = -1;
    outRow->valueStart = -1;
    outRow->afterName = -1;
    outRow->lastLine = -1;
    outRow->partial = 1;
    return 1;
    }



/* parses a line as a single declaration.  The initializer may be a call
   whose arguments continue on the following lines;  outRow->lastLine
   says where the declaration ends. */
static int parseDeclLine( TextBuffer  *inTB,
                          int          inLine,
                          DeclRow     *outRow ) {
    Line   *l        =  &( inTB->lines[ inLine ] );
    int     inner;
    int     n;
    Token  *toks     =  lineToks.tokens;
    int     k;
    int     depth    =  0;
    int     stop     =  -1;
    int     endsLine;
    int     li;

    if( inLine == partialLine && cursorAtLineEnd( inTB, inLine ) ) {
        n = lexCode( inTB, inLine, &inner );
        toks = lineToks.tokens;
        if( n >= 1 && ! inner
            &&
            parsePartialSpan( l->text, l->len, toks, 0, n, outRow ) ) {
            outRow->line = inLine;
            outRow->lastLine = inLine;
            return 1;
            }
        }

    n = lexCode( inTB, inLine, &inner );
    toks = lineToks.tokens;
    if( n < 3 || inner ) {
        return 0;
        }
    endsLine = isP( &( toks[ n - 1 ] ), ';' );

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
    outRow->lastLine = inLine;
    outRow->afterName = toks[ stop ].start;

    if( ! isP( &( toks[ stop ] ), '=' ) ) {
        return stop == n - 1;
        }

    /* initializer must be simple:  no top level comma, no braces */
    depth = 0;
    for( k = stop + 1;
         k < n;
         k ++ ) {
        Token  *t  =  &( toks[ k ] );

        if( isP( t, '(' ) || isP( t, '[' ) ) {
            depth ++;
            }
        else if( isP( t, ')' ) || isP( t, ']' ) ) {
            depth --;
            }
        else if( isP( t, '{' ) || isP( t, '}' ) ) {
            return 0;
            }
        else if( depth == 0 && isP( t, ',' ) ) {
            return 0;
            }
        }
    if( stop + 1 >= n - ( endsLine ? 1 : 0 ) ) {
        return 0;
        }
    outRow->eqStart = toks[ stop ].start;
    outRow->valueStart = toks[ stop + 1 ].start;
    if( endsLine ) {
        return depth == 0;
        }
    if( depth <= 0 ) {
        /* continued some other way ( a +\n b ):  not a table row */
        return 0;
        }

    /* the call's arguments continue:  find the line that closes it and
       ends the statement */
    for( li = inLine + 1;
         li < inTB->numLines && li <= inLine + 40;
         li ++ ) {
        int  m  =  lexCode( inTB, li, NULL );

        if( m <= 0 ) {
            return 0;
            }
        for( k = 0;
             k < m;
             k ++ ) {
            Token  *t  =  &( lineToks.tokens[ k ] );

            if( isP( t, '(' ) || isP( t, '[' ) ) {
                depth ++;
                }
            else if( isP( t, ')' ) || isP( t, ']' ) ) {
                depth --;
                if( depth < 0 ) {
                    return 0;
                    }
                }
            else if( isP( t, '{' ) || isP( t, '}' ) ) {
                return 0;
                }
            else if( isP( t, ';' ) ) {
                if( depth != 0 || k != m - 1 ) {
                    return 0;
                    }
                outRow->lastLine = li;
                return 1;
                }
            }
        }
    return 0;
    }



int fmt_isDeclarationLine( TextBuffer  *inTB,
                           int          inLine ) {
    DeclRow  row;

    return parseDeclLine( inTB, inLine, &row );
    }



/* lines up rows (each starting its own line;  all rows of a table share
   the same typeStart).  inEqSpaces < 0 means these are parameters (no
   initializers and no ';' to tighten). */
static int alignRows( TextBuffer  *inTB,
                      DeclRow     *inRows,
                      int          inNumRows,
                      int          inTypeGap,
                      int          inEqSpaces ) {
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
            applyEditsCur( inTB, r->line, &edits );
            changed = 1;

            /* a call's arguments continuing below move with it */
            for( j = r->line + 1;
                 j <= r->lastLine;
                 j ++ ) {
                reindentNewLine( inTB, j );
                }
            }
        if( r->partial ) {
            /* the cursor goes where the name will be typed */
            int  cl;
            int  cc;

            getCursor( inTB, &cl, &cc );
            if( cl == r->line ) {
                setCursor( inTB, cl, inTB->lines[ r->line ].len );
                }
            }
        }
    return changed;
    }



/* if inEnd is the last line of a declaration starting at column inIndent,
   returns the declaration's first line, else -1 */
static int declEndingAt( TextBuffer  *inTB,
                         int          inEnd,
                         int          inIndent ) {
    DeclRow  row;
    int      s  =  inEnd;

    if( inEnd < 0 ) {
        return -1;
        }
    /* walk up over continuation lines (indented more than the start) */
    while( s > 0 && inEnd - s < 40 && ! tb_lineIsBlank( inTB, s )
           && tb_lineIndent( inTB, s ) > inIndent ) {
        s --;
        }
    if( tb_lineIndent( inTB, s ) == inIndent
        &&
        parseDeclLine( inTB, s, &row )
        &&
        row.typeStart == inIndent && row.lastLine == inEnd ) {
        return s;
        }
    return -1;
    }



static int alignDeclarationsAt( TextBuffer  *inTB,
                                int          inLine ) {
    static DeclRow  *rows     =  NULL;
    static int       rowCap   =  0;
    int              first    =  inLine;
    int              last;
    int              indent;
    int              i;
    int              n        =  0;
    DeclRow          row;

    if( ! config.alignDeclarations ) {
        return 0;
        }
    if( ! parseDeclLine( inTB, inLine, &row ) ) {
        /* maybe a continuation line of a multi-line declaration:  its
           first line is the nearest one above that is indented less */
        int  ind  =  tb_lineIndent( inTB, inLine );
        int  s    =  inLine - 1;

        while( s >= 0 && inLine - s < 40 && ! tb_lineIsBlank( inTB, s )
               && tb_lineIndent( inTB, s ) >= ind ) {
            s --;
            }
        if( s < 0 || tb_lineIsBlank( inTB, s )
            || ! parseDeclLine( inTB, s, &row ) || row.lastLine < inLine ) {
            return 0;
            }
        first = s;
        }
    indent = row.typeStart;
    last = row.lastLine;

    /* a table can span a few blank lines (declTableBlankLines), the way
       declarations at the top of a block are often grouped */
    while( 1 ) {
        int  e  =  first - 1;
        int  s;

        while( e >= 0 && first - 1 - e < config.declTableBlankLines
               && tb_lineIsBlank( inTB, e ) ) {
            e --;
            }
        s = declEndingAt( inTB, e, indent );
        if( s < 0 ) {
            break;
            }
        first = s;
        }
    while( 1 ) {
        int  b  =  last + 1;

        while( b < inTB->numLines
               && b - ( last + 1 ) < config.declTableBlankLines
               && tb_lineIsBlank( inTB, b ) ) {
            b ++;
            }
        if( b >= inTB->numLines
            || ! parseDeclLine( inTB, b, &row ) || row.typeStart != indent ) {
            break;
            }
        last = row.lastLine;
        }

    if( rowCap < last - first + 1 ) {
        rowCap = last - first + 1 + 16;
        rows = (DeclRow *)realloc( rows, (size_t)rowCap * sizeof( DeclRow ) );
        }
    i = first;
    while( i <= last ) {
        if( parseDeclLine( inTB, i, &( rows[ n ] ) ) ) {
            i = rows[ n ].lastLine + 1;
            n ++;
            }
        else {
            i ++;
            }
        }
    if( n == 0 ) {
        return 0;
        }
    return alignRows( inTB, rows, n, config.declTypeSpaces,
                      config.declEqualsSpaces );
    }



int fmt_alignDeclarations( TextBuffer  *inTB,
                           int          inLine,
                           int          inFlags,
                           int         *ioCursorLine,
                           int         *ioCursorCol ) {
    int  result;

    if( inLine < 0 || inLine >= inTB->numLines ) {
        return 0;
        }
    beginCursor( inTB, inFlags, ioCursorLine, ioCursorCol );
    result = alignDeclarationsAt( inTB, inLine );
    endCursor( inTB, ioCursorLine, ioCursorCol );
    return result;
    }



/* ------------------------------------------------------------------
   Paren groups:  what kind of list is a '(' opening?
   ------------------------------------------------------------------ */

#define G_NONE   0
#define G_OTHER  1
/* function call arguments or function parameters */
#define G_CALL   2
/* for( ;; ) */
#define G_FOR    3
/* [ ] or { } */
#define G_BRACE  4


/* kind of the paren group whose '(' follows the token inPrev (NULL if
   nothing comes before it) */
static int groupKind( Token  *inPrev ) {
    if( inPrev == NULL ) {
        return G_OTHER;
        }
    if( inPrev->type == TK_KEYWORD && inPrev->kw == KW_FOR ) {
        return G_FOR;
        }
    if( inPrev->type == TK_IDENT ) {
        return G_CALL;
        }
    return G_OTHER;
    }



/* kind of a '(' that is the first token on inLine:  looks at the last
   code token of the lines above */
static int groupKindAtLineStart( TextBuffer  *inTB,
                                 int          inLine ) {
    int  li;

    for( li = inLine - 1;
         li >= 0 && li >= inLine - 8;
         li -- ) {
        int  n  =  lexCodeInto( inTB, li, &auxToks, NULL );

        if( n > 0 ) {
            return groupKind( &( auxToks.tokens[ n - 1 ] ) );
            }
        }
    return G_OTHER;
    }



/* finds the innermost '(' that is still open at the start of inLine.
   Returns 0 if there is none (or the innermost open bracket is a [ or
   a {). */
static int findOpenParen( TextBuffer  *inTB,
                          int          inLine,
                          int         *outLine,
                          int         *outCol,
                          int         *outKind ) {
    int  li;
    int  depth  =  0;

    for( li = inLine - 1;
         li >= 0 && li > inLine - 64;
         li -- ) {
        int  n  =  lexCodeInto( inTB, li, &auxToks, NULL );
        int  j;

        for( j = n - 1;
             j >= 0;
             j -- ) {
            Token  *t  =  &( auxToks.tokens[ j ] );

            if( isP( t, ')' ) || isP( t, ']' ) || isP( t, '}' ) ) {
                depth ++;
                }
            else if( isP( t, '(' ) || isP( t, '[' ) || isP( t, '{' ) ) {
                if( depth > 0 ) {
                    depth --;
                    continue;
                    }
                if( ! isP( t, '(' ) ) {
                    return 0;
                    }
                *outLine = li;
                *outCol = t->start;
                if( j > 0 ) {
                    *outKind = groupKind( &( auxToks.tokens[ j - 1 ] ) );
                    }
                else {
                    *outKind = groupKindAtLineStart( inTB, li );
                    }
                return 1;
                }
            }
        }
    return 0;
    }



/* does the for( group opening at toks[ inOpen ] have an empty clause
   (as far as this line shows)? */
static int forHasEmptyClause( Token  *inToks,
                              int     inNum,
                              int     inOpen ) {
    int  k;
    int  depth  =  0;

    for( k = inOpen;
         k < inNum;
         k ++ ) {
        Token  *t  =  &( inToks[ k ] );

        if( isP( t, '(' ) ) {
            depth ++;
            }
        else if( isP( t, ')' ) ) {
            depth --;
            if( depth == 0 ) {
                return isP( &( inToks[ k - 1 ] ), ';' );
                }
            }
        else if( depth == 1 && isP( t, ';' ) ) {
            Token  *p  =  &( inToks[ k - 1 ] );

            if( isP( p, ';' ) || ( k - 1 == inOpen ) ) {
                return 1;
                }
            }
        }
    return 0;
    }



/* ------------------------------------------------------------------
   Splitting argument lists and for clauses onto their own lines
   ------------------------------------------------------------------ */

/* breaks the line after every comma of a call / parameter list and
   after the ;s of a for( ), when more follows on the line.  The new
   lines are re-indented (which lines them up after the '(').
   Returns the number of lines added. */
static int splitLine( TextBuffer   *inTB,
                      int           inLine,
                      LineContext  *inCtx ) {
    int          kinds[ 128 ];
    int          splits[ 128 ];
    int          numKinds   =  0;
    int          numSplits  =  0;
    int          base       =  G_NONE;
    int          inner;
    int          isReturn;
    int          n;
    int          k;
    int          i;
    Token       *toks;
    const char  *text;

    if( ! config.splitArgLists && ! config.splitForClauses
        && ! config.splitBoolOps ) {
        return 0;
        }
    if( inCtx->inComment || inCtx->inPreproc ) {
        return 0;
        }
    if( inCtx->inParen ) {
        int  ol;
        int  oc;

        if( ! findOpenParen( inTB, inLine, &ol, &oc, &base ) ) {
            base = G_OTHER;
            }
        }

    n = lexCode( inTB, inLine, &inner );
    if( n <= 1 || inner ) {
        return 0;
        }
    toks = lineToks.tokens;
    text = inTB->lines[ inLine ].text;
    isReturn = ( toks[ 0 ].type == TK_KEYWORD && toks[ 0 ].kw == KW_RETURN );

    for( k = 0;
         k < n;
         k ++ ) {
        Token  *t    =  &( toks[ k ] );
        int     top  =  numKinds > 0 ? kinds[ numKinds - 1 ] : base;

        if( isP( t, '(' ) || isP( t, '[' ) || isP( t, '{' ) ) {
            int  kind  =  G_BRACE;

            if( isP( t, '(' ) ) {
                if( k > 0 ) {
                    kind = groupKind( &( toks[ k - 1 ] ) );
                    }
                else {
                    kind = groupKindAtLineStart( inTB, inLine );
                    }
                if( kind == G_FOR && forHasEmptyClause( toks, n, k ) ) {
                    kind = G_OTHER;
                    }
                }
            if( numKinds < 128 ) {
                kinds[ numKinds ] = kind;
                numKinds ++;
                }
            }
        else if( isP( t, ')' ) || isP( t, ']' ) || isP( t, '}' ) ) {
            if( numKinds > 0 ) {
                numKinds --;
                }
            else {
                base = G_NONE;
                }
            }
        else if( isBoolOp( text, t ) && config.splitBoolOps
                 && numSplits < 126
                 &&
                 ( top == G_OTHER || top == G_CALL
                   || ( top == G_NONE && isReturn ) ) ) {
            /* the operator goes on a line by itself */
            if( k > 0
                && ( numSplits == 0 || splits[ numSplits - 1 ] != k - 1 ) ) {
                splits[ numSplits ] = k - 1;
                numSplits ++;
                }
            if( k + 1 < n ) {
                splits[ numSplits ] = k;
                numSplits ++;
                }
            }
        else if( k + 1 < n && numSplits < 128 ) {
            Token  *next  =  &( toks[ k + 1 ] );

            if( isP( t, ',' ) && top == G_CALL && config.splitArgLists
                && ! isP( next, ')' ) ) {
                splits[ numSplits ] = k;
                numSplits ++;
                }
            else if( isP( t, ';' ) && top == G_FOR && config.splitForClauses
                     && ! isP( next, ';' ) && ! isP( next, ')' ) ) {
                splits[ numSplits ] = k;
                numSplits ++;
                }
            }
        }

    /* right to left, so earlier columns stay valid */
    for( i = numSplits - 1;
         i >= 0;
         i -- ) {
        int  e   =  tokEnd( &( toks[ splits[ i ] ] ) );
        int  ns  =  toks[ splits[ i ] + 1 ].start;

        if( ns > e ) {
            tb_delete( inTB, inLine, e, inLine, ns );
            }
        tb_insert( inTB, inLine, e, "\n", 1, NULL, NULL );
        }
    for( i = 1;
         i <= numSplits;
         i ++ ) {
        reindentNewLine( inTB, inLine + i );
        }
    return numSplits;
    }



/* ------------------------------------------------------------------
   for( ;; ) clauses as a table

       for( y =  BH - 1;
            y >= BH - 3;
            y    -- ) {

   Left sides line up, operators line up, and right sides (and the
   ++ / -- of the last clause) start in one column.
   ------------------------------------------------------------------ */

typedef struct ForRow {
        int  lhsStart;
        int  lhsEnd;
        int  opStart;
        int  opEnd;
        /* -1 for a postfix ++ / -- row */
        int  rhsStart;
    } ForRow;



static int parseForClause( const char  *inText,
                           Token       *inToks,
                           int          inFrom,
                           int          inTo,
                           ForRow      *outRow ) {
    int  k;
    int  depth  =  0;

    if( inTo - inFrom < 2 ) {
        return 0;
        }
    for( k = inFrom;
         k < inTo;
         k ++ ) {
        Token  *t  =  &( inToks[ k ] );

        if( isP( t, '(' ) || isP( t, '[' ) ) {
            depth ++;
            }
        else if( isP( t, ')' ) || isP( t, ']' ) ) {
            depth --;
            }
        else if( depth == 0 && k > inFrom
                 &&
                 ( isAssignOp( inText, t ) || isCompareOp( inText, t ) ) ) {
            if( k + 1 >= inTo ) {
                return 0;
                }
            outRow->lhsStart = inToks[ inFrom ].start;
            outRow->lhsEnd = tokEnd( &( inToks[ k - 1 ] ) );
            outRow->opStart = t->start;
            outRow->opEnd = tokEnd( t );
            outRow->rhsStart = inToks[ k + 1 ].start;
            return 1;
            }
        }
    if( isIncDec( inText, &( inToks[ inTo - 1 ] ) ) ) {
        outRow->lhsStart = inToks[ inFrom ].start;
        outRow->lhsEnd = tokEnd( &( inToks[ inTo - 2 ] ) );
        outRow->opStart = inToks[ inTo - 1 ].start;
        outRow->opEnd = tokEnd( &( inToks[ inTo - 1 ] ) );
        outRow->rhsStart = -1;
        return 1;
        }
    return 0;
    }



static void formatForTable( TextBuffer  *inTB,
                            int          inLine,
                            int          inOpenCol ) {
    static EditList  edits;
    ForRow           rows[ 3 ];
    int              closeLine;
    int              closeCol;
    int              r;
    int              lhsW     =  0;
    int              opW      =  0;
    int              opCol;
    int              valCol;

    if( ! config.alignForClauses ) {
        return;
        }
    if( ! ind_findMatchingBracket( inTB, inLine, inOpenCol, 8, &closeLine,
                                   &closeCol )
        || closeLine != inLine + 2 ) {
        return;
        }
    for( r = 0;
         r < 3;
         r ++ ) {
        int     li     =  inLine + r;
        int     inner;
        int     n      =  lexCode( inTB, li, &inner );
        Token  *toks   =  lineToks.tokens;
        int     from   =  0;
        int     to     =  -1;
        int     k;

        if( n <= 0 || inner ) {
            return;
            }
        if( r == 0 ) {
            while( from < n && ! ( isP( &( toks[ from ] ), '(' )
                                   && toks[ from ].start == inOpenCol ) ) {
                from ++;
                }
            from ++;
            }
        if( r < 2 ) {
            if( ! isP( &( toks[ n - 1 ] ), ';' ) ) {
                return;
                }
            to = n - 1;
            }
        else {
            for( k = 0;
                 k < n;
                 k ++ ) {
                if( isP( &( toks[ k ] ), ')' )
                    && toks[ k ].start == closeCol ) {
                    to = k;
                    break;
                    }
                }
            }
        if( to < 0 || ! parseForClause( inTB->lines[ li ].text, toks, from,
                                        to, &( rows[ r ] ) ) ) {
            return;
            }
        if( rows[ r ].lhsStart != rows[ 0 ].lhsStart ) {
            return;
            }
        if( rows[ r ].lhsEnd - rows[ r ].lhsStart > lhsW ) {
            lhsW = rows[ r ].lhsEnd - rows[ r ].lhsStart;
            }
        if( rows[ r ].rhsStart >= 0
            && rows[ r ].opEnd - rows[ r ].opStart > opW ) {
            opW = rows[ r ].opEnd - rows[ r ].opStart;
            }
        }
    if( opW == 0 ) {
        return;
        }
    opCol = rows[ 0 ].lhsStart + lhsW + 1;
    valCol = opCol + opW + 1;

    for( r = 0;
         r < 3;
         r ++ ) {
        ForRow  *f  =  &( rows[ r ] );

        edits.num = 0;
        if( f->rhsStart < 0 ) {
            addEdit( &edits, f->lhsEnd, f->opStart - f->lhsEnd,
                     valCol - f->lhsEnd );
            }
        else {
            addEdit( &edits, f->opEnd, f->rhsStart - f->opEnd,
                     valCol - ( opCol + f->opEnd - f->opStart ) );
            addEdit( &edits, f->lhsEnd, f->opStart - f->lhsEnd,
                     opCol - f->lhsEnd );
            }
        applyEditsCur( inTB, inLine + r, &edits );
        }
    }



/* ------------------------------------------------------------------
   Function parameter lists
   ------------------------------------------------------------------ */

/* aligns the parameter list whose '(' is at inOpenCol on inLine.
   Handles both one-line lists and one-param-per-line tables. */
static void formatParamList( TextBuffer  *inTB,
                             int          inLine,
                             int          inOpenCol ) {
    int       closeLine;
    int       closeCol;
    int       lastLine;
    DeclRow   rows[ 64 ];
    int       numRows  =  0;
    int       li;

    if( config.paramTypeSpaces <= 0 ) {
        return;
        }
    if( ind_findMatchingBracket( inTB, inLine, inOpenCol, 64,
                                 &closeLine, &closeCol ) ) {
        lastLine = closeLine;
        }
    else {
        /* list not closed yet:  format through the current line, or
           through the unfinished parameter Tab was pressed on */
        closeLine = -1;
        lastLine = inLine;
        if( partialLine > inLine && partialLine < inLine + 64 ) {
            lastLine = partialLine;
            }
        }

    for( li = inLine;
         li <= lastLine;
         li ++ ) {
        int      n          =  lexCode( inTB, li, NULL );
        Token   *toks       =  lineToks.tokens;
        int      k          =  0;
        int      start;
        int      numOnLine  =  0;
        DeclRow  lineRows[ 32 ];
        int      r;

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
            int  atClose  =  ( k < n && li == closeLine
                               && isP( &( toks[ k ] ), ')' )
                               && toks[ k ].start == closeCol );

            if( k == n || isP( &( toks[ k ] ), ',' ) || atClose ) {
                if( k > start ) {
                    DeclRow  *dr  =  &( lineRows[ numOnLine ] );

                    if( numOnLine >= 32 ) {
                        return;
                        }
                    if( k == n && li != closeLine ) {
                        /* the line ends mid-parameter:  only OK for the
                           unfinished one Tab was pressed on */
                        if( li == partialLine
                            && cursorAtLineEnd( inTB, li )
                            &&
                            parsePartialSpan( inTB->lines[ li ].text,
                                              inTB->lines[ li ].len, toks,
                                              start, k, dr ) ) {
                            dr->line = li;
                            dr->lastLine = li;
                            numOnLine ++;
                            }
                        else {
                            return;
                            }
                        }
                    else if( ! parseDeclSpan( inTB->lines[ li ].text, toks,
                                              start, k, dr ) ) {
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
                        dr->lastLine = li;
                        dr->afterName = k < n ? toks[ k ].start : -1;
                        numOnLine ++;
                        }
                    }
                if( k == n || atClose ) {
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
                           config.paramTypeSpaces, -1 );
                }
            }
        }

    if( numRows == 1 ) {
        alignRows( inTB, rows, 1, config.paramTypeSpaces, -1 );
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
        alignRows( inTB, rows, numRows, config.paramTypeSpaces, -1 );
        }
    }



/* if inLine starts a function definition / prototype at top level
   ( type-words name( ), returns the column of its '(', else -1 */
static int paramListOpenCol( TextBuffer  *inTB,
                             int          inLine ) {
    int     n     =  lexCode( inTB, inLine, NULL );
    Token  *toks  =  lineToks.tokens;
    int     k;

    for( k = 1;
         k < n;
         k ++ ) {
        if( isP( &( toks[ k ] ), '(' ) ) {
            int  j;

            if( toks[ k - 1 ].type != TK_IDENT || k < 2 ) {
                return -1;
                }
            for( j = 0;
                 j < k - 1;
                 j ++ ) {
                Token  *t  =  &( toks[ j ] );

                if( ! ( t->type == TK_IDENT
                        || isP( t, '*' )
                        || ( t->type == TK_KEYWORD
                             && isDeclKeyword( t->kw ) ) ) ) {
                    return -1;
                    }
                }
            return toks[ k ].start;
            }
        if( isP( &( toks[ k ] ), '=' ) ) {
            return -1;
            }
        }
    return -1;
    }



/* ------------------------------------------------------------------
   Line formatting
   ------------------------------------------------------------------ */

/* how an operator token should be spaced */
enum OpKind {
    OP_NONE = 0,
    /* spaces on both sides:  a + b */
    OP_BINARY,
    /* a space after:  ! x   ~ x   - x */
    OP_UNARY_SPACE,
    /* hugs its operand:  *p   &x   -1   Deck  *d */
    OP_UNARY_HUG,
    /* can't tell (a line-leading operator in a continued expression):
       leave its spacing alone */
    OP_UNSURE
    };


#define MAX_OP_TOKS  512


/* is the token a value (so that an operator after it is binary)?
   inMatch[ k ] is the index of the token matching a bracket at k, or
   -1 */
static int isOperand( const char  *inText,
                      Token       *inToks,
                      int         *inMatch,
                      int          inK ) {
    Token  *t  =  &( inToks[ inK ] );

    if( t->type == TK_IDENT || t->type == TK_NUMBER
        || t->type == TK_STRING || t->type == TK_CHAR ) {
        return 1;
        }
    if( isP( t, ']' ) ) {
        return 1;
        }
    if( isP( t, ')' ) ) {
        /* but the ) of a cast comes before an operand:  (int)-x */
        int  open  =  inMatch[ inK ];

        if( open >= 0 && isCast( inText, inToks, open, inK ) ) {
            return 0;
            }
        return 1;
        }
    if( isIncDec( inText, t ) ) {
        /* postfix if it follows a value:  x++ - 1 */
        return inK > 0 && isOperand( inText, inToks, inMatch, inK - 1 );
        }
    return 0;
    }



/* a Capitalized name that isn't ALL_CAPS (Deck, MaxiginGUI) looks like
   a type */
static int looksLikeTypeName( const char  *inText,
                              Token       *inT ) {
    const char  *p  =  inText + inT->start;
    int          i;

    if( syn_isTypeName( p, inT->len ) ) {
        return 1;
        }
    if( p[ 0 ] < 'A' || p[ 0 ] > 'Z' ) {
        return 0;
        }
    for( i = 1;
         i < inT->len;
         i ++ ) {
        if( p[ i ] >= 'a' && p[ i ] <= 'z' ) {
            return 1;
            }
        }
    return 0;
    }



/* a '*' at inK right after a name:  is it the star of a declaration
   (Deck *d,  foo *x;) rather than a multiplication? */
static int isDeclStar( const char   *inText,
                       Token        *inToks,
                       int           inNum,
                       int           inK,
                       LineContext  *inCtx ) {
    Token  *name  =  &( inToks[ inK - 1 ] );
    Token  *pp    =  inK >= 2 ? &( inToks[ inK - 2 ] ) : NULL;
    int     j     =  inK;
    int     stmtStart;

    /* the name after the stars */
    while( j < inNum && isP( &( inToks[ j ] ), '*' ) ) {
        j ++;
        }
    if( j >= inNum ) {
        /* "Deck *" at the end of the line:  an unfinished declaration,
           or a multiplication continued on the next line */
        return looksLikeTypeName( inText, name );
        }
    if( isP( &( inToks[ j ] ), ')' ) ) {
        /* (foo *) is a cast;  a * ) can't be a multiplication */
        return 1;
        }
    if( inToks[ j ].type != TK_IDENT ) {
        return 0;
        }
    if( looksLikeTypeName( inText, name ) ) {
        return 1;
        }
    if( pp != NULL && pp->type == TK_KEYWORD ) {
        /* struct foo *f,  const foo *f,  but not return a * b */
        return isDeclKeyword( pp->kw );
        }
    if( pp == NULL ) {
        if( inCtx->inParen ) {
            /* continued parameter list, or continued call arguments */
            return inCtx->braceDepth == 0;
            }
        stmtStart = ! inCtx->midStatement;
        }
    else if( isP( pp, '(' ) || isP( pp, ',' ) ) {
        /* parameters (outside functions) vs arguments (inside) */
        return inCtx->braceDepth == 0 && ! inCtx->inList;
        }
    else {
        stmtStart = isP( pp, ';' ) || isP( pp, '{' ) || isP( pp, '}' );
        }
    if( ! stmtStart ) {
        return 0;
        }
    if( inCtx->braceDepth == 0 ) {
        return 1;
        }
    /* foo *x;  foo *x = ...  foo *x[ 3 ];  (a * b; does nothing) */
    if( j + 1 >= inNum ) {
        return 1;
        }
    return isP( &( inToks[ j + 1 ] ), ';' ) || isP( &( inToks[ j + 1 ] ), '=' )
           || isP( &( inToks[ j + 1 ] ), ',' )
           || isP( &( inToks[ j + 1 ] ), '[' );
    }



/* classifies every token of a line (outKinds[ k ]).  inPrevValue is 1
   if the line continues a statement whose previous line ended with a
   value, 0 if it ended with something else, -1 if unknown. */
static void classifyOperators( const char   *inText,
                               Token        *inToks,
                               int           inNum,
                               LineContext  *inCtx,
                               int           inPrevValue,
                               int          *outKinds ) {
    static int  match[ MAX_OP_TOKS ];
    int         stack[ 128 ];
    int         numStack     =  0;
    int         openTernary  =  0;
    int         k;

    for( k = 0;
         k < inNum;
         k ++ ) {
        Token  *t  =  &( inToks[ k ] );

        match[ k ] = -1;
        outKinds[ k ] = OP_NONE;
        if( isP( t, '(' ) || isP( t, '[' ) ) {
            if( numStack < 128 ) {
                stack[ numStack ++ ] = k;
                }
            }
        else if( ( isP( t, ')' ) || isP( t, ']' ) ) && numStack > 0 ) {
            numStack --;
            match[ k ] = stack[ numStack ];
            match[ stack[ numStack ] ] = k;
            }
        }

    for( k = 0;
         k < inNum;
         k ++ ) {
        Token  *t        =  &( inToks[ k ] );
        int     afterValue;
        int     nextIsNum;

        if( t->type != TK_PUNCT || t->len > 2 ) {
            continue;
            }
        if( t->len == 2 ) {
            /* << >> */
            if( ( t->c == '<' || t->c == '>' )
                && inText[ t->start + 1 ] == t->c ) {
                outKinds[ k ] = OP_BINARY;
                }
            continue;
            }
        if( strchr( "+-*/%&|^?:!~", t->c ) == NULL ) {
            continue;
            }
        if( t->c == '?' ) {
            openTernary ++;
            outKinds[ k ] = OP_BINARY;
            continue;
            }
        if( t->c == ':' ) {
            /* only the : of a ?:  (not case X: or a bit field) */
            if( openTernary > 0 ) {
                openTernary --;
                outKinds[ k ] = OP_BINARY;
                }
            continue;
            }
        if( t->c == '!' || t->c == '~' ) {
            outKinds[ k ] = OP_UNARY_SPACE;
            continue;
            }
        nextIsNum = k + 1 < inNum && inToks[ k + 1 ].type == TK_NUMBER;

        if( k == 0 && inPrevValue >= 0 ) {
            /* the previous line ended with a value (a binary operator
               continuing it) or not */
            afterValue = inPrevValue;
            }
        else if( k == 0 ) {
            /* nothing before it to go by */
            if( inCtx->midStatement && ! inCtx->inParen ) {
                /* x = a
                       - b   (or a continued unary;  can't tell) */
                if( ( t->c == '-' || t->c == '+' ) && ! nextIsNum ) {
                    /* a space after either way */
                    outKinds[ k ] = OP_UNARY_SPACE;
                    }
                else {
                    outKinds[ k ] = OP_UNSURE;
                    }
                continue;
                }
            afterValue = 0;
            }
        else {
            afterValue = isOperand( inText, inToks, match, k - 1 );
            }

        if( t->c == '*' && afterValue && inToks[ k - 1 ].type == TK_IDENT
            && isDeclStar( inText, inToks, inNum, k, inCtx ) ) {
            outKinds[ k ] = OP_UNARY_HUG;
            continue;
            }
        if( afterValue ) {
            outKinds[ k ] = OP_BINARY;
            }
        else if( t->c == '*' || t->c == '&' ) {
            outKinds[ k ] = OP_UNARY_HUG;
            }
        else if( t->c == '-' || t->c == '+' ) {
            outKinds[ k ] = nextIsNum ? OP_UNARY_HUG : OP_UNARY_SPACE;
            }
        /* a unary / % | ^ is a syntax error;  leave it alone */
        }
    }



/* 1 if the code line before inLine ends with a value (so an operator
   starting inLine is binary), 0 if not, -1 if unknown */
static int prevLineEndsWithValue( TextBuffer  *inTB,
                                  int          inLine ) {
    static int  match[ 1 ]  =  { -1 };
    int         li          =  inLine - 1;
    int         n;

    while( li >= 0 && tb_lineIsBlank( inTB, li ) ) {
        li --;
        }
    if( li < 0 ) {
        return 0;
        }
    n = lexCodeInto( inTB, li, &auxToks, NULL );
    if( n <= 0 ) {
        return -1;
        }
    /* (a ')' is taken as a value;  its cast check needs the match, so
       look at it alone) */
    return isOperand( inTB->lines[ li ].text, &( auxToks.tokens[ n - 1 ] ),
                      match, 0 );
    }



/* token spacing on one line:  paren / bracket padding, commas, spaces
   around assignments, a space before a block's '{' */
static void spaceTokens( TextBuffer   *inTB,
                         int           inLine,
                         LineContext  *inCtx ) {
    static EditList  edits;
    static int       kinds[ MAX_OP_TOKS ];
    int              n;
    Token           *toks;
    Line            *l;
    int              k;
    int              parenStack[ 128 ];
    int              numParens  =  0;
    int              prevValue;

    n = lexCode( inTB, inLine, NULL );
    if( n <= 0 || n > MAX_OP_TOKS ) {
        return;
        }
    l = &( inTB->lines[ inLine ] );
    toks = lineToks.tokens;
    edits.num = 0;
    prevValue = -1;
    if( inCtx->midStatement || inCtx->inParen ) {
        prevValue = prevLineEndsWithValue( inTB, inLine );
        /* (that lexed another line into auxToks, not lineToks) */
        }
    else {
        prevValue = 0;
        }
    classifyOperators( l->text, toks, n, inCtx, prevValue, kinds );

    /* comments were dropped from toks;  gaps that contain a comment must
       not be touched, so check the raw text between tokens */
    for( k = 0;
         k < n;
         k ++ ) {
        Token  *t      =  &( toks[ k ] );
        Token  *next   =  k + 1 < n ? &( toks[ k + 1 ] ) : NULL;
        int     gap    =  0;
        int     clean  =  1;

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

        /* around assignments:  x = 6;  z[ i ] += 2; */
        if( config.assignSpaces && gap == 0
            &&
            ( isAssignOp( l->text, t ) || isAssignOp( l->text, next ) ) ) {
            addEdit( &edits, tokEnd( t ), 0, 1 );
            }

        /* around comparisons and && ||:  x == 5  a && b */
        if( config.compareSpaces && gap == 0
            &&
            ( isCompareOp( l->text, t ) || isCompareOp( l->text, next )
              || isBoolOp( l->text, t ) || isBoolOp( l->text, next ) ) ) {
            addEdit( &edits, tokEnd( t ), 0, 1 );
            }

        /* around other binary operators:  a + b   x ? y : z */
        if( config.operatorSpaces && gap == 0
            &&
            ( kinds[ k ] == OP_BINARY || kinds[ k + 1 ] == OP_BINARY ) ) {
            addEdit( &edits, tokEnd( t ), 0, 1 );
            }

        /* after unary operators:  ! x   - y   but *p  &x  -1 */
        if( config.unarySpaces ) {
            if( gap == 0 && t->type == TK_KEYWORD && t->kw != KW_SIZEOF
                && kinds[ k + 1 ] != OP_NONE ) {
                /* return-x */
                addEdit( &edits, tokEnd( t ), 0, 1 );
                }
            if( kinds[ k ] == OP_UNARY_SPACE && gap == 0 ) {
                addEdit( &edits, tokEnd( t ), 0, 1 );
                }
            else if( kinds[ k ] == OP_UNARY_HUG && gap > 0
                     &&
                     ( next->type != TK_PUNCT || isP( next, '(' )
                       || kinds[ k + 1 ] == OP_UNARY_HUG ) ) {
                addEdit( &edits, tokEnd( t ), gap, 0 );
                }
            }

        /* before the '{' of a block or body:  ) {   else {   struct Foo { */
        if( config.spaceBeforeBrace && gap == 0 && isP( next, '{' )
            &&
            ( isP( t, ')' ) || t->type == TK_IDENT
              || t->type == TK_KEYWORD ) ) {
            addEdit( &edits, tokEnd( t ), 0, 1 );
            }
        }

    applyEditsCur( inTB, inLine, &edits );
    }



static int formatLineAt( TextBuffer  *inTB,
                         int          inLine ) {
    int          added;
    int          li;
    int          openCols[ 8 ];
    int          openLines[ 8 ];
    int          numOpens  =  0;
    int          i;
    LineContext  ctx;

    if( lexCode( inTB, inLine, NULL ) <= 0 ) {
        return 0;
        }
    /* the context at the line's start doesn't change below */
    ind_getLineContext( inTB, inLine, &ctx );
    spaceTokens( inTB, inLine, &ctx );
    added = splitLine( inTB, inLine, &ctx );

    /* for( ) headers that start on these lines, or that this line
       continues */
    if( ctx.inParen ) {
        int  ol;
        int  oc;
        int  kind;

        if( findOpenParen( inTB, inLine, &ol, &oc, &kind )
            && kind == G_FOR ) {
            openLines[ numOpens ] = ol;
            openCols[ numOpens ] = oc;
            numOpens ++;
            }
        }
    for( li = inLine;
         li <= inLine + added;
         li ++ ) {
        int     n     =  lexCode( inTB, li, NULL );
        Token  *toks  =  lineToks.tokens;
        int     k;

        for( k = 0;
             k + 1 < n && numOpens < 8;
             k ++ ) {
            if( toks[ k ].type == TK_KEYWORD && toks[ k ].kw == KW_FOR
                && isP( &( toks[ k + 1 ] ), '(' ) ) {
                openLines[ numOpens ] = li;
                openCols[ numOpens ] = toks[ k + 1 ].start;
                numOpens ++;
                }
            }
        }
    for( i = 0;
         i < numOpens;
         i ++ ) {
        formatForTable( inTB, openLines[ i ], openCols[ i ] );
        }

    /* function parameter list opened on this line, or continued by it */
    if( config.paramTypeSpaces > 0 ) {
        if( ctx.braceDepth == 0 && ! ctx.inParen && ! ctx.inList ) {
            int  oc  =  paramListOpenCol( inTB, inLine );

            if( oc >= 0 ) {
                formatParamList( inTB, inLine, oc );
                }
            }
        else if( ctx.inParamList ) {
            int  ol;
            int  oc;
            int  kind;

            if( findOpenParen( inTB, inLine, &ol, &oc, &kind ) ) {
                formatParamList( inTB, ol, oc );
                }
            }
        }
    return added;
    }



int fmt_formatLine( TextBuffer  *inTB,
                    int          inLine,
                    int          inFlags,
                    int         *ioCursorLine,
                    int         *ioCursorCol ) {
    int  added;

    if( ! config.formatOnEnter ) {
        return 0;
        }
    if( inLine < 0 || inLine >= inTB->numLines ) {
        return 0;
        }
    beginCursor( inTB, inFlags, ioCursorLine, ioCursorCol );
    added = formatLineAt( inTB, inLine );
    endCursor( inTB, ioCursorLine, ioCursorCol );
    return added;
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
        inLastLine += fmt_formatLine( inTB, i, 0, NULL, NULL );
        }
    for( i = inFirstLine;
         i <= inLastLine;
         i ++ ) {
        DeclRow  row;

        if( parseDeclLine( inTB, i, &row ) ) {
            fmt_alignDeclarations( inTB, i, 0, NULL, NULL );
            parseDeclLine( inTB, i, &row );
            i = row.lastLine;
            /* skip the rest of this run (which may span blank lines) */
            while( 1 ) {
                int  b  =  i + 1;

                while( b <= inLastLine
                       && b - ( i + 1 ) < config.declTableBlankLines
                       && tb_lineIsBlank( inTB, b ) ) {
                    b ++;
                    }
                if( b > inLastLine || ! parseDeclLine( inTB, b, &row ) ) {
                    break;
                    }
                i = row.lastLine;
                }
            }
        }
    }
