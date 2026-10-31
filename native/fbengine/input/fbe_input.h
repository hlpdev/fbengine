#ifndef FBE_INPUT_H
#define FBE_INPUT_H

// Input: the keyboard, the mouse, gamepads, and joysticks, as the last fbe_frame_flip read them.
//
// What's counted (presses, releases, text, motion, the wheel) is what the last fbe_frame_flip
// read, so every call in a frame agrees; what's held is as of then. A key is its position,
// SDL's scancode, which is USB HID's usage for most keys, so the key left of S is the same key
// on every layout. A mouse button is SDL's number: 1 left, 2 middle, 3 right, 4 and 5 the side
// buttons, to 32. A gamepad or a joystick is its handle, SDL's instance ID, which no device
// connected later has; its buttons and axes are SDL's.

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
// scancode's when the layout has none; "" for none. It starts SDL's video, which knows the
// layout, if nothing has.
FBE_API const char* fbe_input_key_name(int32_t key);

// Where the text being typed is in the window, and its cursor, for the IME's windows.
// C has no `using`, so it's a typedef.
// NOLINTNEXTLINE(modernize-use-using)
typedef struct fbe_text_area_t {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int32_t cursor;
} fbe_text_area_t;

// Whether the game draws the IME's composition itself, at its text's cursor, rather than the OS
// drawing it over the window, which is what makes fbe_input_composition say it on Windows and
// X11. It's set before the first window or display; false if it's too late.
FBE_API bool fbe_input_set_draws_composition(bool draws);
FBE_API bool fbe_input_draws_composition(void);

// Starts the window's text input, which the IME's composition joins; false without a window.
FBE_API bool fbe_input_start_text(void);
// Stops it, and drops the composition.
FBE_API void fbe_input_stop_text(void);
FBE_API bool fbe_input_text_active(void);
// Where the text being typed is, in the window's units, with the cursor `cursor` units from the
// area's left, so the IME's windows go beside it; false without a window.
FBE_API bool fbe_input_set_text_area(int32_t x, int32_t y, int32_t width, int32_t height,
                                     int32_t cursor);
// Where fbe_input_set_text_area last put it, all 0 at first; false without a window.
FBE_API bool fbe_input_text_area(fbe_text_area_t* area);
// The text the last fbe_frame_flip read typed, in UTF-8, "" for none.
FBE_API const char* fbe_input_text(void);
// The IME's composition, the text not yet typed, as the last fbe_frame_flip read it, "" for
// none, and always "" unless fbe_input_set_draws_composition said the game draws it; its cursor and
// the length of its selection, in characters, -1 when the IME doesn't say.
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

// The gamepads connected, as the last fbe_frame_flip read them, in the order they connected. The
// first call here, or to the connecting or disconnecting ones, starts SDL's gamepads, which takes
// a moment; those connected already connect at the next fbe_frame_flip, as any other does. -1 if
// they can't start.
FBE_API int32_t fbe_input_gamepad_count(void);
// The `index`th of them, 0 if there's no such gamepad.
FBE_API uint64_t fbe_input_gamepad_at(int32_t index);
// The gamepads the last fbe_frame_flip read connecting, and disconnecting, in order; 0 if they
// can't start.
FBE_API int32_t fbe_input_gamepads_added_count(void);
FBE_API uint64_t fbe_input_gamepad_added_at(int32_t index);
FBE_API int32_t fbe_input_gamepads_removed_count(void);
FBE_API uint64_t fbe_input_gamepad_removed_at(int32_t index);
// Whether the gamepad's connected, as of the last fbe_frame_flip; everything else about one
// that isn't is "", false, or 0.
FBE_API bool fbe_input_gamepad_connected(uint64_t pad);
FBE_API const char* fbe_input_gamepad_name(uint64_t pad);
// SDL's SDL_GamepadType for it: Xbox, PlayStation, Switch, and the rest.
FBE_API int32_t fbe_input_gamepad_type(uint64_t pad);
// What a pad that connects again can be known by: its GUID, its kind's (32 hex digits), and its
// serial number, "" when it has none.
FBE_API const char* fbe_input_gamepad_guid(uint64_t pad);
FBE_API const char* fbe_input_gamepad_serial(uint64_t pad);
// The player SDL and the pad's lights say it's for, from 0, -1 for none; false if it isn't
// connected, or for an index under -1.
FBE_API int32_t fbe_input_gamepad_player_index(uint64_t pad);
FBE_API bool fbe_input_gamepad_set_player_index(uint64_t pad, int32_t index);
// Whether the button was held when the last fbe_frame_flip read the OS's events; how many times
// it read it pressed, and released.
FBE_API bool fbe_input_gamepad_down(uint64_t pad, int32_t button);
FBE_API int32_t fbe_input_gamepad_hits(uint64_t pad, int32_t button);
FBE_API int32_t fbe_input_gamepad_ups(uint64_t pad, int32_t button);
// The axis as the last fbe_frame_flip read it: a stick's -1 to 1, right and down positive, a
// trigger's 0 to 1. Past the dead zones, a stick's two axes together, so it's 0 inside and goes
// on smoothly from there; raw, as the gamepad says.
FBE_API float fbe_input_gamepad_axis(uint64_t pad, int32_t axis);
FBE_API float fbe_input_gamepad_raw_axis(uint64_t pad, int32_t axis);
// Every gamepad's dead zones, from 0 to under 1: how far a stick goes, and a trigger, before its
// axis isn't 0; false if they're out of range.
FBE_API bool fbe_input_set_gamepad_dead_zones(float stick, float trigger);
FBE_API float fbe_input_gamepad_stick_dead_zone(void);
FBE_API float fbe_input_gamepad_trigger_dead_zone(void);
// Rumbles the gamepad's low and high frequency motors, or its triggers' motors, from 0 to 1, for
// `seconds`, at least a millisecond and at most 65.535 seconds, replacing the rumble before; 0
// seconds stops it. False if it can't, or for a time under 0 or NaN.
FBE_API bool fbe_input_gamepad_rumble(uint64_t pad, float low, float high, double seconds);
FBE_API bool fbe_input_gamepad_rumble_triggers(uint64_t pad, float left, float right,
                                               double seconds);
