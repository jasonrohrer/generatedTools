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
#include "fs.h"

/* stb_image_write (implementation lives in stbiw.o) */
extern int stbi_write_png( char const *filename, int w, int h, int comp,
                           const void *data, int stride_in_bytes );

/* stb_image reader (implementation lives in stbi.o) -- used to import a PNG
 * (with alpha) as a flat wall of voxels. */
extern unsigned char *stbi_load( char const *filename, int *x, int *y,
                                 int *comp, int req_comp );
extern void stbi_image_free( void *retval_from_stbi_load );

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
    int sel;                /* 1 if part of the current selection */
    int smoothFaces;        /* per-face smooth bitmask; bit (1<<faceDir6) set =
                             * that face is shaded with a fitted surface normal.
                             * faceDir6 order: +Y0 -Y1 +Z2 -Z3 +X4 -X5. */
} Voxel;

/* Map a face's (axis) outward normal to its 0..5 direction index.  Order is
 * shared with the NR[6] face table used elsewhere: +Y0 -Y1 +Z2 -Z3 +X4 -X5. */
static int faceDir6( double nx, double ny, double nz )
{
    if( ny >  0.5 ) return 0;
    if( ny < -0.5 ) return 1;
    if( nz >  0.5 ) return 2;
    if( nz < -0.5 ) return 3;
    if( nx >  0.5 ) return 4;
    return 5;
}

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
        if( old[i].used == 1 ) {
            Voxel *nv;
            voxInsertRaw( old[i].x, old[i].y, old[i].z,
                          old[i].color, old[i].rampStart, old[i].rampLen );
            /* voxInsertRaw resets sel/smooth on a fresh slot; carry them over */
            nv = voxAt( old[i].x, old[i].y, old[i].z );
            if( nv ) { nv->sel = old[i].sel; nv->smoothFaces = old[i].smoothFaces; }
        }
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
            g_vox[slot].sel = 0;
            g_vox[slot].smoothFaces = 0;
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
/* smooth shading -- a voxel flagged "smooth" is shaded with a fitted    */
/* surface normal (the local shape's normal) instead of its blocky per-  */
/* face axis normal, so a voxel sphere shades like a real sphere.        */
/* ------------------------------------------------------------------ */

static int   g_smoothRadius = 2;    /* neighbourhood radius for fitted normals  */
static float g_smoothAmt    = 1.0f; /* 0 = blocky face normal, 1 = fully fitted */

/* Estimate the outward surface normal at voxel v as the (negated) gradient
 * of the solid-occupancy field over a (2R+1)^3 neighbourhood: sum the
 * directions pointing away from every nearby solid cell, distance-weighted.
 * The result is the local "curve of best fit" normal.  Returns 0 (leaving
 * *nx,*ny,*nz untouched) when the neighbourhood is degenerate/flat.
 *
 * This is the local "curve of best fit" surface normal for a smooth face. */
static int voxSmoothNormal( const Voxel *v,
                            double *nx, double *ny, double *nz )
{
    int di, dj, dk, R = g_smoothRadius;
    double ax = 0.0, ay = 0.0, az = 0.0, len;
    if( R < 1 ) R = 1;
    for( di = -R; di <= R; di++ )
      for( dj = -R; dj <= R; dj++ )
        for( dk = -R; dk <= R; dk++ ) {
            double d2, w;
            Voxel *n;
            if( di == 0 && dj == 0 && dk == 0 ) continue;
            d2 = (double)( di*di + dj*dj + dk*dk );
            if( d2 > (double)( R*R ) + 0.5 ) continue;   /* keep it spherical */
            n = voxAt( v->x + di, v->y + dj, v->z + dk );
            if( !n ) continue;
            w = 1.0 / d2;                 /* nearer solids weigh more */
            ax -= di * w; ay -= dj * w; az -= dk * w;   /* away from solid */
        }
    len = sqrt( ax*ax + ay*ay + az*az );
    if( len < 1e-6 ) return 0;
    *nx = ax/len; *ny = ay/len; *nz = az/len;
    return 1;
}

/* Is the face of the voxel at (x,y,z) whose outward normal is (nx,ny,nz)
 * flagged smooth?  Returns 0 if there is no voxel there. */
static int faceIsSmoothAt( int x, int y, int z, double nx, double ny, double nz )
{
    Voxel *v = voxAt( x, y, z );
    if( !v ) return 0;
    return ( v->smoothFaces >> faceDir6( nx, ny, nz ) ) & 1;
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
    int   infinite;     /* 1 = directional "sun": parallel rays, no falloff.
                         * Then (x,y,z) is read as a direction, not a position. */
    float size;         /* soft-light radius: 0 = hard point/sun; larger casts
                         * a penumbra by sampling a sphere of source points. */
    int   samples;      /* number of shadow rays spread over that sphere when
                         * size > 0.  User-controlled so a big soft radius need
                         * not cost many rays (1 = as cheap as a hard light). */
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
    g_lights[ g_numLights ].infinite = 0;
    g_lights[ g_numLights ].size = 0.0f;
    g_lights[ g_numLights ].samples = 8;
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
static int g_tool = 0;             /* 0 pencil,1 line,2 rect,3 box,4 select,
                                    * 5 scribble,6 cylinder,7 sphere,
                                    * 8 smoother (per-face), 9 image wall */
static int g_mode = 0;             /* 0 draw, 1 erase */
static int g_autoSmooth = 0;       /* if set, drawing tools mark every face of
                                    * placed voxels smooth (and erasing marks the
                                    * newly-exposed cavity faces smooth). */
static int g_thickness = 1;        /* extrude depth for line/rect/box/cyl (>=1) */
static int g_sphereDepth = 0;      /* voxels the sphere sinks into the surface */
static int g_pick = 15;            /* current palette index */
static int g_rampStart = 15;
static int g_rampEnd   = 15;

/* ---- image-wall import tool (g_tool == 9) ---- */
static unsigned char *g_impPix = NULL; /* decoded RGBA image, g_impW*g_impH*4 */
static int  *g_impIdx = NULL;          /* per-pixel palette index, -1 = transparent */
static int   g_impW = 0, g_impH = 0;
static int   g_impStage = -1;          /* -1 idle, 0 pick corner, 1 pick U, 2 pick V */
static int   g_impHaveHover = 0;       /* current hover produced a valid cell */
static int   g_impOx, g_impOy, g_impOz;   /* bottom-left corner cell (live in stage 0) */
static int   g_impUx = 1, g_impUy = 0, g_impUz = 0; /* image-width axis (live in stage 1) */
static int   g_impVx = 0, g_impVy = 1, g_impVz = 0; /* image-height axis (live in stage 2) */

/* preview toggles */
static int g_previewShade = 1;     /* 0 flat, 1 quick preview, 2 match render */
static int g_previewEdges = 1;
static int g_showSmoothWire = 1;   /* cyan outline + X on smoothed voxel faces   */
static int g_showSurfNormals = 0;  /* draw fitted surface normals (best-fit viz) */
static int g_hideVoxels      = 0;  /* hide solid faces so the fitted surface
                                    * tiles/normals can be seen by themselves   */

/* paste offset (voxels) applied by the "Paste" op */
static int g_pasteDX = 2, g_pasteDY = 0, g_pasteDZ = 0;

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

/* cached, real-lit faces for the 3D preview (rebuilt only on edits, not per
 * frame) so the preview can show the same shading the oblique render bakes */
typedef struct {
    float x, y, z;          /* cell corner */
    float nx, ny, nz;       /* face normal */
    unsigned char r, g, b;  /* shaded color */
} PFace;
static PFace *g_pface = NULL;
static int    g_pfaceCount = 0, g_pfaceCap = 0;

/* filenames for save/load dialogs */
static char g_status[ 256 ]   = "";

/* window / view layout (raw-GL 3D viewport rect, filled each frame) */
static int g_viewX = 0, g_viewY = 0, g_viewW = 0, g_viewH = 0;
static int g_winH = 720;   /* full window height, for GL-y conversion */

/* side-panel widths (draggable via the splitter handles at each edge) */
static float g_leftW  = 240.0f;
static float g_rightW = 300.0f;

/* mouse drag tracking for the 3D view */
static int g_dragBtn = 0;          /* 0 none, else SDL button */
static int g_splitDrag = 0;        /* 0 none, 1 left splitter, 2 right splitter */
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
    int   group;            /* edits sharing a group id undo/redo together */
    Voxel before, after;    /* voxel contents when present */
} Edit;

static Edit *g_hist = NULL;
static int   g_histCap = 0;
static int   g_histLen = 0;   /* number of recorded edits still live */
static int   g_histPos = 0;   /* how many are currently applied */
static int   g_groupSeq = 0;  /* monotonically increasing group id source */
static int   g_curGroup = 0;  /* group id assigned to new edits */
static int   g_inGroup  = 0;  /* 1 while a multi-edit gesture is open */

static void histClear( void )
{
    g_histLen = 0;
    g_histPos = 0;
}

/* Open/close an undo group so a whole gesture (line, box, delete-selection)
 * undoes/redoes as a single step. */
static void groupBegin( void ) { g_curGroup = ++g_groupSeq; g_inGroup = 1; }
static void groupEnd( void )   { g_inGroup = 0; }

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
        /* skip no-op: same contents already present (unless auto-smooth still
         * has faces to flip on) */
        if( ex && ex->color == color && ex->rampStart == rs &&
            ex->rampLen == ( rl < 1 ? 1 : rl ) &&
            ( !g_autoSmooth || ex->smoothFaces == 0x3F ) ) return;
        voxSet( x, y, z, color, rs, rl );
        if( g_autoSmooth ) {
            Voxel *nv = voxAt( x, y, z );   /* mark all 6 faces smooth */
            if( nv ) nv->smoothFaces = 0x3F;
        }
        e.hadAfter = 1;
        e.after = *voxAt( x, y, z );
    } else {
        if( !ex ) return;    /* nothing to erase; don't record a no-op */
        voxErase( x, y, z );
        e.hadAfter = 0;
    }
    if( !g_inGroup ) g_curGroup = ++g_groupSeq;
    e.group = g_curGroup;
    histPush( e );
    g_renderDirty = 1;

    /* auto-smooth on erase: the six neighbours around the removed cell now
     * expose a face toward the cavity -- flag those faces smooth, each as its
     * own undo record sharing this edit's group so they undo together. */
    if( !place && g_autoSmooth ) {
        static const int D[6][3] = { {1,0,0},{-1,0,0},{0,1,0},
                                     {0,-1,0},{0,0,1},{0,0,-1} };
        int d;
        for( d = 0; d < 6; d++ ) {
            Voxel *n = voxAt( x+D[d][0], y+D[d][1], z+D[d][2] );
            int bit;
            if( !n ) continue;
            /* the neighbour's face pointing back at (x,y,z) is -D */
            bit = 1 << faceDir6( -D[d][0], -D[d][1], -D[d][2] );
            if( n->smoothFaces & bit ) continue;
            {
                Edit se;
                memset( &se, 0, sizeof se );
                se.x = n->x; se.y = n->y; se.z = n->z;
                se.hadBefore = 1; se.before = *n;
                n->smoothFaces |= bit;
                se.hadAfter = 1; se.after = *n;
                se.group = g_curGroup;
                histPush( se );
            }
        }
    }
}

static void applyUndo( Edit *e )
{
    if( e->hadBefore ) {
        Voxel *v;
        voxSet( e->x, e->y, e->z, e->before.color,
                e->before.rampStart, e->before.rampLen );
        v = voxAt( e->x, e->y, e->z );   /* voxSet doesn't touch the smooth mask */
        if( v ) v->smoothFaces = e->before.smoothFaces;
    }
    else               voxErase( e->x, e->y, e->z );
}
static void applyRedo( Edit *e )
{
    if( e->hadAfter ) {
        Voxel *v;
        voxSet( e->x, e->y, e->z, e->after.color,
                e->after.rampStart, e->after.rampLen );
        v = voxAt( e->x, e->y, e->z );
        if( v ) v->smoothFaces = e->after.smoothFaces;
    }
    else              voxErase( e->x, e->y, e->z );
}

static void histUndo( void )
{
    int grp, n = 0;
    if( g_histPos == 0 ) { setStatus( "Nothing to undo" ); return; }
    grp = g_hist[ g_histPos - 1 ].group;
    while( g_histPos > 0 && g_hist[ g_histPos - 1 ].group == grp ) {
        applyUndo( &g_hist[ --g_histPos ] );
        n++;
    }
    g_renderDirty = 1;
    { char m[64]; sprintf( m, "Undo (%d)", n ); setStatus( m ); }
}

static void histRedo( void )
{
    int grp, n = 0;
    if( g_histPos >= g_histLen ) { setStatus( "Nothing to redo" ); return; }
    grp = g_hist[ g_histPos ].group;
    while( g_histPos < g_histLen && g_hist[ g_histPos ].group == grp ) {
        applyRedo( &g_hist[ g_histPos++ ] );
        n++;
    }
    g_renderDirty = 1;
    { char m[64]; sprintf( m, "Redo (%d)", n ); setStatus( m ); }
}

/* ------------------------------------------------------------------ */
/* orientation transforms for the oblique view                         */
/* ------------------------------------------------------------------ */

/* World (x,y,z) -> view frame (u,v,w): u right, v up, w toward viewer.
 * The transform is a 90*g_orient degree rotation about the vertical axis. */
static void toUVW( int x, int y, int z, int *u, int *v, int *w )
{
    *v = y;
    /* +w is "toward the viewer" (the rendered front face).  Each orient is
     * aligned so it shows the SAME face as the matching 3D-view preset:
     * Front sees -z, Right sees +x, Back sees +z, Left sees -x. */
    switch( g_orient & 3 ) {
        case 0:  *u = -x; *w = -z; break;   /* Front: front face = -z */
        case 1:  *u = -z; *w =  x; break;   /* Right: front face = +x */
        case 2:  *u =  x; *w =  z; break;   /* Back:  front face = +z */
        default: *u =  z; *w = -x; break;   /* Left:  front face = -x */
    }
}

