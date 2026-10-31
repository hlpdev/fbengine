// The mouse, as the events fbe_frame_flip reads say: where it is, how it moved, its wheel and
// buttons; and Simulate's mouse.

#include "core/engine.hpp"
#include "fbengine.h"
#include "input/input.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <SDL3/SDL.h>

namespace {

// SDL numbers buttons from 1, to 32.
constexpr std::int32_t most_buttons = 32;

// A button: whether it's held, and what the last Flip counted of it.
struct button_t {
    bool down = false;
    std::int32_t hits = 0;
    std::int32_t ups = 0;
    std::int32_t clicks = 0;
};

std::array<button_t, most_buttons + 1> buttons{};
fbe::frame_counts_t counts;

// Where the mouse is, which stays until it moves, in the window `in`, by its ID: one closed is
// no one's, and a window opened after it has the mouse nowhere until it moves.
float x = 0.0F;
float y = 0.0F;
SDL_WindowID in = 0;

// What the last Flip read of its motion and its wheel.
float x_speed = 0.0F;
float y_speed = 0.0F;
float wheel = 0.0F;
float wheel_x = 0.0F;
std::int32_t wheel_steps = 0;
// The wheel's turn not yet a whole notch, which the next turn adds to.
float wheel_part = 0.0F;

// Where Simulate last put the mouse, or the events last read did, which its next motion is from,
// in the window `pushed_in`.
float pushed_x = 0.0F;
float pushed_y = 0.0F;
SDL_WindowID pushed_in = 0;

/// The open window's ID, 0 without one.
SDL_WindowID open_window() {
    SDL_Window* window = fbe::window();
    return window != nullptr ? SDL_GetWindowID(window) : 0;
}

// Headless, the offscreen driver has no relative mode, so it's only this: the window that's in
// it, by its ID, which a window opened later never has.
SDL_WindowID headless_relative = 0;

bool valid_button(std::int32_t button) {
    return button >= 1 && button <= most_buttons;
}

/// A valid button's state.
button_t& state_of(std::int32_t button) {
    return buttons[static_cast<std::size_t>(button)];
}

void zero_counts() {
    for (button_t& button : buttons) {
        button.hits = 0;
        button.ups = 0;
        button.clicks = 0;
    }
    x_speed = 0.0F;
    y_speed = 0.0F;
    wheel = 0.0F;
    wheel_x = 0.0F;
    wheel_steps = 0;
}

/// Zeroes what was counted when this is the first event of a Flip.
void count() {
    if (!counts.start()) {
        zero_counts();
    }
}

void motion_event(const SDL_MouseMotionEvent& event) {
    // A closed window's motion, read after it closed, isn't the open one's.
    if (event.windowID == 0 || event.windowID != open_window()) {
        return;
    }
    count();
    x = event.x;
    y = event.y;
    in = event.windowID;
    pushed_x = x;
    pushed_y = y;
    pushed_in = in;
    x_speed += event.xrel;
    y_speed += event.yrel;
}

void button_event(const SDL_MouseButtonEvent& event) {
    if (!valid_button(event.button)) {
        return;
    }
    count();
    button_t& button = state_of(event.button);
    if (event.down) {
        if (!button.down) {
            ++button.hits;
            button.clicks = std::max(button.clicks, static_cast<std::int32_t>(event.clicks));
        }
        button.down = true;
    } else if (button.down) {
        ++button.ups;
        button.down = false;
    }
}

void wheel_event(const SDL_MouseWheelEvent& event) {
    count();
    // A flipped wheel is the OS's natural scrolling, which turns the other way.
    const float turned = event.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0F : 1.0F;
    wheel += event.y * turned;
    wheel_x += event.x * turned;
    // A turn the other way starts afresh, so a leftover fraction never cancels it.
    if (wheel_part * event.y * turned < 0.0F) {
        wheel_part = 0.0F;
    }
    wheel_part += event.y * turned;
    const float whole = std::trunc(wheel_part);
    wheel_steps += static_cast<std::int32_t>(whole);
    wheel_part -= whole;
}

/// Lets go of every button, as a window that loses the focus, or closes, may never hear them go
/// up.
void release_buttons() {
    count();
    for (button_t& button : buttons) {
        if (button.down) {
            ++button.ups;
            button.down = false;
        }
    }
}

/// A motion event at `to_x`, `to_y` that moved by `by_x`, `by_y`, for Simulate.
bool push_motion(float to_x, float to_y, float by_x, float by_y) {
    SDL_Window* window = fbe::needed_window();
    if (window == nullptr) {
        return false;
    }
    SDL_Event pushed{};
    pushed.type = SDL_EVENT_MOUSE_MOTION;
    pushed.motion.timestamp = SDL_GetTicksNS();
    pushed.motion.windowID = SDL_GetWindowID(window);
    pushed.motion.x = to_x;
    pushed.motion.y = to_y;
    pushed.motion.xrel = by_x;
    pushed.motion.yrel = by_y;
    if (!fbe::push_event(pushed)) {
        return false;
    }
    pushed_x = to_x;
    pushed_y = to_y;
    pushed_in = pushed.motion.windowID;
    return true;
}

/// Where Simulate's next motion is from: where it or the events last put the mouse in the open
/// window, or its corner, 0, in a window it's never been in.
float pushed_from_x() {
    return pushed_in == open_window() ? pushed_x : 0.0F;
}

float pushed_from_y() {
    return pushed_in == open_window() ? pushed_y : 0.0F;
}

} // namespace

