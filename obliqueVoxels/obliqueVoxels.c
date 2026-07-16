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

/* The default palette, baked in so a fresh scene never depends on a .gpl
 * sitting in the working directory: Sheltzy32 with ramps, exactly the contents
 * of sheltzy32_withRamps.gpl.  The first 32 entries are the base palette; then
 * 8 black padding entries align what follows to rows of 10; then ten 10-entry
 * hue ramps (dark -> light) that the palette grid can be dragged across. */
static const unsigned char g_palDefault[ 140 * 3 ] = {
    140,255,222,  69,184,179, 131,151, 64, 201,236,133,  70,198, 87,
     21,137,104,  44, 91,109,  34, 42, 92,  86,106,137, 139,171,191,
    204,226,225, 255,219,165, 204,172,104, 163,109, 62, 104, 60, 52,
      0,  0,  0,  56,  0, 44, 102, 59,147, 139,114,222, 156,216,252,
     94,150,221,  57, 83,192, 128, 12, 83, 195, 75,145, 255,148,179,
    189, 31, 63, 236, 97, 74, 255,164,104, 255,246,174, 255,218,112,
    244,176, 60, 255,255,255,   0,  0,  0,   0,  0,  0,   0,  0,  0,
      0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,
      0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,  56,  0, 44,
     34, 42, 92,  44, 91,109,  69,184,179, 156,216,252, 255,255,255,
      0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,  56,  0, 44,
     34, 42, 92,  57, 83,192,  94,150,221, 156,216,252, 255,255,255,
      0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,  56,  0, 44,
     34, 42, 92,  86,106,137, 139,171,191, 204,226,225, 255,255,255,
      0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,
     56,  0, 44, 102, 59,147, 139,114,222, 156,216,252, 255,255,255,
      0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,
     56,  0, 44, 128, 12, 83, 195, 75,145, 255,148,179, 255,255,255,
      0,  0,  0,   0,  0,  0,   0,  0,  0,  56,  0, 44, 128, 12, 83,
    189, 31, 63, 236, 97, 74, 255,164,104, 255,219,165, 255,255,255,
      0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,  56,  0, 44,
    104, 60, 52, 163,109, 62, 204,172,104, 255,219,165, 255,255,255,
      0,  0,  0,   0,  0,  0,   0,  0,  0,  56,  0, 44, 104, 60, 52,
    163,109, 62, 244,176, 60, 255,218,112, 255,246,174, 255,255,255,
      0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,  56,  0, 44,
     34, 42, 92,  44, 91,109, 131,151, 64, 201,236,133, 255,255,255,
      0,  0,  0,   0,  0,  0,   0,  0,  0,  56,  0, 44,  34, 42, 92,
     44, 91,109,  21,137,104,  70,198, 87, 201,236,133, 255,255,255
};

static void paletteDefault( void )
{
    memcpy( g_pal, g_palDefault, sizeof g_palDefault );
    g_palCount = (int)( sizeof g_palDefault / 3 );
    strcpy( g_palName, "Sheltzy32" );
}

/* Palette index of the brightest entry -- the sane default color for a new
 * light.  A fixed index would land on one of the ramp-alignment padding blacks
 * in a palette like the default one, and a black light renders nothing. */
static int paletteBrightest( void )
{
    int i, best = 0, bestLum = -1;
    for( i = 0; i < g_palCount; i++ ) {
        int lum = g_pal[i*3+0] + g_pal[i*3+1] + g_pal[i*3+2];
        if( lum > bestLum ) { bestLum = lum; best = i; }
    }
    return best;
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

/* ---- layers -------------------------------------------------------
 * The model is a small stack of independent voxel layers.  Editing acts on
 * the ACTIVE layer only; rendering/preview/picking act on the COMPOSITE (all
 * *visible* layers unioned, a higher layer index winning where cells coincide).
 * The composite is built into a reserved scratch slot (FLAT_LAYER) on demand.
 *
 * Trick: g_vox / g_voxCap / g_voxUsed / g_voxTomb are macros aliasing the
 * active layer's fields, so every existing spatial-hash routine and edit loop
 * operates on the active layer with no change.  A render/pick pass temporarily
 * points g_activeLayer at FLAT_LAYER so the very same code sees the union. */
#define MAX_LAYERS 16
#define FLAT_LAYER MAX_LAYERS      /* reserved scratch slot for the composite */

typedef struct {
    Voxel *vox;
    int    cap, used, tomb;        /* the spatial-hash state for this layer */
    int    visible;                /* shown in composite render/preview */
    char   name[32];
} Layer;

static Layer g_layers[ MAX_LAYERS + 1 ];   /* +1 for the FLAT_LAYER scratch */
static int   g_numLayers   = 1;
static int   g_activeLayer = 0;
static int   g_flatDirty   = 1;    /* composite needs rebuilding */

#define g_vox     ( g_layers[ g_activeLayer ].vox  )
#define g_voxCap  ( g_layers[ g_activeLayer ].cap  )
#define g_voxUsed ( g_layers[ g_activeLayer ].used )
#define g_voxTomb ( g_layers[ g_activeLayer ].tomb )

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
    if( g_activeLayer != FLAT_LAYER ) g_flatDirty = 1;
}

static void voxErase( int x, int y, int z )
{
    int i = voxFind( x, y, z );
    if( i >= 0 ) { g_vox[i].used = 2; g_voxUsed--; g_voxTomb++; }
    if( g_activeLayer != FLAT_LAYER ) g_flatDirty = 1;
}

/* ------------------------------------------------------------------ */
/* layer management                                                    */
/* ------------------------------------------------------------------ */

static void histClear( void );        /* forward: structural edits reset undo */
static void setStatus( const char *s );
static int  g_renderDirty;            /* tentative decl; defined with =1 below */

/* Free every layer and return to a single empty layer. */
static void layersReset( void )
{
    int i;
    for( i = 0; i <= MAX_LAYERS; i++ ) {
        free( g_layers[i].vox );
        g_layers[i].vox = NULL;
        g_layers[i].cap = g_layers[i].used = g_layers[i].tomb = 0;
        g_layers[i].visible = 1;
        g_layers[i].name[0] = '\0';
    }
    g_numLayers = 1;
    g_activeLayer = 0;
    strcpy( g_layers[0].name, "Layer 1" );
    g_flatDirty = 1;
}

/* Rebuild the FLAT scratch layer as the union of all visible layers, a higher
 * layer index winning where cells coincide.  A no-op unless g_flatDirty. */
static void ensureFlat( void )
{
    int save, L, i;
    if( !g_flatDirty ) return;
    save = g_activeLayer;
    g_activeLayer = FLAT_LAYER;
    voxClear();
    for( L = 0; L < g_numLayers; L++ ) {
        if( !g_layers[L].visible ) continue;
        for( i = 0; i < g_layers[L].cap; i++ ) {
            Voxel *s = &g_layers[L].vox[i];
            Voxel *d;
            if( s->used != 1 ) continue;
            voxSet( s->x, s->y, s->z, s->color, s->rampStart, s->rampLen );
            d = voxAt( s->x, s->y, s->z );
            if( d ) { d->smoothFaces = s->smoothFaces; d->sel = s->sel; }
        }
    }
    g_activeLayer = save;
    g_flatDirty = 0;
}

/* Switch the active layer, keeping the selection "live" positionally: the set
 * of selected cell positions is projected onto whatever the new layer has at
 * those positions.  This makes cross-layer boolean gestures work -- select a
 * sphere in one layer, switch to a box layer, and the sphere-shaped overlap is
 * now selected. */
static void setActiveLayer( int L )
{
    int i, n = 0, k = 0, *buf = NULL;
    if( L < 0 || L >= g_numLayers || L == g_activeLayer ) return;
    for( i = 0; i < g_voxCap; i++ )
        if( g_vox[i].used == 1 && g_vox[i].sel ) n++;
    if( n ) {
        buf = (int*)malloc( sizeof(int) * 3 * n );
        for( i = 0; i < g_voxCap; i++ )
            if( g_vox[i].used == 1 && g_vox[i].sel ) {
                buf[k*3+0] = g_vox[i].x; buf[k*3+1] = g_vox[i].y;
                buf[k*3+2] = g_vox[i].z; k++;
            }
    }
    g_activeLayer = L;
    for( i = 0; i < g_voxCap; i++ )
        if( g_vox[i].used == 1 ) g_vox[i].sel = 0;
    for( i = 0; i < n; i++ ) {
        Voxel *v = voxAt( buf[i*3+0], buf[i*3+1], buf[i*3+2] );
        if( v ) v->sel = 1;
    }
    free( buf );
    g_renderDirty = 1;
}

static void layerAdd( void )
{
    int idx = g_numLayers;
    if( g_numLayers >= MAX_LAYERS ) { setStatus( "Max layers reached" ); return; }
    g_layers[idx].vox = NULL;
    g_layers[idx].cap = g_layers[idx].used = g_layers[idx].tomb = 0;
    g_layers[idx].visible = 1;
    sprintf( g_layers[idx].name, "Layer %d", idx + 1 );
    g_numLayers++;
    setActiveLayer( idx );      /* projects (clears) selection onto the new layer */
    histClear();                /* undo can't cross a layer-set change */
    g_flatDirty = 1; g_renderDirty = 1;
}

static void layerDelete( int L )
{
    int i;
    if( g_numLayers <= 1 ) { setStatus( "Can't delete the last layer" ); return; }
    if( L < 0 || L >= g_numLayers ) return;
    free( g_layers[L].vox );
    for( i = L; i < g_numLayers - 1; i++ ) g_layers[i] = g_layers[i+1];
    g_numLayers--;
    /* zero the vacated top slot so its (now duplicated) pointer isn't freed twice */
    g_layers[g_numLayers].vox = NULL;
    g_layers[g_numLayers].cap = g_layers[g_numLayers].used =
        g_layers[g_numLayers].tomb = 0;
    if( g_activeLayer >= g_numLayers ) g_activeLayer = g_numLayers - 1;
    else if( g_activeLayer > L )       g_activeLayer--;
    histClear();
    g_flatDirty = 1; g_renderDirty = 1;
}

