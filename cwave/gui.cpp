/* gui.cpp -- implementation of the C GUI shim over Dear ImGui.
 *
 * Compiled as C++.  Uses the SDL2 platform backend and the legacy
 * OpenGL2 renderer backend (fixed-function pipeline), which matches
 * cwave's raw-GL waveform drawing and avoids any shader compilation
 * so the app launches instantly.
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

int gui_want_capture_mouse( void )
{
    return ImGui::GetIO().WantCaptureMouse ? 1 : 0;
}
int gui_want_capture_keyboard( void )
{
    return ImGui::GetIO().WantCaptureKeyboard ? 1 : 0;
}
int gui_any_window_hovered( void )
{
    return ImGui::IsWindowHovered( ImGuiHoveredFlags_AnyWindow ) ? 1 : 0;
}

/* ---- main menu bar ---- */
int  gui_begin_main_menu_bar( void ) { return ImGui::BeginMainMenuBar() ? 1 : 0; }
void gui_end_main_menu_bar( void )   { ImGui::EndMainMenuBar(); }

float gui_main_menu_bar_height( void )
{
    return ImGui::GetFrameHeight();
}

int  gui_begin_menu( const char *label ) { return ImGui::BeginMenu( label ) ? 1 : 0; }
void gui_end_menu( void )                { ImGui::EndMenu(); }

int gui_menu_item( const char *label, const char *shortcut, int enabled )
{
    return ImGui::MenuItem( label, shortcut, false, enabled != 0 ) ? 1 : 0;
}

void gui_separator( void ) { ImGui::Separator(); }

/* ---- windows ---- */
void gui_set_next_window_pos( float x, float y )
{
    ImGui::SetNextWindowPos( ImVec2( x, y ) );
}
void gui_set_next_window_size( float w, float h )
{
    ImGui::SetNextWindowSize( ImVec2( w, h ) );
}
void gui_set_next_window_size_appearing( float w, float h )
{
    ImGui::SetNextWindowSize( ImVec2( w, h ), ImGuiCond_Appearing );
}
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

/* ---- widgets ---- */
void gui_text( const char *s ) { ImGui::TextUnformatted( s ); }

void gui_text_colored( float r, float g, float b, const char *s )
{
    ImGui::TextColored( ImVec4( r, g, b, 1.0f ), "%s", s );
}

int  gui_button( const char *label ) { return ImGui::Button( label ) ? 1 : 0; }
int  gui_small_button( const char *label ) { return ImGui::SmallButton( label ) ? 1 : 0; }
void gui_same_line( void ) { ImGui::SameLine(); }
void gui_spacing( void ) { ImGui::Spacing(); }

int gui_slider_float( const char *label, float *v, float mn, float mx,
                      const char *fmt )
{
    return ImGui::SliderFloat( label, v, mn, mx, fmt ? fmt : "%.3f" ) ? 1 : 0;
}

int gui_input_float( const char *label, float *v )
{
    return ImGui::InputFloat( label, v ) ? 1 : 0;
}

int gui_input_text( const char *label, char *buf, int buflen )
{
    return ImGui::InputText( label, buf, (size_t)buflen,
                             ImGuiInputTextFlags_EnterReturnsTrue ) ? 1 : 0;
}

int gui_checkbox( const char *label, int *v )
{
    bool b = ( *v != 0 );
    bool changed = ImGui::Checkbox( label, &b );
    *v = b ? 1 : 0;
    return changed ? 1 : 0;
}

int gui_selectable( const char *label, int selected )
{
    return ImGui::Selectable( label, selected != 0 ) ? 1 : 0;
}

float gui_content_avail_h( void )
{
    return ImGui::GetContentRegionAvail().y;
}

void gui_progress_bar( float fraction, const char *overlay )
{
    ImGui::ProgressBar( fraction, ImVec2( -1.0f, 0.0f ), overlay );
}

/* ---- popups ---- */
void gui_open_popup( const char *id ) { ImGui::OpenPopup( id ); }

/* Close the current popup when Escape is pressed while it is focused.  Skip
 * when a widget inside it is active (e.g. a text field being edited) so that
 * Escape first cancels the edit rather than the whole dialog.  ImGui does not
 * reliably close popups on Escape without keyboard-nav enabled, so we do it. */
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
int gui_begin_popup_modal_resizable( const char *id )
{
    if( ImGui::BeginPopupModal( id, NULL, 0 ) ) {
        popup_escape_to_close();
        return 1;
    }
    return 0;
}
void gui_end_popup( void ) { ImGui::EndPopup(); }
void gui_close_current_popup( void ) { ImGui::CloseCurrentPopup(); }

} /* extern "C" */