namespace fbe {

void mouse_event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        motion_event(event.motion);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        button_event(event.button);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        wheel_event(event.wheel);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_DESTROYED:
        release_buttons();
        break;
    default:
        break;
    }
}

} // namespace fbe

extern "C" {

FBE_API float fbe_input_mouse_x(void) {
    return in == open_window() ? x : 0.0F;
}

FBE_API float fbe_input_mouse_y(void) {
    return in == open_window() ? y : 0.0F;
}

FBE_API float fbe_input_mouse_x_speed(void) {
    return counts.current() ? x_speed : 0.0F;
}

FBE_API float fbe_input_mouse_y_speed(void) {
    return counts.current() ? y_speed : 0.0F;
}

FBE_API float fbe_input_mouse_wheel(void) {
    return counts.current() ? wheel : 0.0F;
}

FBE_API float fbe_input_mouse_wheel_x(void) {
    return counts.current() ? wheel_x : 0.0F;
}

FBE_API int32_t fbe_input_mouse_wheel_steps(void) {
    return counts.current() ? wheel_steps : 0;
}

FBE_API bool fbe_input_mouse_down(int32_t button) {
    return valid_button(button) && state_of(button).down;
}

FBE_API int32_t fbe_input_mouse_hits(int32_t button) {
    return valid_button(button) && counts.current() ? state_of(button).hits : 0;
}

FBE_API int32_t fbe_input_mouse_ups(int32_t button) {
    return valid_button(button) && counts.current() ? state_of(button).ups : 0;
}

FBE_API int32_t fbe_input_mouse_clicks(int32_t button) {
    return valid_button(button) && counts.current() ? state_of(button).clicks : 0;
}

FBE_API void fbe_input_flush_mouse(void) {
    zero_counts();
    wheel_part = 0.0F;
}

FBE_API bool fbe_input_move_mouse(float to_x, float to_y) {
    SDL_Window* window = fbe::needed_window();
    if (window == nullptr) {
        return false;
    }
    // SDL's own motion for the move says it moved by nothing, so it's no speed.
    SDL_WarpMouseInWindow(window, to_x, to_y);
    x = to_x;
    y = to_y;
    in = SDL_GetWindowID(window);
    pushed_x = to_x;
    pushed_y = to_y;
    pushed_in = in;
    return true;
}

FBE_API void fbe_input_set_cursor_visible(bool visible) {
    // The cursor is SDL's video's, which starts here when nothing has started it, as Displays
    // does; if it can't, Graphics says why.
    if (!fbe::start_video()) {
        return;
    }
    if (visible) {
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
}

FBE_API bool fbe_input_cursor_visible(void) {
    return !fbe::video_started() || SDL_CursorVisible();
}

FBE_API bool fbe_input_set_mouse_relative(bool relative) {
    SDL_Window* window = fbe::needed_window();
    if (window == nullptr) {
        return false;
    }
    if (fbe_window_headless()) {
        headless_relative = relative ? SDL_GetWindowID(window) : 0;
        return true;
    }
    return SDL_SetWindowRelativeMouseMode(window, relative)
               ? true
               : fbe::fail_sdl("couldn't change the mouse's relative mode");
}

FBE_API bool fbe_input_mouse_relative(void) {
    SDL_Window* window = fbe::window();
    if (window == nullptr) {
        return false;
    }
    return fbe_window_headless() ? headless_relative == SDL_GetWindowID(window)
                                 : SDL_GetWindowRelativeMouseMode(window);
}

FBE_API bool fbe_input_simulate_mouse_move(float to_x, float to_y) {
    return push_motion(to_x, to_y, to_x - pushed_from_x(), to_y - pushed_from_y());
}

FBE_API bool fbe_input_simulate_mouse_motion(float by_x, float by_y) {
    return push_motion(pushed_from_x() + by_x, pushed_from_y() + by_y, by_x, by_y);
}

FBE_API bool fbe_input_simulate_mouse_button(int32_t button, bool down, int32_t clicks) {
    if (!valid_button(button)) {
        return fbe::fail(std::format("{} isn't a MouseButton", button));
    }
    SDL_Window* window = fbe::needed_window();
    if (window == nullptr) {
        return false;
    }
    SDL_Event pushed{};
    pushed.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    pushed.button.timestamp = SDL_GetTicksNS();
    pushed.button.windowID = SDL_GetWindowID(window);
    pushed.button.button = static_cast<Uint8>(button);
    pushed.button.down = down;
    pushed.button.clicks = static_cast<Uint8>(std::clamp(clicks, 1, 255));
    pushed.button.x = x;
    pushed.button.y = y;
    return fbe::push_event(pushed);
}

FBE_API bool fbe_input_simulate_mouse_wheel(float by_x, float by_y) {
    SDL_Window* window = fbe::needed_window();
    if (window == nullptr) {
        return false;
    }
    SDL_Event pushed{};
    pushed.type = SDL_EVENT_MOUSE_WHEEL;
    pushed.wheel.timestamp = SDL_GetTicksNS();
    pushed.wheel.windowID = SDL_GetWindowID(window);
    pushed.wheel.x = by_x;
    pushed.wheel.y = by_y;
    pushed.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    pushed.wheel.mouse_x = x;
    pushed.wheel.mouse_y = y;
    return fbe::push_event(pushed);
}

} // extern "C"
