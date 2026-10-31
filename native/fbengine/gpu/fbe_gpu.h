#ifndef FBE_GPU_H
#define FBE_GPU_H

// The GPU: Vulkan 1.2, the device fbengine draws with, and the window's swapchain, which
// fbe_frame_flip presents. The GPU starts the first time something needs it: a window that's
// shown, at fbe_window_open; otherwise fbe_gpu_start, or the first thing that draws.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// What kind of GPU a device is, as its driver says.
#define FBE_GPU_OTHER      0
#define FBE_GPU_INTEGRATED 1
#define FBE_GPU_DISCRETE   2
#define FBE_GPU_VIRTUAL    3
#define FBE_GPU_CPU        4

// The devices Vulkan has, in its order; -1 if Vulkan can't start.
FBE_API int32_t fbe_gpu_device_count(void);
// A device's name, "" if there's no such device.
FBE_API const char* fbe_gpu_device_name(int32_t device);
// A device's FBE_GPU_ kind, FBE_GPU_OTHER if there's no such device.
FBE_API int32_t fbe_gpu_device_kind(int32_t device);
// What a device lacks of what fbengine needs, like "Vulkan 1.2" or "dynamic rendering", "" if it
// has it all, and "" if there's no such device.
FBE_API const char* fbe_gpu_device_lacks(int32_t device);

// The device the GPU starts on, -1 for the best that has what fbengine needs (a discrete GPU
// first); false once it's started, or if there's no such device.
FBE_API bool fbe_gpu_set_device(int32_t device);
// The device the GPU started on, or the one it will, -1 for the best.
FBE_API int32_t fbe_gpu_device(void);

// Starts the GPU, if it hasn't started, with a swapchain for the window if there is one; false if
// it can't.
FBE_API bool fbe_gpu_start(void);
FBE_API bool fbe_gpu_started(void);

// How many frames fbe_frame_flip has presented, which skips them while the window is minimized.
// A headless window's are drawn into an image of its own, its size in pixels, which nothing shows.
FBE_API int64_t fbe_gpu_presented(void);
// The size of the images the window is presented in, which follows its size in pixels; 0 without
// them.
FBE_API int32_t fbe_gpu_swapchain_width(void);
FBE_API int32_t fbe_gpu_swapchain_height(void);
// Whether the swapchain waits for the display, which the last fbe_frame_flip asked for.
FBE_API bool fbe_gpu_swapchain_vsync(void);
// Whether the window's images are a headless window's own, not a swapchain's.
FBE_API bool fbe_gpu_swapchain_headless(void);

// How many errors Vulkan's validation layers found since the GPU started, which are on in a debug
// build of the DLL when the layers are there, or when FBE_GPU_VALIDATION is 1 (and off when it's
// 0); each is printed as it's found.
FBE_API int32_t fbe_gpu_validation_errors(void);
FBE_API bool fbe_gpu_validating(void);

#ifdef __cplusplus
}
#endif

#endif
