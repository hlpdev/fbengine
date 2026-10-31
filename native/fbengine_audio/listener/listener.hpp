#pragma once

// The listener, whose settings the mix is given as it starts.

#include <miniaudio.h>

namespace fbe::audio {

/// Gives the mix, started but not yet playing, the listener's settings.
void start_listener(ma_engine* mix);

/// What the mixing thread does before each mix: the speed of sound, if it changed.
void update_listener(ma_engine* mix);

} // namespace fbe::audio
