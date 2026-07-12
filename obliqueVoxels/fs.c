/* fs.c -- directory listing for the file browser.  Built with a relaxed
 * standard (see Makefile) so POSIX dirent/stat/getcwd are available. */
#include "fs.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* case-insensitive test: does 'name' end with 'ext'? */
static int ci_suffix( const char *name, const char *ext )
{
    size_t L, E, i;
    if( !ext || !ext[0] ) return 1;
    L = strlen( name ); E = strlen( ext );
    if( L < E ) return 0;
    for( i = 0; i < E; i++ ) {
        char a = name[L - E + i], b = ext[i];
        if( a >= 'A' && a <= 'Z' ) a = (char)( a + 32 );
        if( b >= 'A' && b <= 'Z' ) b = (char)( b + 32 );
        if( a != b ) return 0;
    }
    return 1;
}

int fs_is_dir( const char *path )
{
    struct stat st;
    if( stat( path, &st ) != 0 ) return 0;
    return S_ISDIR( st.st_mode ) ? 1 : 0;
}

void fs_cwd( char *buf, int buflen )
{
    if( buflen < 2 || !getcwd( buf, (size_t)buflen ) ) {
        if( buflen >= 2 ) { buf[0] = '.'; buf[1] = 0; }
    }
}

static int fs_cmp( const void *a, const void *b )
{
    const FSEntry *x = (const FSEntry *)a, *y = (const FSEntry *)b;
    if( x->isDir != y->isDir ) return y->isDir - x->isDir;   /* dirs first */
    return strcmp( x->name, y->name );
}

int fs_list( const char *dir, const char *ext, FSEntry *out, int maxOut )
{
    DIR *d;
    struct dirent *e;
    int n = 0;
    d = opendir( dir );
    if( !d ) return 0;
    while( ( e = readdir( d ) ) != NULL && n < maxOut ) {
        const char *nm = e->d_name;
        char full[2048];
        int isDir;
        size_t used;
        if( nm[0] == '.' ) {
            /* keep only ".."; drop "." and every hidden dotfile */
            if( !( nm[1] == '.' && nm[2] == '\0' ) ) continue;
        }
        full[0] = 0;
        strncpy( full, dir, sizeof full - 1 );
        full[ sizeof full - 1 ] = 0;
        used = strlen( full );
        if( used == 0 || full[used - 1] != '/' )
            strncat( full, "/", sizeof full - strlen( full ) - 1 );
        strncat( full, nm, sizeof full - strlen( full ) - 1 );
        isDir = fs_is_dir( full );
        if( !isDir && !ci_suffix( nm, ext ) ) continue;
        strncpy( out[n].name, nm, sizeof out[n].name - 1 );
        out[n].name[ sizeof out[n].name - 1 ] = 0;
        out[n].isDir = isDir;
        n++;
    }
    closedir( d );
    qsort( out, (size_t)n, sizeof( FSEntry ), fs_cmp );
    return n;
}

void fs_join( const char *dir, const char *child, char *buf, int buflen )
{
    int len;
    if( buflen < 2 ) { if( buflen > 0 ) buf[0] = 0; return; }
    if( child && child[0] == '/' ) {           /* absolute child replaces dir */
        strncpy( buf, child, (size_t)buflen - 1 );
        buf[buflen - 1] = 0;
        return;
    }
    strncpy( buf, dir, (size_t)buflen - 1 );
    buf[buflen - 1] = 0;
    len = (int)strlen( buf );
    if( child && strcmp( child, ".." ) == 0 ) {          /* go up one level */
        while( len > 1 && buf[len - 1] == '/' ) buf[--len] = 0;
        while( len > 0 && buf[len - 1] != '/' ) buf[--len] = 0;
        while( len > 1 && buf[len - 1] == '/' ) buf[--len] = 0;
        if( buf[0] == 0 ) { buf[0] = '/'; buf[1] = 0; }
        return;
    }
    if( !child || !child[0] ) return;                    /* nothing to append */
    if( len == 0 || buf[len - 1] != '/' )
        strncat( buf, "/", (size_t)buflen - strlen( buf ) - 1 );
    strncat( buf, child, (size_t)buflen - strlen( buf ) - 1 );
}
