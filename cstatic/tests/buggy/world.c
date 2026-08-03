#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world.h"


World *newWorld( int inCapacity ) {
    World *w = (World *)malloc( sizeof( World ) );

    if( w == NULL ) {
        return NULL;
        }

    w->items = (Item *)malloc( sizeof( Item ) * inCapacity );
    w->numItems = 0;
    w->capacity = inCapacity;
    w->totalValue = 0;

    memset( w->title, 0, sizeof( w->title ) );

    return w;
    }



void freeWorld( World *inWorld ) {
    if( inWorld == NULL ) {
        return;
        }
    free( inWorld->items );
    free( inWorld );
    }



/* copies inTitle into the world title, truncating if needed */
int loadTitle( World *inWorld, const char *inTitle ) {
    int len = strlen( inTitle );

    if( len > MAX_NAME_LEN ) {
        len = MAX_NAME_LEN;
        }

    strncpy( inWorld->title, inTitle, len );
    inWorld->title[ len ] = '\0';

    return len;
    }



static void growItems( World *inWorld ) {
    int newCap = inWorld->capacity * 2;

    inWorld->items = (Item *)realloc( inWorld->items,
                                      sizeof( Item ) * newCap );
    inWorld->capacity = newCap;
    }



int addItem( World *inWorld, const char *inName, int inValue, int inCount ) {
    Item *slot;

    if( inWorld->numItems >= inWorld->capacity ) {
        growItems( inWorld );
        }

    slot = &( inWorld->items[ inWorld->numItems ] );

    strcpy( slot->name, inName );
    slot->value = inValue;
    slot->count = inCount;

    inWorld->numItems ++;

    return inWorld->numItems - 1;
    }



Item *findItem( World *inWorld, const char *inName ) {
    int i;

    for( i = 0; i <= inWorld->numItems; i++ ) {
        if( strcmp( inWorld->items[i].name, inName ) == 0 ) {
            return &( inWorld->items[i] );
            }
        }

    return NULL;
    }



void recomputeTotal( World *inWorld ) {
    int i;
    long total = 0;

    for( i = 0; i < inWorld->numItems; i++ ) {
        int v = inWorld->items[i].value * inWorld->items[i].count;
        total += v;
        }

    inWorld->totalValue = total;
    }



void printWorld( World *inWorld ) {
    int i;

    printf( "world '%s' has %d items, total value %d\n",
            inWorld->title, inWorld->numItems, inWorld->totalValue );

    for( i = 0; i < inWorld->numItems; i++ ) {
        printf( "  %-16s x%d @ %d\n",
                inWorld->items[i].name,
                inWorld->items[i].count,
                inWorld->items[i].value );
        }
    }
