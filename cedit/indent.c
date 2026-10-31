/*
  indent.c

  See indent.h
*/

#include "indent.h"
#include "syntax.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>


#define MAX_FRAMES    512
#define MAX_IFS       64

/* lines to look back for an anchor before giving up and starting at the
   top of the file */
#define MAX_ANCHOR_LOOKBACK  20000

/* statement kinds beyond keyword ids */
#define STMT_LABEL  ( KW_COUNT + 1 )


typedef struct StmtState {
        /* a statement has begun at this level */
        int  active;
        /* indent of the line where the statement began */
        int  indent;
        /* keyword id of first token, or STMT_LABEL, or 0 */
        int  kind;
        /* for if/for/while/switch:  header paren closed */
        int  headerDone;
        /* a sub-statement (or block) is expected next */
        int  expectSub;
        /* saw '=' at statement level (so a '{' starts an initializer) */
        int  sawEquals;
        /* significant tokens seen at statement level */
        int  numTokens;
        int  firstIsIdent;
        /* saw struct/union/enum keyword at statement level */
        int  sawStruct;
        int  sawEnum;
        int  sawString;
        /* line of the most recent '=' at statement level, and column of
           the token after it, if on the same line (else -1) */
        int  equalsLine;
        int  assignCol;
        int  assignLine;
        int  afterEquals;
    } StmtState;


typedef struct Frame {
        /* '{' '(' '[' */
        char       type;
        char       isList;
        char       isSwitch;
        char       isStruct;
        /* extern "C" { ... }:  contents not indented */
        char       isExternC;
        char       continuesStmt;
        char       isHeaderParen;
        /* indent of the statement owning a block, or indent of the line
           holding the opener for parens and lists */
        int        base;
        /* column of first token after opener on the opener's line;
           -1 = no token seen yet, -2 = first token was on a later line */
        int        alignCol;
        int        openLine;
        /* paren directly after an identifier at top level */
        char       isParamList;
        StmtState  saved;
        int        ifStack[ MAX_IFS ];
        int        numIfs;
    } Frame;


typedef struct ParseState {
        Frame      frames[ MAX_FRAMES ];
        int        numFrames;
        StmtState  stmt;
        /* the top-level "block" if stack */
        int        topIfStack[ MAX_IFS ];
        int        topNumIfs;
        /* previous significant token, for param list detection */
        int        prevWasIdent;
        /* previous significant token was ')' */
        int        prevWasCloseParen;
    } ParseState;



static void resetStmt( StmtState  *inS ) {
    memset( inS, 0, sizeof( StmtState ) );
    inS->assignCol = -1;
    inS->assignLine = -1;
    inS->equalsLine = -1;
    }



static Frame *topFrame( ParseState  *inP ) {
    if( inP->numFrames == 0 ) {
        return NULL;
        }
    return &( inP->frames[ inP->numFrames - 1 ] );
    }



/* the frame holding the current statement context:  innermost '{' block
   that is not a list, or NULL for top level */
static void getIfStack( ParseState  *inP,
                        int        **outStack,
                        int        **outNum ) {
    Frame  *f  =  topFrame( inP );

    if( f == NULL ) {
        *outStack = inP->topIfStack;
        *outNum = &( inP->topNumIfs );
        }
    else {
        *outStack = f->ifStack;
        *outNum = &( f->numIfs );
        }
    }



static void pushIf( ParseState  *inP,
                    int          inIndent ) {
    int  *stack;
    int  *num;

    getIfStack( inP, &stack, &num );
    if( *num < MAX_IFS ) {
        stack[ *num ] = inIndent;
        ( *num ) ++;
        }
    }



static int popIf( ParseState  *inP,
                  int          inPeekOnly ) {
    int  *stack;
    int  *num;

    getIfStack( inP, &stack, &num );
    if( *num == 0 ) {
        return -1;
        }
    if( inPeekOnly ) {
        return stack[ *num - 1 ];
        }
    ( *num ) --;
    return stack[ *num ];
    }