/* Inverse: view frame (u,v,w) -> world (x,y,z). */
static void fromUVW( double u, double v, double w, double *x, double *y, double *z )
{
    *y = v;
    switch( g_orient & 3 ) {
        case 0:  *x = -u; *z = -w; break;
        case 1:  *x =  w; *z = -u; break;
        case 2:  *x =  u; *z =  w; break;
        default: *x = -w; *z =  u; break;
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
/* Representative flat/preview color for a voxel.  Flat and quick-light preview
 * modes need a single color to tint the whole voxel; taking v->color naively
 * grabs the ramp's first index, which is often pure black (ramps are commonly
 * dragged out dark->light).  Instead pick the middle of the ramp, and if that
 * sample is black while the ramp holds any non-black color, walk outward to the
 * nearest non-black sample -- so a ramped voxel never reads as solid black. */
static int voxFlatColor( const Voxel *v )
{
    int rl = v->rampLen < 1 ? 1 : v->rampLen;
    int start, mid, i;
    if( rl <= 1 ) return v->color;
    start = clampi( v->rampStart, 0, g_palCount - 1 );
    if( start + rl > g_palCount ) rl = g_palCount - start;
    if( rl < 1 ) return v->color;
    mid = start + rl / 2;
    if( g_pal[mid*3]+g_pal[mid*3+1]+g_pal[mid*3+2] == 0 ) {
        for( i = 1; i < rl; i++ ) {
            int lo = mid - i, hi = mid + i;
            if( lo >= start &&
                g_pal[lo*3]+g_pal[lo*3+1]+g_pal[lo*3+2] > 0 ) { mid = lo; break; }
            if( hi < start + rl &&
                g_pal[hi*3]+g_pal[hi*3+1]+g_pal[hi*3+2] > 0 ) { mid = hi; break; }
        }
    }
    return clampi( mid, 0, g_palCount - 1 );
}

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

/* forward decls for preview overlays defined later */
static void drawGesturePreview( void );
static void importWallDrawPreview( void );
static void shadingNormalForFace( const Voxel *v,
                                  double fx, double fy, double fz,
                                  double *ox, double *oy, double *oz );

/* Emit one shaded unit-square face at cell (x,y,z) given its normal & color. */
static void emitFace( float x, float y, float z,
                      float nx, float ny, float nz,
                      unsigned char r, unsigned char g, unsigned char b )
{
    glColor3ub( r, g, b );
    if( ny > 0.5f ) {
        glVertex3f( x,   y+1, z   ); glVertex3f( x,   y+1, z+1 );
        glVertex3f( x+1, y+1, z+1 ); glVertex3f( x+1, y+1, z   );
    } else if( ny < -0.5f ) {
        glVertex3f( x,   y, z   ); glVertex3f( x+1, y, z   );
        glVertex3f( x+1, y, z+1 ); glVertex3f( x,   y, z+1 );
    } else if( nz > 0.5f ) {
        glVertex3f( x,   y,   z+1 ); glVertex3f( x+1, y,   z+1 );
        glVertex3f( x+1, y+1, z+1 ); glVertex3f( x,   y+1, z+1 );
    } else if( nz < -0.5f ) {
        glVertex3f( x,   y,   z ); glVertex3f( x,   y+1, z );
        glVertex3f( x+1, y+1, z ); glVertex3f( x+1, y,   z );
    } else if( nx > 0.5f ) {
        glVertex3f( x+1, y,   z   ); glVertex3f( x+1, y,   z+1 );
        glVertex3f( x+1, y+1, z+1 ); glVertex3f( x+1, y+1, z   );
    } else {
        glVertex3f( x, y,   z   ); glVertex3f( x, y+1, z   );
        glVertex3f( x, y+1, z+1 ); glVertex3f( x, y,   z+1 );
    }
}

/* Fill c[4][3] with the four corners of the unit face of cell (x,y,z) whose
 * outward normal is (nx,ny,nz), each pushed out by `push` along that normal so
 * overlays sit just clear of the solid face and don't z-fight. */
static void faceCorners( int x, int y, int z, int nx, int ny, int nz,
                         float push, float c[4][3] )
{
    float fx=(float)x, fy=(float)y, fz=(float)z;
    float px=nx*push, py=ny*push, pz=nz*push;
    if( ny > 0 )      { c[0][0]=fx;   c[0][1]=fy+1; c[0][2]=fz;
                        c[1][0]=fx;   c[1][1]=fy+1; c[1][2]=fz+1;
                        c[2][0]=fx+1; c[2][1]=fy+1; c[2][2]=fz+1;
                        c[3][0]=fx+1; c[3][1]=fy+1; c[3][2]=fz; }
    else if( ny < 0 ) { c[0][0]=fx;   c[0][1]=fy;   c[0][2]=fz;
                        c[1][0]=fx+1; c[1][1]=fy;   c[1][2]=fz;
                        c[2][0]=fx+1; c[2][1]=fy;   c[2][2]=fz+1;
                        c[3][0]=fx;   c[3][1]=fy;   c[3][2]=fz+1; }
    else if( nz > 0 ) { c[0][0]=fx;   c[0][1]=fy;   c[0][2]=fz+1;
                        c[1][0]=fx+1; c[1][1]=fy;   c[1][2]=fz+1;
                        c[2][0]=fx+1; c[2][1]=fy+1; c[2][2]=fz+1;
                        c[3][0]=fx;   c[3][1]=fy+1; c[3][2]=fz+1; }
    else if( nz < 0 ) { c[0][0]=fx;   c[0][1]=fy;   c[0][2]=fz;
                        c[1][0]=fx;   c[1][1]=fy+1; c[1][2]=fz;
                        c[2][0]=fx+1; c[2][1]=fy+1; c[2][2]=fz;
                        c[3][0]=fx+1; c[3][1]=fy;   c[3][2]=fz; }
    else if( nx > 0 ) { c[0][0]=fx+1; c[0][1]=fy;   c[0][2]=fz;
                        c[1][0]=fx+1; c[1][1]=fy;   c[1][2]=fz+1;
                        c[2][0]=fx+1; c[2][1]=fy+1; c[2][2]=fz+1;
                        c[3][0]=fx+1; c[3][1]=fy+1; c[3][2]=fz; }
    else              { c[0][0]=fx;   c[0][1]=fy;   c[0][2]=fz;
                        c[1][0]=fx;   c[1][1]=fy+1; c[1][2]=fz;
                        c[2][0]=fx;   c[2][1]=fy+1; c[2][2]=fz+1;
                        c[3][0]=fx;   c[3][1]=fy;   c[3][2]=fz+1; }
    { int k; for( k=0;k<4;k++ ){ c[k][0]+=px; c[k][1]+=py; c[k][2]+=pz; } }
}

static void drawScene3D( void )
{
    int i;
    glEnable( GL_DEPTH_TEST );
    glDisable( GL_LIGHTING );
    glDisable( GL_TEXTURE_2D );

    drawGrid();

    /* solid faces: from the lit cache (matches the oblique render) when we
     * have one, else fall back to cheap per-normal preview shading.  Skipped
     * entirely in "hide voxels" mode so the fitted-surface tiles/normals can
     * be inspected on their own. */
    if( g_hideVoxels ) {
        /* nothing */
    } else if( g_pfaceCount > 0 ) {
        glBegin( GL_QUADS );
        for( i = 0; i < g_pfaceCount; i++ ) {
            PFace *p = &g_pface[i];
            emitFace( p->x, p->y, p->z, p->nx, p->ny, p->nz, p->r, p->g, p->b );
        }
        glEnd();
    } else {
        glBegin( GL_QUADS );
        for( i = 0; i < g_voxCap; i++ ) {
            Voxel *v;
            unsigned char r, g, b;
            int x, y, z;
            if( g_vox[i].used != 1 ) continue;
            v = &g_vox[i];
            x = v->x; y = v->y; z = v->z;
            { int fc = voxFlatColor( v );
              r = g_pal[ fc*3+0 ]; g = g_pal[ fc*3+1 ]; b = g_pal[ fc*3+2 ]; }
            if( !voxAt( x, y+1, z ) ) drawCubeFace( (float)x,(float)y,(float)z, 0,1,0, r,g,b );
            if( !voxAt( x, y-1, z ) ) drawCubeFace( (float)x,(float)y,(float)z, 0,-1,0, r,g,b );
            if( !voxAt( x, y, z+1 ) ) drawCubeFace( (float)x,(float)y,(float)z, 0,0,1, r,g,b );
            if( !voxAt( x, y, z-1 ) ) drawCubeFace( (float)x,(float)y,(float)z, 0,0,-1, r,g,b );
            if( !voxAt( x+1, y, z ) ) drawCubeFace( (float)x,(float)y,(float)z, 1,0,0, r,g,b );
            if( !voxAt( x-1, y, z ) ) drawCubeFace( (float)x,(float)y,(float)z, -1,0,0, r,g,b );
        }
        glEnd();
    }

    /* selection highlight: bright wire boxes around selected voxels */
    { int any = 0;
      for( i = 0; i < g_voxCap; i++ ) if( g_vox[i].used==1 && g_vox[i].sel ){any=1;break;}
      if( any ) {
        glLineWidth( 2.0f );
        glColor3f( 1.0f, 0.9f, 0.15f );
        glBegin( GL_LINES );
        for( i = 0; i < g_voxCap; i++ ) {
            float x, y, z;
            if( g_vox[i].used != 1 || !g_vox[i].sel ) continue;
            x=(float)g_vox[i].x; y=(float)g_vox[i].y; z=(float)g_vox[i].z;
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
        glLineWidth( 1.0f );
      }
    }

    drawGesturePreview();

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

    /* smoothed faces: a cyan outline with an X across every *visible* face that
     * is flagged smooth, so you can see exactly which faces are marked.
     * Toggleable in Display. */
    if( g_showSmoothWire ) {
        static const int NB[6][3] = { {0,1,0},{0,-1,0},{0,0,1},
                                      {0,0,-1},{1,0,0},{-1,0,0} };
        glLineWidth( 1.5f );
        glColor3f( 0.25f, 0.95f, 1.0f );
        for( i = 0; i < g_voxCap; i++ ) {
            int f;
            Voxel *v = &g_vox[i];
            if( v->used != 1 || v->smoothFaces == 0 ) continue;
            for( f = 0; f < 6; f++ ) {
                float c[4][3];
                if( !( ( v->smoothFaces >> f ) & 1 ) ) continue;
                if( voxAt( v->x+NB[f][0], v->y+NB[f][1], v->z+NB[f][2] ) )
                    continue;                        /* hidden face */
                faceCorners( v->x, v->y, v->z,
                             NB[f][0], NB[f][1], NB[f][2], 0.02f, c );
                glBegin( GL_LINE_LOOP );
                glVertex3fv( c[0] ); glVertex3fv( c[1] );
                glVertex3fv( c[2] ); glVertex3fv( c[3] );
                glEnd();
                glBegin( GL_LINES );                 /* the X */
                glVertex3fv( c[0] ); glVertex3fv( c[2] );
                glVertex3fv( c[1] ); glVertex3fv( c[3] );
                glEnd();
            }
        }
        glLineWidth( 1.0f );
    }

    /* surface-normal visualization: for every *visible* face draw a small tile
     * lying in the plane perpendicular to the shading normal it will actually be
     * shaded with (flat for a plain face, fitted+constrained for a smooth one),
     * plus a spike along that normal.  This answers "how is this face shaded?"
     * for every face -- smooth faces tilt, flat faces stay square. */
    if( g_showSurfNormals ) {
        static const int NB[6][3] = { {0,1,0},{0,-1,0},{0,0,1},
                                      {0,0,-1},{1,0,0},{-1,0,0} };
        static const double NR[6][3] = { {0,1,0},{0,-1,0},{0,0,1},
                                         {0,0,-1},{1,0,0},{-1,0,0} };
        float sp = 1.1f, tile = 0.42f, base = 0.5f;
        glDisable( GL_DEPTH_TEST );
        for( i = 0; i < g_voxCap; i++ ) {
            int f;
            Voxel *v = &g_vox[i];
            if( v->used != 1 ) continue;
            for( f = 0; f < 6; f++ ) {
                double nx, ny, nz, t1x, t1y, t1z, t2x, t2y, t2z, hx, hy, hz, hl;
                float cx, cy, cz, ox, oy, oz;
                int smoothF;
                if( voxAt( v->x+NB[f][0], v->y+NB[f][1], v->z+NB[f][2] ) )
                    continue;                        /* hidden face */
                smoothF = ( v->smoothFaces >> f ) & 1;
                shadingNormalForFace( v, NR[f][0], NR[f][1], NR[f][2],
                                      &nx, &ny, &nz );
                /* face centre = cell centre + half a cell along the face axis */
                cx = (float)v->x + 0.5f + (float)(NR[f][0]*base);
                cy = (float)v->y + 0.5f + (float)(NR[f][1]*base);
                cz = (float)v->z + 0.5f + (float)(NR[f][2]*base);
                ox = cx + (float)( nx*0.06 );
                oy = cy + (float)( ny*0.06 );
                oz = cz + (float)( nz*0.06 );
                /* tangent basis around the shading normal */
                if( fabs( nx ) <= fabs( ny ) && fabs( nx ) <= fabs( nz ) )
                    { hx = 1.0; hy = 0.0; hz = 0.0; }
                else if( fabs( ny ) <= fabs( nz ) )
                    { hx = 0.0; hy = 1.0; hz = 0.0; }
                else
                    { hx = 0.0; hy = 0.0; hz = 1.0; }
                t1x = hy*nz - hz*ny; t1y = hz*nx - hx*nz; t1z = hx*ny - hy*nx;
                hl = sqrt( t1x*t1x + t1y*t1y + t1z*t1z ); if( hl < 1e-6 ) hl = 1.0;
                t1x /= hl; t1y /= hl; t1z /= hl;
                t2x = ny*t1z - nz*t1y; t2y = nz*t1x - nx*t1z; t2z = nx*t1y - ny*t1x;
                /* cyan-ish translucent tile for a smooth face, grey for a flat
                 * one, so which faces round reads at a glance. */
                if( smoothF ) glColor4f( 0.2f, 0.85f, 1.0f, 0.40f );
                else          glColor4f( 0.7f, 0.7f, 0.7f, 0.28f );
                glBegin( GL_QUADS );
                glVertex3f( ox+(float)(( t1x+t2x)*tile), oy+(float)(( t1y+t2y)*tile), oz+(float)(( t1z+t2z)*tile) );
                glVertex3f( ox+(float)(( t1x-t2x)*tile), oy+(float)(( t1y-t2y)*tile), oz+(float)(( t1z-t2z)*tile) );
                glVertex3f( ox+(float)((-t1x-t2x)*tile), oy+(float)((-t1y-t2y)*tile), oz+(float)((-t1z-t2z)*tile) );
                glVertex3f( ox+(float)((-t1x+t2x)*tile), oy+(float)((-t1y+t2y)*tile), oz+(float)((-t1z+t2z)*tile) );
                glEnd();
                /* outward spike along the shading normal */
                glLineWidth( 2.0f );
                if( smoothF ) glColor3f( 0.4f, 0.95f, 1.0f );
                else          glColor3f( 0.85f, 0.85f, 0.85f );
                glBegin( GL_LINES );
                glVertex3f( cx, cy, cz );
                glVertex3f( cx+(float)(nx*sp), cy+(float)(ny*sp), cz+(float)(nz*sp) );
                glEnd();
            }
        }
        glLineWidth( 1.0f );
        glEnable( GL_DEPTH_TEST );
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

/* Project a world point to window pixel coords (top-down y, matching mouse
 * coords).  Returns 0 if the point is behind the camera.  Rebuilds the 3D-view
 * camera matrices like mouseRay(), so it is safe during event handling. */
static int worldToScreen( double wx, double wy, double wz,
                          double *sx, double *sy )
{
    GLdouble model[16], proj[16];
    GLint    view[4];
    GLdouble px, py, pz;
    double   ex, ey, ez;

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

    view[0] = g_viewX;
    view[1] = g_winH - ( g_viewY + g_viewH );
    view[2] = g_viewW;
    view[3] = g_viewH;

    if( !gluProject( wx, wy, wz, model, proj, view, &px, &py, &pz ) ) return 0;
    if( pz < 0.0 || pz > 1.0 ) return 0;   /* clipped / behind the eye */
    *sx = px;
    *sy = (double)g_winH - 1.0 - py;       /* GL y-up -> window y-down */
    return 1;
}

/* Amanatides-Woo voxel DDA.  On hit returns 1 and fills the hit cell plus the
 * empty neighbour cell (placement cell) on the face that was entered. */
static int rayVoxel( double ox, double oy, double oz,
                     double dx, double dy, double dz,
                     int *hx, int *hy, int *hz,
                     int *px, int *py, int *pz, int *axisOut )
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
            if( axisOut ) *axisOut = lastAxis;
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

/* Place or erase a single cell with the current paint color/ramp. */
static void putCell( int x, int y, int z )
{
    if( g_mode == 1 ) editVoxel( x, y, z, 0, 0, 0, 0 );
    else              editVoxel( x, y, z, 1, g_pick, g_rampStart,
                                 g_rampEnd - g_rampStart + 1 );
}

/* Pick the cell under (mx,my) that the current mode acts on, plus the plane it
 * lives on.  Returns 1 on a hit.  *cx/cy/cz = the acted cell; *axis = the plane
 * normal axis (0/1/2); *dir = +/-1 extrude direction along that axis (out of
 * the surface for draw, into the solid for erase); *onGround set if it fell to
 * the y=0 plane. */
static int pickCell( int mx, int my, int *cx, int *cy, int *cz,
                     int *axis, int *dir, int *onGround )
{
    double ox, oy, oz, dx, dy, dz;
    int hx, hy, hz, px, py, pz, ax = 1;
    mouseRay( mx, my, &ox, &oy, &oz, &dx, &dy, &dz );
    *onGround = 0;

    if( rayVoxel( ox, oy, oz, dx, dy, dz, &hx, &hy, &hz, &px, &py, &pz, &ax ) ) {
        if( g_mode == 1 ) {
            /* erase acts on the hit cell; extrude burrows into the solid */
            *cx = hx; *cy = hy; *cz = hz;
            *dir = ( ax == 0 ) ? ( hx - px ) : ( ax == 1 ) ? ( hy - py )
                                                           : ( hz - pz );
        } else {
            /* draw acts on the empty neighbour; extrude piles outward */
            *cx = px; *cy = py; *cz = pz;
            *dir = ( ax == 0 ) ? ( px - hx ) : ( ax == 1 ) ? ( py - hy )
                                                           : ( pz - hz );
        }
        if( *dir == 0 ) *dir = 1;
        *axis = ax;
        return 1;
    }

    /* missed all voxels: drop onto the ground plane y=0 */
    if( dy < -1e-6 ) {
        double t = -oy / dy;
        if( t > 0 ) {
            int gx = (int)floor( ox + dx * t );
            int gz = (int)floor( oz + dz * t );
            if( abs( gx ) < 512 && abs( gz ) < 512 ) {
                *cx = gx; *cy = 0; *cz = gz;
                *axis = 1; *dir = 1; *onGround = 1;
                return 1;
            }
        }
    }
    return 0;
}

/* Intersect the mouse ray with the working plane (axisN = planeCoord+0.5) and
 * return the integer cell on that plane under the cursor.  Fixes the plane-axis
 * coordinate to planeCoord so drags glide across a single flat layer. */
static int planeCell( int mx, int my, int axisN, int planeCoord,
                      int *cx, int *cy, int *cz )
{
    double ox, oy, oz, dx, dy, dz, o[3], d[3], t, hit[3];
    double target = planeCoord + 0.5;
    mouseRay( mx, my, &ox, &oy, &oz, &dx, &dy, &dz );
    o[0]=ox; o[1]=oy; o[2]=oz; d[0]=dx; d[1]=dy; d[2]=dz;
    if( d[axisN] > -1e-9 && d[axisN] < 1e-9 ) return 0;
    t = ( target - o[axisN] ) / d[axisN];
    if( t <= 0 ) return 0;
    hit[0] = o[0] + d[0]*t; hit[1] = o[1] + d[1]*t; hit[2] = o[2] + d[2]*t;
    hit[axisN] = target;
    *cx = (int)floor( hit[0] );
    *cy = (int)floor( hit[1] );
    *cz = (int)floor( hit[2] );
    return 1;
}

/* ---- region gesture state (line / rect / box / select drag) ---- */
static int g_gActive = 0;          /* 1 while a region drag is in progress */
static int g_gAx, g_gAy, g_gAz;    /* anchor cell (fixes the plane coord) */
static int g_gBx, g_gBy, g_gBz;    /* current cell on the plane */
static int g_gAxis = 1;            /* plane normal axis */
static int g_gDir  = 1;            /* extrude direction along plane axis */
static int g_gHaveB = 0;           /* current cell is valid */

/* ---- sphere gesture state ---- */
static int    g_sphActive = 0;             /* 1 while a sphere drag is open */
static double g_sphSx, g_sphSy, g_sphSz;   /* clicked surface point (world) */
static double g_sphNx, g_sphNy, g_sphNz;   /* outward surface normal (unit) */
static double g_sphR = 0.0;                /* current radius in voxels */

/* selection marquee depth: how many layers to sweep below (into the solid) and
 * above (out of the surface) the clicked plane, like the Box tool's thickness
 * but two-sided.  Only used by the Select tool. */
static int g_selBelow = 0;
static int g_selAbove = 0;

/* Inclusive layer-offset range along g_gDir that regionForEach sweeps.  Set by
 * setRegionLayers() from the active tool just before enumerating a region. */
static int g_regLayerLo = 0, g_regLayerHi = 0;
static void setRegionLayers( void )
{
    if( g_tool == 4 ) { g_regLayerLo = -g_selBelow; g_regLayerHi = g_selAbove; }
    else              { g_regLayerLo = 0; g_regLayerHi = g_thickness - 1; }
}

/* Enumerate the cells of the active region, calling cb(x,y,z,ud) for each.
 * Handles line / rect(outline) / box(filled) shapes plus thickness extrusion
 * along the plane normal.  Capped so a huge drag can't stall the app. */
static void regionForEach( int tool,
                           void (*cb)( int, int, int, void* ), void *ud )
{
    int A = g_gAxis;
    int i0 = ( A + 1 ) % 3, i1 = ( A + 2 ) % 3;   /* the two in-plane axes */
    int a[3], b[3], c[3];
    int lo0, hi0, lo1, hi1, p0, p1, k, count = 0;
    int limit = 200000;
    a[0]=g_gAx; a[1]=g_gAy; a[2]=g_gAz;
    b[0]=g_gBx; b[1]=g_gBy; b[2]=g_gBz;

    lo0 = a[i0] < b[i0] ? a[i0] : b[i0];
    hi0 = a[i0] > b[i0] ? a[i0] : b[i0];
    lo1 = a[i1] < b[i1] ? a[i1] : b[i1];
    hi1 = a[i1] > b[i1] ? a[i1] : b[i1];

    for( k = g_regLayerLo; k <= g_regLayerHi; k++ ) {
        int layer = a[A] + k * g_gDir;
        c[A] = layer;
        if( tool == 1 ) {
            /* line: integer Bresenham in the (i0,i1) plane */
            int x0 = a[i0], y0 = a[i1], x1 = b[i0], y1 = b[i1];
            int dx =  ( x1 > x0 ? x1-x0 : x0-x1 ), sx = x0 < x1 ? 1 : -1;
            int dy = -( y1 > y0 ? y1-y0 : y0-y1 ), sy = y0 < y1 ? 1 : -1;
            int err = dx + dy, e2;
            for( ;; ) {
                c[i0] = x0; c[i1] = y0;
                cb( c[0], c[1], c[2], ud );
                if( ++count > limit ) return;
                if( x0 == x1 && y0 == y1 ) break;
                e2 = 2 * err;
                if( e2 >= dy ) { err += dy; x0 += sx; }
                if( e2 <= dx ) { err += dx; y0 += sy; }
            }
        } else {
            /* filled rectangle span, with per-tool masks */
            double cen0 = ( lo0 + hi0 ) * 0.5, cen1 = ( lo1 + hi1 ) * 0.5;
            double ra = ( hi0 - lo0 ) * 0.5 + 0.5, rb = ( hi1 - lo1 ) * 0.5 + 0.5;
            for( p0 = lo0; p0 <= hi0; p0++ )
              for( p1 = lo1; p1 <= hi1; p1++ ) {
                if( tool == 2 &&                       /* rect: outline only */
                    p0 != lo0 && p0 != hi0 &&
                    p1 != lo1 && p1 != hi1 ) continue;
                if( tool == 6 ) {                      /* cylinder: ellipse */
                    double dx0 = ( p0 - cen0 ) / ra, dy0 = ( p1 - cen1 ) / rb;
                    if( dx0*dx0 + dy0*dy0 > 1.0 ) continue;
                }
                c[i0] = p0; c[i1] = p1;
                cb( c[0], c[1], c[2], ud );
                if( ++count > limit ) return;
              }
        }
    }
}

/* callbacks over a region */
static int g_regCount;
static void cbPut( int x, int y, int z, void *ud )
{ (void)ud; putCell( x, y, z ); g_regCount++; }
static void cbSelect( int x, int y, int z, void *ud )
{
    Voxel *v = voxAt( x, y, z );
    (void)ud;
    if( v ) { v->sel = ( g_mode == 1 ) ? 0 : 1; g_regCount++; }
}
/* Commit the active region gesture into the model (or the selection). */
static void regionCommit( void )
{
    char msg[96];
    if( !g_gHaveB ) return;
    g_regCount = 0;
    setRegionLayers();
    if( g_tool == 4 ) {
        regionForEach( 3, cbSelect, NULL );   /* select: filled marquee */
        sprintf( msg, "%s %d voxels",
                 g_mode == 1 ? "Deselected" : "Selected", g_regCount );
    } else {
        groupBegin();
        regionForEach( g_tool, cbPut, NULL );
        groupEnd();
        sprintf( msg, "%s %d voxels", g_mode == 1 ? "Erased" : "Placed",
                 g_regCount );
    }
    setStatus( msg );
}

/* Single-click pencil (or single-cell select). */
static void applyToolAt( int mx, int my )
{
    int cx, cy, cz, axis, dir, ground;
    if( !pickCell( mx, my, &cx, &cy, &cz, &axis, &dir, &ground ) ) return;
    if( g_tool == 4 ) {
        Voxel *v = voxAt( cx, cy, cz );
        if( v ) { v->sel = ( g_mode == 1 ) ? 0 : 1;
                  setStatus( g_mode == 1 ? "Deselected voxel"
                                         : "Selected voxel" ); }
        return;
    }
    putCell( cx, cy, cz );
    setStatus( g_mode == 1 ? "Erased voxel" : "Placed voxel" );
}

/* Begin a region drag under the cursor. */
static void gestureBegin( int mx, int my )
{
    int cx, cy, cz, axis, dir, ground;
    g_gActive = 0; g_gHaveB = 0;
    if( !pickCell( mx, my, &cx, &cy, &cz, &axis, &dir, &ground ) ) return;
    g_gAx = g_gBx = cx; g_gAy = g_gBy = cy; g_gAz = g_gBz = cz;
    g_gAxis = axis; g_gDir = dir;
    g_gActive = 1; g_gHaveB = 1;
}

/* Update the region drag as the mouse moves. */
static void gestureUpdate( int mx, int my )
{
    int cx, cy, cz, planeCoord;
    if( !g_gActive ) return;
    planeCoord = ( g_gAxis == 0 ) ? g_gAx : ( g_gAxis == 1 ) ? g_gAy : g_gAz;
    if( planeCell( mx, my, g_gAxis, planeCoord, &cx, &cy, &cz ) ) {
        g_gBx = cx; g_gBy = cy; g_gBz = cz;
        /* keep the plane-axis coordinate pinned to the anchor layer */
        if( g_gAxis == 0 ) g_gBx = g_gAx;
        else if( g_gAxis == 1 ) g_gBy = g_gAy;
        else g_gBz = g_gAz;
        g_gHaveB = 1;
    }
}

/* Scribble-select: select (or, in erase mode, deselect) the surface voxel the
 * cursor is over.  Called on the initial click and every mouse-move of a
 * scribble drag, so dragging paints a selection across whatever it touches. */
static int g_scribbling = 0;
static void scribbleAt( int mx, int my )
{
    double ox, oy, oz, dx, dy, dz;
    int hx, hy, hz, px, py, pz, ax;
    mouseRay( mx, my, &ox, &oy, &oz, &dx, &dy, &dz );
    if( rayVoxel( ox, oy, oz, dx, dy, dz, &hx, &hy, &hz, &px, &py, &pz, &ax ) ) {
        Voxel *v = voxAt( hx, hy, hz );
        if( v ) v->sel = ( g_mode == 1 ) ? 0 : 1;
    }
}

/* Smoother tool: mark (draw) or clear (erase) the single visible face the cursor
 * is over as smooth.  Called on the initial click and every move of the drag, so
 * dragging paints face-smoothness across whatever the cursor sweeps.  Each real
 * change is one undo record; the whole drag shares an open group. */
static int g_smoothing = 0;
static void smoothFaceAt( int mx, int my )
{
    double ox, oy, oz, dx, dy, dz;
    int hx, hy, hz, px, py, pz, ax, bit, newMask;
    Voxel *v;
    Edit e;
    mouseRay( mx, my, &ox, &oy, &oz, &dx, &dy, &dz );
    if( !rayVoxel( ox, oy, oz, dx, dy, dz, &hx, &hy, &hz, &px, &py, &pz, &ax ) )
        return;
    v = voxAt( hx, hy, hz );
    if( !v ) return;
    bit = 1 << faceDir6( px - hx, py - hy, pz - hz );
    newMask = ( g_mode == 1 ) ? ( v->smoothFaces & ~bit )
                              : ( v->smoothFaces |  bit );
    if( newMask == v->smoothFaces ) return;          /* no change -> no record */
    memset( &e, 0, sizeof e );
    e.x = v->x; e.y = v->y; e.z = v->z;
    e.hadBefore = 1; e.before = *v;
    v->smoothFaces = newMask;
    e.hadAfter = 1; e.after = *v;
    e.group = g_curGroup;
    histPush( e );
    g_renderDirty = 1;
}

/* ------------------------------------------------------------------ */
/* sphere gesture (grow a ball on a surface)                           */
/* ------------------------------------------------------------------ */

/* Find the outward-facing surface point + unit normal under the cursor,
 * independent of draw/erase mode.  Returns 1 on a hit. */
static int pickSurface( int mx, int my, double *sx, double *sy, double *sz,
                        double *nx, double *ny, double *nz )
{
    double ox, oy, oz, dx, dy, dz;
    int hx, hy, hz, px, py, pz, ax;
    mouseRay( mx, my, &ox, &oy, &oz, &dx, &dy, &dz );
    if( rayVoxel( ox, oy, oz, dx, dy, dz, &hx, &hy, &hz, &px, &py, &pz, &ax ) ) {
        /* outward normal points from the hit cell to the empty neighbour */
        double NX = px - hx, NY = py - hy, NZ = pz - hz;
        *nx = NX; *ny = NY; *nz = NZ;
        /* surface point = centre of the hit voxel + half a cell outward */
        *sx = hx + 0.5 + NX*0.5;
        *sy = hy + 0.5 + NY*0.5;
        *sz = hz + 0.5 + NZ*0.5;
        return 1;
    }
    /* missed all voxels: land on the ground plane y=0, normal up */
    if( dy < -1e-6 ) {
        double t = -oy / dy;
        if( t > 0 ) {
            double gx = ox + dx*t, gz = oz + dz*t;
            if( gx > -512 && gx < 512 && gz > -512 && gz < 512 ) {
                *sx = floor( gx ) + 0.5; *sy = 0.0; *sz = floor( gz ) + 0.5;
                *nx = 0; *ny = 1; *nz = 0;
                return 1;
            }
        }
    }
    return 0;
}

/* The sphere centre sits along the surface normal, sunk g_sphereDepth voxels
 * into the surface (same meaning in draw and erase). */
static void sphereCenter( double *cx, double *cy, double *cz )
{
    double off = g_sphR - g_sphereDepth;
    *cx = g_sphSx + g_sphNx * off;
    *cy = g_sphSy + g_sphNy * off;
    *cz = g_sphSz + g_sphNz * off;
}

/* Enumerate every cell whose centre lies within the current sphere. */
static void sphereForEach( void (*cb)( int, int, int, void* ), void *ud )
{
    double cx, cy, cz;
    int ix, iy, iz, lo, hi, r;
    double r2;
    if( g_sphR < 0.5 ) return;
    sphereCenter( &cx, &cy, &cz );
    r = (int)ceil( g_sphR ) + 1;
    r2 = g_sphR * g_sphR;
    for( ix = (int)floor(cx)-r; ix <= (int)floor(cx)+r; ix++ )
    for( iy = (int)floor(cy)-r; iy <= (int)floor(cy)+r; iy++ )
    for( iz = (int)floor(cz)-r; iz <= (int)floor(cz)+r; iz++ ) {
        double dx = ix+0.5-cx, dy = iy+0.5-cy, dz = iz+0.5-cz;
        if( dx*dx+dy*dy+dz*dz <= r2 ) cb( ix, iy, iz, ud );
    }
    (void)lo; (void)hi;
}

static void cbSpherePut( int x, int y, int z, void *ud )
{ (void)ud; putCell( x, y, z ); g_regCount++; }

static void sphereBegin( int mx, int my )
{
    g_sphActive = 0; g_sphR = 0.0;
    if( !pickSurface( mx, my, &g_sphSx,&g_sphSy,&g_sphSz,
                              &g_sphNx,&g_sphNy,&g_sphNz ) ) return;
    g_sphActive = 1;
}

static void sphereUpdate( int mx, int my )
{
    int dpx, dpy;
    if( !g_sphActive ) return;
    dpx = mx - g_downX; dpy = my - g_downY;
    /* radius grows with drag distance; ~12 px per voxel of radius */
    g_sphR = sqrt( (double)dpx*dpx + (double)dpy*dpy ) / 12.0;
    if( g_sphR > 96.0 ) g_sphR = 96.0;
}

static void sphereCommit( void )
{
    char msg[80];
    if( !g_sphActive || g_sphR < 0.5 ) return;
    g_regCount = 0;
    groupBegin();
    sphereForEach( cbSpherePut, NULL );
    groupEnd();
    sprintf( msg, "%s sphere r=%.1f (%d voxels)",
             g_mode==1 ? "Carved" : "Placed", g_sphR, g_regCount );
    setStatus( msg );
}

/* ------------------------------------------------------------------ */
/* selection operations + clipboard                                    */
/* ------------------------------------------------------------------ */

typedef struct { int dx, dy, dz, color, rampStart, rampLen; } ClipVox;
static ClipVox *g_clip = NULL;
static int      g_clipCount = 0;

static int selCount( void )
{
    int i, n = 0;
    for( i = 0; i < g_voxCap; i++ )
        if( g_vox[i].used == 1 && g_vox[i].sel ) n++;
    return n;
}

static void selClear( void )
{
    int i;
    for( i = 0; i < g_voxCap; i++ )
        if( g_vox[i].used == 1 ) g_vox[i].sel = 0;
}

static void selAll( void )
{
    int i;
    for( i = 0; i < g_voxCap; i++ )
        if( g_vox[i].used == 1 ) g_vox[i].sel = 1;
}

/* Invert: select every unselected voxel and drop the current selection. */
static void selInvert( void )
{
    int i;
    for( i = 0; i < g_voxCap; i++ )
        if( g_vox[i].used == 1 ) g_vox[i].sel = !g_vox[i].sel;
    { char m[64]; sprintf( m, "%d selected", selCount() ); setStatus( m ); }
}

/* Mark (flag=1) or clear (flag=0) ALL six faces of every selected voxel as
 * smooth, as one undo step.  Only visible faces actually affect shading, but we
 * remember all six so a face that becomes exposed later is already smooth.
 * Records a before/after Edit per voxel whose mask actually changes. */
static void selSmooth( int flag )
{
    int i, n = 0;
    int newMask = flag ? 0x3F : 0;
    if( selCount() == 0 ) { setStatus( "Selection empty" ); return; }
    groupBegin();
    for( i = 0; i < g_voxCap; i++ ) {
        Voxel *v = &g_vox[i];
        Edit e;
        if( v->used != 1 || !v->sel || v->smoothFaces == newMask ) continue;
        memset( &e, 0, sizeof e );
        e.x = v->x; e.y = v->y; e.z = v->z;
        e.hadBefore = 1; e.before = *v;
        v->smoothFaces = newMask;
        e.hadAfter = 1; e.after = *v;
        e.group = g_curGroup;
        histPush( e );
        n++;
    }
    groupEnd();
    g_renderDirty = 1;
    { char m[64]; sprintf( m, "%s %d voxels",
        flag ? "Smoothed" : "Unsmoothed", n ); setStatus( m ); }
}

/* Extrude the selection along an axis: copy every selected voxel forward
 * step-by-step, filling a prism whose cross-section is the selected shape.
 * The whole extruded volume becomes the new selection so it can be repeated. */
static const int g_axis6[6][3] =
    { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
static int g_extrudeDir  = 4;   /* index into g_axis6; default +Z */
static int g_extrudeDist = 4;

static void selExtrude( void )
{
    int i, k, n = 0, cap = selCount();
    int dx = g_axis6[g_extrudeDir][0];
    int dy = g_axis6[g_extrudeDir][1];
    int dz = g_axis6[g_extrudeDir][2];
    int *src;   /* x,y,z,color,rampStart,rampLen per source voxel */
    if( cap == 0 ) { setStatus( "Selection empty" ); return; }
    if( g_extrudeDist < 1 ) { setStatus( "Extrude distance is 0" ); return; }
    src = (int*)malloc( (size_t)cap * 6 * sizeof( int ) );
    for( i = 0; i < g_voxCap; i++ )
        if( g_vox[i].used == 1 && g_vox[i].sel ) {
            src[n*6+0]=g_vox[i].x; src[n*6+1]=g_vox[i].y; src[n*6+2]=g_vox[i].z;
            src[n*6+3]=g_vox[i].color; src[n*6+4]=g_vox[i].rampStart;
            src[n*6+5]=g_vox[i].rampLen; n++;
        }
    groupBegin();
    for( k = 1; k <= g_extrudeDist; k++ )
        for( i = 0; i < n; i++ )
            editVoxel( src[i*6+0]+dx*k, src[i*6+1]+dy*k, src[i*6+2]+dz*k, 1,
                       src[i*6+3], src[i*6+4], src[i*6+5] );
    groupEnd();
    /* select the full extruded volume (voxSet cleared sel flags on new cells) */
    for( k = 0; k <= g_extrudeDist; k++ )
        for( i = 0; i < n; i++ ) {
            Voxel *v = voxAt( src[i*6+0]+dx*k, src[i*6+1]+dy*k, src[i*6+2]+dz*k );
            if( v ) v->sel = 1;
        }
    free( src );
    { char m[80]; sprintf( m, "Extruded %d voxels by %d", n, g_extrudeDist );
      setStatus( m ); }
}

/* Delete every selected voxel as one undo group. */
static void selDelete( void )
{
    int i, n = 0;
    int *coords, cap = selCount();
    if( cap == 0 ) { setStatus( "Selection empty" ); return; }
    coords = (int*)malloc( (size_t)cap * 3 * sizeof( int ) );
    for( i = 0; i < g_voxCap; i++ )
        if( g_vox[i].used == 1 && g_vox[i].sel ) {
            coords[n*3+0]=g_vox[i].x; coords[n*3+1]=g_vox[i].y;
            coords[n*3+2]=g_vox[i].z; n++;
        }
    groupBegin();
    for( i = 0; i < n; i++ )
        editVoxel( coords[i*3+0], coords[i*3+1], coords[i*3+2], 0, 0,0,0 );
    groupEnd();
    free( coords );
    { char m[64]; sprintf( m, "Deleted %d voxels", n ); setStatus( m ); }
}

/* Repaint every selected voxel with the current color/ramp, one undo group. */
static void selRecolor( void )
{
    int i, n = 0;
    int *coords, cap = selCount();
    int rl = g_rampEnd - g_rampStart + 1;
    if( cap == 0 ) { setStatus( "Selection empty" ); return; }
    coords = (int*)malloc( (size_t)cap * 3 * sizeof( int ) );
    for( i = 0; i < g_voxCap; i++ )
        if( g_vox[i].used == 1 && g_vox[i].sel ) {
            coords[n*3+0]=g_vox[i].x; coords[n*3+1]=g_vox[i].y;
            coords[n*3+2]=g_vox[i].z; n++;
        }
    groupBegin();
    for( i = 0; i < n; i++ )
        editVoxel( coords[i*3+0], coords[i*3+1], coords[i*3+2], 1,
                   g_pick, g_rampStart, rl );
    groupEnd();
    /* recolor drops selection flags (voxSet rewrites the cell); restore them */
    for( i = 0; i < n; i++ ) {
        Voxel *v = voxAt( coords[i*3+0], coords[i*3+1], coords[i*3+2] );
        if( v ) v->sel = 1;
    }
    free( coords );
    { char m[64]; sprintf( m, "Recolored %d voxels", n ); setStatus( m ); }
}

/* Copy the selection into the clipboard, relative to its min corner. */
static void selCopy( void )
{
    int i, n = 0, have = 0, mnx=0, mny=0, mnz=0;
    int cap = selCount();
    if( cap == 0 ) { setStatus( "Selection empty" ); return; }
    for( i = 0; i < g_voxCap; i++ )
        if( g_vox[i].used == 1 && g_vox[i].sel ) {
            if( !have ) { mnx=g_vox[i].x; mny=g_vox[i].y; mnz=g_vox[i].z; have=1; }
            if( g_vox[i].x < mnx ) mnx = g_vox[i].x;
            if( g_vox[i].y < mny ) mny = g_vox[i].y;
            if( g_vox[i].z < mnz ) mnz = g_vox[i].z;
        }
    free( g_clip );
    g_clip = (ClipVox*)malloc( (size_t)cap * sizeof( ClipVox ) );
    for( i = 0; i < g_voxCap; i++ )
        if( g_vox[i].used == 1 && g_vox[i].sel ) {
            g_clip[n].dx = g_vox[i].x - mnx;
            g_clip[n].dy = g_vox[i].y - mny;
            g_clip[n].dz = g_vox[i].z - mnz;
            g_clip[n].color = g_vox[i].color;
            g_clip[n].rampStart = g_vox[i].rampStart;
            g_clip[n].rampLen = g_vox[i].rampLen;
            n++;
        }
    g_clipCount = n;
    { char m[64]; sprintf( m, "Copied %d voxels", n ); setStatus( m ); }
}

/* Paste the clipboard, offset by (g_pasteDX,DY,DZ) from the ORIGINAL min
 * corner.  The pasted copy becomes the new selection. */
static void selPaste( void )
{
    int i, mnx=0, mny=0, mnz=0, have=0;
    if( g_clipCount == 0 ) { setStatus( "Clipboard empty" ); return; }
    /* place relative to current selection's min corner if any, else origin */
    for( i = 0; i < g_voxCap; i++ )
        if( g_vox[i].used == 1 && g_vox[i].sel ) {
            if( !have ) { mnx=g_vox[i].x; mny=g_vox[i].y; mnz=g_vox[i].z; have=1; }
            if( g_vox[i].x < mnx ) mnx = g_vox[i].x;
            if( g_vox[i].y < mny ) mny = g_vox[i].y;
            if( g_vox[i].z < mnz ) mnz = g_vox[i].z;
        }
    selClear();
    groupBegin();
    for( i = 0; i < g_clipCount; i++ ) {
        int x = mnx + g_pasteDX + g_clip[i].dx;
        int y = mny + g_pasteDY + g_clip[i].dy;
        int z = mnz + g_pasteDZ + g_clip[i].dz;
        editVoxel( x, y, z, 1, g_clip[i].color, g_clip[i].rampStart,
                   g_clip[i].rampLen );
    }
    groupEnd();
    for( i = 0; i < g_clipCount; i++ ) {
        Voxel *v = voxAt( mnx + g_pasteDX + g_clip[i].dx,
                          mny + g_pasteDY + g_clip[i].dy,
                          mnz + g_pasteDZ + g_clip[i].dz );
        if( v ) v->sel = 1;
    }
    { char m[64]; sprintf( m, "Pasted %d voxels", g_clipCount );
      setStatus( m ); }
}

/* ---- ghost preview of the in-progress region drag ---- */
static void cbGhost( int x, int y, int z, void *ud )
{
    float fx=(float)x, fy=(float)y, fz=(float)z;
    (void)ud;
    /* +Y */ glVertex3f(fx,fy+1,fz);   glVertex3f(fx,fy+1,fz+1); glVertex3f(fx+1,fy+1,fz+1); glVertex3f(fx+1,fy+1,fz);
    /* -Y */ glVertex3f(fx,fy,fz);     glVertex3f(fx+1,fy,fz);   glVertex3f(fx+1,fy,fz+1);   glVertex3f(fx,fy,fz+1);
    /* +Z */ glVertex3f(fx,fy,fz+1);   glVertex3f(fx+1,fy,fz+1); glVertex3f(fx+1,fy+1,fz+1); glVertex3f(fx,fy+1,fz+1);
    /* -Z */ glVertex3f(fx,fy,fz);     glVertex3f(fx,fy+1,fz);   glVertex3f(fx+1,fy+1,fz);   glVertex3f(fx+1,fy,fz);
    /* +X */ glVertex3f(fx+1,fy,fz);   glVertex3f(fx+1,fy,fz+1); glVertex3f(fx+1,fy+1,fz+1); glVertex3f(fx+1,fy+1,fz);
    /* -X */ glVertex3f(fx,fy,fz);     glVertex3f(fx,fy+1,fz);   glVertex3f(fx,fy+1,fz+1);   glVertex3f(fx,fy,fz+1);
}

static void drawGesturePreview( void )
{
    int tool;
    /* image-wall placement has its own multi-stage ghost overlay */
    if( g_tool == 9 ) { importWallDrawPreview(); return; }
    /* sphere gesture previews as a translucent ghost ball */
    if( g_sphActive && g_sphR >= 0.5 ) {
        glEnable( GL_BLEND );
        glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
        glDepthMask( GL_FALSE );
        if( g_mode == 1 ) glColor4f( 1.0f, 0.25f, 0.2f, 0.40f );
        else              glColor4f( 0.3f, 1.0f, 0.45f, 0.40f );
        glBegin( GL_QUADS );
        sphereForEach( cbGhost, NULL );
        glEnd();
        glDepthMask( GL_TRUE );
        glDisable( GL_BLEND );
        return;
    }
    if( !g_gActive || !g_gHaveB ) return;
    tool = ( g_tool == 4 ) ? 3 : g_tool;   /* select previews as a filled box */
    if( g_tool == 0 ) return;              /* pencil: nothing to preview */
    setRegionLayers();
    glEnable( GL_BLEND );
    glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
    glDepthMask( GL_FALSE );
    if( g_tool == 4 )       glColor4f( 1.0f, 0.9f, 0.15f, 0.35f ); /* select */
    else if( g_mode == 1 )  glColor4f( 1.0f, 0.25f, 0.2f, 0.40f ); /* erase */
    else                    glColor4f( 0.3f, 1.0f, 0.45f, 0.40f ); /* draw */
    glBegin( GL_QUADS );
    regionForEach( tool, cbGhost, NULL );
    glEnd();
    glDepthMask( GL_TRUE );
    glDisable( GL_BLEND );
}

/* ------------------------------------------------------------------ */
/* image-wall import tool                                              */
/*                                                                     */
/* Import a PNG (with alpha) as a flat wall of voxels, one voxel per   */
/* opaque pixel, coloured by nearest palette match.  Placement is a    */
/* three-click gesture in the 3D view: click 1 fixes the bottom-left   */
/* corner cell, click 2 chooses the image-width axis (one of the six   */
/* face directions), click 3 chooses a perpendicular image-height axis */
/* and commits.  Translucent ghosts preview each stage.                */
/* ------------------------------------------------------------------ */

/* Nearest palette index to an arbitrary RGB (squared distance, ties to
 * the brighter sample so ramps that start black don't win by luminance). */
static int nearestPaletteIndex( int r, int g, int b )
{
    int i, best = 0, bestLum = -1;
    double bestD = 1e30;
    for( i = 0; i < g_palCount; i++ ) {
        double dr = g_pal[i*3+0] - r;
        double dg = g_pal[i*3+1] - g;
        double db = g_pal[i*3+2] - b;
        double d  = dr*dr + dg*dg + db*db;
        int lum   = g_pal[i*3+0] + g_pal[i*3+1] + g_pal[i*3+2];
        if( d < bestD - 1e-6 || ( d < bestD + 1e-6 && lum > bestLum ) ) {
            bestD = d; best = i; bestLum = lum;
        }
    }
    return best;
}

/* Free any loaded image and go idle. */
static void importWallFree( void )
{
    if( g_impPix ) { stbi_image_free( g_impPix ); g_impPix = NULL; }
    if( g_impIdx ) { free( g_impIdx ); g_impIdx = NULL; }
    g_impW = g_impH = 0;
    g_impStage = -1;
    g_impHaveHover = 0;
}

/* Load a PNG, precompute its palette-index map, and arm the placement tool. */
static int importWallLoad( const char *path )
{
    int w = 0, h = 0, comp = 0, i, n;
    unsigned char *data = stbi_load( path, &w, &h, &comp, 4 );
    char m[128];
    if( !data ) { setStatus( "PNG load failed" ); return 0; }
    if( w < 1 || h < 1 || w > 512 || h > 512 ) {
        stbi_image_free( data );
        setStatus( "Image must be 1..512 px on a side" );
        return 0;
    }
    importWallFree();
    n = w * h;
    g_impIdx = (int*)malloc( (size_t)n * sizeof(int) );
    if( !g_impIdx ) { stbi_image_free( data ); setStatus( "Out of memory" );
                      return 0; }
    g_impPix = data; g_impW = w; g_impH = h;
    for( i = 0; i < n; i++ ) {
        if( data[i*4+3] < 128 ) g_impIdx[i] = -1;   /* transparent */
        else g_impIdx[i] = nearestPaletteIndex( data[i*4+0],
                                                data[i*4+1], data[i*4+2] );
    }
    g_impStage = 0;
    g_impHaveHover = 0;
    g_impUx = 1; g_impUy = 0; g_impUz = 0;
    g_impVx = 0; g_impVy = 1; g_impVz = 0;
    g_tool = 9;
    sprintf( m, "Image %dx%d loaded -- click to place bottom-left corner", w, h );
    setStatus( m );
    return 1;
}

/* Choose the face-direction axis whose *on-screen* projection best matches the
 * cursor's screen direction away from the corner cell.  Using screen space
 * (rather than a picked world point) lets the user aim the vertical +Y axis
 * even in an empty scene, where any picked point would collapse onto the
 * ground plane.  If excludeParallel is set, the two axes parallel to
 * (ux,uy,uz) are skipped so the height axis stays perpendicular to the width
 * axis. */
static int importSnapAxis( int mx, int my, int excludeParallel,
                           int ux, int uy, int uz,
                           int *ax, int *ay, int *az )
{
    static const int DIRS[6][3] = {
        { 1,0,0 }, { -1,0,0 }, { 0,1,0 }, { 0,-1,0 }, { 0,0,1 }, { 0,0,-1 } };
    double ocx = g_impOx + 0.5, ocy = g_impOy + 0.5, ocz = g_impOz + 0.5;
    double sox, soy, mvx, mvy, mlen, best = -2.0;
    int i, bi = -1;
    if( !worldToScreen( ocx, ocy, ocz, &sox, &soy ) ) return 0;
    mvx = mx - sox; mvy = my - soy;
    mlen = sqrt( mvx*mvx + mvy*mvy );
    if( mlen < 1.0 ) return 0;              /* cursor sitting on the corner */
    mvx /= mlen; mvy /= mlen;
    for( i = 0; i < 6; i++ ) {
        double ex, ey, dlen, score;
        if( excludeParallel &&
            ( DIRS[i][0]*ux + DIRS[i][1]*uy + DIRS[i][2]*uz ) != 0 ) continue;
        if( !worldToScreen( ocx + DIRS[i][0], ocy + DIRS[i][1],
                            ocz + DIRS[i][2], &ex, &ey ) ) continue;
        ex -= sox; ey -= soy;
        dlen = sqrt( ex*ex + ey*ey );
        if( dlen < 1e-6 ) continue;          /* axis points at/away from eye */
        score = ( ex*mvx + ey*mvy ) / dlen;  /* cosine of screen-angle match */
        if( score > best ) { best = score; bi = i; }
    }
    if( bi < 0 ) return 0;
    *ax = DIRS[bi][0]; *ay = DIRS[bi][1]; *az = DIRS[bi][2];
    return 1;
}

/* Enumerate every voxel cell of the placed wall (opaque pixels only),
 * calling cb with the cell and its palette index.  Image bottom row maps to
 * V=0 so the picture stands upright from the clicked corner. */
static void importWallForEach( void (*cb)( int, int, int, int, void* ),
                               void *ud )
{
    int px, py;
    if( !g_impIdx ) return;
    for( py = 0; py < g_impH; py++ )
    for( px = 0; px < g_impW; px++ ) {
        int idx = g_impIdx[ py*g_impW + px ];
        int u, v, cx, cy, cz;
        if( idx < 0 ) continue;                  /* transparent pixel */
        u = px; v = g_impH - 1 - py;
        cx = g_impOx + g_impUx*u + g_impVx*v;
        cy = g_impOy + g_impUy*u + g_impVy*v;
        cz = g_impOz + g_impUz*u + g_impVz*v;
        cb( cx, cy, cz, idx, ud );
    }
}

/* ghost callback: one translucent cube tinted with the pixel's colour
 * (must run inside an open glBegin(GL_QUADS)). */
static void cbImpGhost( int x, int y, int z, int idx, void *ud )
{
    int i = clampi( idx, 0, g_palCount-1 );
    (void)ud;
    glColor4f( g_pal[i*3+0]/255.0f, g_pal[i*3+1]/255.0f,
               g_pal[i*3+2]/255.0f, 0.6f );
    cbGhost( x, y, z, NULL );
}

static void cbImpPlace( int x, int y, int z, int idx, void *ud )
{
    int *count = (int*)ud;
    editVoxel( x, y, z, 1, idx, idx, 1 );   /* flat single-colour voxel */
    (*count)++;
}

static void importWallDrawPreview( void )
{
    if( !g_impPix || g_impStage < 0 ) return;
    glEnable( GL_BLEND );
    glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
    glDepthMask( GL_FALSE );
    if( g_impStage == 0 ) {
        /* single ghost cube where the corner would land */
        if( g_impHaveHover ) {
            glColor4f( 0.3f, 0.8f, 1.0f, 0.5f );
            glBegin( GL_QUADS );
            cbGhost( g_impOx, g_impOy, g_impOz, NULL );
            glEnd();
        }
    } else if( g_impStage == 1 ) {
        /* translucent column of image-width voxels along the chosen U axis */
        int i;
        glColor4f( 0.3f, 0.8f, 1.0f, 0.45f );
        glBegin( GL_QUADS );
        for( i = 0; i < g_impW; i++ )
            cbGhost( g_impOx + g_impUx*i, g_impOy + g_impUy*i,
                     g_impOz + g_impUz*i, NULL );
        glEnd();
    } else {
        /* full preview wall, tinted per pixel */
        glBegin( GL_QUADS );
        importWallForEach( cbImpGhost, NULL );
        glEnd();
    }
    glDepthMask( GL_TRUE );
    glDisable( GL_BLEND );
}

/* Update the live hover state for the current placement stage. */
static void importWallHover( int mx, int my )
{
    if( !g_impPix ) return;
    if( g_impStage == 0 ) {
        int cx, cy, cz, axis, dir, onGround;
        int savedMode = g_mode;
        g_mode = 0;   /* corner is always a placement (draw) cell */
        g_impHaveHover = pickCell( mx, my, &cx, &cy, &cz,
                                   &axis, &dir, &onGround );
        g_mode = savedMode;
        if( g_impHaveHover ) { g_impOx = cx; g_impOy = cy; g_impOz = cz; }
    } else if( g_impStage == 1 ) {
        int ax, ay, az;
        if( importSnapAxis( mx, my, 0, 0, 0, 0, &ax, &ay, &az ) ) {
            g_impUx = ax; g_impUy = ay; g_impUz = az;
        }
    } else if( g_impStage == 2 ) {
        int ax, ay, az;
        if( importSnapAxis( mx, my, 1, g_impUx, g_impUy, g_impUz,
                            &ax, &ay, &az ) ) {
            g_impVx = ax; g_impVy = ay; g_impVz = az;
        }
    }
}

/* Advance the three-click gesture, committing on the third click. */
static void importWallClick( int mx, int my )
{
    if( !g_impPix ) return;
    importWallHover( mx, my );   /* fold in the click position first */
    if( g_impStage == 0 ) {
        if( !g_impHaveHover ) return;
        g_impStage = 1;
        setStatus( "Corner set -- click to choose the image-width axis" );
    } else if( g_impStage == 1 ) {
        g_impStage = 2;
        setStatus( "Width axis set -- click to choose the height axis" );
    } else if( g_impStage == 2 ) {
        int count = 0;
        char m[64];
        groupBegin();
        importWallForEach( cbImpPlace, &count );
        groupEnd();
        g_renderDirty = 1;
        sprintf( m, "Placed %d voxels from image", count );
        setStatus( m );
        g_impStage = 0;   /* ready to stamp another copy */
        g_impHaveHover = 0;
    }
}

/* ------------------------------------------------------------------ */
/* oblique CPU renderer                                                */
/* ------------------------------------------------------------------ */

/* Evenly-spread unit-sphere sample offsets (Fibonacci sphere), used to spread a
 * soft light's shadow rays over a sphere of source points so edges get a
 * penumbra instead of a hard step.  Filled once at startup. */
#define SOFT_MAX_SAMPLES 64
static double g_sphereOff[ SOFT_MAX_SAMPLES ][3];
static int    g_sphereOffN = 0;

static void initSoftSamples( void )
{
    int i;
    double ga = M_PI * ( 3.0 - sqrt( 5.0 ) );   /* golden angle */
    for( i = 0; i < SOFT_MAX_SAMPLES; i++ ) {
        double y  = 1.0 - ( i + 0.5 ) * 2.0 / SOFT_MAX_SAMPLES;
        double r  = sqrt( 1.0 - y*y );
        double th = ga * i;
        g_sphereOff[i][0] = cos( th ) * r;
        g_sphereOff[i][1] = y;
        g_sphereOff[i][2] = sin( th ) * r;
    }
    g_sphereOffN = SOFT_MAX_SAMPLES;
}

/* Fraction (0..1) of a light visible from P.  For a hard light this is a single
 * shadow ray (1 or 0).  For a soft light we jitter the target over a sphere of
 * radius `size` (a jittered direction for an infinite sun) and average over the
 * caller-chosen number of rays -- so a wide soft radius costs only as many rays
 * as the user asks for, decoupling penumbra spread from render cost.
 * shadowFn is a world-frame ray caster (shadowedWorld); both the oblique
 * renderer and the 3D match preview now shade in true world space. */
static double softVisible( double px, double py, double pz,
                           double lx, double ly, double lz,
                           double dirx, double diry, double dirz,
                           int infinite, double size, int samples,
                           int (*shadowFn)( double,double,double,
                                            double,double,double ) )
{
    int i, ns, blocked = 0;
    if( size <= 1e-4 || samples <= 1 )
        return shadowFn( px, py, pz, lx, ly, lz ) ? 0.0 : 1.0;
    ns = samples;
    if( ns > g_sphereOffN ) ns = g_sphereOffN;
    for( i = 0; i < ns; i++ ) {
        double ox = g_sphereOff[i][0], oy = g_sphereOff[i][1], oz = g_sphereOff[i][2];
        double sx, sy, sz;
        if( infinite ) {
            /* jitter the direction toward the sun by an angular amount ~size */
            double ddx = dirx + ox*size*0.12;
            double ddy = diry + oy*size*0.12;
            double ddz = dirz + oz*size*0.12;
            double dl = sqrt( ddx*ddx + ddy*ddy + ddz*ddz );
            if( dl < 1e-6 ) dl = 1.0;
            sx = px + ddx/dl*1000.0; sy = py + ddy/dl*1000.0; sz = pz + ddz/dl*1000.0;
        } else {
            sx = lx + ox*size; sy = ly + oy*size; sz = lz + oz*size;
        }
        if( shadowFn( px, py, pz, sx, sy, sz ) ) blocked++;
    }
    return 1.0 - (double)blocked / ns;
}


/* Blend a blocky face normal toward a precomputed fitted surface normal.
 * When have==0 the face normal is returned unchanged.  (fnx,fny,fnz) and
 * (wnx,wny,wnz) share a frame -- uvw in the oblique renderer, world in the 3D
 * preview -- so this serves both.  g_smoothAmt sets how far to blend. */
static void blendSmoothN( int have, double wnx, double wny, double wnz,
                          double fnx, double fny, double fnz,
                          double *ox, double *oy, double *oz )
{
    double a, nx, ny, nz, len;
    *ox = fnx; *oy = fny; *oz = fnz;
    if( !have ) return;
    a = g_smoothAmt; if( a < 0.0 ) a = 0.0; if( a > 1.0 ) a = 1.0;
    nx = fnx*(1.0-a) + wnx*a;
    ny = fny*(1.0-a) + wny*a;
    nz = fnz*(1.0-a) + wnz*a;
    len = sqrt( nx*nx + ny*ny + nz*nz );
    if( len < 1e-6 ) return;   /* keep the face normal on a degenerate blend */
    *ox = nx/len; *oy = ny/len; *oz = nz/len;
}

/* Shading normal for one *visible* world face of voxel v whose flat axis normal
 * is (fx,fy,fz).  Both the oblique renderer and the 3D match preview call this
 * so they agree.
 *
 * Smoothing is now a per-FACE property (v->smoothFaces bitmask):
 *
 *   - A non-smooth face keeps its blocky axis normal (returned unchanged).
 *
 *   - A smooth face starts from the fitted surface normal (the negated gradient
 *     of local occupancy -- what makes a voxel sphere shade round), then is
 *     "met" to its neighbouring visible faces: for each of the four in-plane
 *     neighbour directions we find the visible face that continues the surface
 *     (coplanar if the next cell is solid & open above; a perpendicular face on
 *     THIS voxel if the next cell is empty (convex edge); a perpendicular face
 *     on the diagonal cell if the surface steps up (concave edge)).  Where that
 *     neighbour face is *not* smooth we constrain our normal so the two faces
 *     meet sanely:
 *       * a non-smooth COPLANAR neighbour locks us fully flat (we abut a flat
 *         run and must stay flat with it);
 *       * a non-smooth PERPENDICULAR neighbour locks the tangent component along
 *         its axis to zero, keeping us at 90 degrees to it while still free to
 *         rotate about the other in-plane axis.
 *     So a ring whose rim faces are smooth but whose flat top/bottom faces are
 *     not rounds only circumferentially (the vertical tangent is pinned by the
 *     flat caps), and a cylinder's top rim keeps a crisp flat cap edge. */
static void shadingNormalForFace( const Voxel *v,
                                  double fx, double fy, double fz,
                                  double *ox, double *oy, double *oz )
{
    double n[3];
    int faceAxis, s, k, lock[3];
    double len;

    *ox = fx; *oy = fy; *oz = fz;
    if( !( ( v->smoothFaces >> faceDir6( fx, fy, fz ) ) & 1 ) ) return;

    if( !voxSmoothNormal( v, &n[0], &n[1], &n[2] ) ) return;  /* flat fit */

    faceAxis = ( fy != 0.0 ) ? 1 : ( fz != 0.0 ? 2 : 0 );
    lock[0] = lock[1] = lock[2] = 0;

    /* examine the four axis-aligned in-plane neighbour faces */
    for( k = 0; k < 3; k++ ) {
        if( k == faceAxis ) continue;              /* only the two tangents */
        for( s = -1; s <= 1; s += 2 ) {
            int e[3], A[3], nsm, dr[3];
            e[0]=e[1]=e[2]=0; e[k] = s;            /* the in-plane step */
            A[0]=v->x+e[0]; A[1]=v->y+e[1]; A[2]=v->z+e[2];
            dr[0]=(faceAxis==0)?(int)(fx>0?1:-1):0;
            dr[1]=(faceAxis==1)?(int)(fy>0?1:-1):0;
            dr[2]=(faceAxis==2)?(int)(fz>0?1:-1):0;
            if( !voxAt( A[0], A[1], A[2] ) ) {
                /* convex edge: the neighbour face is on THIS voxel, normal e */
                nsm = faceIsSmoothAt( v->x, v->y, v->z,
                                      (double)e[0],(double)e[1],(double)e[2] );
                if( !nsm ) lock[k] = 1;            /* perpendicular -> lock tangent */
            } else if( !voxAt( A[0]+dr[0], A[1]+dr[1], A[2]+dr[2] ) ) {
                /* coplanar continuation: neighbour face (A, faceNormal) */
                nsm = faceIsSmoothAt( A[0], A[1], A[2], fx, fy, fz );
                if( !nsm ) { *ox=fx; *oy=fy; *oz=fz; return; }  /* full lock */
            } else {
                /* concave edge: neighbour face on cell A+dr, normal -e */
                nsm = faceIsSmoothAt( A[0]+dr[0], A[1]+dr[1], A[2]+dr[2],
                                      (double)-e[0],(double)-e[1],(double)-e[2] );
                if( !nsm ) lock[k] = 1;            /* perpendicular -> lock tangent */
            }
        }
    }

    for( k = 0; k < 3; k++ ) if( lock[k] ) n[k] = 0.0;
    len = sqrt( n[0]*n[0] + n[1]*n[1] + n[2]*n[2] );
    if( len < 1e-6 ) return;                       /* nothing left -> flat */
    n[0] /= len; n[1] /= len; n[2] /= len;

    blendSmoothN( 1, n[0], n[1], n[2], fx, fy, fz, ox, oy, oz );
}

/* Both the oblique renderer and the 3D preview shade in true world space. */
static void shadeWorld( double, double, double, double, double, double,
                        double, double, double, const Voxel *, unsigned char * );

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
    /* True-world-space shading: the uvw cell mapping negates some axes, which
     * shifts a continuous +1 face sample point by up to one cell when mapped
     * back to world.  We recover each sample's TRUE world position (fromUVW +
     * this constant shift) and shade it in world space with shadeWorld -- the
     * exact same math the "match render" 3D preview uses on true world faces,
     * so the baked pixels and the preview agree voxel-for-voxel. */
    double shX, shY, shZ;       /* uvw->world sample shift */
    double fnwX, fnwY, fnwZ;    /* front-face world normal (top is always +Y) */

    W      = g_voxPx;
    frontH = g_voxPx / g_frontScrunch; if( frontH < 1 ) frontH = 1;
    topH   = g_voxPx / g_topScrunch;   if( topH < 1 ) topH = 1;

    { double fx0, fy0, fz0;
      fromUVW( 0.5, 1.0, 0.5, &fx0, &fy0, &fz0 );   /* top-face center of vox 0 */
      shX = 0.5 - fx0; shY = 1.0 - fy0; shZ = 0.5 - fz0; }
    fromUVW( 0.0, 0.0, 1.0, &fnwX, &fnwY, &fnwZ );   /* +w unit dir -> world */

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

        /* ---- front face (+w side, world normal fnw) ---- */
        {
            int fy0 = (int)( by - frontH ), fy1 = (int)by;
            /* visible only if nothing occupies the cell in front */
            int occFront = voxOccUVW( u, v, w + 1 );
            double sfx, sfy, sfz;
            shadingNormalForFace( vox, fnwX, fnwY, fnwZ, &sfx, &sfy, &sfz );
            if( !occFront ) {
                for( py = fy0; py < fy1; py++ ) {
                    if( py < 0 || py >= imgH ) continue;
                    for( ny = 0; ny < W; ny++ ) {
                        double fx, fyf, Px, Py, Pz, Wx, Wy, Wz;
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
                            unsigned char *o = &g_img[ (py*imgW + px)*4 ];
                            zbuf[ py*imgW + px ] = depth;
                            fromUVW( Px, Py, Pz, &Wx, &Wy, &Wz );
                            Wx += shX; Wy += shY; Wz += shZ;
                            shadeWorld( Wx, Wy, Wz, sfx, sfy, sfz,
                                        fnwX, fnwY, fnwZ, vox, o );
                        }
                    }
                }
            }
        }

        /* ---- top face (+v side, world normal (0,1,0)) ---- */
        {
            int ty1 = (int)( by - frontH ), ty0 = ty1 - topH;
            int occTop = voxOccUVW( u, v + 1, w );
            double stx, sty, stz;
            shadingNormalForFace( vox, 0,1,0, &stx, &sty, &stz );
            if( !occTop ) {
                for( py = ty0; py < ty1; py++ ) {
                    if( py < 0 || py >= imgH ) continue;
                    for( ny = 0; ny < W; ny++ ) {
                        double fx, fzf, Px, Py, Pz, Wx, Wy, Wz;
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
                            unsigned char *o = &g_img[ (py*imgW + px)*4 ];
                            zbuf[ py*imgW + px ] = depth;
                            fromUVW( Px, Py, Pz, &Wx, &Wy, &Wz );
                            Wx += shX; Wy += shY; Wz += shZ;
                            shadeWorld( Wx, Wy, Wz, stx, sty, stz,
                                        0,1,0, vox, o );
                        }
                    }
                }
            }
        }
    }

    free( zbuf );
}

/* World-frame shadow ray: 1 if a voxel blocks P->L before reaching L. */
static int shadowedWorld( double px, double py, double pz,
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
        if( t > len - 0.02 ) return 0;
        if( voxAt( x, y, z ) ) return 1;
    }
    return 0;
}

/* Shade a face sample directly in world space -- the same Lambert + colored
 * point-light + shadow math the oblique renderer uses, so the 3D preview and
 * the baked pixel art agree surface-by-surface. */
static void shadeWorld( double px, double py, double pz,
                        double nx, double ny, double nz,
                        double gnx, double gny, double gnz,
                        const Voxel *v, unsigned char *out )
{
    int i;
    double accR = g_ambient, accG = g_ambient, accB = g_ambient;
    double scalarLit = g_ambient;
    for( i = 0; i < g_numLights; i++ ) {
        double ldx, ldy, ldz, len, nl, atten, f, lx, ly, lz, vis;
        if( !g_lights[i].enabled ) continue;
        if( g_lights[i].infinite ) {
            /* directional sun: (x,y,z) read as the direction toward the light */
            ldx = g_lights[i].x; ldy = g_lights[i].y; ldz = g_lights[i].z;
            len = sqrt( ldx*ldx + ldy*ldy + ldz*ldz );
            if( len < 1e-6 ) continue;
            ldx /= len; ldy /= len; ldz /= len;
            lx = px + ldx*1000.0; ly = py + ldy*1000.0; lz = pz + ldz*1000.0;
        } else {
            ldx = g_lights[i].x - px; ldy = g_lights[i].y - py; ldz = g_lights[i].z - pz;
            len = sqrt( ldx*ldx + ldy*ldy + ldz*ldz );
            if( len < 1e-5 ) continue;
            ldx /= len; ldy /= len; ldz /= len;
            lx = g_lights[i].x; ly = g_lights[i].y; lz = g_lights[i].z;
        }
        nl = nx*ldx + ny*ldy + nz*ldz;
        if( nl <= 0 ) continue;
        vis = softVisible( px + gnx*0.02, py + gny*0.02, pz + gnz*0.02, lx, ly, lz,
                           ldx, ldy, ldz, g_lights[i].infinite,
                           g_lights[i].size, g_lights[i].samples, shadowedWorld );
        if( vis <= 0.0 ) continue;
        atten = g_lights[i].infinite ? g_lights[i].intensity
                                     : g_lights[i].intensity / ( 1.0 + 0.03 * len );
        f = nl * atten * vis;
        accR += f * g_pal[ g_lights[i].color*3+0 ] / 255.0;
        accG += f * g_pal[ g_lights[i].color*3+1 ] / 255.0;
        accB += f * g_pal[ g_lights[i].color*3+2 ] / 255.0;
        scalarLit += f;
    }
    if( g_shadingMode == 1 ) {
        int rl = v->rampLen < 1 ? 1 : v->rampLen;
        int lo = clampi( v->rampStart, 0, g_palCount - 1 );
        int hi = clampi( v->rampStart + rl - 1, 0, g_palCount - 1 );
        int lumLo = g_pal[lo*3]+g_pal[lo*3+1]+g_pal[lo*3+2];
        int lumHi = g_pal[hi*3]+g_pal[hi*3+1]+g_pal[hi*3+2];
        double t = clampf( (float)scalarLit, 0.0f, 1.0f );
        int idx = (int)( t * rl );
        if( idx >= rl ) idx = rl - 1;
        if( lumLo > lumHi ) idx = rl - 1 - idx;
        idx = clampi( v->rampStart + idx, 0, g_palCount - 1 );
        out[0] = g_pal[ idx*3+0 ]; out[1] = g_pal[ idx*3+1 ]; out[2] = g_pal[ idx*3+2 ];
    } else {
        int bc = voxFlatColor( v );
        double br = g_pal[ bc*3+0 ] / 255.0;
        double bg = g_pal[ bc*3+1 ] / 255.0;
        double bb = g_pal[ bc*3+2 ] / 255.0;
        out[0] = (unsigned char)clampi( (int)( br * accR * 255.0 + 0.5 ), 0, 255 );
        out[1] = (unsigned char)clampi( (int)( bg * accG * 255.0 + 0.5 ), 0, 255 );
        out[2] = (unsigned char)clampi( (int)( bb * accB * 255.0 + 0.5 ), 0, 255 );
    }
    out[3] = 255;   /* opaque: the oblique renderer bakes onto a clear buffer */
}

/* Quick (shadowless) preview shade used for the 3D view's non-match modes. */
static void shadeQuick( double nx, double ny, double nz,
                        const Voxel *v, unsigned char *out )
{
    int fc = voxFlatColor( v );
    float f = previewFace( (float)nx, (float)ny, (float)nz );
    out[0] = (unsigned char)clampf( g_pal[fc*3+0] * f, 0, 255 );
    out[1] = (unsigned char)clampf( g_pal[fc*3+1] * f, 0, 255 );
    out[2] = (unsigned char)clampf( g_pal[fc*3+2] * f, 0, 255 );
}

static void pfacePush( float x, float y, float z, float nx, float ny, float nz,
                       const unsigned char *c )
{
    PFace *p;
    if( g_pfaceCount == g_pfaceCap ) {
        g_pfaceCap = g_pfaceCap ? g_pfaceCap * 2 : 4096;
        g_pface = (PFace*)realloc( g_pface, (size_t)g_pfaceCap * sizeof(PFace) );
    }
    p = &g_pface[ g_pfaceCount++ ];
    p->x=x; p->y=y; p->z=z; p->nx=nx; p->ny=ny; p->nz=nz;
    p->r=c[0]; p->g=c[1]; p->b=c[2];
}

/* Rebuild the exposed-face color cache for the 3D preview.  Mode 2 ("match
 * render") uses the full lit/shadowed shader at each face center; modes 0/1
 * use the cheap normal-keyed preview so orbiting stays instant. */
static void rebuildPreviewFaces( void )
{
    int i;
    static const int NB[6][3] = { {0,1,0},{0,-1,0},{0,0,1},{0,0,-1},{1,0,0},{-1,0,0} };
    static const float NR[6][3] = { {0,1,0},{0,-1,0},{0,0,1},{0,0,-1},{1,0,0},{-1,0,0} };
    g_pfaceCount = 0;
    if( g_previewShade == 0 ) return;   /* flat mode: use fallback path */
    for( i = 0; i < g_voxCap; i++ ) {
        int f; Voxel *v;
        if( g_vox[i].used != 1 ) continue;
        v = &g_vox[i];
        for( f = 0; f < 6; f++ ) {
            int nx = v->x+NB[f][0], ny = v->y+NB[f][1], nz = v->z+NB[f][2];
            unsigned char c[4];
            if( voxAt( nx, ny, nz ) ) continue;   /* hidden face */
            if( g_previewShade == 2 ) {
                double cxp = v->x + 0.5 + NR[f][0]*0.5;
                double cyp = v->y + 0.5 + NR[f][1]*0.5;
                double czp = v->z + 0.5 + NR[f][2]*0.5;
                double sx, sy, sz;
                shadingNormalForFace( v, NR[f][0], NR[f][1], NR[f][2],
                                      &sx, &sy, &sz );
                shadeWorld( cxp, cyp, czp, sx, sy, sz,
                            NR[f][0], NR[f][1], NR[f][2], v, c );
            } else {
                shadeQuick( NR[f][0],NR[f][1],NR[f][2], v, c );
            }
            pfacePush( (float)v->x,(float)v->y,(float)v->z,
                       NR[f][0],NR[f][1],NR[f][2], c );
        }
    }
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
        rebuildPreviewFaces();
        g_renderDirty = 0;
    }
}

