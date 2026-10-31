#pragma once

// What input's files share: the events fbe_frame_flip hands it, the window it reads them for,
// and how each device counts what a Flip read.

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>
#include <cstdint>

namespace fbe {

/// An event fbe_frame_flip read, handed to input, which hands it to each device.
void input_event(const SDL_Event& event);

/// The keyboard's part of input_event.
void keyboard_event(const SDL_Event& event);

/// The window fbengine opened, or null.
SDL_Window* input_window();

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
