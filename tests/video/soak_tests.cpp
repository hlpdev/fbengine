// A soak: each test video opened, played to its end, sought, looped, its frame and sound taken,
// and freed, a thousand times over, leaves nothing open and the memory as it was once the first
// rounds have warmed it.

#include "fbengine_video.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
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

namespace {

std::vector<std::uint8_t> read(const std::string& name) {
    std::ifstream file(std::string(FBE_VIDEO_MEDIA) + "/" + name, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

/// What the process has allocated, as tests/audio/memory_tests.cpp counts it: its committed memory
/// on Windows, its malloc zones' bytes in use on macOS, and its virtual size on Linux.
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

/// Whether a sanitizer's allocator is built in, which keeps what's freed mapped a while, so the
/// process's size says nothing of a leak: LeakSanitizer says that there.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool sanitized = true;
#elifdef __has_feature
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool sanitized = true;
#else
constexpr bool sanitized = false;
#endif
#else
constexpr bool sanitized = false;
#endif

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

TEST_CASE("a thousand videos played and freed leave nothing open, and the memory as it was") {
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
    for (int round = 20; round < 1000; ++round) {
        played(videos[static_cast<std::size_t>(round) % videos.size()]);
    }
    const std::uint64_t after = allocated();
    INFO("before " << before << ", after " << after);
    if (!sanitized) {
        CHECK(after < before + (std::uint64_t{ 8 } << 20U));
    }
    CHECK(fbe_video_count() == 0);
}
