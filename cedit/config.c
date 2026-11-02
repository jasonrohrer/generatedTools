/*
  config.c

  See config.h
*/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


Config  config;


static char  lastWarning[ 512 ]  =  "";


#define CT_INT    1
#define CT_STR    2
#define CT_COLOR  3
/* section header in the written default file, not a setting */
#define CT_HEADER 4

/* which default file a key is written to */
#define CF_COLORS 1
#define CF_STYLE  2
#define CF_PROJ   3


typedef struct ConfigEntry {
        const char  *key;
        int          type;
        int          file;
        void        *ptr;
        int          size;
        const char  *help;
    } ConfigEntry;


#define SC( x )  ( &( config.syntaxColor[ x ] ) )
#define UC( x )  ( &( config.uiColor[ x ] ) )


static ConfigEntry  entries[]  =  {

    { "Syntax colors", CT_HEADER, CF_COLORS, NULL, 0,
      "Colors are #RRGGBB or #RRGGBBAA hex." },
    { "color.text", CT_COLOR, CF_COLORS, SC( HL_TEXT ), 0,
      "plain identifiers and anything not covered below" },
    { "color.keyword", CT_COLOR, CF_COLORS, SC( HL_KEYWORD ), 0,
      "if else while for return static struct typedef ..." },
    { "color.type", CT_COLOR, CF_COLORS, SC( HL_TYPE ), 0,
      "int char void ..., typedef names, struct tags" },
    { "color.preprocessor", CT_COLOR, CF_COLORS, SC( HL_PREPROC ), 0,
      "#include #define #ifdef ..." },
    { "color.comment", CT_COLOR, CF_COLORS, SC( HL_COMMENT ), 0, "" },
    { "color.string", CT_COLOR, CF_COLORS, SC( HL_STRING ), 0,
      "string literals and <file.h> include paths" },
    { "color.char", CT_COLOR, CF_COLORS, SC( HL_CHAR ), 0,
      "character literals" },
    { "color.escape", CT_COLOR, CF_COLORS, SC( HL_ESCAPE ), 0,
      "escape sequences like \\n inside strings and chars" },
    { "color.number", CT_COLOR, CF_COLORS, SC( HL_NUMBER ), 0, "" },
    { "color.operator", CT_COLOR, CF_COLORS, SC( HL_OPERATOR ), 0,
      "+ - * / = ; , -> etc." },
    { "color.bracket", CT_COLOR, CF_COLORS, SC( HL_BRACKET ), 0,
      "( ) [ ] { }" },
    { "color.functionDef", CT_COLOR, CF_COLORS, SC( HL_FUNC_DEF ), 0,
      "name of a function being defined or declared" },
    { "color.functionCall", CT_COLOR, CF_COLORS, SC( HL_FUNC_CALL ), 0,
      "name of a function being called" },
    { "color.constant", CT_COLOR, CF_COLORS, SC( HL_CONSTANT ), 0,
      "ALL_CAPS names (macros, enum values) and NULL" },
    { "color.variableDecl", CT_COLOR, CF_COLORS, SC( HL_VAR_DECL ), 0,
      "name of a variable being declared, #define'd names" },

    { "Editor colors", CT_HEADER, CF_COLORS, NULL, 0, "" },
    { "color.background", CT_COLOR, CF_COLORS, UC( UI_BACKGROUND ), 0, "" },
    { "color.cursor", CT_COLOR, CF_COLORS, UC( UI_CURSOR ), 0, "" },
    { "color.cursorText", CT_COLOR, CF_COLORS, UC( UI_CURSOR_TEXT ), 0,
      "character under the block cursor" },
    { "color.selection", CT_COLOR, CF_COLORS, UC( UI_SELECTION ), 0,
      "the region between mark and cursor" },
    { "color.currentLine", CT_COLOR, CF_COLORS, UC( UI_CURRENT_LINE ), 0, "" },
    { "color.currentColumn", CT_COLOR, CF_COLORS, UC( UI_CURRENT_COLUMN ), 0,
      "" },
    { "color.gutter", CT_COLOR, CF_COLORS, UC( UI_GUTTER_BG ), 0,
      "line number gutter background" },
    { "color.lineNumber", CT_COLOR, CF_COLORS, UC( UI_LINE_NUMBER ), 0, "" },
    { "color.lineNumberCurrent", CT_COLOR, CF_COLORS,
      UC( UI_LINE_NUMBER_CURRENT ), 0, "" },
    { "color.ruler", CT_COLOR, CF_COLORS, UC( UI_RULER ), 0,
      "vertical line at fillColumn" },
    { "color.trailingSpace", CT_COLOR, CF_COLORS, UC( UI_TRAILING_SPACE ), 0,
      "" },
    { "color.matchParen", CT_COLOR, CF_COLORS, UC( UI_MATCH_PAREN ), 0,
      "background of the bracket matching the one at the cursor" },
    { "color.mismatchParen", CT_COLOR, CF_COLORS, UC( UI_MISMATCH_PAREN ), 0,
      "" },
    { "color.mark", CT_COLOR, CF_COLORS, UC( UI_MARK ), 0,
      "position of the mark" },
    { "color.searchMatch", CT_COLOR, CF_COLORS, UC( UI_SEARCH_MATCH ), 0, "" },
    { "color.searchCurrent", CT_COLOR, CF_COLORS, UC( UI_SEARCH_CURRENT ), 0,
      "" },
    { "color.errorLine", CT_COLOR, CF_COLORS, UC( UI_ERROR_LINE ), 0,
      "source line of the current compiler error" },
    { "color.statusBg", CT_COLOR, CF_COLORS, UC( UI_STATUS_BG ), 0, "" },
    { "color.statusFg", CT_COLOR, CF_COLORS, UC( UI_STATUS_FG ), 0, "" },
    { "color.statusModified", CT_COLOR, CF_COLORS, UC( UI_STATUS_MODIFIED ), 0,
      "" },
    { "color.minibufferBg", CT_COLOR, CF_COLORS, UC( UI_MINIBUF_BG ), 0, "" },
    { "color.minibufferFg", CT_COLOR, CF_COLORS, UC( UI_MINIBUF_FG ), 0, "" },
    { "color.prompt", CT_COLOR, CF_COLORS, UC( UI_PROMPT ), 0, "" },
    { "color.messageError", CT_COLOR, CF_COLORS, UC( UI_MESSAGE_ERROR ), 0,
      "" },
    { "color.tabBg", CT_COLOR, CF_COLORS, UC( UI_TAB_BG ), 0,
      "buffer tab bar" },
    { "color.tabFg", CT_COLOR, CF_COLORS, UC( UI_TAB_FG ), 0, "" },
    { "color.tabActiveBg", CT_COLOR, CF_COLORS, UC( UI_TAB_ACTIVE_BG ), 0, "" },
    { "color.tabActiveFg", CT_COLOR, CF_COLORS, UC( UI_TAB_ACTIVE_FG ), 0, "" },
    { "color.compileBg", CT_COLOR, CF_COLORS, UC( UI_COMPILE_BG ), 0,
      "compile output pane" },
    { "color.compileFg", CT_COLOR, CF_COLORS, UC( UI_COMPILE_FG ), 0, "" },
    { "color.compileError", CT_COLOR, CF_COLORS, UC( UI_COMPILE_ERROR ), 0,
      "" },
    { "color.compileWarning", CT_COLOR, CF_COLORS, UC( UI_COMPILE_WARNING ), 0,
      "" },
    { "color.compileCurrent", CT_COLOR, CF_COLORS, UC( UI_COMPILE_CURRENT ), 0,
      "background of the error currently being visited" },
    { "color.listBg", CT_COLOR, CF_COLORS, UC( UI_LIST_BG ), 0,
      "completion lists (files, buffers, commands)" },
    { "color.listFg", CT_COLOR, CF_COLORS, UC( UI_LIST_FG ), 0, "" },
    { "color.listSelected", CT_COLOR, CF_COLORS, UC( UI_LIST_SELECTED ), 0,
      "" },
    { "color.scrollBar", CT_COLOR, CF_COLORS, UC( UI_SCROLLBAR_BG ), 0,
      "scroll bar track" },
    { "color.scrollBarThumb", CT_COLOR, CF_COLORS, UC( UI_SCROLLBAR_THUMB ),
      0, "" },


    { "Display", CT_HEADER, CF_STYLE, NULL, 0, "" },
    { "font", CT_STR, CF_STYLE, config.fontPath, sizeof( config.fontPath ),
      "path to a monospace .ttf;  empty = built-in Cousine" },
    { "fontSize", CT_INT, CF_STYLE, &config.fontSize, 0,
      "pixels;  also changeable live with Ctrl+= / Ctrl+-" },
    { "lineSpacing", CT_INT, CF_STYLE, &config.lineSpacing, 0,
      "extra pixels between lines" },
    { "windowCols", CT_INT, CF_STYLE, &config.windowCols, 0,
      "initial window size:  columns of text (not counting the line\n"
      "numbers and scroll bar), so fillColumn + 1 fits exactly" },
    { "windowRows", CT_INT, CF_STYLE, &config.windowRows, 0, "" },
    { "showLineNumbers", CT_INT, CF_STYLE, &config.showLineNumbers, 0, "" },
    { "highlightCurrentLine", CT_INT, CF_STYLE,
      &config.highlightCurrentLine, 0, "" },
    { "highlightCurrentColumn", CT_INT, CF_STYLE,
      &config.highlightCurrentColumn, 0, "" },
    { "fillColumn", CT_INT, CF_STYLE, &config.fillColumn, 0,
      "column for the ruler line" },
    { "showRuler", CT_INT, CF_STYLE, &config.showRuler, 0, "" },
    { "showScrollBar", CT_INT, CF_STYLE, &config.showScrollBar, 0,
      "vertical scroll bar at the right edge (drag it, or click to jump)" },
    { "showTrailingSpace", CT_INT, CF_STYLE, &config.showTrailingSpace, 0,
      "" },
    { "smoothScroll", CT_INT, CF_STYLE, &config.smoothScroll, 0,
      "animate scrolling" },
    { "scrollMargin", CT_INT, CF_STYLE, &config.scrollMargin, 0,
      "lines kept visible above/below the cursor" },


    { "Indentation", CT_HEADER, CF_STYLE, NULL, 0,
      "Only spaces are ever inserted.  Defaults are a Ratliff-like\n"
      "style (closing brace indented to the level of the block body):\n"
      "\n"
      "   static int foo( int  inX ) {\n"
      "       if( inX > 0 ) {\n"
      "           return 1;\n"
      "           }\n"
      "       else {\n"
      "           return 0;\n"
      "           }\n"
      "       }\n"
      "\n"
      "For K&R style set closeBraceOffset = 0." },
    { "tabWidth", CT_INT, CF_STYLE, &config.tabWidth, 0,
      "tabs in loaded files are expanded to spaces using this width" },
    { "indentSize", CT_INT, CF_STYLE, &config.indentSize, 0,
      "block body indent" },
    { "structIndent", CT_INT, CF_STYLE, &config.structIndent, 0,
      "members of a struct/union body, relative to the struct line" },
    { "closeBraceOffset", CT_INT, CF_STYLE, &config.closeBraceOffset, 0,
      "closing } relative to the statement that opened the block\n"
      "(4 = Ratliff, 0 = K&R / Allman)" },
    { "openBraceOffset", CT_INT, CF_STYLE, &config.openBraceOffset, 0,
      "a { on its own line, relative to the if/while/function header\n"
      "(0 = Allman, 2 = GNU)" },
    { "caseIndent", CT_INT, CF_STYLE, &config.caseIndent, 0,
      "case labels relative to the switch statement" },
    { "caseBodyIndent", CT_INT, CF_STYLE, &config.caseBodyIndent, 0,
      "statements under a case, relative to the case label" },
    { "substatementIndent", CT_INT, CF_STYLE, &config.substatementIndent, 0,
      "body of an if/while/for/else without braces" },
    { "continuationIndent", CT_INT, CF_STYLE, &config.continuationIndent, 0,
      "continuation lines of a statement split outside of parens" },
    { "alignAssignments", CT_INT, CF_STYLE, &config.alignAssignments, 0,
      "1 = continuation lines of  x = a  line up with the a:\n"
      "   glowW = mainSprite->w\n"
      "           + 2 * glowBorder;" },
    { "alignArgs", CT_INT, CF_STYLE, &config.alignArgs, 0,
      "1 = line up continued arguments with the first argument after (" },
    { "argIndent", CT_INT, CF_STYLE, &config.argIndent, 0,
      "indent of arguments when ( ends the line" },
    { "alignCloseParen", CT_INT, CF_STYLE, &config.alignCloseParen, 0,
      "1 = a ) starting a line lines up with the arguments,\n"
      "0 = with the line that has the (" },
    { "alignComments", CT_INT, CF_STYLE, &config.alignComments, 0,
      "1 = continuation lines of a /* comment line up with its text" },


    { "Electric keys", CT_HEADER, CF_STYLE, NULL, 0, "" },
    { "autoNewline", CT_INT, CF_STYLE, &config.autoNewline, 0,
      "newline+indent automatically after typing { } and ;\n"
      "(not inside parens, like emacs auto-newline)" },
    { "hungryDelete", CT_INT, CF_STYLE, &config.hungryDelete, 0,
      "backspace/delete eat all whitespace (including newlines) at once;\n"
      "Ctrl+Backspace is unaffected" },
    { "reindentOnEnter", CT_INT, CF_STYLE, &config.reindentOnEnter, 0,
      "Enter re-indents the line being left, as well as the new line" },
    { "electricChars", CT_INT, CF_STYLE, &config.electricChars, 0,
      "re-indent the line when typing { } : # and else" },
    { "cleanupBraceSemi", CT_INT, CF_STYLE, &config.cleanupBraceSemi, 0,
      "typing ; on an empty line right after } joins it up:  };" },


    { "Auto formatting", CT_HEADER, CF_STYLE, NULL, 0,
      "Applied to a line when you leave it with Enter (or auto-newline),\n"
      "and by the format-region / format-buffer commands.\n"
      "Strings and comments are never touched." },
    { "formatOnEnter", CT_INT, CF_STYLE, &config.formatOnEnter, 0,
      "master switch for formatting as you type" },
    { "parenPadding", CT_INT, CF_STYLE, &config.parenPadding, 0,
      "foo(a, b)  ->  foo( a, b )     (empty () left alone)" },
    { "bracketPadding", CT_INT, CF_STYLE, &config.bracketPadding, 0,
      "a[i]  ->  a[ i ]" },
    { "castPadding", CT_INT, CF_STYLE, &config.castPadding, 0,
      "0 = casts stay tight:  (int)x   1 = ( int )x" },
    { "commaSpace", CT_INT, CF_STYLE, &config.commaSpace, 0,
      "a,b  ->  a, b" },
    { "paramTypeSpaces", CT_INT, CF_STYLE, &config.paramTypeSpaces, 0,
      "spaces between type and name in function parameter lists\n"
      "(0 = leave alone):   int foo( int  inX, char  *inS )" },
    { "alignDeclarations", CT_INT, CF_STYLE, &config.alignDeclarations, 0,
      "line up consecutive variable declarations into columns:\n"
      "   Deck  *d      =  getDeck();\n"
      "   int    i;\n"
      "   long   total  =  0;" },
    { "declTypeSpaces", CT_INT, CF_STYLE, &config.declTypeSpaces, 0,
      "minimum spaces between type column and name column" },
    { "declEqualsSpaces", CT_INT, CF_STYLE, &config.declEqualsSpaces, 0,
      "spaces on each side of = in aligned declarations" },
    { "declTableBlankLines", CT_INT, CF_STYLE, &config.declTableBlankLines,
      0, "a declaration table continues across this many blank lines\n"
      "(1:  one blank line groups declarations within a table, two\n"
      "start a new table;  0:  any blank line ends the table)" },
    { "assignSpaces", CT_INT, CF_STYLE, &config.assignSpaces, 0,
      "1 = at least one space on each side of = += -= etc:  x = 6;" },
    { "compareSpaces", CT_INT, CF_STYLE, &config.compareSpaces, 0,
      "1 = at least one space on each side of == != < > <= >= && ||:\n"
      "    if( x == 5 ) {" },
    { "splitBoolOps", CT_INT, CF_STYLE, &config.splitBoolOps, 0,
      "1 = the operands of && and || inside ( ) (and after return) go\n"
      "on their own lines, with the operator on a line by itself:\n"
      "    if( x == 10\n"
      "        &&\n"
      "        ( y > 20\n"
      "          ||\n"
      "          z ) ) {" },
    { "spaceBeforeBrace", CT_INT, CF_STYLE, &config.spaceBeforeBrace, 0,
      "1 = a space between ) or else and a block's {:  while( 1 ) {" },
    { "splitArgLists", CT_INT, CF_STYLE, &config.splitArgLists, 0,
      "1 = function arguments and parameters go one per line, lined\n"
      "up after the (:\n"
      "    x = myFunction( 10,\n"
      "                    &currentStruct );" },
    { "splitForClauses", CT_INT, CF_STYLE, &config.splitForClauses, 0,
      "1 = the three clauses of a for( ) go on their own lines" },
    { "alignForClauses", CT_INT, CF_STYLE, &config.alignForClauses, 0,
      "1 = ... lined up as a table, ++ / -- in the value column:\n"
      "    for( x = 0;\n"
      "         x < 10;\n"
      "         x   ++ ) {" },


    { "Compile and debug", CT_HEADER, CF_PROJ, NULL, 0,
      "Usually set per-folder in ./cedit.conf\n"
      "In debugCommand:  %f = file name, %p = full file path,\n"
      "%l = line number, %e = debugProgram, %% = a percent sign." },
    { "compileCommand", CT_STR, CF_PROJ, config.compileCommand,
      sizeof( config.compileCommand ), "run through /bin/sh" },
    { "saveBeforeCompile", CT_INT, CF_PROJ, &config.saveBeforeCompile, 0,
      "save all modified buffers before compiling" },
    { "debugProgram", CT_STR, CF_PROJ, config.debugProgram,
      sizeof( config.debugProgram ), "" },
    { "debugCommand", CT_STR, CF_PROJ, config.debugCommand,
      sizeof( config.debugCommand ),
      "launched in the background;  example:\n"
      "debugCommand = ddd --debugger 'gdb -ex \"break %f:%l\" -ex run' %e" },


    { "Types", CT_HEADER, CF_STYLE, NULL, 0, "" },
    { "extraTypes", CT_STR, CF_STYLE, config.extraTypes,
      sizeof( config.extraTypes ),
      "space separated names to always highlight as types\n"
      "(typedefs in open files are found automatically)" },

    { NULL, 0, 0, NULL, 0, NULL }
    };



