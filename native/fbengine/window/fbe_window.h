#ifndef FBE_WINDOW_H
#define FBE_WINDOW_H

// The window and the displays.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// How the window fills the screen.
#define FBE_WINDOW_WINDOWED   0
#define FBE_WINDOW_FULLSCREEN 1
#define FBE_WINDOW_BORDERLESS 2

// Window events fbe_window_simulate pushes, as the OS would, for the next fbe_frame_flip to read.
#define FBE_WINDOW_SIMULATE_CLOSE    0
#define FBE_WINDOW_SIMULATE_FOCUS    1
#define FBE_WINDOW_SIMULATE_BLUR     2
#define FBE_WINDOW_SIMULATE_MINIMIZE 3
#define FBE_WINDOW_SIMULATE_RESTORE  4

// A size a display can show, and how often it shows it, in hertz, 0 when it doesn't say. C has no
// `using`, so it's a typedef.
// NOLINTNEXTLINE(modernize-use-using)
typedef struct fbe_display_mode_t {
    int32_t width;
    int32_t height;
    float refresh_rate;
    float pixel_density;
} fbe_display_mode_t;

// Whether windows are offscreen, never shown, which only works before the first window or
// display is asked for; false if it's too late.
FBE_API bool fbe_window_set_headless(bool headless);
FBE_API bool fbe_window_headless(void);

// Opens the window, or makes the open one this size and mode; false if it can't.
FBE_API bool fbe_window_open(int32_t width, int32_t height, int32_t mode);
// Closes the window, if there is one.
FBE_API void fbe_window_close(void);
FBE_API bool fbe_window_is_open(void);

// The window's title, which can be set before it's open.
FBE_API void fbe_window_set_title(const char* title);
FBE_API const char* fbe_window_title(void);

// The window's size in the desktop's units, and in pixels, which a high DPI display has more
// of; and how many pixels a unit is. 0 without a window.
FBE_API int32_t fbe_window_width(void);
FBE_API int32_t fbe_window_height(void);
FBE_API int32_t fbe_window_pixel_width(void);
FBE_API int32_t fbe_window_pixel_height(void);
FBE_API float fbe_window_scale(void);

// The window's mode, FBE_WINDOW_WINDOWED without a window; false if it can't take this one.
FBE_API bool fbe_window_set_mode(int32_t mode);
FBE_API int32_t fbe_window_mode(void);

// Where the window's top left is on the desktop, in its units, 0 without a window.
FBE_API int32_t fbe_window_x(void);
FBE_API int32_t fbe_window_y(void);

// Moves the window, if the desktop lets it, or sizes a windowed one, more than 0 each way, in the
// desktop's units; false, saying why, without a window, for a fullscreen one's size, or if the OS
// won't size it.
FBE_API bool fbe_window_set_position(int32_t x, int32_t y);
FBE_API bool fbe_window_set_size(int32_t width, int32_t height);

// Whether a window there, that big, would be on a display, at least a little of it.
FBE_API bool fbe_window_place_shows(int32_t x, int32_t y, int32_t width, int32_t height);

// The least the window can be sized to, by a person or by a call, in the desktop's units, 0 for no
// least; false, saying why, without a window, or if the OS refuses.
FBE_API bool fbe_window_set_minimum_size(int32_t width, int32_t height);
FBE_API int32_t fbe_window_minimum_width(void);
FBE_API int32_t fbe_window_minimum_height(void);

// Whether the window can be resized, which can be set before it's open.
FBE_API void fbe_window_set_resizable(bool resizable);
FBE_API bool fbe_window_resizable(void);

// What the OS's events said of the window by the last fbe_frame_flip: that its close button was
// pressed, or the app was asked to quit, which stays so until the window's closed; whether it has
// the keyboard's focus; whether it's minimized. All false without a window.
FBE_API bool fbe_window_closed(void);
FBE_API bool fbe_window_focused(void);
FBE_API bool fbe_window_minimized(void);

// Pushes one of the FBE_WINDOW_SIMULATE_ events for the open window; false without one.
FBE_API bool fbe_window_simulate(int32_t event);

// The displays, the first being the main one; -1 if they can't be found.
FBE_API int32_t fbe_display_count(void);
// A display's name, "" if there's no such display.
FBE_API const char* fbe_display_name(int32_t display);
// The mode its desktop is in; false if there's no such display.
FBE_API bool fbe_display_desktop(int32_t display, fbe_display_mode_t* mode);
// The modes it has for fullscreen, largest first; -1 if there's no such display.
FBE_API int32_t fbe_display_mode_count(int32_t display);
FBE_API bool fbe_display_mode_at(int32_t display, int32_t index, fbe_display_mode_t* mode);

#ifdef __cplusplus
}
#endif

#endif
