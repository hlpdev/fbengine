// The screen: the target each frame's drawn into, its size its own or the window's, multisampled
// if asked, with depth; kept across Flip, as Blitz's windowed buffer was; and the part of the
// window Flip shows it in, stretched, fitted, or by whole pixels.

#include "core/engine.hpp"
#include "fbengine.h"
#include "gpu/vulkan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>

namespace {

/// The screen's handle, 0 without one; the size it's been made, and the size and samples asked
/// for, 0 by 0 following the window's pixels.
std::uint64_t screen = 0;
VkExtent2D made{};
std::int32_t made_samples = 0;
VkExtent2D wanted{};
std::int32_t samples = 1;
std::int32_t scaling = FBE_GPU_FIT;
bool smooth = true;
/// The window's size in pixels as the screen was last brought up to date for it, which a new
/// render size or samples makes it again for at once.
VkExtent2D window_size{};

/// The largest side a screen has, as a window's.
constexpr std::int32_t largest = 16384;

/// The screen's size for a window `window` pixels big: its own, or the window's.
VkExtent2D size_for(VkExtent2D window) {
    return wanted.width != 0 ? wanted : window;
}

} // namespace

namespace fbe::gpu {

std::uint64_t update_screen(VkExtent2D window) {
    window_size = window;
    const VkExtent2D size = size_for(window);
    if (size.width == 0 || size.height == 0) {
        return screen;
    }
    if (screen != 0 && size.width == made.width && size.height == made.height &&
        samples == made_samples && fbe_texture_width(screen) > 0) {
        return screen;
    }
    // A screen that can't be made leaves the old one, if there is one, which Flip shows.
    const std::uint64_t made_now = fbe_texture_create_target(static_cast<std::int32_t>(size.width),
                                                             static_cast<std::int32_t>(size.height),
                                                             FBE_TEXTURE_RGBA8, samples, true);
    if (made_now == 0) {
        return screen;
    }
    if (screen != 0) {
        fbe_texture_release(screen);
    }
    screen = made_now;
    made = size;
    made_samples = samples;
    return screen;
}

VkRect2D screen_area(VkExtent2D window) {
    const VkExtent2D size = size_for(window);
    if (size.width == 0 || size.height == 0 || scaling == FBE_GPU_STRETCH) {
        return { .extent = window };
    }
    double scale = std::min(static_cast<double>(window.width) / size.width,
                            static_cast<double>(window.height) / size.height);
    // Whole pixels, as pixel art wants, unless the window's smaller than the screen.
    if (scaling == FBE_GPU_INTEGER && scale >= 1.0) {
        scale = std::floor(scale);
    }
    const auto width = static_cast<std::uint32_t>(std::lround(size.width * scale));
    const auto height = static_cast<std::uint32_t>(std::lround(size.height * scale));
    return {
        .offset = {
            .x = static_cast<std::int32_t>((window.width - width) / 2),
            .y = static_cast<std::int32_t>((window.height - height) / 2),
        },
        .extent = { .width = std::max(width, 1U), .height = std::max(height, 1U) },
    };
}

std::uint64_t screen_handle() {
    return screen;
}

bool screen_smooth() {
    return smooth;
}

void close_screen() {
    if (screen != 0 && fbe::gpu::device() != nullptr) {
        fbe_texture_release(screen);
    }
    stop_screen();
}

void stop_screen() {
    // Stopping frees every texture, the screen too.
    screen = 0;
    window_size = {};
    made = {};
    made_samples = 0;
}

} // namespace fbe::gpu

extern "C" {

FBE_API uint64_t fbe_gpu_screen(void) {
    return fbe::gpu::screen_handle();
}

FBE_API bool fbe_gpu_set_render_size(int32_t width, int32_t height) {
    const bool following = width == 0 && height == 0;
    if (!following && (width < 1 || height < 1 || width > largest || height > largest)) {
        return fbe::fail(std::format("a render size is 1 to {} pixels on a side, or 0 by 0 for the "
                                     "window's, not {}x{}",
                                     largest, width, height));
    }
    wanted = {
        .width = static_cast<std::uint32_t>(width),
        .height = static_cast<std::uint32_t>(height),
    };
    if (screen != 0) {
        fbe::gpu::update_screen(window_size);
    }
    return true;
}

FBE_API int32_t fbe_gpu_render_width(void) {
    return static_cast<std::int32_t>(wanted.width);
}

FBE_API int32_t fbe_gpu_render_height(void) {
    return static_cast<std::int32_t>(wanted.height);
}

FBE_API bool fbe_gpu_set_render_samples(int32_t count) {
    if (count != 1 && count != 2 && count != 4 && count != 8) {
        return fbe::fail(
            std::format("the screen draws 1, 2, 4, or 8 samples a pixel, not {}", count));
    }
    samples = count;
    if (screen != 0) {
        fbe::gpu::update_screen(window_size);
    }
    return true;
}

FBE_API int32_t fbe_gpu_render_samples(void) {
    return samples;
}

FBE_API bool fbe_gpu_set_scaling(int32_t mode, bool smoothly) {
    if (mode != FBE_GPU_STRETCH && mode != FBE_GPU_FIT && mode != FBE_GPU_INTEGER) {
        return fbe::fail(std::format("{} isn't a Scaling", mode));
    }
    scaling = mode;
    smooth = smoothly;
    return true;
}

FBE_API int32_t fbe_gpu_scaling(void) {
    return scaling;
}

FBE_API bool fbe_gpu_scaling_smooth(void) {
    return smooth;
}

} // extern "C"
