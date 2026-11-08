// A soak of fbengine_audio, its sources built in with tests/own_new.cpp, which counts each of
// their allocations that's live.

#include "fbengine_audio.h"
#include "own_new.hpp"
#include "stability.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

/// Each of the corpus's seeds that's a sound, by name.
std::vector<std::vector<std::uint8_t>> corpus_sounds() {
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(FBE_AUDIO_TEST_CORPUS)) {
        const std::filesystem::path& path = entry.path();
        if (entry.is_regular_file() && path.extension() != ".sh" &&
            !path.filename().string().contains("opus")) {
            paths.push_back(path);
        }
    }
    std::ranges::sort(paths);
    std::vector<std::vector<std::uint8_t>> sounds;
    sounds.reserve(paths.size());
    for (const std::filesystem::path& path : paths) {
        sounds.push_back(fbe::tests::read(path));
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
// device, leave as many allocations live after each round, once the first rounds have warmed it,
// as there were: tests/own_new.cpp counts each of the engine's, built in here, exactly, on any
// platform, so even a round's least, an emitter, left fails. Bytes, which other threads' malloc
// moves, are only shown; LeakSanitizer's nightly run has what malloc itself leaks.
TEST_CASE("thousands of sounds played and freed leave the memory as it was", "[stability]") {
    const std::vector<std::vector<std::uint8_t>> sounds = corpus_sounds();
    REQUIRE(sounds.size() >= 18);
    REQUIRE(fbe_audio_start(2, 48000, 2, nullptr, 0.0));
    for (int round = 0; round < 20; ++round) {
        round_of(sounds, round);
    }
    // Each round's streamed and loaded alternate, so a round is like the one two before it.
    const long even = fbe::tests::live;
    round_of(sounds, 20);
    const long odd = fbe::tests::live;
    const std::uint64_t bytes = fbe::tests::malloc_bytes();
    // Nothing of Catch's in the rounds but round_of's, whose first INFO or failure allocates for
    // good.
    int first_off = -1;
    long off_by = 0;
    for (int round = 21; round < 200; ++round) {
        round_of(sounds, round);
        const long was = round % 2 == 0 ? even : odd;
        if (first_off < 0 && fbe::tests::live != was) {
            first_off = round;
            off_by = fbe::tests::live - was;
        }
    }
    INFO("round " << first_off << " left " << off_by << " allocations live");
    CHECK(first_off == -1);
    WARN("bytes allocated over the warmed rounds: "
         << static_cast<std::int64_t>(fbe::tests::malloc_bytes() - bytes));
    CHECK(fbe_channel_count() == 0);
    fbe_audio_stop();
}
