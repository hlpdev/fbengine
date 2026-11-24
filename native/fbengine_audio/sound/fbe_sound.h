#ifndef FBE_SOUND_H
#define FBE_SOUND_H

// Sounds: a whole sound decoded into memory, at its own rate and channels, a streamed one, its
// file kept to decode as each channel plays it, for music and long sounds, or a fed one, its frames
// given as they come, for voices. A sound's handle is the
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
// A fed sound, at `sample_rate` Hz and of `channels` channels, which the game feeds its frames as
// they come, as a voice's from the network, and one channel plays, holding up to `room` frames
// unplayed (1 to 60 seconds' worth). Its channel plays what it's fed, and silence while it has
// none, never ending until it's stopped; once it runs dry it waits to have `buffered` frames (0 to
// `room`) before it plays on, so a feed that comes unevenly doesn't stutter. Its channel reads
// ahead what the mix makes at a time, its period, as every channel does, so what's fed after it's
// read silence plays once that's been heard. 0 if a value's out of range.
FBE_AUDIO_API uint64_t fbe_sound_open_feed(int32_t sample_rate, int32_t channels, int32_t room,
                                           int32_t buffered);
// Feeds the fed sound `frames` frames, its channels' samples each, one after another, from
// `samples`: how many it took, fewer when it's full, or -1 if it isn't a fed sound or `frames` is
// negative.
FBE_AUDIO_API int32_t fbe_sound_feed(uint64_t sound, const float* samples, int32_t frames);
// Whether fbe_sound_open_feed made it.
FBE_AUDIO_API bool fbe_sound_fed(uint64_t sound);
// How many frames it's been fed that haven't been played, 0 for a sound that isn't fed.
FBE_AUDIO_API int32_t fbe_sound_queued(uint64_t sound);
// Whether fbe_sound_stream made it.
FBE_AUDIO_API bool fbe_sound_streamed(uint64_t sound);
// Lets the sound go; the channels playing it play on. A sound that's gone already is nothing.
FBE_AUDIO_API void fbe_sound_free(uint64_t sound);
// How many sounds there are, those not freed.
FBE_AUDIO_API int32_t fbe_sound_count(void);
// The sound's frames a second, channels, and length in frames, 0 for a sound that's gone.
FBE_AUDIO_API int32_t fbe_sound_sample_rate(uint64_t sound);
FBE_AUDIO_API int32_t fbe_sound_channels(uint64_t sound);
FBE_AUDIO_API int64_t fbe_sound_frames(uint64_t sound);
// Plays the sound on a channel of its own, at `volume` (0 or more), `pan` (-1, the left, to 1, the
// right), and `pitch` (1/16 to 16, 2 an octave up and twice as fast), looping or once, paused
// until fbe_channel_resume if `paused`, fading in from silence over `fade_in` seconds (0 or more),
// into `bus`, or the whole mix for 0, from `emitter`, in the world, or 0 for a channel that isn't;
// the channel, or 0 if the audio's stopped, the sound, bus, or emitter is gone, a value's out of
// range, or it's a fed sound another channel plays.
FBE_AUDIO_API uint64_t fbe_sound_play(uint64_t sound, float volume, float pan, float pitch,
                                      bool loop, bool paused, double fade_in, uint64_t bus,
                                      uint64_t emitter);

#ifdef __cplusplus
}
#endif

#endif
