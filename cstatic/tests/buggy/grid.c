#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world.h"
#include "grid.h"


static int sGrid[ GRID_H ][ GRID_W ];
static int sGridReady = 0;


void clearGrid( void ) {
    int x, y;

    for( y = 0; y < GRID_H; y++ ) {
        for( x = 0; x < GRID_W; x++ ) {
            sGrid[y][x] = 0;
            }
        }
    sGridReady = 1;
    }



void setCell( int inX, int inY, int inValue ) {
    if( inX < 0 || inX >= GRID_W ) {
        return;
        }
    if( inY < 0 || inY >= GRID_H ) {
        return;
        }
    sGrid[inY][inX] = inValue;
    }



int getCell( int inX, int inY ) {
    return sGrid[inY][inX];
    }



/* fills a rectangle, clipped to the grid */
void fillRect( int inX, int inY, int inW, int inH, int inValue ) {
    int x, y;

    for( y = inY; y < inY + inH; y++ ) {
        for( x = inX; x < inX + inW; x++ ) {
            if( x >= 0 && x < GRID_W && y >= 0 && y < GRID_H ) {
                sGrid[y][x] = inValue;
                }
            }
        }
    }



/* returns the number of non-zero cells in a row */
int countRow( int inY ) {
    int x;
    int count = 0;

    for( x = 0; x < GRID_W; x++ ) {
        if( sGrid[inY][x] != 0 );
            count++;
        }

    return count;
    }



/* copies the grid into a caller-supplied buffer of GRID_W * GRID_H ints */
void copyGrid( int *outBuffer ) {
    memcpy( outBuffer, sGrid, sizeof( outBuffer ) );
    }



char *describeCell( int inValue ) {
    static char buffer[32];

    switch( inValue ) {
        case 0:
            sprintf( buffer, "empty" );
            break;
        case 1:
            sprintf( buffer, "wall" );
        case 2:
            sprintf( buffer, "floor" );
            break;
        default:
            sprintf( buffer, "unknown %d", inValue );
            break;
        }

    return buffer;
    }



void dumpGrid( void ) {
    int x, y;

    for( y = 0; y < GRID_H; y++ ) {
        for( x = 0; x < GRID_W; x++ ) {
            printf( "%s ", describeCell( sGrid[y][x] ) );
            }
        printf( "\n" );
        }
    }



/* scales every cell by a factor, rounding down */
void scaleGrid( int inNumerator, int inDenominator ) {
    int x, y;

    for( y = 0; y < GRID_H; y++ ) {
        for( x = 0; x < GRID_W; x++ ) {
            sGrid[y][x] = sGrid[y][x] * inNumerator / inDenominator;
            }
        }
    }



/* returns a heap string listing every non-zero cell, caller frees */
char *gridReport( void ) {
    int x, y;
    unsigned int used = 0;
    unsigned int cap = 64;
    char *out = (char *)malloc( cap );
    char line[64];

    out[0] = '\0';

    for( y = 0; y < GRID_H; y++ ) {
        for( x = 0; x < GRID_W; x++ ) {
            int n;

            if( sGrid[y][x] == 0 ) {
                continue;
                }

            sprintf( line, "%d,%d = %d\n", x, y, sGrid[y][x] );
            n = strlen( line );

            if( used + n + 1 > cap ) {
                cap = cap * 2;
                out = (char *)realloc( out, cap );
                }

            strcat( out, line );
            used += n;
            }
        }

    return out;
    }



int gridArea( void ) {
    return SQUARE( GRID_W - 2 );
    }