// Zeroes what the last fbe_frame_flip counted of every gamepad and joystick, their presses,
// releases, and connections, until the next; buttons held stay held.
FBE_API void fbe_input_flush_gamepads(void);

// Every joystick connected, gamepads too, as the last fbe_frame_flip read them, in the order
// they connected, unmapped: wheels, flight sticks, and pads SDL has no mapping for. The first call
// starts SDL's joysticks, as fbe_input_gamepad_count's does; -1 if they can't start.
FBE_API int32_t fbe_input_joystick_count(void);
FBE_API uint64_t fbe_input_joystick_at(int32_t index);
FBE_API int32_t fbe_input_joysticks_added_count(void);
FBE_API uint64_t fbe_input_joystick_added_at(int32_t index);
FBE_API int32_t fbe_input_joysticks_removed_count(void);
FBE_API uint64_t fbe_input_joystick_removed_at(int32_t index);
FBE_API bool fbe_input_joystick_connected(uint64_t stick);
FBE_API const char* fbe_input_joystick_name(uint64_t stick);
// Its GUID, 32 hex digits, and its serial number, "" when it has none.
FBE_API const char* fbe_input_joystick_guid(uint64_t stick);
FBE_API const char* fbe_input_joystick_serial(uint64_t stick);
// SDL's SDL_JoystickType for it: a gamepad, a wheel, a flight stick, and the rest.
FBE_API int32_t fbe_input_joystick_type(uint64_t stick);
// Whether SDL maps it as a gamepad, which fbe_input_gamepad_ takes too.
FBE_API bool fbe_input_joystick_is_gamepad(uint64_t stick);
FBE_API int32_t fbe_input_joystick_axis_count(uint64_t stick);
FBE_API int32_t fbe_input_joystick_button_count(uint64_t stick);
FBE_API int32_t fbe_input_joystick_hat_count(uint64_t stick);
// The `axis`th axis, -1 to 1, with no dead zone.
FBE_API float fbe_input_joystick_axis(uint64_t stick, int32_t axis);
FBE_API bool fbe_input_joystick_down(uint64_t stick, int32_t button);
FBE_API int32_t fbe_input_joystick_hits(uint64_t stick, int32_t button);
FBE_API int32_t fbe_input_joystick_ups(uint64_t stick, int32_t button);
// The `hat`th hat's direction, SDL's SDL_HAT_ bits: 1 up, 2 right, 4 down, 8 left, 0 centered.
FBE_API int32_t fbe_input_joystick_hat(uint64_t stick, int32_t hat);

// SDL's hint `name`, "" when it's not set, for tests of what fbengine sets.
FBE_API const char* fbe_input_sdl_hint(const char* name);

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
// Connects one of SDL's virtual devices, a gamepad with every button and axis SDL's have, which
// can rumble if `rumbles`, or a joystick of the SDL_JoystickType `type` with these many axes,
// buttons, and hats; its handle, which is the device's too, or 0 if it can't.
FBE_API uint64_t fbe_input_simulate_gamepad(const char* name, bool rumbles);
FBE_API uint64_t fbe_input_simulate_joystick(const char* name, int32_t type, int32_t axes,
                                             int32_t buttons, int32_t hats);
// Disconnects a virtual device; false if there's no such device.
FBE_API bool fbe_input_simulate_unplug(uint64_t device);
// Sets a virtual device's button, its axis (a gamepad's trigger 0 to 1, any other -1 to 1), or
// its hat, which SDL reads as the next fbe_frame_flip does; false if there's no such device or
// no such button, axis, or hat.
FBE_API bool fbe_input_simulate_device_button(uint64_t device, int32_t button, bool down);
FBE_API bool fbe_input_simulate_device_axis(uint64_t device, int32_t axis, float value);
FBE_API bool fbe_input_simulate_device_hat(uint64_t device, int32_t hat, int32_t value);
// How hard SDL last told a virtual device's motor to rumble, 0 to 1: 0 low frequency, 1 high, 2
// the left trigger's, 3 the right's; 0 if there's no such device or motor.
FBE_API float fbe_input_simulate_device_rumble(uint64_t device, int32_t motor);

#ifdef __cplusplus
}
#endif

#endif
