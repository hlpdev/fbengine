// fbengine_audio_native's C API with another thread mixing, as a device's does: the audio started
// with no device, and fbe_audio_mix called from a thread of the test's own, while the test's main
// thread changes what's playing.

#include "fbengine_audio.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <thread>
#include <vector>

namespace {

constexpr std::int32_t rate = 48000;

/// A mono 16-bit WAV of a second of a 440 Hz sine at a tenth of full scale.
std::vector<std::uint8_t> tone() {
    std::vector<std::uint8_t> bytes;
    const auto add = [&](std::uint32_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            bytes.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
        }
    };
    const std::uint32_t data = rate * 2;
    bytes.insert(bytes.end(), { 'R', 'I', 'F', 'F' });
    add(36 + data, 4);
    bytes.insert(bytes.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
    add(16, 4);
    add(1, 2);
    add(1, 2);
    add(rate, 4);
    add(rate * 2, 4);
    add(2, 2);
    add(16, 2);
    bytes.insert(bytes.end(), { 'd', 'a', 't', 'a' });
    add(data, 4);
    for (int i = 0; i < rate; ++i) {
        const double sample = 3276.0 * std::sin(2.0 * std::numbers::pi * 440.0 * i / rate);
        add(static_cast<std::uint32_t>(static_cast<std::int16_t>(sample)) & 0xFFFFU, 2);
    }
    return bytes;
}

} // namespace

// A low-pass at 0 Hz puts out a full-scale click; taking one away mustn't let the mixing thread
// make it at 0 Hz between the cutoff going and the low-pass going.
TEST_CASE("a bus's low-pass taken away while another thread mixes never clicks") {
    REQUIRE(fbe_audio_start(2, rate, 1));
    const std::vector<std::uint8_t> bytes = tone();
    const std::uint64_t sound =
        fbe_sound_load(bytes.data(), static_cast<std::int64_t>(bytes.size()));
    REQUIRE(sound != 0);
    const std::uint64_t bus = fbe_bus_add(0);
    REQUIRE(fbe_sound_play(sound, 1.0F, 0.0F, 1.0F, true, false, 0.0, bus, 0) != 0);

    std::atomic<bool> done = false;
    float loudest = 0.0F;
    std::thread mixing([&] {
        std::vector<float> samples(64);
        while (!done.load()) {
            fbe_audio_mix(samples.data(), 64);
            for (const float sample : samples) {
                loudest = std::max(loudest, std::abs(sample));
            }
        }
    });
    for (int i = 0; i < 100000; ++i) {
        fbe_bus_set_low_pass(bus, 1000.0F);
        fbe_bus_set_low_pass(bus, 2000.0F);
        fbe_bus_set_low_pass(bus, 0.0F);
    }
    done = true;
    mixing.join();
    CHECK(loudest < 0.2F);
    fbe_audio_stop();
}
