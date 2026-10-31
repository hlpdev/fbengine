// The backends AudioDevice.DEFAULT opens its device on: never miniaudio's null one, which it would
// otherwise fall back to on a machine with no output, playing to nothing.

#include "audio/backends.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <miniaudio.h>
#include <vector>

TEST_CASE("the default output's backends are every one built but the null backend") {
    const std::vector<ma_backend> backends = fbe::audio::output_backends();
    CHECK_FALSE(backends.empty());
    CHECK(std::ranges::find(backends, ma_backend_null) == backends.end());
#ifdef _WIN32
    CHECK(std::ranges::find(backends, ma_backend_wasapi) != backends.end());
#elifdef __APPLE__
    CHECK(std::ranges::find(backends, ma_backend_coreaudio) != backends.end());
#else
    CHECK(std::ranges::find(backends, ma_backend_alsa) != backends.end());
#endif
}

// A machine with no output, as CI's containers are, has no context at all, rather than a null one.
TEST_CASE("a context of the default output's backends is never the null backend's") {
    const std::vector<ma_backend> backends = fbe::audio::output_backends();
    ma_context context;
    if (ma_context_init(backends.data(), static_cast<ma_uint32>(backends.size()), nullptr,
                        &context) == MA_SUCCESS) {
        CHECK(context.backend != ma_backend_null);
        ma_context_uninit(&context);
    }
}
