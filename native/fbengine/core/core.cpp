#include "core/engine.hpp"
#include "fbengine.h"

#include <exception>
#include <format>
#include <SDL3/SDL.h>
#include <string>
#include <string_view>

namespace {

thread_local std::string last_error;

bool headless = false;
bool video = false;

} // namespace

#ifdef __APPLE__
// The Objective-C runtime's own, which `@autoreleasepool` compiles to.
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* pool);
#endif

namespace fbe {

#ifdef __APPLE__
autoreleased_t::autoreleased_t() noexcept : m_pool(objc_autoreleasePoolPush()) {}

autoreleased_t::~autoreleased_t() {
    objc_autoreleasePoolPop(m_pool);
}
#else
autoreleased_t::autoreleased_t() noexcept = default;

autoreleased_t::~autoreleased_t() {
    // There's none here to let go of.
    m_pool = nullptr;
}
#endif

// NOLINTNEXTLINE(bugprone-exception-escape): what it assigns second fits in any string as it is.
bool fail(std::string_view message) noexcept {
    try {
        last_error = message;
    } catch (const std::exception&) {
        // No memory for it, or more than a string holds.
        last_error = "out of memory";
    }
    return false;
}

bool fail_sdl(std::string_view doing) {
    return fail(std::format("{}: {}", doing, SDL_GetError()));
}

bool start_video() {
    if (video) {
        return true;
    }
    if (headless) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    }
    // SDL otherwise turns a hidden cursor's frequent warps into relative mode by itself, above
    // about 33 frames a second, which changes MoveMouse's units with the frame rate; mouse look
    // asks for relative mode instead.
    SDL_SetHint(SDL_HINT_MOUSE_EMULATE_WARP_WITH_RELATIVE, "0");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        return fail_sdl("couldn't start the video");
    }
    video = true;
    return true;
}

bool video_started() {
    return video;
}

} // namespace fbe

extern "C" {

FBE_API const char* fbe_version(void) {
    return FBE_VERSION;
}

FBE_API const char* fbe_error(void) {
    return last_error.c_str();
}

FBE_API bool fbe_window_set_headless(bool on) {
    if (fbe::video_started() && on != headless) {
        return fbe::fail("headless is set before the first window or display");
    }
    headless = on;
    return true;
}

FBE_API bool fbe_window_headless(void) {
    return headless;
}

} // extern "C"
