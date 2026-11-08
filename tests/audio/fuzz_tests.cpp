// The decoders against what a game's files can be: each seed of tests/audio/corpus (corpus/make.sh
// makes them) and the package's broken files, cut short at every length to 4 KB and at a stride
// after, and with bytes changed, put in, taken out, and repeated, half of them in a header, from a
// fixed seed, so a failure is the same each run. Each is loaded whole and streamed, and each that
// loads plays to its end in the mix, sought and looped on the way: a sound or an error, never a
// crash, a hang, or a read outside its bytes, which a build with a sanitizer checks.

#include "fbengine_audio.h"
#include "stability.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

using fbe::tests::random_t;
using fbe::tests::seed_t;

namespace {

constexpr std::int32_t rate = 48000;
constexpr std::int32_t period = 1024;

/// The corpus's seeds that end in one of `endings`, and what fuzzing found in them, in
/// corpus/found, by name, so their order is the same everywhere.
std::vector<std::filesystem::path> seeds(std::initializer_list<std::string_view> endings) {
    std::vector<std::filesystem::path> found;
    const std::filesystem::path corpus(FBE_AUDIO_TEST_CORPUS);
    for (const std::filesystem::path& folder : { corpus, corpus / "found" }) {
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (std::ranges::find(endings, entry.path().extension().string()) != endings.end()) {
                found.push_back(entry.path());
            }
        }
    }
    std::ranges::sort(found);
    return found;
}

/// Mixes `periods` periods.
void mix(std::int64_t periods) {
    std::vector<float> samples(static_cast<std::size_t>(period) * 2);
    for (std::int64_t i = 0; i < periods; ++i) {
        REQUIRE(fbe_audio_mix(samples.data(), period));
    }
}

/// Plays `sound` once to its end in the mix, sought three times at random and looped a while on
/// the way, which from its last change of place it must reach in its frames at the mix's rate and
/// a few periods more.
void played(std::uint64_t sound, random_t& random) {
    const std::int64_t frames = fbe_sound_frames(sound);
    const std::int32_t own_rate = fbe_sound_sample_rate(sound);
    REQUIRE(frames > 0);
    REQUIRE(own_rate >= 8000);
    REQUIRE(own_rate <= 384000);
    const std::uint64_t channel = fbe_sound_play(sound, 1.0F, 0.0F, 1.0F, false, false, 0.0, 0, 0);
    REQUIRE(channel != 0);
    const double seconds = static_cast<double>(frames) / own_rate;
    for (int seek = 0; seek < 3; ++seek) {
        mix(static_cast<std::int64_t>(random.below(3)));
        fbe_channel_seek(channel, seconds * random.unit());
    }
    fbe_channel_set_loop(channel, true);
    mix(1 + static_cast<std::int64_t>(random.below(3)));
    fbe_channel_set_loop(channel, false);
    const std::int64_t heard = (frames * rate / own_rate) + 1;
    const std::int64_t most = (heard / period) + 8;
    std::int64_t mixed = 0;
    while (fbe_channel_playing(channel) && mixed < most) {
        mix(1);
        ++mixed;
    }
    CHECK(!fbe_channel_playing(channel));
    fbe_channel_stop(channel);
}

/// Loads and streams `bytes`, as `what` says they were made, and plays what loads. With
/// FBE_FUZZ_LAST set to a file's path, the bytes are written there first, so a sanitizer's stop
/// leaves what stopped it.
void tried(const std::vector<std::uint8_t>& bytes, const std::string& what, random_t& random) {
    INFO(what);
    // NOLINTNEXTLINE(clang-diagnostic-deprecated-declarations): only read here.
    if (const char* last = std::getenv("FBE_FUZZ_LAST")) {
        std::ofstream file(last, std::ios::binary);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): ofstream takes chars.
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    const auto size = static_cast<std::int64_t>(bytes.size());
    for (const bool stream : { false, true }) {
        INFO((stream ? "streamed" : "loaded"));
        const std::uint64_t sound =
            stream ? fbe_sound_stream(bytes.data(), size) : fbe_sound_load(bytes.data(), size);
        if (sound == 0) {
            CHECK(!std::string_view(fbe_audio_error()).empty());
            continue;
        }
        played(sound, random);
        fbe_sound_free(sound);
    }
}

