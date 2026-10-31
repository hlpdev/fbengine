// The frame: fbe_frame_flip ends one, waiting for the frame limit and reading the OS's events, and
// keeps the time each took.

#include "core/engine.hpp"
#include "fbengine.h"
#include "input/input.hpp"

#include <cstdint>
#include <format>
#include <SDL3/SDL.h>

namespace {

constexpr double nanoseconds = 1e9;
// The frame rate is how many frames ended in the last half second or more, over that time.
constexpr std::uint64_t rate_period = 500'000'000;
// The most frames a second a limit can ask for.
constexpr std::int32_t fastest = 1000;

std::uint64_t last_flip = 0;
std::int64_t frames = 0;
double delta = 0.0;
double rate = 0.0;
std::uint64_t rate_since = 0;
std::int64_t rate_frames = 0;
std::int32_t limit = 0;
bool vsync = true;

} // namespace

extern "C" {

FBE_API void fbe_frame_flip(bool wait_for_vsync) {
    // The renderer presents the frame here, from E2, waiting for the display when it's asked to.
    vsync = wait_for_vsync;

    // SDL's clock starts at the first flip, so the first's time can be 0.
    const bool first = frames == 0;
    std::uint64_t now = SDL_GetTicksNS();
    if (limit > 0 && !first) {
        const std::uint64_t next =
            last_flip + static_cast<std::uint64_t>(nanoseconds / static_cast<double>(limit));
        if (now < next) {
            SDL_DelayPrecise(next - now);
            now = SDL_GetTicksNS();
        }
    }

    if (fbe::video_started()) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            fbe::window_event(event);
            fbe::input_event(event);
        }
    }

    delta = first ? 0.0 : static_cast<double>(now - last_flip) / nanoseconds;
    last_flip = now;
    ++frames;
    if (first) {
        rate_since = now;
    } else {
        ++rate_frames;
        const std::uint64_t spent = now - rate_since;
        if (spent >= rate_period) {
            rate = static_cast<double>(rate_frames) * nanoseconds / static_cast<double>(spent);
            rate_since = now;
            rate_frames = 0;
        }
    }
}

FBE_API double fbe_frame_delta(void) {
    return delta;
}

FBE_API int64_t fbe_frame_count(void) {
    return frames;
}

FBE_API double fbe_frame_rate(void) {
    return rate;
}

FBE_API bool fbe_frame_set_limit(int32_t frames_per_second) {
    if (frames_per_second < 0 || frames_per_second > fastest) {
        return fbe::fail(std::format("a frame limit is 0, for none, to {} frames a second, not {}",
                                     fastest, frames_per_second));
    }
    limit = frames_per_second;
    return true;
}

FBE_API int32_t fbe_frame_limit(void) {
    return limit;
}

FBE_API bool fbe_frame_vsync(void) {
    return vsync;
}

} // extern "C"
