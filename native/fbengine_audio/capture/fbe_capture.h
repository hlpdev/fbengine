#ifndef FBE_CAPTURE_H
#define FBE_CAPTURE_H

// Capture: a microphone's samples as it hears them, for voice chat, apart from the mix, which
// needn't be started. There's one capture at a time; the main thread starts it, and reads what
// it's heard, which the device's own thread keeps for it meanwhile.

#include "../audio/fbe_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Lists `device`'s inputs, for fbe_capture_input_name, as fbe_audio_list_outputs lists outputs:
// 0's the OS's, 1's the null backend's one, and 2 has none; how many, or -1, for fbe_audio_error,
// if there's no backend to list them. A second input of one name is listed as "name (2)".
FBE_AUDIO_API int32_t fbe_capture_list_inputs(int32_t device);
// The name of input `index` of those fbe_capture_list_inputs listed last, "" for another index.
FBE_AUDIO_API const char* fbe_capture_input_name(int32_t index);
// Starts capturing from `device`: 0 the OS's default input, or `input`, one of its names as
// fbe_capture_list_inputs lists them; 1 the null backend's, which hears silence at the pace a
// microphone would; or 2 none, where fbe_capture_hear stands in for a microphone, as tests want.
// Its samples are converted to `sample_rate` (8000 to 384000 Hz) and `channels` (1 to 8), 0 for
// the input's own (48000 and 1 with no device), and it keeps up to `seconds` (0.01 to 60, 0 for 1)
// of them unread, dropping what it hears past that. False if it's started already, a value's out
// of range, there's no input of that name, or the device won't start.
FBE_AUDIO_API bool fbe_capture_start(int32_t device, const char* input, int32_t sample_rate,
                                     int32_t channels, double seconds);
// Stops capturing and lets the device go, with what it heard unread; stopped already, it does
// nothing.
FBE_AUDIO_API void fbe_capture_stop(void);
FBE_AUDIO_API bool fbe_capture_started(void);
// What fbe_capture_start was given for its device, -1 when it's stopped.
FBE_AUDIO_API int32_t fbe_capture_device(void);
// The name of the input it captures from, "" when it's stopped or has none.
FBE_AUDIO_API const char* fbe_capture_input(void);
// Its frames a second and channels, 0 when it's stopped.
FBE_AUDIO_API int32_t fbe_capture_sample_rate(void);
FBE_AUDIO_API int32_t fbe_capture_channels(void);
// How many frames it's heard that haven't been read, 0 when it's stopped.
FBE_AUDIO_API int32_t fbe_capture_available(void);
// Reads up to `frames` of the frames it's heard, oldest first, into `samples`, which has room for
// frames times fbe_capture_channels() floats: how many it read, or -1 if it's stopped or `frames`
// is negative.
FBE_AUDIO_API int32_t fbe_capture_read(float* samples, int32_t frames);
// How many frames it's dropped since it started, heard while it held all it keeps.
FBE_AUDIO_API int64_t fbe_capture_dropped(void);
// Whether its device stopped on its own, as one does when its microphone's unplugged; it hears
// nothing more until it's started again.
FBE_AUDIO_API bool fbe_capture_lost(void);
// On device 2, takes the `frames` frames at `samples` as heard, as a microphone's would be: false
// unless it's started on 2, or if `frames` is negative.
FBE_AUDIO_API bool fbe_capture_hear(const float* samples, int32_t frames);
// Unplugs its device, as the OS would its microphone, which stops it and makes it lost; false
// unless it's started on a device.
FBE_AUDIO_API bool fbe_capture_simulate_unplug(void);

#ifdef __cplusplus
}
#endif

#endif
