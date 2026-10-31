/*
  indent.h

  C89 auto-indentation engine.

  To indent a line, we back up to a nearby "anchor" (a line that starts at
  column 0 with an identifier, outside of any comment:  in practice the
  start of a top-level function or declaration), then run a small
  syntactic parse forward to the line, tracking a stack of open braces,
  parens and brackets, and the state of the current statement (has it
  started, is it a control header expecting a sub-statement, etc.).
  The first token of the target line plus that parse state determine the
  indentation, using the style settings in config.

  Like emacs cc-mode, indentation is computed relative to the ACTUAL
  indentation of the lines that open the enclosing constructs, so
  indenting one line never requires re-indenting the whole file.

  Pure C89, no SDL.
*/

#ifndef CEDIT_INDENT_H
#define CEDIT_INDENT_H

#include "textBuffer.h"


/* returns the desired indent of the line, or -1 if the line should be left
   alone (e.g. inside a comment whose opener has no text after it, or a
   continued macro line) */
int ind_computeIndent( TextBuffer  *inTB,
                       int          inLine );


/* "smart" indent for a line inside a block comment:  a leading * lines up
   under the opener's *, text lines up with text after the opener.
   -1 if there's no good answer. */
int ind_commentIndent( TextBuffer  *inTB,
                       int          inLine );


/* re-indents one line.  Returns the change in the line's indentation. */
int ind_indentLine( TextBuffer  *inTB,
                    int          inLine );


/* re-indents a range of lines (inclusive) */
void ind_indentRegion( TextBuffer  *inTB,
                       int          inFirstLine,
                       int          inLastLine );


/* syntactic context at the start of a line, for the formatter */
typedef struct LineContext {
        /* 1 if the line starts inside a comment or continued macro */
        int  inComment;
        int  inPreproc;
        /* 1 if the line starts inside ( or [ */
        int  inParen;
        /* 1 if inside a ( that is at top level, directly after an
           identifier (i.e. a function parameter list) */
        int  inParamList;
        /* 1 if inside a { } initializer / enum list */
        int  inList;
        /* 1 if a statement is in progress (line is a continuation) */
        int  midStatement;
        /* number of open braces */
        int  braceDepth;
    } LineContext;


void ind_getLineContext( TextBuffer   *inTB,
                         int           inLine,
                         LineContext  *outContext );


/* finds the bracket matching the one at (inLine, inCol), skipping
   strings and comments.  Returns 1 if found.  Searches at most
   inMaxLines lines. */
int ind_findMatchingBracket( TextBuffer  *inTB,
                             int          inLine,
                             int          inCol,
                             int          inMaxLines,
                             int         *outLine,
                             int         *outCol );


#endif
