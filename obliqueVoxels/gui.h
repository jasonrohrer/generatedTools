/* gui.h -- thin C API over Dear ImGui + SDL2/OpenGL2 backends.
 *
 * Dear ImGui is C++, but the obliqueVoxels application is C89.  This shim
 * exposes exactly the immediate-mode calls the app needs, so all application
 * logic stays in C.  Implemented in gui.cpp.
 */
#ifndef OV_GUI_H
#define OV_GUI_H

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;
union  SDL_Event;

/* ---- lifecycle ---- */
int  gui_init( struct SDL_Window *window, void *gl_context );
void gui_shutdown( void );
void gui_process_event( const union SDL_Event *ev );
void gui_new_frame( struct SDL_Window *window );
void gui_render( void );

int  gui_want_capture_mouse( void );
int  gui_want_capture_keyboard( void );
/* True if the mouse is over ANY ImGui window (a panel, menu, or popup). */
int  gui_any_window_hovered( void );

/* ---- main menu bar ---- */
int   gui_begin_main_menu_bar( void );
void  gui_end_main_menu_bar( void );
float gui_main_menu_bar_height( void );
int   gui_begin_menu( const char *label );
void  gui_end_menu( void );
int   gui_menu_item( const char *label, const char *shortcut, int enabled );
int   gui_menu_item_check( const char *label, const char *shortcut, int *checked );
void  gui_separator( void );
void  gui_separator_text( const char *label );

/* ---- windows / layout ---- */
void gui_set_next_window_pos( float x, float y );
void gui_set_next_window_size( float w, float h );
void gui_set_next_window_size_appearing( float w, float h );
void gui_get_window_size( float *w, float *h );
/* flags: 0 default; 1 = no-title/no-resize/no-move fixed panel */
int  gui_begin( const char *name, int fixedPanel );
void gui_end( void );
int  gui_begin_child( const char *id, float w, float h, int border );
void gui_end_child( void );
void gui_content_avail( float *w, float *h );

/* ---- widgets ---- */
void gui_text( const char *s );
void gui_text_colored( float r, float g, float b, const char *s );
int  gui_button( const char *label );
int  gui_button_sized( const char *label, float w, float h );
int  gui_small_button( const char *label );
void gui_same_line( void );
void gui_spacing( void );
void gui_dummy( float w, float h );
int  gui_slider_float( const char *label, float *v, float mn, float mx,
                       const char *fmt );
int  gui_slider_int( const char *label, int *v, int mn, int mx );
int  gui_drag_int( const char *label, int *v, float speed, int mn, int mx );
int  gui_input_int( const char *label, int *v );
int  gui_checkbox( const char *label, int *v );
/* radio: shows 'label'; active when *v==thisVal; click sets *v=thisVal.
 * returns 1 if clicked. */
int  gui_radio_int( const char *label, int *v, int thisVal );
int  gui_combo( const char *label, int *idx,
                const char *const items[], int count );
int  gui_input_text( const char *label, char *buf, int buflen );

/* ---- ids (for lists) ---- */
void gui_push_id( int id );
void gui_pop_id( void );

/* ---- color / palette ---- */
/* A clickable color swatch.  rgb are 0..255.  size in pixels.  'selected'
 * draws a highlight border.  Returns 1 if clicked this frame. */
int  gui_color_button( const char *id, int r, int g, int b,
                       int selected, float size );

/* Palette grid picker.  'rgb' is count*3 bytes (r,g,b per entry).  Draws a
 * grid 'perRow' wide with 'cell'-pixel cells.  A single click sets *picked and
 * collapses the ramp to that one index.  A click-drag selects an inclusive
 * ramp [*rampStart,*rampEnd]; *picked follows the drag start.  The current
 * *picked cell and the [*rampStart,*rampEnd] range are outlined.  Returns 1 if
 * the selection changed this frame. */
int  gui_palette_grid( const unsigned char *rgb, int count, int perRow,
                       float cell, int *picked, int *rampStart, int *rampEnd );

int  gui_selectable( const char *label, int selected );

/* Draw a filled rectangle on the top-most (foreground) draw list, above every
 * window.  Used to paint the draggable panel splitter handles.  rgba 0..255. */
void gui_overlay_rect( float x0, float y0, float x1, float y1,
                       int r, int g, int b, int a );

/* Draw a short text label centred horizontally on cx, with its top at y, on the
 * foreground draw list (above every window).  Painted with a translucent
 * backdrop + drop shadow so it reads over the 3D view.  Used for the view-name
 * indicator above the 3D viewport. */
void gui_overlay_text( float cx, float y, const char *s, int r, int g, int b );

/* ---- image (the oblique render texture) ---- */
void gui_image( unsigned int texId, float w, float h );

/* Pan/zoom image canvas filling the available content region.  Draws texId
 * centred + (*panX,*panY) screen-px offset, scaled by *zoom.  Scroll wheel
 * over the canvas zooms about the cursor (in INTEGER steps, min 1x, for crisp
 * pixel art); left-drag pans.  Updates the three state values in place.  If
 * bgTex is non-zero and showBg is set, it is drawn first (behind texId),
 * centred on the same point but snapped to a whole-pixel offset so its pixel
 * grid aligns with the render even when their dimensions differ in parity. */
void gui_pan_zoom_image( unsigned int texId, int imgW, int imgH,
                         unsigned int bgTex, int bgW, int bgH, int showBg,
                         int bgOffX, int bgOffY,
                         float *zoom, float *panX, float *panY );

/* ---- popups / modals ---- */
int  gui_any_popup_open( void );
void gui_open_popup( const char *id );
int  gui_begin_popup( const char *id );
int  gui_begin_popup_modal( const char *id );
void gui_end_popup( void );
void gui_close_current_popup( void );

#ifdef __cplusplus
}
#endif

#endif /* OV_GUI_H */