static void savePNG( const char *path )
{
    ensureRender();
    if( g_img && g_imgW > 0 && g_imgH > 0 &&
        stbi_write_png( path, g_imgW, g_imgH, 4, g_img, g_imgW*4 ) ) {
        char msg[1400];
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
    fprintf( f, "OBLIQUEVOXELS 2\n" );
    fprintf( f, "PALETTE %s %d\n", g_palName, g_palCount );
    for( i = 0; i < g_palCount; i++ )
        fprintf( f, "C %d %d %d\n", g_pal[i*3+0], g_pal[i*3+1], g_pal[i*3+2] );
    fprintf( f, "AMBIENT %.4f\n", g_ambient );
    for( i = 0; i < g_numLights; i++ )
        fprintf( f, "L %.4f %.4f %.4f %d %.4f %d %d %.4f %d\n",
                 g_lights[i].x, g_lights[i].y, g_lights[i].z,
                 g_lights[i].color, g_lights[i].intensity, g_lights[i].enabled,
                 g_lights[i].infinite, g_lights[i].size, g_lights[i].samples );
    fprintf( f, "RENDER %d %d %d %d %d %d %.4f\n", g_shadingMode, g_voxPx,
             g_frontScrunch, g_topScrunch, g_orient,
             g_smoothRadius, g_smoothAmt );
    /* one voxel per line: V x y z color rampStart rampLen [smoothFaceMask]
     * The trailing field is a 6-bit per-face smooth mask (order +Y +Z ... see
     * faceDir6).  In the legacy version-1 format this field was a 0/1/2 whole-
     * voxel smooth flag; loadSculpture migrates those. */
    for( i = 0; i < g_voxCap; i++ ) {
        Voxel *v;
        if( g_vox[i].used != 1 ) continue;
        v = &g_vox[i];
        fprintf( f, "V %d %d %d %d %d %d %d\n",
                 v->x, v->y, v->z, v->color, v->rampStart, v->rampLen,
                 v->smoothFaces );
    }
    fclose( f );
    { char msg[1400]; sprintf( msg, "Saved %d voxels -> %s", g_voxUsed, path );
      setStatus( msg ); }
}

static int loadSculpture( const char *path )
{
    FILE *f = fopen( path, "r" );
    char line[ 256 ];
    int palIdx = 0, palExpected = 0, ver = 1;
    if( !f ) { setStatus( "Open failed" ); return 0; }
    if( !fgets( line, sizeof line, f ) ||
        strncmp( line, "OBLIQUEVOXELS", 13 ) != 0 ) {
        fclose( f ); setStatus( "Not an .ovox file" ); return 0;
    }
    sscanf( line, "OBLIQUEVOXELS %d", &ver );
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
            float x,y,z,inten,sz=0.0f; int col,en,inf=0,smp=8,got;
            got = sscanf( line, "L %f %f %f %d %f %d %d %f %d",
                          &x,&y,&z,&col,&inten,&en,&inf,&sz,&smp );
            if( got >= 6 && g_numLights < MAX_LIGHTS ) {
                g_lights[g_numLights].x=x; g_lights[g_numLights].y=y;
                g_lights[g_numLights].z=z; g_lights[g_numLights].color=col;
                g_lights[g_numLights].intensity=inten;
                g_lights[g_numLights].enabled=en;
                g_lights[g_numLights].infinite = ( got >= 7 ) ? inf : 0;
                g_lights[g_numLights].size = ( got >= 8 ) ? sz : 0.0f;
                g_lights[g_numLights].samples = ( got >= 9 ) ? smp : 8;
                g_numLights++;
            }
        } else if( line[0] == 'R' ) {
            float sa = g_smoothAmt; int sr = g_smoothRadius;
            sscanf( line, "RENDER %d %d %d %d %d %d %f", &g_shadingMode, &g_voxPx,
                    &g_frontScrunch, &g_topScrunch, &g_orient, &sr, &sa );
            g_smoothRadius = clampi( sr, 1, 4 );
            g_smoothAmt = clampf( sa, 0.0f, 1.0f );
        } else if( line[0] == 'V' && line[1] == ' ' ) {
            int x,y,z,col,rs,rl,sm=0;
            int got = sscanf( line, "V %d %d %d %d %d %d %d",
                              &x,&y,&z,&col,&rs,&rl,&sm );
            if( got >= 6 ) {
                Voxel *nv;
                voxSet( x,y,z,col,rs,rl );
                nv = voxAt( x,y,z );
                if( nv ) {
                    if( got < 7 )      nv->smoothFaces = 0;
                    else if( ver >= 2 ) nv->smoothFaces = sm & 0x3F;
                    else                /* legacy 0/1/2 whole-voxel smooth flag */
                        nv->smoothFaces = ( sm != 0 ) ? 0x3F : 0;
                }
            }
        }
        /* older files may carry 'S x y z' smoother-cell lines; silently ignored */
    }
    fclose( f );
    if( palIdx > 0 ) g_palCount = palIdx;
    (void)palExpected;
    g_pick = clampi( g_pick, 0, g_palCount-1 );
    g_rampStart = clampi( g_rampStart, 0, g_palCount-1 );
    g_rampEnd = clampi( g_rampEnd, g_rampStart, g_palCount-1 );
    g_renderDirty = 1;
    { char msg[1400]; sprintf( msg, "Loaded %d voxels from %s", g_voxUsed, path );
      setStatus( msg ); }
    return 1;
}

