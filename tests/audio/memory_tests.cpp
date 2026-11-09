// What fbengine_audio_native's C API leaves allocated: a corrupt file's failed load and stream,
// over and over, as a game's retries would, add nothing.

#include "fbengine_audio.h"
#include "stability.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using fbe::tests::malloc_bytes;
using fbe::tests::sanitized;

namespace {

/// The bytes of the test data file `name`, or none.
std::vector<std::uint8_t> read(const std::string& name) {
    return fbe::tests::read(std::filesystem::path(FBE_AUDIO_TEST_DATA) / name);
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
    const std::uint64_t before = malloc_bytes();
    for (int i = 0; i < 20; ++i) {
        fail();
    }
    // The 40 calls leaked 1.25 GB before.
    if (!sanitized) {
        CHECK(malloc_bytes() < before + (std::uint64_t{ 64 } << 20U));
    }
}
