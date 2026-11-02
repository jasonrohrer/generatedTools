/*
  syntax.c

  See syntax.h
*/

#include "syntax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>



static const char  *keywordNames[ KW_COUNT ]  =  {
    "",
    "auto", "break", "case", "char", "const", "continue", "default",
    "do", "double", "else", "enum", "extern", "float", "for", "goto",
    "if", "int", "long", "register", "return", "short", "signed",
    "sizeof", "static", "struct", "switch", "typedef", "union",
    "unsigned", "void", "volatile", "while" };



void syn_initTokenList( TokenList  *inList ) {
    inList->tokens = NULL;
    inList->num = 0;
    inList->cap = 0;
    }



void syn_freeTokenList( TokenList  *inList ) {
    free( inList->tokens );
    syn_initTokenList( inList );
    }



static Token *addToken( TokenList  *inList,
                        int         inType,
                        int         inStart,
                        int         inLen,
                        int         inPreproc ) {
    Token  *t;

    if( inList->num >= inList->cap ) {
        inList->cap = inList->cap * 2 + 32;
        inList->tokens = (Token *)realloc( inList->tokens,
                                           (size_t)inList->cap
                                           * sizeof( Token ) );
        if( inList->tokens == NULL ) {
            fprintf( stderr, "cedit:  out of memory\n" );
            exit( 1 );
            }
        }
    t = &( inList->tokens[ inList->num ] );
    inList->num ++;

    t->type = inType;
    t->start = inStart;
    t->len = inLen;
    t->kw = KW_NONE;
    t->c = '\0';
    t->inPreproc = (char)inPreproc;
    t->open = 0;
    return t;
    }



int syn_isIdentChar( char  inC ) {
    return ( inC >= 'a' && inC <= 'z' )
           || ( inC >= 'A' && inC <= 'Z' )
           || ( inC >= '0' && inC <= '9' )
           || inC == '_';
    }



static int isDigit( char  inC ) {
    return inC >= '0' && inC <= '9';
    }



static int lookupKeyword( const char  *inText,
                          int          inLen ) {
    int  i;

    if( inLen < 2 || inLen > 8 ) {
        return KW_NONE;
        }
    /* quick reject:  no keyword starts with these */
    if( inText[ 0 ] < 'a' || inText[ 0 ] > 'w'
        ||
        strchr( "hjkmnopq", inText[ 0 ] ) != NULL ) {
        return KW_NONE;
        }
    for( i = 1;
         i < KW_COUNT;
         i ++ ) {
        if( keywordNames[ i ][ 0 ] == inText[ 0 ]
            &&
            (int)strlen( keywordNames[ i ] ) == inLen
            &&
            memcmp( keywordNames[ i ], inText, (size_t)inLen ) == 0 ) {
            return i;
            }
        }
    return KW_NONE;
    }



/* three, then two char operators, longest first */
static const char  *ops3[]  =  { "<<=", ">>=", "...", NULL };
static const char  *ops2[]  =  { "->", "++", "--", "<<", ">>", "<=", ">=",
                                 "==", "!=", "&&", "||", "+=", "-=", "*=",
                                 "/=", "%=", "&=", "|=", "^=", "##", NULL };