static void clearIfs( ParseState  *inP ) {
    int  *stack;
    int  *num;

    getIfStack( inP, &stack, &num );
    *num = 0;
    }



static Frame *pushFrame( ParseState  *inP,
                         char         inType,
                         int          inBase,
                         int          inLine ) {
    Frame  *f;

    if( inP->numFrames >= MAX_FRAMES ) {
        /* absurd nesting;  drop the oldest */
        memmove( &( inP->frames[ 0 ] ), &( inP->frames[ 1 ] ),
                 sizeof( Frame ) * ( MAX_FRAMES - 1 ) );
        inP->numFrames --;
        }
    f = &( inP->frames[ inP->numFrames ] );
    inP->numFrames ++;

    f->type = inType;
    f->isList = 0;
    f->isSwitch = 0;
    f->isStruct = 0;
    f->isExternC = 0;
    f->continuesStmt = 0;
    f->isHeaderParen = 0;
    f->isParamList = 0;
    f->base = inBase;
    f->alignCol = -1;
    f->openLine = inLine;
    f->saved = inP->stmt;
    f->numIfs = 0;
    return f;
    }



static int isStmtContext( ParseState  *inP ) {
    Frame  *f  =  topFrame( inP );

    return f == NULL || ( f->type == '{' && ! f->isList );
    }



static int isPunctTok( Token  *inT,
                       char    inC ) {
    return inT->type == TK_PUNCT && inT->len == 1 && inT->c == inC;
    }



