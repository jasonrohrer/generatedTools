#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world.h"
#include "grid.h"


static const char *sItemNames[] = {
    "sword",
    "shield",
    "potion of extreme healing",
    "rope",
    "lantern"
    };

#define NUM_ITEM_NAMES 5


static void seedWorld( World *inWorld ) {
    int i;

    for( i = 0; i < NUM_ITEM_NAMES; i++ ) {
        addItem( inWorld, sItemNames[i], i * 10, i + 1 );
        }
    }



static void seedGrid( void ) {
    int i;

    clearGrid();

    for( i = 0; i < 10; i++ ) {
        setCell( i, i, i % 3 );
        }

    fillRect( 3, 3, 4, 4, 2 );
    }



static int readCoord( const char *inText ) {
    int value;

    if( inText != NULL ) {
        value = atoi( inText );
        }

    return value;
    }



static void reportPair( int inA, int inB ) {
    printf( "cell A is %s, cell B is %s\n",
            describeCell( inA ), describeCell( inB ) );
    }



int main( int inArgC, char **inArgV ) {
    World *w = newWorld( MAX_ITEMS );
    Item *found;
    char *report;
    int x, y;
    int buffer[ GRID_W * GRID_H ];

    loadTitle( w, "the sunless lands" );

    seedWorld( w );
    seedGrid();

    recomputeTotal( w );
    printWorld( w );

    found = findItem( w, "rope" );

    if( found != NULL ) {
        printf( "found %s worth %d\n", found->name, found->value );
        }

    x = readCoord( inArgC > 1 ? inArgV[1] : NULL );
    y = readCoord( inArgC > 2 ? inArgV[2] : NULL );

    printf( "cell at %d,%d is %d\n", x, y, getCell( x, y ) );

    reportPair( getCell( 0, 0 ), getCell( 1, 1 ) );

    copyGrid( buffer );
    printf( "row 0 count %d, area %d\n", countRow( 0 ), gridArea() );

    report = gridReport();
    printf( "%s", report );
    free( report );

    freeWorld( w );

    printf( "final total was %ld\n", w->totalValue );

    return 0;
    }