static unsigned int rgb( unsigned int  inHex ) {
    unsigned int  r  =  ( inHex >> 16 ) & 0xFF;
    unsigned int  g  =  ( inHex >> 8 ) & 0xFF;
    unsigned int  b  =  inHex & 0xFF;

    return 0xFF000000u | ( b << 16 ) | ( g << 8 ) | r;
    }



static void setDefaults( void ) {
    unsigned int  *s  =  config.syntaxColor;
    unsigned int  *u  =  config.uiColor;

    memset( &config, 0, sizeof( config ) );

    /* colors match a classic emacs font-lock setup on a light yellow
       background */
    s[ HL_TEXT ]      = rgb( 0x000000 );
    s[ HL_KEYWORD ]   = rgb( 0xC88000 );
    s[ HL_TYPE ]      = rgb( 0x008080 );
    s[ HL_PREPROC ]   = rgb( 0x483D8B );
    s[ HL_COMMENT ]   = rgb( 0x00AA00 );
    s[ HL_STRING ]    = rgb( 0xFF00FF );
    s[ HL_CHAR ]      = rgb( 0xFF00FF );
    s[ HL_ESCAPE ]    = rgb( 0xB000B0 );
    s[ HL_NUMBER ]    = rgb( 0x000000 );
    s[ HL_OPERATOR ]  = rgb( 0x000000 );
    s[ HL_BRACKET ]   = rgb( 0x000000 );
    s[ HL_FUNC_DEF ]  = rgb( 0xC80000 );
    s[ HL_FUNC_CALL ] = rgb( 0x000000 );
    s[ HL_CONSTANT ]  = rgb( 0xC800FF );
    s[ HL_VAR_DECL ]  = rgb( 0x0000FF );

    u[ UI_BACKGROUND ]          = rgb( 0xFFFFDC );
    u[ UI_CURSOR ]              = rgb( 0x000000 );
    u[ UI_CURSOR_TEXT ]         = rgb( 0xFFFFDC );
    u[ UI_SELECTION ]           = rgb( 0xEEDC82 );
    u[ UI_CURRENT_LINE ]        = rgb( 0xEEEEFF );
    u[ UI_CURRENT_COLUMN ]      = rgb( 0xEEEEFF );
    u[ UI_GUTTER_BG ]           = rgb( 0xF2F2D2 );
    u[ UI_LINE_NUMBER ]         = rgb( 0xA8A890 );
    u[ UI_LINE_NUMBER_CURRENT ] = rgb( 0x404030 );
    u[ UI_RULER ]               = rgb( 0xE6E6C4 );
    u[ UI_TRAILING_SPACE ]      = rgb( 0xFFD8D8 );
    u[ UI_MATCH_PAREN ]         = rgb( 0x40E0D0 );
    u[ UI_MISMATCH_PAREN ]      = rgb( 0xFF6060 );
    u[ UI_MARK ]                = rgb( 0xFFE2A8 );
    u[ UI_SEARCH_MATCH ]        = rgb( 0xAFEEEE );
    u[ UI_SEARCH_CURRENT ]      = rgb( 0xFF80FF );
    u[ UI_ERROR_LINE ]          = rgb( 0xFFD0D0 );
    u[ UI_STATUS_BG ]           = rgb( 0xBFBFBF );
    u[ UI_STATUS_FG ]           = rgb( 0x000000 );
    u[ UI_STATUS_MODIFIED ]     = rgb( 0xB00000 );
    u[ UI_MINIBUF_BG ]          = rgb( 0xFFFFDC );
    u[ UI_MINIBUF_FG ]          = rgb( 0x000000 );
    u[ UI_PROMPT ]              = rgb( 0x0000C0 );
    u[ UI_MESSAGE_ERROR ]       = rgb( 0xD00000 );
    u[ UI_TAB_BG ]              = rgb( 0xD8D8C0 );
    u[ UI_TAB_FG ]              = rgb( 0x505040 );
    u[ UI_TAB_ACTIVE_BG ]       = rgb( 0xFFFFDC );
    u[ UI_TAB_ACTIVE_FG ]       = rgb( 0x000000 );
    u[ UI_COMPILE_BG ]          = rgb( 0xF8F8E8 );
    u[ UI_COMPILE_FG ]          = rgb( 0x202020 );
    u[ UI_COMPILE_ERROR ]       = rgb( 0xD00000 );
    u[ UI_COMPILE_WARNING ]     = rgb( 0xB06000 );
    u[ UI_COMPILE_CURRENT ]     = rgb( 0xFFE2A8 );
    u[ UI_LIST_BG ]             = rgb( 0xF0F0D8 );
    u[ UI_LIST_FG ]             = rgb( 0x000000 );
    u[ UI_LIST_SELECTED ]       = rgb( 0xEEDC82 );
    u[ UI_SCROLLBAR_BG ]        = rgb( 0xF2F2D2 );
    u[ UI_SCROLLBAR_THUMB ]     = rgb( 0xB4B49A );

    config.fontPath[ 0 ] = '\0';
    config.fontSize = 16;
    config.lineSpacing = 1;
    config.windowCols = 81;
    config.windowRows = 40;
    config.showLineNumbers = 1;
    config.highlightCurrentLine = 1;
    config.highlightCurrentColumn = 1;
    config.fillColumn = 80;
    config.showScrollBar = 1;
    config.showRuler = 1;
    config.showTrailingSpace = 1;
    config.smoothScroll = 1;
    config.scrollMargin = 3;

    config.tabWidth = 4;
    config.indentSize = 4;
    config.structIndent = 8;
    config.closeBraceOffset = 4;
    config.openBraceOffset = 0;
    config.caseIndent = 4;
    config.caseBodyIndent = 4;
    config.substatementIndent = 4;
    config.continuationIndent = 4;
    config.alignAssignments = 1;
    config.alignArgs = 1;
    config.argIndent = 4;
    config.alignCloseParen = 1;
    config.alignComments = 1;

    config.autoNewline = 1;
    config.hungryDelete = 1;
    config.reindentOnEnter = 1;
    config.electricChars = 1;
    config.cleanupBraceSemi = 1;

    config.formatOnEnter = 1;
    config.parenPadding = 1;
    config.bracketPadding = 1;
    config.castPadding = 0;
    config.commaSpace = 1;
    config.paramTypeSpaces = 2;
    config.alignDeclarations = 1;
    config.declTypeSpaces = 2;
    config.declEqualsSpaces = 2;
    config.declTableBlankLines = 1;
    config.assignSpaces = 1;
    config.compareSpaces = 1;
    config.splitBoolOps = 1;
    config.spaceBeforeBrace = 1;
    config.splitArgLists = 1;
    config.splitForClauses = 1;
    config.alignForClauses = 1;

    strcpy( config.compileCommand, "make" );
    config.debugCommand[ 0 ] = '\0';
    config.debugProgram[ 0 ] = '\0';
    config.saveBeforeCompile = 1;

    config.extraTypes[ 0 ] = '\0';
    }



