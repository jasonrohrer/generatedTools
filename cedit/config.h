/*
  config.h

  All user-tweakable settings:  colors, indentation style, auto-format
  rules, compile and debug commands.

  Every setting is a "key = value" line.  Lines starting with '#' are
  comments.  Files are read in this order, later ones overriding earlier:

     built-in defaults
     ~/.cedit/colors.conf
     ~/.cedit/style.conf
     ~/.cedit/cedit.conf
     ./cedit.conf           (per-folder:  compile/debug commands, etc.)

  Any key may appear in any file.  Run "cedit --write-config" (or the
  write-default-config command) to get fully commented copies of
  colors.conf and style.conf in ~/.cedit/ to start from.

  Pure C89, no SDL.
*/

#ifndef CEDIT_CONFIG_H
#define CEDIT_CONFIG_H

#include "syntax.h"


/* UI colors, in addition to the HL_* syntax classes */
enum UIColor {
    UI_BACKGROUND = 0,
    UI_CURSOR,
    UI_CURSOR_TEXT,
    UI_SELECTION,
    UI_CURRENT_LINE,
    UI_CURRENT_COLUMN,
    UI_GUTTER_BG,
    UI_LINE_NUMBER,
    UI_LINE_NUMBER_CURRENT,
    UI_RULER,
    UI_TRAILING_SPACE,
    UI_MATCH_PAREN,
    UI_MISMATCH_PAREN,
    UI_MARK,
    UI_SEARCH_MATCH,
    UI_SEARCH_CURRENT,
    UI_ERROR_LINE,
    UI_STATUS_BG,
    UI_STATUS_FG,
    UI_STATUS_MODIFIED,
    UI_MINIBUF_BG,
    UI_MINIBUF_FG,
    UI_PROMPT,
    UI_MESSAGE_ERROR,
    UI_TAB_BG,
    UI_TAB_FG,
    UI_TAB_ACTIVE_BG,
    UI_TAB_ACTIVE_FG,
    UI_COMPILE_BG,
    UI_COMPILE_FG,
    UI_COMPILE_ERROR,
    UI_COMPILE_WARNING,
    UI_COMPILE_CURRENT,
    UI_LIST_BG,
    UI_LIST_FG,
    UI_LIST_SELECTED,
    UI_SCROLLBAR_BG,
    UI_SCROLLBAR_THUMB,
    UI_WRAP_MARK,
    UI_COUNT
    };


typedef struct Config {
        /* colors, packed as 0xAABBGGRR (ImGui's IM_COL32 layout) */
        unsigned int  syntaxColor[ HL_COUNT ];
        unsigned int  uiColor[ UI_COUNT ];

        /* --- display --- */
        char   fontPath[ 512 ];
        int    fontSize;
        int    windowCols;
        int    windowRows;
        int    showLineNumbers;
        int    highlightCurrentLine;
        int    highlightCurrentColumn;
        int    fillColumn;
        int    showRuler;
        int    wrapLines;
        int    showScrollBar;
        int    showTrailingSpace;
        int    smoothScroll;
        int    scrollMargin;
        int    lineSpacing;

        /* --- indentation --- */
        int    tabWidth;
        int    indentSize;
        int    structIndent;
        int    closeBraceOffset;
        int    openBraceOffset;
        int    caseIndent;
        int    caseBodyIndent;
        int    substatementIndent;
        int    continuationIndent;
        int    alignAssignments;
        int    alignArgs;
        int    argIndent;
        int    alignCloseParen;
        int    alignComments;
        int    commentBodyIndent;

        /* --- electric behavior --- */
        int    autoNewline;
        int    hungryDelete;
        int    reindentOnEnter;
        int    electricChars;
        int    cleanupBraceSemi;

        /* --- auto formatting --- */
        int    formatOnEnter;
        int    parenPadding;
        int    bracketPadding;
        int    castPadding;
        int    commaSpace;
        int    paramTypeSpaces;
        int    alignDeclarations;
        int    declTypeSpaces;
        int    declEqualsSpaces;
        int    declStorageSpaces;
        int    declAlignValuesRight;
        int    declTableBlankLines;
        int    assignSpaces;
        int    compareSpaces;
        int    operatorSpaces;
        int    unarySpaces;
        int    splitBoolOps;
        int    spaceBeforeBrace;
        int    splitArgLists;
        int    splitForClauses;
        int    alignForClauses;

        /* --- compile / debug --- */
        char   compileCommand[ 1024 ];
        char   debugCommand[ 1024 ];
        char   debugProgram[ 512 ];
        int    saveBeforeCompile;

        /* --- misc --- */
        char   extraTypes[ 2048 ];
    } Config;


extern Config  config;


/* fills in defaults, then reads the config files */
void config_load( void );

/* reads one file;  returns 1 if it existed */
int config_loadFile( const char  *inPath );

/* sets one key;  returns 1 if key was known */
int config_set( const char  *inKey,
                const char  *inValue );

/* writes commented default colors.conf and style.conf into inDir.
   Existing files are not overwritten.  Fills outMessage. */
void config_writeDefaults( const char  *inDir,
                           char        *outMessage,
                           int          inMessageSize );

/* path to ~/.cedit (static storage) */
const char *config_homeDir( void );

/* last config parse warning, or "" */
const char *config_lastWarning( void );


#endif
