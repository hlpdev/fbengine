#ifndef FBE_LISTENER_H
#define FBE_LISTENER_H

// The listener: where the 3D sounds, the channels played from emitters, are heard from, in
// std.math's axes (right-handed, Y up, -Z forward), and in a game's units. Its settings are kept
// while the audio is stopped.

#include "../audio/fbe_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

FBE_AUDIO_API void fbe_listener_set_position(float x, float y, float z);
// Which way it faces, and which way is up for it, neither of them 0.
FBE_AUDIO_API void fbe_listener_set_direction(float forward_x, float forward_y, float forward_z,
                                              float up_x, float up_y, float up_z);
// How fast it moves, in units a second, for the Doppler effect.
FBE_AUDIO_API void fbe_listener_set_velocity(float x, float y, float z);
// How many units a second sound goes, 343.3 at first, as meters; false if it isn't more than 0.
FBE_AUDIO_API bool fbe_listener_set_speed_of_sound(float units_per_second);
FBE_AUDIO_API float fbe_listener_speed_of_sound(void);

#ifdef __cplusplus
}
#endif

#endif
