/*
  textBuffer.h

  The document model for cedit:  an array of lines, plus a linear
  undo/redo history, plus "markers" (positions that track edits, used for
  the mark and for compiler-error locations).

  Positions are (line, col) pairs, both 0-based.  col is a byte offset into
  the line.  Lines never contain '\n'.

  All text changes go through tb_insert / tb_delete so that undo history,
  markers, the modified flag, and the syntax-state cache stay consistent.

  Pure C89, no SDL.
*/

#ifndef CEDIT_TEXT_BUFFER_H
#define CEDIT_TEXT_BUFFER_H


/* A line's text is either its own malloc'd block (cap > 0), or, for
   lines straight from a loaded file, a slice of the buffer's shared
   arena (cap < 0, with -cap bytes usable in place).  Arena lines are
   copied out to their own block the first time they need to grow, which
   makes loading a huge file one big read instead of a malloc per line. */
typedef struct Line {
        char  *text;
        int    len;
        int    cap;
    } Line;


#define UNDO_INSERT  1
#define UNDO_DELETE  2

typedef struct UndoRecord {
        int    type;
        int    line;
        int    col;
        char  *text;
        int    len;
        /* records with the same group are undone/redone together */
        int    group;
        /* cursor position before this group started */
        int    cursorLine;
        int    cursorCol;
    } UndoRecord;


typedef struct Marker {
        int  line;
        int  col;
        int  inUse;
    } Marker;


typedef struct TextBuffer {
        Line        *lines;
        int          numLines;
        int          capLines;
        /* shared text block for lines loaded from a file (may be NULL) */
        char        *arena;

        /* full path (as given/opened), or NULL for scratch buffers */
        char        *path;
        /* short display name (file name part, made unique by editor) */
        char        *name;

        int          readOnly;

        UndoRecord  *undo;
        int          numUndo;
        int          capUndo;
        /* number of records currently applied; records past this are redo */
        int          undoPos;
        /* undoPos at last save, or -1 if that state was lost */
        int          savedUndoPos;
        int          nextGroup;
        int          currentGroup;

        Marker      *markers;
        int          numMarkers;

        /* syntax cache:  lineState[ i ] is the lexer state at start of line
           i.  Valid for lines 0..stateValidThrough (inclusive). */
        unsigned char  *lineState;
        int             capLineState;
        int             stateValidThrough;

        /* incremented on every change, for consumers that cache stuff */
        unsigned long   changeCount;

        /* modification time of the file when loaded/saved */
        long   fileMTime;
        /* typedef scan deferred until after the first frame (big files) */
        int    needsTypeScan;

        /* cursor, mark, view (owned by editor, stored here so each buffer
           remembers its own).  The cursor is also tracked by a marker so
           edits elsewhere in the buffer move it correctly;  cursorLine /
           cursorCol are kept in sync by the editor. */
        int    cursorMarker;
        int    cursorLine;
        int    cursorCol;
        /* column the cursor "wants" when moving vertically */
        int    goalCol;
        int    markMarker;
        int    markActive;
        /* the mark has been set at least once */
        int    markSet;
        /* view position, in lines and columns (fractional while smooth
           scrolling) */
        float  scrollY;
        float  scrollX;
        float  targetScrollY;
    } TextBuffer;



TextBuffer *tb_create( void );

void tb_destroy( TextBuffer  *inTB );

/* loads file into a fresh buffer.  Returns NULL if the file can't be read.
   Tabs are expanded to spaces (inTabWidth); *outTabsExpanded gets how many
   tabs were expanded (buffer is marked modified if any were). */
TextBuffer *tb_load( const char  *inPath,
                     int          inTabWidth,
                     int         *outTabsExpanded );

/* returns 1 on success.  Always writes a final newline. */
int tb_save( TextBuffer  *inTB,
             const char  *inPath );

int tb_isModified( TextBuffer  *inTB );

/* replace all contents (no undo) */
void tb_setText( TextBuffer  *inTB,
                 const char  *inText,
                 int          inLen );

/* inserts text (may contain '\n') at position.  Out params (may be NULL)
   get the position just after the inserted text. */
void tb_insert( TextBuffer  *inTB,
                int          inLine,
                int          inCol,
                const char  *inText,
                int          inLen,
                int         *outLine,
                int         *outCol );

/* deletes the range [start, end) */
void tb_delete( TextBuffer  *inTB,
                int          inStartLine,
                int          inStartCol,
                int          inEndLine,
                int          inEndCol );

/* returns a malloc'd copy of the range [start, end), with '\n' between
   lines.  *outLen gets the length (not counting the terminating \0). */
char *tb_getText( TextBuffer  *inTB,
                  int          inStartLine,
                  int          inStartCol,
                  int          inEndLine,
                  int          inEndCol,
                  int         *outLen );

/* clamps a position into the buffer */
void tb_clampPos( TextBuffer  *inTB,
                  int         *ioLine,
                  int         *ioCol );

/* -1, 0, 1 like strcmp */
int tb_comparePos( int  inLineA,
                   int  inColA,
                   int  inLineB,
                   int  inColB );


/* ------- undo ------- */

/* starts a new undo group;  all following edits until the next call
   belong to it.  The cursor position is remembered for restoring on undo. */
void tb_beginGroup( TextBuffer  *inTB );

/* makes following edits join the most recent group (used to coalesce
   runs of typed characters) */
void tb_continueGroup( TextBuffer  *inTB );

/* returns 1 if something was undone;  sets cursor */
int tb_undo( TextBuffer  *inTB );

int tb_redo( TextBuffer  *inTB );

void tb_clearUndo( TextBuffer  *inTB );


/* ------- markers ------- */

/* returns marker id */
int tb_addMarker( TextBuffer  *inTB,
                  int          inLine,
                  int          inCol );

void tb_removeMarker( TextBuffer  *inTB,
                      int          inID );

void tb_setMarker( TextBuffer  *inTB,
                   int          inID,
                   int          inLine,
                   int          inCol );

void tb_getMarker( TextBuffer  *inTB,
                   int          inID,
                   int         *outLine,
                   int         *outCol );



/* ------- small helpers ------- */

/* number of leading spaces on a line */
int tb_lineIndent( TextBuffer  *inTB,
                   int          inLine );

/* 1 if line is empty or only spaces */
int tb_lineIsBlank( TextBuffer  *inTB,
                    int          inLine );

/* changes the leading whitespace of a line to inIndent spaces.
   Returns the change in line length (new - old). */
int tb_setLineIndent( TextBuffer  *inTB,
                      int          inLine,
                      int          inIndent );


#endif