const char *config_homeDir( void ) {
    static char  dir[ 512 ];
    const char  *home  =  getenv( "HOME" );

    if( home == NULL ) {
        home = ".";
        }
    sprintf( dir, "%.490s/.cedit", home );
    return dir;
    }



const char *config_lastWarning( void ) {
    return lastWarning;
    }



static int parseHexDigit( char  inC ) {
    if( inC >= '0' && inC <= '9' ) {
        return inC - '0';
        }
    if( inC >= 'a' && inC <= 'f' ) {
        return inC - 'a' + 10;
        }
    if( inC >= 'A' && inC <= 'F' ) {
        return inC - 'A' + 10;
        }
    return -1;
    }



static int parseColor( const char    *inValue,
                       unsigned int  *outColor ) {
    unsigned int  v      =  0;
    int           n      =  0;
    const char   *p      =  inValue;
    unsigned int  alpha  =  0xFF;

    if( *p == '#' ) {
        p ++;
        }
    while( *p != '\0' && parseHexDigit( *p ) >= 0 && n < 8 ) {
        v = ( v << 4 ) | (unsigned int)parseHexDigit( *p );
        p ++;
        n ++;
        }
    if( n == 8 ) {
        alpha = v & 0xFF;
        v >>= 8;
        }
    else if( n != 6 ) {
        return 0;
        }
    *outColor = ( alpha << 24 ) | ( rgb( v ) & 0x00FFFFFF );
    return 1;
    }



