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
    - spaces around assignments ( x = 6; ), and before a block's '{'
    - the arguments of calls and parameter lists one per line, lined up
      after the '(', and for( ) clauses one per line as a table:
          for( x = 0;
               x < 10;
               x   ++ ) {

  Only spaces are ever added or removed, and never inside strings or
  comments.  Spacing that is already wider than needed is left alone
  (it's probably deliberate alignment).

  Pure C89, no SDL.
*/

#ifndef CEDIT_FORMAT_H
#define CEDIT_FORMAT_H

#include "textBuffer.h"


/* flag:  the cursor's line may be an unfinished declaration ("int  "
   or a parameter "Deck *"), and the cursor goes to where its name will
   line up with the table around it (Tab does this) */
#define FMT_PARTIAL  1


/* formats one line in place (which may split it into several lines).
   ioCursorLine/ioCursorCol (may be NULL) are moved along with the text
   they were on.  Returns the number of lines added. */
int fmt_formatLine( TextBuffer  *inTB,
                    int          inLine,
                    int          inFlags,
                    int         *ioCursorLine,
                    int         *ioCursorCol );


/* if inLine is (part of) a declaration, lines up the run of consecutive
   declarations around it.  Returns 1 if anything changed.  The cursor
   (may be NULL) is moved along with its text. */
int fmt_alignDeclarations( TextBuffer  *inTB,
                           int          inLine,
                           int          inFlags,
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