/* processes one significant token */
static void parseToken( ParseState  *inP,
                        Token       *inT,
                        const char  *inText,
                        int          inLineIndex,
                        int          inLineIndent,
                        int          inFirstOnLine ) {
    Frame      *top   =  topFrame( inP );
    StmtState  *s     =  &( inP->stmt );
    int         wasIdent;
    int         wasCloseParen;

    (void)inText;
    wasIdent = inP->prevWasIdent;
    wasCloseParen = inP->prevWasCloseParen;
    inP->prevWasIdent = ( inT->type == TK_IDENT );
    inP->prevWasCloseParen = isPunctTok( inT, ')' );

    if( top != NULL && top->alignCol == -1 ) {
        if( inLineIndex == top->openLine ) {
            top->alignCol = inT->start;
            }
        else {
            top->alignCol = -2;
            }
        }

    if( isStmtContext( inP ) ) {

        if( s->expectSub && s->kind == KW_ELSE
            &&
            inT->type == TK_KEYWORD && inT->kw == KW_IF
            &&
            ! inFirstOnLine ) {
            /* "else if" continues as an if at the else's indent */
            s->kind = KW_IF;
            s->expectSub = 0;
            s->headerDone = 0;
            s->numTokens ++;
            pushIf( inP, s->indent );
            return;
            }

        if( ( ! s->active || s->expectSub ) && ! isPunctTok( inT, '{' ) ) {
            /* a statement begins, possibly as the sub-statement of an
               if/while/for/else/do */
            int  isSub   =  s->expectSub;
            int  indent  =  inLineIndent;

            if( inFirstOnLine ) {
                indent = inT->start;
                }
            else if( s->active ) {
                /* if( x ) return;  continuation lines of the sub-statement
                   are relative to the header line */
                indent = s->indent;
                }

            if( inT->type == TK_KEYWORD && inT->kw == KW_ELSE ) {
                int  ifIndent  =  popIf( inP, 0 );

                resetStmt( s );
                s->active = 1;
                s->indent = ifIndent >= 0 ? ifIndent : indent;
                s->kind = KW_ELSE;
                s->expectSub = 1;
                s->numTokens = 1;
                return;
                }

            if( ! isSub ) {
                /* an else can only follow directly after an if */
                clearIfs( inP );
                }
            resetStmt( s );
            s->active = 1;
            s->indent = indent;
            s->firstIsIdent = ( inT->type == TK_IDENT );
            if( inT->type == TK_KEYWORD ) {
                s->kind = inT->kw;
                if( inT->kw == KW_IF ) {
                    pushIf( inP, indent );
                    }
                else if( inT->kw == KW_DO ) {
                    s->expectSub = 1;
                    s->numTokens = 1;
                    return;
                    }
                }
            }

        s->numTokens ++;

        if( s->afterEquals ) {
            s->afterEquals = 0;
            if( inLineIndex == s->equalsLine && s->assignCol < 0 ) {
                s->assignCol = inT->start;
                s->assignLine = inLineIndex;
                }
            }

        if( inT->type == TK_KEYWORD ) {
            if( inT->kw == KW_STRUCT || inT->kw == KW_UNION ) {
                s->sawStruct = 1;
                }
            else if( inT->kw == KW_ENUM ) {
                s->sawEnum = 1;
                }
            }
        else if( inT->type == TK_STRING ) {
            s->sawString = 1;
            }

        if( inT->type == TK_PUNCT && inT->len == 1 ) {
            switch( inT->c ) {
                case '{': {
                    Frame  *f;
                    int     base;
                    int     isList    =  0;
                    int     contStmt  =  0;
                    int     isSwitch  =  0;
                    int     isStruct  =  0;

                    if( s->active && s->numTokens > 1 && ! s->expectSub
                        &&
                        ( s->sawEquals || s->kind == KW_ENUM
                          || s->sawEnum ) ) {
                        /* initializer or enum list */
                        isList = 1;
                        contStmt = 1;
                        base = s->indent;
                        }
                    else if( s->active && s->numTokens > 1 ) {
                        /* block owned by the statement:  function body,
                           control statement body, struct body */
                        base = s->indent;
                        if( s->kind == KW_SWITCH ) {
                            isSwitch = 1;
                            }
                        if( ! s->expectSub && ! wasCloseParen
                            &&
                            s->sawStruct ) {
                            /* struct Foo { ... } x;  continues after } */
                            contStmt = 1;
                            isStruct = 1;
                            }
                        }
                    else {
                        /* bare block */
                        base = inLineIndent;
                        if( s->expectSub ) {
                            base = s->indent;
                            }
                        }

                    f = pushFrame( inP, '{', base, inLineIndex );
                    f->isList = (char)isList;
                    f->isSwitch = (char)isSwitch;
                    f->isStruct = (char)isStruct;
                    f->isExternC = ( s->kind == KW_EXTERN && s->sawString );
                    f->continuesStmt = (char)contStmt;
                    resetStmt( s );
                    return;
                    }
                case '}':
                    /* stray close at this level */
                    resetStmt( s );
                    return;
                case ';':
                    resetStmt( s );
                    return;
                case ':':
                    if( s->kind == KW_CASE || s->kind == KW_DEFAULT
                        ||
                        ( s->firstIsIdent && s->numTokens == 2 ) ) {
                        if( s->firstIsIdent && s->kind == 0 ) {
                            s->kind = STMT_LABEL;
                            }
                        resetStmt( s );
                        }
                    return;
                case '=':
                    s->sawEquals = 1;
                    s->afterEquals = 1;
                    s->equalsLine = inLineIndex;
                    return;
                case '(':
                case '[': {
                    Frame  *f  =  pushFrame( inP, inT->c, inLineIndent,
                                             inLineIndex );

                    if( inT->c == '('
                        &&
                        ( s->kind == KW_IF || s->kind == KW_WHILE
                          || s->kind == KW_FOR || s->kind == KW_SWITCH )
                        &&
                        ! s->headerDone ) {
                        f->isHeaderParen = 1;
                        }
                    if( inT->c == '(' && inP->numFrames == 1 && wasIdent ) {
                        f->isParamList = 1;
                        }
                    return;
                    }
                default:
                    return;
                }
            }
        return;
        }

    /* inside parens, brackets, or a brace list */
    if( inT->type == TK_PUNCT && inT->len == 1 ) {
        switch( inT->c ) {
            case '(':
            case '[': {
                /* args of a nested call that ends the line are indented
                   relative to the start of the enclosing argument:
                       if( findPiece(
                               a, ...                                   */
                int  base  =  inLineIndent;

                if( top->alignCol >= 0 && top->openLine == inLineIndex ) {
                    base = top->alignCol;
                    }
                pushFrame( inP, inT->c, base, inLineIndex );
                return;
                }
            case '{': {
                Frame  *f  =  pushFrame( inP, '{', inLineIndent, inLineIndex );

                f->isList = 1;
                f->continuesStmt = 1;
                return;
                }
            case ')':
            case ']':
                if( top->type == '{' ) {
                    /* unbalanced;  ignore */
                    return;
                    }
                inP->numFrames --;
                if( top->isHeaderParen && isStmtContext( inP ) ) {
                    inP->stmt.headerDone = 1;
                    inP->stmt.expectSub = 1;
                    }
                return;
            case '}': {
                /* close a list, or recover from unbalanced parens */
                while( inP->numFrames > 0 ) {
                    Frame  *f  =  topFrame( inP );

                    inP->numFrames --;
                    if( f->type == '{' ) {
                        inP->stmt = f->saved;
                        if( ! f->continuesStmt ) {
                            resetStmt( &( inP->stmt ) );
                            }
                        break;
                        }
                    }
                return;
                }
            default:
                return;
            }
        }
    }



