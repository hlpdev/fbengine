#pragma once

// The channels playing, which fbe_sound_play starts and fbe_audio_stop ends, and what the thread
// that mixes does around each mix.

#include "bus/buses.hpp"
#include "emitter/emitters.hpp"
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
    /// Seconds it fades in over, from silence, or 0.
    double fade_in = 0.0;
    /// What it plays into, or null for the whole mix.
    std::shared_ptr<bus_t> bus;
    /// Where it plays from, or null for a channel that isn't in the world.
    const emitter_t* emitter = nullptr;
};

/// Plays `data` on a new channel, giving its handle, or 0, with fbe_audio_error's message, if the
/// audio's stopped or miniaudio can't.
std::uint64_t play(std::shared_ptr<const sound_data_t> data, const play_t& how);

/// The miniaudio sound of the channel `handle` is for, or null for one that's finished.
ma_sound* channel_sound(std::uint64_t handle);

/// Pitches the channel `handle` is for by `doppler` too; false for one that's finished.
bool set_doppler(std::uint64_t handle, float doppler);

/// The pitches a channel plays at, its own times its emitter's Doppler pitch held to them too.
constexpr float lowest_pitch = 1.0F / 16.0F;
constexpr float highest_pitch = 16.0F;

/// Ends every channel, before the mix they play in goes.
void end_channels();

/// What the mixing thread does just before and after each mix: the seeks channels asked for, and
/// where each has played to.
void before_mix();
void after_mix();

/// Whether each is in range, or false, with fbe_audio_error's message.
bool check_volume(float volume);
bool check_pan(float pan);
bool check_pitch(float pitch);

} // namespace fbe::audio
