// A soak: each test video opened, played to its end, sought, looped, its frame and sound taken,
// and freed, a thousand times over, leaves nothing open, and as many allocations live after each
// round, once the first rounds have warmed it, as there were: fbengine_video's sources are built in
// here with tests/own_new.cpp, which counts each of theirs, on every thread, exactly, on any
// platform. Bytes, which other threads' malloc moves, are only shown; LeakSanitizer's nightly run
// has what malloc itself, pl_mpeg's, leaks.

#include "fbengine_video.h"
#include "own_new.hpp"
#include "stability.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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
    const long before = fbe::tests::live;
    const std::uint64_t bytes = fbe::tests::malloc_bytes();
    // Nothing of Catch's in the rounds, whose first INFO or failure allocates for good.
    int first_off = -1;
    long off_by = 0;
    for (int round = 20; round < 1000; ++round) {
        played(videos[static_cast<std::size_t>(round) % videos.size()]);
        if (first_off < 0 && fbe::tests::live != before) {
            first_off = round;
            off_by = fbe::tests::live - before;
        }
    }
    INFO("round " << first_off << " left " << off_by << " allocations live");
    CHECK(first_off == -1);
    WARN("bytes allocated over the warmed rounds: "
         << static_cast<std::int64_t>(fbe::tests::malloc_bytes() - bytes));
    CHECK(fbe_video_count() == 0);
}
