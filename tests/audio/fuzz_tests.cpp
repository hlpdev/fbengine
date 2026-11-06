// The decoders against what a game's files can be: each seed of tests/audio/corpus (corpus/make.sh
// makes them) and the package's broken files, cut short at every length to 4 KB and at a
// stride after, and with bytes changed, put in, taken out, and repeated, from a fixed seed, so a
// failure is the same each run. Each is loaded whole and streamed, and each that loads plays to
// its end in the mix: a sound or an error, never a crash, a hang, or a read outside its bytes,
// which a build with a sanitizer checks.

#include "fbengine_audio.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::int32_t rate = 48000;
constexpr std::int32_t period = 1024;

std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

/// The corpus's seeds that end in `ending`, and what fuzzing found in them, in corpus/found, by
/// name, so their order is the same everywhere.
std::vector<std::filesystem::path> seeds(std::string_view ending) {
    std::vector<std::filesystem::path> found;
    const std::filesystem::path corpus(FBE_AUDIO_TEST_CORPUS);
    for (const std::filesystem::path& folder : { corpus, corpus / "found" }) {
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (entry.path().extension() == ending) {
                found.push_back(entry.path());
            }
        }
    }
    std::ranges::sort(found);
    return found;
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
        return count == 0 ? 0 : static_cast<std::size_t>(next() % count);
    }

private:
    std::uint64_t m_state;
};

/// What a decoder checks a value against, more often than a random byte is.
constexpr std::array<std::uint8_t, 8> edges{ 0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF, 0x10, 0x40 };

/// `bytes`, with one to four changes of `random`'s: a byte set, a byte set to an edge, bytes put
/// in, taken out, repeated after themselves, or zeroed.
std::vector<std::uint8_t> changed(std::vector<std::uint8_t> bytes, random_t& random) {
    const std::size_t changes = 1 + random.below(4);
    for (std::size_t c = 0; c < changes && !bytes.empty(); ++c) {
        const std::size_t at = random.below(bytes.size());
        const std::size_t length = std::min<std::size_t>(1 + random.below(32), bytes.size() - at);
        const auto from = bytes.begin() + static_cast<std::ptrdiff_t>(at);
        switch (random.below(6)) {
        case 0:
            bytes[at] = static_cast<std::uint8_t>(random.next());
            break;
        case 1:
            bytes[at] = edges.at(random.below(edges.size()));
            break;
        case 2: {
            std::vector<std::uint8_t> put(length);
            for (std::uint8_t& byte : put) {
                byte = static_cast<std::uint8_t>(random.next());
            }
            bytes.insert(from, put.begin(), put.end());
            break;
        }
        case 3:
            bytes.erase(from, from + static_cast<std::ptrdiff_t>(length));
            break;
        case 4: {
            const std::vector<std::uint8_t> again(from, from + static_cast<std::ptrdiff_t>(length));
            bytes.insert(from + static_cast<std::ptrdiff_t>(length), again.begin(), again.end());
            break;
        }
        default:
            std::fill(from, from + static_cast<std::ptrdiff_t>(length), std::uint8_t{ 0 });
            break;
        }
    }
    return bytes;
}

/// Plays `sound` once to its end in the mix, which it must reach in its frames at the mix's rate
/// and a few periods more.
void played(std::uint64_t sound) {
    const std::int64_t frames = fbe_sound_frames(sound);
    const std::int32_t own_rate = fbe_sound_sample_rate(sound);
    REQUIRE(frames > 0);
    REQUIRE(own_rate >= 8000);
    REQUIRE(own_rate <= 384000);
    const std::uint64_t channel = fbe_sound_play(sound, 1.0F, 0.0F, 1.0F, false, false, 0.0, 0, 0);
    REQUIRE(channel != 0);
    const std::int64_t heard = (frames * rate / own_rate) + 1;
    const std::int64_t most = (heard / period) + 8;
    std::vector<float> samples(static_cast<std::size_t>(period) * 2);
    std::int64_t mixed = 0;
    while (fbe_channel_playing(channel) && mixed < most) {
        REQUIRE(fbe_audio_mix(samples.data(), period));
        ++mixed;
    }
    CHECK(!fbe_channel_playing(channel));
    fbe_channel_stop(channel);
}

