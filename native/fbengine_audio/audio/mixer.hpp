#pragma once

// What fbengine_audio_native's own files share: its errors, and the mix, miniaudio's engine,
// which fbe_audio_start makes.

#include <miniaudio.h>
#include <string_view>

namespace fbe::audio {

/// Keeps `message` for fbe_audio_error(), and returns false, for a call that failed.
bool fail(std::string_view message);

/// miniaudio's last result, for fbe_audio_error(), with what was being done, and returns false.
bool fail_ma(std::string_view doing, ma_result result);

/// The mix, or null while it's stopped.
ma_engine* engine();

} // namespace fbe::audio
