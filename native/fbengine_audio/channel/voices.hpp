#pragma once

// The channels playing, which fbe_sound_play starts and fbe_audio_stop ends.

#include "sound/sound.hpp"

#include <cstdint>
#include <memory>

namespace fbe::audio {

/// How a channel starts.
struct play_t {
    float volume = 1.0F;
    float pan = 0.0F;
    float pitch = 1.0F;
    bool loop = false;
    bool paused = false;
};

/// Plays `data` on a new channel, giving its handle, or 0, with fbe_audio_error's message, if the
/// audio's stopped or miniaudio can't.
std::uint64_t play(std::shared_ptr<const sound_data_t> data, const play_t& how);

/// Ends every channel, before the mix they play in goes.
void end_channels();

/// Whether each is in range, or false, with fbe_audio_error's message.
bool check_volume(float volume);
bool check_pan(float pan);
bool check_pitch(float pitch);

} // namespace fbe::audio
