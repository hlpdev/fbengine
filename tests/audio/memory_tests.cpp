// What fbengine_audio_native's C API leaves allocated: a corrupt file's failed load and stream,
// over and over, as a game's retries would, add nothing.

#include "fbengine_audio.h"

#include <catch2/catch_test_macros.hpp>
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

/// The bytes of the test data file `name`, or none.
std::vector<std::uint8_t> read(const std::string& name) {
    std::ifstream file(std::string(FBE_AUDIO_TEST_DATA) + "/" + name, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

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
    CHECK(allocated() < before + (std::uint64_t{ 64 } << 20U));
}
