/*
  format.h

  Auto-formatting of C89 code as you type (controlled by config):

    - padding inside parens and brackets:   foo(a[i])  ->  foo( a[ i ] )
      (casts stay tight:  (int)x)
    - a space after commas
    - two spaces between type and name in function parameter lists:
          int foo( int  inX, const char  *inName )
    - consecutive variable declarations lined up into a table:
          Deck  *d      =  getDeck();
          int    i;
          long   total  =  0;

  Only spaces are ever added or removed, and never inside strings or
  comments.  Spacing that is already wider than needed is left alone
  (it's probably deliberate alignment).

  Pure C89, no SDL.
*/

#ifndef CEDIT_FORMAT_H
#define CEDIT_FORMAT_H

#include "textBuffer.h"


/* formats one line in place.  inCursorCol is a column on that line;
   returns where that column ended up after the edits. */
int fmt_formatLine( TextBuffer  *inTB,
                    int          inLine,
                    int          inCursorCol );


/* if inLine is a declaration, lines up the run of consecutive
   declarations around it.  Returns 1 if anything changed.
   If ioCursorLine/ioCursorCol are non-NULL and on an affected line, the
   cursor is moved along with its text. */
int fmt_alignDeclarations( TextBuffer  *inTB,
                           int          inLine,
                           int         *ioCursorLine,
                           int         *ioCursorCol );


/* formats every line in the range, then aligns declaration runs */
void fmt_formatRegion( TextBuffer  *inTB,
                       int          inFirstLine,
                       int          inLastLine );


/* 1 if the line parses as a single-line variable declaration */
int fmt_isDeclarationLine( TextBuffer  *inTB,
                           int          inLine );


#endif
