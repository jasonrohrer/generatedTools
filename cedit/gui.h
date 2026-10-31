/*
  gui.h

  Thin C API over Dear ImGui + its SDL2 / OpenGL2 backends.

  Dear ImGui is C++, but cedit is C89.  This shim exposes just what the
  editor needs, which is mostly ImGui's draw lists:  cedit draws its own
  text area (it has to, for speed with huge files and for exact control
  over a fixed character grid), and uses ImGui for font rasterizing,
  batching, and the GL plumbing.

  Implemented in gui.cpp.
*/

#ifndef CEDIT_GUI_H
#define CEDIT_GUI_H

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;
union  SDL_Event;


/* inFontPath may be NULL/empty to use the built-in font.
   Returns 0 on failure. */
int  gui_init( struct SDL_Window  *inWindow,
               void               *inGLContext,
               const char         *inFontPath,
               float               inFontSize );

void gui_shutdown( void );

void gui_processEvent( const union SDL_Event  *inEvent );

void gui_newFrame( void );

/* draws and swaps nothing;  caller swaps */
void gui_render( void );

/* change the font size used by all text calls */
void gui_setFontSize( float  inSize );

/* width of one character cell, and font size, in pixels */
float gui_charWidth( void );

float gui_fontSize( void );

/* colors are 0xAABBGGRR */
void gui_rect( float         inX0,
               float         inY0,
               float         inX1,
               float         inY1,
               unsigned int  inColor );

void gui_rectOutline( float         inX0,
                      float         inY0,
                      float         inX1,
                      float         inY1,
                      unsigned int  inColor,
                      float         inThickness );

void gui_line( float         inX0,
               float         inY0,
               float         inX1,
               float         inY1,
               unsigned int  inColor,
               float         inThickness );

/* draws inLen bytes of UTF-8 text */
void gui_text( float         inX,
               float         inY,
               unsigned int  inColor,
               const char   *inText,
               int           inLen );

/* clip all following drawing to this rect (until gui_popClip) */
void gui_pushClip( float  inX0,
                   float  inY0,
                   float  inX1,
                   float  inY1 );

void gui_popClip( void );


#ifdef __cplusplus
}
#endif

#endif
