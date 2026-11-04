/*
  compile.c

  See compile.h
*/

#include "compile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>


static pid_t          childPID     =  -1;
static int            childFD      =  -1;
static int            lastStatus   =  -1;
static TextBuffer    *output       =  NULL;
static int            parsedLines  =  0;
static CompileError  *errors       =  NULL;
static int            numErrors    =  0;
static int            capErrors    =  0;
/* directory prefix from "make: Entering directory" */
static char           dirPrefix[ 1024 ]  =  "";



static void clearErrors( void ) {
    int  i;

    for( i = 0;
         i < numErrors;
         i ++ ) {
        free( errors[ i ].file );
        if( errors[ i ].buffer != NULL ) {
            tb_removeMarker( errors[ i ].buffer, errors[ i ].marker );
            }
        }
    numErrors = 0;
    }



int comp_start( const char  *inCommand,
                TextBuffer  *inOutput ) {
    int    fds[ 2 ];
    pid_t  pid;
    char   header[ 1200 ];

    if( childPID > 0 ) {
        comp_kill();
        }
    clearErrors();
    dirPrefix[ 0 ] = '\0';
    output = inOutput;
    tb_setText( output, "", 0 );
    tb_clearUndo( output );
    parsedLines = 0;

    sprintf( header, "-*- compiling:  %.1000s\n\n", inCommand );
    tb_insert( output, 0, 0, header, (int)strlen( header ), NULL, NULL );
    parsedLines = 2;

    if( pipe( fds ) != 0 ) {
        return 0;
        }
    pid = fork();
    if( pid < 0 ) {
        close( fds[ 0 ] );
        close( fds[ 1 ] );
        return 0;
        }
    if( pid == 0 ) {
        int  devNull  =  open( "/dev/null", O_RDONLY );

        if( devNull >= 0 ) {
            dup2( devNull, 0 );
            close( devNull );
            }
        dup2( fds[ 1 ], 1 );
        dup2( fds[ 1 ], 2 );
        close( fds[ 0 ] );
        close( fds[ 1 ] );
        /* own process group, so kill can take out make's children too */
        setpgid( 0, 0 );
        execl( "/bin/sh", "sh", "-c", inCommand, (char *)NULL );
        _exit( 127 );
        }
    close( fds[ 1 ] );
    childFD = fds[ 0 ];
    fcntl( childFD, F_SETFL, fcntl( childFD, F_GETFL ) | O_NONBLOCK );
    childPID = pid;
    lastStatus = -1;
    return 1;
    }



int comp_isRunning( void ) {
    return childPID > 0;
    }



int comp_exitStatus( void ) {
    return lastStatus;
    }



void comp_kill( void ) {
    if( childPID > 0 ) {
        kill( - childPID, SIGTERM );
        kill( childPID, SIGTERM );
        }
    }



static int parseInt( const char  *inS,
                     int         *outLen ) {
    int  v  =  0;
    int  n  =  0;

    while( inS[ n ] >= '0' && inS[ n ] <= '9' ) {
        v = v * 10 + ( inS[ n ] - '0' );
        n ++;
        }
    *outLen = n;
    return v;
    }



