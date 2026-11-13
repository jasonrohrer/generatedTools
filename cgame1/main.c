/*
 * Path Master
 *
 * A roguelike auto-walker.  The wizard walks and fights alone;  the
 * player masters the wand.
 *
 * This file:  window, integer pixel scaling, the main loop, and the
 * headless test modes.
 *
 * Usage:
 *     pathMaster                    play
 *     pathMaster --fullscreen       start in fullscreen
 *     pathMaster --seed N           fixed random seed
 *     pathMaster --autoplay         a scripted player manages the wand
 *     pathMaster --speed N          start at N ticks per frame
 *     pathMaster --fonttest         show sample text in the info panel
 *     pathMaster --sim N            no window:  simulate N scripted runs
 *                                   and print how far each one got
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>
#include <SDL_opengl.h>

#include "game.h"


static SDL_Window *window     = 0;
static int         fullscreen = 0;

/* where the game picture sits inside the window, in window pixels */
static int viewX     = 0;
static int viewY     = 0;
static int viewScale = 1;



/* largest whole multiple of 640x480 that fits */
static int fitScale( int inW, int inH ) {
    int sx = inW / SCREEN_W;
    int sy = inH / SCREEN_H;
    int s = sx < sy ? sx : sy;
    return s < 1 ? 1 : s;
    }



static void runSim( int inRuns, unsigned int inSeed ) {
    int r;
    long totalWave = 0;

    g.noSave = 1;
    for( r = 0; r < inRuns; r++ ) {
        gameSeed( inSeed + (unsigned int)r );
        gameInit();
        gameStartRun();
        /* at most two hours of game time */
        while( g.state != STATE_DEAD && g.runTicks < 60 * 60 * 120 ) {
            gameTick();
            if( g.tick % 20 == 0 ) {
                gameAiStep();
                }
            }
        printf( "run %2d:  wave %3d  level %2d  kills %4d  time %5.1f min"
                "  dmg %6.1f  rate %.2f  maxHp %.0f\n",
                r, g.wave + 1, g.level, g.kills,
                (double)g.runTicks / 3600.0, (double)g.wand.dmg,
                1.0 / (double)g.wand.interval, (double)g.wand.maxHp );
        totalWave += g.wave + 1;
        }
    printf( "average wave %.1f\n", (double)totalWave / (double)inRuns );
    }



static void toMousePos( int inWinX, int inWinY ) {
    int winW, winH, drawW, drawH;
    int x = inWinX;
    int y = inWinY;

    /* window coordinates can differ from drawable pixels */
    SDL_GetWindowSize( window, &winW, &winH );
    SDL_GL_GetDrawableSize( window, &drawW, &drawH );
    if( winW > 0 && winH > 0 ) {
        x = x * drawW / winW;
        y = y * drawH / winH;
        }
    x -= viewX;
    y -= viewY;
    /* integer division rounds toward zero, which would make the black
       bars left of and above the picture count as part of it */
    x = x < 0 ? -1 : x / viewScale;
    y = y < 0 ? -1 : y / viewScale;
    uiMouseMove( x, y );
    }



