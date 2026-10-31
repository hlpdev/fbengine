#ifndef FBE_INPUT_H
#define FBE_INPUT_H

// Input: the keyboard and the mouse, as the last fbe_frame_flip read them.
//
// What's counted (presses, releases, text, motion, the wheel) is what the last fbe_frame_flip
// read, so every call in a frame agrees; what's held is as of then. A key is its position,
// SDL's scancode, which is USB HID's usage for most keys, so the key left of S is the same key
// on every layout. A mouse button is SDL's number: 1 left, 2 middle, 3 right, 4 and 5 the side
// buttons, to 32.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Whether the key was held when the last fbe_frame_flip read the OS's events. False for a key
// out of range.
FBE_API bool fbe_input_key_down(int32_t key);
// How many times the last fbe_frame_flip read the key pressed, leaving out the OS's repeats.
FBE_API int32_t fbe_input_key_hits(int32_t key);
// How many times it read the key released.
FBE_API int32_t fbe_input_key_ups(int32_t key);
// How many times it read the key go down, the OS's repeats while it's held included.
FBE_API int32_t fbe_input_key_typed(int32_t key);
// The keys the last fbe_frame_flip read pressed, in order, the first 64 of a frame.
FBE_API int32_t fbe_input_keys_hit_count(void);
// The `index`th of them, 0 if there's no such press.
FBE_API int32_t fbe_input_key_hit_at(int32_t index);
// Zeroes what the last fbe_frame_flip counted of the keyboard, and its text, until the next;
// keys held stay held.
FBE_API void fbe_input_flush_keys(void);
// The key's name on the keyboard's layout ("Q" for the key left of W on an AZERTY one), or its
// scancode's when the layout has none; "" for none.
FBE_API const char* fbe_input_key_name(int32_t key);

// Starts the window's text input, which the IME's composition joins; false without a window.
FBE_API bool fbe_input_start_text(void);
// Stops it, and drops the composition.
FBE_API void fbe_input_stop_text(void);
FBE_API bool fbe_input_text_active(void);
// Where the text being typed is, in the window's units, with the cursor `cursor` units from the
// area's left, so the IME's windows go beside it; false without a window.
FBE_API bool fbe_input_set_text_area(int32_t x, int32_t y, int32_t width, int32_t height,
                                     int32_t cursor);
// The text the last fbe_frame_flip read typed, in UTF-8, "" for none.
FBE_API const char* fbe_input_text(void);
// The IME's composition, the text not yet typed, as the last fbe_frame_flip read it, "" for
// none; its cursor and the length of its selection, in characters, -1 when the IME doesn't say.
FBE_API const char* fbe_input_composition(void);
FBE_API int32_t fbe_input_composition_start(void);
FBE_API int32_t fbe_input_composition_length(void);

// The mouse's position in the window's units, from its top left, as the last fbe_frame_flip read
// it or fbe_input_move_mouse put it; outside the window while a button's held, and 0 until it
// first moves.
FBE_API float fbe_input_mouse_x(void);
FBE_API float fbe_input_mouse_y(void);
// How far it moved by the motion the last fbe_frame_flip read, in the window's units, or in the
// mouse's own counts in relative mode; its own moves by fbe_input_move_mouse left out.
FBE_API float fbe_input_mouse_x_speed(void);
FBE_API float fbe_input_mouse_y_speed(void);
// How far the wheel turned by the last fbe_frame_flip, in notches, which a smooth wheel gives
// fractions of: up, away from the player, positive, and right positive, whatever the OS's
// natural scrolling; and in whole notches up, fractions kept for the next.
FBE_API float fbe_input_mouse_wheel(void);
FBE_API float fbe_input_mouse_wheel_x(void);
FBE_API int32_t fbe_input_mouse_wheel_steps(void);
// Whether the button was held when the last fbe_frame_flip read the OS's events; false for a
// button out of range.
FBE_API bool fbe_input_mouse_down(int32_t button);
// How many times the last fbe_frame_flip read the button pressed, and released.
FBE_API int32_t fbe_input_mouse_hits(int32_t button);
FBE_API int32_t fbe_input_mouse_ups(int32_t button);
// The most clicks in a row the OS counted of those presses: 1 for a click, 2 for a double click.
FBE_API int32_t fbe_input_mouse_clicks(int32_t button);
// Zeroes what the last fbe_frame_flip counted of the mouse, its presses, releases, motion, and
// wheel, until the next; buttons held stay held.
FBE_API void fbe_input_flush_mouse(void);
// Puts the mouse at `x`, `y` in the window's units; false without a window.
FBE_API bool fbe_input_move_mouse(float x, float y);
// Shows or hides the mouse's cursor.
FBE_API void fbe_input_set_cursor_visible(bool visible);
FBE_API bool fbe_input_cursor_visible(void);
// Relative mode, for mouse look: the cursor hidden and held in the window, and the mouse's
// motion its own raw counts, unscaled by the OS's speed and acceleration; false without a window.
// Headless, where there's no mouse to hold, it's only what was set.
FBE_API bool fbe_input_set_mouse_relative(bool relative);
FBE_API bool fbe_input_mouse_relative(void);

// Pushes the key going down (`repeat` for the OS's repeat while it's held) or up, as the OS
// would, for the next fbe_frame_flip to read; false without a window, or for a key out of range.
FBE_API bool fbe_input_simulate_key(int32_t key, bool down, bool repeat);
// Pushes text typed, which only counts while text input is on; false without a window.
FBE_API bool fbe_input_simulate_text(const char* text);
// Pushes the IME's composition; false without a window.
FBE_API bool fbe_input_simulate_composition(const char* text, int32_t start, int32_t length);
// Pushes the mouse moving to `x`, `y`, in the window's units; false without a window.
FBE_API bool fbe_input_simulate_mouse_move(float x, float y);
// Pushes the mouse moving by `x`, `y`, as relative mode's raw motion does; false without a
// window.
FBE_API bool fbe_input_simulate_mouse_motion(float x, float y);
// Pushes the button going down, the `clicks`th in a row, or up; false without a window, or for a
// button out of range.
FBE_API bool fbe_input_simulate_mouse_button(int32_t button, bool down, int32_t clicks);
// Pushes the wheel turning `y` notches up and `x` right; false without a window.
FBE_API bool fbe_input_simulate_mouse_wheel(float x, float y);

#ifdef __cplusplus
}
#endif

#endif
