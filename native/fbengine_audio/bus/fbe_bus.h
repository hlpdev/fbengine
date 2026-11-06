#ifndef FBE_BUS_H
#define FBE_BUS_H

// Buses: groups of channels, each mixed through its own volume, low-pass, and reverb into its
// parent bus or the whole mix. A bus keeps its settings while the audio is stopped. A bus's handle
// is the game's until it frees it; the channels and buses in it keep it until they go.

#include "../audio/fbe_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

// A bus in `parent`, or in the whole mix for 0; 0 if `parent` is gone.
FBE_AUDIO_API uint64_t fbe_bus_add(uint64_t parent);
// Lets the bus go; its channels and buses play on through it. A bus that's gone already is nothing.
FBE_AUDIO_API void fbe_bus_free(uint64_t bus);
// What its channels are multiplied by, 0 or more, a change heard ramped over the next 5 ms of the
// mix, as a channel's is; false if it's out of range or the bus is gone.
FBE_AUDIO_API bool fbe_bus_set_volume(uint64_t bus, float volume);
FBE_AUDIO_API float fbe_bus_volume(uint64_t bus);
// A fourth-order low-pass at `cutoff` Hz, from 10 to 24000, or none for 0, held to between 1/2400
// of the mix's rate, below which its float32 filter rings (20 Hz at 48000, 160 at 384000), and 0.9
// of half of it; false if it's out of range or the bus is gone.
FBE_AUDIO_API bool fbe_bus_set_low_pass(uint64_t bus, float cutoff);
FBE_AUDIO_API float fbe_bus_low_pass(uint64_t bus);
// A Freeverb reverb, each value 0 to 1: the room's size, how its walls damp highs, the stereo
// width, how loud the reverb is, and how loud what goes in is, 1 as it is. A mix of more than 2
// channels, or under 22050 or over 176400 Hz, which Freeverb, as verblib has it, can't do, plays
// it dry. False if a value's out of range or the bus is gone.
FBE_AUDIO_API bool fbe_bus_set_reverb(uint64_t bus, float room_size, float damping, float width,
                                      float wet, float dry);
// Takes its reverb away.
FBE_AUDIO_API void fbe_bus_clear_reverb(uint64_t bus);
// Whether it has a reverb, and whether the mix plays it.
FBE_AUDIO_API bool fbe_bus_has_reverb(uint64_t bus);
FBE_AUDIO_API bool fbe_bus_reverb_heard(uint64_t bus);

#ifdef __cplusplus
}
#endif

#endif