int main( int inArgC, char **inArgV ) {
    unsigned int seed = (unsigned int)time( 0 );
    int autoplay = 0;
    int simRuns = 0;
    int startFull = 0;
    int i, scale;
    int running = 1;
    SDL_Rect bounds;
    SDL_GLContext context;
    Uint32 lastTime;
    double acc = 0.0;
    int frameCount = 0;

    for( i = 1; i < inArgC; i++ ) {
        if( strcmp( inArgV[i], "--autoplay" ) == 0 ) {
            autoplay = 1;
            }
        else if( strcmp( inArgV[i], "--fonttest" ) == 0 ) {
            uiFontTest = 1;
            }
        else if( strcmp( inArgV[i], "--fullscreen" ) == 0 ) {
            startFull = 1;
            }
        else if( strcmp( inArgV[i], "--seed" ) == 0 && i + 1 < inArgC ) {
            i++;
            seed = (unsigned int)atoi( inArgV[i] );
            }
        else if( strcmp( inArgV[i], "--speed" ) == 0 && i + 1 < inArgC ) {
            i++;
            uiSpeed = atoi( inArgV[i] );
            }
        else if( strcmp( inArgV[i], "--sim" ) == 0 && i + 1 < inArgC ) {
            i++;
            simRuns = atoi( inArgV[i] );
            }
        else {
            printf( "Unknown option %s\n", inArgV[i] );
            return 1;
            }
        }

    if( simRuns > 0 ) {
        runSim( simRuns, seed );
        return 0;
        }

    if( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_AUDIO ) != 0 ) {
        printf( "SDL_Init failed:  %s\n", SDL_GetError() );
        return 1;
        }

    /* a window that comfortably fits on the screen */
    scale = 1;
    if( SDL_GetDisplayUsableBounds( 0, &bounds ) == 0 ) {
        SDL_Rect full;
        /* room for the window's title bar and border */
        int marginH = 40;
        if( SDL_GetDisplayBounds( 0, &full ) == 0 && full.h == bounds.h ) {
            /* nothing is known about panels or task bars on this
               desktop, so leave room for one of those too */
            marginH = 90;
            }
        scale = fitScale( bounds.w - 16, bounds.h - marginH );
        }

    SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
    window = SDL_CreateWindow( "Path Master", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, SCREEN_W * scale,
                               SCREEN_H * scale,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE );
    if( window == 0 ) {
        printf( "Could not open window:  %s\n", SDL_GetError() );
        SDL_Quit();
        return 1;
        }
    context = SDL_GL_CreateContext( window );
    if( context == 0 ) {
        printf( "Could not make GL context:  %s\n", SDL_GetError() );
        SDL_DestroyWindow( window );
        SDL_Quit();
        return 1;
        }
    SDL_GL_SetSwapInterval( 1 );

    gfxInit();
    artInit();
    gfxUpload();
    sndInit();

    gameSeed( seed );
    gameInit();
    if( autoplay ) {
        /* the scripted player does not get to set records */
        g.noSave = 1;
        g.best = 0;
        gameStartRun();
        }
    if( startFull ) {
        uiWantFullscreen = 1;
        }

    lastTime = SDL_GetTicks();

    while( running ) {
        SDL_Event e;
        Uint32 now;
        int drawW, drawH;
        int steps;

        while( SDL_PollEvent( &e ) ) {
            switch( e.type ) {
                case SDL_QUIT:
                    running = 0;
                    break;
                case SDL_MOUSEMOTION:
                    toMousePos( e.motion.x, e.motion.y );
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if( e.button.button == SDL_BUTTON_LEFT ) {
                        toMousePos( e.button.x, e.button.y );
                        uiMouseDown();
                        }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if( e.button.button == SDL_BUTTON_LEFT ) {
                        toMousePos( e.button.x, e.button.y );
                        uiMouseUp();
                        }
                    break;
                }
            }

        if( uiWantQuit ) {
            running = 0;
            }
        if( uiWantFullscreen ) {
            uiWantFullscreen = 0;
            fullscreen = ! fullscreen;
            SDL_SetWindowFullscreen( window,
                                     fullscreen
                                     ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0 );
            }

        /* fixed time step, 60 game frames per second */
        now = SDL_GetTicks();
        acc += (double)( now - lastTime );
        lastTime = now;
        if( acc > 100.0 ) {
            acc = 100.0;
            }
        steps = 0;
        while( acc >= 1000.0 / 60.0 ) {
            acc -= 1000.0 / 60.0;
            steps++;
            }
        for( i = 0; i < steps; i++ ) {
            int t;
            int ticks = uiSpeed;
            /* menus and effects keep moving even when paused */
            if( g.state != STATE_PLAY && ticks < 1 ) {
                ticks = 1;
                }
            if( g.state != STATE_PLAY && ticks > 1 ) {
                ticks = 1;
                }
            for( t = 0; t < ticks; t++ ) {
                gameTick();
                }
            frameCount++;
            if( autoplay && frameCount % 12 == 0 ) {
                gameAiStep();
                }
            }

        /* black bars around a sharp, whole-number scaled picture */
        SDL_GL_GetDrawableSize( window, &drawW, &drawH );
        viewScale = fitScale( drawW, drawH );
        viewX = ( drawW - SCREEN_W * viewScale ) / 2;
        viewY = ( drawH - SCREEN_H * viewScale ) / 2;

        glViewport( 0, 0, drawW, drawH );
        glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
        glClear( GL_COLOR_BUFFER_BIT );
        /* GL viewports are measured from the bottom */
        glViewport( viewX, drawH - viewY - SCREEN_H * viewScale,
                    SCREEN_W * viewScale, SCREEN_H * viewScale );
        glEnable( GL_SCISSOR_TEST );
        glScissor( viewX, drawH - viewY - SCREEN_H * viewScale,
                   SCREEN_W * viewScale, SCREEN_H * viewScale );
        gfxFrameBegin();
        uiDraw();
        gfxFrameEnd();
        glDisable( GL_SCISSOR_TEST );

        SDL_GL_SwapWindow( window );
        if( steps == 0 ) {
            SDL_Delay( 1 );
            }
        }

    SDL_GL_DeleteContext( context );
    SDL_DestroyWindow( window );
    SDL_Quit();
    return 0;
    }
