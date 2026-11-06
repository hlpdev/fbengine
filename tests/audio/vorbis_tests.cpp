// stb_vorbis as native/fbengine_audio/CMakeLists.txt patches it: its CRC table, const and worked
// out there, is what finds a streamed Ogg's pages as it's sought, so a seek in one plays what the
// whole sound decoded in memory does from there.

#include "fbengine_audio.h"
#include "stability.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace {

constexpr std::int32_t rate = 44100;

/// `frames` frames of `sound`, played from `seconds` in on its own.
std::vector<float> from(std::uint64_t sound, double seconds, std::int32_t frames) {
    const std::uint64_t channel = fbe_sound_play(sound, 1.0F, 0.0F, 1.0F, false, false, 0.0, 0, 0);
    REQUIRE(channel != 0);
    REQUIRE(fbe_channel_seek(channel, seconds));
    std::vector<float> samples(static_cast<std::size_t>(frames) * 2);
    REQUIRE(fbe_audio_mix(samples.data(), frames));
    fbe_channel_stop(channel);
    // What's stopped ramps out in the next mix, before the next play.
    std::vector<float> after(4410 * 2);
    REQUIRE(fbe_audio_mix(after.data(), 4410));
    return samples;
}

} // namespace

TEST_CASE("a streamed Ogg Vorbis of many pages, sought, plays what it does loaded from there") {
    const std::vector<std::uint8_t> bytes = fbe::tests::read(
        std::filesystem::path(FBE_AUDIO_TEST_CORPUS) / "ogg_vorbis_stereo_44100_small_pages.ogg");
    REQUIRE(!bytes.empty());
    REQUIRE(fbe_audio_start(2, rate, 2, nullptr, 0.0));
    const auto size = static_cast<std::int64_t>(bytes.size());
    const std::uint64_t loaded = fbe_sound_load(bytes.data(), size);
    const std::uint64_t streamed = fbe_sound_stream(bytes.data(), size);
    REQUIRE(loaded != 0);
    REQUIRE(streamed != 0);
    // Into each of its pages, and back.
    for (const double seconds : std::array{ 0.07, 0.015, 0.045, 0.085, 0.03 }) {
        INFO("from " << seconds << " s");
        const std::vector<float> whole = from(loaded, seconds, 512);
        const std::vector<float> paged = from(streamed, seconds, 512);
        float louder = 0.0F;
        float apart = 0.0F;
        for (std::size_t i = 0; i < whole.size(); ++i) {
            louder = std::fmax(louder, std::fabs(whole[i]));
            apart = std::fmax(apart, std::fabs(whole[i] - paged[i]));
        }
        CHECK(louder > 0.1F);
        CHECK(apart < 1e-4F);
    }
    fbe_sound_free(loaded);
    fbe_sound_free(streamed);
    fbe_audio_stop();
}
