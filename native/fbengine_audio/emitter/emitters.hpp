#pragma once

// Emitters, which channels play from: fbe_sound_play places a channel at one before it starts, and
// the emitter moves it after.

#include <cstdint>
#include <miniaudio.h>

namespace fbe::audio {

struct emitter_t;

/// The emitter `handle` is for, or null for 0 or one that's gone.
emitter_t* find_emitter(std::uint64_t handle);

/// Gives `sound` `emitter`'s place and falloff, and gives the Doppler pitch it plays at.
float place(const emitter_t& emitter, ma_sound& sound);

/// Makes room for one more of `emitter`'s channels, so join can't run out of memory: before a
/// channel plays, which can.
void make_room(emitter_t& emitter);

/// Makes `channel` one of `emitter`'s, which move with it.
void join(emitter_t& emitter, std::uint64_t channel);

/// Gives every emitter's channels their Doppler pitch again, as the listener moves.
void pitch_every_emitter();

} // namespace fbe::audio
