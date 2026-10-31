#ifndef FBE_FRAME_H
#define FBE_FRAME_H

// The frame: ending one, and its timing.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Ends a frame: presents it (from E2, waiting for the display if `vsync`), waits for the frame
// limit, and reads the OS's events.
FBE_API void fbe_frame_flip(bool vsync);
// How many seconds the last frame took, flip to flip; 0 before the second flip.
FBE_API double fbe_frame_delta(void);
// How many frames have been flipped.
FBE_API int64_t fbe_frame_count(void);
// Frames a second, over the last half second or more; 0 until then.
FBE_API double fbe_frame_rate(void);
// The most frames a second fbe_frame_flip lets through, 0 for no limit; false if it's not 0 to
// 1000.
FBE_API bool fbe_frame_set_limit(int32_t frames_per_second);
FBE_API int32_t fbe_frame_limit(void);
// Whether the last fbe_frame_flip asked to wait for the display.
FBE_API bool fbe_frame_vsync(void);

#ifdef __cplusplus
}
#endif

#endif
