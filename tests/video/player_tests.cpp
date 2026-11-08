// fbengine_video's player, given what a game's bugs might: its calls in any order, with the ends of
// a double and nonsense, on videos playing and freed, each refused or played on, its clock in the
// video; and hundreds of videos opened, played, and freed, as a long game's would be, leaving
// nothing behind.

#include "fbengine_video.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
// Without windows.h's min and max, which std::numeric_limits' are taken for.
#define NOMINMAX
// psapi.h needs windows.h's types first.
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#elifdef __APPLE__
#include <malloc/malloc.h>
#else
#include <unistd.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define FBE_UNDER_ASAN
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FBE_UNDER_ASAN
#endif
#endif

namespace {

std::vector<std::uint8_t> read(const std::string& name) {
    std::ifstream file(std::string(FBE_VIDEO_MEDIA) + "/" + name, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

/// What the process has allocated: its committed memory on Windows, its malloc zones' bytes in use
/// on macOS, and its virtual size on Linux, where a large malloc maps its own.
std::uint64_t allocated() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): psapi's way to the _EX fields.
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                         sizeof(counters));
    return counters.PrivateUsage;
#elifdef __APPLE__
    malloc_statistics_t statistics{};
    malloc_zone_statistics(nullptr, &statistics);
    return statistics.size_in_use;
#else
    std::ifstream statm("/proc/self/statm");
    std::uint64_t pages = 0;
    statm >> pages;
    return pages * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
#endif
}

// xorshift64, from a seed, so the calls are the same each run.
class random_t {
public:
    explicit random_t(std::uint64_t seed) : m_state(seed) {}

    std::uint64_t next() {
        m_state ^= m_state << 13U;
        m_state ^= m_state >> 7U;
        m_state ^= m_state << 17U;
        return m_state;
    }

    std::size_t below(std::size_t count) {
        return static_cast<std::size_t>(next() % count);
    }

private:
    std::uint64_t m_state;
};

// A time a game might give, by mistake or not: the ends of a double, around 0, a billion seconds,
// and around the video's length.
double some_time(random_t& random, double duration) {
    const std::array<double, 16> times{
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::denorm_min(),
        -0.0,
        0.0,
        -1.0,
        1.0e9,
        1.0e9 + 1.0,
        duration,
        std::nextafter(duration, 1.0e300),
        std::nextafter(duration, 0.0),
        1.0 / 60,
        0.25,
    };
    if (random.below(3) == 0) {
        return duration * static_cast<double>(random.below(1001)) / 1000.0;
    }
    return times[random.below(times.size())];
}

// Whether `video`'s clock and frame are in it.
void in_the_video(std::uint64_t video, double duration) {
    const double time = fbe_video_time(video);
    CHECK(std::isfinite(time));
    CHECK(time >= 0.0);
    CHECK(time <= duration);
    const std::int64_t frame = fbe_video_frame_number(video);
    CHECK(frame >= 0);
    CHECK(static_cast<double>(frame) <= (duration * fbe_video_frame_rate(video)) + 1.0);
}

} // namespace

