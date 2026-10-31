#ifndef FBE_CHANNEL_H
#define FBE_CHANNEL_H

// Channels: one play of a sound each. A channel is finished once its sound ends or it's stopped,
// and a finished channel's calls do nothing, and say 0 or false, as 0's do. The setters say false
// only for a value out of range, finished or not.

#include "../audio/fbe_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Whether the channel hasn't finished; a paused channel is playing.
FBE_AUDIO_API bool fbe_channel_playing(uint64_t channel);
FBE_AUDIO_API bool fbe_channel_paused(uint64_t channel);
// Stops the channel, which is finished after.
FBE_AUDIO_API void fbe_channel_stop(uint64_t channel);
FBE_AUDIO_API void fbe_channel_pause(uint64_t channel);
FBE_AUDIO_API void fbe_channel_resume(uint64_t channel);
// 0 or more.
FBE_AUDIO_API bool fbe_channel_set_volume(uint64_t channel, float volume);
FBE_AUDIO_API float fbe_channel_volume(uint64_t channel);
// -1, the left, to 1, the right, in a stereo mix; in a mix that isn't stereo, a pan does nothing.
// A change of volume or pan is heard ramped over the next stretch the mix makes, at most 512
// frames, never half applied.
FBE_AUDIO_API bool fbe_channel_set_pan(uint64_t channel, float pan);
FBE_AUDIO_API float fbe_channel_pan(uint64_t channel);
// 1/16 to 16.
FBE_AUDIO_API bool fbe_channel_set_pitch(uint64_t channel, float pitch);
FBE_AUDIO_API float fbe_channel_pitch(uint64_t channel);
FBE_AUDIO_API void fbe_channel_set_loop(uint64_t channel, bool loop);
FBE_AUDIO_API bool fbe_channel_looping(uint64_t channel);
// How many seconds into its sound the channel has been mixed to, as of the last mix.
FBE_AUDIO_API double fbe_channel_position(uint64_t channel);
// Plays it on from `seconds` into its sound, 0 to its length, from the next mix; false if
// `seconds` is out of range.
FBE_AUDIO_API bool fbe_channel_seek(uint64_t channel, double seconds);
// Fades it from where its fade is, 1 at first, to `level` (0 or more) over `seconds`. Its fade
// multiplies its volume.
FBE_AUDIO_API bool fbe_channel_fade(uint64_t channel, float level, double seconds);
// Fades it to silence over `seconds`, then finishes it; 0 finishes it now.
FBE_AUDIO_API bool fbe_channel_fade_out(uint64_t channel, double seconds);
// Where its fade is.
FBE_AUDIO_API float fbe_channel_fade_level(uint64_t channel);
// Moves it into `bus`, or the whole mix for 0; false if the bus is gone.
FBE_AUDIO_API bool fbe_channel_set_bus(uint64_t channel, uint64_t bus);
// How a mono channel's pan, not from an emitter, is heard in a stereo mix, for every channel: 0 as
// a balance, which takes from the other side only, so both are at full in the middle; 1 at constant
// power, cos and sin of (pan + 1) * pi / 4, 0.707 each in the middle; 2 linearly, (1 - pan) / 2
// and (1 + pan) / 2, half each in the middle. A stereo channel's pan, or an emitter's, is a
// balance; in a mix that isn't stereo, a pan does nothing. A law changed while channels play is
// ramped to, as a pan is. False for another law.
FBE_AUDIO_API bool fbe_channel_set_pan_law(int32_t law);
FBE_AUDIO_API int32_t fbe_channel_pan_law(void);
// How many channels are playing, paused ones too.
FBE_AUDIO_API int32_t fbe_channel_count(void);

#ifdef __cplusplus
}
#endif

#endif
