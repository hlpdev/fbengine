// fbengine_audio's C API with each of a call's allocations failing in turn. Its sources are built
// into this test, whose operator new fails the allocation it's told to, on the main thread only:
// nothing a call throws gets out of it, a call that fails says "out of memory", and what it was
// called on works after.

#include "fbengine_audio.h"
#include "own_new.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <numbers>
#include <string>
#include <vector>

// tests/own_new.cpp's operator new and delete, every form, count and fail this thread's
// allocations.
using fbe::tests::allocated;
using fbe::tests::failed;
using fbe::tests::failing;

namespace {

constexpr std::int32_t silent = 1;
constexpr std::int32_t manual = 2;
constexpr std::int32_t rate = 48000;

/// A mono 16-bit WAV of a tenth of a second of a 440 Hz sine, at 48000 Hz.
std::vector<std::uint8_t> tone() {
    std::vector<std::uint8_t> bytes;
    const auto add = [&](std::uint32_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            bytes.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
        }
    };
    constexpr std::uint32_t frames = rate / 10;
    bytes.insert(bytes.end(), { 'R', 'I', 'F', 'F' });
    add(36 + (frames * 2), 4);
    bytes.insert(bytes.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
    add(16, 4);
    add(1, 2);
    add(1, 2);
    add(rate, 4);
    add(rate * 2, 4);
    add(2, 2);
    add(16, 2);
    bytes.insert(bytes.end(), { 'd', 'a', 't', 'a' });
    add(frames * 2, 4);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const double sample = 3276.0 * std::sin(2.0 * std::numbers::pi * 440.0 * i / rate);
        add(static_cast<std::uint32_t>(static_cast<std::uint16_t>(std::lround(sample))), 2);
    }
    return bytes;
}

/// Runs `call` with its first allocation failing, then its second, and so on, until one in which
/// none did; `before` and `after` run around each, with every allocation working. Gives how many
/// allocations `call` makes, and counts the times it failed without saying it ran out of memory.
struct sweep_t {
    long allocations = 0;
    long unsaid = 0;
};

template <typename Before, typename Call, typename After>
sweep_t sweep(Before before, Call call, After after) {
    sweep_t swept;
    for (long n = 0;; ++n) {
        before();
        allocated = 0;
        failed = false;
        failing = n;
        const bool worked = call();
        failing = -1;
        const bool reached = failed;
        if (reached && !worked && std::string(fbe_audio_error()) != "out of memory") {
            ++swept.unsaid;
        }
        after();
        if (!reached) {
            swept.allocations = n;
            return swept;
        }
    }
}

void nothing() {}

/// The next `frames` frames of a stereo mix, as a device's thread would mix them.
void mix(std::int32_t frames) {
    std::vector<float> samples(static_cast<std::size_t>(frames) * 2);
    fbe_audio_mix(samples.data(), frames);
}

} // namespace

TEST_CASE("StartingAndListingRunOutOfMemoryWhole") {
    for (const std::int32_t device : { manual, silent }) {
        const sweep_t started = sweep(
            nothing, [&] { return fbe_audio_start(device, rate, 2, nullptr, 0.0); },
            [] { fbe_audio_stop(); });
        CHECK(started.allocations > 0);
        CHECK(started.unsaid == 0);
        REQUIRE(fbe_audio_start(device, rate, 2, nullptr, 0.0));
        CHECK(fbe_audio_started());
        fbe_audio_stop();
    }
    const sweep_t listed =
        sweep(nothing, [] { return fbe_audio_list_outputs(silent) >= 0; }, nothing);
    CHECK(listed.unsaid == 0);
    CHECK(fbe_audio_list_outputs(silent) == 1);
}

