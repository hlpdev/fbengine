// fbengine_video against the test videos with bytes changed at random, as a file damaged on a disk
// or made to break a decoder might be: each opens or is refused, and each that opens plays, seeks,
// and gives its frames and sound, without reading or writing outside its memory, which a build
// with a sanitizer checks. The bytes changed are the same each run.

#include "fbengine_video.h"
#include "video/decoder.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> read(const std::string& name) {
    std::ifstream file(std::string(FBE_VIDEO_MEDIA) + "/" + name, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

// xorshift64, from a seed.
class random_t {
public:
    explicit random_t(std::uint64_t seed) : m_state(seed) {}

    std::uint64_t next() {
        m_state ^= m_state << 13U;
        m_state ^= m_state >> 7U;
        m_state ^= m_state << 17U;
        return m_state;
    }

    // From 0 to `count`, not counting it.
    std::size_t below(std::size_t count) {
        return static_cast<std::size_t>(next() % count);
    }

private:
    std::uint64_t m_state;
};

// Plays `video` to its end, seeks about it, and takes its frame and sound.
void exercise(std::uint64_t video, random_t& random) {
    fbe_video_play(video);
    for (int i = 0; i < 300 && !fbe_video_ended(video); ++i) {
        fbe_video_update(video, 0.02 * static_cast<double>(1 + random.below(4)));
    }
    const double duration = fbe_video_duration(video);
    fbe_video_seek(video, duration * static_cast<double>(random.below(101)) / 100.0);
    fbe_video_seek(video, duration);
    fbe_video_set_loop(video, true);
    fbe_video_play(video);
    fbe_video_update(video, duration * 3.5);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(fbe_video_width(video)) *
                                     static_cast<std::size_t>(fbe_video_height(video)) * 4);
    CHECK(fbe_video_frame(video, pixels.data(), static_cast<std::int64_t>(pixels.size())));
    if (fbe_video_has_sound(video)) {
        std::vector<std::uint8_t> wav(static_cast<std::size_t>(fbe_video_sound(video, nullptr, 0)));
        CHECK(fbe_video_sound(video, wav.data(), static_cast<std::int64_t>(wav.size())) ==
              static_cast<std::int64_t>(wav.size()));
    }
}

} // namespace

TEST_CASE("videos with bytes changed at random open or are refused, and play to their ends") {
    const std::array<std::vector<std::uint8_t>, 7> videos{
        read("colors.mpg"), read("countb.mpg"), read("cgop.mpg"), read("ntsc.mpg"),
        read("busy.mpg"),   read("late.mpg"),   read("gap.mpg"),
    };
    for (const std::vector<std::uint8_t>& video : videos) {
        REQUIRE(!video.empty());
    }
    random_t random(0x2545F4914F6CDD1DULL);
    int opened = 0;
    for (int i = 0; i < 300; ++i) {
        std::vector<std::uint8_t> bytes = videos[random.below(videos.size())];
        const std::size_t changes = 1 + random.below(16);
        for (std::size_t c = 0; c < changes; ++c) {
            bytes[random.below(bytes.size())] = static_cast<std::uint8_t>(random.next());
        }
        // Some cut short, too.
        if (random.below(4) == 0) {
            bytes.resize(1 + random.below(bytes.size()));
        }
        const std::uint64_t video =
            fbe_video_open(bytes.data(), static_cast<std::int64_t>(bytes.size()));
        if (video == 0) {
            continue;
        }
        ++opened;
        exercise(video, random);
        CHECK(fbe_video_free(video));
    }
    // Most bytes changed are a picture's or a sound's, which still open.
    CHECK(opened > 150);
    CHECK(fbe_video_count() == 0);
}

// A sound longer than a WAV's 32-bit sizes hold, 3.4 hours of two channels at 44100 Hz, is refused
// as the video's opened; a test video that long would be 50 MB, so this is the arithmetic.
TEST_CASE("a WAV holds 4 GB of samples, less its header") {
    CHECK(fbe::video::wav_holds(0));
    CHECK(fbe::video::wav_holds(fbe::video::most_sound_bytes));
    CHECK_FALSE(fbe::video::wav_holds(fbe::video::most_sound_bytes + 1));
    // Its RIFF size, 36 more than its samples', fits 32 bits.
    CHECK(fbe::video::most_sound_bytes + 36 <= 0xFFFFFFFFULL);
    const std::uint64_t hours = 3600ULL * 44100 * 2 * 4;
    CHECK(fbe::video::wav_holds(3 * hours));
    CHECK_FALSE(fbe::video::wav_holds(4 * hours));
}