int syn_lexLine( const char  *inText,
                 int          inLen,
                 int          inStartState,
                 TokenList   *outTokens ) {
    int     pos           =  0;
    int     preproc       =  0;
    int     nextState     =  0;
    int     sawDirective  =  0;
    int     isInclude     =  0;
    Token  *t;

    outTokens->num = 0;

    if( inStartState & LEX_IN_PREPROC ) {
        preproc = 1;
        }

    if( inStartState & LEX_IN_COMMENT ) {
        int  end  =  -1;
        int  i;

        for( i = 0;
             i + 1 < inLen;
             i ++ ) {
            if( inText[ i ] == '*' && inText[ i + 1 ] == '/' ) {
                end = i + 2;
                break;
                }
            }
        if( end < 0 ) {
            t = addToken( outTokens, TK_COMMENT, 0, inLen, preproc );
            t->open = 1;
            nextState = LEX_IN_COMMENT;
            if( preproc && inLen > 0 && inText[ inLen - 1 ] == '\\' ) {
                nextState |= LEX_IN_PREPROC;
                }
            return nextState;
            }
        addToken( outTokens, TK_COMMENT, 0, end, preproc );
        pos = end;
        }
    else if( inStartState & LEX_IN_STRING ) {
        int  i  =  0;

        while( i < inLen && inText[ i ] != '"' ) {
            if( inText[ i ] == '\\' ) {
                i ++;
                }
            i ++;
            }
        if( i >= inLen ) {
            t = addToken( outTokens, TK_STRING, 0, inLen, preproc );
            t->open = 1;
            if( inLen > 0 && inText[ inLen - 1 ] == '\\' ) {
                nextState = LEX_IN_STRING;
                if( preproc ) {
                    nextState |= LEX_IN_PREPROC;
                    }
                }
            return nextState;
            }
        addToken( outTokens, TK_STRING, 0, i + 1, preproc );
        pos = i + 1;
        }

    if( ! preproc ) {
        /* a '#' as the first non-space char starts a preprocessor line */
        int  i  =  pos;

        while( i < inLen && inText[ i ] == ' ' ) {
            i ++;
            }
        if( i < inLen && inText[ i ] == '#' && outTokens->num == 0 ) {
            preproc = 1;
            }
        }

    while( pos < inLen ) {
        char  c  =  inText[ pos ];

        if( c == ' ' || c == '\t' || c == '\r' ) {
            pos ++;
            continue;
            }

        if( c == '/' && pos + 1 < inLen && inText[ pos + 1 ] == '*' ) {
            int  i  =  pos + 2;
            int  e  =  -1;

            while( i + 1 < inLen ) {
                if( inText[ i ] == '*' && inText[ i + 1 ] == '/' ) {
                    e = i + 2;
                    break;
                    }
                i ++;
                }
            if( e < 0 ) {
                t = addToken( outTokens, TK_COMMENT, pos, inLen - pos,
                              preproc );
                t->open = 1;
                nextState |= LEX_IN_COMMENT;
                pos = inLen;
                break;
                }
            addToken( outTokens, TK_COMMENT, pos, e - pos, preproc );
            pos = e;
            continue;
            }

        if( c == '/' && pos + 1 < inLen && inText[ pos + 1 ] == '/' ) {
            addToken( outTokens, TK_COMMENT, pos, inLen - pos, preproc );
            pos = inLen;
            break;
            }

        if( preproc && ! sawDirective && c == '#' ) {
            int  i  =  pos + 1;
            int  ws;

            while( i < inLen && inText[ i ] == ' ' ) {
                i ++;
                }
            ws = i;
            while( i < inLen && syn_isIdentChar( inText[ i ] ) ) {
                i ++;
                }
            if( i - ws == 7 && strncmp( inText + ws, "include", 7 ) == 0 ) {
                isInclude = 1;
                }
            addToken( outTokens, TK_DIRECTIVE, pos, i - pos, preproc );
            sawDirective = 1;
            pos = i;
            continue;
            }

        if( isInclude && c == '<' ) {
            int  i  =  pos + 1;

            while( i < inLen && inText[ i ] != '>' ) {
                i ++;
                }
            if( i < inLen ) {
                i ++;
                }
            addToken( outTokens, TK_INCLUDE_PATH, pos, i - pos, preproc );
            pos = i;
            continue;
            }

        if( c == '"' || c == '\'' ) {
            int  i       =  pos + 1;
            int  closed  =  0;

            while( i < inLen ) {
                if( inText[ i ] == '\\' ) {
                    i += 2;
                    continue;
                    }
                if( inText[ i ] == c ) {
                    closed = 1;
                    i ++;
                    break;
                    }
                i ++;
                }
            if( i > inLen ) {
                i = inLen;
                }
            t = addToken( outTokens, c == '"' ? TK_STRING : TK_CHAR,
                          pos, i - pos, preproc );
            if( ! closed ) {
                t->open = 1;
                if( c == '"' && inLen > 0 && inText[ inLen - 1 ] == '\\' ) {
                    nextState |= LEX_IN_STRING;
                    }
                }
            pos = i;
            continue;
            }

        if( isDigit( c )
            ||
            ( c == '.' && pos + 1 < inLen && isDigit( inText[ pos + 1 ] ) ) ) {
            int  i  =  pos + 1;

            while( i < inLen ) {
                char  d  =  inText[ i ];

                if( syn_isIdentChar( d ) || d == '.' ) {
                    i ++;
                    }
                else if( ( d == '+' || d == '-' )
                         &&
                         ( inText[ i - 1 ] == 'e' || inText[ i - 1 ] == 'E' )
                         &&
                         ! ( inText[ pos ] == '0' && pos + 1 < inLen
                             &&
                             ( inText[ pos + 1 ] == 'x'
                               || inText[ pos + 1 ] == 'X' ) ) ) {
                    i ++;
                    }
                else {
                    break;
                    }
                }
            addToken( outTokens, TK_NUMBER, pos, i - pos, preproc );
            pos = i;
            continue;
            }

        if( syn_isIdentChar( c ) ) {
            int  i  =  pos + 1;
            int  kw;

            while( i < inLen && syn_isIdentChar( inText[ i ] ) ) {
                i ++;
                }
            kw = lookupKeyword( inText + pos, i - pos );
            t = addToken( outTokens, kw != KW_NONE ? TK_KEYWORD : TK_IDENT,
                          pos, i - pos, preproc );
            t->kw = kw;
            pos = i;
            continue;
            }

        /* punctuation / operators */
        {
            int  len  =  1;
            int  k;

            for( k = 0;
                 ops3[ k ] != NULL;
                 k ++ ) {
                if( pos + 3 <= inLen
                    &&
                    strncmp( inText + pos, ops3[ k ], 3 ) == 0 ) {
                    len = 3;
                    break;
                    }
                }
            if( len == 1 ) {
                for( k = 0;
                     ops2[ k ] != NULL;
                     k ++ ) {
                    if( pos + 2 <= inLen
                        &&
                        strncmp( inText + pos, ops2[ k ], 2 ) == 0 ) {
                        len = 2;
                        break;
                        }
                    }
                }
            t = addToken( outTokens, TK_PUNCT, pos, len, preproc );
            t->c = c;
            pos += len;
            }
        }

    if( preproc && inLen > 0 && inText[ inLen - 1 ] == '\\' ) {
        nextState |= LEX_IN_PREPROC;
        }
    return nextState;
    }



