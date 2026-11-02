/*
  editor.h

  Editor state and commands:  the list of open buffers, the minibuffer
  (prompts with completion), keyboard dispatch (emacs-style bindings),
  kill ring, incremental search, dabbrev completion, compile / next-error
  / debug.

  Rendering lives in cedit.c and only reads this state.
*/

#ifndef CEDIT_EDITOR_H
#define CEDIT_EDITOR_H

#include "textBuffer.h"


/* modifier flags for ed_keyDown */
#define MOD_CTRL   1
#define MOD_ALT    2
#define MOD_SHIFT  4


/* minibuffer modes */
enum MinibufferMode {
    MB_NONE = 0,
    MB_FIND_FILE,
    MB_SWITCH_BUFFER,
    MB_COMMAND,
    MB_GOTO_LINE,
    MB_ISEARCH,
    MB_COMPILE,
    MB_DEBUG_PROGRAM,
    MB_DEBUG_COMMAND,
    MB_SAVE_AS,
    MB_REPLACE_FROM,
    MB_REPLACE_TO,
    MB_QUERY_REPLACE,
    MB_YES_NO
    };


#define MB_TEXT_MAX  1024


typedef struct Editor {
        TextBuffer   **buffers;
        int            numBuffers;
        int            current;
        /* most-recently-used order of buffer indices, for C-x b default */
        int           *mru;

        /* --- minibuffer --- */
        int            mbMode;
        char           mbPrompt[ 256 ];
        char           mbText[ MB_TEXT_MAX ];
        int            mbCursor;
        /* completion candidates (malloc'd strings) */
        char         **cands;
        int            numCands;
        int            capCands;
        int            candSel;
        int            candScroll;
        /* what to do on yes/no */
        int            yesNoAction;

        /* message shown in the minibuffer area when idle */
        char           message[ 1024 ];
        int            messageIsError;

        /* --- prefix keys --- */
        int            prefixCtrlX;
        int            prefixCtrlC;
        int            prefixMetaG;

        /* --- command tracking --- */
        int            lastCommand;
        int            thisCommand;

        /* --- incremental search --- */
        char           searchText[ 256 ];
        char           lastSearch[ 256 ];
        int            searchForward;
        int            searchFailing;
        int            searchStartLine;
        int            searchStartCol;
        /* match currently shown */
        int            matchLine;
        int            matchCol;
        int            matchLen;
        int            searchWrapped;

        /* --- query replace --- */
        char           replaceFrom[ 256 ];
        char           replaceTo[ 256 ];
        int            replaceCount;

        /* --- compile --- */
        TextBuffer    *compileBuffer;
        int            showCompile;
        int            currentError;
        /* line in compile buffer to highlight (-1 none) */
        int            compileHighlightLine;
        /* top line of compile pane view */
        int            compileScroll;
        int            compileFollow;

        /* --- display --- */
        float          fontSize;
        int            wantQuit;
        /* set by commands that need the view to put the cursor in the
           middle of the screen (C-l, jumps) */
        int            recenterRequest;
        /* rows of text visible in the main view, set by renderer */
        int            visibleRows;
        int            visibleCols;
        int            compileRows;

        /* debug program chosen this session */
        char           debugProgram[ 512 ];
        char           debugCommand[ 1024 ];
    } Editor;


extern Editor  ed;


void ed_init( int     inArgc,
              char  **inArgv );

/* inKey is an SDL keycode.  Returns 1 if the key did something (so a
   text-input event for the same keypress should be ignored). */
int ed_keyDown( int  inKey,
                int  inMods );

/* UTF-8 text typed (no modifiers except shift) */
void ed_textInput( const char  *inText );

/* call every frame;  returns 1 if something changed (compile output) */
int ed_tick( void );

TextBuffer *ed_cur( void );

/* mouse:  place cursor (inExtend = shift-click / drag) */
void ed_mouseSetCursor( int  inLine,
                        int  inCol,
                        int  inExtend );

void ed_mouseSelectWord( int  inLine,
                         int  inCol );

/* middle click:  pastes the X11 PRIMARY selection at the position */
void ed_mousePastePrimary( int  inLine,
                           int  inCol );

void ed_switchToBuffer( int  inIndex );

/* jump to compile error shown on line inOutLine of the compile pane */
void ed_visitErrorAtOutputLine( int  inOutLine );

void ed_message( const char  *inFormat,
                 ... );

void ed_errorMessage( const char  *inFormat,
                      ... );

/* region (mark..cursor), if active.  Returns 0 if no active region. */
int ed_getRegion( int  *outStartLine,
                  int  *outStartCol,
                  int  *outEndLine,
                  int  *outEndCol );

/* 1 if any buffer has unsaved changes */
int ed_anyModified( void );

/* request to quit (asks if unsaved buffers) */
void ed_requestQuit( void );

/* for the renderer:  current isearch/query-replace match, or 0 */
int ed_searchHighlight( const char  **outText,
                        int          *outCaseFold );


#endif
