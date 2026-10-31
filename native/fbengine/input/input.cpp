// Input: fbe_frame_flip hands it each event it reads, which goes to each device's state.

#include "input/input.hpp"

#include "core/engine.hpp"
#include "fbengine.h"

#include <SDL3/SDL.h>

namespace fbe {

void input_event(const SDL_Event& event) {
    keyboard_event(event);
    mouse_event(event);
    gamepad_event(event);
    joystick_event(event);
}

SDL_Window* needed_window() {
    SDL_Window* open = window();
    if (open == nullptr) {
        fail("there's no window; Graphics opens one");
    }
    return open;
}

bool push_event(SDL_Event& event) {
    return SDL_PushEvent(&event) ? true : fail_sdl("couldn't push the event");
}

bool frame_counts_t::start() {
    // fbe_frame_flip counts its frame once it has read the events, so they're the next one's.
    const std::int64_t reading = fbe_frame_count() + 1;
    if (m_frame == reading) {
        return true;
    }
    m_frame = reading;
    return false;
}

bool frame_counts_t::current() const {
    return m_frame == fbe_frame_count();
}

} // namespace fbe
