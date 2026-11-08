// fbengine_video's decoders against what a game's videos can be: MPEG program streams of MPEG-1
// video, with MP2 sound or none, from packages/fbengine_video/tests/media and what once broke
// pl_mpeg there, cut short at every length to 4 KB and at a stride after, and with bytes changed,
// put in, taken out, and repeated, half of them in a header, from a fixed seed, so a failure is
// the same each run. Each opens or is refused, and each that opens plays to its end, seeks, loops,
// and gives its frame and sound: never a crash, a hang, or a read outside its bytes, which a build
// with a sanitizer checks.

#include "fbengine_video.h"
#include "stability.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using fbe::tests::random_t;
using fbe::tests::seed_t;

namespace {

/// Plays `video` to its end, a frame or a few at a time, seeks about it, loops past its end, and
/// takes its frame and sound. A changed video's duration can be anything its times say, so what's
/// played past is at most a minute of it.
void played(std::uint64_t video, random_t& random) {
    const double duration = std::min(fbe_video_duration(video), 60.0);
    CHECK(fbe_video_play(video));
    for (int i = 0; i < 400 && !fbe_video_ended(video); ++i) {
        fbe_video_update(video, 0.02 * static_cast<double>(1 + random.below(8)));
    }
    for (int seek = 0; seek < 3; ++seek) {
        fbe_video_seek(video, duration * static_cast<double>(random.below(101)) / 100.0);
        fbe_video_update(video, 0.04);
    }
    fbe_video_set_loop(video, true);
    fbe_video_play(video);
    fbe_video_update(video, (duration * 1.5) + 0.1);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(fbe_video_width(video)) *
                                     static_cast<std::size_t>(fbe_video_height(video)) * 4);
    CHECK(fbe_video_frame(video, pixels.data(), static_cast<std::int64_t>(pixels.size())));
    if (fbe_video_has_sound(video)) {
        std::vector<std::uint8_t> wav(static_cast<std::size_t>(fbe_video_sound(video, nullptr, 0)));
        CHECK(fbe_video_sound(video, wav.data(), static_cast<std::int64_t>(wav.size())) ==
              static_cast<std::int64_t>(wav.size()));
    }
}

/// Opens `bytes`, as `what` says they were made, and plays them if they open.
void tried(const std::vector<std::uint8_t>& bytes, const std::string& what, random_t& random) {
    INFO(what);
    const std::uint64_t video =
        fbe_video_open(bytes.data(), static_cast<std::int64_t>(bytes.size()));
    if (video == 0) {
        CHECK(!std::string_view(fbe_video_error()).empty());
        return;
    }
    played(video, random);
    CHECK(fbe_video_free(video));
}

/// Every one of `names`, in the test videos' folder, cut short at each length to 4096 and at 48
/// more after, and changed `changes` ways, all from `seed`.
void fuzzed(const std::vector<std::string_view>& names, int changes, std::uint64_t seed) {
    random_t random(seed);
    for (const std::string_view name : names) {
        const seed_t original(fbe::tests::read(std::filesystem::path(FBE_VIDEO_MEDIA) / name));
        const std::vector<std::uint8_t>& bytes = original.bytes;
        REQUIRE(!bytes.empty());
        const std::string named(name);
        tried(bytes, named, random);
        const std::size_t near = std::min<std::size_t>(bytes.size(), 4096);
        for (std::size_t length = 0; length < near; ++length) {
            tried({ bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length) },
                  named + " cut to " + std::to_string(length), random);
        }
        const std::size_t stride = std::max<std::size_t>(1, (bytes.size() - near) / 48);
        for (std::size_t length = near; length < bytes.size(); length += stride) {
            tried({ bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length) },
                  named + " cut to " + std::to_string(length), random);
        }
        for (int i = 0; i < changes; ++i) {
            tried(fbe::tests::changed(original, random), named + " changed, " + std::to_string(i),
                  random);
        }
    }
    CHECK(fbe_video_count() == 0);
}

} // namespace

TEST_CASE("the test videos, cut short and changed, open and play or are refused", "[stability]") {
    // Each kind of picture group, with sound and without.
    fuzzed(
        {
            "colors.mpg",
            "silent.mpg",
            "countb.mpg",
            "cgop.mpg",
            "ntsc.mpg",
            "opengop.mpg",
            "wide.mpg",
            "cutb.mpg",
        },
        300, 0x9E3779B97F4A7C15ULL);
}

TEST_CASE("the videos that once broke pl_mpeg, cut short and changed, open and play or are "
          "refused",
          "[stability]") {
    fuzzed(
        {
            "broken/heap-read-decode-block.mpg",
            "broken/heap-read-process-macroblock.mpg",
            "broken/heap-write-decode-block.mpg",
            "broken/issue68.mpg",
            "broken/zero1.mpg",
            "broken/zerotail.mpg",
        },
        300, 0xD1B54A32D192ED03ULL);
}