/* handles a '}' in statement context (closing a block) */
static void closeBlock( ParseState  *inP ) {
    Frame  *f  =  topFrame( inP );

    if( f == NULL ) {
        resetStmt( &( inP->stmt ) );
        return;
        }
    inP->numFrames --;
    inP->stmt = f->saved;
    if( f->continuesStmt ) {
        inP->stmt.active = 1;
        inP->stmt.expectSub = 0;
        }
    else {
        resetStmt( &( inP->stmt ) );
        }
    }



static int findAnchor( TextBuffer  *inTB,
                       int          inLine ) {
    int  i;
    int  limit  =  inLine - MAX_ANCHOR_LOOKBACK;

    if( limit < 0 ) {
        limit = 0;
        }
    for( i = inLine - 1;
         i > limit;
         i -- ) {
        Line  *l  =  &( inTB->lines[ i ] );

        if( l->len > 0
            &&
            ( ( l->text[ 0 ] >= 'a' && l->text[ 0 ] <= 'z' )
              || ( l->text[ 0 ] >= 'A' && l->text[ 0 ] <= 'Z' )
              || l->text[ 0 ] == '_' ) ) {
            /* exclude labels like "done:" */
            int  j  =  0;

            while( j < l->len && syn_isIdentChar( l->text[ j ] ) ) {
                j ++;
                }
            while( j < l->len && l->text[ j ] == ' ' ) {
                j ++;
                }
            if( j < l->len && l->text[ j ] == ':'
                &&
                ( j + 1 >= l->len || l->text[ j + 1 ] != ':' ) ) {
                continue;
                }
            if( syn_getLineState( inTB, i ) == 0 ) {
                return i;
                }
            }
        }
    return 0;
    }



/* runs the parse from the anchor up to (not including) inLine */
/* feeds the tokens of one line into the parse */
static void feedLine( TextBuffer  *inTB,
                      int          inLine,
                      ParseState  *inP ) {
    static TokenList  tl;
    static int        tlInit  =  0;
    Line             *l       =  &( inTB->lines[ inLine ] );
    int               indent  =  tb_lineIndent( inTB, inLine );
    int               first   =  1;
    int               k;

    if( ! tlInit ) {
        syn_initTokenList( &tl );
        tlInit = 1;
        }
    syn_lexLine( l->text, l->len, syn_getLineState( inTB, inLine ), &tl );

    for( k = 0;
         k < tl.num;
         k ++ ) {
        Token  *t  =  &( tl.tokens[ k ] );

        if( t->type == TK_COMMENT || t->inPreproc ) {
            continue;
            }
        if( isStmtContext( inP ) && isPunctTok( t, '}' ) ) {
            Frame  *top  =  topFrame( inP );

            if( top != NULL && top->alignCol == -1 ) {
                top->alignCol = ( inLine == top->openLine ) ? t->start : -2;
                }
            closeBlock( inP );
            inP->prevWasIdent = 0;
            inP->prevWasCloseParen = 0;
            }
        else {
            parseToken( inP, t, l->text, inLine, indent, first );
            }
        first = 0;
        }
    }



