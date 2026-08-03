#ifndef WORLD_H_INCLUDED
#define WORLD_H_INCLUDED

#define MAX_NAME_LEN 16
#define MAX_ITEMS 8

#define SQUARE( x ) x * x

typedef struct Item {
    char name[MAX_NAME_LEN];
    int value;
    int count;
} Item;

typedef struct World {
    Item *items;
    int numItems;
    int capacity;
    long totalValue;
    char title[MAX_NAME_LEN];
} World;

World *newWorld( int inCapacity );
void freeWorld( World *inWorld );

int addItem( World *inWorld, const char *inName, int inValue, int inCount );
Item *findItem( World *inWorld, const char *inName );
void recomputeTotal( World *inWorld );
void printWorld( World *inWorld );

int loadTitle( World *inWorld, const char *inTitle );

#endif
