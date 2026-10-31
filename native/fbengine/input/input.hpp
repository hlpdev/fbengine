#pragma once

// What input's files share: the events fbe_frame_flip hands it, and how each device counts what a
// Flip read.

#include <cstdint>
#include <SDL3/SDL_events.h>

namespace fbe {

/// An event fbe_frame_flip read, handed to input, which hands it to each device.
void input_event(const SDL_Event& event);

/// The keyboard's part of input_event.
void keyboard_event(const SDL_Event& event);

/// The mouse's part of input_event.
void mouse_event(const SDL_Event& event);

/// The window, for a call that needs one: null without one, with fbe_error saying so.
SDL_Window* needed_window();

/// Pushes an event as the OS would, for the next fbe_frame_flip; false, saying why, if it can't.
bool push_event(SDL_Event& event);

/// What a device counted of one Flip's events: zeroed when the first event of a new Flip
/// arrives, and read as zero once a Flip went by with none.
class frame_counts_t {
public:
    /// Whether the counts are the Flip being read's; false the first time, so the device zeroes
    /// them and counts afresh.
    bool start();

    /// Whether the counts are what the last Flip read.
    [[nodiscard]] bool current() const;

private:
    std::int64_t m_frame = -1;
};

} // namespace fbe
