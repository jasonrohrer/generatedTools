/* obliqueVoxels.c -- a voxel-sculpture editor that renders pure pixel art
 * from an oblique top-front projection.
 *
 * Pure C89.  Links against SDL2, OpenGL + GLU, and (through gui.cpp) Dear
 * ImGui.  PNG output uses stb_image_write (built as stbiw.o).
 *
 * The idea: sculpt with voxels in a free 3D view, then render the sculpture
 * with an oblique projection in which the top face and the front face of every
 * voxel each map cleanly onto whole pixels (no shear, no squashing).  Depth
 * recedes straight up: each voxel behind another sits directly above it.  The
 * renderer shades and shadows the sculpture in 3D and bakes that lighting into
 * the top/front pixels, so you get lit pixel art for free.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <SDL.h>
#include <SDL_opengl.h>
#include <GL/glu.h>

#include "gui.h"

/* stb_image_write (implementation lives in stbiw.o) */
extern int stbi_write_png( char const *filename, int w, int h, int comp,
                           const void *data, int stride_in_bytes );

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int   clampi( int v, int lo, int hi ) { return v < lo ? lo : ( v > hi ? hi : v ); }
static float clampf( float v, float lo, float hi ) { return v < lo ? lo : ( v > hi ? hi : v ); }

/* ------------------------------------------------------------------ */
/* palette                                                             */
/* ------------------------------------------------------------------ */

#define MAX_COLORS 512

static unsigned char g_pal[ MAX_COLORS * 3 ]; /* r,g,b per color */
static int  g_palCount = 0;
static char g_palName[ 128 ] = "default";

/* Build a simple default palette: a dark->light grey ramp plus a few hues,
 * used if no .gpl is found next to the binary. */
static void paletteDefault( void )
{
    int i;
    g_palCount = 0;
    for( i = 0; i < 8; i++ ) {
        int v = i * 255 / 7;
        g_pal[ g_palCount*3+0 ] = (unsigned char)v;
        g_pal[ g_palCount*3+1 ] = (unsigned char)v;
        g_pal[ g_palCount*3+2 ] = (unsigned char)v;
        g_palCount++;
    }
    /* a few saturated hues */
    { static const unsigned char hues[8][3] = {
        {190,60,60},{60,150,80},{70,90,180},{200,170,60},
        {150,80,170},{60,170,180},{210,120,60},{230,230,140} };
      for( i = 0; i < 8; i++ ) {
        g_pal[ g_palCount*3+0 ] = hues[i][0];
        g_pal[ g_palCount*3+1 ] = hues[i][1];
        g_pal[ g_palCount*3+2 ] = hues[i][2];
        g_palCount++;
      } }
    strcpy( g_palName, "default" );
}

/* Load a GIMP .gpl palette.  Returns 1 on success. */
static int paletteLoad( const char *path )
{
    FILE *f = fopen( path, "r" );
    char line[ 256 ];
    int count = 0;
    if( !f ) return 0;
    if( !fgets( line, sizeof line, f ) ) { fclose( f ); return 0; }
    if( strncmp( line, "GIMP Palette", 12 ) != 0 ) { fclose( f ); return 0; }

    while( fgets( line, sizeof line, f ) && count < MAX_COLORS ) {
        int r, g, b;
        if( line[0] == '#' ) {
            /* try to pick up "#Palette Name: X" */
            char *p = strstr( line, "Name:" );
            if( p ) {
                p += 5;
                while( *p == ' ' || *p == '\t' ) p++;
                strncpy( g_palName, p, sizeof g_palName - 1 );
                g_palName[ sizeof g_palName - 1 ] = '\0';
                /* strip trailing newline */
                { char *e = g_palName + strlen( g_palName );
                  while( e > g_palName && ( e[-1] == '\n' || e[-1] == '\r' ) )
                      *--e = '\0'; }
            }
            continue;
        }
        if( sscanf( line, "%d %d %d", &r, &g, &b ) == 3 ) {
            g_pal[ count*3+0 ] = (unsigned char)clampi( r, 0, 255 );
            g_pal[ count*3+1 ] = (unsigned char)clampi( g, 0, 255 );
            g_pal[ count*3+2 ] = (unsigned char)clampi( b, 0, 255 );
            count++;
        }
    }
    fclose( f );
    if( count == 0 ) return 0;
    g_palCount = count;
    return 1;
}

/* ------------------------------------------------------------------ */
/* voxel model -- a spatial hash of unit voxels                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int x, y, z;            /* integer voxel position */
    int used;               /* 0 empty, 1 occupied, 2 tombstone */
    int color;              /* base palette index */
    int rampStart;          /* first palette index of shading ramp */
    int rampLen;            /* number of ramp colors (>=1) */
} Voxel;

static Voxel *g_vox = NULL;
static int    g_voxCap  = 0;    /* power of two, or 0 */
static int    g_voxUsed = 0;    /* occupied slots */
static int    g_voxTomb = 0;    /* tombstones */

static unsigned int voxHash( int x, int y, int z )
{
    unsigned int h = (unsigned int)( x * 73856093 ) ^
                     (unsigned int)( y * 19349663 ) ^
                     (unsigned int)( z * 83492791 );
    return h;
}

static void voxInit( int cap )
{
    g_vox = (Voxel*)calloc( (size_t)cap, sizeof( Voxel ) );
    g_voxCap = cap;
    g_voxUsed = 0;
    g_voxTomb = 0;
}

static void voxClear( void )
{
    if( g_vox ) memset( g_vox, 0, (size_t)g_voxCap * sizeof( Voxel ) );
    g_voxUsed = 0;
    g_voxTomb = 0;
}

/* Return index of the occupied slot for (x,y,z), or -1. */
static int voxFind( int x, int y, int z )
{
    unsigned int mask, i, start;
    if( g_voxCap == 0 ) return -1;
    mask = (unsigned int)( g_voxCap - 1 );
    start = voxHash( x, y, z ) & mask;
    i = start;
    for( ;; ) {
        Voxel *v = &g_vox[i];
        if( v->used == 0 ) return -1;
        if( v->used == 1 && v->x == x && v->y == y && v->z == z )
            return (int)i;
        i = ( i + 1 ) & mask;
        if( i == start ) return -1;
    }
}

static Voxel *voxAt( int x, int y, int z )
{
    int i = voxFind( x, y, z );
    return i < 0 ? NULL : &g_vox[i];
}

static void voxInsertRaw( int x, int y, int z, int color, int rs, int rl );

static void voxGrow( void )
{
    Voxel *old = g_vox;
    int    oldCap = g_voxCap;
    int    newCap = oldCap ? oldCap * 2 : 1024;
    int    i;
    voxInit( newCap );
    for( i = 0; i < oldCap; i++ )
        if( old[i].used == 1 )
            voxInsertRaw( old[i].x, old[i].y, old[i].z,
                          old[i].color, old[i].rampStart, old[i].rampLen );
    free( old );
}

/* Insert without growth check (caller ensures capacity). */
static void voxInsertRaw( int x, int y, int z, int color, int rs, int rl )
{
    unsigned int mask = (unsigned int)( g_voxCap - 1 );
    unsigned int i = voxHash( x, y, z ) & mask;
    int firstTomb = -1;
    for( ;; ) {
        Voxel *v = &g_vox[i];
        if( v->used == 0 ) {
            int slot = ( firstTomb >= 0 ) ? firstTomb : (int)i;
            if( firstTomb >= 0 ) g_voxTomb--;
            g_vox[slot].x = x; g_vox[slot].y = y; g_vox[slot].z = z;
            g_vox[slot].used = 1;
            g_vox[slot].color = color;
            g_vox[slot].rampStart = rs;
            g_vox[slot].rampLen = rl < 1 ? 1 : rl;
            g_voxUsed++;
            return;
        }
        if( v->used == 2 && firstTomb < 0 ) firstTomb = (int)i;
        if( v->used == 1 && v->x == x && v->y == y && v->z == z ) {
            v->color = color; v->rampStart = rs; v->rampLen = rl < 1 ? 1 : rl;
            return; /* overwrite existing */
        }
        i = ( i + 1 ) & mask;
    }
}

static void voxSet( int x, int y, int z, int color, int rs, int rl )
{
    if( g_voxCap == 0 ) voxInit( 1024 );
    if( ( g_voxUsed + g_voxTomb ) * 10 >= g_voxCap * 7 ) voxGrow();
    voxInsertRaw( x, y, z, color, rs, rl );
}

static void voxErase( int x, int y, int z )
{
    int i = voxFind( x, y, z );
    if( i >= 0 ) { g_vox[i].used = 2; g_voxUsed--; g_voxTomb++; }
}

/* ------------------------------------------------------------------ */
/* lights                                                              */
/* ------------------------------------------------------------------ */

