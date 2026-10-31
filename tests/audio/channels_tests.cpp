// An emitter's channel list, which lets go of finished channels as more are added, so an emitter
// that never moves doesn't keep every channel it played.

#include "emitter/channels.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

TEST_CASE("an emitter that never moves keeps only about the channels playing") {
    fbe::audio::channel_list_t channels;
    std::uint64_t newest = 0;
    // Only the last few added are still playing, as with short sounds played one after another.
    const auto playing = [&](std::uint64_t channel) { return channel + 4 > newest; };
    for (newest = 1; newest <= 1'000'000; ++newest) {
        channels.add(newest, playing);
    }
    CHECK(channels.size() <= 64);
}

TEST_CASE("an emitter's channels that are playing are kept, however many") {
    fbe::audio::channel_list_t channels;
    for (std::uint64_t channel = 1; channel <= 1000; ++channel) {
        channels.add(channel, [](std::uint64_t) { return true; });
    }
    CHECK(channels.size() == 1000);
    std::vector<std::uint64_t> seen;
    channels.each([&](std::uint64_t channel) {
        seen.push_back(channel);
        return channel % 2 == 0;
    });
    CHECK(seen.size() == 1000);
    CHECK(channels.size() == 500);
}
