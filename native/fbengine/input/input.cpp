// Input: fbe_frame_flip hands it each event it reads, which goes to each device's state.

#include "input/input.hpp"

#include "fbengine.h"

#include <SDL3/SDL.h>

namespace fbe {

void input_event(const SDL_Event& event) {
    keyboard_event(event);
}

SDL_Window* input_window() {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    SDL_Window* window = windows != nullptr && count > 0 ? windows[0] : nullptr;
    SDL_free(static_cast<void*>(windows));
    return window;
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