#define MAX_LIGHTS 8

typedef struct {
    float x, y, z;
    int   color;        /* palette index */
    float intensity;
    int   enabled;
} Light;

static Light g_lights[ MAX_LIGHTS ];
static int   g_numLights = 0;
static float g_ambient = 0.25f;

static void lightAdd( float x, float y, float z, int color, float inten )
{
    if( g_numLights >= MAX_LIGHTS ) return;
    g_lights[ g_numLights ].x = x;
    g_lights[ g_numLights ].y = y;
    g_lights[ g_numLights ].z = z;
    g_lights[ g_numLights ].color = color;
    g_lights[ g_numLights ].intensity = inten;
    g_lights[ g_numLights ].enabled = 1;
    g_numLights++;
}

static void lightRemove( int idx )
{
    int i;
    if( idx < 0 || idx >= g_numLights ) return;
    for( i = idx; i < g_numLights - 1; i++ ) g_lights[i] = g_lights[i+1];
    g_numLights--;
}

/* ------------------------------------------------------------------ */
/* editor / camera / render state                                      */
/* ------------------------------------------------------------------ */

/* orbit camera */
static float cam_yaw   = 0.9f;
static float cam_pitch = 0.6f;
static float cam_dist  = 22.0f;
static float cam_tx = 0.0f, cam_ty = 2.0f, cam_tz = 0.0f;

/* tool + current paint color/ramp */
static int g_tool = 0;             /* 0 pencil, 1 erase */
static int g_pick = 15;            /* current palette index */
static int g_rampStart = 15;
static int g_rampEnd   = 15;

/* preview toggles */
static int g_previewShade = 1;
static int g_previewEdges = 1;

/* oblique render parameters */
static int g_shadingMode  = 0;     /* 0 natural, 1 palette-ramp */
static int g_voxPx        = 6;     /* voxel pixel size (horizontal & base) */
static int g_frontScrunch = 1;     /* 1..3 : front face height = voxPx/scrunch */
static int g_topScrunch   = 1;     /* 1..3 : top face height   = voxPx/scrunch */
static int g_orient       = 0;     /* 0..3 : 90-degree yaw of the oblique view */
static int g_renderZoom   = 3;     /* integer display magnification */
static int g_selLight     = -1;    /* selected light in the panel (-1 none) */

/* the baked oblique render */
static unsigned char *g_img = NULL;
static int   g_imgW = 0, g_imgH = 0;
static GLuint g_imgTex = 0;
static int   g_renderDirty = 1;

/* filenames for save/load dialogs */
static char g_fileName[ 256 ] = "sculpture.ovox";
static char g_pngName[ 256 ]  = "render.png";
static char g_gplName[ 256 ]  = "sheltzy32.gpl";
static char g_status[ 256 ]   = "";

/* window / view layout (raw-GL 3D viewport rect, filled each frame) */
static int g_viewX = 0, g_viewY = 0, g_viewW = 0, g_viewH = 0;
static int g_winH = 720;   /* full window height, for GL-y conversion */

/* mouse drag tracking for the 3D view */
static int g_dragBtn = 0;          /* 0 none, else SDL button */
static int g_downX = 0, g_downY = 0;
static int g_lastX = 0, g_lastY = 0;
static int g_moved = 0;

static void setStatus( const char *s )
{
    strncpy( g_status, s, sizeof g_status - 1 );
    g_status[ sizeof g_status - 1 ] = '\0';
}

/* ------------------------------------------------------------------ */
/* undo / redo -- a linear history of single-voxel edits               */
/* ------------------------------------------------------------------ */

typedef struct {
    int   x, y, z;
    int   hadBefore, hadAfter;
    Voxel before, after;    /* voxel contents when present */
} Edit;

static Edit *g_hist = NULL;
static int   g_histCap = 0;
static int   g_histLen = 0;   /* number of recorded edits still live */
static int   g_histPos = 0;   /* how many are currently applied */

static void histClear( void )
{
    g_histLen = 0;
    g_histPos = 0;
}

static void histPush( Edit e )
{
    if( g_histPos == g_histCap ) {
        g_histCap = g_histCap ? g_histCap * 2 : 256;
        g_hist = (Edit*)realloc( g_hist, (size_t)g_histCap * sizeof( Edit ) );
    }
    g_hist[ g_histPos++ ] = e;
    g_histLen = g_histPos;   /* truncate any redo tail */
}

/* Perform a tool edit at (x,y,z) and record it for undo.  place!=0 sets a
 * voxel; place==0 erases. */
static void editVoxel( int x, int y, int z, int place,
                       int color, int rs, int rl )
{
    Edit  e;
    Voxel *ex = voxAt( x, y, z );
    memset( &e, 0, sizeof e );
    e.x = x; e.y = y; e.z = z;
    e.hadBefore = ex ? 1 : 0;
    if( ex ) e.before = *ex;
    if( place ) {
        voxSet( x, y, z, color, rs, rl );
        e.hadAfter = 1;
        e.after = *voxAt( x, y, z );
    } else {
        if( !ex ) return;    /* nothing to erase; don't record a no-op */
        voxErase( x, y, z );
        e.hadAfter = 0;
    }
    histPush( e );
    g_renderDirty = 1;
}

static void histUndo( void )
{
    Edit *e;
    if( g_histPos == 0 ) { setStatus( "Nothing to undo" ); return; }
    e = &g_hist[ --g_histPos ];
    if( e->hadBefore ) voxSet( e->x, e->y, e->z, e->before.color,
                               e->before.rampStart, e->before.rampLen );
    else               voxErase( e->x, e->y, e->z );
    g_renderDirty = 1;
    setStatus( "Undo" );
}

static void histRedo( void )
{
    Edit *e;
    if( g_histPos >= g_histLen ) { setStatus( "Nothing to redo" ); return; }
    e = &g_hist[ g_histPos++ ];
    if( e->hadAfter ) voxSet( e->x, e->y, e->z, e->after.color,
                              e->after.rampStart, e->after.rampLen );
    else              voxErase( e->x, e->y, e->z );
    g_renderDirty = 1;
    setStatus( "Redo" );
}

/* ------------------------------------------------------------------ */
/* orientation transforms for the oblique view                         */
/* ------------------------------------------------------------------ */

/* World (x,y,z) -> view frame (u,v,w): u right, v up, w toward viewer.
 * The transform is a 90*g_orient degree rotation about the vertical axis. */
static void toUVW( int x, int y, int z, int *u, int *v, int *w )
{
    *v = y;
    switch( g_orient & 3 ) {
        case 0:  *u =  x; *w =  z; break;
        case 1:  *u =  z; *w = -x; break;
        case 2:  *u = -x; *w = -z; break;
        default: *u = -z; *w =  x; break;
    }
}

/* Inverse: view frame (u,v,w) -> world (x,y,z). */
static void fromUVW( double u, double v, double w, double *x, double *y, double *z )
{
    *y = v;
    switch( g_orient & 3 ) {
        case 0:  *x =  u; *z =  w; break;
        case 1:  *x = -w; *z =  u; break;
        case 2:  *x = -u; *z = -w; break;
        default: *x =  w; *z = -u; break;
    }
}

/* Occupancy test in the view frame (integer cell). */
static int voxOccUVW( int iu, int iv, int iw )
{
    double x, y, z;
    fromUVW( (double)iu, (double)iv, (double)iw, &x, &y, &z );
    return voxAt( (int)floor( x + 0.5 ), (int)floor( y + 0.5 ),
                  (int)floor( z + 0.5 ) ) != NULL;
}

/* ------------------------------------------------------------------ */
/* 3D preview rendering (fixed-function OpenGL)                         */
/* ------------------------------------------------------------------ */

static void camEye( double *ex, double *ey, double *ez )
{
    double cp = cos( cam_pitch ), sp = sin( cam_pitch );
    double cy = cos( cam_yaw ),   sy = sin( cam_yaw );
    *ex = cam_tx + cam_dist * cp * cy;
    *ey = cam_ty + cam_dist * sp;
    *ez = cam_tz + cam_dist * cp * sy;
}

static void setupCamera( int w, int h )
{
    double ex, ey, ez;
    glMatrixMode( GL_PROJECTION );
    glLoadIdentity();
    gluPerspective( 42.0, h ? (double)w / (double)h : 1.0, 0.1, 2000.0 );
    glMatrixMode( GL_MODELVIEW );
    glLoadIdentity();
    camEye( &ex, &ey, &ez );
    gluLookAt( ex, ey, ez, cam_tx, cam_ty, cam_tz, 0.0, 1.0, 0.0 );
}

