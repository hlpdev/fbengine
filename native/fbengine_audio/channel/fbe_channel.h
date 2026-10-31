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
// -1, the left, to 1, the right.
FBE_AUDIO_API bool fbe_channel_set_pan(uint64_t channel, float pan);
FBE_AUDIO_API float fbe_channel_pan(uint64_t channel);
// 1/16 to 16.
FBE_AUDIO_API bool fbe_channel_set_pitch(uint64_t channel, float pitch);
FBE_AUDIO_API float fbe_channel_pitch(uint64_t channel);
FBE_AUDIO_API void fbe_channel_set_loop(uint64_t channel, bool loop);
FBE_AUDIO_API bool fbe_channel_looping(uint64_t channel);
// How many channels are playing, paused ones too.
FBE_AUDIO_API int32_t fbe_channel_count(void);

#ifdef __cplusplus
}
#endif

#endif
