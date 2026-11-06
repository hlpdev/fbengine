// What fbengine_audio_native's C API leaves allocated: a corrupt file's failed load and stream,
// over and over, as a game's retries would, add nothing.

#include "fbengine_audio.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
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

/// The bytes of the test data file `name`, or none.
std::vector<std::uint8_t> read(const std::string& name) {
    std::ifstream file(std::string(FBE_AUDIO_TEST_DATA) + "/" + name, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

/// Whether a sanitizer's allocator is built in, which keeps what's freed mapped a while, so the
/// process's size says nothing of a leak: LeakSanitizer says that there.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool sanitized = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool sanitized = true;
#else
constexpr bool sanitized = false;
#endif
#else
constexpr bool sanitized = false;
#endif

/// What the process has allocated: its committed memory on Windows, its malloc zones' bytes in use
/// on macOS, and its virtual size on Linux, where a large malloc maps its own. Each counts a block
/// that's allocated and never touched.
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

} // namespace

// leak16.ogg's sparse codebook says 16 MB of temporary lengths, and an error return before their
// free left them, on each of the two tries miniaudio makes a call: 32 MB a load or stream.
TEST_CASE("ACorruptOggsFailedLoadsLeakNothing") {
    const std::vector<std::uint8_t> bytes = read("leak16.ogg");
    REQUIRE(!bytes.empty());
    const auto size = static_cast<std::int64_t>(bytes.size());
    const auto fail = [&] {
        CHECK(fbe_sound_load(bytes.data(), size) == 0);
        CHECK(fbe_sound_stream(bytes.data(), size) == 0);
    };
    fail();
    const std::uint64_t before = allocated();
    for (int i = 0; i < 20; ++i) {
        fail();
    }
    // The 40 calls leaked 1.25 GB before.
    if (!sanitized) {
        CHECK(allocated() < before + (std::uint64_t{ 64 } << 20U));
    }
}

namespace {

/// Each of the corpus's seeds that's a sound, by name.
std::vector<std::vector<std::uint8_t>> corpus_sounds() {
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(FBE_AUDIO_TEST_CORPUS)) {
        const std::filesystem::path& path = entry.path();
        if (entry.is_regular_file() && path.extension() != ".sh" &&
            path.filename().string().find("opus") == std::string::npos) {
            paths.push_back(path);
        }
    }
    std::ranges::sort(paths);
    std::vector<std::vector<std::uint8_t>> sounds;
    for (const std::filesystem::path& path : paths) {
        std::ifstream file(path, std::ios::binary);
        sounds.emplace_back(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    return sounds;
}

/// A round of what a game does with sounds: each of `sounds` loaded and streamed, played into a
/// bus, from an emitter or not, mixed a while, some freed while they play, and everything let go.
void round_of(const std::vector<std::vector<std::uint8_t>>& sounds, int round) {
    const std::uint64_t bus = fbe_bus_add(0);
    const std::uint64_t emitter = fbe_emitter_add();
    fbe_bus_set_reverb(bus, 0.5F, 0.5F, 1.0F, 0.3F, 1.0F);
    std::vector<std::uint64_t> made;
    std::vector<std::uint64_t> loops;
    for (std::size_t at = 0; at < sounds.size(); ++at) {
        const std::vector<std::uint8_t>& bytes = sounds[at];
        const auto size = static_cast<std::int64_t>(bytes.size());
        const bool streamed = (static_cast<std::size_t>(round) + at) % 2 == 0;
        const std::uint64_t sound =
            streamed ? fbe_sound_stream(bytes.data(), size) : fbe_sound_load(bytes.data(), size);
        INFO("sound " << at << ", round " << round << ": " << fbe_audio_error());
        REQUIRE(sound != 0);
        const bool loop = at % 3 == 0;
        const std::uint64_t channel = fbe_sound_play(sound, 0.5F, 0.0F, 1.0F, loop, false, 0.0, bus,
                                                     at % 2 == 0 ? emitter : 0);
        REQUIRE(channel != 0);
        if (loop) {
            loops.push_back(channel);
        }
        made.push_back(sound);
    }
    std::vector<float> samples(2048);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(fbe_audio_mix(samples.data(), 1024));
    }
    // Freed under what plays them, as a game's scene going does.
    for (const std::uint64_t sound : made) {
        fbe_sound_free(sound);
    }
    fbe_bus_free(bus);
    fbe_emitter_free(emitter);
    // A loop plays on without its sound until it's stopped, as a game's music is.
    for (const std::uint64_t channel : loops) {
        fbe_channel_stop(channel);
    }
    for (int i = 0; i < 4; ++i) {
        REQUIRE(fbe_audio_mix(samples.data(), 1024));
    }
}

} // namespace

// A soak: thousands of sounds loaded, streamed, played, and freed, with buses and emitters, on no
// device, leave the memory as it was once the first rounds have warmed it.
TEST_CASE("thousands of sounds played and freed leave the memory as it was") {
    const std::vector<std::vector<std::uint8_t>> sounds = corpus_sounds();
    REQUIRE(sounds.size() >= 18);
    REQUIRE(fbe_audio_start(2, 48000, 2, nullptr, 0.0));
    for (int round = 0; round < 20; ++round) {
        round_of(sounds, round);
    }
    const std::uint64_t before = allocated();
    for (int round = 20; round < 200; ++round) {
        round_of(sounds, round);
    }
    const std::uint64_t after = allocated();
    INFO("before " << before << ", after " << after);
    // 180 rounds of 19 sounds each, 3420, and their channels, buses, and emitters.
    if (!sanitized) {
        CHECK(after < before + (std::uint64_t{ 8 } << 20U));
    }
    CHECK(fbe_channel_count() == 0);
    fbe_audio_stop();
}