/* Read only the lighting (AMBIENT + L lines) from another .ovox file and use it
 * to REPLACE the current scene's lighting.  Voxels/palette are left untouched,
 * so lighting can be copied between sculptures (e.g. a matched chess set). */
static int importLighting( const char *path )
{
    FILE *f = fopen( path, "r" );
    char line[ 256 ];
    Light tmp[ MAX_LIGHTS ];
    int n = 0;
    float amb = g_ambient;
    int haveAmb = 0;
    if( !f ) { setStatus( "Import failed: cannot open file" ); return 0; }
    if( !fgets( line, sizeof line, f ) ||
        strncmp( line, "OBLIQUEVOXELS", 13 ) != 0 ) {
        fclose( f ); setStatus( "Import failed: not an .ovox file" ); return 0;
    }
    while( fgets( line, sizeof line, f ) ) {
        if( line[0] == 'A' ) {
            float a; if( sscanf( line, "AMBIENT %f", &a ) == 1 ) { amb=a; haveAmb=1; }
        } else if( line[0] == 'L' && line[1] == ' ' ) {
            float x,y,z,inten,sz=0.0f; int col,en,inf=0,smp=8,got;
            got = sscanf( line, "L %f %f %f %d %f %d %d %f %d",
                          &x,&y,&z,&col,&inten,&en,&inf,&sz,&smp );
            if( got >= 6 && n < MAX_LIGHTS ) {
                tmp[n].x=x; tmp[n].y=y; tmp[n].z=z; tmp[n].color=col;
                tmp[n].intensity=inten; tmp[n].enabled=en;
                tmp[n].infinite = ( got >= 7 ) ? inf : 0;
                tmp[n].size = ( got >= 8 ) ? sz : 0.0f;
                tmp[n].samples = ( got >= 9 ) ? smp : 8;
                n++;
            }
        }
    }
    fclose( f );
    { int i; for( i = 0; i < n; i++ ) g_lights[i] = tmp[i]; }
    g_numLights = n;
    if( haveAmb ) g_ambient = amb;
    g_selLight = ( n > 0 ) ? 0 : -1;
    g_renderDirty = 1;
    { char msg[1400];
      sprintf( msg, "Imported %d light%s from %s", n, n==1?"":"s", path );
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

/* Which modal to open next frame.  We must call ImGui::OpenPopup at the same
 * ID-stack scope as BeginPopupModal (root, in buildDialogs) -- calling it from
 * inside a menu resolves a different id and the popup silently never opens. */
static const char *g_pendingPopup = NULL;

/* ---- file-browser dialog state ---- */
static FSEntry    g_fbEntries[ 1024 ];
static int        g_fbCount = 0;
static char       g_fbDir[ 1024 ] = "";
static char       g_fbExt[ 16 ]   = "";
static char       g_fbFile[ 256 ] = "";
static const char *g_fbAction = NULL;   /* "Open" "Save" "PNG" "Palette" */

static void fbRefresh( void )
{
    g_fbCount = fs_list( g_fbDir, g_fbExt, g_fbEntries, 1024 );
}

static void setFbFile( const char *s )
{
    strncpy( g_fbFile, s, sizeof g_fbFile - 1 );
    g_fbFile[ sizeof g_fbFile - 1 ] = 0;
}

/* Open the unified file browser for the given purpose. */
static void openFileDialog( const char *id )
{
    g_pendingPopup = "FileBrowser";
    g_fbAction = id;
    if( g_fbDir[0] == 0 ) fs_cwd( g_fbDir, sizeof g_fbDir );
    if( strcmp( id, "PNG" ) == 0 )          { strcpy( g_fbExt, ".png" );  setFbFile( "render.png" ); }
    else if( strcmp( id, "ImportPNG" ) == 0 ) { strcpy( g_fbExt, ".png" ); setFbFile( "" ); }
    else if( strcmp( id, "Palette" ) == 0 ) { strcpy( g_fbExt, ".gpl" );  setFbFile( "" ); }
    else if( strcmp( id, "ImportLight" ) == 0 ) { strcpy( g_fbExt, ".ovox" ); setFbFile( "" ); }
    else                                    { strcpy( g_fbExt, ".ovox" ); setFbFile( "sculpture.ovox" ); }
    fbRefresh();
}

static void buildMenuBar( int *quit )
{
    if( !gui_begin_main_menu_bar() ) return;
    if( gui_begin_menu( "File" ) ) {
        if( gui_menu_item( "New", NULL, 1 ) ) {
            voxClear(); g_numLights = 0; histClear();
            g_renderDirty = 1;
            setStatus( "New sculpture" );
        }
        if( gui_menu_item( "Open .ovox...", NULL, 1 ) ) openFileDialog( "Open" );
        if( gui_menu_item( "Save .ovox...", NULL, 1 ) ) openFileDialog( "Save" );
        if( gui_menu_item( "Import lighting (.ovox)...", NULL, 1 ) )
            openFileDialog( "ImportLight" );
        gui_separator();
        if( gui_menu_item( "Load palette (.gpl)...", NULL, 1 ) )
            openFileDialog( "Palette" );
        if( gui_menu_item( "Import PNG as voxel wall...", NULL, 1 ) )
            openFileDialog( "ImportPNG" );
        if( gui_menu_item( "Export PNG...", NULL, 1 ) ) openFileDialog( "PNG" );
        gui_separator();
        if( gui_menu_item( "Quit", NULL, 1 ) ) *quit = 1;
        gui_end_menu();
    }
    if( gui_begin_menu( "Edit" ) ) {
        if( gui_menu_item( "Undo", "Ctrl+Z", g_histPos > 0 ) ) histUndo();
        if( gui_menu_item( "Redo", "Ctrl+Y", g_histPos < g_histLen ) ) histRedo();
        gui_separator();
        if( gui_menu_item( "Draw mode",  "B", 1 ) ) g_mode = 0;
        if( gui_menu_item( "Erase mode", "E", 1 ) ) g_mode = 1;
        gui_separator();
        if( gui_menu_item( "Pencil", "1", 1 ) ) g_tool = 0;
        if( gui_menu_item( "Line",   "2", 1 ) ) g_tool = 1;
        if( gui_menu_item( "Rect",   "3", 1 ) ) g_tool = 2;
        if( gui_menu_item( "Box",    "4", 1 ) ) g_tool = 3;
        if( gui_menu_item( "Select", "5", 1 ) ) g_tool = 4;
        if( gui_menu_item( "Scribble select", "6", 1 ) ) g_tool = 5;
        if( gui_menu_item( "Cylinder", "7", 1 ) ) g_tool = 6;
        if( gui_menu_item( "Sphere",   "8", 1 ) ) g_tool = 7;
        if( gui_menu_item( "Smoother (faces)", "9", 1 ) ) g_tool = 8;
        if( gui_menu_item( "Image wall", NULL, g_impPix ? 1 : 0 ) ) {
            g_tool = 9; g_impStage = 0; g_impHaveHover = 0;
        }
        gui_end_menu();
    }
    if( gui_begin_menu( "Select" ) ) {
        if( gui_menu_item( "Select all",   "Ctrl+A", 1 ) ) { selAll(); }
        if( gui_menu_item( "Clear",        "Esc",    1 ) ) { selClear(); }
        gui_separator();
        if( gui_menu_item( "Delete",       "Del",    1 ) ) selDelete();
        if( gui_menu_item( "Recolor",      NULL,     1 ) ) selRecolor();
        gui_separator();
        if( gui_menu_item( "Copy",         "Ctrl+C", 1 ) ) selCopy();
        if( gui_menu_item( "Paste",        "Ctrl+V", 1 ) ) selPaste();
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
        if( gui_menu_item( "Preview: flat",         NULL, 1 ) ) { g_previewShade=0; g_renderDirty=1; }
        if( gui_menu_item( "Preview: quick light",  NULL, 1 ) ) { g_previewShade=1; g_renderDirty=1; }
        if( gui_menu_item( "Preview: match render", NULL, 1 ) ) { g_previewShade=2; g_renderDirty=1; }
        gui_separator();
        gui_menu_item_check( "Voxel edges",        NULL, &g_previewEdges );
        gui_menu_item_check( "Smooth faces (cyan X)", NULL, &g_showSmoothWire );
        gui_menu_item_check( "Surface normals",    NULL, &g_showSurfNormals );
        gui_menu_item_check( "Hide voxels (faces only)", NULL, &g_hideVoxels );
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
    gui_set_next_window_size( g_leftW, h );
    gui_begin( "Tools", 1 );

    gui_separator_text( "Tool" );
    gui_radio_int( "Pencil", &g_tool, 0 ); gui_same_line();
    gui_radio_int( "Line",   &g_tool, 1 ); gui_same_line();
    gui_radio_int( "Rect",   &g_tool, 2 );
    gui_radio_int( "Box",    &g_tool, 3 ); gui_same_line();
    gui_radio_int( "Cylinder", &g_tool, 6 );
    gui_radio_int( "Sphere", &g_tool, 7 );
    gui_radio_int( "Select", &g_tool, 4 ); gui_same_line();
    gui_radio_int( "Scribble", &g_tool, 5 );
    gui_radio_int( "Smoother (faces)", &g_tool, 8 );
    if( g_impPix ) gui_radio_int( "Image wall", &g_tool, 9 );

    gui_separator_text( "Mode" );
    gui_radio_int( "Draw",  &g_mode, 0 ); gui_same_line();
    gui_radio_int( "Erase", &g_mode, 1 );
    /* auto-smooth: newly drawn (or erase-exposed) faces become smooth */
    if( g_tool <= 7 )
        gui_checkbox( "auto-smooth new faces", &g_autoSmooth );
    if( ( g_tool >= 1 && g_tool <= 3 ) || g_tool == 6 )
        gui_slider_int( "thickness", &g_thickness, 1, 32 );
    if( g_tool == 7 ) {
        gui_slider_int( "sphere depth", &g_sphereDepth, 0, 32 );
        gui_text( "drag on a surface to grow the ball" );
    }
    if( g_tool == 9 ) {
        gui_separator_text( "Image wall" );
        if( g_impPix ) {
            sprintf( buf, "%dx%d image loaded", g_impW, g_impH );
            gui_text( buf );
            if( g_impStage == 0 )
                gui_text( "1. click the bottom-left corner cell" );
            else if( g_impStage == 1 )
                gui_text( "2. click to aim the image-width axis" );
            else
                gui_text( "3. click to aim the height axis\n   (commits the wall)" );
            gui_text( "Esc resets placement." );
        } else {
            gui_text( "File > Import PNG as voxel wall..." );
        }
    }

    if( g_tool == 8 ) {
        gui_separator_text( "Smoother (faces)" );
        gui_text( "Drag over voxel faces to mark them\n"
                  "smooth (Draw) or flat (Erase).  A\n"
                  "smooth face rounds toward its\n"
                  "neighbours but stays perpendicular\n"
                  "to any flat face it meets." );
    }

    if( g_tool == 4 || g_tool == 5 ) {
        int nsel;
        gui_separator_text( "Selection" );
        if( g_tool == 4 ) {
            gui_text( "marquee depth (below/above surface):" );
            gui_slider_int( "below", &g_selBelow, 0, 64 );
            gui_slider_int( "above", &g_selAbove, 0, 64 );
        }
        nsel = selCount();
        sprintf( buf, "%d selected", nsel );
        gui_text( buf );
        if( gui_button( "All" ) ) selAll();
        gui_same_line();
        if( gui_button( "Clear" ) ) selClear();
        gui_same_line();
        if( gui_button( "Invert" ) ) selInvert();
        if( gui_button( "Delete" ) ) selDelete();
        gui_same_line();
        if( gui_button( "Recolor" ) ) selRecolor();
        if( gui_button( "Copy" ) ) selCopy();
        gui_same_line();
        if( gui_button( "Paste" ) ) selPaste();
        gui_text( "paste offset (x,y,z):" );
        gui_slider_int( "px", &g_pasteDX, -32, 32 );
        gui_slider_int( "py", &g_pasteDY, -32, 32 );
        gui_slider_int( "pz", &g_pasteDZ, -32, 32 );
        { static const char *dirItems[6] =
              { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
          gui_separator_text( "Extrude selection" );
          gui_combo( "dir", &g_extrudeDir, dirItems, 6 );
          gui_slider_int( "distance", &g_extrudeDist, 1, 64 );
          if( gui_button( "Extrude" ) ) selExtrude();
        }
        gui_separator_text( "Smooth shading" );
        gui_text( "mark ALL six faces of the selection\nsmooth (only visible ones shade round).\nUse the Smoother tool for single faces." );
        if( gui_button( "Smooth" ) ) selSmooth( 1 );
        gui_same_line();
        if( gui_button( "Unsmooth" ) ) selSmooth( 0 );
    }

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
    { float aw, ah; int perRow;
      gui_content_avail( &aw, &ah );
      perRow = (int)( aw / 22.0f );
      if( perRow < 4 ) perRow = 4;
      if( perRow > g_palCount ) perRow = g_palCount;
      if( perRow < 1 ) perRow = 1;
      gui_palette_grid( g_pal, g_palCount, perRow, 22.0f,
                        &g_pick, &g_rampStart, &g_rampEnd );
    }

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
        if( gui_checkbox( "infinite (sun)", &L->infinite ) ) g_renderDirty=1;
        gui_text( L->infinite ? "x,y,z = direction toward sun"
                              : "x,y,z = light position" );
        if( gui_slider_float( "x", &L->x, -40.0f, 40.0f, "%.1f" ) ) g_renderDirty=1;
        if( gui_slider_float( "y", &L->y, -40.0f, 40.0f, "%.1f" ) ) g_renderDirty=1;
        if( gui_slider_float( "z", &L->z, -40.0f, 40.0f, "%.1f" ) ) g_renderDirty=1;
        /* y-axis orbit: rotate (x,z) about the vertical.  The angle is derived
         * live from x,z each frame, so dragging x/z moves this slider too and
         * dragging this slider spins x/z around the origin at fixed radius. */
        { float rad = (float)sqrt( (double)L->x*L->x + (double)L->z*L->z );
          float ang = (float)( atan2( (double)L->z, (double)L->x ) * 180.0/M_PI );
          if( gui_slider_float( "y-angle", &ang, -180.0f, 180.0f, "%.0f deg" ) ) {
              double a = ang * M_PI / 180.0;
              L->x = (float)( rad * cos( a ) );
              L->z = (float)( rad * sin( a ) );
              g_renderDirty = 1;
          }
        }
        if( gui_slider_float( "intensity", &L->intensity, 0.0f, 3.0f, "%.2f" ) )
            g_renderDirty=1;
        if( gui_slider_float( "size (soft)", &L->size, 0.0f, 12.0f, "%.1f" ) )
            g_renderDirty=1;
        if( L->size > 0.05f ) {
            if( gui_slider_int( "soft rays", &L->samples, 1, SOFT_MAX_SAMPLES ) ) {
                if( L->samples < 1 ) L->samples = 1;
                g_renderDirty = 1;
            }
            gui_text( "more rays = smoother penumbra, slower" );
        } else {
            gui_text( "0 = hard point/sun" );
        }
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
    float pw = g_rightW;
    static const char *orientItems[4] = { "Front", "Right", "Back", "Left" };

    gui_set_next_window_pos( winW - pw, top );
    gui_set_next_window_size( pw, h );
    gui_begin( "Oblique Render", 1 );

    if( gui_slider_int( "voxel px", &g_voxPx, 1, 12 ) )       g_renderDirty = 1;
    if( gui_slider_int( "front scrunch", &g_frontScrunch, 1, 3 ) ) g_renderDirty = 1;
    if( gui_slider_int( "top scrunch", &g_topScrunch, 1, 3 ) )     g_renderDirty = 1;
    if( gui_combo( "view dir", &g_orient, orientItems, 4 ) )  g_renderDirty = 1;
    if( gui_slider_int( "smooth radius", &g_smoothRadius, 1, 4 ) ) g_renderDirty = 1;
    if( gui_slider_float( "smooth amount", &g_smoothAmt, 0.0f, 1.0f, "%.2f" ) )
        g_renderDirty = 1;
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

/* Carry out the current file-browser action against g_fbDir/g_fbFile. */
static void fbDoAction( void )
{
    char full[ 1300 ];
    if( !g_fbAction || g_fbFile[0] == 0 ) return;
    fs_join( g_fbDir, g_fbFile, full, sizeof full );
    if( strcmp( g_fbAction, "Open" ) == 0 ) {
        loadSculpture( full );
    } else if( strcmp( g_fbAction, "Save" ) == 0 ) {
        saveSculpture( full );
    } else if( strcmp( g_fbAction, "PNG" ) == 0 ) {
        savePNG( full );
    } else if( strcmp( g_fbAction, "ImportPNG" ) == 0 ) {
        importWallLoad( full );
    } else if( strcmp( g_fbAction, "ImportLight" ) == 0 ) {
        importLighting( full );
    } else if( strcmp( g_fbAction, "Palette" ) == 0 ) {
        if( paletteLoad( full ) ) {
            g_renderDirty = 1;
            g_pick = clampi( g_pick, 0, g_palCount-1 );
            g_rampStart = clampi( g_rampStart, 0, g_palCount-1 );
            g_rampEnd = clampi( g_rampEnd, g_rampStart, g_palCount-1 );
            setStatus( "Palette loaded" );
        } else setStatus( "Palette load failed" );
    }
}

static void buildDialogs( void )
{
    const char *act, *actLabel, *title;
    int i, navigated = 0;
    char label[ 300 ];

    if( g_pendingPopup ) { gui_open_popup( g_pendingPopup ); g_pendingPopup = NULL; }
    if( !gui_begin_popup_modal( "FileBrowser" ) ) return;

    act = g_fbAction ? g_fbAction : "Open";
    if( strcmp( act, "Save" ) == 0 )         { title = "Save sculpture (.ovox)";  actLabel = "Save"; }
    else if( strcmp( act, "PNG" ) == 0 )     { title = "Export render (.png)";    actLabel = "Export"; }
    else if( strcmp( act, "ImportPNG" ) == 0 ) { title = "Import PNG as voxel wall"; actLabel = "Import"; }
    else if( strcmp( act, "ImportLight" ) == 0 ) { title = "Import lighting from (.ovox)"; actLabel = "Import"; }
    else if( strcmp( act, "Palette" ) == 0 ) { title = "Load palette (.gpl)";     actLabel = "Load"; }
    else                                     { title = "Open sculpture (.ovox)";  actLabel = "Open"; }

    gui_text( title );
    gui_text( g_fbDir );

    if( gui_begin_child( "fblist", 460.0f, 300.0f, 1 ) ) {
        for( i = 0; i < g_fbCount && !navigated; i++ ) {
            int isDir = g_fbEntries[i].isDir;
            int selected = ( !isDir && strcmp( g_fbEntries[i].name, g_fbFile ) == 0 );
            gui_push_id( i );
            if( isDir ) sprintf( label, "[%.250s]", g_fbEntries[i].name );
            else        sprintf( label, "%.250s", g_fbEntries[i].name );
            if( gui_selectable( label, selected ) ) {
                if( isDir ) {
                    char nd[ 1024 ];
                    fs_join( g_fbDir, g_fbEntries[i].name, nd, sizeof nd );
                    strncpy( g_fbDir, nd, sizeof g_fbDir - 1 );
                    g_fbDir[ sizeof g_fbDir - 1 ] = 0;
                    fbRefresh();
                    navigated = 1;
                } else {
                    setFbFile( g_fbEntries[i].name );
                }
            }
            gui_pop_id();
        }
    }
    gui_end_child();

    if( gui_input_text( "name", g_fbFile, sizeof g_fbFile ) ) {
        fbDoAction(); gui_close_current_popup();
    }
    if( gui_button( actLabel ) ) { fbDoAction(); gui_close_current_popup(); }
    gui_same_line();
    if( gui_button( "Cancel" ) ) gui_close_current_popup();
    gui_end_popup();
}

static void buildStatusBar( float winW, float winH )
{
    gui_set_next_window_pos( g_leftW, winH - 26.0f );
    gui_set_next_window_size( winW - g_leftW - g_rightW, 26.0f );
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

/* Is window pixel (x,y) inside the raw-GL 3D viewport?  Used to gate voxel
 * clicks geometrically -- ImGui's hover flag lags a frame, so a click landing
 * the instant the cursor reaches a side panel could otherwise leak into the
 * 3D view and drop a stray voxel far off in space. */
static int inView( int x, int y )
{
    return x >= g_viewX && x < g_viewX + g_viewW &&
           y >= g_viewY && y < g_viewY + g_viewH;
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

    initSoftSamples();
    voxInit( 1024 );
    if( argc > 1 ) {
        if( !loadSculpture( argv[1] ) ) buildDemoScene();
    } else {
        buildDemoScene();
    }
    setStatus( "Pencil: left-click paints.  Line/Rect/Box/Select: left-drag.  "
               "Right-drag: orbit  |  Mid-drag: pan  |  Wheel: zoom" );

    while( running ) {
        SDL_Event ev;
        int winW, winH;
        float menuH;
        int overGui;

        while( SDL_PollEvent( &ev ) ) {
            gui_process_event( &ev );
            overGui = gui_any_popup_open();   /* immediate, non-lagged */
            switch( ev.type ) {
                case SDL_QUIT: running = 0; break;
                case SDL_KEYDOWN:
                    if( !gui_want_capture_keyboard() ) {
                        int ctrl = ( ev.key.keysym.mod & KMOD_CTRL ) ? 1 : 0;
                        int shift = ( ev.key.keysym.mod & KMOD_SHIFT ) ? 1 : 0;
                        SDL_Keycode k = ev.key.keysym.sym;
                        if( ctrl && k == SDLK_z ) { if( shift ) histRedo(); else histUndo(); }
                        else if( ctrl && k == SDLK_y ) histRedo();
                        else if( ctrl && k == SDLK_c ) selCopy();
                        else if( ctrl && k == SDLK_v ) selPaste();
                        else if( ctrl && k == SDLK_a ) { selAll(); setStatus("Selected all"); }
                        else if( k == SDLK_b ) g_mode = 0;   /* draw */
                        else if( k == SDLK_e ) g_mode = 1;   /* erase */
                        else if( k == SDLK_1 ) g_tool = 0;
                        else if( k == SDLK_2 ) g_tool = 1;
                        else if( k == SDLK_3 ) g_tool = 2;
                        else if( k == SDLK_4 ) g_tool = 3;
                        else if( k == SDLK_5 ) g_tool = 4;
                        else if( k == SDLK_6 ) g_tool = 5;
                        else if( k == SDLK_7 ) g_tool = 6;
                        else if( k == SDLK_8 ) g_tool = 7;
                        else if( k == SDLK_9 ) g_tool = 8;
                        else if( k == SDLK_DELETE || k == SDLK_BACKSPACE ) selDelete();
                        else if( k == SDLK_ESCAPE ) {
                            if( g_tool == 9 && g_impPix && g_impStage > 0 ) {
                                g_impStage = 0; g_impHaveHover = 0;
                                setStatus( "Placement reset -- click a new corner" );
                            } else { selClear(); setStatus("Selection cleared"); }
                        }
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    /* grab a panel splitter handle if the click landed on one */
                    if( ev.button.button == SDL_BUTTON_LEFT && !overGui ) {
                        int ww, wh, mx = ev.button.x;
                        SDL_GetWindowSize( window, &ww, &wh );
                        if( mx >= (int)g_leftW - 6 && mx < (int)g_leftW )
                            g_splitDrag = 1;
                        else if( mx >= ww - (int)g_rightW &&
                                 mx < ww - (int)g_rightW + 6 )
                            g_splitDrag = 2;
                    }
                    if( !overGui && !g_splitDrag &&
                        inView( ev.button.x, ev.button.y ) ) {
                        g_dragBtn = ev.button.button;
                        g_downX = g_lastX = ev.button.x;
                        g_downY = g_lastY = ev.button.y;
                        g_moved = 0;
                        /* scribble-select paints selection along the drag;
                         * other region tools grab the left button for a plane
                         * gesture */
                        if( g_dragBtn == SDL_BUTTON_LEFT && g_tool == 5 ) {
                            g_scribbling = 1;
                            scribbleAt( ev.button.x, ev.button.y );
                        } else if( g_dragBtn == SDL_BUTTON_LEFT && g_tool == 8 ) {
                            g_smoothing = 1;
                            groupBegin();
                            smoothFaceAt( ev.button.x, ev.button.y );
                        } else if( g_dragBtn == SDL_BUTTON_LEFT && g_tool == 7 )
                            sphereBegin( ev.button.x, ev.button.y );
                        else if( g_dragBtn == SDL_BUTTON_LEFT &&
                                 g_tool != 0 && g_tool != 9 )
                            gestureBegin( ev.button.x, ev.button.y );
                    }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if( ev.button.button == SDL_BUTTON_LEFT ) g_splitDrag = 0;
                    if( g_dragBtn == ev.button.button ) {
                        if( g_dragBtn == SDL_BUTTON_LEFT ) {
                            if( g_gActive ) { regionCommit(); g_gActive = 0; }
                            else if( g_sphActive ) { sphereCommit(); g_sphActive = 0; }
                            else if( g_scribbling ) {
                                char m[64];
                                sprintf( m, "%s (%d selected)",
                                    g_mode==1 ? "Scribble deselect" : "Scribble select",
                                    selCount() );
                                setStatus( m );
                                g_scribbling = 0;
                            }
                            else if( g_smoothing ) {
                                groupEnd();
                                g_smoothing = 0;
                                setStatus( g_mode==1 ? "Faces unsmoothed"
                                                     : "Faces smoothed" );
                            }
                            else if( !g_moved && g_tool == 0 )
                                applyToolAt( ev.button.x, ev.button.y );
                            else if( !g_moved && g_tool == 9 )
                                importWallClick( ev.button.x, ev.button.y );
                        }
                        g_dragBtn = 0;
                    }
                    break;
                case SDL_MOUSEMOTION:
                    /* image-wall placement tracks the cursor with no button held */
                    if( g_tool == 9 && g_impStage >= 0 && !overGui &&
                        inView( ev.motion.x, ev.motion.y ) )
                        importWallHover( ev.motion.x, ev.motion.y );
                    if( g_splitDrag == 1 )      g_leftW  += ev.motion.xrel;
                    else if( g_splitDrag == 2 ) g_rightW -= ev.motion.xrel;
                    else if( g_dragBtn ) {
                        int dx = ev.motion.x - g_lastX;
                        int dy = ev.motion.y - g_lastY;
                        g_lastX = ev.motion.x; g_lastY = ev.motion.y;
                        if( abs( ev.motion.x - g_downX ) +
                            abs( ev.motion.y - g_downY ) > 3 ) g_moved = 1;
                        if( g_scribbling && g_dragBtn == SDL_BUTTON_LEFT )
                            scribbleAt( ev.motion.x, ev.motion.y );
                        else if( g_smoothing && g_dragBtn == SDL_BUTTON_LEFT )
                            smoothFaceAt( ev.motion.x, ev.motion.y );
                        else if( g_sphActive && g_dragBtn == SDL_BUTTON_LEFT )
                            sphereUpdate( ev.motion.x, ev.motion.y );
                        else if( g_gActive && g_dragBtn == SDL_BUTTON_LEFT )
                            gestureUpdate( ev.motion.x, ev.motion.y );
                        else if( g_dragBtn == SDL_BUTTON_LEFT ||
                                 g_dragBtn == SDL_BUTTON_RIGHT )
                            orbitDrag( dx, dy );   /* right-drag always orbits */
                        else
                            panDrag( dx, dy );      /* middle-drag pans */
                    }
                    break;
                case SDL_MOUSEWHEEL:
                    { int mxw, myw; SDL_GetMouseState( &mxw, &myw );
                      if( !overGui && inView( mxw, myw ) ) {
                        cam_dist *= ( ev.wheel.y > 0 ) ? 0.9f : 1.1111f;
                        cam_dist = clampf( cam_dist, 1.5f, 400.0f );
                      } }
                    break;
                default: break;
            }
        }

        SDL_GetWindowSize( window, &winW, &winH );
        g_winH = winH;
        menuH = gui_main_menu_bar_height();

        /* keep panel widths sane for the current window */
        if( g_rightW < 200.0f ) g_rightW = 200.0f;
        if( g_rightW > winW - 160.0f - 120.0f ) g_rightW = winW - 160.0f - 120.0f;
        if( g_rightW < 200.0f ) g_rightW = 200.0f;
        if( g_leftW < 160.0f ) g_leftW = 160.0f;
        if( g_leftW > winW - g_rightW - 120.0f )
            g_leftW = winW - g_rightW - 120.0f;
        if( g_leftW < 160.0f ) g_leftW = 160.0f;

        /* 3D view occupies the middle strip between the two side panels */
        g_viewX = (int)g_leftW;
        g_viewY = (int)menuH;
        g_viewW = winW - (int)g_leftW - (int)g_rightW;
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
        ensureRender();          /* keep the lit-preview cache current */
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

        /* draggable splitter handles at each panel/view boundary (drawn on the
         * foreground; the drag itself is handled in the SDL event loop) */
        { int mx, my, hotL, hotR;
          float sy = menuH, sh = winH - menuH - 26.0f;
          SDL_GetMouseState( &mx, &my );
          hotL = ( g_splitDrag == 1 ) ||
                 ( mx >= (int)g_leftW - 6 && mx < (int)g_leftW );
          hotR = ( g_splitDrag == 2 ) ||
                 ( mx >= winW - (int)g_rightW && mx < winW - (int)g_rightW + 6 );
          gui_overlay_rect( g_leftW - 6.0f, sy, g_leftW, sy + sh,
                            hotL?120:66, hotL?145:70, hotL?190:80, 255 );
          gui_overlay_rect( (float)winW - g_rightW, sy,
                            (float)winW - g_rightW + 6.0f, sy + sh,
                            hotR?120:66, hotR?145:70, hotR?190:80, 255 );
        }
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
