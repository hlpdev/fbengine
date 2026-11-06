// fbengine_audio_native's C API with another thread mixing, as a device's does: the audio started
// with no device, and fbe_audio_mix called from a thread of the test's own, while the test's main
// thread changes what's playing.

#include "fbengine_audio.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stop_token>
#include <thread>
#include <vector>

namespace {

constexpr std::int32_t rate = 48000;

/// A mono 16-bit WAV of `samples`, at 48000 Hz.
std::vector<std::uint8_t> wav(const std::vector<std::int16_t>& samples) {
    std::vector<std::uint8_t> bytes;
    const auto add = [&](std::uint32_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            bytes.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
        }
    };
    const auto data = static_cast<std::uint32_t>(samples.size() * 2);
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
    for (const std::int16_t sample : samples) {
        add(static_cast<std::uint32_t>(static_cast<std::uint16_t>(sample)), 2);
    }
    return bytes;
}

/// A second of a 440 Hz sine at a tenth of full scale.
std::vector<std::uint8_t> tone() {
    std::vector<std::int16_t> samples(rate);
    for (int i = 0; i < rate; ++i) {
        samples[static_cast<std::size_t>(i)] =
            static_cast<std::int16_t>(3276.0 * std::sin(2.0 * std::numbers::pi * 440.0 * i / rate));
    }
    return wav(samples);
}

/// Plays a looping sound of `bytes` into `bus`, or the whole mix for 0, giving its channel.
std::uint64_t loop(const std::vector<std::uint8_t>& bytes, std::uint64_t bus) {
    const std::uint64_t sound =
        fbe_sound_load(bytes.data(), static_cast<std::int64_t>(bytes.size()));
    const std::uint64_t channel = fbe_sound_play(sound, 1.0F, 0.0F, 1.0F, true, false, 0.0, bus, 0);
    fbe_sound_free(sound);
    return channel;
}

/// Runs `change` on this thread while another mixes, as a device's does, giving the loudest sample
/// on each side the mix made meanwhile.
template <typename Change>
std::pair<float, float> mixed_beside(Change change) {
    float left = 0.0F;
    float right = 0.0F;
    // A jthread, which a REQUIRE's failure stops and joins on its way out.
    std::jthread mixing([&](const std::stop_token& stop) {
        std::vector<float> samples(128);
        while (!stop.stop_requested()) {
            fbe_audio_mix(samples.data(), 64);
            for (std::size_t i = 0; i < samples.size(); i += 2) {
                left = std::max(left, std::abs(samples[i]));
                right = std::max(right, std::abs(samples[i + 1]));
            }
        }
    });
    change();
    mixing.request_stop();
    mixing.join();
    return { left, right };
}

} // namespace

// A low-pass at 0 Hz puts out a full-scale click; taking one away mustn't let the mixing thread
// make it at 0 Hz between the cutoff going and the low-pass going.
TEST_CASE("a bus's low-pass taken away while another thread mixes never clicks", "[stability]") {
    REQUIRE(fbe_audio_start(2, rate, 1, nullptr, 0.0));
    const std::uint64_t bus = fbe_bus_add(0);
    REQUIRE(loop(tone(), bus) != 0);
    const auto [loudest, unused] = mixed_beside([&] {
        for (int i = 0; i < 100000; ++i) {
            fbe_bus_set_low_pass(bus, 1000.0F);
            fbe_bus_set_low_pass(bus, 2000.0F);
            fbe_bus_set_low_pass(bus, 0.0F);
        }
    });
    CHECK(loudest < 0.2F);
    fbe_audio_stop();
}

// Under a law of constant power, a mono 0.5 at pan 0 is 0.354 on each side, and at 0.5, 0.191 on
// the left and 0.462 on the right. Its pan and volume reach the mixing thread together, so a mix
// between never has one side louder than either pan makes it.
TEST_CASE("a pan changed while another thread mixes is never heard half changed", "[stability]") {
    REQUIRE(fbe_audio_start(2, rate, 2, nullptr, 0.0));
    REQUIRE(fbe_channel_set_pan_law(1));
    const std::uint64_t channel = loop(wav(std::vector<std::int16_t>(4800, 16384)), 0);
    REQUIRE(channel != 0);
    const auto [left, right] = mixed_beside([&] {
        for (int i = 0; i < 1000000; ++i) {
            fbe_channel_set_pan(channel, 0.5F);
            fbe_channel_set_pan(channel, 0.0F);
        }
    });
    CHECK(left <= 0.3536F);
    CHECK(right <= 0.4620F);
    fbe_channel_set_pan_law(0);
    fbe_audio_stop();
}