/* runs the parse from the anchor up to (not including) inLine */
static void parseUpTo( TextBuffer  *inTB,
                       int          inLine,
                       ParseState  *outP ) {
    int  i;

    outP->numFrames = 0;
    outP->topNumIfs = 0;
    outP->prevWasIdent = 0;
    outP->prevWasCloseParen = 0;
    resetStmt( &( outP->stmt ) );

    for( i = findAnchor( inTB, inLine );
         i < inLine;
         i ++ ) {
        feedLine( inTB, i, outP );
        }
    }



/* line where the comment containing the start of inLine began, or -1 */
static int findCommentStart( TextBuffer  *inTB,
                             int          inLine ) {
    int  i;

    for( i = inLine - 1;
         i >= 0;
         i -- ) {
        if( ! ( syn_getLineState( inTB, i ) & LEX_IN_COMMENT ) ) {
            return i;
            }
        }
    return -1;
    }



int ind_commentIndent( TextBuffer  *inTB,
                       int          inLine ) {
    static TokenList  tl;
    static int        tlInit  =  0;
    int               k;
    int               i;
    Line             *l;
    int               indent;
    int               openCol  =  -1;

    if( ! tlInit ) {
        syn_initTokenList( &tl );
        tlInit = 1;
        }
    if( ! ( syn_getLineState( inTB, inLine ) & LEX_IN_COMMENT ) ) {
        return -1;
        }
    i = findCommentStart( inTB, inLine );
    if( i < 0 ) {
        return -1;
        }
    l = &( inTB->lines[ i ] );
    syn_lexLine( l->text, l->len, syn_getLineState( inTB, i ), &tl );
    for( k = 0;
         k < tl.num;
         k ++ ) {
        if( tl.tokens[ k ].type == TK_COMMENT && tl.tokens[ k ].open ) {
            openCol = tl.tokens[ k ].start;
            }
        }
    if( openCol < 0 ) {
        return -1;
        }

    indent = tb_lineIndent( inTB, inLine );
    l = &( inTB->lines[ inLine ] );
    if( indent + 1 < l->len && l->text[ indent ] == '*'
        &&
        l->text[ indent + 1 ] == '/' ) {
        /* closing a comment whose opener stands alone:  line up with the
           opener.  Otherwise leave it be. */
        Line  *o  =  &( inTB->lines[ i ] );
        int    j  =  openCol + 2;

        while( j < o->len && o->text[ j ] == ' ' ) {
            j ++;
            }
        if( j >= o->len ) {
            return openCol;
            }
        return -1;
        }
    if( indent < l->len && l->text[ indent ] == '*' ) {
        return openCol + 1;
        }
    if( ! config.alignComments ) {
        return -1;
        }
    /* text after the opening slash-star? */
    {
        Line  *o  =  &( inTB->lines[ i ] );
        int    j  =  openCol + 2;

        while( j < o->len && o->text[ j ] == ' ' ) {
            j ++;
            }
        if( j < o->len ) {
            return j;
            }
        }
    return -1;
    }