/* face brightness used only for the 3D preview (cheap, no shadows) */
static float previewFace( float nx, float ny, float nz )
{
    if( !g_previewShade ) {
        /* flat, normal-keyed so the shape still reads */
        if( ny > 0.5f ) return 1.0f;      /* top */
        if( ny < -0.5f ) return 0.45f;    /* bottom */
        if( nz > 0.5f || nz < -0.5f ) return 0.8f; /* front/back */
        return 0.62f;                     /* sides */
    } else {
        float lit = g_ambient;
        int i;
        for( i = 0; i < g_numLights; i++ ) {
            float lx, ly, lz, len, nl;
            if( !g_lights[i].enabled ) continue;
            lx = g_lights[i].x - 0.0f; ly = g_lights[i].y; lz = g_lights[i].z;
            /* direction is fine as a whole-scene approximation for preview */
            len = (float)sqrt( lx*lx + ly*ly + lz*lz );
            if( len < 1e-4f ) continue;
            lx /= len; ly /= len; lz /= len;
            nl = nx*lx + ny*ly + nz*lz;
            if( nl > 0 ) lit += nl * g_lights[i].intensity * 0.7f;
        }
        return clampf( lit, 0.0f, 1.2f );
    }
}

static void drawCubeFace( float x, float y, float z,
                          float nx, float ny, float nz,
                          unsigned char r, unsigned char g, unsigned char b )
{
    float f = previewFace( nx, ny, nz );
    float cr = clampf( r / 255.0f * f, 0.0f, 1.0f );
    float cg = clampf( g / 255.0f * f, 0.0f, 1.0f );
    float cb = clampf( b / 255.0f * f, 0.0f, 1.0f );
    glColor3f( cr, cg, cb );

    /* emit the unit-square face at the given +/- normal of the cell (x,y,z) */
    if( ny > 0.5f ) {          /* +Y top */
        glVertex3f( x,   y+1, z   ); glVertex3f( x,   y+1, z+1 );
        glVertex3f( x+1, y+1, z+1 ); glVertex3f( x+1, y+1, z   );
    } else if( ny < -0.5f ) {  /* -Y bottom */
        glVertex3f( x,   y, z   ); glVertex3f( x+1, y, z   );
        glVertex3f( x+1, y, z+1 ); glVertex3f( x,   y, z+1 );
    } else if( nz > 0.5f ) {   /* +Z */
        glVertex3f( x,   y,   z+1 ); glVertex3f( x+1, y,   z+1 );
        glVertex3f( x+1, y+1, z+1 ); glVertex3f( x,   y+1, z+1 );
    } else if( nz < -0.5f ) {  /* -Z */
        glVertex3f( x,   y,   z ); glVertex3f( x,   y+1, z );
        glVertex3f( x+1, y+1, z ); glVertex3f( x+1, y,   z );
    } else if( nx > 0.5f ) {   /* +X */
        glVertex3f( x+1, y,   z   ); glVertex3f( x+1, y,   z+1 );
        glVertex3f( x+1, y+1, z+1 ); glVertex3f( x+1, y+1, z   );
    } else {                   /* -X */
        glVertex3f( x, y,   z   ); glVertex3f( x, y+1, z   );
        glVertex3f( x, y+1, z+1 ); glVertex3f( x, y,   z+1 );
    }
}

static void drawGrid( void )
{
    int i;
    glLineWidth( 1.0f );
    glBegin( GL_LINES );
    for( i = -16; i <= 16; i++ ) {
        float a = ( i == 0 ) ? 0.55f : 0.22f;
        glColor3f( a, a, a );
        glVertex3f( (float)i, 0.0f, -16.0f ); glVertex3f( (float)i, 0.0f, 16.0f );
        glVertex3f( -16.0f, 0.0f, (float)i ); glVertex3f( 16.0f, 0.0f, (float)i );
    }
    glEnd();
}

static void drawScene3D( void )
{
    int i;
    glEnable( GL_DEPTH_TEST );
    glDisable( GL_LIGHTING );
    glDisable( GL_TEXTURE_2D );

    drawGrid();

    /* solid faces */
    glBegin( GL_QUADS );
    for( i = 0; i < g_voxCap; i++ ) {
        Voxel *v;
        unsigned char r, g, b;
        int x, y, z;
        if( g_vox[i].used != 1 ) continue;
        v = &g_vox[i];
        x = v->x; y = v->y; z = v->z;
        r = g_pal[ v->color*3+0 ]; g = g_pal[ v->color*3+1 ]; b = g_pal[ v->color*3+2 ];
        if( !voxAt( x, y+1, z ) ) drawCubeFace( (float)x,(float)y,(float)z, 0,1,0, r,g,b );
        if( !voxAt( x, y-1, z ) ) drawCubeFace( (float)x,(float)y,(float)z, 0,-1,0, r,g,b );
        if( !voxAt( x, y, z+1 ) ) drawCubeFace( (float)x,(float)y,(float)z, 0,0,1, r,g,b );
        if( !voxAt( x, y, z-1 ) ) drawCubeFace( (float)x,(float)y,(float)z, 0,0,-1, r,g,b );
        if( !voxAt( x+1, y, z ) ) drawCubeFace( (float)x,(float)y,(float)z, 1,0,0, r,g,b );
        if( !voxAt( x-1, y, z ) ) drawCubeFace( (float)x,(float)y,(float)z, -1,0,0, r,g,b );
    }
    glEnd();

    /* voxel edges */
    if( g_previewEdges ) {
        glColor4f( 0.0f, 0.0f, 0.0f, 0.5f );
        glLineWidth( 1.0f );
        glBegin( GL_LINES );
        for( i = 0; i < g_voxCap; i++ ) {
            float x, y, z;
            if( g_vox[i].used != 1 ) continue;
            x = (float)g_vox[i].x; y = (float)g_vox[i].y; z = (float)g_vox[i].z;
            /* 12 cube edges */
            glVertex3f(x,y,z);     glVertex3f(x+1,y,z);
            glVertex3f(x,y,z+1);   glVertex3f(x+1,y,z+1);
            glVertex3f(x,y+1,z);   glVertex3f(x+1,y+1,z);
            glVertex3f(x,y+1,z+1); glVertex3f(x+1,y+1,z+1);
            glVertex3f(x,y,z);     glVertex3f(x,y+1,z);
            glVertex3f(x+1,y,z);   glVertex3f(x+1,y+1,z);
            glVertex3f(x,y,z+1);   glVertex3f(x,y+1,z+1);
            glVertex3f(x+1,y,z+1); glVertex3f(x+1,y+1,z+1);
            glVertex3f(x,y,z);     glVertex3f(x,y,z+1);
            glVertex3f(x+1,y,z);   glVertex3f(x+1,y,z+1);
            glVertex3f(x,y+1,z);   glVertex3f(x,y+1,z+1);
            glVertex3f(x+1,y+1,z); glVertex3f(x+1,y+1,z+1);
        }
        glEnd();
    }

    /* light markers */
    for( i = 0; i < g_numLights; i++ ) {
        float lx = g_lights[i].x, ly = g_lights[i].y, lz = g_lights[i].z;
        unsigned char r = g_pal[ g_lights[i].color*3+0 ];
        unsigned char g = g_pal[ g_lights[i].color*3+1 ];
        unsigned char b = g_pal[ g_lights[i].color*3+2 ];
        float s = ( i == g_selLight ) ? 0.5f : 0.32f;
        glColor3f( r/255.0f, g/255.0f, b/255.0f );
        glBegin( GL_LINES );
        glVertex3f( lx-s, ly, lz ); glVertex3f( lx+s, ly, lz );
        glVertex3f( lx, ly-s, lz ); glVertex3f( lx, ly+s, lz );
        glVertex3f( lx, ly, lz-s ); glVertex3f( lx, ly, lz+s );
        /* little diagonal cross so it reads as a star */
        glVertex3f( lx-s*0.6f, ly-s*0.6f, lz ); glVertex3f( lx+s*0.6f, ly+s*0.6f, lz );
        glVertex3f( lx-s*0.6f, ly+s*0.6f, lz ); glVertex3f( lx+s*0.6f, ly-s*0.6f, lz );
        glEnd();
    }
}

/* ------------------------------------------------------------------ */
/* picking: mouse -> world ray -> voxel or ground                      */
/* ------------------------------------------------------------------ */