int syn_getLineState( TextBuffer  *inTB,
                      int          inLine ) {
    static TokenList  scratch;
    static int        scratchInit  =  0;

    if( ! scratchInit ) {
        syn_initTokenList( &scratch );
        scratchInit = 1;
        }

    if( inLine < 0 ) {
        return 0;
        }
    if( inLine >= inTB->numLines ) {
        inLine = inTB->numLines - 1;
        }

    if( inTB->capLineState < inTB->numLines + 1 ) {
        int  newCap  =  inTB->numLines * 2 + 64;

        inTB->lineState = (unsigned char *)realloc( inTB->lineState,
                                                    (size_t)newCap );
        if( inTB->lineState == NULL ) {
            fprintf( stderr, "cedit:  out of memory\n" );
            exit( 1 );
            }
        inTB->capLineState = newCap;
        }

    if( inTB->stateValidThrough < 0 ) {
        inTB->stateValidThrough = 0;
        }
    if( inTB->stateValidThrough == 0 ) {
        inTB->lineState[ 0 ] = 0;
        }

    while( inTB->stateValidThrough < inLine ) {
        int    i  =  inTB->stateValidThrough;
        Line  *l  =  &( inTB->lines[ i ] );
        int    s  =  inTB->lineState[ i ];

        /* fast path:  plain lines without any comment/quote/backslash
           chars can't change state */
        if( s == 0
            &&
            ( l->len == 0
              ||
              ( memchr( l->text, '/', (size_t)l->len ) == NULL
                &&
                memchr( l->text, '"', (size_t)l->len ) == NULL
                &&
                l->text[ l->len - 1 ] != '\\' ) ) ) {
            inTB->lineState[ i + 1 ] = 0;
            }
        else {
            inTB->lineState[ i + 1 ] =
                (unsigned char)syn_lexLine( l->text, l->len, s, &scratch );
            }
        inTB->stateValidThrough ++;
        }

    return inTB->lineState[ inLine ];
    }



/* ------------------------------------------------------------------
   Type name set
   ------------------------------------------------------------------ */

typedef struct TypeEntry {
        char  *name;
        int    len;
        /* 1 = builtin or configured (survives syn_clearScannedTypes) */
        int    permanent;
    } TypeEntry;

static TypeEntry  *typeTable      =  NULL;
static int         typeTableSize  =  0;
static int         typeTableUsed  =  0;
static int         builtinsAdded  =  0;