TEST_CASE("SoundsChannelsBusesAndEmittersRunOutOfMemoryWhole") {
    REQUIRE(fbe_audio_start(manual, rate, 2, nullptr, 0.0));
    const std::vector<std::uint8_t> bytes = tone();
    const auto size = static_cast<std::int64_t>(bytes.size());
    std::uint64_t sound = 0;
    CHECK(sweep(
              nothing, [&] { return (sound = fbe_sound_load(bytes.data(), size)) != 0; },
              [&] { fbe_sound_free(sound); })
              .unsaid == 0);
    CHECK(sweep(
              nothing, [&] { return (sound = fbe_sound_stream(bytes.data(), size)) != 0; },
              [&] { fbe_sound_free(sound); })
              .unsaid == 0);
    sound = fbe_sound_load(bytes.data(), size);
    REQUIRE(sound != 0);

    std::uint64_t bus = 0;
    CHECK(sweep(
              nothing, [&] { return (bus = fbe_bus_add(0)) != 0; }, [&] { fbe_bus_free(bus); })
              .unsaid == 0);
    bus = fbe_bus_add(0);
    REQUIRE(bus != 0);
    CHECK(sweep(
              nothing, [&] { return fbe_bus_set_reverb(bus, 0.5F, 0.5F, 1.0F, 0.3F, 1.0F); },
              [&] { fbe_bus_clear_reverb(bus); })
              .unsaid == 0);
    std::uint64_t emitter = 0;
    CHECK(sweep(
              nothing, [&] { return (emitter = fbe_emitter_add()) != 0; },
              [&] { fbe_emitter_free(emitter); })
              .unsaid == 0);
    emitter = fbe_emitter_add();
    REQUIRE(emitter != 0);

    // Plays, into the mix, a bus, and from an emitter: one that fails isn't playing.
    for (const bool from : { false, true }) {
        std::uint64_t channel = 0;
        const sweep_t played = sweep(
            nothing,
            [&] {
                channel = fbe_sound_play(sound, 1.0F, 0.0F, 1.0F, true, false, 0.0, from ? bus : 0,
                                         from ? emitter : 0);
                return channel != 0;
            },
            [&] {
                if (channel == 0) {
                    CHECK(fbe_channel_count() == 0);
                }
                fbe_channel_stop(channel);
                mix(512);
            });
        CHECK(played.unsaid == 0);
    }
    // A stop that runs out of memory stops it all the same.
    std::uint64_t channel = 0;
    sweep(
        [&] {
            channel = fbe_sound_play(sound, 1.0F, 0.0F, 1.0F, true, false, 0.0, 0, emitter);
            mix(64);
        },
        [&] {
            fbe_channel_stop(channel);
            return true;
        },
        [&] {
            CHECK(!fbe_channel_playing(channel));
            mix(512);
            CHECK(fbe_channel_count() == 0);
        });
    // A freed emitter's channels play on where it was.
    std::uint64_t freed = 0;
    sweep(
        [&] {
            freed = fbe_emitter_add();
            channel = fbe_sound_play(sound, 1.0F, 0.0F, 1.0F, true, false, 0.0, 0, freed);
        },
        [&] {
            fbe_emitter_free(freed);
            return true;
        },
        [&] {
            CHECK(fbe_channel_playing(channel));
            fbe_listener_set_velocity(1.0F, 0.0F, 0.0F);
            fbe_listener_set_velocity(0.0F, 0.0F, 0.0F);
            fbe_channel_stop(channel);
            mix(512);
        });

    // What a call says of a value out of range is a message, which can run out of memory too.
    channel = fbe_sound_play(sound, 1.0F, 0.0F, 1.0F, true, false, 0.0, 0, 0);
    const auto unsaid = [](auto call) { return sweep(nothing, call, nothing).unsaid; };
    CHECK(unsaid([] { return fbe_audio_set_volume(-1.0F); }) == 0);
    CHECK(unsaid([] { return fbe_channel_set_pan_law(9); }) == 0);
    CHECK(unsaid([&] { return fbe_channel_set_volume(channel, -1.0F); }) == 0);
    CHECK(unsaid([&] { return fbe_channel_set_pitch(channel, 100.0F); }) == 0);
    CHECK(unsaid([&] { return fbe_channel_seek(channel, -1.0); }) == 0);
    CHECK(unsaid([&] { return fbe_channel_fade(channel, 0.5F, -1.0); }) == 0);
    CHECK(unsaid([&] { return fbe_bus_set_low_pass(bus, -5.0F); }) == 0);
    CHECK(unsaid([&] { return fbe_bus_set_volume(bus, -1.0F); }) == 0);
    CHECK(unsaid([&] { return fbe_emitter_set_range(emitter, 0.0F, 0.0F); }) == 0);
    CHECK(unsaid([&] { return fbe_emitter_set_falloff(emitter, 9); }) == 0);
    CHECK(unsaid([&] { return fbe_emitter_set_doppler(emitter, -1.0F); }) == 0);
    CHECK(unsaid([] { return fbe_listener_set_speed_of_sound(-1.0F); }) == 0);
    CHECK(unsaid([] { return fbe_audio_simulate(99); }) == 0);
    std::vector<float> samples(2);
    CHECK(unsaid([&] { return fbe_audio_mix(samples.data(), -1); }) == 0);
    fbe_channel_stop(channel);
    fbe_audio_stop();
    REQUIRE(fbe_audio_start(manual, rate, 2, nullptr, 0.0));
    CHECK(fbe_sound_play(sound, 1.0F, 0.0F, 1.0F, false, false, 0.0, bus, emitter) != 0);
    fbe_audio_stop();
}

TEST_CASE("ALostDeviceMadeAgainRunsOutOfMemoryWhole") {
    REQUIRE(fbe_audio_start(silent, rate, 2, nullptr, 0.0));
    sweep([] { fbe_audio_simulate(FBE_AUDIO_SIMULATE_UNPLUG); },
          [] {
              fbe_audio_recover();
              return true;
          },
          nothing);
    fbe_audio_recover();
    fbe_audio_recover();
    CHECK(!fbe_audio_lost());
    fbe_audio_stop();
}
