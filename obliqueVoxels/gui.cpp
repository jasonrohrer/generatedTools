/* gui.cpp -- implementation of the C GUI shim over Dear ImGui.
 *
 * Compiled as C++.  Uses the SDL2 platform backend and the legacy OpenGL2
 * renderer backend (fixed-function pipeline), which matches obliqueVoxels'
 * raw-GL 3D voxel drawing and avoids any shader compilation so the app
 * launches instantly.
 */
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl2.h"

#include <SDL.h>
#include <SDL_opengl.h>

#include "gui.h"

extern "C" {

int gui_init( SDL_Window *window, void *gl_context )
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;   /* no imgui.ini clutter, faster startup */
    io.LogFilename = NULL;
    /* The app manages the SDL mouse cursor itself (per-tool cursors over the 3D
     * view), so stop the SDL2 backend from overriding it each frame. */
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding    = 4.0f;
    st.FrameRounding     = 3.0f;
    st.GrabRounding      = 3.0f;
    st.ScrollbarRounding = 3.0f;

    if( !ImGui_ImplSDL2_InitForOpenGL( window, gl_context ) ) return 1;
    if( !ImGui_ImplOpenGL2_Init() ) return 1;
    return 0;
}

void gui_shutdown( void )
{
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void gui_process_event( const SDL_Event *ev )
{
    ImGui_ImplSDL2_ProcessEvent( ev );
}

void gui_new_frame( SDL_Window *window )
{
    (void)window;
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

void gui_render( void )
{
    ImGui::Render();
    ImGui_ImplOpenGL2_RenderDrawData( ImGui::GetDrawData() );
}

int gui_want_capture_mouse( void )    { return ImGui::GetIO().WantCaptureMouse ? 1 : 0; }
int gui_want_capture_keyboard( void ) { return ImGui::GetIO().WantCaptureKeyboard ? 1 : 0; }
int gui_any_window_hovered( void )
{
    return ImGui::IsWindowHovered( ImGuiHoveredFlags_AnyWindow ) ? 1 : 0;
}
/* True if any popup/menu/modal is currently open.  Unlike WantCaptureMouse
 * this is immediate (not hover-lagged), so it reliably blocks 3D-view clicks
 * while a dropdown or dialog is up. */
int gui_any_popup_open( void )
{
    return ImGui::IsPopupOpen( 0, ImGuiPopupFlags_AnyPopupId |
                                  ImGuiPopupFlags_AnyPopupLevel ) ? 1 : 0;
}

/* ---- main menu bar ---- */
int  gui_begin_main_menu_bar( void ) { return ImGui::BeginMainMenuBar() ? 1 : 0; }
void gui_end_main_menu_bar( void )   { ImGui::EndMainMenuBar(); }
float gui_main_menu_bar_height( void ) { return ImGui::GetFrameHeight(); }
int  gui_begin_menu( const char *label ) { return ImGui::BeginMenu( label ) ? 1 : 0; }
void gui_end_menu( void )                { ImGui::EndMenu(); }
int gui_menu_item( const char *label, const char *shortcut, int enabled )
{
    return ImGui::MenuItem( label, shortcut, false, enabled != 0 ) ? 1 : 0;
}
int gui_menu_item_check( const char *label, const char *shortcut, int *checked )
{
    bool b = ( *checked != 0 );
    bool clicked = ImGui::MenuItem( label, shortcut, &b );
    *checked = b ? 1 : 0;
    return clicked ? 1 : 0;
}
void gui_separator( void ) { ImGui::Separator(); }
void gui_separator_text( const char *label ) { ImGui::SeparatorText( label ); }

/* ---- windows ---- */
void gui_set_next_window_pos( float x, float y )
{ ImGui::SetNextWindowPos( ImVec2( x, y ) ); }
void gui_set_next_window_size( float w, float h )
{ ImGui::SetNextWindowSize( ImVec2( w, h ) ); }
void gui_set_next_window_size_appearing( float w, float h )
{ ImGui::SetNextWindowSize( ImVec2( w, h ), ImGuiCond_Appearing ); }
void gui_get_window_size( float *w, float *h )
{
    ImVec2 s = ImGui::GetWindowSize();
    if( w ) *w = s.x;
    if( h ) *h = s.y;
}

int gui_begin( const char *name, int fixedPanel )
{
    ImGuiWindowFlags flags = 0;
    if( fixedPanel )
        flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoBringToFrontOnFocus;
    return ImGui::Begin( name, NULL, flags ) ? 1 : 0;
}
void gui_end( void ) { ImGui::End(); }

int gui_begin_child( const char *id, float w, float h, int border )
{
    return ImGui::BeginChild( id, ImVec2( w, h ), border != 0 ) ? 1 : 0;
}
void gui_end_child( void ) { ImGui::EndChild(); }
void gui_content_avail( float *w, float *h )
{
    ImVec2 a = ImGui::GetContentRegionAvail();
    if( w ) *w = a.x;
    if( h ) *h = a.y;
}

/* ---- widgets ---- */
void gui_text( const char *s ) { ImGui::TextUnformatted( s ); }
void gui_text_colored( float r, float g, float b, const char *s )
{ ImGui::TextColored( ImVec4( r, g, b, 1.0f ), "%s", s ); }
int  gui_button( const char *label ) { return ImGui::Button( label ) ? 1 : 0; }
int  gui_button_sized( const char *label, float w, float h )
{ return ImGui::Button( label, ImVec2( w, h ) ) ? 1 : 0; }
int  gui_small_button( const char *label ) { return ImGui::SmallButton( label ) ? 1 : 0; }
void gui_same_line( void ) { ImGui::SameLine(); }
void gui_spacing( void ) { ImGui::Spacing(); }
void gui_dummy( float w, float h ) { ImGui::Dummy( ImVec2( w, h ) ); }

int gui_slider_float( const char *label, float *v, float mn, float mx, const char *fmt )
{ return ImGui::SliderFloat( label, v, mn, mx, fmt ? fmt : "%.3f" ) ? 1 : 0; }
int gui_slider_int( const char *label, int *v, int mn, int mx )
{ return ImGui::SliderInt( label, v, mn, mx ) ? 1 : 0; }
int gui_drag_int( const char *label, int *v, float speed, int mn, int mx )
{ return ImGui::DragInt( label, v, speed, mn, mx ) ? 1 : 0; }
int gui_input_int( const char *label, int *v ) { return ImGui::InputInt( label, v ) ? 1 : 0; }

int gui_checkbox( const char *label, int *v )
{
    bool b = ( *v != 0 );
    bool changed = ImGui::Checkbox( label, &b );
    *v = b ? 1 : 0;
    return changed ? 1 : 0;
}
int gui_radio_int( const char *label, int *v, int thisVal )
{
    if( ImGui::RadioButton( label, *v == thisVal ) ) { *v = thisVal; return 1; }
    return 0;
}
int gui_combo( const char *label, int *idx, const char *const items[], int count )
{ return ImGui::Combo( label, idx, items, count ) ? 1 : 0; }
int gui_input_text( const char *label, char *buf, int buflen )
{
    return ImGui::InputText( label, buf, (size_t)buflen,
                             ImGuiInputTextFlags_EnterReturnsTrue ) ? 1 : 0;
}

void gui_push_id( int id ) { ImGui::PushID( id ); }
void gui_pop_id( void )    { ImGui::PopID(); }

/* ---- color / palette ---- */
int gui_color_button( const char *id, int r, int g, int b, int selected, float size )
{
    ImU32 col = IM_COL32( r, g, b, 255 );
    ImGuiColorEditFlags fl = ImGuiColorEditFlags_NoTooltip |
                             ImGuiColorEditFlags_NoDragDrop |
                             ImGuiColorEditFlags_NoAlpha;
    ImVec4 c = ImGui::ColorConvertU32ToFloat4( col );
    bool clicked = ImGui::ColorButton( id, c, fl, ImVec2( size, size ) );
    if( selected ) {
        ImVec2 mn = ImGui::GetItemRectMin();
        ImVec2 mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRect( ImVec2( mn.x - 1, mn.y - 1 ),
                                             ImVec2( mx.x + 1, mx.y + 1 ),
                                             IM_COL32( 255, 255, 255, 255 ),
                                             0.0f, 0, 2.0f );
    }
    return clicked ? 1 : 0;
}

int gui_palette_grid( const unsigned char *rgb, int count, int perRow,
                      float cell, int *picked, int *rampStart, int *rampEnd )
{
    if( count <= 0 || perRow <= 0 ) return 0;

    int rows = ( count + perRow - 1 ) / perRow;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 total( perRow * cell, rows * cell );
    ImDrawList *dl = ImGui::GetWindowDrawList();

    /* draw cells */
    for( int i = 0; i < count; i++ ) {
        int cx = i % perRow, cy = i / perRow;
        ImVec2 p0( origin.x + cx * cell, origin.y + cy * cell );
        ImVec2 p1( p0.x + cell, p0.y + cell );
        ImU32 col = IM_COL32( rgb[i*3], rgb[i*3+1], rgb[i*3+2], 255 );
        dl->AddRectFilled( p0, p1, col );
        dl->AddRect( p0, p1, IM_COL32( 0, 0, 0, 90 ) );
    }

    ImGui::InvisibleButton( "palgrid", total );
    bool active  = ImGui::IsItemActive();
    bool clicked = ImGui::IsItemClicked();
    int changed = 0;

    static int dragStart = -1;
    ImVec2 mp = ImGui::GetIO().MousePos;
    int mx = (int)( ( mp.x - origin.x ) / cell );
    int my = (int)( ( mp.y - origin.y ) / cell );
    int hovIdx = -1;
    if( mx >= 0 && mx < perRow && my >= 0 && my < rows ) {
        int idx = my * perRow + mx;
        if( idx < count ) hovIdx = idx;
    }

    if( clicked && hovIdx >= 0 ) {
        dragStart = hovIdx;
        *picked = hovIdx;
        *rampStart = hovIdx;
        *rampEnd   = hovIdx;
        changed = 1;
    } else if( active && dragStart >= 0 && hovIdx >= 0 ) {
        int a = dragStart, b = hovIdx;
        if( a > b ) { int t = a; a = b; b = t; }
        if( a != *rampStart || b != *rampEnd ) {
            *rampStart = a;
            *rampEnd   = b;
            *picked    = dragStart;
            changed = 1;
        }
    }
    if( !active ) dragStart = -1;

    /* highlight the current ramp range */
    if( *rampStart >= 0 && *rampEnd >= *rampStart ) {
        for( int i = *rampStart; i <= *rampEnd && i < count; i++ ) {
            int cx = i % perRow, cy = i / perRow;
            ImVec2 p0( origin.x + cx * cell, origin.y + cy * cell );
            ImVec2 p1( p0.x + cell, p0.y + cell );
            dl->AddRect( p0, p1, IM_COL32( 255, 220, 40, 255 ), 0.0f, 0, 2.0f );
        }
    }
    /* highlight the picked cell */
    if( *picked >= 0 && *picked < count ) {
        int cx = *picked % perRow, cy = *picked / perRow;
        ImVec2 p0( origin.x + cx * cell, origin.y + cy * cell );
        ImVec2 p1( p0.x + cell, p0.y + cell );
        dl->AddRect( ImVec2( p0.x - 1, p0.y - 1 ), ImVec2( p1.x + 1, p1.y + 1 ),
                     IM_COL32( 255, 255, 255, 255 ), 0.0f, 0, 2.0f );
    }
    return changed;
}

int gui_selectable( const char *label, int selected )
{
    return ImGui::Selectable( label, selected != 0 ) ? 1 : 0;
}

void gui_overlay_rect( float x0, float y0, float x1, float y1,
                       int r, int g, int b, int a )
{
    ImGui::GetForegroundDrawList()->AddRectFilled(
        ImVec2( x0, y0 ), ImVec2( x1, y1 ), IM_COL32( r, g, b, a ) );
}

void gui_overlay_text( float cx, float y, const char *s, int r, int g, int b )
{
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImVec2 ts = ImGui::CalcTextSize( s );
    ImVec2 p( cx - ts.x * 0.5f, y );
    dl->AddRectFilled( ImVec2( p.x - 7.0f, p.y - 3.0f ),
                       ImVec2( p.x + ts.x + 7.0f, p.y + ts.y + 3.0f ),
                       IM_COL32( 0, 0, 0, 130 ), 4.0f );
    dl->AddText( ImVec2( p.x + 1.0f, p.y + 1.0f ), IM_COL32( 0, 0, 0, 190 ), s );
    dl->AddText( p, IM_COL32( r, g, b, 255 ), s );
}

/* ---- image ---- */
void gui_image( unsigned int texId, float w, float h )
{
    ImGui::Image( (ImTextureID)(intptr_t)texId, ImVec2( w, h ) );
}

/* Pan/zoom image canvas: fills the available content region, draws the texture
 * centred plus a (*panX,*panY) screen-pixel offset, scaled by *zoom.  Scroll
 * wheel over the canvas zooms about the mouse point; a left-drag pans.  All
 * three state values are updated in place.  Used for the oblique-render
 * preview so it centres, scroll-zooms on the cursor, and drag-pans. */
void gui_pan_zoom_image( unsigned int texId, int imgW, int imgH,
                         unsigned int bgTex, int bgW, int bgH, int showBg,
                         float *zoom, float *panX, float *panY )
{
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if( avail.x < 16.0f ) avail.x = 16.0f;
    if( avail.y < 16.0f ) avail.y = 16.0f;

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 cMin = origin;
    ImVec2 cMax( origin.x + avail.x, origin.y + avail.y );
    ImVec2 center( origin.x + avail.x * 0.5f, origin.y + avail.y * 0.5f );

    ImGui::InvisibleButton( "pzcanvas", avail );
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();
    ImGuiIO &io  = ImGui::GetIO();

    /* wheel zoom in whole-integer steps (pixel art wants integer magnification),
     * anchored on the point under the cursor */
    if( hovered && io.MouseWheel != 0.0f ) {
        float old = *zoom;
        int   step = io.MouseWheel > 0.0f ? 1 : -1;
        float nz  = floorf( old + 0.5f ) + (float)step;
        if( nz < 1.0f )  nz = 1.0f;
        if( nz > 64.0f ) nz = 64.0f;
        {
            float icx = center.x + *panX;      /* image centre on screen */
            float icy = center.y + *panY;
            float ux  = ( io.MousePos.x - icx ) / old;  /* img-space offset */
            float uy  = ( io.MousePos.y - icy ) / old;
            *panX = ( io.MousePos.x - center.x ) - ux * nz;
            *panY = ( io.MousePos.y - center.y ) - uy * nz;
            *zoom = nz;
        }
    }

    /* left-drag pans */
    if( active && ImGui::IsMouseDragging( ImGuiMouseButton_Left, 0.0f ) ) {
        *panX += io.MouseDelta.x;
        *panY += io.MouseDelta.y;
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled( cMin, cMax, IM_COL32( 18, 19, 22, 255 ) );
    dl->PushClipRect( cMin, cMax, true );
    if( texId && imgW > 0 && imgH > 0 ) {
        float z  = *zoom;
        float hw = imgW * z * 0.5f;
        float hh = imgH * z * 0.5f;
        ImVec2 p0( center.x + *panX - hw, center.y + *panY - hh );
        ImVec2 p1( center.x + *panX + hw, center.y + *panY + hh );
        /* background image first, centred on the render but offset by a whole
         * number of pixels so the two pixel grids stay aligned */
        if( showBg && bgTex && bgW > 0 && bgH > 0 ) {
            float ox = floorf( ( (float)imgW - (float)bgW ) * 0.5f + 0.5f );
            float oy = floorf( ( (float)imgH - (float)bgH ) * 0.5f + 0.5f );
            ImVec2 b0( p0.x + ox * z, p0.y + oy * z );
            ImVec2 b1( b0.x + bgW * z, b0.y + bgH * z );
            dl->AddImage( (ImTextureID)(intptr_t)bgTex, b0, b1 );
        }
        dl->AddImage( (ImTextureID)(intptr_t)texId, p0, p1 );
    }
    dl->PopClipRect();
    dl->AddRect( cMin, cMax, IM_COL32( 90, 90, 100, 255 ) );
}

/* ---- popups ---- */
void gui_open_popup( const char *id ) { ImGui::OpenPopup( id ); }
int  gui_begin_popup( const char *id ) { return ImGui::BeginPopup( id ) ? 1 : 0; }

static void popup_escape_to_close( void )
{
    if( ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows ) &&
        !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed( ImGuiKey_Escape ) )
        ImGui::CloseCurrentPopup();
}
int gui_begin_popup_modal( const char *id )
{
    if( ImGui::BeginPopupModal( id, NULL, ImGuiWindowFlags_AlwaysAutoResize ) ) {
        popup_escape_to_close();
        return 1;
    }
    return 0;
}
void gui_end_popup( void ) { ImGui::EndPopup(); }
void gui_close_current_popup( void ) { ImGui::CloseCurrentPopup(); }

} /* extern "C" */
