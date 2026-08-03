#ifndef GRID_H_INCLUDED
#define GRID_H_INCLUDED

#define GRID_W 12
#define GRID_H 10

void clearGrid( void );

char *describeCell( int inValue );

void setCell( int inX, int inY, int inValue );
int getCell( int inX, int inY );

void fillRect( int inX, int inY, int inW, int inH, int inValue );

int countRow( int inY );

void copyGrid( int *outBuffer );

void dumpGrid( void );

void scaleGrid( int inNumerator, int inDenominator );

char *gridReport( void );

int gridArea( void );

#endif