/* Fill origin/dir of the world-space ray under window pixel (mx,my). */
static void mouseRay( int mx, int my,
                      double *ox, double *oy, double *oz,
                      double *dx, double *dy, double *dz )
{
    GLdouble model[16], proj[16];
    GLint    view[4];
    GLdouble nx, ny, nz, fx, fy, fz;
    double   winY, ex, ey, ez;

    /* Rebuild the exact 3D-view camera matrices here rather than reading
     * current GL state: this function runs during event handling, when the
     * live GL matrices belong to ImGui, not the voxel view. */
    glMatrixMode( GL_PROJECTION ); glPushMatrix(); glLoadIdentity();
    gluPerspective( 42.0, g_viewH ? (double)g_viewW / (double)g_viewH : 1.0,
                    0.1, 2000.0 );
    glGetDoublev( GL_PROJECTION_MATRIX, proj );
    glPopMatrix();
    glMatrixMode( GL_MODELVIEW ); glPushMatrix(); glLoadIdentity();
    camEye( &ex, &ey, &ez );
    gluLookAt( ex, ey, ez, cam_tx, cam_ty, cam_tz, 0.0, 1.0, 0.0 );
    glGetDoublev( GL_MODELVIEW_MATRIX, model );
    glPopMatrix();

    /* the viewport rect the 3D view uses, in GL (bottom-left origin) coords */
    view[0] = g_viewX;
    view[1] = g_winH - ( g_viewY + g_viewH );
    view[2] = g_viewW;
    view[3] = g_viewH;

    winY = (double)g_winH - 1.0 - (double)my;
    gluUnProject( (double)mx, winY, 0.0, model, proj, view, &nx, &ny, &nz );
    gluUnProject( (double)mx, winY, 1.0, model, proj, view, &fx, &fy, &fz );
    *ox = nx; *oy = ny; *oz = nz;
    *dx = fx - nx; *dy = fy - ny; *dz = fz - nz;
    { double len = sqrt( (*dx)*(*dx) + (*dy)*(*dy) + (*dz)*(*dz) );
      if( len > 1e-9 ) { *dx/=len; *dy/=len; *dz/=len; } }
}

/* Amanatides-Woo voxel DDA.  On hit returns 1 and fills the hit cell plus the
 * empty neighbour cell (placement cell) on the face that was entered. */
static int rayVoxel( double ox, double oy, double oz,
                     double dx, double dy, double dz,
                     int *hx, int *hy, int *hz,
                     int *px, int *py, int *pz )
{
    int x = (int)floor( ox ), y = (int)floor( oy ), z = (int)floor( oz );
    int sx = dx > 0 ? 1 : ( dx < 0 ? -1 : 0 );
    int sy = dy > 0 ? 1 : ( dy < 0 ? -1 : 0 );
    int sz = dz > 0 ? 1 : ( dz < 0 ? -1 : 0 );
    double INF = 1e30;
    double tMaxX = INF, tMaxY = INF, tMaxZ = INF;
    double tDX = INF, tDY = INF, tDZ = INF;
    int lastAxis = -1;
    int step;

    if( sx != 0 ) {
        double nb = ( sx > 0 ) ? ( x + 1 ) : x;
        tMaxX = ( nb - ox ) / dx; tDX = ( sx ) / dx;
    }
    if( sy != 0 ) {
        double nb = ( sy > 0 ) ? ( y + 1 ) : y;
        tMaxY = ( nb - oy ) / dy; tDY = ( sy ) / dy;
    }
    if( sz != 0 ) {
        double nb = ( sz > 0 ) ? ( z + 1 ) : z;
        tMaxZ = ( nb - oz ) / dz; tDZ = ( sz ) / dz;
    }

    for( step = 0; step < 4096; step++ ) {
        if( voxAt( x, y, z ) ) {
            *hx = x; *hy = y; *hz = z;
            *px = x; *py = y; *pz = z;
            if( lastAxis == 0 ) *px = x - sx;
            else if( lastAxis == 1 ) *py = y - sy;
            else if( lastAxis == 2 ) *pz = z - sz;
            return 1;
        }
        if( tMaxX < tMaxY && tMaxX < tMaxZ ) {
            x += sx; tMaxX += tDX; lastAxis = 0;
        } else if( tMaxY < tMaxZ ) {
            y += sy; tMaxY += tDY; lastAxis = 1;
        } else {
            z += sz; tMaxZ += tDZ; lastAxis = 2;
        }
    }
    return 0;
}