TEST_CASE("a video's calls, in any order, with the ends of a double, are refused or play on",
          "[stability]") {
    const std::array<std::vector<std::uint8_t>, 4> videos{
        read("count.mpg"),
        read("colors.mpg"),
        read("longsound.mpg"),
        read("cgop.mpg"),
    };
    random_t random(0x9E3779B97F4A7C15ULL);
    for (const std::vector<std::uint8_t>& bytes : videos) {
        REQUIRE(!bytes.empty());
        const std::uint64_t video =
            fbe_video_open(bytes.data(), static_cast<std::int64_t>(bytes.size()));
        REQUIRE(video != 0);
        const double duration = fbe_video_duration(video);
        const auto pixels = static_cast<std::size_t>(fbe_video_width(video)) *
                            static_cast<std::size_t>(fbe_video_height(video)) * 4;
        std::vector<std::uint8_t> frame(pixels + 8);
        for (int i = 0; i < 3000; ++i) {
            switch (random.below(9)) {
            case 0:
                fbe_video_play(video);
                break;
            case 1:
                fbe_video_pause(video);
                break;
            case 2:
                fbe_video_set_loop(video, random.below(2) == 0);
                break;
            case 3:
            case 4:
                fbe_video_update(video, some_time(random, duration));
                break;
            case 5:
                fbe_video_seek(video, some_time(random, duration));
                break;
            case 6: {
                // The right size, or one off, or none.
                const std::array<std::int64_t, 4> sizes{
                    static_cast<std::int64_t>(pixels),
                    static_cast<std::int64_t>(pixels) - 1,
                    static_cast<std::int64_t>(pixels) + 1,
                    -1,
                };
                const std::int64_t size = sizes[random.below(sizes.size())];
                CHECK(fbe_video_frame(video, frame.data(), size) == (std::cmp_equal(size, pixels)));
                break;
            }
            case 7: {
                const std::int64_t whole = fbe_video_sound(video, nullptr, 0);
                std::vector<std::uint8_t> wav(
                    static_cast<std::size_t>(std::max<std::int64_t>(whole, 0)));
                const std::array<std::int64_t, 3> capacities{ -1, whole - 1, whole };
                const std::int64_t capacity = capacities[random.below(capacities.size())];
                fbe_video_sound(video, capacity >= 0 ? wav.data() : nullptr, capacity);
                break;
            }
            default:
                fbe_video_ended(video);
                fbe_video_frames_shown(video);
                fbe_video_playing(video);
                fbe_video_looping(video);
                break;
            }
            in_the_video(video, duration);
        }
        CHECK(fbe_video_free(video));
        // Freed, each call says so and does nothing.
        CHECK_FALSE(fbe_video_play(video));
        CHECK_FALSE(fbe_video_update(video, 1.0));
        CHECK_FALSE(fbe_video_seek(video, 0.0));
        CHECK_FALSE(fbe_video_frame(video, frame.data(), static_cast<std::int64_t>(pixels)));
        CHECK(fbe_video_sound(video, nullptr, 0) < 0);
        CHECK_FALSE(fbe_video_free(video));
        CHECK_FALSE(fbe_video_free(0));
    }
    CHECK(fbe_video_count() == 0);
}

TEST_CASE("hundreds of videos opened, played, and freed leave nothing", "[stability]") {
    const std::vector<std::uint8_t> bytes = read("busy.mpg");
    REQUIRE(!bytes.empty());
    // How much the round's videos held, all open, more than before it.
    std::uint64_t held = 0;
    const auto round = [&bytes, &held] {
        const std::uint64_t start = allocated();
        // Many at once, each with its thread decoding, freed playing.
        std::vector<std::uint64_t> videos;
        for (int i = 0; i < 20; ++i) {
            const std::uint64_t video =
                fbe_video_open(bytes.data(), static_cast<std::int64_t>(bytes.size()));
            REQUIRE(video != 0);
            fbe_video_set_loop(video, i % 2 == 0);
            fbe_video_play(video);
            fbe_video_update(video, 0.1 * static_cast<double>(i % 5));
            videos.push_back(video);
        }
        const std::uint64_t open = allocated();
        held = open > start ? open - start : 0;
        for (const std::uint64_t video : videos) {
            CHECK(fbe_video_free(video));
        }
    };
    // The allocator's pools, and the decoder's first frames, start in the first rounds.
    for (int i = 0; i < 3; ++i) {
        round();
    }
    const std::uint64_t before = allocated();
    for (int i = 0; i < 30; ++i) {
        round();
    }
    CHECK(fbe_video_count() == 0);
    // 600 videos freed leave less than a round's 20 hold.
    INFO("a round's videos held " << held << " bytes");
    CHECK(held > 0);
    // ASan keeps what's freed a while before it's used again, so bytes grow under it; its leak
    // check, and leak_tests' count, see a leak there.
#ifndef FBE_UNDER_ASAN
    CHECK(allocated() < before + held);
#endif
}