/* tries to parse  file:line[:col]: [kind:] message  */
static void parseLine( int  inLine ) {
    Line  *l     =  &( output->lines[ inLine ] );
    char   buf[ 2048 ];
    int    len   =  l->len;
    int    i;
    int    fileEnd;
    int    lineNum;
    int    col   =  0;
    int    n;
    int    kind  =  CE_ERROR;
    char  *rest;

    if( len > (int)sizeof( buf ) - 1 ) {
        len = (int)sizeof( buf ) - 1;
        }
    memcpy( buf, l->text, (size_t)len );
    buf[ len ] = '\0';

    /* indented line:  detail of the previous message */
    if( len > 0 && buf[ 0 ] == ' ' && numErrors > 0
        &&
        errors[ numErrors - 1 ].outLine
        + errors[ numErrors - 1 ].numDetailLines + 1 == inLine ) {
        errors[ numErrors - 1 ].numDetailLines ++;
        return;
        }
    /* gcc's  "  | ^~~~" excerpt lines also start with spaces or digits */

    if( strncmp( buf, "make", 4 ) == 0 ) {
        char  *e  =  strstr( buf, "Entering directory" );

        if( e != NULL ) {
            char  *q  =  strchr( e, '\'' );
            char  *q2;

            if( q == NULL ) {
                q = strchr( e, '`' );
                }
            if( q != NULL ) {
                q2 = strchr( q + 1, '\'' );
                if( q2 != NULL ) {
                    *q2 = '\0';
                    sprintf( dirPrefix, "%.1000s/", q + 1 );
                    }
                }
            }
        else if( strstr( buf, "Leaving directory" ) != NULL ) {
            dirPrefix[ 0 ] = '\0';
            }
        return;
        }

    /* file name:  no spaces, ends at ':' followed by a digit */
    fileEnd = -1;
    for( i = 0;
         i < len;
         i ++ ) {
        if( buf[ i ] == ' ' || buf[ i ] == '\t' ) {
            return;
            }
        if( buf[ i ] == ':' && i + 1 < len
            &&
            buf[ i + 1 ] >= '0' && buf[ i + 1 ] <= '9' ) {
            fileEnd = i;
            break;
            }
        }
    if( fileEnd <= 0 ) {
        return;
        }
    lineNum = parseInt( buf + fileEnd + 1, &n );
    i = fileEnd + 1 + n;
    if( buf[ i ] != ':' ) {
        return;
        }
    i ++;
    if( buf[ i ] >= '0' && buf[ i ] <= '9' ) {
        col = parseInt( buf + i, &n );
        i += n;
        if( buf[ i ] != ':' ) {
            return;
            }
        i ++;
        }
    rest = buf + i;
    while( *rest == ' ' ) {
        rest ++;
        }
    if( strncmp( rest, "warning", 7 ) == 0 ) {
        kind = CE_WARNING;
        }
    else if( strncmp( rest, "note", 4 ) == 0 ) {
        kind = CE_NOTE;
        }

    if( numErrors >= capErrors ) {
        capErrors = capErrors * 2 + 32;
        errors = (CompileError *)realloc( errors, (size_t)capErrors
                                          * sizeof( CompileError ) );
        }
    {
        CompileError  *e  =  &( errors[ numErrors ] );
        size_t         pl  =  strlen( dirPrefix );

        buf[ fileEnd ] = '\0';
        if( buf[ 0 ] == '/' ) {
            pl = 0;
            }
        e->file = (char *)malloc( pl + (size_t)fileEnd + 1 );
        memcpy( e->file, dirPrefix, pl );
        strcpy( e->file + pl, buf );
        e->line = lineNum;
        e->col = col;
        e->outLine = inLine;
        e->numDetailLines = 0;
        e->kind = kind;
        e->buffer = NULL;
        e->marker = -1;
        numErrors ++;
        }
    }



static void appendOutput( const char  *inData,
                          int          inLen ) {
    int  last  =  output->numLines - 1;

    tb_insert( output, last, output->lines[ last ].len, inData, inLen,
               NULL, NULL );
    tb_clearUndo( output );
    }



static void parseNewLines( int  inAll ) {
    int  limit  =  output->numLines - 1;

    if( inAll ) {
        limit = output->numLines;
        }
    while( parsedLines < limit ) {
        parseLine( parsedLines );
        parsedLines ++;
        }
    }



int comp_poll( void ) {
    char  buf[ 8192 ];
    int   changed  =  0;
    int   reads    =  0;

    if( childPID <= 0 ) {
        return 0;
        }

    /* bounded number of reads per poll, so a chatty build can't stall
       the UI */
    while( reads < 64 ) {
        ssize_t  n  =  read( childFD, buf, sizeof( buf ) );

        if( n > 0 ) {
            int  i;
            int  k  =  0;

            /* strip \r */
            for( i = 0;
                 i < n;
                 i ++ ) {
                if( buf[ i ] != '\r' ) {
                    buf[ k ++ ] = buf[ i ];
                    }
                }
            appendOutput( buf, k );
            changed = 1;
            reads ++;
            continue;
            }
        if( n == 0 ) {
            /* EOF */
            int  status  =  0;
            char msg[ 128 ];

            close( childFD );
            childFD = -1;
            waitpid( childPID, &status, 0 );
            childPID = -1;
            if( WIFEXITED( status ) ) {
                lastStatus = WEXITSTATUS( status );
                }
            else {
                lastStatus = -1;
                }
            parseNewLines( 1 );
            {
                int  ne;
                int  nw;

                comp_countErrors( &ne, &nw );
                if( lastStatus == 0 ) {
                    sprintf( msg, "\n-*- compilation finished "
                             "(%d errors, %d warnings)\n", ne, nw );
                    }
                else if( lastStatus > 0 ) {
                    sprintf( msg, "\n-*- compilation exited abnormally "
                             "with code %d (%d errors, %d warnings)\n",
                             lastStatus, ne, nw );
                    }
                else {
                    sprintf( msg, "\n-*- compilation killed\n" );
                    }
                }
            appendOutput( msg, (int)strlen( msg ) );
            parsedLines = output->numLines;
            return 1;
            }
        if( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ) {
            break;
            }
        /* real error */
        break;
        }
    if( changed ) {
        parseNewLines( 0 );
        }
    return changed;
    }



