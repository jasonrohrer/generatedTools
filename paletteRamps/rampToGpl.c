// rampToGpl.c
//
// Appends the color ramps found in a PNG to a GIMP (.gpl) palette.
//
// Usage:  rampToGpl <input.gpl> <ramps.png> <output.gpl>
//
// The PNG holds one ramp per row.  A row is treated as a "spacer" (and
// skipped) if every pixel in it is the same color -- these are just visual
// gaps between ramps, and their solid color need not be black.  Any row where
// the color changes at least once is a ramp, and ALL of its pixels are added
// to the palette, including any repeated "padding" pixels at the start or end
// of a shorter ramp, so that the ramps stay aligned when loaded into aseprite.
//
// Only stb_image.h and the C standard library are used.  It compiles with a
// bare "gcc rampToGpl.c" -- no -lm or other linked libraries needed, because
// we restrict stb_image to PNG and disable its HDR/linear paths (the only code
// in stb_image that calls pow()/ldexp()).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Trim stb_image down so it needs no math library:
//   STBI_ONLY_PNG   - only compile the PNG loader (also implies STBI_NO_HDR)
//   STBI_NO_LINEAR  - drop the float/linear loader that calls pow()
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"


// True if a palette line's first non-blank character is a digit, i.e. it looks
// like an "R G B name" color entry rather than a comment or the header.
static int lineIsColor( const char *s, int len ) {
    int i = 0;
    while( i < len && ( s[i] == ' ' || s[i] == '\t' ) ) {
        i++;
    }
    return ( i < len && s[i] >= '0' && s[i] <= '9' );
}


// True if every pixel in row y of the (w x h, 3-channel) image is identical.
static int rowIsSpacer( const unsigned char *img, int w, int y ) {
    const unsigned char *row = img + (size_t)y * w * 3;
    for( int x = 1; x < w; x++ ) {
        if( row[x*3+0] != row[0] ||
            row[x*3+1] != row[1] ||
            row[x*3+2] != row[2] ) {
            return 0;
        }
    }
    return 1;
}


int main( int argc, char **argv ) {
    if( argc != 4 ) {
        fprintf( stderr,
                 "Usage: %s <input.gpl> <ramps.png> <output.gpl>\n", argv[0] );
        return 1;
    }
    const char *inGpl  = argv[1];
    const char *inPng  = argv[2];
    const char *outGpl = argv[3];

    // Read the whole input palette into memory.
    FILE *f = fopen( inGpl, "rb" );
    if( !f ) {
        fprintf( stderr, "Error: cannot open gpl input '%s'\n", inGpl );
        return 1;
    }
    fseek( f, 0, SEEK_END );
    long L = ftell( f );
    fseek( f, 0, SEEK_SET );
    char *buf = malloc( L + 1 );
    if( !buf ) {
        fprintf( stderr, "Error: out of memory\n" );
        fclose( f );
        return 1;
    }
    if( fread( buf, 1, L, f ) != (size_t)L ) {
        fprintf( stderr, "Error: failed reading '%s'\n", inGpl );
        fclose( f );
        free( buf );
        return 1;
    }
    buf[L] = '\0';
    fclose( f );

    // Match the input's line endings for anything we emit.
    int crlf = ( strstr( buf, "\r\n" ) != NULL );
    const char *NL = crlf ? "\r\n" : "\n";

    // Load the ramp image, forcing 3 (RGB) channels.
    int w, h, n;
    unsigned char *img = stbi_load( inPng, &w, &h, &n, 3 );
    if( !img ) {
        fprintf( stderr, "Error: cannot load png '%s': %s\n",
                 inPng, stbi_failure_reason() );
        free( buf );
        return 1;
    }

    // Count how many palette entries the ramps will add, and find the padding
    // color: the color of the spacer rows in the PNG (which is also the color
    // used to pad shorter ramps).  Fall back to black if there are no spacers.
    int rampPixels = 0;
    unsigned char padColor[3] = { 0, 0, 0 };
    int foundPadColor = 0;
    for( int y = 0; y < h; y++ ) {
        if( rowIsSpacer( img, w, y ) ) {
            if( !foundPadColor ) {
                const unsigned char *row = img + (size_t)y * w * 3;
                padColor[0] = row[0];
                padColor[1] = row[1];
                padColor[2] = row[2];
                foundPadColor = 1;
            }
        }
        else {
            rampPixels += w;
        }
    }

    // Count the palette entries already present.
    int existing = 0;
    for( const char *p = buf; *p; ) {
        const char *nl = strchr( p, '\n' );
        int len = nl ? (int)( nl - p ) : (int)strlen( p );
        if( lineIsColor( p, len ) ) {
            existing++;
        }
        if( !nl ) break;
        p = nl + 1;
    }

    // Aseprite lays the palette out in a grid that is one ramp (w squares)
    // wide.  For the ramps to start on a fresh row, the number of entries
    // before them must be a multiple of w, so pad the tail of the main palette
    // with padColor to fill out its last partial row.
    int padCount = ( w - ( existing % w ) ) % w;
    int newTotal = existing + padCount + rampPixels;

    // Write the output palette.
    FILE *o = fopen( outGpl, "wb" );
    if( !o ) {
        fprintf( stderr, "Error: cannot open output '%s'\n", outGpl );
        stbi_image_free( img );
        free( buf );
        return 1;
    }

    // Copy the original palette line by line, updating the "#Colors:" comment
    // (if any) to reflect the new total.
    for( const char *p = buf; *p; ) {
        const char *nl = strchr( p, '\n' );
        int len = nl ? (int)( nl - p ) : (int)strlen( p );
        int clen = len;
        if( clen > 0 && p[clen-1] == '\r' ) {
            clen--;  // drop the trailing '\r'; NL is re-added below
        }
        if( clen >= 8 && strncmp( p, "#Colors:", 8 ) == 0 ) {
            fprintf( o, "#Colors: %d%s", newTotal, NL );
        }
        else {
            fwrite( p, 1, clen, o );
            fputs( NL, o );
        }
        if( !nl ) break;
        p = nl + 1;
    }

    // Pad out the last partial row of the main palette so the ramps align.
    if( padCount > 0 ) {
        fprintf( o, "#Padding to align ramps%s", NL );
        for( int i = 0; i < padCount; i++ ) {
            fprintf( o, "%d\t%d\t%d\t%02x%02x%02x%s",
                     padColor[0], padColor[1], padColor[2],
                     padColor[0], padColor[1], padColor[2], NL );
        }
    }

    // Append the ramps.
    fprintf( o, "#Ramps%s", NL );
    for( int y = 0; y < h; y++ ) {
        if( rowIsSpacer( img, w, y ) ) {
            continue;
        }
        const unsigned char *row = img + (size_t)y * w * 3;
        for( int x = 0; x < w; x++ ) {
            unsigned char r = row[x*3+0];
            unsigned char g = row[x*3+1];
            unsigned char b = row[x*3+2];
            fprintf( o, "%d\t%d\t%d\t%02x%02x%02x%s", r, g, b, r, g, b, NL );
        }
    }

    fclose( o );
    stbi_image_free( img );
    free( buf );

    printf( "Wrote '%s': %d existing + %d padding + %d ramp = %d colors\n",
            outGpl, existing, padCount, rampPixels, newTotal );
    return 0;
}