int config_set( const char  *inKey,
                const char  *inValue ) {
    int  i;

    for( i = 0;
         entries[ i ].key != NULL;
         i ++ ) {
        ConfigEntry  *e  =  &( entries[ i ] );

        if( e->type == CT_HEADER || strcmp( e->key, inKey ) != 0 ) {
            continue;
            }
        if( e->type == CT_INT ) {
            *( (int *)e->ptr ) = atoi( inValue );
            }
        else if( e->type == CT_STR ) {
            strncpy( (char *)e->ptr, inValue, (size_t)e->size - 1 );
            ( (char *)e->ptr )[ e->size - 1 ] = '\0';
            }
        else if( e->type == CT_COLOR ) {
            if( ! parseColor( inValue, (unsigned int *)e->ptr ) ) {
                sprintf( lastWarning, "bad color for %.100s:  %.100s",
                         inKey, inValue );
                }
            }
        return 1;
        }
    return 0;
    }



static char *trim( char  *inS ) {
    char  *e;

    while( *inS == ' ' || *inS == '\t' ) {
        inS ++;
        }
    e = inS + strlen( inS );
    while( e > inS
           &&
           ( e[ -1 ] == ' ' || e[ -1 ] == '\t' || e[ -1 ] == '\n'
             || e[ -1 ] == '\r' ) ) {
        e --;
        }
    *e = '\0';
    return inS;
    }



