/* gui.h -- thin C API over Dear ImGui + SDL2/OpenGL2 backends.
 *
 * Dear ImGui is C++, but the cwave application is C89.  This shim
 * exposes exactly the immediate-mode calls the app needs, so all
 * application logic stays in C.  Implemented in gui.cpp.
 */
#ifndef CWAVE_GUI_H
#define CWAVE_GUI_H

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

/* ---- main menu bar ---- */
int   gui_begin_main_menu_bar( void );
void  gui_end_main_menu_bar( void );
float gui_main_menu_bar_height( void );
int   gui_begin_menu( const char *label );
void  gui_end_menu( void );
int   gui_menu_item( const char *label, const char *shortcut, int enabled );
void  gui_separator( void );

/* ---- windows / layout ---- */
void gui_set_next_window_pos( float x, float y );
void gui_set_next_window_size( float w, float h );
/* flags: 0 default; 1 = no-title/no-resize/no-move fixed panel */
int  gui_begin( const char *name, int fixedPanel );
void gui_end( void );
int  gui_begin_child( const char *id, float w, float h, int border );
void gui_end_child( void );

/* ---- widgets ---- */
void gui_text( const char *s );
void gui_text_colored( float r, float g, float b, const char *s );
int  gui_button( const char *label );
int  gui_small_button( const char *label );
void gui_same_line( void );
void gui_spacing( void );
int  gui_slider_float( const char *label, float *v, float mn, float mx,
                       const char *fmt );
int  gui_input_float( const char *label, float *v );
int  gui_input_text( const char *label, char *buf, int buflen );
int  gui_checkbox( const char *label, int *v );
int  gui_selectable( const char *label, int selected );
float gui_content_avail_h( void );
void gui_progress_bar( float fraction, const char *overlay );

/* ---- popups / modals ---- */
void gui_open_popup( const char *id );
int  gui_begin_popup_modal( const char *id );
void gui_end_popup( void );
void gui_close_current_popup( void );

#ifdef __cplusplus
}
#endif

#endif /* CWAVE_GUI_H */
