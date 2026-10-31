#include "core/engine.hpp"
#include "fbengine.h"

#include <format>
#include <SDL3/SDL.h>
#include <string>
#include <string_view>

namespace {

thread_local std::string last_error;

bool headless = false;
bool video = false;

} // namespace

namespace fbe {

bool fail(std::string_view message) {
    last_error = message;
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