int config_loadFile( const char  *inPath ) {
    FILE  *f  =  fopen( inPath, "r" );
    char   line[ 4096 ];
    int    lineNum  =  0;

    if( f == NULL ) {
        return 0;
        }
    while( fgets( line, sizeof( line ), f ) != NULL ) {
        char  *t  =  trim( line );
        char  *eq;
        char  *key;
        char  *val;

        lineNum ++;
        if( t[ 0 ] == '\0' || t[ 0 ] == '#' ) {
            continue;
            }
        eq = strchr( t, '=' );
        if( eq == NULL ) {
            sprintf( lastWarning, "%.300s:%d: expected key = value",
                     inPath, lineNum );
            continue;
            }
        *eq = '\0';
        key = trim( t );
        val = trim( eq + 1 );
        if( ! config_set( key, val ) ) {
            sprintf( lastWarning, "%.300s:%d: unknown setting %.100s",
                     inPath, lineNum, key );
            }
        }
    fclose( f );
    return 1;
    }



void config_load( void ) {
    char         path[ 600 ];
    const char  *dir  =  config_homeDir();

    setDefaults();
    lastWarning[ 0 ] = '\0';

    sprintf( path, "%s/colors.conf", dir );
    config_loadFile( path );
    sprintf( path, "%s/style.conf", dir );
    config_loadFile( path );
    sprintf( path, "%s/cedit.conf", dir );
    config_loadFile( path );
    config_loadFile( "cedit.conf" );

    if( config.fontSize < 6 ) {
        config.fontSize = 6;
        }
    if( config.fontSize > 96 ) {
        config.fontSize = 96;
        }
    if( config.tabWidth < 1 ) {
        config.tabWidth = 4;
        }
    if( config.indentSize < 0 ) {
        config.indentSize = 4;
        config.structIndent = 8;
        }

    syn_clearScannedTypes();
    syn_addConfiguredTypes( config.extraTypes );
    }