/* Apply the current tool at window pixel (mx,my). */
static void applyToolAt( int mx, int my )
{
    double ox, oy, oz, dx, dy, dz;
    int hx, hy, hz, px, py, pz;
    mouseRay( mx, my, &ox, &oy, &oz, &dx, &dy, &dz );

    if( rayVoxel( ox, oy, oz, dx, dy, dz, &hx, &hy, &hz, &px, &py, &pz ) ) {
        if( g_tool == 1 ) {
            editVoxel( hx, hy, hz, 0, 0, 0, 0 );
            setStatus( "Erased voxel" );
        } else {
            editVoxel( px, py, pz, 1, g_pick, g_rampStart,
                       g_rampEnd - g_rampStart + 1 );
            setStatus( "Placed voxel on face" );
        }
        return;
    }

    /* no voxel hit: pencil drops onto the ground plane y=0 */
    if( g_tool == 0 && dy < -1e-6 ) {
        double t = -oy / dy;
        if( t > 0 ) {
            int gx = (int)floor( ox + dx * t );
            int gz = (int)floor( oz + dz * t );
            if( abs( gx ) < 256 && abs( gz ) < 256 ) {
                editVoxel( gx, 0, gz, 1, g_pick, g_rampStart,
                           g_rampEnd - g_rampStart + 1 );
                setStatus( "Placed voxel on ground" );
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* oblique CPU renderer                                                */
/* ------------------------------------------------------------------ */

/* Cast a shadow ray in the view frame from surface point P toward a light at
 * L (also view-frame).  Returns 1 if blocked by a voxel before the light. */
static int shadowedUVW( double px, double py, double pz,
                        double lx, double ly, double lz )
{
    double dx = lx - px, dy = ly - py, dz = lz - pz;
    double len = sqrt( dx*dx + dy*dy + dz*dz );
    int x, y, z, sx, sy, sz, step, maxCells;
    double INF = 1e30, tMaxX, tMaxY, tMaxZ, tDX, tDY, tDZ;
    if( len < 1e-6 ) return 0;
    dx /= len; dy /= len; dz /= len;

    x = (int)floor( px ); y = (int)floor( py ); z = (int)floor( pz );
    sx = dx > 0 ? 1 : ( dx < 0 ? -1 : 0 );
    sy = dy > 0 ? 1 : ( dy < 0 ? -1 : 0 );
    sz = dz > 0 ? 1 : ( dz < 0 ? -1 : 0 );
    tMaxX = tMaxY = tMaxZ = INF; tDX = tDY = tDZ = INF;
    if( sx != 0 ) { double nb = (sx>0)?(x+1):x; tMaxX=(nb-px)/dx; tDX=sx/dx; }
    if( sy != 0 ) { double nb = (sy>0)?(y+1):y; tMaxY=(nb-py)/dy; tDY=sy/dy; }
    if( sz != 0 ) { double nb = (sz>0)?(z+1):z; tMaxZ=(nb-pz)/dz; tDZ=sz/dz; }

    maxCells = 512;
    for( step = 0; step < maxCells; step++ ) {
        double t;
        if( tMaxX < tMaxY && tMaxX < tMaxZ ) { t = tMaxX; x += sx; tMaxX += tDX; }
        else if( tMaxY < tMaxZ )             { t = tMaxY; y += sy; tMaxY += tDY; }
        else                                 { t = tMaxZ; z += sz; tMaxZ += tDZ; }
        if( t > len - 0.02 ) return 0;   /* reached the light, unobstructed */
        if( voxOccUVW( x, y, z ) ) return 1;
    }
    return 0;
}

/* Shade one surface sample.  P and N are in the view frame.  Writes an RGBA
 * pixel (a=255). */
static void shadeSample( double px, double py, double pz,
                         double nx, double ny, double nz,
                         const Voxel *v, unsigned char *out )
{
    int i;
    double accR = g_ambient, accG = g_ambient, accB = g_ambient;
    double scalarLit = g_ambient;

    for( i = 0; i < g_numLights; i++ ) {
        double lu, lv, lw, ldx, ldy, ldz, len, nl, atten, f;
        int wx, wy, wz;
        if( !g_lights[i].enabled ) continue;
        /* transform world light into the view frame */
        { int iu,iv,iw; double du,dv,dw;
          /* rotate a world vector into uvw the same way toUVW rotates cells */
          double lxw = g_lights[i].x, lyw = g_lights[i].y, lzw = g_lights[i].z;
          switch( g_orient & 3 ) {
            case 0: du = lxw; dw = lzw; break;
            case 1: du = lzw; dw = -lxw; break;
            case 2: du = -lxw; dw = -lzw; break;
            default: du = -lzw; dw = lxw; break;
          }
          dv = lyw; lu = du; lv = dv; lw = dw;
          (void)iu;(void)iv;(void)iw;(void)wx;(void)wy;(void)wz;
        }
        ldx = lu - px; ldy = lv - py; ldz = lw - pz;
        len = sqrt( ldx*ldx + ldy*ldy + ldz*ldz );
        if( len < 1e-5 ) continue;
        ldx /= len; ldy /= len; ldz /= len;
        nl = nx*ldx + ny*ldy + nz*ldz;
        if( nl <= 0 ) continue;
        /* shadow test: nudge off the surface along the normal */
        if( shadowedUVW( px + nx*0.02, py + ny*0.02, pz + nz*0.02, lu, lv, lw ) )
            continue;
        atten = g_lights[i].intensity / ( 1.0 + 0.03 * len );
        f = nl * atten;
        accR += f * g_pal[ g_lights[i].color*3+0 ] / 255.0;
        accG += f * g_pal[ g_lights[i].color*3+1 ] / 255.0;
        accB += f * g_pal[ g_lights[i].color*3+2 ] / 255.0;
        scalarLit += f;
    }

    if( g_shadingMode == 1 ) {
        /* palette-ramp shading: index the voxel's ramp by brightness.  The
         * ramp is a contiguous palette run; we orient it by luminance so the
         * darkest end always maps to the least-lit sample regardless of how
         * the run happens to be ordered in the palette. */
        int rl = v->rampLen < 1 ? 1 : v->rampLen;
        int lo = clampi( v->rampStart, 0, g_palCount - 1 );
        int hi = clampi( v->rampStart + rl - 1, 0, g_palCount - 1 );
        int lumLo = g_pal[lo*3]+g_pal[lo*3+1]+g_pal[lo*3+2];
        int lumHi = g_pal[hi*3]+g_pal[hi*3+1]+g_pal[hi*3+2];
        double t = clampf( (float)scalarLit, 0.0f, 1.0f );
        int idx = (int)( t * rl );
        if( idx >= rl ) idx = rl - 1;
        if( lumLo > lumHi ) idx = rl - 1 - idx;   /* run stored light->dark */
        idx = clampi( v->rampStart + idx, 0, g_palCount - 1 );
        out[0] = g_pal[ idx*3+0 ];
        out[1] = g_pal[ idx*3+1 ];
        out[2] = g_pal[ idx*3+2 ];
    } else {
        /* natural shading: base color modulated by (possibly colored) light */
        double br = g_pal[ v->color*3+0 ] / 255.0;
        double bg = g_pal[ v->color*3+1 ] / 255.0;
        double bb = g_pal[ v->color*3+2 ] / 255.0;
        out[0] = (unsigned char)clampi( (int)( br * accR * 255.0 + 0.5 ), 0, 255 );
        out[1] = (unsigned char)clampi( (int)( bg * accG * 255.0 + 0.5 ), 0, 255 );
        out[2] = (unsigned char)clampi( (int)( bb * accB * 255.0 + 0.5 ), 0, 255 );
    }
    out[3] = 255;
}

/* Render the sculpture to the g_img RGBA buffer via the oblique projection. */
static void renderOblique( void )
{
    int i;
    int have = 0;
    int umin=0, umax=0, vmin=0, vmax=0, wmin=0, wmax=0;
    int W, frontH, topH;
    int imgW, imgH;
    long bottomMax = 0, topMin = 0;   /* image-y extents (before offset) */
    float *zbuf;
    int C;

    W      = g_voxPx;
    frontH = g_voxPx / g_frontScrunch; if( frontH < 1 ) frontH = 1;
    topH   = g_voxPx / g_topScrunch;   if( topH < 1 ) topH = 1;

    /* gather view-frame bounds */
    for( i = 0; i < g_voxCap; i++ ) {
        int u, v, w;
        if( g_vox[i].used != 1 ) continue;
        toUVW( g_vox[i].x, g_vox[i].y, g_vox[i].z, &u, &v, &w );
        if( !have ) { umin=umax=u; vmin=vmax=v; wmin=wmax=w; have=1; }
        if( u<umin ) umin=u;
        if( u>umax ) umax=u;
        if( v<vmin ) vmin=v;
        if( v>vmax ) vmax=v;
        if( w<wmin ) wmin=w;
        if( w>wmax ) wmax=w;
    }

    if( !have ) {
        /* empty scene: a tiny transparent image */
        imgW = imgH = 1;
        free( g_img );
        g_img = (unsigned char*)calloc( (size_t)imgW*imgH*4, 1 );
        g_imgW = imgW; g_imgH = imgH;
        return;
    }

    /* bottomY(v,w) = C - v*frontH + w*topH  (image y grows downward).
     * front face rows: [bottomY-frontH, bottomY)
     * top   face rows: [bottomY-frontH-topH, bottomY-frontH) */
    {
        int v, w;
        long lo = 0x7fffffffL, hi = -0x7fffffffL;
        for( v = vmin; v <= vmax; v++ ) for( w = wmin; w <= wmax; w++ ) {
            long by = - (long)v * frontH + (long)w * topH;
            long top = by - frontH - topH;   /* highest pixel of this column */
            long bot = by;                    /* lowest pixel (front bottom)  */
            if( top < lo ) lo = top;
            if( bot > hi ) hi = bot;
        }
        topMin = lo; bottomMax = hi;
    }

    imgW = ( umax - umin + 1 ) * W;
    imgH = (int)( bottomMax - topMin );
    if( imgW < 1 ) imgW = 1;
    if( imgH < 1 ) imgH = 1;

    free( g_img );
    g_img = (unsigned char*)calloc( (size_t)imgW*imgH*4, 1 );
    zbuf  = (float*)malloc( (size_t)imgW*imgH * sizeof( float ) );
    for( i = 0; i < imgW*imgH; i++ ) zbuf[i] = -1e30f;
    g_imgW = imgW; g_imgH = imgH;

    /* rasterize front + top faces of every voxel with a per-pixel depth test */
    for( C = 0; C < g_voxCap; C++ ) {
        int u, v, w, x0, py, px, ny;
        long by;
        Voxel *vox;
        if( g_vox[C].used != 1 ) continue;
        vox = &g_vox[C];
        toUVW( vox->x, vox->y, vox->z, &u, &v, &w );
        x0 = ( u - umin ) * W;
        by = - (long)v * frontH + (long)w * topH - topMin;  /* into image space */

        /* ---- front face (+w side, normal (0,0,1)) ---- */
        {
            int fy0 = (int)( by - frontH ), fy1 = (int)by;
            /* visible only if nothing occupies the cell in front */
            int occFront = voxOccUVW( u, v, w + 1 );
            if( !occFront ) {
                for( py = fy0; py < fy1; py++ ) {
                    if( py < 0 || py >= imgH ) continue;
                    for( ny = 0; ny < W; ny++ ) {
                        double fx, fyf, Px, Py, Pz;
                        float depth;
                        px = x0 + ny;
                        if( px < 0 || px >= imgW ) continue;
                        fx  = ( ny + 0.5 ) / W;                 /* 0..1 across u */
                        fyf = ( py - fy0 + 0.5 ) / frontH;      /* 0 top .. 1 bot */
                        Px = u + fx;
                        Py = v + ( 1.0 - fyf );
                        Pz = w + 1.0;                            /* front plane */
                        depth = (float)( ( v + (1.0-fyf) ) * topH + (w+1.0) * frontH );
                        if( depth > zbuf[ py*imgW + px ] ) {
                            zbuf[ py*imgW + px ] = depth;
                            shadeSample( Px, Py, Pz, 0,0,1, vox,
                                         &g_img[ (py*imgW + px)*4 ] );
                        }
                    }
                }
            }
        }

        /* ---- top face (+v side, normal (0,1,0)) ---- */
        {
            int ty1 = (int)( by - frontH ), ty0 = ty1 - topH;
            int occTop = voxOccUVW( u, v + 1, w );
            if( !occTop ) {
                for( py = ty0; py < ty1; py++ ) {
                    if( py < 0 || py >= imgH ) continue;
                    for( ny = 0; ny < W; ny++ ) {
                        double fx, fzf, Px, Py, Pz;
                        float depth;
                        px = x0 + ny;
                        if( px < 0 || px >= imgW ) continue;
                        fx  = ( ny + 0.5 ) / W;
                        fzf = ( py - ty0 + 0.5 ) / topH;   /* 0 back(top) 1 front */
                        Px = u + fx;
                        Py = v + 1.0;                       /* top plane */
                        Pz = w + fzf;
                        depth = (float)( ( v + 1.0 ) * topH + ( w + fzf ) * frontH );
                        if( depth > zbuf[ py*imgW + px ] ) {
                            zbuf[ py*imgW + px ] = depth;
                            shadeSample( Px, Py, Pz, 0,1,0, vox,
                                         &g_img[ (py*imgW + px)*4 ] );
                        }
                    }
                }
            }
        }
    }

    free( zbuf );
}

static void uploadRenderTexture( void )
{
    if( !g_img ) return;
    if( g_imgTex == 0 ) glGenTextures( 1, &g_imgTex );
    glBindTexture( GL_TEXTURE_2D, g_imgTex );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
    glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
    glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, g_imgW, g_imgH, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, g_img );
    glBindTexture( GL_TEXTURE_2D, 0 );
}

static void ensureRender( void )
{
    if( g_renderDirty ) {
        renderOblique();
        uploadRenderTexture();
        g_renderDirty = 0;
    }
}

static void savePNG( const char *path )
{
    ensureRender();
    if( g_img && g_imgW > 0 && g_imgH > 0 &&
        stbi_write_png( path, g_imgW, g_imgH, 4, g_img, g_imgW*4 ) ) {
        char msg[300];
        sprintf( msg, "Saved PNG %dx%d -> %s", g_imgW, g_imgH, path );
        setStatus( msg );
    } else {
        setStatus( "PNG save failed" );
    }
}

/* ------------------------------------------------------------------ */
/* bespoke .ovox save / load                                           */
/* ------------------------------------------------------------------ */

static void saveSculpture( const char *path )
{
    FILE *f = fopen( path, "w" );
    int i;
    if( !f ) { setStatus( "Save failed" ); return; }
    fprintf( f, "OBLIQUEVOXELS 1\n" );
    fprintf( f, "PALETTE %s %d\n", g_palName, g_palCount );
    for( i = 0; i < g_palCount; i++ )
        fprintf( f, "C %d %d %d\n", g_pal[i*3+0], g_pal[i*3+1], g_pal[i*3+2] );
    fprintf( f, "AMBIENT %.4f\n", g_ambient );
    for( i = 0; i < g_numLights; i++ )
        fprintf( f, "L %.4f %.4f %.4f %d %.4f %d\n",
                 g_lights[i].x, g_lights[i].y, g_lights[i].z,
                 g_lights[i].color, g_lights[i].intensity, g_lights[i].enabled );
    fprintf( f, "RENDER %d %d %d %d %d\n", g_shadingMode, g_voxPx,
             g_frontScrunch, g_topScrunch, g_orient );
    /* one voxel per line: V x y z color rampStart rampLen */
    for( i = 0; i < g_voxCap; i++ ) {
        Voxel *v;
        if( g_vox[i].used != 1 ) continue;
        v = &g_vox[i];
        fprintf( f, "V %d %d %d %d %d %d\n",
                 v->x, v->y, v->z, v->color, v->rampStart, v->rampLen );
    }
    fclose( f );
    { char msg[300]; sprintf( msg, "Saved %d voxels -> %s", g_voxUsed, path );
      setStatus( msg ); }
}

static int loadSculpture( const char *path )
{
    FILE *f = fopen( path, "r" );
    char line[ 256 ];
    int palIdx = 0, palExpected = 0;
    if( !f ) { setStatus( "Open failed" ); return 0; }
    if( !fgets( line, sizeof line, f ) ||
        strncmp( line, "OBLIQUEVOXELS", 13 ) != 0 ) {
        fclose( f ); setStatus( "Not an .ovox file" ); return 0;
    }
    voxClear();
    histClear();
    g_numLights = 0;

    while( fgets( line, sizeof line, f ) ) {
        if( line[0] == 'P' && line[1] == 'A' ) {
            char nm[128]; int n = 0;
            if( sscanf( line, "PALETTE %127s %d", nm, &n ) >= 1 ) {
                strncpy( g_palName, nm, sizeof g_palName - 1 );
                g_palName[ sizeof g_palName - 1 ] = '\0';
                palExpected = n; palIdx = 0;
            }
        } else if( line[0] == 'C' && line[1] == ' ' ) {
            int r,g,b;
            if( sscanf( line, "C %d %d %d", &r,&g,&b ) == 3 &&
                palIdx < MAX_COLORS ) {
                g_pal[palIdx*3+0]=(unsigned char)r;
                g_pal[palIdx*3+1]=(unsigned char)g;
                g_pal[palIdx*3+2]=(unsigned char)b;
                palIdx++;
            }
        } else if( line[0] == 'A' ) {
            float a; if( sscanf( line, "AMBIENT %f", &a ) == 1 ) g_ambient = a;
        } else if( line[0] == 'L' && line[1] == ' ' ) {
            float x,y,z,inten; int col,en;
            if( sscanf( line, "L %f %f %f %d %f %d",
                        &x,&y,&z,&col,&inten,&en ) == 6 &&
                g_numLights < MAX_LIGHTS ) {
                g_lights[g_numLights].x=x; g_lights[g_numLights].y=y;
                g_lights[g_numLights].z=z; g_lights[g_numLights].color=col;
                g_lights[g_numLights].intensity=inten;
                g_lights[g_numLights].enabled=en;
                g_numLights++;
            }
        } else if( line[0] == 'R' ) {
            sscanf( line, "RENDER %d %d %d %d %d", &g_shadingMode, &g_voxPx,
                    &g_frontScrunch, &g_topScrunch, &g_orient );
        } else if( line[0] == 'V' && line[1] == ' ' ) {
            int x,y,z,col,rs,rl;
            if( sscanf( line, "V %d %d %d %d %d %d",
                        &x,&y,&z,&col,&rs,&rl ) == 6 )
                voxSet( x,y,z,col,rs,rl );
        }
    }
    fclose( f );
    if( palIdx > 0 ) g_palCount = palIdx;
    (void)palExpected;
    g_pick = clampi( g_pick, 0, g_palCount-1 );
    g_rampStart = clampi( g_rampStart, 0, g_palCount-1 );
    g_rampEnd = clampi( g_rampEnd, g_rampStart, g_palCount-1 );
    g_renderDirty = 1;
    { char msg[300]; sprintf( msg, "Loaded %d voxels from %s", g_voxUsed, path );
      setStatus( msg ); }
    return 1;
}

/* ------------------------------------------------------------------ */
/* demo scene                                                          */
/* ------------------------------------------------------------------ */

static void buildDemoScene( void )
{
    int x, z, y;
    /* a little 6x6 platform */
    for( x = 0; x < 6; x++ ) for( z = 0; z < 6; z++ )
        voxSet( x, 0, z, 12, 12, 1 );
    /* a stubby tower */
    for( y = 1; y <= 4; y++ ) {
        voxSet( 2, y, 2, 5, 5, 1 );
        voxSet( 3, y, 2, 5, 5, 1 );
        voxSet( 2, y, 3, 5, 5, 1 );
        voxSet( 3, y, 3, 5, 5, 1 );
    }
    /* a cap */
    voxSet( 2, 5, 2, 15, 15, 1 );
    voxSet( 3, 5, 2, 15, 15, 1 );
    voxSet( 2, 5, 3, 15, 15, 1 );
    voxSet( 3, 5, 3, 15, 15, 1 );

    lightAdd( 10.0f, 12.0f, 6.0f, g_palCount>34?33:g_palCount-1, 1.1f );
    cam_tx = 3.0f; cam_ty = 2.5f; cam_tz = 3.0f;
}

/* ------------------------------------------------------------------ */
/* view presets                                                        */
/* ------------------------------------------------------------------ */

static void setView( float yaw, float pitch )
{
    cam_yaw = yaw; cam_pitch = pitch;
}

/* ------------------------------------------------------------------ */
/* UI panels                                                           */
/* ------------------------------------------------------------------ */

static int  g_showFront = 0, g_showTop = 0;   /* unused reserved */

static void openFileDialog( const char *id ) { gui_open_popup( id ); }

static void buildMenuBar( int *quit )
{
    if( !gui_begin_main_menu_bar() ) return;
    if( gui_begin_menu( "File" ) ) {
        if( gui_menu_item( "New", NULL, 1 ) ) {
            voxClear(); g_numLights = 0; histClear(); g_renderDirty = 1;
            setStatus( "New sculpture" );
        }
        if( gui_menu_item( "Open .ovox...", NULL, 1 ) ) openFileDialog( "Open" );
        if( gui_menu_item( "Save .ovox...", NULL, 1 ) ) openFileDialog( "Save" );
        gui_separator();
        if( gui_menu_item( "Load palette (.gpl)...", NULL, 1 ) )
            openFileDialog( "Palette" );
        if( gui_menu_item( "Export PNG...", NULL, 1 ) ) openFileDialog( "PNG" );
        gui_separator();
        if( gui_menu_item( "Quit", NULL, 1 ) ) *quit = 1;
        gui_end_menu();
    }
    if( gui_begin_menu( "Edit" ) ) {
        if( gui_menu_item( "Undo", "Ctrl+Z", g_histPos > 0 ) ) histUndo();
        if( gui_menu_item( "Redo", "Ctrl+Y", g_histPos < g_histLen ) ) histRedo();
        gui_separator();
        if( gui_menu_item( "Pencil", "B", 1 ) ) g_tool = 0;
        if( gui_menu_item( "Erase",  "E", 1 ) ) g_tool = 1;
        gui_end_menu();
    }
    if( gui_begin_menu( "View" ) ) {
        if( gui_menu_item( "Front",  NULL, 1 ) ) setView( -1.5708f, 0.0f );
        if( gui_menu_item( "Back",   NULL, 1 ) ) setView(  1.5708f, 0.0f );
        if( gui_menu_item( "Left",   NULL, 1 ) ) setView(  3.1416f, 0.0f );
        if( gui_menu_item( "Right",  NULL, 1 ) ) setView(  0.0f,    0.0f );
        if( gui_menu_item( "Top",    NULL, 1 ) ) setView( -1.5708f, 1.5620f );
        if( gui_menu_item( "Bottom", NULL, 1 ) ) setView( -1.5708f,-1.5620f );
        gui_separator();
        if( gui_menu_item( "Iso",    NULL, 1 ) ) setView( 0.9f, 0.6f );
        gui_end_menu();
    }
    if( gui_begin_menu( "Display" ) ) {
        gui_menu_item_check( "Shade preview", NULL, &g_previewShade );
        gui_menu_item_check( "Voxel edges",   NULL, &g_previewEdges );
        gui_end_menu();
    }
    (void)g_showFront; (void)g_showTop;
    gui_end_main_menu_bar();
}

static void colorRGB( int idx, int *r, int *g, int *b )
{
    idx = clampi( idx, 0, g_palCount-1 );
    *r = g_pal[idx*3+0]; *g = g_pal[idx*3+1]; *b = g_pal[idx*3+2];
}

static void buildLeftPanel( float top, float h )
{
    static const char *shadeItems[2] = { "Natural", "Palette ramp" };
    int r,g,b;
    char buf[128];

    gui_set_next_window_pos( 0.0f, top );
    gui_set_next_window_size( 240.0f, h );
    gui_begin( "Tools", 1 );

    gui_separator_text( "Tool" );
    gui_radio_int( "Pencil", &g_tool, 0 ); gui_same_line();
    gui_radio_int( "Erase",  &g_tool, 1 );

    gui_separator_text( "Paint color / ramp" );
    colorRGB( g_pick, &r, &g, &b );
    gui_color_button( "cur", r, g, b, 1, 24.0f );
    gui_same_line();
    if( g_rampEnd > g_rampStart )
        sprintf( buf, "ramp %d..%d (%d)", g_rampStart, g_rampEnd,
                 g_rampEnd-g_rampStart+1 );
    else
        sprintf( buf, "index %d (flat)", g_pick );
    gui_text( buf );
    gui_text( "click=pick  drag=ramp" );
    gui_palette_grid( g_pal, g_palCount, 8, 22.0f,
                      &g_pick, &g_rampStart, &g_rampEnd );

    gui_separator_text( "Shading mode" );
    if( gui_combo( "mode", &g_shadingMode, shadeItems, 2 ) ) g_renderDirty = 1;
    if( gui_slider_float( "ambient", &g_ambient, 0.0f, 1.0f, "%.2f" ) )
        g_renderDirty = 1;

    gui_separator_text( "Lights" );
    { int i;
      for( i = 0; i < g_numLights; i++ ) {
        int lr,lg,lb;
        gui_push_id( i );
        colorRGB( g_lights[i].color, &lr,&lg,&lb );
        if( gui_color_button( "lc", lr,lg,lb, i==g_selLight, 18.0f ) )
            g_selLight = i;
        gui_same_line();
        sprintf( buf, "Light %d", i );
        if( gui_small_button( buf ) ) g_selLight = i;
        gui_same_line();
        if( gui_small_button( "x" ) ) {
            lightRemove( i ); g_renderDirty = 1;
            if( g_selLight >= g_numLights ) g_selLight = g_numLights-1;
            gui_pop_id();
            break;
        }
        gui_pop_id();
      }
    }
    if( g_numLights < MAX_LIGHTS && gui_button( "Add light" ) ) {
        lightAdd( cam_tx + 6.0f, cam_ty + 8.0f, cam_tz + 6.0f,
                  g_palCount>34?33:g_palCount-1, 1.0f );
        g_selLight = g_numLights - 1;
        g_renderDirty = 1;
    }

    if( g_selLight >= 0 && g_selLight < g_numLights ) {
        Light *L = &g_lights[ g_selLight ];
        gui_separator_text( "Selected light" );
        if( gui_slider_float( "x", &L->x, -40.0f, 40.0f, "%.1f" ) ) g_renderDirty=1;
        if( gui_slider_float( "y", &L->y, -40.0f, 40.0f, "%.1f" ) ) g_renderDirty=1;
        if( gui_slider_float( "z", &L->z, -40.0f, 40.0f, "%.1f" ) ) g_renderDirty=1;
        if( gui_slider_float( "intensity", &L->intensity, 0.0f, 3.0f, "%.2f" ) )
            g_renderDirty=1;
        if( gui_checkbox( "enabled", &L->enabled ) ) g_renderDirty=1;
        if( gui_button( "Set light color from paint" ) ) {
            L->color = g_pick; g_renderDirty = 1;
        }
    }

    gui_end();
}

static void buildRightPanel( float top, float h, float winW )
{
    char buf[128];
    float pw = 300.0f;
    static const char *orientItems[4] = { "0", "90", "180", "270" };

    gui_set_next_window_pos( winW - pw, top );
    gui_set_next_window_size( pw, h );
    gui_begin( "Oblique Render", 1 );

    if( gui_slider_int( "voxel px", &g_voxPx, 1, 12 ) )       g_renderDirty = 1;
    if( gui_slider_int( "front scrunch", &g_frontScrunch, 1, 3 ) ) g_renderDirty = 1;
    if( gui_slider_int( "top scrunch", &g_topScrunch, 1, 3 ) )     g_renderDirty = 1;
    if( gui_combo( "orient", &g_orient, orientItems, 4 ) )    g_renderDirty = 1;
    gui_slider_int( "zoom", &g_renderZoom, 1, 12 );

    ensureRender();
    sprintf( buf, "render: %d x %d px", g_imgW, g_imgH );
    gui_text( buf );

    if( gui_button( "Export PNG..." ) ) openFileDialog( "PNG" );

    gui_separator();
    { float aw, ah;
      gui_content_avail( &aw, &ah );
      if( gui_begin_child( "renderview", aw, ah, 1 ) ) {
        if( g_imgTex && g_imgW > 0 )
            gui_image( g_imgTex, (float)g_imgW * g_renderZoom,
                       (float)g_imgH * g_renderZoom );
      }
      gui_end_child();
    }
    gui_end();
}

static void buildDialogs( void )
{
    if( gui_begin_popup_modal( "Save" ) ) {
        gui_text( "Save sculpture as:" );
        if( gui_input_text( "##savef", g_fileName, sizeof g_fileName ) ) {
            saveSculpture( g_fileName ); gui_close_current_popup();
        }
        if( gui_button( "Save" ) ) { saveSculpture( g_fileName ); gui_close_current_popup(); }
        gui_same_line();
        if( gui_button( "Cancel" ) ) gui_close_current_popup();
        gui_end_popup();
    }
    if( gui_begin_popup_modal( "Open" ) ) {
        gui_text( "Open sculpture:" );
        if( gui_input_text( "##openf", g_fileName, sizeof g_fileName ) ) {
            loadSculpture( g_fileName ); gui_close_current_popup();
        }
        if( gui_button( "Open" ) ) { loadSculpture( g_fileName ); gui_close_current_popup(); }
        gui_same_line();
        if( gui_button( "Cancel" ) ) gui_close_current_popup();
        gui_end_popup();
    }
    if( gui_begin_popup_modal( "PNG" ) ) {
        gui_text( "Export PNG as:" );
        if( gui_input_text( "##pngf", g_pngName, sizeof g_pngName ) ) {
            savePNG( g_pngName ); gui_close_current_popup();
        }
        if( gui_button( "Export" ) ) { savePNG( g_pngName ); gui_close_current_popup(); }
        gui_same_line();
        if( gui_button( "Cancel" ) ) gui_close_current_popup();
        gui_end_popup();
    }
    if( gui_begin_popup_modal( "Palette" ) ) {
        gui_text( "Load GIMP palette (.gpl):" );
        if( gui_input_text( "##gplf", g_gplName, sizeof g_gplName ) ) {
            if( paletteLoad( g_gplName ) ) g_renderDirty = 1;
            gui_close_current_popup();
        }
        if( gui_button( "Load" ) ) {
            if( paletteLoad( g_gplName ) ) { g_renderDirty = 1;
                g_pick = clampi(g_pick,0,g_palCount-1);
                g_rampStart = clampi(g_rampStart,0,g_palCount-1);
                g_rampEnd = clampi(g_rampEnd,g_rampStart,g_palCount-1);
                setStatus( "Palette loaded" );
            } else setStatus( "Palette load failed" );
            gui_close_current_popup();
        }
        gui_same_line();
        if( gui_button( "Cancel" ) ) gui_close_current_popup();
        gui_end_popup();
    }
}

static void buildStatusBar( float winW, float winH )
{
    gui_set_next_window_pos( 240.0f, winH - 26.0f );
    gui_set_next_window_size( winW - 240.0f - 300.0f, 26.0f );
    gui_begin( "status", 1 );
    gui_text( g_status );
    gui_end();
}

/* ------------------------------------------------------------------ */
/* input handling for the 3D view                                      */
/* ------------------------------------------------------------------ */

static void orbitDrag( int dx, int dy )
{
    cam_yaw   += dx * 0.008f;
    cam_pitch += dy * 0.008f;
    cam_pitch = clampf( cam_pitch, -1.5620f, 1.5620f );
}

static void panDrag( int dx, int dy )
{
    /* move the target in the camera's right/up plane */
    double cp = cos( cam_pitch ), sp = sin( cam_pitch );
    double cy = cos( cam_yaw ),   sy = sin( cam_yaw );
    double fx = -cp*cy, fy = -sp, fz = -cp*sy;      /* forward */
    double rx, ry, rz, ux, uy, uz, s;
    /* right = forward x worldUp */
    rx = fz*0.0 - fy*1.0; ry = fx*1.0 - fz*0.0; rz = fy*0.0 - fx*1.0;
    { double l = sqrt(rx*rx+ry*ry+rz*rz); if(l>1e-6){rx/=l;ry/=l;rz/=l;} }
    /* up = right x forward */
    ux = ry*fz - rz*fy; uy = rz*fx - rx*fz; uz = rx*fy - ry*fx;
    s = cam_dist * 0.0016;
    cam_tx += (float)( ( -dx*rx + dy*ux ) * s );
    cam_ty += (float)( ( -dx*ry + dy*uy ) * s );
    cam_tz += (float)( ( -dx*rz + dy*uz ) * s );
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main( int argc, char **argv )
{
    SDL_Window   *window;
    SDL_GLContext glctx;
    int running = 1;
    const char *quitEnv = getenv( "OV_QUIT" );
    int quitAfter = quitEnv ? ( atoi( quitEnv ) < 1 ? 1 : atoi( quitEnv ) ) : 0;
    const char *exportEnv = getenv( "OV_EXPORT" );  /* auto-export PNG on quit */
    long frameNum = 0;

    /* palette: try the bundled sheltzy32.gpl, else a default */
    if( !paletteLoad( "sheltzy32.gpl" ) ) paletteDefault();
    g_pick = clampi( 15, 0, g_palCount-1 );
    g_rampStart = g_rampEnd = g_pick;

    if( SDL_Init( SDL_INIT_VIDEO ) != 0 ) {
        fprintf( stderr, "SDL_Init failed: %s\n", SDL_GetError() );
        return 1;
    }
    SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
    SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 24 );

    window = SDL_CreateWindow( "obliqueVoxels",
                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                1200, 720,
                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                SDL_WINDOW_ALLOW_HIGHDPI );
    if( !window ) { fprintf( stderr, "CreateWindow: %s\n", SDL_GetError() );
                    SDL_Quit(); return 1; }
    glctx = SDL_GL_CreateContext( window );
    if( !glctx ) { fprintf( stderr, "GL context: %s\n", SDL_GetError() );
                   SDL_DestroyWindow( window ); SDL_Quit(); return 1; }
    SDL_GL_MakeCurrent( window, glctx );
    SDL_GL_SetSwapInterval( 1 );

    if( gui_init( window, glctx ) ) {
        fprintf( stderr, "GUI init failed\n" ); return 1;
    }

    voxInit( 1024 );
    if( argc > 1 ) {
        if( !loadSculpture( argv[1] ) ) buildDemoScene();
    } else {
        buildDemoScene();
    }
    setStatus( "Left-drag: orbit  |  Left-click: paint/erase  |  "
               "Mid/Right-drag: pan  |  Wheel: zoom" );

    while( running ) {
        SDL_Event ev;
        int winW, winH;
        float menuH;
        int overGui;

        while( SDL_PollEvent( &ev ) ) {
            gui_process_event( &ev );
            overGui = gui_any_window_hovered();
            switch( ev.type ) {
                case SDL_QUIT: running = 0; break;
                case SDL_KEYDOWN:
                    if( !gui_want_capture_keyboard() ) {
                        int ctrl = ( ev.key.keysym.mod & KMOD_CTRL ) ? 1 : 0;
                        int shift = ( ev.key.keysym.mod & KMOD_SHIFT ) ? 1 : 0;
                        SDL_Keycode k = ev.key.keysym.sym;
                        if( ctrl && k == SDLK_z ) { if( shift ) histRedo(); else histUndo(); }
                        else if( ctrl && k == SDLK_y ) histRedo();
                        else if( k == SDLK_b ) g_tool = 0;
                        else if( k == SDLK_e ) g_tool = 1;
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if( !overGui ) {
                        g_dragBtn = ev.button.button;
                        g_downX = g_lastX = ev.button.x;
                        g_downY = g_lastY = ev.button.y;
                        g_moved = 0;
                    }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if( g_dragBtn == ev.button.button ) {
                        if( g_dragBtn == SDL_BUTTON_LEFT && !g_moved )
                            applyToolAt( ev.button.x, ev.button.y );
                        g_dragBtn = 0;
                    }
                    break;
                case SDL_MOUSEMOTION:
                    if( g_dragBtn ) {
                        int dx = ev.motion.x - g_lastX;
                        int dy = ev.motion.y - g_lastY;
                        g_lastX = ev.motion.x; g_lastY = ev.motion.y;
                        if( abs( ev.motion.x - g_downX ) +
                            abs( ev.motion.y - g_downY ) > 3 ) g_moved = 1;
                        if( g_dragBtn == SDL_BUTTON_LEFT )
                            orbitDrag( dx, dy );
                        else
                            panDrag( dx, dy );
                    }
                    break;
                case SDL_MOUSEWHEEL:
                    if( !overGui ) {
                        cam_dist *= ( ev.wheel.y > 0 ) ? 0.9f : 1.1111f;
                        cam_dist = clampf( cam_dist, 1.5f, 400.0f );
                    }
                    break;
                default: break;
            }
        }

        SDL_GetWindowSize( window, &winW, &winH );
        g_winH = winH;
        menuH = gui_main_menu_bar_height();

        /* 3D view occupies the middle strip between the two side panels */
        g_viewX = 240;
        g_viewY = (int)menuH;
        g_viewW = winW - 240 - 300;
        g_viewH = winH - (int)menuH - 26;
        if( g_viewW < 1 ) g_viewW = 1;
        if( g_viewH < 1 ) g_viewH = 1;

        glClearColor( 0.10f, 0.11f, 0.13f, 1.0f );
        glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

        /* raw-GL 3D viewport (origin is bottom-left in GL) */
        glViewport( g_viewX, winH - ( g_viewY + g_viewH ), g_viewW, g_viewH );
        glEnable( GL_SCISSOR_TEST );
        glScissor( g_viewX, winH - ( g_viewY + g_viewH ), g_viewW, g_viewH );
        glClearColor( 0.14f, 0.15f, 0.18f, 1.0f );
        glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
        setupCamera( g_viewW, g_viewH );
        drawScene3D();
        glDisable( GL_SCISSOR_TEST );
        glViewport( 0, 0, winW, winH );

        gui_new_frame( window );
        { int quit = 0;
          buildMenuBar( &quit );
          if( quit ) running = 0; }
        buildLeftPanel( menuH, winH - menuH - 26.0f );
        buildRightPanel( menuH, winH - menuH - 26.0f, (float)winW );
        buildStatusBar( (float)winW, (float)winH );
        buildDialogs();
        gui_render();

        SDL_GL_SwapWindow( window );

        if( quitAfter && ++frameNum >= quitAfter ) running = 0;
    }

    if( exportEnv ) savePNG( exportEnv );

    gui_shutdown();
    if( g_imgTex ) glDeleteTextures( 1, &g_imgTex );
    free( g_img );
    free( g_vox );
    SDL_GL_DeleteContext( glctx );
    SDL_DestroyWindow( window );
    SDL_Quit();
    return 0;
}
