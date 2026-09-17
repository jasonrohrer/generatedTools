/*
  caSimMingin.c

  Compiles the "null" (dummy, platform-independent) implementation of
  mingin.h for the headless caSim simulator.

  We undefine the platform macros so that mingin.h falls through to its
  dummy implementation, which has no windowing, audio, or file access.

  The dummy implementation defines its own  int main( void )  entry point,
  which would collide with caSim's main, so we rename it away with a macro.
*/

#undef __linux__
#undef _WIN32

#define main caSimMinginDummyMain

int caSimMinginDummyMain( void );

#define MINGIN_IMPLEMENTATION
#include "mingin.h"

#undef main