static void writeHelp( FILE        *inF,
                       const char  *inHelp ) {
    const char  *p  =  inHelp;

    if( p == NULL || p[ 0 ] == '\0' ) {
        return;
        }
    fputs( "# ", inF );
    while( *p != '\0' ) {
        fputc( *p, inF );
        if( *p == '\n' ) {
            fputs( "# ", inF );
            }
        p ++;
        }
    fputc( '\n', inF );
    }



/* returns 1 on success */
static int writeFile( const char  *inPath,
                      int          inWhich,
                      const char  *inTitle ) {
    FILE  *f  =  fopen( inPath, "w" );
    int    i;

    if( f == NULL ) {
        return 0;
        }
    fprintf( f, "# %s\n#\n# key = value.  Lines starting with # are "
             "comments.\n# See also ~/.cedit/*.conf and ./cedit.conf "
             "(per folder).\n", inTitle );

    for( i = 0;
         entries[ i ].key != NULL;
         i ++ ) {
        ConfigEntry  *e  =  &( entries[ i ] );

        if( e->file != inWhich ) {
            continue;
            }
        if( e->type == CT_HEADER ) {
            fprintf( f, "\n\n# ---- %s ----\n", e->key );
            writeHelp( f, e->help );
            fputc( '\n', f );
            continue;
            }
        writeHelp( f, e->help );
        if( e->type == CT_INT ) {
            fprintf( f, "%s = %d\n", e->key, *( (int *)e->ptr ) );
            }
        else if( e->type == CT_STR ) {
            fprintf( f, "%s = %s\n", e->key, (char *)e->ptr );
            }
        else {
            unsigned int  c  =  *( (unsigned int *)e->ptr );

            fprintf( f, "%s = #%02X%02X%02X", e->key,
                     c & 0xFF, ( c >> 8 ) & 0xFF, ( c >> 16 ) & 0xFF );
            if( ( c >> 24 ) != 0xFF ) {
                fprintf( f, "%02X", c >> 24 );
                }
            fputc( '\n', f );
            }
        fputc( '\n', f );
        }
    return fclose( f ) == 0;
    }