int comp_numErrors( void ) {
    return numErrors;
    }



CompileError *comp_getError( int  inIndex ) {
    if( inIndex < 0 || inIndex >= numErrors ) {
        return NULL;
        }
    return &( errors[ inIndex ] );
    }



void comp_countErrors( int  *outErrors,
                       int  *outWarnings ) {
    int  i;

    *outErrors = 0;
    *outWarnings = 0;
    for( i = 0;
         i < numErrors;
         i ++ ) {
        if( errors[ i ].kind == CE_ERROR ) {
            ( *outErrors ) ++;
            }
        else if( errors[ i ].kind == CE_WARNING ) {
            ( *outWarnings ) ++;
            }
        }
    }



void comp_forgetBuffer( TextBuffer  *inBuffer ) {
    int  i;

    for( i = 0;
         i < numErrors;
         i ++ ) {
        if( errors[ i ].buffer == inBuffer ) {
            errors[ i ].buffer = NULL;
            errors[ i ].marker = -1;
            }
        }
    if( output == inBuffer ) {
        output = NULL;
        comp_kill();
        }
    }



int comp_launchDetached( const char  *inCommand ) {
    pid_t  pid  =  fork();

    if( pid < 0 ) {
        return 0;
        }
    if( pid == 0 ) {
        /* double fork so the debugger is never our zombie */
        pid_t  pid2;

        setsid();
        pid2 = fork();
        if( pid2 != 0 ) {
            _exit( 0 );
            }
        {
            int  devNull  =  open( "/dev/null", O_RDWR );

            if( devNull >= 0 ) {
                dup2( devNull, 0 );
                dup2( devNull, 1 );
                dup2( devNull, 2 );
                close( devNull );
                }
            }
        execl( "/bin/sh", "sh", "-c", inCommand, (char *)NULL );
        _exit( 127 );
        }
    waitpid( pid, NULL, 0 );
    return 1;
    }



char *comp_expandDebugCommand( const char  *inTemplate,
                               const char  *inFileName,
                               const char  *inFullPath,
                               int          inLine,
                               const char  *inProgram ) {
    size_t       cap  =  strlen( inTemplate ) + strlen( inFullPath ) * 4
                         + strlen( inProgram ) * 4 + 256;
    char        *out  =  (char *)malloc( cap );
    size_t       n    =  0;
    const char  *p    =  inTemplate;
    char         num[ 32 ];

    while( *p != '\0' && n < cap - 1 ) {
        const char  *sub  =  NULL;

        if( *p == '%' && p[ 1 ] != '\0' ) {
            switch( p[ 1 ] ) {
                case 'f':
                    sub = inFileName;
                    break;
                case 'p':
                    sub = inFullPath;
                    break;
                case 'l':
                    sprintf( num, "%d", inLine );
                    sub = num;
                    break;
                case 'e':
                    sub = inProgram;
                    break;
                case '%':
                    sub = "%";
                    break;
                default:
                    break;
                }
            }
        if( sub != NULL ) {
            size_t  sl  =  strlen( sub );

            if( n + sl < cap - 1 ) {
                memcpy( out + n, sub, sl );
                n += sl;
                }
            p += 2;
            }
        else {
            out[ n ++ ] = *p;
            p ++;
            }
        }
    out[ n ] = '\0';
    return out;
    }