static const char  *builtinTypes[]  =  {
    "size_t", "ptrdiff_t", "wchar_t", "FILE", "va_list", "time_t",
    "clock_t", "fpos_t", "div_t", "ldiv_t", "jmp_buf", "sig_atomic_t",
    "off_t", "ssize_t", "pid_t", "uid_t", "gid_t", "mode_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t",
    "uint32_t", "uint64_t", "intptr_t", "uintptr_t", "DIR",
    NULL };



static unsigned int hashName( const char  *inName,
                              int          inLen ) {
    unsigned int  h  =  2166136261u;
    int           i;

    for( i = 0;
         i < inLen;
         i ++ ) {
        h ^= (unsigned char)inName[ i ];
        h *= 16777619u;
        }
    return h;
    }



static void addTypeInternal( const char  *inName,
                             int          inLen,
                             int          inPermanent );



static void growTypeTable( void ) {
    TypeEntry  *old      =  typeTable;
    int         oldSize  =  typeTableSize;
    int         i;

    typeTableSize = oldSize == 0 ? 256 : oldSize * 2;
    typeTable = (TypeEntry *)calloc( (size_t)typeTableSize,
                                     sizeof( TypeEntry ) );
    typeTableUsed = 0;

    for( i = 0;
         i < oldSize;
         i ++ ) {
        if( old[ i ].name != NULL ) {
            addTypeInternal( old[ i ].name, old[ i ].len, old[ i ].permanent );
            free( old[ i ].name );
            }
        }
    free( old );
    }



static int findType( const char  *inName,
                     int          inLen ) {
    unsigned int  h;
    int           i;

    if( typeTableSize == 0 ) {
        return -1;
        }
    h = hashName( inName, inLen );
    i = (int)( h & (unsigned int)( typeTableSize - 1 ) );

    while( typeTable[ i ].name != NULL ) {
        if( typeTable[ i ].len == inLen
            &&
            memcmp( typeTable[ i ].name, inName, (size_t)inLen ) == 0 ) {
            return i;
            }
        i = ( i + 1 ) & ( typeTableSize - 1 );
        }
    return -1;
    }



static void addTypeInternal( const char  *inName,
                             int          inLen,
                             int          inPermanent ) {
    unsigned int  h;
    int           i;

    if( inLen <= 0 ) {
        return;
        }
    i = findType( inName, inLen );
    if( i >= 0 ) {
        if( inPermanent ) {
            typeTable[ i ].permanent = 1;
            }
        return;
        }
    if( ( typeTableUsed + 1 ) * 2 >= typeTableSize ) {
        growTypeTable();
        }
    h = hashName( inName, inLen );
    i = (int)( h & (unsigned int)( typeTableSize - 1 ) );
    while( typeTable[ i ].name != NULL ) {
        i = ( i + 1 ) & ( typeTableSize - 1 );
        }
    typeTable[ i ].name = (char *)malloc( (size_t)inLen + 1 );
    memcpy( typeTable[ i ].name, inName, (size_t)inLen );
    typeTable[ i ].name[ inLen ] = '\0';
    typeTable[ i ].len = inLen;
    typeTable[ i ].permanent = inPermanent;
    typeTableUsed ++;
    }



static void ensureBuiltins( void ) {
    int  i;

    if( builtinsAdded ) {
        return;
        }
    builtinsAdded = 1;
    for( i = 0;
         builtinTypes[ i ] != NULL;
         i ++ ) {
        addTypeInternal( builtinTypes[ i ], (int)strlen( builtinTypes[ i ] ),
                         1 );
        }
    }



int syn_isTypeName( const char  *inName,
                    int          inLen ) {
    ensureBuiltins();
    return findType( inName, inLen ) >= 0;
    }



void syn_addTypeName( const char  *inName,
                      int          inLen ) {
    ensureBuiltins();
    addTypeInternal( inName, inLen, 0 );
    }



void syn_clearScannedTypes( void ) {
    TypeEntry  *old      =  typeTable;
    int         oldSize  =  typeTableSize;
    int         i;

    typeTable = NULL;
    typeTableSize = 0;
    typeTableUsed = 0;
    for( i = 0;
         i < oldSize;
         i ++ ) {
        if( old[ i ].name != NULL ) {
            if( old[ i ].permanent ) {
                addTypeInternal( old[ i ].name, old[ i ].len, 1 );
                }
            free( old[ i ].name );
            }
        }
    free( old );
    }