void config_writeDefaults( const char  *inDir,
                           char        *outMessage,
                           int          inMessageSize ) {
    char         path[ 600 ];
    char         msg[ 1400 ];
    struct stat  st;
    int          i;
    const char  *names[ 3 ]  =  { "colors.conf", "style.conf",
                                  "cedit.conf.example" };
    int          which[ 3 ]  =  { CF_COLORS, CF_STYLE, CF_PROJ };
    const char  *titles[ 3 ] =  { "cedit colors", "cedit style",
                                  "cedit per-folder settings "
                                  "(copy to ./cedit.conf)" };

    mkdir( inDir, 0755 );
    msg[ 0 ] = '\0';

    for( i = 0;
         i < 3;
         i ++ ) {
        sprintf( path, "%.500s/%s", inDir, names[ i ] );
        if( stat( path, &st ) == 0 ) {
            sprintf( msg + strlen( msg ), "%.300s exists (kept).  ", path );
            }
        else if( writeFile( path, which[ i ], titles[ i ] ) ) {
            sprintf( msg + strlen( msg ), "wrote %.300s  ", path );
            }
        else {
            sprintf( msg + strlen( msg ), "FAILED to write %.300s  ", path );
            }
        }
    strncpy( outMessage, msg, (size_t)inMessageSize - 1 );
    outMessage[ inMessageSize - 1 ] = '\0';
    }
