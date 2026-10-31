#ifndef FBE_AUDIO_H
#define FBE_AUDIO_H

// fbengine_audio's mix and the device it plays on, and what every area of its C API shares: how
// its functions are exported, and its errors. Each area's header (fbe_<area>.h) has its own binding
// file in packages/fbengine_audio/c/, which tools/bindgen.sh makes, so an area's header uses only
// fixed-size types and its own structs, never another area's. Everything but fbe_audio_version and
// fbe_audio_error is the main thread's, which the FB API's @MainThread keeps it to.
//
// Sounds, channels, buses, and emitters are handles: a 32-bit slot, a 24-bit generation, and an
// 8-bit kind, fbengine_audio's from 64 to 95, 0 for none. A call given a freed one, or one of
// another kind, takes it for none.

#include <stdbool.h>
#include <stdint.h>

#ifndef _WIN32
#define FBE_AUDIO_API __attribute__((visibility("default")))
#else
#ifdef FBE_AUDIO_BUILDING
#define FBE_AUDIO_API __declspec(dllexport)
#else
#define FBE_AUDIO_API __declspec(dllimport)
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// What fbe_audio_simulate does to the device, as the OS would: its output unplugged, which stops
// it; every output plugged back in, so fbe_audio_recover has the default to fall back to; every
// output unplugged, so it has none until they're plugged back in; its mix stalled, the device
// left started, as a lost output's is on CoreAudio and PulseAudio; and the default output
// changed, which a device opened on it follows.
#define FBE_AUDIO_SIMULATE_UNPLUG         0
#define FBE_AUDIO_SIMULATE_PLUG_IN        1
#define FBE_AUDIO_SIMULATE_UNPLUG_ALL     2
#define FBE_AUDIO_SIMULATE_STALL          3
#define FBE_AUDIO_SIMULATE_DEFAULT_CHANGE 4

// The version of fbengine this DLL is, like "0.1.0", which is its packages'.
FBE_AUDIO_API const char* fbe_audio_version(void);

// What the last call that failed on this thread says went wrong.
FBE_AUDIO_API const char* fbe_audio_error(void);

// Starts the mix on `device`: 0 the OS's default output, on any of its backends but the null one,
// 1 miniaudio's null backend, which plays to nothing at the pace a device would, or 2 none, where
// fbe_audio_mix makes each frame when it's called. `sample_rate` is from 8000 to 384000 Hz and
// `channels` from 1 to 8, or 0 for the device's own (48000 Hz and 2 with no device). `output` is
// the name of one of the device's outputs, as fbe_audio_list_outputs lists them, or "" or null for
// its default; `period` the seconds it mixes at a time, from 0.001 to 1, or 0 for its own. False if
// it's started already, a value's out of range, there's no output of that name, or the device
// won't start.
FBE_AUDIO_API bool fbe_audio_start(int32_t device, int32_t sample_rate, int32_t channels,
                                   const char* output, double period);
// Stops the mix, which finishes every channel, and lets the device go; stopped already, it does
// nothing.
FBE_AUDIO_API void fbe_audio_stop(void);
FBE_AUDIO_API bool fbe_audio_started(void);
// Lists `device`'s outputs, 0's the OS's and 1's the null backend's one, for fbe_audio_output_name:
// how many, or -1, for fbe_audio_error, if there's no backend to list them; 2 has none. A second
// output of one name is listed as "name (2)", which fbe_audio_start takes for it.
FBE_AUDIO_API int32_t fbe_audio_list_outputs(int32_t device);
// The name of output `index` of those fbe_audio_list_outputs listed last, "" for another index.
FBE_AUDIO_API const char* fbe_audio_output_name(int32_t index);
// The seconds the device mixes at a time, 0 when it's stopped or there's no device.
FBE_AUDIO_API double fbe_audio_period(void);
// What fbe_audio_start was given for its device, -1 when it's stopped.
FBE_AUDIO_API int32_t fbe_audio_device(void);
// The name of the output the device plays on, "" when it's stopped or has none.
FBE_AUDIO_API const char* fbe_audio_device_name(void);
// Whether the device was lost, though the mix is started: it stopped on its own, as one does when
// its output's unplugged, or it's started but its mix hasn't moved for a quarter of a second (four
// periods if they're longer, two seconds before its first frames), as a lost output's doesn't on
// CoreAudio and PulseAudio; or fbe_audio_recover made it again since the flip before. Nothing's
// mixed meanwhile. False when the mix is stopped or has no device.
FBE_AUDIO_API bool fbe_audio_lost(void);
// fbengine_audio's work at each flip, the only one that moves the device: one that's lost, or one
// on the default output when the OS's default has changed (asked once a second, on WASAPI, whose
// own rerouting is off), is made again on the default output, at the mix's rate and channels and
// the period it was started with, and the channels go on where they were. One that won't open, as
// with no output at all, is tried again at the next.
FBE_AUDIO_API void fbe_audio_recover(void);
// Does one of the FBE_AUDIO_SIMULATE_ events to the device; false when the mix is stopped or has
// no device, or for another event.
FBE_AUDIO_API bool fbe_audio_simulate(int32_t event);
// The mix's frames a second and channels, 0 when it's stopped.
FBE_AUDIO_API int32_t fbe_audio_sample_rate(void);
FBE_AUDIO_API int32_t fbe_audio_channels(void);
// How many frames have been mixed since it started, 0 when it's stopped.
FBE_AUDIO_API int64_t fbe_audio_frames(void);
// The volume the whole mix is multiplied by, 1 at first, kept while it's stopped; false if it's
// negative or not a number.
FBE_AUDIO_API bool fbe_audio_set_volume(float volume);
FBE_AUDIO_API float fbe_audio_volume(void);
// Mixes the next `frames` frames into `samples`, which has room for frames times
// fbe_audio_channels() floats, interleaved, clipped to -1 to 1 as a device's are; false unless it's
// started with no device, or if `frames` is negative.
FBE_AUDIO_API bool fbe_audio_mix(float* samples, int32_t frames);

#ifdef __cplusplus
}
#endif

#endif