void syn_addConfiguredTypes( const char  *inList ) {
    const char  *p  =  inList;

    ensureBuiltins();
    while( *p != '\0' ) {
        const char  *s;

        while( *p != '\0' && ! syn_isIdentChar( *p ) ) {
            p ++;
            }
        s = p;
        while( *p != '\0' && syn_isIdentChar( *p ) ) {
            p ++;
            }
        if( p > s ) {
            addTypeInternal( s, (int)( p - s ), 1 );
            }
        }
    }



void syn_scanTypedefs( TextBuffer  *inTB ) {
    TokenList  tl;
    int        state         =  0;
    int        i;
    int        inTypedef     =  0;
    int        braceDepth    =  0;
    int        parenDepth    =  0;
    /* candidate name:  line, start, len */
    int        candLine      =  -1;
    int        candStart     =  0;
    int        candLen       =  0;
    int        candIsFuncPtr =  0;
    int        prevWasStar   =  0;
    int        prevPrevParen =  0;

    ensureBuiltins();
    syn_initTokenList( &tl );

    for( i = 0;
         i < inTB->numLines;
         i ++ ) {
        Line  *l  =  &( inTB->lines[ i ] );
        int    k;

        /* cheap skip of lines that can't matter */
        if( ! inTypedef && state == 0
            &&
            ( l->len == 0
              ||
              ( memchr( l->text, '/', (size_t)l->len ) == NULL
                &&
                memchr( l->text, '"', (size_t)l->len ) == NULL
                &&
                l->text[ l->len - 1 ] != '\\' ) ) ) {
            /* lines aren't \0 terminated, so a bounded search */
            int  found  =  0;
            int  j;

            for( j = 0;
                 j + 7 <= l->len;
                 j ++ ) {
                if( l->text[ j ] == 't'
                    &&
                    memcmp( l->text + j, "typedef", 7 ) == 0 ) {
                    found = 1;
                    break;
                    }
                }
            if( ! found ) {
                continue;
                }
            }

        state = syn_lexLine( l->text, l->len, state, &tl );

        for( k = 0;
             k < tl.num;
             k ++ ) {
            Token  *t  =  &( tl.tokens[ k ] );

            if( t->type == TK_COMMENT || t->inPreproc ) {
                continue;
                }
            if( ! inTypedef ) {
                if( t->type == TK_KEYWORD && t->kw == KW_TYPEDEF ) {
                    inTypedef = 1;
                    braceDepth = 0;
                    parenDepth = 0;
                    candLine = -1;
                    candIsFuncPtr = 0;
                    prevWasStar = 0;
                    prevPrevParen = 0;
                    }
                continue;
                }

            if( t->type == TK_PUNCT ) {
                if( t->c == '{' ) {
                    braceDepth ++;
                    }
                else if( t->c == '}' ) {
                    braceDepth --;
                    }
                else if( t->c == '(' ) {
                    parenDepth ++;
                    }
                else if( t->c == ')' ) {
                    parenDepth --;
                    }
                else if( t->c == ';' && braceDepth <= 0 ) {
                    if( candLine >= 0 ) {
                        syn_addTypeName(
                            inTB->lines[ candLine ].text + candStart,
                            candLen );
                        }
                    inTypedef = 0;
                    }
                prevPrevParen = ( t->c == '(' );
                prevWasStar = ( t->c == '*' );
                if( prevWasStar ) {
                    /* keep prevPrevParen from the paren before star */
                    prevPrevParen = ( k > 0
                                      &&
                                      tl.tokens[ k - 1 ].type == TK_PUNCT
                                      &&
                                      tl.tokens[ k - 1 ].c == '(' );
                    }
                continue;
                }
            if( t->type == TK_IDENT && braceDepth == 0 ) {
                if( prevWasStar && prevPrevParen && parenDepth == 1 ) {
                    candLine = i;
                    candStart = t->start;
                    candLen = t->len;
                    candIsFuncPtr = 1;
                    }
                else if( parenDepth == 0 && ! candIsFuncPtr ) {
                    candLine = i;
                    candStart = t->start;
                    candLen = t->len;
                    }
                }
            prevWasStar = 0;
            prevPrevParen = 0;
            }
        }
    syn_freeTokenList( &tl );
    }



/* ------------------------------------------------------------------
   Highlighting
   ------------------------------------------------------------------ */

