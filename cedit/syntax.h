/*
  syntax.h

  C89 lexer, shared by the syntax highlighter and the indentation engine.

  Lexing is line-at-a-time.  The only state that carries across lines is a
  small set of flags (inside a block comment, inside a backslash-continued
  preprocessor line, inside a backslash-continued string).  The buffer
  caches the state at the start of every line (TextBuffer.lineState), so
  any single line can be lexed in isolation, which is what makes scrolling
  through huge files fast:  only visible lines are ever highlighted.

  Pure C89, no SDL.
*/

#ifndef CEDIT_SYNTAX_H
#define CEDIT_SYNTAX_H

#include "textBuffer.h"


/* line-start lexer state flags */
#define LEX_IN_COMMENT  1
#define LEX_IN_PREPROC  2
#define LEX_IN_STRING   4


/* token types */
enum TokenType {
    TK_IDENT = 1,
    TK_KEYWORD,
    TK_NUMBER,
    TK_STRING,
    TK_CHAR,
    TK_COMMENT,
    TK_PUNCT,
    /* the "#" and directive word of a preprocessor line */
    TK_DIRECTIVE,
    /* <file.h> after #include */
    TK_INCLUDE_PATH
    };


/* keyword ids (Token.kw), 0 for non-keywords */
enum KeywordID {
    KW_NONE = 0,
    KW_AUTO, KW_BREAK, KW_CASE, KW_CHAR, KW_CONST, KW_CONTINUE, KW_DEFAULT,
    KW_DO, KW_DOUBLE, KW_ELSE, KW_ENUM, KW_EXTERN, KW_FLOAT, KW_FOR, KW_GOTO,
    KW_IF, KW_INT, KW_LONG, KW_REGISTER, KW_RETURN, KW_SHORT, KW_SIGNED,
    KW_SIZEOF, KW_STATIC, KW_STRUCT, KW_SWITCH, KW_TYPEDEF, KW_UNION,
    KW_UNSIGNED, KW_VOID, KW_VOLATILE, KW_WHILE,
    KW_COUNT
    };


typedef struct Token {
        int  type;
        int  start;
        int  len;
        /* keyword id for TK_KEYWORD */
        int  kw;
        /* for TK_PUNCT, the first char (multi-char operators like "->"
           have len > 1) */
        char c;
        /* 1 if this token is part of a preprocessor line */
        char inPreproc;
        /* 1 if comment/string is unterminated at end of line */
        char open;
    } Token;


typedef struct TokenList {
        Token  *tokens;
        int     num;
        int     cap;
    } TokenList;


void syn_initTokenList( TokenList  *inList );

void syn_freeTokenList( TokenList  *inList );


/* lexes one line.  Returns the state at the start of the next line. */
int syn_lexLine( const char  *inText,
                 int          inLen,
                 int          inStartState,
                 TokenList   *outTokens );


/* makes sure inTB->lineState is valid through inLine, and returns the
   state at the start of inLine. */
int syn_getLineState( TextBuffer  *inTB,
                      int          inLine );



/* ---- highlighting ---- */

/* highlight classes (indices into the color table, see config.h) */
enum HighlightClass {
    HL_TEXT = 0,
    HL_KEYWORD,
    HL_TYPE,
    HL_PREPROC,
    HL_COMMENT,
    HL_STRING,
    HL_CHAR,
    HL_NUMBER,
    HL_OPERATOR,
    HL_BRACKET,
    HL_FUNC_DEF,
    HL_FUNC_CALL,
    HL_CONSTANT,
    HL_VAR_DECL,
    HL_ESCAPE,
    HL_COUNT
    };


/* fills outClasses[ 0 .. inLen - 1 ] with a highlight class per byte */
void syn_highlightLine( TextBuffer     *inTB,
                        int             inLine,
                        unsigned char  *outClasses );



/* ---- type names ---- */

/* 1 if the identifier is a known type name (builtin like size_t, user
   configured, or found in a typedef in an open buffer) */
int syn_isTypeName( const char  *inName,
                    int          inLen );

void syn_addTypeName( const char  *inName,
                      int          inLen );

/* clears types found by scanning (keeps builtin/configured ones) */
void syn_clearScannedTypes( void );

/* finds typedef names in the buffer and adds them */
void syn_scanTypedefs( TextBuffer  *inTB );

/* adds a space separated list of type names as "configured" types */
void syn_addConfiguredTypes( const char  *inList );


/* 1 if token is a keyword that names (part of) a type:  char int void etc,
   or an identifier that is a known type name */
int syn_tokenIsType( const char  *inLineText,
                     Token       *inToken );

int syn_isIdentChar( char  inC );


#endif
