#ifndef FBENGINE_H
#define FBENGINE_H

// fbengine's C API, which packages/fbengine wraps, through the bindings fb-bindgen makes of this
// header. Every type here is the same size on every target, so the bindings are too. Everything
// but fbe_version and fbe_error is the main thread's, which the FB API's @MainThread keeps it to.

#include <stdbool.h>
#include <stdint.h>

#ifndef _WIN32
#define FBE_API __attribute__((visibility("default")))
#else
#ifdef FBE_BUILDING
#define FBE_API __declspec(dllexport)
#else
#define FBE_API __declspec(dllimport)
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// How the window fills the screen.
#define FBE_WINDOWED   0
#define FBE_FULLSCREEN 1
#define FBE_BORDERLESS 2

// A size a display can show, and how often it shows it, in hertz, 0 when it doesn't say. C has no
// `using`, so it's a typedef.
// NOLINTNEXTLINE(modernize-use-using)
typedef struct fbe_display_mode_t {
    int32_t width;
    int32_t height;
    float refresh_rate;
    float pixel_density;
} fbe_display_mode_t;

// The version of fbengine this DLL is, like "0.1.0", which is its packages'.
FBE_API const char* fbe_version(void);

// What the last call that failed on this thread says went wrong.
FBE_API const char* fbe_error(void);

// Whether windows are offscreen, never shown, which only works before the first window or
// display is asked for; false if it's too late.
FBE_API bool fbe_set_headless(bool headless);
FBE_API bool fbe_headless(void);

// Opens the window, or makes the open one this size and mode; false if it can't.
FBE_API bool fbe_graphics(int32_t width, int32_t height, int32_t mode);
// Closes the window, if there is one.
FBE_API void fbe_end_graphics(void);
FBE_API bool fbe_has_graphics(void);

// The window's title, which can be set before it's open.
FBE_API void fbe_set_title(const char* title);
FBE_API const char* fbe_title(void);

// The window's size in the desktop's units, and in pixels, which a high DPI display has more
// of; and how many pixels a unit is. 0 without a window.
FBE_API int32_t fbe_window_width(void);
FBE_API int32_t fbe_window_height(void);
FBE_API int32_t fbe_window_pixel_width(void);
FBE_API int32_t fbe_window_pixel_height(void);
FBE_API float fbe_window_scale(void);

// The window's mode, FBE_WINDOWED without a window; false if it can't take this one.
FBE_API bool fbe_set_window_mode(int32_t mode);
FBE_API int32_t fbe_window_mode(void);

// Whether the window can be resized, which can be set before it's open.
FBE_API void fbe_set_resizable(bool resizable);
FBE_API bool fbe_resizable(void);

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
