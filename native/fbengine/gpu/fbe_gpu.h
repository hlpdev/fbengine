#ifndef FBE_GPU_H
#define FBE_GPU_H

// The GPU: Vulkan 1.2, the device fbengine draws with, and the window's swapchain, which
// fbe_frame_flip presents. The GPU starts the first time something needs it: a window that's
// shown, at fbe_window_open; otherwise fbe_gpu_start, or the first thing that draws.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// How the screen is scaled into the window, where their sizes differ: stretched to fill it,
// fitted, as big as it fits keeping its shape, or by whole pixels, as big as that fits.
#define FBE_GPU_STRETCH 0
#define FBE_GPU_FIT     1
#define FBE_GPU_INTEGER 2

// A part of the window, in its pixels from its top left. C has no `using`, so it's a typedef.
// NOLINTNEXTLINE(modernize-use-using)
typedef struct fbe_gpu_rect_t {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} fbe_gpu_rect_t;

// What the last frame did, as a debug overlay shows it: the draws its passes recorded and their
// triangles, the bytes it uploaded, its passes and the image barriers between them, the images
// transient targets have, and how long the GPU took over its commands, in milliseconds, a frame or
// two behind, and -1 until it's known or where the GPU writes no timestamps.
// NOLINTNEXTLINE(modernize-use-using)
typedef struct fbe_gpu_counters_t {
    int64_t draws;
    int64_t triangles;
    int64_t uploaded;
    int32_t passes;
    int32_t barriers;
    int32_t transient_images;
    float gpu_milliseconds;
} fbe_gpu_counters_t;

// What kind of GPU a device is, as its driver says.
#define FBE_GPU_OTHER      0
#define FBE_GPU_INTEGRATED 1
#define FBE_GPU_DISCRETE   2
#define FBE_GPU_VIRTUAL    3
#define FBE_GPU_CPU        4

// A test hook, for a soak that counts what's kept: how many things wait for the GPU to be done
// with a frame before they're freed, as a texture's image and a buffer are; 0 with no GPU started.
// Each Flip frees what the frame before it left, so it comes back to what it was.
FBE_API int32_t fbe_gpu_waiting_frees(void);
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
// it can't, and then it never starts, saying why each time.
FBE_API bool fbe_gpu_start(void);
FBE_API bool fbe_gpu_started(void);
// Whether there's to be no GPU at all, as for a dedicated server, set before anything starts it:
// Vulkan's never loaded and no device is made, so it needs no driver, loader, or display. Each
// thing that would start it fails saying "the GPU is disabled", and a model's loaded without its
// images. False, disabling it, if it's started, or Vulkan's loaded; enabling it again is true.
FBE_API bool fbe_gpu_set_disabled(bool disable);
FBE_API bool fbe_gpu_disabled(void);
// How many times it's been stopped, so what was made before can tell that it's gone.
FBE_API uint64_t fbe_gpu_stops(void);
// Stops the GPU, if it's started, freeing everything it made, the window's swapchain first; the
// window stays open, and the GPU can start again.
FBE_API void fbe_gpu_stop(void);
// Whether the GPU was lost, as a driver's crash or reset loses it: from then on nothing draws,
// fbe_frame_flip shows nothing, and what needs the GPU fails, saying so, until fbe_gpu_stop and
// fbe_gpu_start make it again, and with it everything on it, as nothing made before is left.
FBE_API bool fbe_gpu_lost(void);

// How many bytes of the GPU's own memory (its device-local heaps) the program can use, as the
// driver says where it can (VK_EXT_memory_budget) or else most of the heaps' size, and how many it
// uses, which counts against it; -1 before the GPU starts.
FBE_API int64_t fbe_gpu_memory_budget(void);
FBE_API int64_t fbe_gpu_memory_usage(void);

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

// Where the GPU keeps the pipelines it makes, so they're made faster the next time the program
// runs: read as the GPU starts, if it's this GPU's and driver's, and written at the fbe_frame_flip
// after new pipelines are made; "" keeps none. By default it's the user's cache folder's
// fbengine/<program>/pipelines.bin (LOCALAPPDATA on Windows, ~/Library/Caches on macOS,
// XDG_CACHE_HOME or ~/.cache elsewhere), or "" where they can't be found.
FBE_API void fbe_gpu_set_pipeline_cache(const char* path);
FBE_API const char* fbe_gpu_pipeline_cache(void);
// How many pipelines the GPU has made.
FBE_API int32_t fbe_gpu_pipelines(void);

// How far every texture's mipmaps are biased, added to the level the GPU picks: negative is
// sharper, positive blurrier; false if it's not -16 to 16. The GPU's limit clamps it, and a GPU
// whose samplers have none, as Metal's through MoltenVK, draws with no bias.
FBE_API bool fbe_gpu_set_lod_bias(float bias);
FBE_API float fbe_gpu_lod_bias(void);
// The most anisotropy textures filtered LINEAR are sampled with, sharper at glancing angles: 1 for
// none, up to 16; false if it's not 1 to 16. fbe_gpu_max_anisotropy, the GPU's most, clamps it, 0
// before the GPU starts and 1 on a GPU with none.
FBE_API bool fbe_gpu_set_anisotropy(float anisotropy);
FBE_API float fbe_gpu_anisotropy(void);
FBE_API float fbe_gpu_max_anisotropy(void);

// The screen: the target, a texture, each frame's drawn into, RGBA8 with depth, which Flip shows
// in the window, scaled, starting the GPU if nothing has; 0 without a window, or if the GPU can't
// start. It's made again, a new texture, when its size or samples change, a new size of the
// window's once the Flip after it has shown the frame drawn into the old one, and its pixels stay
// from one Flip to the next.
FBE_API uint64_t fbe_gpu_screen(void);
// The screen's size in pixels, 0 by 0 for the window's in pixels, which it follows as of the last
// fbe_frame_flip; false if it's not 1 to 16384 a side. A new size, or samples, makes the screen
// again at once.
FBE_API bool fbe_gpu_set_render_size(int32_t width, int32_t height);
FBE_API int32_t fbe_gpu_render_width(void);
FBE_API int32_t fbe_gpu_render_height(void);
// How many samples the screen draws a pixel, 1, 2, 4, or 8; false for another, or, once the GPU's
// started, for one it doesn't draw. Set before, the screen draws the most the GPU does, up to it,
// and fbe_gpu_render_samples is what the screen draws.
FBE_API bool fbe_gpu_set_render_samples(int32_t samples);
FBE_API int32_t fbe_gpu_render_samples(void);
// How the screen is scaled into the window, an FBE_GPU_ scaling, FIT unless set, and whether
// smoothly, or by its nearest pixels; false for another scaling.
FBE_API bool fbe_gpu_set_scaling(int32_t scaling, bool smooth);
FBE_API int32_t fbe_gpu_scaling(void);
FBE_API bool fbe_gpu_scaling_smooth(void);
// The part of the window the screen there is now is shown in, in the window's pixels; false
// without a window.
FBE_API bool fbe_gpu_screen_area(fbe_gpu_rect_t* area);

// Fills `counters` with what the last frame did; false if there are none to fill.
FBE_API bool fbe_gpu_counters(fbe_gpu_counters_t* counters);

// How many errors Vulkan's validation layers have found since the program started, which are on
// in a debug build of the DLL when the layers are there, or when FBE_GPU_VALIDATION is 1 (and off
// when it's 0); each is printed as it's found.
FBE_API int32_t fbe_gpu_validation_errors(void);
FBE_API bool fbe_gpu_validating(void);

#ifdef __cplusplus
}
#endif

#endif