/* column of the first [ token on a line, or -1 */
static int firstBracketCol( TextBuffer  *inTB,
                            int          inLine ) {
    static TokenList  tl;
    static int        tlInit  =  0;
    Line             *l       =  &( inTB->lines[ inLine ] );
    int               k;

    if( ! tlInit ) {
        syn_initTokenList( &tl );
        tlInit = 1;
        }
    syn_lexLine( l->text, l->len, syn_getLineState( inTB, inLine ), &tl );
    for( k = 0;
         k < tl.num;
         k ++ ) {
        if( isPunctTok( &( tl.tokens[ k ] ), '[' ) ) {
            return tl.tokens[ k ].start;
            }
        }
    return -1;
    }



/* decides the indent of inLine given the parse state just before it */
static int decideIndent( TextBuffer  *inTB,
                         int          inLine,
                         ParseState  *p ) {
    static TokenList  tl;
    static int        tlInit  =  0;
    int               state;
    Line             *l;
    Token            *first  =  NULL;
    int               k;
    Frame            *top;
    StmtState        *s;

    if( ! tlInit ) {
        syn_initTokenList( &tl );
        tlInit = 1;
        }
    if( inLine < 0 || inLine >= inTB->numLines ) {
        return -1;
        }

    state = syn_getLineState( inTB, inLine );
    if( state & LEX_IN_PREPROC ) {
        return -1;
        }
    if( state & LEX_IN_COMMENT ) {
        /* comment bodies are often hand-formatted;  leave them be */
        return -1;
        }
    if( state & LEX_IN_STRING ) {
        return -1;
        }

    l = &( inTB->lines[ inLine ] );
    syn_lexLine( l->text, l->len, state, &tl );

    for( k = 0;
         k < tl.num;
         k ++ ) {
        if( tl.tokens[ k ].type != TK_COMMENT ) {
            first = &( tl.tokens[ k ] );
            break;
            }
        }
    if( first != NULL && first->inPreproc ) {
        return 0;
        }
    if( tl.num > 0 && tl.tokens[ 0 ].type == TK_COMMENT ) {
        /* comment-first line.  If it lines up with a comment on the
           previous line, keep that alignment (like emacs c-lineup-comment).
           Otherwise indent like a statement start. */
        int  col  =  tl.tokens[ 0 ].start;

        if( inLine > 0 && tl.tokens[ 0 ].start > 0 ) {
            Line  *prev  =  &( inTB->lines[ inLine - 1 ] );

            if( col + 1 < prev->len
                &&
                prev->text[ col ] == '/'
                &&
                ( prev->text[ col + 1 ] == '*'
                  || prev->text[ col + 1 ] == '/' )
                &&
                tb_lineIndent( inTB, inLine - 1 ) < col ) {
                return col;
                }
            }
        first = NULL;
        }

    top = topFrame( p );
    s = &( p->stmt );

    if( first != NULL && isPunctTok( first, '[' ) && inLine > 0
        &&
        ( s->active || ( top != NULL && top->type != '{' ) ) ) {
        /* continued array dimensions / subscripts line up with the first
           [ on the previous line:
               static int  grid[ W ]
                               [ H ];                                     */
        int  prevCol  =  firstBracketCol( inTB, inLine - 1 );

        if( prevCol >= 0 ) {
            return prevCol;
            }
        }

    if( top != NULL && top->type != '{' ) {
        /* inside ( or [ */
        int  closes  =  first != NULL
                        &&
                        ( isPunctTok( first, ')' ) || isPunctTok( first, ']' ) );
        int  alignCol  =  top->alignCol;

        if( closes ) {
            if( config.alignCloseParen && alignCol >= 0 ) {
                return alignCol;
                }
            return top->base;
            }
        if( config.alignArgs && alignCol >= 0 ) {
            return alignCol;
            }
        return top->base + config.argIndent;
        }

    if( top != NULL && top->isList ) {
        if( first != NULL && isPunctTok( first, '}' ) ) {
            return top->base + config.closeBraceOffset;
            }
        if( top->alignCol >= 0 ) {
            return top->alignCol;
            }
        return top->base + config.indentSize;
        }

    /* statement context */
    {
        int  bodyIndent;
        int  caseIndent  =  0;

        if( top == NULL ) {
            bodyIndent = 0;
            }
        else if( top->isSwitch ) {
            caseIndent = top->base + config.caseIndent;
            bodyIndent = caseIndent + config.caseBodyIndent;
            }
        else if( top->isStruct ) {
            bodyIndent = top->base + config.structIndent;
            }
        else if( top->isExternC ) {
            bodyIndent = top->base;
            }
        else {
            bodyIndent = top->base + config.indentSize;
            }

        if( first != NULL && isPunctTok( first, '}' ) ) {
            if( top == NULL ) {
                return 0;
                }
            if( top->isExternC ) {
                return top->base;
                }
            return top->base + config.closeBraceOffset;
            }

        if( s->expectSub ) {
            if( first != NULL && isPunctTok( first, '{' ) ) {
                return s->indent + config.openBraceOffset;
                }
            return s->indent + config.substatementIndent;
            }

        if( s->active ) {
            if( first != NULL && isPunctTok( first, '{' ) ) {
                return s->indent + config.openBraceOffset;
                }
            if( config.alignAssignments && s->assignCol >= 0 ) {
                return s->assignCol;
                }
            return s->indent + config.continuationIndent;
            }

        if( first != NULL && first->type == TK_KEYWORD
            &&
            first->kw == KW_ELSE ) {
            int  ifIndent  =  popIf( p, 1 );

            if( ifIndent >= 0 ) {
                return ifIndent;
                }
            return bodyIndent;
            }

        if( top != NULL && top->isSwitch && first != NULL
            &&
            first->type == TK_KEYWORD
            &&
            ( first->kw == KW_CASE || first->kw == KW_DEFAULT ) ) {
            return caseIndent;
            }

        return bodyIndent;
        }
    }



