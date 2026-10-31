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

/// The gamepads' part of input_event.
void gamepad_event(const SDL_Event& event);

/// The joysticks' part of input_event.
void joystick_event(const SDL_Event& event);

/// Starts SDL's gamepads and joysticks, the first time they're asked for; false, saying why, if
/// they can't start. Those connected already are read connecting at the next fbe_frame_flip.
bool start_gamepads();

/// Zeroes what the last Flip counted of the joysticks.
void flush_joysticks();

/// The window, for a call that needs one: null without one, with fbe_error saying so.
SDL_Window* needed_window();

/// Pushes an event as the OS would, for the next fbe_frame_flip; false, saying why, if it can't.
bool push_event(SDL_Event& event);

/// A gamepad's or a joystick's button: whether it's held, and what the last Flip counted of it.
struct button_t {
    bool down = false;
    std::int32_t hits = 0;
    std::int32_t ups = 0;

    /// Counts the button going down, unless it's held already.
    void press() {
        if (!down) {
            ++hits;
        }
        down = true;
    }

    /// Counts the button going up, if it was held.
    void release() {
        if (down) {
            ++ups;
        }
        down = false;
    }

    void zero() {
        hits = 0;
        ups = 0;
    }
};

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
