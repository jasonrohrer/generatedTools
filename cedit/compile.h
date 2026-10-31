/*
  compile.h

  Runs the compile command in the background (through /bin/sh, with
  stderr merged into stdout), streams its output into a text buffer
  without ever blocking the editor, and parses gcc-style messages:

      file.c:12:5: error: expected ';' before '}' token
      file.c:12: warning: ...

  Indented lines right after a message (gcc's source excerpts, or the
  explanation lines of tools like cstatic) are attached to it as detail.

  Also launches the (detached) debugger.

  Pure C89 + POSIX, no SDL.
*/

#ifndef CEDIT_COMPILE_H
#define CEDIT_COMPILE_H

#include "textBuffer.h"


#define CE_ERROR    0
#define CE_WARNING  1
#define CE_NOTE     2


typedef struct CompileError {
        char        *file;
        int          line;
        int          col;
        /* line in the compile output buffer */
        int          outLine;
        /* number of detail lines following it in the output */
        int          numDetailLines;
        int          kind;
        /* buffer + marker tracking this location as the file is edited;
           NULL until the error is first visited */
        TextBuffer  *buffer;
        int          marker;
    } CompileError;



/* starts inCommand, output goes to inOutput (which is cleared).
   Returns 1 on success. */
int comp_start( const char  *inCommand,
                TextBuffer  *inOutput );

/* reads whatever output is available (never blocks).  Returns 1 if
   anything changed (new output, or the process finished). */
int comp_poll( void );

int comp_isRunning( void );

/* exit status of the last finished compile (-1 if killed/unknown) */
int comp_exitStatus( void );

void comp_kill( void );

int comp_numErrors( void );

CompileError *comp_getError( int  inIndex );

/* number of errors and warnings (not notes) */
void comp_countErrors( int  *outErrors,
                       int  *outWarnings );

/* forgets markers that point into a buffer that is going away */
void comp_forgetBuffer( TextBuffer  *inBuffer );

/* runs a command through /bin/sh fully detached from the editor
   (own session, output to /dev/null).  Returns 1 on success. */
int comp_launchDetached( const char  *inCommand );

/* expands %f %p %l %e %% in a debug command template.  Returns a
   malloc'd string. */
char *comp_expandDebugCommand( const char  *inTemplate,
                               const char  *inFileName,
                               const char  *inFullPath,
                               int          inLine,
                               const char  *inProgram );


#endif
