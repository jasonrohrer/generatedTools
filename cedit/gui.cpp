/*
  gui.cpp

  The C <-> Dear ImGui shim.  See gui.h.
*/

#include "gui.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl2.h"

#include <SDL.h>
#include <SDL_opengl.h>

#include <float.h>
#include <stdio.h>


/* built-in font (Cousine, Apache 2.0), from fontData.c */
extern "C" {
    extern const unsigned char  cousineFontData[];
    extern const unsigned int   cousineFontDataSize;
    }


static ImFont      *font         =  NULL;
static float        fontSize     =  16.0f;
static float        charW        =  9.6f;
static ImDrawList  *drawList     =  NULL;
static int          inFrame      =  0;
static float        measuredFor  =  -1.0f;



static void measure( void ) {
    if( font == NULL || measuredFor == fontSize ) {
        return;
        }
    /* average over many chars to get the exact (fractional) advance */
    charW = font->CalcTextSizeA( fontSize, FLT_MAX, 0.0f,
                                 "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM"
                                 "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM"
                                 "MMMMMMMMMM" ).x / 100.0f;
    measuredFor = fontSize;
    }



extern "C" int gui_init( SDL_Window  *inWindow,
                         void        *inGLContext,
                         const char  *inFontPath,
                         float        inFontSize ) {
    ImFontConfig  cfg;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO  &io  =  ImGui::GetIO();

    io.IniFilename = NULL;
    io.LogFilename = NULL;

    fontSize = inFontSize;

    if( inFontPath != NULL && inFontPath[ 0 ] != '\0' ) {
        font = io.Fonts->AddFontFromFileTTF( inFontPath, inFontSize );
        if( font == NULL ) {
            fprintf( stderr, "cedit:  could not load font %s, using "
                     "built-in font\n", inFontPath );
            }
        }
    if( font == NULL ) {
        cfg.FontDataOwnedByAtlas = false;
        font = io.Fonts->AddFontFromMemoryTTF(
            (void *)cousineFontData, (int)cousineFontDataSize,
            inFontSize, &cfg );
        }
    if( font == NULL ) {
        font = io.Fonts->AddFontDefault();
        }

    if( ! ImGui_ImplSDL2_InitForOpenGL( inWindow, inGLContext ) ) {
        return 0;
        }
    if( ! ImGui_ImplOpenGL2_Init() ) {
        return 0;
        }
    /* close estimate until the first frame measures it exactly */
    charW = inFontSize * 0.6f;
    return 1;
    }



extern "C" void gui_shutdown( void ) {
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    }



extern "C" void gui_processEvent( const SDL_Event  *inEvent ) {
    ImGui_ImplSDL2_ProcessEvent( inEvent );
    }



extern "C" void gui_newFrame( void ) {
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    drawList = ImGui::GetBackgroundDrawList();
    inFrame = 1;
    measure();
    }



extern "C" void gui_render( void ) {
    ImGuiIO  &io  =  ImGui::GetIO();

    ImGui::Render();
    glViewport( 0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y );
    glClearColor( 0, 0, 0, 1 );
    glClear( GL_COLOR_BUFFER_BIT );
    ImGui_ImplOpenGL2_RenderDrawData( ImGui::GetDrawData() );
    drawList = NULL;
    inFrame = 0;
    }



extern "C" void gui_setFontSize( float  inSize ) {
    fontSize = inSize;
    charW = inSize * 0.6f;
    if( inFrame ) {
        measure();
        }
    }



extern "C" float gui_charWidth( void ) {
    return charW;
    }



extern "C" float gui_fontSize( void ) {
    return fontSize;
    }



extern "C" void gui_rect( float         inX0,
                          float         inY0,
                          float         inX1,
                          float         inY1,
                          unsigned int  inColor ) {
    if( drawList != NULL ) {
        drawList->AddRectFilled( ImVec2( inX0, inY0 ), ImVec2( inX1, inY1 ),
                                 inColor );
        }
    }



extern "C" void gui_rectOutline( float         inX0,
                                 float         inY0,
                                 float         inX1,
                                 float         inY1,
                                 unsigned int  inColor,
                                 float         inThickness ) {
    if( drawList != NULL ) {
        drawList->AddRect( ImVec2( inX0, inY0 ), ImVec2( inX1, inY1 ),
                           inColor, 0.0f, 0, inThickness );
        }
    }



extern "C" void gui_line( float         inX0,
                          float         inY0,
                          float         inX1,
                          float         inY1,
                          unsigned int  inColor,
                          float         inThickness ) {
    if( drawList != NULL ) {
        drawList->AddLine( ImVec2( inX0, inY0 ), ImVec2( inX1, inY1 ),
                           inColor, inThickness );
        }
    }



extern "C" void gui_text( float         inX,
                          float         inY,
                          unsigned int  inColor,
                          const char   *inText,
                          int           inLen ) {
    if( drawList != NULL && inLen > 0 ) {
        drawList->AddText( font, fontSize, ImVec2( inX, inY ), inColor,
                           inText, inText + inLen );
        }
    }



extern "C" void gui_pushClip( float  inX0,
                              float  inY0,
                              float  inX1,
                              float  inY1 ) {
    if( drawList != NULL ) {
        drawList->PushClipRect( ImVec2( inX0, inY0 ), ImVec2( inX1, inY1 ),
                                true );
        }
    }



extern "C" void gui_popClip( void ) {
    if( drawList != NULL ) {
        drawList->PopClipRect();
        }
    }