/* Swap two layers to reorder the composite z-order (higher index wins). */
static void layerSwap( int a, int b )
{
    Layer t;
    if( a < 0 || b < 0 || a >= g_numLayers || b >= g_numLayers || a == b ) return;
    t = g_layers[a]; g_layers[a] = g_layers[b]; g_layers[b] = t;
    if( g_activeLayer == a )      g_activeLayer = b;
    else if( g_activeLayer == b ) g_activeLayer = a;
    histClear();
    g_flatDirty = 1; g_renderDirty = 1;
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
                                    * 8 smoother (per-face), 9 image wall,
                                    * 10 eyedropper */
static int g_mode = 0;             /* 0 draw, 1 erase */
static int g_altEyedrop = 0;       /* ALT held: temporarily forced eyedropper */
static int g_savedTool = 0;        /* tool to restore when ALT is released */
static int g_autoSmooth = 0;       /* if set, drawing tools mark every face of
                                    * placed voxels smooth (and erasing marks the
                                    * newly-exposed cavity faces smooth). */
static int g_thickness = 1;        /* extrude depth for line/rect/box/cyl (>=1) */
static int g_sphereDepth = 0;      /* voxels the sphere sinks into the surface */
static int g_pick = 15;            /* current palette index */
static int g_rampStart = 15;
static int g_rampEnd   = 15;

/* ---- symmetry planes ---- every editing operation (draw / erase / select /
 * smooth / sphere / eyedrop-excepted) is mirrored across each enabled plane, so
 * up to 8-fold symmetry is possible.  A plane perpendicular to axis a sits at
 * world coordinate g_symPos[a] (a cell boundary) or g_symPos[a]+0.5 (through the
 * centre of that column of cells) when g_symHalf[a] is set -- the latter lets an
 * odd-width shape stay symmetric about its middle voxel. */
static int g_symOn[3]   = { 0, 0, 0 };   /* mirror across X(0)/Y(1)/Z(2)? */
static int g_symPos[3]  = { 0, 0, 0 };   /* plane position (integer cell coord) */
static int g_symHalf[3] = { 0, 0, 0 };   /* +0.5: plane through cell centres    */
static int g_symShow    = 0;             /* draw translucent symmetry planes?   */

/* ---- pending selection move ---- a live, uncommitted translation of the
 * current selection.  The originals stay put and a green ghost previews the
 * moved copy until "Commit Move" bakes it (overwriting collisions, undoable). */
static int g_moveDX = 0, g_moveDY = 0, g_moveDZ = 0;

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
static int g_previewShade = 2;     /* 0 flat, 1 quick preview, 2 match render */
static int g_previewEdges = 1;
static int g_showSmoothWire = 1;   /* cyan outline + X on smoothed voxel faces   */
static int g_showSurfNormals = 0;  /* draw fitted surface normals (best-fit viz) */
static int g_hideVoxels      = 0;  /* hide solid faces so the fitted surface
                                    * tiles/normals can be seen by themselves   */

/* paste offset (voxels) applied by the "Paste" op */
static int g_pasteDX = 2, g_pasteDY = 0, g_pasteDZ = 0;

/* oblique render parameters */
static int g_shadingMode  = 0;     /* 0 natural, 1 palette-ramp */

/* Specular highlight.  g_shininess is a pure MIX amount: at 0 no specular term
 * is computed or added at all, so the render is bit-for-bit the plain
 * Lambert + shadow result it has always been. */
static float g_shininess = 0.0f;   /* 0 = off (matte, the original look)      */
static float g_specPower = 24.0f;  /* Phong / Blinn-Phong exponent            */
static int   g_specBlinn = 1;      /* 1 = Blinn-Phong, 0 = classic Phong      */
static int g_voxPx        = 6;     /* voxel pixel size (horizontal & base) */
static int g_frontScrunch = 1;     /* 1..3 : front face height = voxPx/scrunch */
static int g_topScrunch   = 1;     /* 1..3 : top face height   = voxPx/scrunch */
static int g_orient       = 0;     /* 0..3 : 90-degree yaw of the oblique view */
static float g_prevZoom   = 3.0f;  /* preview magnification (integer, wheel+slider) */
static float g_prevPanX   = 0.0f;  /* preview pan offset, screen px from centre */
static float g_prevPanY   = 0.0f;
static int g_selLight     = -1;    /* selected light in the panel (-1 none) */
static int g_ortho        = 0;     /* 3D view: 0 perspective, 1 orthographic */

/* Optional background image shown behind the pixel-render preview, for context.
 * Stored as raw RGBA so it can be baked into the .ovox file (base64). */
static unsigned char *g_bgPix = NULL;   /* g_bgW*g_bgH*4 RGBA, or NULL */
static int    g_bgW = 0, g_bgH = 0;
static GLuint g_bgTex = 0;
static int    g_bgShow = 1;             /* view toggle: draw the BG image */
static int    g_bgOffX = 0, g_bgOffY = 0; /* BG image offset in render pixels */

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
    int   layer;            /* which layer this edit applies to */
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
    e.layer = g_activeLayer;   /* edits always target the active layer */
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
    int save = g_activeLayer;
    if( e->layer >= 0 && e->layer < g_numLayers ) g_activeLayer = e->layer;
    if( e->hadBefore ) {
        Voxel *v;
        voxSet( e->x, e->y, e->z, e->before.color,
                e->before.rampStart, e->before.rampLen );
        v = voxAt( e->x, e->y, e->z );   /* voxSet doesn't touch the smooth mask */
        if( v ) v->smoothFaces = e->before.smoothFaces;
    }
    else               voxErase( e->x, e->y, e->z );
    g_activeLayer = save;
}
static void applyRedo( Edit *e )
{
    int save = g_activeLayer;
    if( e->layer >= 0 && e->layer < g_numLayers ) g_activeLayer = e->layer;
    if( e->hadAfter ) {
        Voxel *v;
        voxSet( e->x, e->y, e->z, e->after.color,
                e->after.rampStart, e->after.rampLen );
        v = voxAt( e->x, e->y, e->z );
        if( v ) v->smoothFaces = e->after.smoothFaces;
    }
    else              voxErase( e->x, e->y, e->z );
    g_activeLayer = save;
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

/* Unit world-space direction from any surface point TOWARD the viewer, for the
 * oblique projection -- the eye vector a specular highlight needs.
 *
 * Derived from the projection itself rather than from the 3D orbit camera, on
 * purpose: the highlight belongs to the baked pixel art, so it must not swim
 * around as you orbit, and shading it this way keeps the "match render" preview
 * agreeing with the render face-for-face.
 *
 * In the view frame a point's image position is (u*voxPx, C - v*frontH +
 * w*topH), so a displacement leaves the image unchanged iff du = 0 and
 * -dv*frontH + dw*topH = 0.  (0, topH, frontH) satisfies that and has dw > 0
 * (toward the viewer), so it IS the projector direction.  With no scrunch it
 * reduces to (0,1,1) -- the classic 45 degree top-front oblique.  Being a
 * parallel projection, it is constant over the whole image. */
static void obliqueViewDir( double *vx, double *vy, double *vz )
{
    int frontH = g_voxPx / g_frontScrunch;
    int topH   = g_voxPx / g_topScrunch;
    double len;
    if( frontH < 1 ) frontH = 1;
    if( topH   < 1 ) topH   = 1;
    fromUVW( 0.0, (double)topH, (double)frontH, vx, vy, vz );
    len = sqrt( (*vx)*(*vx) + (*vy)*(*vy) + (*vz)*(*vz) );
    if( len > 1e-9 ) { *vx /= len; *vy /= len; *vz /= len; }
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

/* Load the 3D-view projection into the current matrix (assumes GL_PROJECTION is
 * active and identity-loaded).  Orthographic mode builds an ortho box sized so
 * the on-screen scale at the camera target matches the 42-degree perspective
 * view, so toggling ortho doesn't jump the zoom. */
static void applyProjection( int w, int h )
{
    double aspect = h ? (double)w / (double)h : 1.0;
    if( g_ortho ) {
        double halfH = cam_dist * tan( 21.0 * M_PI / 180.0 );
        double halfW;
        if( halfH < 1e-3 ) halfH = 1e-3;
        halfW = halfH * aspect;
        glOrtho( -halfW, halfW, -halfH, halfH, -2000.0, 2000.0 );
    } else {
        gluPerspective( 42.0, aspect, 0.1, 2000.0 );
    }
}

static void setupCamera( int w, int h )
{
    double ex, ey, ez;
    glMatrixMode( GL_PROJECTION );
    glLoadIdentity();
    applyProjection( w, h );
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

/* Per-voxel move offset under symmetry: a selected voxel on the + side of an
 * enabled mirror plane moves with +offset along that axis, one on the - side
 * moves the opposite way, and one sitting exactly on a mid-voxel plane doesn't
 * move along that axis at all -- so a symmetric selection stays symmetric as it
 * is dragged.  With symmetry off it is just the plain (g_moveDX,DY,DZ). */
static void selMoveOffset( int x, int y, int z, int *odx, int *ody, int *odz )
{
    int coord[3], mv[3], off[3], a;
    coord[0] = x; coord[1] = y; coord[2] = z;
    mv[0] = g_moveDX; mv[1] = g_moveDY; mv[2] = g_moveDZ;
    for( a = 0; a < 3; a++ ) {
        int s = 1;
        if( g_symOn[a] ) {
            int c = coord[a];
            if( g_symHalf[a] )
                s = ( c > g_symPos[a] ) ? 1 : ( c < g_symPos[a] ? -1 : 0 );
            else
                s = ( c >= g_symPos[a] ) ? 1 : -1;
        }
        off[a] = mv[a] * s;
    }
    *odx = off[0]; *ody = off[1]; *odz = off[2];
}

/* forward decls for preview overlays defined later */
static int  symActive( void );
static void drawGesturePreview( void );
static void cbGhostCube( float fx, float fy, float fz );
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

/* Composite (all visible layers) integer cell bounds; lo/hi are inclusive cell
 * coords.  Falls back to a small box around the origin when the scene is empty. */
static void compositeBounds( int lo[3], int hi[3] )
{
    int L, i, a, any = 0;
    for( a = 0; a < 3; a++ ) { lo[a] = 0; hi[a] = 0; }
    for( L = 0; L < g_numLayers; L++ ) {
        if( !g_layers[L].visible ) continue;
        for( i = 0; i < g_layers[L].cap; i++ ) {
            Voxel *s = &g_layers[L].vox[i];
            if( s->used != 1 ) continue;
            if( !any ) {
                lo[0]=hi[0]=s->x; lo[1]=hi[1]=s->y; lo[2]=hi[2]=s->z; any=1;
            } else {
                if( s->x < lo[0] ) lo[0]=s->x;
                if( s->x > hi[0] ) hi[0]=s->x;
                if( s->y < lo[1] ) lo[1]=s->y;
                if( s->y > hi[1] ) hi[1]=s->y;
                if( s->z < lo[2] ) lo[2]=s->z;
                if( s->z > hi[2] ) hi[2]=s->z;
            }
        }
    }
    if( !any ) { for( a=0;a<3;a++ ){ lo[a]=-4; hi[a]=4; } }
}

/* emit one vertex on the plane perpendicular to axis a (fixed at c), with the
 * two in-plane axes u,vax set to uv,vv. */
static void planeVert( int a, float c, int u, float uv, int vax, float vv )
{
    float p[3];
    p[a] = c; p[u] = uv; p[vax] = vv;
    glVertex3f( p[0], p[1], p[2] );
}

/* Draw each enabled symmetry plane as a translucent striped sheet, extending 5
 * voxels beyond the model's extents so it reads as an infinite mirror without
 * near-plane clipping.  X plane = red, Y = green, Z = blue. */
static void drawSymmetryPlanes( void )
{
    int lo[3], hi[3], a;
    if( !g_symShow || !symActive() ) return;
    compositeBounds( lo, hi );
    glEnable( GL_BLEND );
    glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
    glDepthMask( GL_FALSE );
    for( a = 0; a < 3; a++ ) {
        float c, col[3], t, ulo, uhi, vlo, vhi;
        int u = (a+1)%3, vax = (a+2)%3;
        if( !g_symOn[a] ) continue;
        c = (float)g_symPos[a] + ( g_symHalf[a] ? 0.5f : 0.0f );
        ulo = (float)( lo[u] - 5 );  uhi = (float)( hi[u] + 6 );
        vlo = (float)( lo[vax] - 5 ); vhi = (float)( hi[vax] + 6 );
        col[0]=col[1]=col[2]=0.25f; col[a]=1.0f;
        glColor4f( col[0], col[1], col[2], 0.13f );
        glBegin( GL_QUADS );
        planeVert( a, c, u, ulo, vax, vlo );
        planeVert( a, c, u, uhi, vax, vlo );
        planeVert( a, c, u, uhi, vax, vhi );
        planeVert( a, c, u, ulo, vax, vhi );
        glEnd();
        /* stripes: parallel lines every 2 units along the u axis */
        glColor4f( col[0], col[1], col[2], 0.55f );
        glLineWidth( 1.0f );
        glBegin( GL_LINES );
        for( t = (float)((int)ulo); t <= uhi; t += 2.0f ) {
            planeVert( a, c, u, t, vax, vlo );
            planeVert( a, c, u, t, vax, vhi );
        }
        glEnd();
    }
    glDepthMask( GL_TRUE );
    glDisable( GL_BLEND );
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

    drawSymmetryPlanes();

    drawGesturePreview();

    /* pending selection move: a green translucent ghost of the selection at the
     * current offset, previewing where "Commit Move" will drop it. */
    if( g_moveDX || g_moveDY || g_moveDZ ) {
        glEnable( GL_BLEND );
        glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
        glDepthMask( GL_FALSE );
        glColor4f( 0.3f, 1.0f, 0.5f, 0.40f );
        glBegin( GL_QUADS );
        for( i = 0; i < g_voxCap; i++ )
            if( g_vox[i].used == 1 && g_vox[i].sel ) {
                int ox, oy, oz;
                selMoveOffset( g_vox[i].x, g_vox[i].y, g_vox[i].z, &ox, &oy, &oz );
                cbGhostCube( (float)( g_vox[i].x + ox ),
                             (float)( g_vox[i].y + oy ),
                             (float)( g_vox[i].z + oz ) );
            }
        glEnd();
        glDepthMask( GL_TRUE );
        glDisable( GL_BLEND );
    }

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
        /* Depth test stays ON: the tiles/spikes are lifted just above their
         * faces, so real occlusion keeps far patches from punching through
         * near ones (previously disabled, which jumbled dense scenes). */
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
    applyProjection( g_viewW, g_viewH );
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
    applyProjection( g_viewW, g_viewH );
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
    int save = g_activeLayer;
    int result = 0;
    /* pick against the composite (all visible layers), not just the active one,
     * so you can draw onto surfaces belonging to other layers. */
    ensureFlat();
    g_activeLayer = FLAT_LAYER;

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
            result = 1;
            break;
        }
        if( tMaxX < tMaxY && tMaxX < tMaxZ ) {
            x += sx; tMaxX += tDX; lastAxis = 0;
        } else if( tMaxY < tMaxZ ) {
            y += sy; tMaxY += tDY; lastAxis = 1;
        } else {
            z += sz; tMaxZ += tDZ; lastAxis = 2;
        }
    }
    g_activeLayer = save;
    return result;
}

/* ---- symmetry helpers ---- */
static int symActive( void ) { return g_symOn[0] || g_symOn[1] || g_symOn[2]; }

/* Reflect coordinate v across symmetry axis a's plane. */
static int symReflect( int a, int v )
{
    return g_symHalf[a] ? ( 2 * g_symPos[a] - v )
                        : ( 2 * g_symPos[a] - v - 1 );
}

/* Fill out[][3] with the up-to-8 unique mirror images of cell (x,y,z) under the
 * enabled planes (out[0] is always the original).  Returns the count (>=1). */
static int symImages( int x, int y, int z, int out[8][3] )
{
    int vals[3][2], cnt[3], base[3], i0, i1, i2, n = 0, a;
    base[0] = x; base[1] = y; base[2] = z;
    for( a = 0; a < 3; a++ ) {
        vals[a][0] = base[a]; cnt[a] = 1;
        if( g_symOn[a] ) {
            int r = symReflect( a, base[a] );
            if( r != base[a] ) { vals[a][1] = r; cnt[a] = 2; }
        }
    }
    for( i0 = 0; i0 < cnt[0]; i0++ )
      for( i1 = 0; i1 < cnt[1]; i1++ )
        for( i2 = 0; i2 < cnt[2]; i2++ ) {
            out[n][0] = vals[0][i0];
            out[n][1] = vals[1][i1];
            out[n][2] = vals[2][i2];
            n++;
        }
    return n;
}

/* Like symImages, but each image also carries the face normal (nx,ny,nz) with
 * the reflected axis' component negated.  out[][6] holds {x,y,z, nx,ny,nz} per
 * image.  Used by the Smoother so a smoothed face mirrors to the correct
 * opposite face.  Reflection is enumerated independently of symImages so a
 * mid-voxel (+0.5) center column -- whose cell maps to itself but whose +A and
 * -A faces mirror to each other -- still yields both face images. */
static int symFaceImages( int x, int y, int z, int nx, int ny, int nz,
                          int out[8][6] )
{
    int coord[3], nrm[3], opt[3], i0, i1, i2, n = 0, k;
    coord[0] = x; coord[1] = y; coord[2] = z;
    nrm[0] = nx; nrm[1] = ny; nrm[2] = nz;
    for( k = 0; k < 3; k++ ) opt[k] = g_symOn[k] ? 2 : 1;
    for( i0 = 0; i0 < opt[0]; i0++ )
      for( i1 = 0; i1 < opt[1]; i1++ )
        for( i2 = 0; i2 < opt[2]; i2++ ) {
            int im[3], nn[3], sel[3], a, dup, j;
            sel[0]=i0; sel[1]=i1; sel[2]=i2;
            for( a = 0; a < 3; a++ ) {
                if( sel[a] ) { im[a] = symReflect( a, coord[a] ); nn[a] = -nrm[a]; }
                else         { im[a] = coord[a];                  nn[a] =  nrm[a]; }
            }
            /* skip a duplicate (cell,normal) already emitted */
            dup = 0;
            for( j = 0; j < n; j++ )
                if( out[j][0]==im[0] && out[j][1]==im[1] && out[j][2]==im[2] &&
                    out[j][3]==nn[0] && out[j][4]==nn[1] && out[j][5]==nn[2] )
                    { dup = 1; break; }
            if( dup ) continue;
            out[n][0]=im[0]; out[n][1]=im[1]; out[n][2]=im[2];
            out[n][3]=nn[0]; out[n][4]=nn[1]; out[n][5]=nn[2];
            n++;
        }
    return n;
}

/* Place or erase one cell (no symmetry) with the current paint color/ramp. */
static void putCell1( int x, int y, int z )
{
    if( g_mode == 1 ) editVoxel( x, y, z, 0, 0, 0, 0 );
    else              editVoxel( x, y, z, 1, g_pick, g_rampStart,
                                 g_rampEnd - g_rampStart + 1 );
}

/* Place or erase a cell and all of its symmetry images.  When drawing, if any
 * mirror image sits on an already-selected voxel the whole newly-placed group
 * inherits that selection -- so a voxel drawn where its mirror was selected
 * (e.g. selection made before its counterpart existed) is auto-selected while
 * symmetry stays on. */
static void putCell( int x, int y, int z )
{
    int out[8][3], n, i, anySel = 0;
    if( !symActive() ) { putCell1( x, y, z ); return; }
    n = symImages( x, y, z, out );
    if( g_mode == 0 ) {
        for( i = 0; i < n; i++ ) {
            Voxel *v = voxAt( out[i][0], out[i][1], out[i][2] );
            if( v && v->sel ) { anySel = 1; break; }
        }
    }
    for( i = 0; i < n; i++ ) putCell1( out[i][0], out[i][1], out[i][2] );
    if( anySel )
        for( i = 0; i < n; i++ ) {
            Voxel *v = voxAt( out[i][0], out[i][1], out[i][2] );
            if( v ) v->sel = 1;
        }
}

/* Select (or, in erase mode, deselect) a cell and all its symmetry images. */
static void selectCell( int x, int y, int z, int *counter )
{
    int out[8][3], n, i;
    n = symActive() ? symImages( x, y, z, out )
                    : ( out[0][0]=x, out[0][1]=y, out[0][2]=z, 1 );
    for( i = 0; i < n; i++ ) {
        Voxel *v = voxAt( out[i][0], out[i][1], out[i][2] );
        if( v ) { v->sel = ( g_mode == 1 ) ? 0 : 1; if( counter ) (*counter)++; }
    }
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

    if( tool == 11 ) {
        /* ellipsoid: a full 3D ball in the gesture frame.  The drag rectangle
         * gives the two in-plane semi-axes; g_thickness gives the third (along
         * the plane normal); g_sphereDepth sinks it into the surface (dome at
         * depth 0, centred near thickness/2, crater when erasing). */
        double cen0 = ( lo0 + hi0 ) * 0.5 + 0.5, cen1 = ( lo1 + hi1 ) * 0.5 + 0.5;
        double ra = ( hi0 - lo0 ) * 0.5 + 0.5, rb = ( hi1 - lo1 ) * 0.5 + 0.5;
        double rc = g_thickness * 0.5;
        double surfA, cenA;
        int cellA, camin, camax, p0e, p1e;
        if( rc < 0.5 ) rc = 0.5;
        surfA = a[A] + ( g_gDir > 0 ? 0.0 : 1.0 );
        cenA  = surfA + g_gDir * ( rc - g_sphereDepth );
        camin = (int)floor( cenA - rc - 1.0 );
        camax = (int)ceil ( cenA + rc + 1.0 );
        for( cellA = camin; cellA <= camax; cellA++ ) {
            double dA = ( cellA + 0.5 - cenA ) / rc;
            if( dA*dA > 1.0 ) continue;
            c[A] = cellA;
            for( p0e = lo0; p0e <= hi0; p0e++ )
              for( p1e = lo1; p1e <= hi1; p1e++ ) {
                double d0 = ( p0e + 0.5 - cen0 ) / ra;
                double d1 = ( p1e + 0.5 - cen1 ) / rb;
                if( d0*d0 + d1*d1 + dA*dA > 1.0 ) continue;
                c[i0] = p0e; c[i1] = p1e;
                cb( c[0], c[1], c[2], ud );
                if( ++count > limit ) return;
              }
        }
        return;
    }

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
    (void)ud;
    selectCell( x, y, z, &g_regCount );
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

/* ---- pencil paint-drag state ----
 * The pencil is a scribble tool like the smoother/scribble-select: dragging with
 * the left button paints a set of target cells (one per real surface the cursor
 * sweeps over) as a translucent ghost, and the whole set is committed as ONE
 * undo step when the button is released.  A plain click paints a single cell, so
 * the click and drag paths are identical -- the click just paints one cell.
 * Target cells are always computed against the *real* voxels (pickCell picks the
 * empty neighbour of the surface hit, or the ground cell on a miss), never
 * against the pending ghost, so a drag lays down one shell of clay without ever
 * piling ghost-on-ghost.  Left-drag therefore paints instead of orbiting -- the
 * right button remains the (redundant) orbit control. */
#define PENCIL_MAX 16384
static int g_pencilActive = 0;
static int g_pencilN = 0;
static int g_pencilCells[ PENCIL_MAX ][3];

static void pencilAddCell( int x, int y, int z )
{
    int i;
    for( i = 0; i < g_pencilN; i++ )     /* dedup: the same surface is swept a lot */
        if( g_pencilCells[i][0] == x && g_pencilCells[i][1] == y &&
            g_pencilCells[i][2] == z ) return;
    if( g_pencilN >= PENCIL_MAX ) return;
    g_pencilCells[g_pencilN][0] = x;
    g_pencilCells[g_pencilN][1] = y;
    g_pencilCells[g_pencilN][2] = z;
    g_pencilN++;
}

/* Paint the cell the cursor is over into the pending ghost set. */
static void pencilAt( int mx, int my )
{
    int cx, cy, cz, axis, dir, ground;
    if( !g_pencilActive ) return;
    if( !pickCell( mx, my, &cx, &cy, &cz, &axis, &dir, &ground ) ) return;
    pencilAddCell( cx, cy, cz );
}

static void pencilBegin( int mx, int my )
{
    g_pencilActive = 1;
    g_pencilN = 0;
    pencilAt( mx, my );
}

/* Commit the pending ghost set as real voxels (or erasures in erase mode) in a
 * single undo group.  putCell reads g_mode, so draw vs erase follows the current
 * mode, and applies any active symmetry planes. */
static void pencilCommit( void )
{
    int i, n;
    if( !g_pencilActive ) return;
    g_pencilActive = 0;
    n = g_pencilN;
    g_pencilN = 0;
    if( n == 0 ) return;
    groupBegin();
    for( i = 0; i < n; i++ )
        putCell( g_pencilCells[i][0], g_pencilCells[i][1], g_pencilCells[i][2] );
    groupEnd();
    { char m[64];
      sprintf( m, g_mode == 1 ? "Erased %d voxel%s" : "Painted %d voxel%s",
               n, n == 1 ? "" : "s" );
      setStatus( m ); }
}

/* Begin a region drag under the cursor. */
static void gestureBegin( int mx, int my )
{
    int cx, cy, cz, axis, dir, ground;
    g_gActive = 0; g_gHaveB = 0;
    if( !pickCell( mx, my, &cx, &cy, &cz, &axis, &dir, &ground ) ) return;
    /* Select tool: the marquee sits IN the clicked voxel's layer (not the empty
     * cell above it), with the below/above sliders sweeping into/out of the
     * surface.  This must be identical in Draw and Erase mode so dragging Erase
     * over the same spot deselects exactly what Draw selected -- so we normalise
     * to the solid hit cell + outward dir regardless of g_mode.  (pickCell gives
     * the empty neighbour + outward dir in draw mode, and the solid cell +
     * inward dir in erase mode.) */
    if( g_tool == 4 && !ground ) {
        if( g_mode != 1 ) {
            /* draw mode: step back from the empty neighbour into the solid */
            if( axis == 0 ) cx -= dir; else if( axis == 1 ) cy -= dir;
            else cz -= dir;
        } else {
            dir = -dir;              /* erase mode: flip inward dir to outward */
        }
    }
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
        selectCell( hx, hy, hz, NULL );
    }
}

/* Eyedropper: sample the color / ramp of the voxel under the cursor into the
 * current paint color, so a subsequent stroke reuses it. */
static void eyedropAt( int mx, int my )
{
    double ox, oy, oz, dx, dy, dz;
    int hx, hy, hz, px, py, pz, ax;
    Voxel *v;
    char m[80];
    mouseRay( mx, my, &ox, &oy, &oz, &dx, &dy, &dz );
    if( !rayVoxel( ox, oy, oz, dx, dy, dz, &hx, &hy, &hz, &px, &py, &pz, &ax ) ) {
        setStatus( "Eyedropper: no voxel under cursor" );
        return;
    }
    v = voxAt( hx, hy, hz );
    if( !v ) return;
    g_pick      = v->color;
    g_rampStart = v->rampStart;
    g_rampEnd   = v->rampStart + v->rampLen - 1;
    if( g_rampEnd < g_rampStart ) g_rampEnd = g_rampStart;
    if( v->rampLen > 1 )
        sprintf( m, "Picked ramp %d..%d", g_rampStart, g_rampEnd );
    else
        sprintf( m, "Picked color %d", g_pick );
    setStatus( m );
}

/* Smoother tool: mark (draw) or clear (erase) the single visible face the cursor
 * is over as smooth.  Called on the initial click and every move of the drag, so
 * dragging paints face-smoothness across whatever the cursor sweeps.  Each real
 * change is one undo record; the whole drag shares an open group. */
static int g_smoothing = 0;

/* Mark (draw) or clear (erase) one face of the voxel at (cx,cy,cz) whose
 * outward normal is (nx,ny,nz).  Records one undo Edit if the mask changes. */
static void smoothOneFace( int cx, int cy, int cz, int nx, int ny, int nz )
{
    int bit, newMask;
    Voxel *v = voxAt( cx, cy, cz );
    Edit e;
    if( !v ) return;
    bit = 1 << faceDir6( nx, ny, nz );
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
    /* smoothFaces was mutated directly (not via voxSet), so the composite scratch
     * layer won't pick it up unless we flag it dirty -- otherwise the pixel render,
     * which shades the composite, keeps the stale mask until an unrelated edit
     * happens to rebuild it. */
    if( g_activeLayer != FLAT_LAYER ) g_flatDirty = 1;
    g_renderDirty = 1;
}

static void smoothFaceAt( int mx, int my )
{
    double ox, oy, oz, dx, dy, dz;
    int hx, hy, hz, px, py, pz, ax, nx, ny, nz;
    mouseRay( mx, my, &ox, &oy, &oz, &dx, &dy, &dz );
    if( !rayVoxel( ox, oy, oz, dx, dy, dz, &hx, &hy, &hz, &px, &py, &pz, &ax ) )
        return;
    if( !voxAt( hx, hy, hz ) ) return;
    nx = px - hx; ny = py - hy; nz = pz - hz;
    if( symActive() ) {
        int out[8][6], n, i;
        n = symFaceImages( hx, hy, hz, nx, ny, nz, out );
        for( i = 0; i < n; i++ )
            smoothOneFace( out[i][0], out[i][1], out[i][2],
                           out[i][3], out[i][4], out[i][5] );
    } else {
        smoothOneFace( hx, hy, hz, nx, ny, nz );
    }
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

/* Enforce the symmetry invariant on the live selection: whatever is selected on
 * one side of an enabled mirror plane is also selected on the other.  Called
 * whenever a symmetry plane is toggled, moved, or its +0.5 flips, so the
 * selection mirrors instantly ("moving a mirror over a drawing").  Add-only: a
 * selected voxel selects its mirror images where voxels exist; mirror positions
 * sitting in empty air are simply skipped.  One pass suffices since the mirror
 * group is closed under composition. */
static void symmetryChanged( void )
{
    int i, n = 0, cap;
    int (*sc)[3];
    if( !symActive() ) return;
    cap = selCount();
    if( cap == 0 ) return;
    sc = (int (*)[3])malloc( (size_t)cap * sizeof *sc );
    for( i = 0; i < g_voxCap; i++ )
        if( g_vox[i].used == 1 && g_vox[i].sel ) {
            sc[n][0] = g_vox[i].x; sc[n][1] = g_vox[i].y; sc[n][2] = g_vox[i].z;
            n++;
        }
    for( i = 0; i < n; i++ ) {
        int out[8][3], m, k;
        m = symImages( sc[i][0], sc[i][1], sc[i][2], out );
        for( k = 0; k < m; k++ ) {
            Voxel *v = voxAt( out[k][0], out[k][1], out[k][2] );
            if( v ) v->sel = 1;
        }
    }
    free( sc );
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
    if( g_activeLayer != FLAT_LAYER ) g_flatDirty = 1;   /* see smoothOneFace */
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

/* Write full voxel contents (color/ramp/smoothFaces/sel) at (x,y,z), or erase
 * it when nd==NULL, recording one undo Edit.  Unlike editVoxel this preserves
 * the smooth-face mask, which a move must carry along. */
static void recordVoxelWrite( int x, int y, int z, const Voxel *nd )
{
    Edit e;
    Voxel *ex = voxAt( x, y, z );
    memset( &e, 0, sizeof e );
    e.x = x; e.y = y; e.z = z;
    e.hadBefore = ex ? 1 : 0;
    if( ex ) e.before = *ex;
    if( nd ) {
        Voxel *v;
        voxSet( x, y, z, nd->color, nd->rampStart, nd->rampLen );
        v = voxAt( x, y, z );
        if( v ) { v->smoothFaces = nd->smoothFaces; v->sel = nd->sel; }
        e.hadAfter = 1; e.after = *v;
    } else {
        if( !ex ) return;
        voxErase( x, y, z );
        e.hadAfter = 0;
    }
    e.group = g_curGroup;
    histPush( e );
    g_renderDirty = 1;
}

/* Commit the pending selection move: translate every selected voxel by
 * (g_moveDX,DY,DZ), overwriting whatever occupies the destinations, as one undo
 * group.  Colliding voxels not in the selection are overwritten (recoverable by
 * undo).  The moved copy becomes the new selection and the offset resets. */
static void selMoveCommit( void )
{
    int i, n = 0, cap = selCount();
    Voxel *src;
    if( cap == 0 ) { setStatus( "Selection empty" ); return; }
    if( g_moveDX == 0 && g_moveDY == 0 && g_moveDZ == 0 )
        { setStatus( "Move offset is 0" ); return; }
    src = (Voxel*)malloc( (size_t)cap * sizeof( Voxel ) );
    for( i = 0; i < g_voxCap; i++ )
        if( g_vox[i].used == 1 && g_vox[i].sel ) src[n++] = g_vox[i];
    groupBegin();
    /* erase all sources first so a move onto another source cell is clean */
    for( i = 0; i < n; i++ )
        recordVoxelWrite( src[i].x, src[i].y, src[i].z, NULL );
    /* place the moved copies (selected) at their new homes -- each voxel's
     * offset respects symmetry so a mirrored selection stays mirrored */
    for( i = 0; i < n; i++ ) {
        Voxel nd = src[i];
        int ox, oy, oz;
        selMoveOffset( src[i].x, src[i].y, src[i].z, &ox, &oy, &oz );
        nd.sel = 1;
        recordVoxelWrite( src[i].x+ox, src[i].y+oy, src[i].z+oz, &nd );
    }
    groupEnd();
    free( src );
    g_moveDX = g_moveDY = g_moveDZ = 0;
    { char m[64]; sprintf( m, "Moved %d voxels", n ); setStatus( m ); }
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
static void cbGhostCube( float fx, float fy, float fz )
{
    /* +Y */ glVertex3f(fx,fy+1,fz);   glVertex3f(fx,fy+1,fz+1); glVertex3f(fx+1,fy+1,fz+1); glVertex3f(fx+1,fy+1,fz);
    /* -Y */ glVertex3f(fx,fy,fz);     glVertex3f(fx+1,fy,fz);   glVertex3f(fx+1,fy,fz+1);   glVertex3f(fx,fy,fz+1);
    /* +Z */ glVertex3f(fx,fy,fz+1);   glVertex3f(fx+1,fy,fz+1); glVertex3f(fx+1,fy+1,fz+1); glVertex3f(fx,fy+1,fz+1);
    /* -Z */ glVertex3f(fx,fy,fz);     glVertex3f(fx,fy+1,fz);   glVertex3f(fx+1,fy+1,fz);   glVertex3f(fx+1,fy,fz);
    /* +X */ glVertex3f(fx+1,fy,fz);   glVertex3f(fx+1,fy,fz+1); glVertex3f(fx+1,fy+1,fz+1); glVertex3f(fx+1,fy+1,fz);
    /* -X */ glVertex3f(fx,fy,fz);     glVertex3f(fx,fy+1,fz);   glVertex3f(fx,fy+1,fz+1);   glVertex3f(fx,fy,fz+1);
}
static void cbGhost( int x, int y, int z, void *ud )
{
    int out[8][3], n, i;
    (void)ud;
    n = symActive() ? symImages( x, y, z, out )
                    : ( out[0][0]=x, out[0][1]=y, out[0][2]=z, 1 );
    for( i = 0; i < n; i++ )
        cbGhostCube( (float)out[i][0], (float)out[i][1], (float)out[i][2] );
}

/* Wire cube + an X across all six faces of the voxel at (x,y,z), drawn in the
 * current GL color.  Used during a delete/select drag to flag which existing
 * voxels a region actually intersects (they'd otherwise hide inside the
 * translucent ghost). */
static void drawHitMarkCube( int x, int y, int z )
{
    float fx = (float)x, fy = (float)y, fz = (float)z;
    glBegin( GL_LINES );
    /* 12 cube edges */
    glVertex3f(fx,fy,fz);     glVertex3f(fx+1,fy,fz);
    glVertex3f(fx,fy,fz+1);   glVertex3f(fx+1,fy,fz+1);
    glVertex3f(fx,fy+1,fz);   glVertex3f(fx+1,fy+1,fz);
    glVertex3f(fx,fy+1,fz+1); glVertex3f(fx+1,fy+1,fz+1);
    glVertex3f(fx,fy,fz);     glVertex3f(fx,fy+1,fz);
    glVertex3f(fx+1,fy,fz);   glVertex3f(fx+1,fy+1,fz);
    glVertex3f(fx,fy,fz+1);   glVertex3f(fx,fy+1,fz+1);
    glVertex3f(fx+1,fy,fz+1); glVertex3f(fx+1,fy+1,fz+1);
    glVertex3f(fx,fy,fz);     glVertex3f(fx,fy,fz+1);
    glVertex3f(fx+1,fy,fz);   glVertex3f(fx+1,fy,fz+1);
    glVertex3f(fx,fy+1,fz);   glVertex3f(fx,fy+1,fz+1);
    glVertex3f(fx+1,fy+1,fz); glVertex3f(fx+1,fy+1,fz+1);
    /* X on each of the six faces (both diagonals) */
    glVertex3f(fx,fy,fz);       glVertex3f(fx+1,fy+1,fz);      /* -Z */
    glVertex3f(fx+1,fy,fz);     glVertex3f(fx,fy+1,fz);
    glVertex3f(fx,fy,fz+1);     glVertex3f(fx+1,fy+1,fz+1);    /* +Z */
    glVertex3f(fx+1,fy,fz+1);   glVertex3f(fx,fy+1,fz+1);
    glVertex3f(fx,fy,fz);       glVertex3f(fx,fy+1,fz+1);      /* -X */
    glVertex3f(fx,fy+1,fz);     glVertex3f(fx,fy,fz+1);
    glVertex3f(fx+1,fy,fz);     glVertex3f(fx+1,fy+1,fz+1);    /* +X */
    glVertex3f(fx+1,fy+1,fz);   glVertex3f(fx+1,fy,fz+1);
    glVertex3f(fx,fy,fz);       glVertex3f(fx+1,fy,fz+1);      /* -Y */
    glVertex3f(fx+1,fy,fz);     glVertex3f(fx,fy,fz+1);
    glVertex3f(fx,fy+1,fz);     glVertex3f(fx+1,fy+1,fz+1);    /* +Y */
    glVertex3f(fx+1,fy+1,fz);   glVertex3f(fx,fy+1,fz+1);
    glEnd();
}
/* Region/sphere callback: mark every existing voxel the cell (and its symmetry
 * images) lands on. */
static void cbHitMark( int x, int y, int z, void *ud )
{
    int out[8][3], n, i;
    (void)ud;
    n = symActive() ? symImages( x, y, z, out )
                    : ( out[0][0]=x, out[0][1]=y, out[0][2]=z, 1 );
    for( i = 0; i < n; i++ )
        if( voxAt( out[i][0], out[i][1], out[i][2] ) )
            drawHitMarkCube( out[i][0], out[i][1], out[i][2] );
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
        /* in erase mode, X-mark the existing voxels this ball will carve */
        if( g_mode == 1 ) {
            glLineWidth( 2.0f );
            glColor3f( 1.0f, 0.3f, 0.25f );
            sphereForEach( cbHitMark, NULL );
            glLineWidth( 1.0f );
        }
        glDisable( GL_BLEND );
        return;
    }
    /* pencil paint-drag previews its pending cells as translucent ghost cubes;
     * on erase it also X-marks the real voxels the stroke will remove */
    if( g_pencilActive && g_pencilN > 0 ) {
        int i;
        glEnable( GL_BLEND );
        glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
        glDepthMask( GL_FALSE );
        if( g_mode == 1 ) glColor4f( 1.0f, 0.25f, 0.2f, 0.40f );
        else              glColor4f( 0.3f, 1.0f, 0.45f, 0.40f );
        glBegin( GL_QUADS );
        for( i = 0; i < g_pencilN; i++ )
            cbGhost( g_pencilCells[i][0], g_pencilCells[i][1],
                     g_pencilCells[i][2], NULL );
        glEnd();
        glDepthMask( GL_TRUE );
        if( g_mode == 1 ) {
            glLineWidth( 2.0f );
            glColor3f( 1.0f, 0.3f, 0.25f );
            for( i = 0; i < g_pencilN; i++ )
                cbHitMark( g_pencilCells[i][0], g_pencilCells[i][1],
                           g_pencilCells[i][2], NULL );
            glLineWidth( 1.0f );
        }
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
    /* flag existing voxels the region intersects with an outline + face X:
     * red for a delete drag, yellow for a select marquee. */
    if( g_tool == 4 || g_mode == 1 ) {
        glLineWidth( 2.0f );
        if( g_tool == 4 ) glColor3f( 1.0f, 0.9f, 0.15f );
        else              glColor3f( 1.0f, 0.3f, 0.25f );
        regionForEach( tool, cbHitMark, NULL );
        glLineWidth( 1.0f );
    }
    glDisable( GL_BLEND );
}

/* ------------------------------------------------------------------ */
/* MagicaVoxel .vox import                                             */
/*                                                                     */
/* The format is a RIFF-ish tree: a "VOX " magic + version, then a     */
/* MAIN chunk whose children are the payload.  Each chunk is a 4-byte  */
/* id, an int32 content size, an int32 children size, the content, and */
/* then the children's chunks.  All ints are little-endian.            */
/*                                                                     */
/* Geometry is the point, so we read SIZE/XYZI model pairs and the     */
/* nTRN/nGRP/nSHP scene graph that places them (a file with several    */
/* models relies on that graph, so ignoring it would pile everything   */
/* at the origin).  We also map each voxel through the file's RGBA     */
/* palette to the nearest sample in ours -- flat one-colour voxels,    */
/* like the PNG wall import.  That is more than the notes asked for,   */
/* but a model imported in the default paint colour (index 15 = pure   */
/* black) arrives as an unreadable black blob; Select All + Recolor    */
/* puts the whole thing on one ramp when that is what you want.  A     */
/* file with no RGBA chunk falls back to the current paint colour.     */
/*                                                                     */
/* Coordinates: MagicaVoxel is z-up with +y receding from the viewer   */
/* in its front view; we are y-up with -z facing the viewer in ours.   */
/* So (x,y,z)_vox -> (-x, z, y)_world, a proper rotation (det +1), and */
/* the model arrives unmirrored facing our Front view.                 */
/* ------------------------------------------------------------------ */

#define VOX_MAX_MODELS 1024
#define VOX_MAX_NODES  4096
#define VOX_MAX_DEPTH  32

typedef struct {
    int sx, sy, sz;
    int n;                       /* voxel count */
    const unsigned char *data;   /* n * 4 bytes: x, y, z, colorIndex */
} VoxModel;

typedef struct {
    int id;
    int type;      /* 0 = nTRN, 1 = nGRP, 2 = nSHP */
    int child;     /* nTRN: the single child node id */
    int rot;       /* nTRN: frame-0 rotation byte */
    int t[3];      /* nTRN: frame-0 translation */
    int nkid;      /* nGRP: child count / nSHP: model count */
    int *kid;      /* nGRP: child node ids / nSHP: model ids */
} VoxNode;

typedef struct {
    VoxModel *model;  int nmodel;
    VoxNode  *node;   int nnode;
    int pendSX, pendSY, pendSZ, havePend;
    unsigned char pal[ 256*4 ];   /* the file's RGBA chunk, if it has one */
    int havePal;
    int bad;
} VoxParse;

static int nearestPaletteIndex( int r, int g, int b );

/* byte cursor over one chunk's content */
typedef struct {
    const unsigned char *p, *end;
    int bad;
} VoxRd;

static int voxRd32( VoxRd *r )
{
    unsigned int v;
    if( r->bad || r->p + 4 > r->end ) { r->bad = 1; return 0; }
    v = (unsigned int)r->p[0] | ( (unsigned int)r->p[1] << 8 ) |
        ( (unsigned int)r->p[2] << 16 ) | ( (unsigned int)r->p[3] << 24 );
    r->p += 4;
    return (int)v;
}

/* Read a length-prefixed string, copying at most cap-1 bytes and skipping
 * the rest so the cursor always lands past the whole field. */
static void voxRdStr( VoxRd *r, char *out, int cap )
{
    int n = voxRd32( r ), keep;
    out[0] = 0;
    if( r->bad || n < 0 || r->p + n > r->end ) { r->bad = 1; return; }
    keep = n < cap - 1 ? n : cap - 1;
    memcpy( out, r->p, (size_t)keep );
    out[ keep ] = 0;
    r->p += n;
}

/* Consume a DICT, pulling out the "_r" rotation and "_t" translation if the
 * caller wants them (pass NULL to just skip the dict). */
static void voxRdDict( VoxRd *r, int *rot, int *t )
{
    int n = voxRd32( r ), i;
    if( r->bad || n < 0 ) { r->bad = 1; return; }
    for( i = 0; i < n && !r->bad; i++ ) {
        char key[ 64 ], val[ 128 ];
        voxRdStr( r, key, sizeof key );
        voxRdStr( r, val, sizeof val );
        if( r->bad ) return;
        if( rot && strcmp( key, "_r" ) == 0 ) *rot = atoi( val );
        else if( t && strcmp( key, "_t" ) == 0 )
            sscanf( val, "%d %d %d", &t[0], &t[1], &t[2] );
    }
}

/* Decode a MagicaVoxel rotation byte into a 3x3 matrix: bits 0-1 give the
 * column of row 0's single non-zero, bits 2-3 row 1's (row 2 gets the one
 * left over), and bits 4/5/6 are those entries' signs. */
static void voxRotMat( int r, int m[3][3] )
{
    int i0 = r & 3, i1 = ( r >> 2 ) & 3, i2 = 3 - i0 - i1;
    int j, k;
    for( j = 0; j < 3; j++ ) for( k = 0; k < 3; k++ ) m[j][k] = 0;
    if( i0 > 2 || i1 > 2 || i0 == i1 ) {   /* malformed -> identity */
        m[0][0] = m[1][1] = m[2][2] = 1;
        return;
    }
    m[0][i0] = ( r & 0x10 ) ? -1 : 1;
    m[1][i1] = ( r & 0x20 ) ? -1 : 1;
    m[2][i2] = ( r & 0x40 ) ? -1 : 1;
}

/* collected world-space voxels: 4 ints each -- x, y, z, file colour index */
typedef struct { int *p; int n, cap; } VoxOut;

static void voxOutAdd( VoxOut *o, int x, int y, int z, int ci )
{
    if( o->n == o->cap ) {
        int nc = o->cap ? o->cap * 2 : 4096;
        int *np = (int*)realloc( o->p, (size_t)nc * 4 * sizeof(int) );
        if( !np ) return;                    /* out of memory: drop the rest */
        o->p = np; o->cap = nc;
    }
    o->p[ o->n*4+0 ] = x; o->p[ o->n*4+1 ] = y;
    o->p[ o->n*4+2 ] = z; o->p[ o->n*4+3 ] = ci;
    o->n++;
}

static VoxNode *voxFindNode( VoxParse *P, int id )
{
    int i;
    for( i = 0; i < P->nnode; i++ ) if( P->node[i].id == id ) return &P->node[i];
    return NULL;
}

static void voxParseNode( VoxParse *P, const char *id,
                          const unsigned char *body, int len )
{
    VoxRd r;
    VoxNode nd;
    int i;
    if( P->nnode >= VOX_MAX_NODES ) return;
    r.p = body; r.end = body + len; r.bad = 0;
    memset( &nd, 0, sizeof nd );
    nd.child = -1;
    nd.rot = 4;    /* the identity encoding: row0->col0, row1->col1, all + */
    nd.id = voxRd32( &r );
    voxRdDict( &r, NULL, NULL );          /* node attributes: ignored */
    if( strcmp( id, "nTRN" ) == 0 ) {
        int nframes;
        nd.type = 0;
        nd.child = voxRd32( &r );
        (void)voxRd32( &r );              /* reserved id */
        (void)voxRd32( &r );              /* layer id */
        nframes = voxRd32( &r );
        if( r.bad || nframes < 0 ) return;
        for( i = 0; i < nframes && !r.bad; i++ ) {
            /* only frame 0 places the node; later frames are animation */
            if( i == 0 ) voxRdDict( &r, &nd.rot, nd.t );
            else         voxRdDict( &r, NULL, NULL );
        }
    } else {
        int n;
        nd.type = ( strcmp( id, "nGRP" ) == 0 ) ? 1 : 2;
        n = voxRd32( &r );
        if( r.bad || n < 0 || n > VOX_MAX_NODES ) return;
        nd.kid = n ? (int*)malloc( (size_t)n * sizeof(int) ) : NULL;
        if( n && !nd.kid ) return;
        for( i = 0; i < n && !r.bad; i++ ) {
            nd.kid[i] = voxRd32( &r );
            /* nSHP follows each model id with a per-model DICT */
            if( nd.type == 2 ) voxRdDict( &r, NULL, NULL );
        }
        nd.nkid = r.bad ? 0 : n;
    }
    if( r.bad ) { free( nd.kid ); return; }
    P->node[ P->nnode++ ] = nd;
}

/* Walk the chunk tree, collecting models and scene-graph nodes. */
static void voxScanChunks( VoxParse *P, const unsigned char *p,
                           const unsigned char *end, int depth )
{
    while( p + 12 <= end ) {
        char id[5];
        int nc, ncc;
        const unsigned char *body, *kids, *next;
        VoxRd r;
        memcpy( id, p, 4 ); id[4] = 0;
        r.p = p + 4; r.end = end; r.bad = 0;
        nc  = voxRd32( &r );
        ncc = voxRd32( &r );
        if( r.bad || nc < 0 || ncc < 0 ) return;
        body = p + 12;
        kids = body + nc;
        next = kids + ncc;
        if( kids > end || kids < body || next > end || next < kids ) return;

        if( strcmp( id, "SIZE" ) == 0 && nc >= 12 ) {
            r.p = body; r.end = kids; r.bad = 0;
            P->pendSX = voxRd32( &r );
            P->pendSY = voxRd32( &r );
            P->pendSZ = voxRd32( &r );
            P->havePend = !r.bad;
        } else if( strcmp( id, "XYZI" ) == 0 && nc >= 4 ) {
            r.p = body; r.end = kids; r.bad = 0;
            {
                int n = voxRd32( &r );
                if( !r.bad && n >= 0 && n <= ( nc - 4 ) / 4 &&
                    P->havePend && P->nmodel < VOX_MAX_MODELS ) {
                    VoxModel *m = &P->model[ P->nmodel++ ];
                    m->sx = P->pendSX; m->sy = P->pendSY; m->sz = P->pendSZ;
                    m->n = n; m->data = body + 4;
                }
                P->havePend = 0;
            }
        } else if( strcmp( id, "RGBA" ) == 0 && nc >= 256*4 ) {
            memcpy( P->pal, body, 256*4 );
            P->havePal = 1;
        } else if( strcmp( id, "nTRN" ) == 0 || strcmp( id, "nGRP" ) == 0 ||
                   strcmp( id, "nSHP" ) == 0 ) {
            voxParseNode( P, id, body, nc );
        }

        if( ncc > 0 && depth < VOX_MAX_DEPTH )
            voxScanChunks( P, kids, next, depth + 1 );
        p = next;
    }
}

/* Emit one model's voxels through the accumulated transform.  A model's local
 * origin is its centre, so a voxel's model-space point is (v - size/2). */
static void voxEmitModel( VoxOut *o, const VoxModel *m, int mat[3][3],
                          const int *t )
{
    int i;
    int cx = m->sx / 2, cy = m->sy / 2, cz = m->sz / 2;
    for( i = 0; i < m->n; i++ ) {
        int lx = (int)m->data[i*4+0] - cx;
        int ly = (int)m->data[i*4+1] - cy;
        int lz = (int)m->data[i*4+2] - cz;
        int wx = mat[0][0]*lx + mat[0][1]*ly + mat[0][2]*lz + t[0];
        int wy = mat[1][0]*lx + mat[1][1]*ly + mat[1][2]*lz + t[1];
        int wz = mat[2][0]*lx + mat[2][1]*ly + mat[2][2]*lz + t[2];
        /* vox z-up -> our y-up, unmirrored */
        voxOutAdd( o, -wx, wz, wy, (int)m->data[i*4+3] );
    }
}

/* Recursively place the subtree rooted at node `id`, given the transform
 * accumulated from its ancestors (world = mat * p + t). */
static void voxWalkNode( VoxParse *P, VoxOut *o, int id, int mat[3][3],
                         const int *t, int depth )
{
    VoxNode *nd = voxFindNode( P, id );
    int i, j, k;
    if( !nd || depth > VOX_MAX_DEPTH ) return;
    if( nd->type == 0 ) {                       /* nTRN */
        int nm[3][3], nt[3], cm[3][3];
        voxRotMat( nd->rot, nm );
        /* compose: world = mat * ( nm * p + nd->t ) + t */
        for( j = 0; j < 3; j++ )
            for( k = 0; k < 3; k++ )
                cm[j][k] = mat[j][0]*nm[0][k] + mat[j][1]*nm[1][k] +
                           mat[j][2]*nm[2][k];
        for( j = 0; j < 3; j++ )
            nt[j] = mat[j][0]*nd->t[0] + mat[j][1]*nd->t[1] +
                    mat[j][2]*nd->t[2] + t[j];
        voxWalkNode( P, o, nd->child, cm, nt, depth + 1 );
    } else if( nd->type == 1 ) {                /* nGRP */
        for( i = 0; i < nd->nkid; i++ )
            voxWalkNode( P, o, nd->kid[i], mat, t, depth + 1 );
    } else {                                    /* nSHP */
        for( i = 0; i < nd->nkid; i++ )
            if( nd->kid[i] >= 0 && nd->kid[i] < P->nmodel )
                voxEmitModel( o, &P->model[ nd->kid[i] ], mat, t );
    }
}

/* Read the whole file into memory (returns NULL on failure). */
static unsigned char *voxSlurp( const char *path, long *lenOut )
{
    FILE *f = fopen( path, "rb" );
    unsigned char *buf;
    long len;
    if( !f ) return NULL;
    if( fseek( f, 0, SEEK_END ) != 0 ) { fclose( f ); return NULL; }
    len = ftell( f );
    if( len < 8 || len > 64L*1024*1024 ) { fclose( f ); return NULL; }
    rewind( f );
    buf = (unsigned char*)malloc( (size_t)len );
    if( !buf ) { fclose( f ); return NULL; }
    if( fread( buf, 1, (size_t)len, f ) != (size_t)len ) {
        free( buf ); fclose( f ); return NULL;
    }
    fclose( f );
    *lenOut = len;
    return buf;
}

/* Import a MagicaVoxel .vox into the active layer as one undo step.  The
 * imported voxels land centred on the origin in x/z and resting on y=0, so a
 * model arrives where you can see it no matter where its scene graph put it. */
static int importVox( const char *path )
{
    unsigned char *buf;
    long len = 0;
    VoxParse P;
    VoxOut out;
    int i, ident[3][3], zero[3];
    int minX, maxX, minY, maxY, minZ, maxZ, ox, oy, oz, count = 0;
    int cmap[ 256 ];      /* file colour index -> our palette index */
    char m[128];

    buf = voxSlurp( path, &len );
    if( !buf ) { setStatus( "VOX: cannot read file" ); return 0; }
    if( memcmp( buf, "VOX ", 4 ) != 0 ) {
        free( buf ); setStatus( "VOX: not a MagicaVoxel file" ); return 0;
    }

    memset( &P, 0, sizeof P );
    memset( &out, 0, sizeof out );
    P.model = (VoxModel*)malloc( VOX_MAX_MODELS * sizeof(VoxModel) );
    P.node  = (VoxNode*) malloc( VOX_MAX_NODES  * sizeof(VoxNode) );
    if( !P.model || !P.node ) {
        free( P.model ); free( P.node ); free( buf );
        setStatus( "VOX: out of memory" ); return 0;
    }
    /* skip the 4-byte magic + int32 version, then walk MAIN and its kids */
    voxScanChunks( &P, buf + 8, buf + len, 0 );

    for( i = 0; i < 3; i++ ) {
        int j;
        for( j = 0; j < 3; j++ ) ident[i][j] = ( i == j );
        zero[i] = 0;
    }
    if( P.nnode > 0 && voxFindNode( &P, 0 ) )
        voxWalkNode( &P, &out, 0, ident, zero, 0 );   /* root is always id 0 */
    else
        for( i = 0; i < P.nmodel; i++ )               /* no scene graph */
            voxEmitModel( &out, &P.model[i], ident, zero );

    /* A voxel's colour index c refers to the file palette's entry c-1. */
    for( i = 0; i < 256; i++ ) {
        if( P.havePal ) {
            int e = ( i + 255 ) & 255;   /* c-1, wrapping index 0 harmlessly */
            cmap[i] = nearestPaletteIndex( P.pal[e*4+0], P.pal[e*4+1],
                                           P.pal[e*4+2] );
        } else cmap[i] = -1;             /* no palette: use the paint colour */
    }

    for( i = 0; i < P.nnode; i++ ) free( P.node[i].kid );
    free( P.model ); free( P.node ); free( buf );

    if( out.n < 1 ) {
        free( out.p );
        setStatus( "VOX: no voxels found" );
        return 0;
    }

    minX = maxX = out.p[0]; minY = maxY = out.p[1]; minZ = maxZ = out.p[2];
    for( i = 1; i < out.n; i++ ) {
        int x = out.p[i*4+0], y = out.p[i*4+1], z = out.p[i*4+2];
        if( x < minX ) minX = x;
        if( x > maxX ) maxX = x;
        if( y < minY ) minY = y;
        if( y > maxY ) maxY = y;
        if( z < minZ ) minZ = z;
        if( z > maxZ ) maxZ = z;
    }
    /* centre x/z on the origin and drop the model onto y=0.  The halves divide
     * a non-negative extent so the rounding doesn't flip with the sign of the
     * bounds the way -(min+max)/2 would. */
    ox = -minX - ( maxX - minX ) / 2;
    oy = -minY;
    oz = -minZ - ( maxZ - minZ ) / 2;

    groupBegin();
    for( i = 0; i < out.n; i++ ) {
        int ci = cmap[ out.p[i*4+3] & 255 ];
        if( ci >= 0 )                     /* flat voxel in the file's colour */
            editVoxel( out.p[i*4+0] + ox, out.p[i*4+1] + oy,
                       out.p[i*4+2] + oz, 1, ci, ci, 1 );
        else                              /* no file palette: current paint */
            editVoxel( out.p[i*4+0] + ox, out.p[i*4+1] + oy,
                       out.p[i*4+2] + oz, 1, g_pick, g_rampStart,
                       g_rampEnd - g_rampStart + 1 );
        count++;
    }
    groupEnd();
    free( out.p );
    g_renderDirty = 1;

    /* Frame the 3D view on what just arrived: an imported model can be far
     * bigger than whatever you were last zoomed in on, so without this it
     * lands off-screen or fills the view entirely. */
    {
        float ex = (float)( maxX - minX + 1 );
        float ey = (float)( maxY - minY + 1 );
        float ez = (float)( maxZ - minZ + 1 );
        float big = ex > ey ? ex : ey;
        if( ez > big ) big = ez;
        cam_tx = (float)( minX + ox ) + ex * 0.5f;
        cam_ty = ey * 0.5f;
        cam_tz = (float)( minZ + oz ) + ez * 0.5f;
        cam_dist = clampf( big * 2.0f, 1.5f, 400.0f );

        /* An import into an unlit scene (e.g. straight off the command line)
         * would render solid black, so give it a key light scaled to the
         * model: up and in front, on the side the Front view calls right. */
        if( g_numLights == 0 )
            lightAdd( cam_tx - big, big * 2.0f, cam_tz - big,
                      paletteBrightest(), 1.4f );
    }
    sprintf( m, "Imported %d voxels (%d x %d x %d) from .vox", count,
             maxX - minX + 1, maxY - minY + 1, maxZ - minZ + 1 );
    setStatus( m );
    return 1;
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
    double n[3], bev[3];
    int faceAxis, s, k, lock[3];
    double len;

    *ox = fx; *oy = fy; *oz = fz;
    if( !( ( v->smoothFaces >> faceDir6( fx, fy, fz ) ) & 1 ) ) return;

    if( !voxSmoothNormal( v, &n[0], &n[1], &n[2] ) ) return;  /* flat fit */

    faceAxis = ( fy != 0.0 ) ? 1 : ( fz != 0.0 ? 2 : 0 );
    lock[0] = lock[1] = lock[2] = 0;
    /* bev = the purely geometric "bevel" direction this face should lean:
     * the flat face normal plus the outward normal of every SMOOTH
     * perpendicular neighbour it rounds toward.  Used only to sanity-check the
     * fitted normal's in-plane sign below -- the fitted gradient can point the
     * wrong way at a near-symmetric corner (where its primary signal cancels
     * and far cells tip it), and that geometry knows which way is correct. */
    bev[0]=fx; bev[1]=fy; bev[2]=fz;

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
                else       bev[k] += e[k];         /* round toward it */
            } else if( !voxAt( A[0]+dr[0], A[1]+dr[1], A[2]+dr[2] ) ) {
                /* coplanar continuation: neighbour face (A, faceNormal) */
                nsm = faceIsSmoothAt( A[0], A[1], A[2], fx, fy, fz );
                if( !nsm ) { *ox=fx; *oy=fy; *oz=fz; return; }  /* full lock */
            } else {
                /* concave edge: neighbour face on cell A+dr, normal -e */
                nsm = faceIsSmoothAt( A[0]+dr[0], A[1]+dr[1], A[2]+dr[2],
                                      (double)-e[0],(double)-e[1],(double)-e[2] );
                if( !nsm ) lock[k] = 1;            /* perpendicular -> lock tangent */
                else       bev[k] -= e[k];         /* round toward it */
            }
        }
    }

    for( k = 0; k < 3; k++ ) if( lock[k] ) n[k] = 0.0;

    /* Sign guard: where geometry says this face rounds toward a tangent
     * direction (bev[k] != 0) but the fitted normal's tangent points the
     * opposite way, the fit is unreliable (near-symmetric corner) -- flip that
     * tangent component to agree with the geometry.  Coplanar smooth surfaces
     * (spheres) leave bev[k] == 0, so they are untouched. */
    for( k = 0; k < 3; k++ )
        if( k != faceAxis && bev[k] != 0.0 && n[k]*bev[k] < 0.0 )
            n[k] = -n[k];

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

/* Context for the smooth self-shadow suppression below.  Set by shadeWorld
 * before each face's shadow rays: the receiver voxel cell and whether the
 * receiver face being shaded is itself smooth. */
static int g_shadRecvX, g_shadRecvY, g_shadRecvZ;
static int g_shadRecvSmooth = 0;

/* True if every *visible* (exposed) face of the voxel at (x,y,z) is smooth --
 * i.e. it is part of a contiguous smoothed surface with no flat facets showing.
 * Such a voxel should not cast a hard self-shadow onto an immediate smooth
 * neighbour, which otherwise shows up as a dark stripe where the fitted surface
 * curves away from the light. */
static int allVisibleFacesSmooth( int x, int y, int z )
{
    static const int nb[6][3] = {
        { 0, 1, 0 }, { 0,-1, 0 }, { 0, 0, 1 },
        { 0, 0,-1 }, { 1, 0, 0 }, {-1, 0, 0 } };   /* faceDir6 order */
    Voxel *v = voxAt( x, y, z );
    int f, anyVisible = 0;
    if( !v ) return 0;
    for( f = 0; f < 6; f++ ) {
        if( voxAt( x+nb[f][0], y+nb[f][1], z+nb[f][2] ) ) continue;  /* hidden */
        anyVisible = 1;
        if( !( ( v->smoothFaces >> f ) & 1 ) ) return 0;  /* a flat facet shows */
    }
    return anyVisible;   /* fully-enclosed voxel (no visible face) can't shadow */
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
        if( voxAt( x, y, z ) ) {
            /* Smooth self-shadow suppression: when shading a smooth face, ignore
             * an occluder that sits in the receiver's own 3x3x3 neighbourhood
             * AND whose every visible face is smooth (part of the same
             * contiguous curved surface).  This kills the dark stripe from a
             * smooth voxel shadowing its immediate smooth neighbour as the
             * surface curves away from the light, while more distant
             * protrusions -- and any voxel showing a flat facet -- still cast
             * real shadows. */
            if( g_shadRecvSmooth ) {
                int ddx = x - g_shadRecvX, ddy = y - g_shadRecvY,
                    ddz = z - g_shadRecvZ;
                if( ddx >= -1 && ddx <= 1 && ddy >= -1 && ddy <= 1 &&
                    ddz >= -1 && ddz <= 1 && allVisibleFacesSmooth( x, y, z ) )
                    continue;
            }
            return 1;
        }
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
    /* Specular is accumulated separately from the diffuse light so it can be
     * ADDED on top at the end (the usual way: base*diffuse + specular), rather
     * than tinting the highlight by the base color.  All of it is skipped when
     * shininess is 0, leaving the original matte result untouched. */
    double specR = 0.0, specG = 0.0, specB = 0.0, specLit = 0.0;
    double vdx = 0.0, vdy = 0.0, vdz = 0.0;
    if( g_shininess > 0.0f ) obliqueViewDir( &vdx, &vdy, &vdz );
    /* publish the receiver cell + face-smoothness for smooth self-shadow
     * suppression inside shadowedWorld */
    if( v ) {
        g_shadRecvX = v->x; g_shadRecvY = v->y; g_shadRecvZ = v->z;
        g_shadRecvSmooth = ( v->smoothFaces >> faceDir6( gnx, gny, gnz ) ) & 1;
    } else {
        g_shadRecvSmooth = 0;
    }
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

        /* Specular, from the same light, shadow term and attenuation as the
         * diffuse above -- so a highlight cannot appear in shadow.  It uses the
         * SHADING normal (n), not the geometric one, so a smooth face's fitted
         * normal carries the highlight across a curved surface. */
        if( g_shininess > 0.0f ) {
            double sp;
            if( g_specBlinn ) {
                /* Blinn-Phong: n . halfway(light, eye) */
                double hx = ldx + vdx, hy = ldy + vdy, hz = ldz + vdz;
                double hlen = sqrt( hx*hx + hy*hy + hz*hz );
                sp = hlen < 1e-9 ? 0.0 : ( nx*hx + ny*hy + nz*hz ) / hlen;
            } else {
                /* Phong: reflect(-light, n) . eye.  n and ld are unit, so the
                 * reflected vector is unit already. */
                double rx = 2.0*nl*nx - ldx;
                double ry = 2.0*nl*ny - ldy;
                double rz = 2.0*nl*nz - ldz;
                sp = rx*vdx + ry*vdy + rz*vdz;
            }
            if( sp > 0.0 ) {
                double s = pow( sp, (double)g_specPower )
                           * g_shininess * atten * vis;
                specR += s * g_pal[ g_lights[i].color*3+0 ] / 255.0;
                specG += s * g_pal[ g_lights[i].color*3+1 ] / 255.0;
                specB += s * g_pal[ g_lights[i].color*3+2 ] / 255.0;
                specLit += s;
            }
        }
    }
    if( g_shadingMode == 1 ) {
        int rl = v->rampLen < 1 ? 1 : v->rampLen;
        int lo = clampi( v->rampStart, 0, g_palCount - 1 );
        int hi = clampi( v->rampStart + rl - 1, 0, g_palCount - 1 );
        int lumLo = g_pal[lo*3]+g_pal[lo*3+1]+g_pal[lo*3+2];
        int lumHi = g_pal[hi*3]+g_pal[hi*3+1]+g_pal[hi*3+2];
        /* Ramp mode has no off-palette colors to spend on a highlight, so the
         * specular simply pushes the brightness up the voxel's own ramp -- a
         * hot spot reads as a patch of the ramp's lightest samples. */
        double t = clampf( (float)( scalarLit + specLit ), 0.0f, 1.0f );
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
        out[0] = (unsigned char)clampi( (int)( ( br*accR + specR ) * 255.0 + 0.5 ), 0, 255 );
        out[1] = (unsigned char)clampi( (int)( ( bg*accG + specG ) * 255.0 + 0.5 ), 0, 255 );
        out[2] = (unsigned char)clampi( (int)( ( bb*accB + specB ) * 255.0 + 0.5 ), 0, 255 );
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
    for( i = 0; i < g_voxCap; i++ ) {
        int f; Voxel *v;
        if( g_vox[i].used != 1 ) continue;
        v = &g_vox[i];
        for( f = 0; f < 6; f++ ) {
            int nx = v->x+NB[f][0], ny = v->y+NB[f][1], nz = v->z+NB[f][2];
            unsigned char c[4];
            if( voxAt( nx, ny, nz ) ) continue;   /* hidden face */
            if( g_previewShade == 0 ) {
                /* flat: unlit base color (still the visible-layer composite) */
                int fc = voxFlatColor( v );
                c[0] = g_pal[fc*3+0]; c[1] = g_pal[fc*3+1]; c[2] = g_pal[fc*3+2];
            } else if( g_previewShade == 2 ) {
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

/* ------------------------------------------------------------------ */
/* background image (context behind the pixel preview)                 */
/* ------------------------------------------------------------------ */

static void bgUpload( void )
{
    if( !g_bgPix || g_bgW <= 0 || g_bgH <= 0 ) return;
    if( g_bgTex == 0 ) glGenTextures( 1, &g_bgTex );
    glBindTexture( GL_TEXTURE_2D, g_bgTex );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
    glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
    glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, g_bgW, g_bgH, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, g_bgPix );
    glBindTexture( GL_TEXTURE_2D, 0 );
}

static void bgClear( void )
{
    if( g_bgPix ) { stbi_image_free( g_bgPix ); g_bgPix = NULL; }
    if( g_bgTex ) { glDeleteTextures( 1, &g_bgTex ); g_bgTex = 0; }
    g_bgW = g_bgH = 0;
    g_bgOffX = g_bgOffY = 0;
}

/* Load a PNG as the preview background image (kept as raw RGBA for baking). */
static int bgLoadPNG( const char *path )
{
    int w = 0, h = 0, comp = 0;
    unsigned char *data = stbi_load( path, &w, &h, &comp, 4 );
    if( !data ) { setStatus( "BG image load failed" ); return 0; }
    if( w < 1 || h < 1 || w > 2048 || h > 2048 ) {
        stbi_image_free( data );
        setStatus( "BG image must be 1..2048 px on a side" );
        return 0;
    }
    bgClear();
    g_bgPix = data; g_bgW = w; g_bgH = h; g_bgShow = 1;
    bgUpload();
    { char m[128]; sprintf( m, "BG image %dx%d loaded", w, h ); setStatus( m ); }
    return 1;
}

static void ensureRender( void )
{
    if( g_renderDirty ) {
        int save = g_activeLayer;
        ensureFlat();                 /* union of visible layers -> FLAT scratch */
        g_activeLayer = FLAT_LAYER;   /* render & preview see the composite */
        renderOblique();
        rebuildPreviewFaces();
        g_activeLayer = save;
        uploadRenderTexture();
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

/* ---- base64 (for baking the background image into the .ovox text file) ---- */

static const char B64ENC[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64val( int c )
{
    if( c >= 'A' && c <= 'Z' ) return c - 'A';
    if( c >= 'a' && c <= 'z' ) return c - 'a' + 26;
    if( c >= '0' && c <= '9' ) return c - '0' + 52;
    if( c == '+' ) return 62;
    if( c == '/' ) return 63;
    return -1;      /* padding, whitespace, anything else: skip */
}

static void b64enc3( const unsigned char *in, int n, char *out )
{
    unsigned long v = ( (unsigned long)in[0] ) << 16;
    if( n > 1 ) v |= ( (unsigned long)in[1] ) << 8;
    if( n > 2 ) v |= (unsigned long)in[2];
    out[0] = B64ENC[ ( v >> 18 ) & 63 ];
    out[1] = B64ENC[ ( v >> 12 ) & 63 ];
    out[2] = ( n > 1 ) ? B64ENC[ ( v >> 6 ) & 63 ] : '=';
    out[3] = ( n > 2 ) ? B64ENC[ v & 63 ] : '=';
}

/* Emit raw bytes as wrapped "BGDATA <base64>" lines (54 bytes -> 72 chars). */
static void b64WriteFile( FILE *f, const unsigned char *data, long len )
{
    long i; char line[ 80 ]; int col = 0;
    for( i = 0; i < len; i += 3 ) {
        int n = (int)( len - i ); if( n > 3 ) n = 3;
        b64enc3( data + i, n, line + col ); col += 4;
        if( col >= 72 || i + 3 >= len ) {
            line[ col ] = '\0';
            fprintf( f, "BGDATA %s\n", line );
            col = 0;
        }
    }
}

/* Decode a base64 string into a freshly malloc'd byte buffer. */
static unsigned char *b64Decode( const char *s, long *outLen )
{
    long cap = (long)strlen( s ) / 4 * 3 + 4, n = 0;
    unsigned char *out = (unsigned char*)malloc( (size_t)cap );
    int quad[4], qn = 0; const char *p;
    if( !out ) { *outLen = 0; return NULL; }
    for( p = s; *p; p++ ) {
        int v = b64val( (unsigned char)*p );
        if( v < 0 ) continue;
        quad[ qn++ ] = v;
        if( qn == 4 ) {
            out[n++] = (unsigned char)( ( quad[0] << 2 ) | ( quad[1] >> 4 ) );
            out[n++] = (unsigned char)( ( ( quad[1] & 15 ) << 4 ) | ( quad[2] >> 2 ) );
            out[n++] = (unsigned char)( ( ( quad[2] & 3 ) << 6 ) | quad[3] );
            qn = 0;
        }
    }
    if( qn >= 2 ) {
        out[n++] = (unsigned char)( ( quad[0] << 2 ) | ( quad[1] >> 4 ) );
        if( qn == 3 )
            out[n++] = (unsigned char)( ( ( quad[1] & 15 ) << 4 ) | ( quad[2] >> 2 ) );
    }
    *outLen = n;
    return out;
}

static void saveSculpture( const char *path )
{
    FILE *f = fopen( path, "w" );
    int i;
    if( !f ) { setStatus( "Save failed" ); return; }
    fprintf( f, "OBLIQUEVOXELS 3\n" );
    fprintf( f, "PALETTE %s %d\n", g_palName, g_palCount );
    for( i = 0; i < g_palCount; i++ )
        fprintf( f, "C %d %d %d\n", g_pal[i*3+0], g_pal[i*3+1], g_pal[i*3+2] );
    fprintf( f, "AMBIENT %.4f\n", g_ambient );
    for( i = 0; i < g_numLights; i++ )
        fprintf( f, "L %.4f %.4f %.4f %d %.4f %d %d %.4f %d\n",
                 g_lights[i].x, g_lights[i].y, g_lights[i].z,
                 g_lights[i].color, g_lights[i].intensity, g_lights[i].enabled,
                 g_lights[i].infinite, g_lights[i].size, g_lights[i].samples );
    fprintf( f, "RENDER %d %d %d %d %d %d %.4f %.4f %.4f %d\n", g_shadingMode,
             g_voxPx, g_frontScrunch, g_topScrunch, g_orient,
             g_smoothRadius, g_smoothAmt,
             g_shininess, g_specPower, g_specBlinn );
    fprintf( f, "ACTIVE %d\n", g_activeLayer );
    /* Optional preview background image, baked in as base64 RGBA so the .ovox
     * stays self-contained (no external file reference). */
    if( g_bgPix && g_bgW > 0 && g_bgH > 0 ) {
        fprintf( f, "BGIMAGE %d %d %d %d\n", g_bgW, g_bgH, g_bgOffX, g_bgOffY );
        b64WriteFile( f, g_bgPix, (long)g_bgW * g_bgH * 4 );
    }
    /* Layers, bottom to top.  Each "LAYER idx visible name" is followed by that
     * layer's voxels.  Name runs to end of line (may contain spaces).  Files
     * with no LAYER line (older formats) load all voxels into a single layer.
     *
     * one voxel per line: V x y z color rampStart rampLen [smoothFaceMask]
     * The trailing field is a 6-bit per-face smooth mask (order +Y +Z ... see
     * faceDir6).  In the legacy version-1 format this field was a 0/1/2 whole-
     * voxel smooth flag; loadSculpture migrates those. */
    { int save = g_activeLayer, L, total = 0;
      for( L = 0; L < g_numLayers; L++ ) {
          fprintf( f, "LAYER %d %d %s\n", L, g_layers[L].visible,
                   g_layers[L].name[0] ? g_layers[L].name : "Layer" );
          g_activeLayer = L;
          for( i = 0; i < g_voxCap; i++ ) {
              Voxel *v;
              if( g_vox[i].used != 1 ) continue;
              v = &g_vox[i];
              fprintf( f, "V %d %d %d %d %d %d %d\n",
                       v->x, v->y, v->z, v->color, v->rampStart, v->rampLen,
                       v->smoothFaces );
              total++;
          }
      }
      g_activeLayer = save;
      fclose( f );
      { char msg[1400];
        sprintf( msg, "Saved %d voxels, %d layers -> %s",
                 total, g_numLayers, path );
        setStatus( msg ); }
      return;
    }
}

static int loadSculpture( const char *path )
{
    FILE *f = fopen( path, "r" );
    char line[ 256 ];
    int palIdx = 0, palExpected = 0, ver = 1, wantActive = 0;
    char *bgB64 = NULL; long bgB64Len = 0, bgB64Cap = 0;
    int bgIW = 0, bgIH = 0, haveBg = 0, bgOX = 0, bgOY = 0;
    if( !f ) { setStatus( "Open failed" ); return 0; }
    if( !fgets( line, sizeof line, f ) ||
        strncmp( line, "OBLIQUEVOXELS", 13 ) != 0 ) {
        fclose( f ); setStatus( "Not an .ovox file" ); return 0;
    }
    sscanf( line, "OBLIQUEVOXELS %d", &ver );
    layersReset();      /* free all layers -> single empty layer 0 (load target) */
    histClear();
    bgClear();          /* drop any prior preview background image */
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
            float a;
            if( sscanf( line, "AMBIENT %f", &a ) == 1 ) g_ambient = a;
            else sscanf( line, "ACTIVE %d", &wantActive );
        } else if( line[0] == 'B' && line[1] == 'G' ) {
            if( line[2] == 'I' ) {                 /* BGIMAGE w h [offx offy] */
                bgOX = bgOY = 0;
                if( sscanf( line, "BGIMAGE %d %d %d %d",
                            &bgIW, &bgIH, &bgOX, &bgOY ) >= 2 )
                    haveBg = 1;
            } else if( line[2] == 'D' ) {          /* BGDATA <base64> */
                char *d = line + 7;                /* skip "BGDATA " */
                int len = (int)strlen( d );
                while( len > 0 && ( d[len-1]=='\n' || d[len-1]=='\r' ||
                                    d[len-1]==' ' ) ) d[--len] = '\0';
                if( bgB64Len + len + 1 > bgB64Cap ) {
                    long nc = bgB64Cap ? bgB64Cap * 2 : 8192;
                    while( nc < bgB64Len + len + 1 ) nc *= 2;
                    bgB64 = (char*)realloc( bgB64, (size_t)nc ); bgB64Cap = nc;
                }
                if( bgB64 ) { memcpy( bgB64 + bgB64Len, d, (size_t)len );
                              bgB64Len += len; bgB64[bgB64Len] = '\0'; }
            }
        } else if( line[0] == 'L' && line[1] == 'A' ) {
            /* LAYER idx visible name...  (name runs to end of line) */
            int idx = 0, vis = 1, off = 0;
            if( sscanf( line, "LAYER %d %d %n", &idx, &vis, &off ) >= 2 &&
                idx >= 0 && idx < MAX_LAYERS ) {
                char *nm = line + off;
                int len = (int)strlen( nm );
                while( len > 0 && ( nm[len-1]=='\n' || nm[len-1]=='\r' ) )
                    nm[--len] = '\0';
                if( idx >= g_numLayers ) g_numLayers = idx + 1;
                g_layers[idx].visible = vis ? 1 : 0;
                if( len > 0 ) {
                    strncpy( g_layers[idx].name, nm,
                             sizeof g_layers[idx].name - 1 );
                    g_layers[idx].name[ sizeof g_layers[idx].name - 1 ] = '\0';
                } else if( g_layers[idx].name[0] == '\0' )
                    sprintf( g_layers[idx].name, "Layer %d", idx + 1 );
                g_activeLayer = idx;   /* subsequent V lines load into this layer */
            }
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
            /* The specular fields are optional trailing values; a file written
             * before they existed describes a matte render, so default to 0. */
            float sh = 0.0f, sp = 24.0f; int sb = 1;
            sscanf( line, "RENDER %d %d %d %d %d %d %f %f %f %d",
                    &g_shadingMode, &g_voxPx,
                    &g_frontScrunch, &g_topScrunch, &g_orient, &sr, &sa,
                    &sh, &sp, &sb );
            g_smoothRadius = clampi( sr, 1, 4 );
            g_smoothAmt = clampf( sa, 0.0f, 1.0f );
            g_shininess = clampf( sh, 0.0f, 1.0f );
            g_specPower = clampf( sp, 1.0f, 128.0f );
            g_specBlinn = sb ? 1 : 0;
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
    /* materialise the baked background image, if any */
    if( haveBg && bgIW > 0 && bgIH > 0 && bgB64 ) {
        long need = (long)bgIW * bgIH * 4, got = 0;
        unsigned char *px = b64Decode( bgB64, &got );
        if( px && got >= need ) {
            bgClear();
            g_bgPix = px; g_bgW = bgIW; g_bgH = bgIH; g_bgShow = 1;
            g_bgOffX = bgOX; g_bgOffY = bgOY;
            bgUpload();
        } else if( px ) free( px );
    }
    if( bgB64 ) free( bgB64 );
    g_activeLayer = clampi( wantActive, 0, g_numLayers - 1 );
    g_flatDirty = 1;
    if( palIdx > 0 ) g_palCount = palIdx;
    (void)palExpected;
    g_pick = clampi( g_pick, 0, g_palCount-1 );
    g_rampStart = clampi( g_rampStart, 0, g_palCount-1 );
    g_rampEnd = clampi( g_rampEnd, g_rampStart, g_palCount-1 );
    g_renderDirty = 1;
    { char msg[1400]; int L, total = 0;
      for( L = 0; L < g_numLayers; L++ ) total += g_layers[L].used;
      sprintf( msg, "Loaded %d voxels, %d layers from %s",
               total, g_numLayers, path );
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

    /* Key light up and in FRONT, on the side the Front view calls right.  The
     * oblique Front view has screen-right = -x and the viewer at -z (toUVW:
     * u = -x, w = -z), so front-upper-right is negative x, positive y,
     * negative z. */
    lightAdd( -10.0f, 12.0f, -6.0f, paletteBrightest(), 1.1f );
    cam_tx = 3.0f; cam_ty = 2.5f; cam_tz = 3.0f;
}

/* ------------------------------------------------------------------ */
/* view presets                                                        */
/* ------------------------------------------------------------------ */

static void setView( float yaw, float pitch )
{
    cam_yaw = yaw; cam_pitch = pitch;
}

/* True if two angles are within a small tolerance modulo 2*pi. */
static int angClose( float a, float b )
{
    double d = (double)a - (double)b;
    while( d >  M_PI ) d -= 2.0 * M_PI;
    while( d < -M_PI ) d += 2.0 * M_PI;
    return fabs( d ) < 0.02;
}

/* Name of the fixed 3D-view preset the camera currently matches (by yaw/pitch),
 * or NULL when the camera has been freely rotated away from any preset.  Panning
 * leaves yaw/pitch untouched, so the name persists through a pan but vanishes on
 * an orbit -- exactly the behaviour the tester asked for. */
static const char *currentViewName( void )
{
    if( angClose( cam_pitch, 0.0f ) ) {
        if( angClose( cam_yaw, -1.5708f ) ) return "Front";
        if( angClose( cam_yaw,  1.5708f ) ) return "Back";
        if( angClose( cam_yaw,  3.1416f ) ) return "Left";
        if( angClose( cam_yaw,  0.0f    ) ) return "Right";
    }
    if( angClose( cam_yaw, -1.5708f ) ) {
        if( angClose( cam_pitch,  1.5620f ) ) return "Top";
        if( angClose( cam_pitch, -1.5620f ) ) return "Bottom";
    }
    if( angClose( cam_yaw, 0.9f ) && angClose( cam_pitch, 0.6f ) ) return "Iso";
    return NULL;
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
    else if( strcmp( id, "BGImage" ) == 0 ) { strcpy( g_fbExt, ".png" ); setFbFile( "" ); }
    else if( strcmp( id, "Palette" ) == 0 ) { strcpy( g_fbExt, ".gpl" );  setFbFile( "" ); }
    else if( strcmp( id, "ImportLight" ) == 0 ) { strcpy( g_fbExt, ".ovox" ); setFbFile( "" ); }
    else if( strcmp( id, "ImportVOX" ) == 0 ) { strcpy( g_fbExt, ".vox" ); setFbFile( "" ); }
    else                                    { strcpy( g_fbExt, ".ovox" ); setFbFile( "sculpture.ovox" ); }
    fbRefresh();
}

static void buildMenuBar( int *quit )
{
    if( !gui_begin_main_menu_bar() ) return;
    if( gui_begin_menu( "File" ) ) {
        if( gui_menu_item( "New", NULL, 1 ) ) {
            layersReset(); g_numLights = 0; histClear(); bgClear();
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
        if( gui_menu_item( "Import MagicaVoxel (.vox)...", NULL, 1 ) )
            openFileDialog( "ImportVOX" );
        if( gui_menu_item( "Export PNG...", NULL, 1 ) ) openFileDialog( "PNG" );
        gui_separator();
        if( gui_menu_item( "Quit", NULL, 1 ) ) *quit = 1;
        gui_end_menu();
    }
    if( gui_begin_menu( "Edit" ) ) {
        if( gui_menu_item( "Undo", "Ctrl+Z", g_histPos > 0 ) ) histUndo();
        if( gui_menu_item( "Redo", "Ctrl+Y", g_histPos < g_histLen ) ) histRedo();
        gui_separator();
        if( gui_menu_item( "Draw mode",  "D", 1 ) ) g_mode = 0;
        if( gui_menu_item( "Erase mode", "E", 1 ) ) g_mode = 1;
        gui_separator();
        if( gui_menu_item( "Pencil", "B", 1 ) ) g_tool = 0;
        if( gui_menu_item( "Line",   "L", 1 ) ) g_tool = 1;
        if( gui_menu_item( "Rect",   "R", 1 ) ) g_tool = 2;
        if( gui_menu_item( "Box",    "X", 1 ) ) g_tool = 3;
        if( gui_menu_item( "Select (marquee)", "M", 1 ) ) g_tool = 4;
        if( gui_menu_item( "Scribble select", "K", 1 ) ) g_tool = 5;
        if( gui_menu_item( "Cylinder", "C", 1 ) ) g_tool = 6;
        if( gui_menu_item( "Sphere",   "S", 1 ) ) g_tool = 7;
        if( gui_menu_item( "Ellipsoid", "O", 1 ) ) g_tool = 11;
        if( gui_menu_item( "Smoother (faces)", "H", 1 ) ) g_tool = 8;
        if( gui_menu_item( "Eyedropper", "I / Alt", 1 ) ) g_tool = 10;
        if( gui_menu_item( "Image wall", "W", g_impPix ? 1 : 0 ) ) {
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
        if( gui_menu_item( "Front",  "1", 1 ) ) setView( -1.5708f, 0.0f );
        if( gui_menu_item( "Back",   "2", 1 ) ) setView(  1.5708f, 0.0f );
        if( gui_menu_item( "Left",   "3", 1 ) ) setView(  3.1416f, 0.0f );
        if( gui_menu_item( "Right",  "4", 1 ) ) setView(  0.0f,    0.0f );
        if( gui_menu_item( "Top",    "5", 1 ) ) setView( -1.5708f, 1.5620f );
        if( gui_menu_item( "Bottom", "6", 1 ) ) setView( -1.5708f,-1.5620f );
        if( gui_menu_item( "Iso",    "7", 1 ) ) setView( 0.9f, 0.6f );
        gui_separator();
        if( gui_menu_item_check( "Orthographic", "0", &g_ortho ) ) {}
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
    gui_radio_int( "Sphere", &g_tool, 7 ); gui_same_line();
    gui_radio_int( "Ellipsoid", &g_tool, 11 );
    gui_radio_int( "Select", &g_tool, 4 ); gui_same_line();
    gui_radio_int( "Scribble", &g_tool, 5 );
    gui_radio_int( "Smoother (faces)", &g_tool, 8 );
    gui_radio_int( "Eyedropper", &g_tool, 10 );
    if( g_impPix ) gui_radio_int( "Image wall", &g_tool, 9 );
    gui_text( "(ctrl+click any slider to type a value)" );

    if( g_tool == 10 )
        gui_text( "click a voxel to sample its color/ramp" );

    gui_separator_text( "Mode" );
    gui_radio_int( "Draw",  &g_mode, 0 ); gui_same_line();
    gui_radio_int( "Erase", &g_mode, 1 );
    /* auto-smooth: newly drawn (or erase-exposed) faces become smooth */
    if( g_tool <= 7 || g_tool == 11 )
        gui_checkbox( "auto-smooth new faces", &g_autoSmooth );
    if( ( g_tool >= 1 && g_tool <= 3 ) || g_tool == 6 || g_tool == 11 )
        gui_slider_int( "thickness", &g_thickness, 1, 32 );
    if( g_tool == 7 ) {
        gui_slider_int( "sphere depth", &g_sphereDepth, 0, 32 );
        gui_text( "drag on a surface to grow the ball" );
    }
    if( g_tool == 11 ) {
        gui_slider_int( "depth (sink)", &g_sphereDepth, 0, 32 );
        gui_text( "drag a W x H rect on a surface;\n"
                  "thickness = 3rd axis, depth sinks it\n"
                  "(dome at 0, crater when erasing)" );
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
        gui_separator_text( "Move selection" );
        gui_text( "green ghost previews the move;\ncommit overwrites collisions." );
        gui_slider_int( "mx", &g_moveDX, -64, 64 );
        gui_slider_int( "my", &g_moveDY, -64, 64 );
        gui_slider_int( "mz", &g_moveDZ, -64, 64 );
        if( gui_button( "Commit Move" ) ) selMoveCommit();
        gui_same_line();
        if( gui_button( "Reset Move" ) ) { g_moveDX=g_moveDY=g_moveDZ=0; }
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

    /* symmetry planes: mirror every edit across the enabled planes */
    gui_separator_text( "Symmetry" );
    { static const char *axLabel[3] = { "mirror X", "mirror Y", "mirror Z" };
      int a;
      for( a = 0; a < 3; a++ ) {
          gui_push_id( 700 + a );
          if( gui_checkbox( axLabel[a], &g_symOn[a] ) ) symmetryChanged();
          if( g_symOn[a] ) {
              gui_same_line();
              if( gui_checkbox( "+0.5", &g_symHalf[a] ) ) symmetryChanged();
              if( gui_slider_int( "pos", &g_symPos[a], -64, 64 ) ) symmetryChanged();
          }
          gui_pop_id();
      }
      if( g_symOn[0] || g_symOn[1] || g_symOn[2] )
          gui_checkbox( "show planes", &g_symShow );
    }

    /* layers: edits act on the ACTIVE layer; the render/preview show every
     * VISIBLE layer unioned (a higher row wins where cells overlap). */
    gui_separator_text( "Layers" );
    { int L;
      /* list top (highest index) first, matching composite z-order */
      for( L = g_numLayers - 1; L >= 0; L-- ) {
          gui_push_id( 900 + L );
          if( gui_checkbox( "##vis", &g_layers[L].visible ) ) {
              g_flatDirty = 1; g_renderDirty = 1;
          }
          gui_same_line();
          if( gui_selectable( g_layers[L].name[0] ? g_layers[L].name : "(layer)",
                              L == g_activeLayer ) )
              setActiveLayer( L );
          gui_pop_id();
      }
      if( gui_button( "+ Add" ) )   layerAdd();
      gui_same_line();
      if( gui_button( "Delete" ) )  layerDelete( g_activeLayer );
      if( gui_button( "Up" ) )      layerSwap( g_activeLayer, g_activeLayer + 1 );
      gui_same_line();
      if( gui_button( "Down" ) )    layerSwap( g_activeLayer, g_activeLayer - 1 );
      gui_input_text( "name", g_layers[g_activeLayer].name,
                      (int)sizeof g_layers[g_activeLayer].name );
      sprintf( buf, "active: %s", g_layers[g_activeLayer].name );
      gui_text( buf );
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
    if( gui_slider_float( "shininess", &g_shininess, 0.0f, 1.0f, "%.2f" ) )
        g_renderDirty = 1;
    if( g_shininess > 0.0f ) {
        if( gui_slider_float( "spec power", &g_specPower, 1.0f, 128.0f, "%.0f" ) )
            g_renderDirty = 1;
        if( gui_checkbox( "Blinn-Phong (off = Phong)", &g_specBlinn ) )
            g_renderDirty = 1;
        gui_text( "higher power = tighter highlight" );
    } else {
        gui_text( "0 = matte (no specular)" );
    }

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
        /* front upper right of the target (see buildDemoScene) */
        lightAdd( cam_tx - 6.0f, cam_ty + 8.0f, cam_tz - 6.0f,
                  paletteBrightest(), 1.0f );
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
    { int zi = (int)( g_prevZoom + 0.5f ); if( zi < 1 ) zi = 1;
      if( gui_slider_int( "zoom", &zi, 1, 32 ) ) g_prevZoom = (float)zi; }
    gui_same_line();
    if( gui_small_button( "reset" ) )
        { g_prevPanX = g_prevPanY = 0.0f; g_prevZoom = 3.0f; }

    ensureRender();
    sprintf( buf, "render: %d x %d px", g_imgW, g_imgH );
    gui_text( buf );

    if( gui_button( "Export PNG..." ) ) openFileDialog( "PNG" );

    gui_separator();
    /* background-image controls (context behind the pixel preview) */
    if( gui_button( "Load BG image..." ) ) openFileDialog( "BGImage" );
    if( g_bgPix ) {
        gui_same_line();
        if( gui_small_button( "Clear BG" ) ) {
            bgClear(); setStatus( "BG image cleared" );
        }
        gui_checkbox( "Show BG", &g_bgShow );
        sprintf( buf, "BG: %d x %d px", g_bgW, g_bgH );
        gui_text( buf );
        /* nudge the BG image into register with the render (saved in .ovox) */
        gui_slider_int( "BG x", &g_bgOffX, -g_bgW, g_bgW );
        gui_slider_int( "BG y", &g_bgOffY, -g_bgH, g_bgH );
        if( gui_small_button( "Center BG" ) ) g_bgOffX = g_bgOffY = 0;
    } else {
        gui_text( "(no background image)" );
    }

    gui_separator();
    { float aw, ah;
      gui_content_avail( &aw, &ah );
      if( gui_begin_child( "renderview", aw, ah, 0 ) ) {
        gui_pan_zoom_image( g_imgTex, g_imgW, g_imgH,
                            g_bgTex, g_bgW, g_bgH, g_bgShow,
                            g_bgOffX, g_bgOffY,
                            &g_prevZoom, &g_prevPanX, &g_prevPanY );
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
    } else if( strcmp( g_fbAction, "ImportVOX" ) == 0 ) {
        importVox( full );
    } else if( strcmp( g_fbAction, "BGImage" ) == 0 ) {
        bgLoadPNG( full );
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
    else if( strcmp( act, "ImportVOX" ) == 0 ) { title = "Import MagicaVoxel model (.vox)"; actLabel = "Import"; }
    else if( strcmp( act, "BGImage" ) == 0 )  { title = "Load background image (.png)"; actLabel = "Load"; }
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
    /* Move the target within the camera's screen-right/up plane so a pan always
     * tracks the mouse regardless of orbit angle.  Basis matches gluLookAt's:
     * forward = normalize(target-eye); right = normalize(forward x worldUp);
     * up = right x forward.  (The previous version had a scrambled cross
     * product that flipped the axes after some orbits.) */
    double cp = cos( cam_pitch ), sp = sin( cam_pitch );
    double cy = cos( cam_yaw ),   sy = sin( cam_yaw );
    double fx = -cp*cy, fy = -sp, fz = -cp*sy;      /* forward (eye->target) */
    double rx, ry, rz, ux, uy, uz, s, l;
    /* right = forward x worldUp, worldUp = (0,1,0) */
    rx = fy*0.0 - fz*1.0;   /* = -fz */
    ry = fz*0.0 - fx*0.0;   /* = 0    */
    rz = fx*1.0 - fy*0.0;   /* = fx   */
    l = sqrt( rx*rx + ry*ry + rz*rz );
    if( l > 1e-6 ) { rx/=l; ry/=l; rz/=l; }
    /* up = right x forward */
    ux = ry*fz - rz*fy; uy = rz*fx - rx*fz; uz = rx*fy - ry*fx;
    s = cam_dist * 0.0016;
    /* drag right -> scene moves right -> target moves left (-right);
       drag down (screen y grows down) -> scene moves down -> target up (+up) */
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
/* per-tool mouse cursors                                              */
/* ------------------------------------------------------------------ */
/* Each cursor is 16x16 ASCII art: '#' = black, '.' = white, ' ' =
 * transparent.  White fill + black edge reads on both the dark 3D background
 * and light voxels.  The app drives the SDL cursor itself (ImGui's cursor
 * management is disabled), so the cursor tells you the active tool/mode. */

static SDL_Cursor *g_curArrow  = NULL;   /* default (over panels) */
static SDL_Cursor *g_curPencil = NULL;   /* draw mode */
static SDL_Cursor *g_curErase  = NULL;   /* erase mode */
static SDL_Cursor *g_curDrop   = NULL;   /* eyedropper */
static SDL_Cursor *g_curSelect = NULL;   /* marquee / scribble select (draw) */
static SDL_Cursor *g_curSelectE= NULL;   /* marquee / scribble select (erase) */
static SDL_Cursor *g_curSmooth = NULL;   /* smoother */
static SDL_Cursor *g_curResize = NULL;   /* panel splitter (system) */

/* Build a cursor from 16x16 ASCII art: '.' = white, '#' = black, ' ' =
 * transparent.  Any transparent cell 8-adjacent to a white cell is
 * auto-promoted to black, so every white shape gets a 1px black halo and stays
 * legible over light voxels *and* the dark 3D background — the art only has to
 * spell out the white silhouette. */
static SDL_Cursor *makeCursor( const char *rows[16], int hotx, int hoty )
{
    Uint8 data[32], mask[32];
    char g[16][16];
    int y, x, dy, dx;
    memset( data, 0, sizeof data );
    memset( mask, 0, sizeof mask );
    for( y = 0; y < 16; y++ )
        for( x = 0; x < 16; x++ ) g[y][x] = rows[y][x];
    /* halo pass: transparent cells touching white become black */
    for( y = 0; y < 16; y++ )
        for( x = 0; x < 16; x++ ) {
            if( g[y][x] != ' ' ) continue;
            for( dy = -1; dy <= 1; dy++ )
                for( dx = -1; dx <= 1; dx++ ) {
                    int ny = y + dy, nx = x + dx;
                    if( ny < 0 || ny > 15 || nx < 0 || nx > 15 ) continue;
                    if( rows[ny][nx] == '.' ) { g[y][x] = '#'; dy = dx = 2; }
                }
        }
    for( y = 0; y < 16; y++ )
        for( x = 0; x < 16; x++ ) {
            char c = g[y][x];
            int byte = y * 2 + ( x >> 3 );
            int bit  = 7 - ( x & 7 );
            if( c == '#' ) { data[byte] |= (Uint8)( 1 << bit );
                             mask[byte] |= (Uint8)( 1 << bit ); }
            else if( c == '.' ) mask[byte] |= (Uint8)( 1 << bit );
            /* ' ' -> transparent (both bits 0) */
        }
    return SDL_CreateCursor( data, mask, 16, 16, hotx, hoty );
}

static void initCursors( void )
{
    /* Draw: a small solid white box (halo pass adds the black border). */
    static const char *pencil[16] = {
        "                ", "                ", "                ",
        "     ......     ", "     ......     ", "     ......     ",
        "     ......     ", "     ......     ", "     ......     ",
        "                ", "                ", "                ",
        "                ", "                ", "                ",
        "                " };
    /* Erase: a hollow white box; the halo pass paints black on both the inside
     * and outside of the ring, so it reads as an empty box over anything. */
    static const char *erase[16] = {
        "                ", "                ", "                ",
        "    ........    ", "    .      .    ", "    .      .    ",
        "    .      .    ", "    .      .    ", "    .      .    ",
        "    .      .    ", "    ........    ", "                ",
        "                ", "                ", "                ",
        "                " };
    /* Eyedropper: an empty white circle (halo pass paints black inside and
     * out).  A symmetric ring avoids the "which end is the tip?" ambiguity of a
     * pointer -- the sampled pixel is simply the ring's centre (the hotspot). */
    static const char *drop[16] = {
        "                ", "      ....      ", "    ..    ..    ",
        "   .        .   ", "  .          .  ", "  .          .  ",
        " .            . ", " .            . ", " .            . ",
        "  .          .  ", "  .          .  ", "   .        .   ",
        "    ..    ..    ", "      ....      ", "                ",
        "                " };
    /* Select: corner brackets + a centre dot (draw); erase variant drops the
     * dot so the mode reads at a glance. */
    static const char *select[16] = {
        "                ", "  ...      ...  ", "  .          .  ",
        "  .          .  ", "                ", "                ",
        "                ", "       ..       ", "       ..       ",
        "                ", "                ", "  .          .  ",
        "  .          .  ", "  ...      ...  ", "                ",
        "                " };
    static const char *selectE[16] = {
        "                ", "  ...      ...  ", "  .          .  ",
        "  .          .  ", "                ", "                ",
        "                ", "                ", "                ",
        "                ", "                ", "  .          .  ",
        "  .          .  ", "  ...      ...  ", "                ",
        "                " };
    static const char *smooth[16] = {
        "                ", "                ", "      ....      ",
        "    ........    ", "   ..........   ", "   ..........   ",
        "  ............  ", "  ............  ", "  ............  ",
        "  ............  ", "   ..........   ", "   ..........   ",
        "    ........    ", "      ....      ", "                ",
        "                " };
    g_curArrow  = SDL_CreateSystemCursor( SDL_SYSTEM_CURSOR_ARROW );
    g_curResize = SDL_CreateSystemCursor( SDL_SYSTEM_CURSOR_SIZEWE );
    g_curPencil = makeCursor( pencil, 7, 6  );
    g_curErase  = makeCursor( erase,  7, 6  );
    g_curDrop   = makeCursor( drop,   7, 7  );
    g_curSelect = makeCursor( select, 7, 8  );
    g_curSelectE= makeCursor( selectE,7, 8  );
    g_curSmooth = makeCursor( smooth, 7, 7  );
}

static void freeCursors( void )
{
    if( g_curArrow  ) SDL_FreeCursor( g_curArrow );
    if( g_curResize ) SDL_FreeCursor( g_curResize );
    if( g_curPencil ) SDL_FreeCursor( g_curPencil );
    if( g_curErase  ) SDL_FreeCursor( g_curErase );
    if( g_curDrop   ) SDL_FreeCursor( g_curDrop );
    if( g_curSelect ) SDL_FreeCursor( g_curSelect );
    if( g_curSelectE) SDL_FreeCursor( g_curSelectE );
    if( g_curSmooth ) SDL_FreeCursor( g_curSmooth );
}

/* Choose and apply the mouse cursor for this frame based on where the mouse is
 * and which tool/mode is active. */
/* Build the live gesture-dimension readout for the 3D view's upper-left corner.
 * Returns 1 (and fills buf) while a sizing gesture is open: W x H for a
 * line/rect/marquee, W x H x thickness for a box/cylinder/ellipsoid, and the
 * radius + diameter for a sphere. */
static int gestureDimText( char *buf )
{
    if( g_sphActive && g_sphR >= 0.5 ) {
        sprintf( buf, "r %.1f   d %d", g_sphR, (int)( g_sphR * 2.0 + 0.5 ) );
        return 1;
    }
    if( g_gActive && g_gHaveB &&
        ( g_tool==1 || g_tool==2 || g_tool==3 ||
          g_tool==4 || g_tool==6 || g_tool==11 ) ) {
        int A = g_gAxis, i0 = (A+1)%3, i1 = (A+2)%3;
        int a[3], b[3], w, h;
        a[0]=g_gAx; a[1]=g_gAy; a[2]=g_gAz;
        b[0]=g_gBx; b[1]=g_gBy; b[2]=g_gBz;
        w = abs( a[i0]-b[i0] ) + 1;
        h = abs( a[i1]-b[i1] ) + 1;
        if( g_tool==3 || g_tool==6 || g_tool==11 )
            sprintf( buf, "%d x %d x %d", w, h, g_thickness );
        else
            sprintf( buf, "%d x %d", w, h );
        return 1;
    }
    return 0;
}

static void updateCursor( int mx, int my, int winW, int overSplitter )
{
    SDL_Cursor *c = g_curArrow;
    int overGui = gui_any_window_hovered() || gui_any_popup_open();
    (void)winW;
    if( overSplitter && !overGui ) {
        c = g_curResize;
    } else if( inView( mx, my ) && !overGui ) {
        if( g_tool == 10 )      c = g_curDrop;    /* eyedropper */
        else if( g_tool == 8 )  c = g_curSmooth;  /* smoother */
        else if( g_tool == 4 || g_tool == 5 )
            c = ( g_mode == 1 ) ? g_curSelectE : g_curSelect;
        else                    c = ( g_mode == 1 ) ? g_curErase : g_curPencil;
    }
    if( c ) SDL_SetCursor( c );
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* self-test helper: assemble a minimal .vox on disk                    */
/* ------------------------------------------------------------------ */

typedef struct { unsigned char b[ 4096 ]; int n; } VoxBuf;

static void vbPut( VoxBuf *v, const void *d, int n )
{
    if( v->n + n > (int)sizeof v->b ) return;
    memcpy( v->b + v->n, d, (size_t)n );
    v->n += n;
}

static void vbI32( VoxBuf *v, int x )
{
    unsigned char t[4];
    t[0] = (unsigned char)( x & 255 );
    t[1] = (unsigned char)( ( x >> 8 ) & 255 );
    t[2] = (unsigned char)( ( x >> 16 ) & 255 );
    t[3] = (unsigned char)( ( x >> 24 ) & 255 );
    vbPut( v, t, 4 );
}

static void vbStr( VoxBuf *v, const char *s )
{
    vbI32( v, (int)strlen( s ) );
    vbPut( v, s, (int)strlen( s ) );
}

static void vbChunk( VoxBuf *v, const char *id, const VoxBuf *content,
                     const VoxBuf *kids )
{
    vbPut( v, id, 4 );
    vbI32( v, content ? content->n : 0 );
    vbI32( v, kids ? kids->n : 0 );
    if( content ) vbPut( v, content->b, content->n );
    if( kids )    vbPut( v, kids->b, kids->n );
}

typedef struct {
    int sx, sy, sz;              /* model box */
    const unsigned char *vx;     /* nv * 4 bytes: x, y, z, colour index */
    int nv;
    int rot, t[3];               /* placement, when the file has a scene graph */
} VoxTestModel;

/* Write a .vox holding nm models.  useGraph wraps them in the real node
 * layout -- root nTRN -> nGRP -> per-model nTRN(rot,t) -> nSHP -- so the
 * scene-graph walk is exercised; otherwise the file is bare SIZE/XYZI pairs
 * (the shape a pre-scene-graph file has).  pal256, when given, is written as
 * the RGBA chunk (256 RGBA quads); without it the import has no file palette
 * and falls back to the current paint colour. */
static int selftestWriteVox( const char *path, const VoxTestModel *m, int nm,
                             int useGraph, const unsigned char *pal256 )
{
    VoxBuf kids, c, f, file;
    FILE *fp;
    char num[64];
    int i;
    kids.n = 0;

    for( i = 0; i < nm; i++ ) {
        c.n = 0;
        vbI32( &c, m[i].sx ); vbI32( &c, m[i].sy ); vbI32( &c, m[i].sz );
        vbChunk( &kids, "SIZE", &c, NULL );
        c.n = 0; vbI32( &c, m[i].nv ); vbPut( &c, m[i].vx, m[i].nv * 4 );
        vbChunk( &kids, "XYZI", &c, NULL );
    }

    if( useGraph ) {
        c.n = 0;
        vbI32( &c, 0 );              /* node 0: the root transform */
        vbI32( &c, 0 );              /* attributes: empty dict */
        vbI32( &c, 1 );              /* child: the group */
        vbI32( &c, -1 );             /* reserved */
        vbI32( &c, -1 );             /* layer */
        vbI32( &c, 1 );              /* one frame */
        vbI32( &c, 0 );              /* frame dict: empty (identity) */
        vbChunk( &kids, "nTRN", &c, NULL );

        c.n = 0;
        vbI32( &c, 1 );              /* node 1: the group */
        vbI32( &c, 0 );
        vbI32( &c, nm );
        for( i = 0; i < nm; i++ ) vbI32( &c, 2 + i*2 );
        vbChunk( &kids, "nGRP", &c, NULL );

        for( i = 0; i < nm; i++ ) {
            c.n = 0;
            vbI32( &c, 2 + i*2 );    /* this model's transform node */
            vbI32( &c, 0 );
            vbI32( &c, 3 + i*2 );    /* child: its shape node */
            vbI32( &c, -1 );
            vbI32( &c, 0 );
            vbI32( &c, 1 );
            f.n = 0;
            vbI32( &f, 2 );          /* frame dict: _r and _t */
            vbStr( &f, "_r" );
            sprintf( num, "%d", m[i].rot );                vbStr( &f, num );
            vbStr( &f, "_t" );
            sprintf( num, "%d %d %d", m[i].t[0], m[i].t[1], m[i].t[2] );
            vbStr( &f, num );
            vbPut( &c, f.b, f.n );
            vbChunk( &kids, "nTRN", &c, NULL );

            c.n = 0;
            vbI32( &c, 3 + i*2 );    /* shape node */
            vbI32( &c, 0 );
            vbI32( &c, 1 );          /* one model */
            vbI32( &c, i );          /* model id */
            vbI32( &c, 0 );          /* per-model dict */
            vbChunk( &kids, "nSHP", &c, NULL );
        }
    }

    if( pal256 ) {
        c.n = 0; vbPut( &c, pal256, 256*4 );
        vbChunk( &kids, "RGBA", &c, NULL );
    }

    file.n = 0;
    vbPut( &file, "VOX ", 4 );
    vbI32( &file, 150 );
    vbChunk( &file, "MAIN", NULL, &kids );

    fp = fopen( path, "wb" );
    if( !fp ) return 0;
    fwrite( file.b, 1, (size_t)file.n, fp );
    fclose( fp );
    return 1;
}

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
    paletteDefault();   /* Sheltzy32-with-ramps, baked in (File menu loads .gpl) */
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
    initCursors();

    initSoftSamples();
    layersReset();
    if( argc > 1 ) {
        size_t al = strlen( argv[1] );
        if( al > 4 && strcmp( argv[1] + al - 4, ".vox" ) == 0 ) {
            /* a MagicaVoxel model: import its geometry into the empty scene */
            if( !importVox( argv[1] ) ) buildDemoScene();
            else histClear();   /* the import is the starting state, not an edit */
        } else if( !loadSculpture( argv[1] ) ) buildDemoScene();
    } else {
        buildDemoScene();
    }
    setStatus( "Pencil: left-click or drag to scribble voxels.  "
               "Line/Rect/Box/Select: left-drag.  "
               "Right-drag: orbit  |  Mid-drag: pan  |  Wheel: zoom" );

    if( getenv( "OV_SELFTEST" ) ) {
        int ok = 1;
        /* symmetry: boundary mirror across X at pos 0 -> x' = -x-1 */
        voxClear();
        g_symOn[0]=1; g_symOn[1]=0; g_symOn[2]=0;
        g_symPos[0]=0; g_symHalf[0]=0; g_mode=0;
        putCell( 2, 3, 4 );
        if( !voxAt(2,3,4) || !voxAt(-3,3,4) ) { ok=0; fprintf(stderr,"FAIL sym-boundary\n"); }
        /* mid-voxel mirror across X centred on cell 0 -> x' = -x */
        voxClear();
        g_symPos[0]=0; g_symHalf[0]=1;
        putCell( 2, 0, 0 );
        if( !voxAt(2,0,0) || !voxAt(-2,0,0) ) { ok=0; fprintf(stderr,"FAIL sym-half\n"); }
        /* self-mapped centre cell places just one voxel */
        voxClear(); putCell( 0, 0, 0 );
        if( g_voxUsed != 1 ) { ok=0; fprintf(stderr,"FAIL sym-center dup (%d)\n",g_voxUsed); }
        /* move selection */
        voxClear(); g_symOn[0]=0;
        voxSet(0,0,0,5,5,1); voxSet(1,0,0,6,6,1);
        { Voxel*a=voxAt(0,0,0),*b=voxAt(1,0,0); a->sel=1; b->sel=1; }
        g_moveDX=10; g_moveDY=0; g_moveDZ=0;
        selMoveCommit();
        if( voxAt(0,0,0) || voxAt(1,0,0) ) { ok=0; fprintf(stderr,"FAIL move-src-remain\n"); }
        if( !voxAt(10,0,0) || !voxAt(11,0,0) ) { ok=0; fprintf(stderr,"FAIL move-dst\n"); }
        if( voxAt(10,0,0) && voxAt(10,0,0)->color!=5 ) { ok=0; fprintf(stderr,"FAIL move-color\n"); }
        if( g_moveDX!=0 ) { ok=0; fprintf(stderr,"FAIL move-reset\n"); }
        /* layers: composite union, higher-layer-wins, visibility, and the
         * positional selection projection across a layer switch */
        layersReset();
        voxSet(0,0,0,3,3,1);                 /* layer 0 */
        layerAdd();                          /* -> active layer 1 */
        if( g_numLayers != 2 || g_activeLayer != 1 )
            { ok=0; fprintf(stderr,"FAIL layer-add\n"); }
        voxSet(0,0,0,7,7,1);                 /* overlaps layer 0's cell */
        voxSet(2,0,0,8,8,1);
        g_flatDirty=1; ensureFlat();
        { int sv=g_activeLayer; g_activeLayer=FLAT_LAYER;
          if(!voxAt(0,0,0)||voxAt(0,0,0)->color!=7)
              {ok=0;fprintf(stderr,"FAIL layer-topwins\n");}
          if(!voxAt(2,0,0)){ok=0;fprintf(stderr,"FAIL layer-union\n");}
          g_activeLayer=sv; }
        g_layers[1].visible=0; g_flatDirty=1; ensureFlat();
        { int sv=g_activeLayer; g_activeLayer=FLAT_LAYER;
          if(!voxAt(0,0,0)||voxAt(0,0,0)->color!=3)
              {ok=0;fprintf(stderr,"FAIL layer-hide\n");}
          if(voxAt(2,0,0)){ok=0;fprintf(stderr,"FAIL layer-hide2\n");}
          g_activeLayer=sv; }
        g_layers[1].visible=1; g_flatDirty=1;
        { Voxel*a; g_activeLayer=1; a=voxAt(0,0,0); if(a)a->sel=1;
          setActiveLayer(0);
          if(!voxAt(0,0,0)||!voxAt(0,0,0)->sel)
              {ok=0;fprintf(stderr,"FAIL sel-project\n");} }
        /* smoothing an active-layer face must flag the composite dirty so the
         * pixel render (which shades the FLAT composite) picks it up live */
        layersReset();
        g_symOn[0]=g_symOn[1]=g_symOn[2]=0; g_mode=0;
        voxSet(5,0,0,1,1,1);
        g_flatDirty=1; ensureFlat();          /* composite now has a flat face */
        g_curGroup=++g_groupSeq;
        smoothOneFace(5,0,0,0,1,0);            /* mark +Y smooth on active layer */
        { int sv=g_activeLayer; ensureFlat();  /* must rebuild from the dirty flag */
          g_activeLayer=FLAT_LAYER;
          if(!voxAt(5,0,0)||!(voxAt(5,0,0)->smoothFaces&(1<<faceDir6(0,1,0))))
              {ok=0;fprintf(stderr,"FAIL smooth-composite\n");}
          g_activeLayer=sv; }
        layersReset();
        /* symmetric move: with mirror-X (boundary at 0) live and a voxel on each
         * side both selected, one +offset drags them apart symmetrically. */
        voxClear();
        g_symOn[0]=1; g_symOn[1]=g_symOn[2]=0; g_symPos[0]=0; g_symHalf[0]=0;
        voxSet(1,0,0,5,5,1); voxSet(-2,0,0,5,5,1);   /* -2 is the mirror of 1 */
        { Voxel*a=voxAt(1,0,0),*b=voxAt(-2,0,0); if(a)a->sel=1; if(b)b->sel=1; }
        g_moveDX=3; g_moveDY=0; g_moveDZ=0;
        selMoveCommit();
        if( !voxAt(4,0,0) ) { ok=0; fprintf(stderr,"FAIL symmove-plus\n"); }
        if( !voxAt(-5,0,0) ) { ok=0; fprintf(stderr,"FAIL symmove-minus\n"); }
        if( voxAt(1,0,0) || voxAt(-2,0,0) )
            { ok=0; fprintf(stderr,"FAIL symmove-src\n"); }
        /* symmetryChanged mirrors a one-sided selection to the other side */
        voxClear();
        g_symOn[0]=1; g_symPos[0]=0; g_symHalf[0]=0;
        voxSet(3,0,0,5,5,1); voxSet(-4,0,0,5,5,1);   /* -4 is the mirror of 3 */
        { Voxel*a=voxAt(3,0,0); if(a)a->sel=1; }      /* select only +side */
        symmetryChanged();
        if( !voxAt(-4,0,0) || !voxAt(-4,0,0)->sel )
            { ok=0; fprintf(stderr,"FAIL symchanged-mirror\n"); }
        g_symOn[0]=0;
        /* ellipsoid: fills its interior, empties its corners */
        layersReset();
        g_symOn[0]=g_symOn[1]=g_symOn[2]=0; g_mode=0;
        g_gAx=0; g_gAy=0; g_gAz=0; g_gBx=6; g_gBy=0; g_gBz=6;
        g_gAxis=1; g_gDir=1; g_gHaveB=1; g_thickness=7; g_sphereDepth=3;
        groupBegin(); regionForEach( 11, cbPut, NULL ); groupEnd();
        if( !voxAt(3,0,3) ) { ok=0; fprintf(stderr,"FAIL ellipsoid-center\n"); }
        if( voxAt(0,0,0) )  { ok=0; fprintf(stderr,"FAIL ellipsoid-corner\n"); }
        g_thickness=1; g_sphereDepth=0; g_gHaveB=0;
        layersReset();

        /* .vox import.  MagicaVoxel is z-up and its +x runs the opposite way
         * from ours, so (x,y,z)vox -> (-x,z,y) here.  The test shape is an
         * asymmetric L -- a symmetric one would map onto itself under a
         * mirror and let a flipped axis pass unnoticed.  Model cells
         * A(0,0,0) B(1,0,0) C(0,0,1), centre = size/2 = (1,0,1), so locals are
         * A(-1,0,-1) B(0,0,-1) C(-1,0,0) -> ours A(1,-1,0) B(0,-1,0) C(1,0,0);
         * re-centring x/z and dropping onto y=0 lifts them by one. */
        {
            const char *p = "/tmp/ov_selftest.vox";
            unsigned char L[12], pole[20], bar[12];
            unsigned char pal[ 256*4 ];
            VoxTestModel tm[2];
            int i, wantRed, wantGreen;
            L[0]=0; L[1]=0; L[2]=0; L[3]=1;        /* A */
            L[4]=1; L[5]=0; L[6]=0; L[7]=1;        /* B */
            L[8]=0; L[9]=0; L[10]=1; L[11]=1;      /* C */
            memset( tm, 0, sizeof tm );
            tm[0].sx=2; tm[0].sy=1; tm[0].sz=2; tm[0].vx=L; tm[0].nv=3;
            layersReset(); histClear();
            if( !selftestWriteVox( p, tm, 1, 0, NULL ) )
                { ok=0; fprintf(stderr,"FAIL vox-write\n"); }
            else if( !importVox( p ) )
                { ok=0; fprintf(stderr,"FAIL vox-import\n"); }
            else {
                if( g_voxUsed != 3 )
                    { ok=0; fprintf(stderr,"FAIL vox-count (%d)\n",g_voxUsed); }
                if( !voxAt(1,0,0) || !voxAt(0,0,0) || !voxAt(1,1,0) )
                    { ok=0; fprintf(stderr,"FAIL vox-map\n"); }
            }
            /* one undo group: the whole import backs out in a single step */
            histUndo();
            if( g_voxUsed != 0 )
                { ok=0; fprintf(stderr,"FAIL vox-undo (%d)\n",g_voxUsed); }

            /* Scene graph + rotation: a 3-tall pole (up the model's z) with
             * rot byte 40 = 90 degrees about x, which tips it onto the vox y
             * axis -- our depth axis -- so it must land along our z, not y. */
            for( i = 0; i < 3; i++ ) {
                pole[i*4+0] = 0; pole[i*4+1] = 0;
                pole[i*4+2] = (unsigned char)i;     /* z = 0,1,2 */
                pole[i*4+3] = 1;
            }
            memset( tm, 0, sizeof tm );
            tm[0].sx=1; tm[0].sy=1; tm[0].sz=3; tm[0].vx=pole; tm[0].nv=3;
            tm[0].rot=40;
            layersReset(); histClear();
            if( !selftestWriteVox( p, tm, 1, 1, NULL ) )
                { ok=0; fprintf(stderr,"FAIL vox-write2\n"); }
            else if( !importVox( p ) )
                { ok=0; fprintf(stderr,"FAIL vox-import2\n"); }
            else if( !voxAt(0,0,-1) || !voxAt(0,0,0) || !voxAt(0,0,1) ||
                     g_voxUsed != 3 )
                { ok=0; fprintf(stderr,"FAIL vox-rot\n"); }

            /* Two models under a group, translated apart.  Relative placement
             * is the only thing that pins the pivot down (a single model's
             * centre offset washes out in the re-centring), so this is what
             * catches a wrong "model origin is its centre" convention.
             * bar: 3x1x1 at t=(10,0,0) -> centre (1,0,0), world x 9..11,
             *      ours x -9..-11, y 0.  pole: 1x1x5 at t=(-10,0,0) ->
             *      centre z 2, world z -2..2, ours x 10, y -2..2.
             * So x spans -11..10 (ox=1) and y spans -2..2 (oy=2). */
            for( i = 0; i < 3; i++ ) {
                bar[i*4+0] = (unsigned char)i; bar[i*4+1] = 0;
                bar[i*4+2] = 0; bar[i*4+3] = 1;
            }
            for( i = 0; i < 5; i++ ) {
                pole[i*4+0] = 0; pole[i*4+1] = 0;
                pole[i*4+2] = (unsigned char)i; pole[i*4+3] = 2;
            }
            memset( tm, 0, sizeof tm );
            tm[0].sx=3; tm[0].sy=1; tm[0].sz=1; tm[0].vx=bar;  tm[0].nv=3;
            tm[0].rot=4; tm[0].t[0]=10;
            tm[1].sx=1; tm[1].sy=1; tm[1].sz=5; tm[1].vx=pole; tm[1].nv=5;
            tm[1].rot=4; tm[1].t[0]=-10;
            /* A voxel's colour index c reads the file palette's entry c-1, so
             * put red at entry 0 (the bar's index 1) and green at entry 1 (the
             * pole's index 2): reading the palette straight would swap them. */
            memset( pal, 0, sizeof pal );
            pal[0]=255; pal[3]=255;                /* entry 0 = red   */
            pal[5]=255; pal[7]=255;                /* entry 1 = green */
            wantRed   = nearestPaletteIndex( 255, 0, 0 );
            wantGreen = nearestPaletteIndex( 0, 255, 0 );
            layersReset(); histClear();
            if( !selftestWriteVox( p, tm, 2, 1, pal ) )
                { ok=0; fprintf(stderr,"FAIL vox-write3\n"); }
            else if( !importVox( p ) )
                { ok=0; fprintf(stderr,"FAIL vox-import3\n"); }
            else {
                if( g_voxUsed != 8 )
                    { ok=0; fprintf(stderr,"FAIL vox-multi-count (%d)\n",
                                    g_voxUsed); }
                if( !voxAt(-8,2,0) || !voxAt(-9,2,0) || !voxAt(-10,2,0) )
                    { ok=0; fprintf(stderr,"FAIL vox-multi-bar\n"); }
                if( !voxAt(11,0,0) || !voxAt(11,4,0) )
                    { ok=0; fprintf(stderr,"FAIL vox-multi-pole\n"); }
                if( wantRed == wantGreen )
                    { ok=0; fprintf(stderr,"FAIL vox-pal-testbad\n"); }
                if( voxAt(-9,2,0) && voxAt(-9,2,0)->color != wantRed )
                    { ok=0; fprintf(stderr,"FAIL vox-pal-bar\n"); }
                if( voxAt(11,0,0) && voxAt(11,0,0)->color != wantGreen )
                    { ok=0; fprintf(stderr,"FAIL vox-pal-pole\n"); }
            }
            remove( p );
            layersReset(); histClear();
        }
        fprintf( stderr, ok ? "SELFTEST OK\n" : "SELFTEST FAILED\n" );
        gui_shutdown(); SDL_GL_DeleteContext(glctx); SDL_DestroyWindow(window);
        SDL_Quit();
        return ok ? 0 : 1;
    }

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
                        /* ---- number keys: 3D view presets ---- */
                        else if( k == SDLK_1 ) setView( -1.5708f, 0.0f );
                        else if( k == SDLK_2 ) setView(  1.5708f, 0.0f );
                        else if( k == SDLK_3 ) setView(  3.1416f, 0.0f );
                        else if( k == SDLK_4 ) setView(  0.0f,    0.0f );
                        else if( k == SDLK_5 ) setView( -1.5708f, 1.5620f );
                        else if( k == SDLK_6 ) setView( -1.5708f,-1.5620f );
                        else if( k == SDLK_7 ) setView( 0.9f, 0.6f );
                        else if( k == SDLK_0 ) g_ortho = !g_ortho;
                        /* ---- draw/erase mode ---- */
                        else if( k == SDLK_d ) g_mode = 0;   /* draw */
                        else if( k == SDLK_e ) g_mode = 1;   /* erase */
                        /* ---- letter keys: drawing tools (Aseprite-ish) ---- */
                        else if( k == SDLK_b ) g_tool = 0;   /* pencil/brush */
                        else if( k == SDLK_l ) g_tool = 1;   /* line */
                        else if( k == SDLK_r ) g_tool = 2;   /* rect */
                        else if( k == SDLK_x ) g_tool = 3;   /* box */
                        else if( k == SDLK_m ) g_tool = 4;   /* marquee select */
                        else if( k == SDLK_k ) g_tool = 5;   /* scribble select */
                        else if( k == SDLK_c ) g_tool = 6;   /* cylinder */
                        else if( k == SDLK_s ) g_tool = 7;   /* sphere */
                        else if( k == SDLK_o ) g_tool = 11;  /* ellipsoid */
                        else if( k == SDLK_h ) g_tool = 8;   /* smoother */
                        else if( k == SDLK_i ) g_tool = 10;  /* eyedropper */
                        else if( k == SDLK_w ) {             /* image wall */
                            if( g_impPix ) { g_tool = 9; g_impStage = 0;
                                             g_impHaveHover = 0; }
                        }
                        /* ALT held: quick-toggle eyedropper (revert on release) */
                        else if( ( k == SDLK_LALT || k == SDLK_RALT ) &&
                                 !g_altEyedrop && g_tool != 10 ) {
                            g_savedTool = g_tool; g_tool = 10; g_altEyedrop = 1;
                        }
                        else if( k == SDLK_DELETE || k == SDLK_BACKSPACE ) selDelete();
                        else if( k == SDLK_ESCAPE ) {
                            if( g_tool == 9 && g_impPix && g_impStage > 0 ) {
                                g_impStage = 0; g_impHaveHover = 0;
                                setStatus( "Placement reset -- click a new corner" );
                            } else { selClear(); setStatus("Selection cleared"); }
                        }
                    }
                    break;
                case SDL_KEYUP:
                    /* release the ALT quick-eyedropper and restore the tool */
                    if( g_altEyedrop &&
                        ( ev.key.keysym.sym == SDLK_LALT ||
                          ev.key.keysym.sym == SDLK_RALT ) ) {
                        g_tool = g_savedTool; g_altEyedrop = 0;
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
                        else if( g_dragBtn == SDL_BUTTON_LEFT && g_tool == 0 )
                            pencilBegin( ev.button.x, ev.button.y );
                        else if( g_dragBtn == SDL_BUTTON_LEFT &&
                                 g_tool != 9 && g_tool != 10 )
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
                            else if( g_pencilActive )
                                pencilCommit();  /* click or drag: one undo step */
                            else if( !g_moved && g_tool == 10 )
                                eyedropAt( ev.button.x, ev.button.y );
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
                        else if( g_pencilActive && g_dragBtn == SDL_BUTTON_LEFT )
                            pencilAt( ev.motion.x, ev.motion.y );
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

        /* view-name / projection indicator, centred above the 3D viewport */
        { const char *vn = currentViewName();
          char lbl[64];
          if( vn ) sprintf( lbl, "%s  \xc2\xb7  %s", vn,
                            g_ortho ? "Orthographic" : "Perspective" );
          else     sprintf( lbl, "%s", g_ortho ? "Orthographic" : "Perspective" );
          gui_overlay_text( (float)g_viewX + g_viewW * 0.5f,
                            (float)g_viewY + 6.0f, lbl,
                            vn ? 235 : 150, vn ? 235 : 150, vn ? 180 : 150 ); }

        /* live gesture-dimension readout in the 3D view's upper-left corner */
        { char dim[64];
          if( gestureDimText( dim ) )
              gui_overlay_text_left( (float)g_viewX + 10.0f,
                                     (float)g_viewY + 6.0f, dim, 120, 255, 170 ); }

        /* draggable splitter handles at each panel/view boundary (drawn on the
         * foreground; the drag itself is handled in the SDL event loop).
         * Suppressed while any menu/popup is open so the foreground handle can't
         * paint over an open dropdown's text. */
        if( !gui_any_popup_open() )
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

        /* per-tool mouse cursor for this frame */
        { int mx, my, overSplit;
          SDL_GetMouseState( &mx, &my );
          overSplit = g_splitDrag ||
                      ( mx >= (int)g_leftW - 6 && mx < (int)g_leftW ) ||
                      ( mx >= winW - (int)g_rightW && mx < winW - (int)g_rightW + 6 );
          updateCursor( mx, my, winW, overSplit ); }

        gui_render();

        SDL_GL_SwapWindow( window );

        if( quitAfter && ++frameNum >= quitAfter ) running = 0;
    }

    if( exportEnv ) savePNG( exportEnv );

    freeCursors();
    bgClear();
    gui_shutdown();
    if( g_imgTex ) glDeleteTextures( 1, &g_imgTex );
    free( g_img );
    free( g_vox );
    SDL_GL_DeleteContext( glctx );
    SDL_DestroyWindow( window );
    SDL_Quit();
    return 0;
}