/// Every seed of `files`, cut short at each length to 4096 and at 96 more after, and changed 1000
/// ways, all from `seed`.
void fuzzed(const std::vector<std::filesystem::path>& files, std::uint64_t seed) {
    REQUIRE(!files.empty());
    REQUIRE(fbe_audio_start(2, rate, 2, nullptr, 0.0));
    random_t random(seed);
    for (const std::filesystem::path& path : files) {
        const seed_t original(fbe::tests::read(path));
        const std::vector<std::uint8_t>& bytes = original.bytes;
        REQUIRE(!bytes.empty());
        const std::string name = path.filename().string();
        tried(bytes, name, random);
        const std::size_t near = std::min<std::size_t>(bytes.size(), 4096);
        for (std::size_t length = 0; length < near; ++length) {
            tried({ bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length) },
                  name + " cut to " + std::to_string(length), random);
        }
        const std::size_t stride = std::max<std::size_t>(1, (bytes.size() - near) / 96);
        for (std::size_t length = near; length < bytes.size(); length += stride) {
            tried({ bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length) },
                  name + " cut to " + std::to_string(length), random);
        }
        for (int i = 0; i < 1000; ++i) {
            tried(fbe::tests::changed(original, random), name + " changed, " + std::to_string(i),
                  random);
        }
    }
    CHECK(fbe_channel_count() == 0);
    fbe_audio_stop();
}

} // namespace

// What's fuzzed reaches each decoder's parser only from a seed that's a sound to start with.
TEST_CASE("every seed of the corpus is a sound, but Opus, which miniaudio doesn't read") {
    REQUIRE(fbe_audio_start(2, rate, 2, nullptr, 0.0));
    const std::vector<std::filesystem::path> files =
        seeds({ ".wav", ".w64", ".aiff", ".flac", ".ogg" });
    CHECK(files.size() >= 23);
    for (const std::filesystem::path& path : files) {
        const std::string name = path.filename().string();
        if (path.parent_path().filename() == "found") {
            continue;
        }
        INFO(name);
        const std::vector<std::uint8_t> bytes = fbe::tests::read(path);
        const std::uint64_t sound =
            fbe_sound_load(bytes.data(), static_cast<std::int64_t>(bytes.size()));
        if (name.contains("opus")) {
            CHECK(sound == 0);
            continue;
        }
        if (sound == 0) {
            FAIL(fbe_audio_error());
        }
        // A tenth of a second at the seed's own rate, and an ADPCM block's or a Vorbis packet's
        // more.
        const double seconds =
            static_cast<double>(fbe_sound_frames(sound)) / fbe_sound_sample_rate(sound);
        CHECK(seconds >= 0.099);
        CHECK(seconds < 0.15);
        fbe_sound_free(sound);
    }
    fbe_audio_stop();
}

// Fuzzing found a W64 whose chunk says it's 2^64 bytes, which dr_wav stepped over 2 GB at a time,
// each step a seek past the end of its bytes that said it went all the way: minutes, until
// native/fbengine_audio/CMakeLists.txt's patch made such a seek fail.
TEST_CASE("a W64 whose chunk says it's 2^64 bytes is refused at once") {
    REQUIRE(fbe_audio_start(2, rate, 2, nullptr, 0.0));
    const std::vector<std::uint8_t> bytes = fbe::tests::read(
        std::filesystem::path(FBE_AUDIO_TEST_CORPUS) / "found" / "w64_chunk_of_2_to_the_64.w64");
    REQUIRE(bytes.size() == 17774);
    const auto size = static_cast<std::int64_t>(bytes.size());
    for (const bool stream : { false, true }) {
        INFO((stream ? "streamed" : "loaded"));
        const auto started = std::chrono::steady_clock::now();
        const std::uint64_t sound =
            stream ? fbe_sound_stream(bytes.data(), size) : fbe_sound_load(bytes.data(), size);
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
        CHECK(sound == 0);
    }
    fbe_audio_stop();
}

TEST_CASE("every WAV, W64, RF64, and AIFF of the corpus, cut short and changed, is a sound or an "
          "error",
          "[stability]") {
    fuzzed(seeds({ ".wav", ".w64", ".aiff" }), 0x9E3779B97F4A7C15ULL);
}

TEST_CASE("every FLAC of the corpus, cut short and changed, is a sound or an error",
          "[stability]") {
    fuzzed(seeds({ ".flac" }), 0xD1B54A32D192ED03ULL);
}

TEST_CASE("every Ogg of the corpus, Vorbis and FLAC, cut short and changed, is a sound or an "
          "error",
          "[stability]") {
    fuzzed(seeds({ ".ogg" }), 0x8CB92BA72F3D8DD7ULL);
}

// The files the package's tests keep for what once broke a decoder, cut short and changed too.
TEST_CASE("the package's broken files, cut short and changed, are sounds or errors",
          "[stability]") {
    std::vector<std::filesystem::path> files;
    constexpr std::array<std::string_view, 8> names{
        "corrupt.ogg", "forged.flac", "forged.ogg", "forged.wav",
        "leak16.ogg",  "rate.ogg",    "rate.wav",   "short.ogg",
    };
    files.reserve(names.size());
    for (const std::string_view name : names) {
        files.emplace_back(std::filesystem::path(FBE_AUDIO_TEST_DATA) / name);
    }
    fuzzed(files, 0x2545F4914F6CDD1DULL);
}
