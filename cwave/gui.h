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
/* True if the mouse is over ANY ImGui window (menu bar, its open dropdown,
 * a dialog, or the transport panel).  Used to decide whether a click belongs
 * to the raw-GL waveform / overview: a click that merely closes an open menu
 * (over empty wave space) is NOT over a window, so we can act on it the same
 * frame instead of swallowing it. */
int  gui_any_window_hovered( void );

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
/* Set the next window's size only when it (re)appears, so the user can then
 * resize it and the manual size sticks for the rest of the session. */
void gui_set_next_window_size_appearing( float w, float h );
/* Read the current window's on-screen size (call between begin/end). */
void gui_get_window_size( float *w, float *h );
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
int  gui_input_int( const char *label, int *v );
/* Dropdown over 'count' string items; *idx is the selected item.  Returns 1
 * if the selection changed this frame. */
int  gui_combo( const char *label, int *idx,
                const char *const items[], int count );
int  gui_input_text( const char *label, char *buf, int buflen );
int  gui_checkbox( const char *label, int *v );
int  gui_selectable( const char *label, int selected );
float gui_content_avail_h( void );
void gui_progress_bar( float fraction, const char *overlay );

/* ---- tab bar (open documents) ---- */
/* Begin a tab bar with the given str id.  Returns 1 if it is visible (call
 * gui_end_tab_bar only when this returned 1). */
int  gui_begin_tab_bar( const char *id );
void gui_end_tab_bar( void );
/* One tab.  Returns 1 when this tab is the active (selected) one this frame.
 * If p_open is non-NULL a small close button is shown and *p_open is set to 0
 * when the user clicks it.  setSelected!=0 forces this tab selected this frame
 * (used to focus a newly created / opened document). */
int  gui_tab_item( const char *label, int *p_open, int setSelected );

/* ---- popups / modals ---- */
void gui_open_popup( const char *id );
int  gui_begin_popup_modal( const char *id );
/* Like gui_begin_popup_modal but the window is user-resizable (no auto-fit),
 * for dialogs whose size the user may want to change (e.g. the file browser). */
int  gui_begin_popup_modal_resizable( const char *id );
void gui_end_popup( void );
void gui_close_current_popup( void );

#ifdef __cplusplus
}
#endif

#endif /* CWAVE_GUI_H */
