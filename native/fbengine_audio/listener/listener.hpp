#pragma once

// The listener, whose settings the mix is given as it starts.

#include <array>
#include <miniaudio.h>

namespace fbe::audio {

/// Gives the mix, started but not yet playing, the listener's settings.
void start_listener(ma_engine* mix);

/// What the mixing thread does before each mix: which way is up for the listener, if it changed,
/// which miniaudio keeps as plain floats it reads as it mixes.
void update_listener(ma_engine* mix);

/// Where the listener is and how fast it moves, and how fast sound goes, for the Doppler effect.
std::array<float, 3> listener_position();
std::array<float, 3> listener_velocity();
float speed_of_sound();

} // namespace fbe::audio
