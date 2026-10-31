#ifndef FBE_SOUND_H
#define FBE_SOUND_H

// Sounds: a whole sound decoded into memory, at its own rate and channels, or a streamed one, its
// file kept to decode as each channel plays it, for music and long sounds. A sound's handle is the
// game's until it frees it; the channels playing it keep what they play until they end.

#include "../audio/fbe_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Decodes a sound, WAV, FLAC, or Ogg Vorbis, told apart by its bytes, from the `size` bytes at
// `data`, which it doesn't keep; 0 if it can't, or it has no frames, or more than 8 channels.
FBE_AUDIO_API uint64_t fbe_sound_load(const uint8_t* data, int64_t size);
// Keeps a copy of the `size` bytes at `data`, a sound file as fbe_sound_load reads, which each
// channel playing it decodes as it plays; 0 if they aren't a sound, or it has no frames, or more
// than 8 channels.
FBE_AUDIO_API uint64_t fbe_sound_stream(const uint8_t* data, int64_t size);
// Whether fbe_sound_stream made it.
FBE_AUDIO_API bool fbe_sound_streamed(uint64_t sound);
// Lets the sound go; the channels playing it play on. A sound that's gone already is nothing.
FBE_AUDIO_API void fbe_sound_free(uint64_t sound);
// The sound's frames a second, channels, and length in frames, 0 for a sound that's gone.
FBE_AUDIO_API int32_t fbe_sound_sample_rate(uint64_t sound);
FBE_AUDIO_API int32_t fbe_sound_channels(uint64_t sound);
FBE_AUDIO_API int64_t fbe_sound_frames(uint64_t sound);
// Plays the sound on a channel of its own, at `volume` (0 or more), `pan` (-1, the left, to 1, the
// right), and `pitch` (1/16 to 16, 2 an octave up and twice as fast), looping or once, paused
// until fbe_channel_resume if `paused`, fading in from silence over `fade_in` seconds (0 or more);
// the channel, or 0 if the audio's stopped, the sound's gone, or a value's out of range.
FBE_AUDIO_API uint64_t fbe_sound_play(uint64_t sound, float volume, float pan, float pitch,
                                      bool loop, bool paused, double fade_in);

#ifdef __cplusplus
}
#endif

#endif