/// Loads and streams `bytes`, as `what` says it was made, and plays what loads. With
/// FBE_FUZZ_LAST set to a file's path, the bytes are written there first, so a sanitizer's stop
/// leaves what stopped it.
void tried(const std::vector<std::uint8_t>& bytes, const std::string& what) {
    INFO(what);
    if (const char* last = std::getenv("FBE_FUZZ_LAST")) {
        std::ofstream file(last, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), // NOLINT: ofstream takes chars.
                   static_cast<std::streamsize>(bytes.size()));
    }
    const auto size = static_cast<std::int64_t>(bytes.size());
    for (const bool stream : { false, true }) {
        INFO((stream ? "streamed" : "loaded"));
        const std::uint64_t sound =
            stream ? fbe_sound_stream(bytes.data(), size) : fbe_sound_load(bytes.data(), size);
        if (sound == 0) {
            CHECK(std::string_view(fbe_audio_error()).size() > 0);
            continue;
        }
        played(sound);
        fbe_sound_free(sound);
    }
}

/// Every seed of `files`, cut short at each length to 4096 and at 96 more after, and changed 1000
/// ways, all from `seed`.
void fuzzed(const std::vector<std::filesystem::path>& files, std::uint64_t seed) {
    REQUIRE(fbe_audio_start(2, rate, 2, nullptr, 0.0));
    random_t random(seed);
    for (const std::filesystem::path& path : files) {
        const std::vector<std::uint8_t> original = read(path);
        REQUIRE(!original.empty());
        const std::string name = path.filename().string();
        tried(original, name);
        const std::size_t near = std::min<std::size_t>(original.size(), 4096);
        for (std::size_t length = 0; length < near; ++length) {
            tried({ original.begin(), original.begin() + static_cast<std::ptrdiff_t>(length) },
                  name + " cut to " + std::to_string(length));
        }
        const std::size_t stride = std::max<std::size_t>(1, (original.size() - near) / 96);
        for (std::size_t length = near; length < original.size(); length += stride) {
            tried({ original.begin(), original.begin() + static_cast<std::ptrdiff_t>(length) },
                  name + " cut to " + std::to_string(length));
        }
        for (int i = 0; i < 1000; ++i) {
            tried(changed(original, random), name + " changed, " + std::to_string(i));
        }
    }
    CHECK(fbe_channel_count() == 0);
    fbe_audio_stop();
}

} // namespace

TEST_CASE("every WAV of the corpus, cut short and changed, is a sound or an error") {
    fuzzed(seeds(".wav"), 0x9E3779B97F4A7C15ULL);
}

TEST_CASE("every FLAC of the corpus, cut short and changed, is a sound or an error") {
    fuzzed(seeds(".flac"), 0xD1B54A32D192ED03ULL);
}

TEST_CASE("every Ogg of the corpus, cut short and changed, is a sound or an error") {
    fuzzed(seeds(".ogg"), 0x8CB92BA72F3D8DD7ULL);
}

// The files the package's tests keep for what once broke a decoder, cut short and changed too.
TEST_CASE("the package's broken files, cut short and changed, are sounds or errors") {
    std::vector<std::filesystem::path> files;
    for (const std::string_view name : { "corrupt.ogg", "forged.flac", "forged.ogg", "forged.wav",
                                         "leak16.ogg", "rate.ogg", "rate.wav", "short.ogg" }) {
        files.emplace_back(std::filesystem::path(FBE_AUDIO_TEST_DATA) / name);
    }
    fuzzed(files, 0x2545F4914F6CDD1DULL);
}
