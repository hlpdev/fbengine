#ifndef FBE_EMITTER_H
#define FBE_EMITTER_H

// Emitters: places in the world that channels play from, heard from the listener's place, quieter
// with distance, panned to their side, and pitched by the Doppler effect. An emitter's channels
// move with it; freed, it leaves them playing where it was, at rest.

#include "../audio/fbe_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

// An emitter at the origin, at rest, with an inverse falloff from 1 unit, a rolloff of 1, and a
// Doppler factor of 1.
FBE_AUDIO_API uint64_t fbe_emitter_add(void);
FBE_AUDIO_API void fbe_emitter_free(uint64_t emitter);
// Each of these moves its channels too; an emitter that's gone is nothing.
FBE_AUDIO_API void fbe_emitter_set_position(uint64_t emitter, float x, float y, float z);
FBE_AUDIO_API void fbe_emitter_set_velocity(uint64_t emitter, float x, float y, float z);
// Places `count` emitters at once, as following what makes their sounds does each frame: each of
// `emitters` at the three floats of `positions` at its index, moving at those of `velocities`.
// Those that are gone are nothing.
FBE_AUDIO_API void fbe_emitter_place_all(const uint64_t* emitters, const float* positions,
                                         const float* velocities, int32_t count);
// Within `nearest` units its channels are at their full volume, and past `farthest` no quieter;
// false unless 0 < nearest <= farthest. (Windows defines near and far as macros.)
FBE_AUDIO_API bool fbe_emitter_set_range(uint64_t emitter, float nearest, float farthest);
FBE_AUDIO_API float fbe_emitter_nearest(uint64_t emitter);
FBE_AUDIO_API float fbe_emitter_farthest(uint64_t emitter);
// How quickly they get quieter past `nearest`, 0 or more, 0 for not at all.
FBE_AUDIO_API bool fbe_emitter_set_rolloff(uint64_t emitter, float rolloff);
FBE_AUDIO_API float fbe_emitter_rolloff(uint64_t emitter);
// How they get quieter: 0 inversely with distance, as OpenAL's inverse clamped, 1 linearly to
// silence at `farthest`, 2 exponentially; false for another.
FBE_AUDIO_API bool fbe_emitter_set_falloff(uint64_t emitter, int32_t falloff);
FBE_AUDIO_API int32_t fbe_emitter_falloff(uint64_t emitter);
// How much its and the listener's velocities pitch them, 0 or more: 1 as in the world, 0 not at
// all.
FBE_AUDIO_API bool fbe_emitter_set_doppler(uint64_t emitter, float factor);
FBE_AUDIO_API float fbe_emitter_doppler(uint64_t emitter);

#ifdef __cplusplus
}
#endif

#endif
