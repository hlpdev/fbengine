// A soak: each test video opened, played to its end, sought, looped, its frame and sound taken,
// and freed, a thousand times over, leaves nothing open, and what malloc holds after each round
// never more than 16 KB over what it held once the first rounds had warmed it.

#include "fbengine_video.h"
#include "stability.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using fbe::tests::allocated;
using fbe::tests::sanitized;

namespace {

std::vector<std::uint8_t> read(const std::string& name) {
    return fbe::tests::read(std::filesystem::path(FBE_VIDEO_MEDIA) / name);
}

/// Opens `bytes`, plays it to its end, seeks, loops past its end, takes its frame and sound, and
/// frees it.
void played(const std::vector<std::uint8_t>& bytes) {
    const std::uint64_t video =
        fbe_video_open(bytes.data(), static_cast<std::int64_t>(bytes.size()));
    REQUIRE(video != 0);
    fbe_video_play(video);
    for (int i = 0; i < 400 && !fbe_video_ended(video); ++i) {
        fbe_video_update(video, 0.05);
    }
    CHECK(fbe_video_ended(video));
    const double duration = fbe_video_duration(video);
    fbe_video_seek(video, duration / 2.0);
    fbe_video_set_loop(video, true);
    fbe_video_play(video);
    fbe_video_update(video, duration * 1.5);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(fbe_video_width(video)) *
                                     static_cast<std::size_t>(fbe_video_height(video)) * 4);
    CHECK(fbe_video_frame(video, pixels.data(), static_cast<std::int64_t>(pixels.size())));
    if (fbe_video_has_sound(video)) {
        std::vector<std::uint8_t> wav(static_cast<std::size_t>(fbe_video_sound(video, nullptr, 0)));
        CHECK(fbe_video_sound(video, wav.data(), static_cast<std::int64_t>(wav.size())) ==
              static_cast<std::int64_t>(wav.size()));
    }
    CHECK(fbe_video_free(video));
}

} // namespace

TEST_CASE("a thousand videos played and freed leave nothing open, and the memory as it was",
          "[stability]") {
    const std::array<std::vector<std::uint8_t>, 5> videos{
        read("colors.mpg"), read("countb.mpg"),    read("ntsc.mpg"),
        read("late.mpg"),   read("longsound.mpg"),
    };
    for (const std::vector<std::uint8_t>& video : videos) {
        REQUIRE(!video.empty());
    }
    for (int round = 0; round < 20; ++round) {
        played(videos[static_cast<std::size_t>(round) % videos.size()]);
    }
    const std::uint64_t before = allocated();
    std::uint64_t most = before;
    for (int round = 20; round < 1000; ++round) {
        played(videos[static_cast<std::size_t>(round) % videos.size()]);
        most = std::max(most, allocated());
    }
    INFO("before " << before << ", most after a round " << most);
    if (!sanitized) {
        CHECK(most <= before + (std::uint64_t{ 16 } << 10U));
    }
    CHECK(fbe_video_count() == 0);
}