int ind_computeIndent( TextBuffer  *inTB,
                       int          inLine ) {
    static ParseState  parse;

    if( inLine < 0 || inLine >= inTB->numLines ) {
        return -1;
        }
    if( syn_getLineState( inTB, inLine ) != 0 ) {
        /* inside a comment, continued macro, or continued string */
        return -1;
        }
    parseUpTo( inTB, inLine, &parse );
    return decideIndent( inTB, inLine, &parse );
    }



int ind_indentLine( TextBuffer  *inTB,
                    int          inLine ) {
    int  want  =  ind_computeIndent( inTB, inLine );

    if( want < 0 ) {
        return 0;
        }
    if( tb_lineIsBlank( inTB, inLine ) ) {
        /* don't leave whitespace on blank lines, except for the line the
           cursor is on (editor handles that case by calling
           tb_setLineIndent itself) */
        return tb_setLineIndent( inTB, inLine, 0 );
        }
    return tb_setLineIndent( inTB, inLine, want );
    }



void ind_indentRegion( TextBuffer  *inTB,
                       int          inFirstLine,
                       int          inLastLine ) {
    int  i;
    /* shift applied to the most recent line, so comment bodies can move
       rigidly along with the line that opened them */
    int  lastDelta      =  0;
    int  lastDeltaLine  =  -1;
    static ParseState  parse;

    if( inFirstLine < 0 ) {
        inFirstLine = 0;
        }
    if( inLastLine >= inTB->numLines ) {
        inLastLine = inTB->numLines - 1;
        }

    /* one parse, carried forward line by line (each line is fed in AFTER
       being re-indented, so later lines see the new indentation) */
    parseUpTo( inTB, inFirstLine, &parse );

    for( i = inFirstLine;
         i <= inLastLine;
         i ++ ) {
        int  state  =  syn_getLineState( inTB, i );

        if( state & LEX_IN_COMMENT ) {
            int  opener  =  findCommentStart( inTB, i );

            if( opener == lastDeltaLine && lastDelta != 0
                &&
                ! tb_lineIsBlank( inTB, i ) ) {
                tb_setLineIndent( inTB, i,
                                  tb_lineIndent( inTB, i ) + lastDelta );
                }
            }
        else if( state == 0 ) {
            int  want  =  decideIndent( inTB, i, &parse );

            if( want >= 0 ) {
                if( tb_lineIsBlank( inTB, i ) ) {
                    want = 0;
                    }
                lastDelta = tb_setLineIndent( inTB, i, want );
                lastDeltaLine = i;
                }
            }
        feedLine( inTB, i, &parse );
        }
    }