static int isTypeKeyword( int  inKW ) {
    switch( inKW ) {
        case KW_CHAR:
        case KW_DOUBLE:
        case KW_FLOAT:
        case KW_INT:
        case KW_LONG:
        case KW_SHORT:
        case KW_SIGNED:
        case KW_UNSIGNED:
        case KW_VOID:
            return 1;
        default:
            return 0;
        }
    }



int syn_tokenIsType( const char  *inLineText,
                     Token       *inToken ) {
    if( inToken->type == TK_KEYWORD ) {
        return isTypeKeyword( inToken->kw );
        }
    if( inToken->type == TK_IDENT ) {
        return syn_isTypeName( inLineText + inToken->start, inToken->len );
        }
    return 0;
    }



static int isAllCaps( const char  *inText,
                      int          inLen ) {
    int  i;
    int  letters  =  0;

    if( inLen < 2 ) {
        return 0;
        }
    for( i = 0;
         i < inLen;
         i ++ ) {
        char  c  =  inText[ i ];

        if( c >= 'a' && c <= 'z' ) {
            return 0;
            }
        if( c >= 'A' && c <= 'Z' ) {
            letters ++;
            }
        }
    return letters > 0;
    }



static int isPunct( Token  *inT,
                    char    inC ) {
    return inT->type == TK_PUNCT && inT->len == 1 && inT->c == inC;
    }