void ind_getLineContext( TextBuffer   *inTB,
                         int           inLine,
                         LineContext  *outContext ) {
    static ParseState  parse;
    Frame             *top;
    int                state;
    int                i;

    memset( outContext, 0, sizeof( LineContext ) );
    state = syn_getLineState( inTB, inLine );
    outContext->inComment = ( state & LEX_IN_COMMENT ) != 0;
    outContext->inPreproc = ( state & LEX_IN_PREPROC ) != 0;

    parseUpTo( inTB, inLine, &parse );
    top = topFrame( &parse );
    if( top != NULL ) {
        outContext->inParen = ( top->type != '{' );
        outContext->inList = ( top->type == '{' && top->isList );
        outContext->inParamList = top->isParamList;
        }
    outContext->midStatement = parse.stmt.active || parse.stmt.expectSub;
    for( i = 0;
         i < parse.numFrames;
         i ++ ) {
        if( parse.frames[ i ].type == '{' ) {
            outContext->braceDepth ++;
            }
        }
    }



int ind_findMatchingBracket( TextBuffer  *inTB,
                             int          inLine,
                             int          inCol,
                             int          inMaxLines,
                             int         *outLine,
                             int         *outCol ) {
    static TokenList  tl;
    static int        tlInit  =  0;
    Line             *l;
    char              c;
    char              match;
    int               dir;
    int               depth  =  0;
    int               li;
    int               k;
    int               startTok  =  -1;
    int               linesSearched  =  0;

    if( ! tlInit ) {
        syn_initTokenList( &tl );
        tlInit = 1;
        }
    if( inLine < 0 || inLine >= inTB->numLines ) {
        return 0;
        }
    l = &( inTB->lines[ inLine ] );
    if( inCol < 0 || inCol >= l->len ) {
        return 0;
        }
    c = l->text[ inCol ];
    switch( c ) {
        case '(': match = ')'; dir = 1; break;
        case '[': match = ']'; dir = 1; break;
        case '{': match = '}'; dir = 1; break;
        case ')': match = '('; dir = -1; break;
        case ']': match = '['; dir = -1; break;
        case '}': match = '{'; dir = -1; break;
        default:
            return 0;
        }

    /* make sure it is a real bracket token (not in a string/comment) */
    syn_lexLine( l->text, l->len, syn_getLineState( inTB, inLine ), &tl );
    for( k = 0;
         k < tl.num;
         k ++ ) {
        if( tl.tokens[ k ].start == inCol
            &&
            tl.tokens[ k ].type == TK_PUNCT ) {
            startTok = k;
            break;
            }
        }
    if( startTok < 0 ) {
        return 0;
        }

    li = inLine;
    k = startTok;
    while( 1 ) {
        while( k >= 0 && k < tl.num ) {
            Token  *t  =  &( tl.tokens[ k ] );

            if( t->type == TK_PUNCT && t->len == 1 ) {
                if( t->c == c ) {
                    depth ++;
                    }
                else if( t->c == match ) {
                    depth --;
                    if( depth == 0 ) {
                        *outLine = li;
                        *outCol = t->start;
                        return 1;
                        }
                    }
                }
            k += dir;
            }
        li += dir;
        linesSearched ++;
        if( li < 0 || li >= inTB->numLines || linesSearched > inMaxLines ) {
            return 0;
            }
        l = &( inTB->lines[ li ] );
        syn_lexLine( l->text, l->len, syn_getLineState( inTB, li ), &tl );
        k = dir > 0 ? 0 : tl.num - 1;
        }
    }