void syn_highlightLine( TextBuffer     *inTB,
                        int             inLine,
                        unsigned char  *outClasses ) {
    static TokenList  tl;
    static int        tlInit  =  0;
    /* class per token */
    static int       *tokClass     =  NULL;
    static int        tokClassCap  =  0;
    /* indices of significant (non-comment) tokens */
    static int       *sig          =  NULL;
    static int        sigCap       =  0;
    int               numSig       =  0;
    Line             *l            =  &( inTB->lines[ inLine ] );
    const char       *text         =  l->text;
    int               state;
    int               k;

    if( ! tlInit ) {
        syn_initTokenList( &tl );
        tlInit = 1;
        }

    memset( outClasses, HL_TEXT, (size_t)l->len );
    if( l->len == 0 ) {
        return;
        }

    state = syn_getLineState( inTB, inLine );
    syn_lexLine( text, l->len, state, &tl );

    if( tokClassCap < tl.num ) {
        tokClassCap = tl.num * 2;
        tokClass = (int *)realloc( tokClass,
                                   (size_t)tokClassCap * sizeof( int ) );
        }
    if( sigCap < tl.num ) {
        sigCap = tl.num * 2;
        sig = (int *)realloc( sig, (size_t)sigCap * sizeof( int ) );
        }

    /* first pass:  basic classes */
    for( k = 0;
         k < tl.num;
         k ++ ) {
        Token  *t  =  &( tl.tokens[ k ] );
        int     c  =  HL_TEXT;

        switch( t->type ) {
            case TK_COMMENT:
                c = HL_COMMENT;
                break;
            case TK_STRING:
            case TK_INCLUDE_PATH:
                c = HL_STRING;
                break;
            case TK_CHAR:
                c = HL_CHAR;
                break;
            case TK_NUMBER:
                c = HL_NUMBER;
                break;
            case TK_DIRECTIVE:
                c = HL_PREPROC;
                break;
            case TK_KEYWORD:
                c = isTypeKeyword( t->kw ) ? HL_TYPE : HL_KEYWORD;
                break;
            case TK_IDENT:
                if( syn_isTypeName( text + t->start, t->len ) ) {
                    c = HL_TYPE;
                    }
                else if( isAllCaps( text + t->start, t->len )
                         ||
                         ( t->len == 4
                           && strncmp( text + t->start, "NULL", 4 ) == 0 ) ) {
                    c = HL_CONSTANT;
                    }
                else if( t->inPreproc && t->len == 7
                         &&
                         strncmp( text + t->start, "defined", 7 ) == 0 ) {
                    c = HL_PREPROC;
                    }
                break;
            case TK_PUNCT:
                if( t->len == 1
                    &&
                    ( t->c == '(' || t->c == ')' || t->c == '[' || t->c == ']'
                      || t->c == '{' || t->c == '}' ) ) {
                    c = HL_BRACKET;
                    }
                else {
                    c = HL_OPERATOR;
                    }
                break;
            }
        tokClass[ k ] = c;
        if( t->type != TK_COMMENT ) {
            sig[ numSig ] = k;
            numSig ++;
            }
        }

    /* second pass:  context (declarations, function names, struct tags) */
    for( k = 0;
         k < numSig;
         k ++ ) {
        int     ti    =  sig[ k ];
        Token  *t     =  &( tl.tokens[ ti ] );
        Token  *next  =  NULL;
        int     j;
        int     prevTypeIdx  =  -1;
        int     sawStar      =  0;

        if( t->type != TK_IDENT ) {
            continue;
            }
        if( k + 1 < numSig ) {
            next = &( tl.tokens[ sig[ k + 1 ] ] );
            }

        /* #define NAME */
        if( t->inPreproc && k >= 1
            &&
            tl.tokens[ sig[ k - 1 ] ].type == TK_DIRECTIVE ) {
            Token  *d  =  &( tl.tokens[ sig[ k - 1 ] ] );

            if( d->len >= 7
                &&
                strncmp( text + d->start + d->len - 6, "define", 6 ) == 0 ) {
                if( next != NULL && isPunct( next, '(' )
                    &&
                    next->start == t->start + t->len ) {
                    tokClass[ ti ] = HL_FUNC_DEF;
                    }
                else {
                    tokClass[ ti ] = HL_VAR_DECL;
                    }
                }
            continue;
            }

        /* struct/union/enum tag */
        if( k >= 1 ) {
            Token  *p  =  &( tl.tokens[ sig[ k - 1 ] ] );

            if( p->type == TK_KEYWORD
                &&
                ( p->kw == KW_STRUCT || p->kw == KW_UNION
                  || p->kw == KW_ENUM ) ) {
                tokClass[ ti ] = HL_TYPE;
                continue;
                }
            }

        /* look back over '*'s for a type */
        j = k - 1;
        while( j >= 0 && isPunct( &( tl.tokens[ sig[ j ] ] ), '*' ) ) {
            sawStar = 1;
            j --;
            }
        if( j >= 0 ) {
            Token  *p  =  &( tl.tokens[ sig[ j ] ] );

            if( tokClass[ sig[ j ] ] == HL_TYPE
                ||
                ( p->type == TK_IDENT
                  &&
                  ! sawStar
                  &&
                  ( j == 0
                    ||
                    tl.tokens[ sig[ j - 1 ] ].type != TK_IDENT ) ) ) {
                prevTypeIdx = j;
                }
            else if( p->type == TK_IDENT && sawStar
                     &&
                     text[ p->start ] >= 'A' && text[ p->start ] <= 'Z'
                     &&
                     ! isAllCaps( text + p->start, p->len ) ) {
                /* "Deck *d":  capitalized identifier followed by star */
                prevTypeIdx = j;
                }
            }

        if( prevTypeIdx >= 0 ) {
            Token  *p  =  &( tl.tokens[ sig[ prevTypeIdx ] ] );

            if( p->type == TK_IDENT && tokClass[ sig[ prevTypeIdx ] ]
                != HL_CONSTANT ) {
                tokClass[ sig[ prevTypeIdx ] ] = HL_TYPE;
                }
            if( tokClass[ sig[ prevTypeIdx ] ] != HL_TYPE ) {
                prevTypeIdx = -1;
                }
            }

        if( next != NULL && isPunct( next, '(' ) ) {
            if( prevTypeIdx >= 0 ) {
                tokClass[ ti ] = HL_FUNC_DEF;
                }
            else if( tokClass[ ti ] == HL_TEXT ) {
                tokClass[ ti ] = HL_FUNC_CALL;
                }
            }
        else if( prevTypeIdx >= 0 && tokClass[ ti ] == HL_TEXT ) {
            tokClass[ ti ] = HL_VAR_DECL;
            }
        }

    /* paint */
    for( k = 0;
         k < tl.num;
         k ++ ) {
        Token  *t  =  &( tl.tokens[ k ] );
        int     e  =  t->start + t->len;

        if( e > l->len ) {
            e = l->len;
            }
        memset( outClasses + t->start, tokClass[ k ],
                (size_t)( e - t->start ) );

        if( t->type == TK_STRING || t->type == TK_CHAR ) {
            int  i;

            for( i = t->start;
                 i < e;
                 i ++ ) {
                if( text[ i ] == '\\' && i + 1 < e ) {
                    outClasses[ i ] = HL_ESCAPE;
                    outClasses[ i + 1 ] = HL_ESCAPE;
                    i ++;
                    }
                }
            }
        }
    /* whole preprocessor line gets the preproc color for its plain
       identifiers?  No:  keep tokens colored, like emacs. */
    }
