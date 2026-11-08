// The game's calls against a mix on another thread, at random from a fixed seed: sounds loaded and
// streamed, played, stopped, paused, sought, faded, and freed while they play; buses added in
// buses, changed, given reverb and a low-pass, and freed under their channels; channels moved
// between them; emitters moved and freed under theirs; and the listener moved. Every sample the mix
// makes meanwhile is finite, and when it's all stopped nothing's left playing. A build with
// ThreadSanitizer checks that no two threads touch a thing unguarded.

#include "fbengine_audio.h"
#include "stability.hpp"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <vector>

using fbe::tests::random_t;

namespace {

/// One of `handles`, at random, or 0 for none.
std::uint64_t any(const std::vector<std::uint64_t>& handles, random_t& random) {
    return handles.empty() ? 0 : handles[random.below(handles.size())];
}

/// Takes one of `handles` out, at random, giving it, or 0 for none.
std::uint64_t taken(std::vector<std::uint64_t>& handles, random_t& random) {
    if (handles.empty()) {
        return 0;
    }
    const std::size_t at = random.below(handles.size());
    const std::uint64_t handle = handles[at];
    handles.erase(handles.begin() + static_cast<std::ptrdiff_t>(at));
    return handle;
}

/// What the game does, `calls` times, while the mix runs on another thread: the files, each loaded
/// and streamed again and again, and what plays them. `device_events` also unplugs the device,
/// plugs it in, changes its default, and stalls it, and recovers as a Flip does.
void hammered(std::uint64_t seed, int calls, bool device_events) {
    const std::filesystem::path corpus(FBE_AUDIO_TEST_CORPUS);
    const std::vector<std::vector<std::uint8_t>> files{
        fbe::tests::read(corpus / "wav_s16_stereo_44100.wav"),
        fbe::tests::read(corpus / "wav_ima_adpcm_stereo_22050.wav"),
        fbe::tests::read(corpus / "flac_s16_stereo_44100_level0.flac"),
        fbe::tests::read(corpus / "ogg_vorbis_stereo_44100_q6.ogg"),
        fbe::tests::read(corpus / "ogg_vorbis_51_48000.ogg"),
    };
    random_t random(seed);
    std::vector<std::uint64_t> sounds;
    std::vector<std::uint64_t> channels;
    std::vector<std::uint64_t> buses;
    std::vector<std::uint64_t> emitters;
    for (int call = 0; call < calls; ++call) {
        switch (random.below(device_events ? 15 : 14)) {
        case 0: {
            const std::vector<std::uint8_t>& file = files[random.below(files.size())];
            const auto size = static_cast<std::int64_t>(file.size());
            const std::uint64_t sound = random.below(2) == 0 ? fbe_sound_load(file.data(), size)
                                                             : fbe_sound_stream(file.data(), size);
            REQUIRE(sound != 0);
            sounds.push_back(sound);
            break;
        }
        case 1:
            fbe_sound_free(taken(sounds, random));
            break;
        case 2:
        case 3: {
            const std::uint64_t channel =
                fbe_sound_play(any(sounds, random), random.unit(), (random.unit() * 2.0F) - 1.0F,
                               0.25F + (random.unit() * 3.0F), random.below(3) == 0,
                               random.below(5) == 0, random.unit() * 0.05, any(buses, random),
                               random.below(3) == 0 ? any(emitters, random) : 0);
            if (channel != 0) {
                channels.push_back(channel);
            }
            break;
        }
        case 4:
            fbe_channel_stop(taken(channels, random));
            break;
        case 5: {
            const std::uint64_t channel = any(channels, random);
            random.below(2) == 0 ? fbe_channel_pause(channel) : fbe_channel_resume(channel);
            fbe_channel_seek(channel, random.unit() * 0.1);
            break;
        }
        case 6: {
            const std::uint64_t channel = any(channels, random);
            fbe_channel_set_volume(channel, random.unit() * 2.0F);
            fbe_channel_set_pan(channel, (random.unit() * 2.0F) - 1.0F);
            fbe_channel_set_pitch(channel, 0.1F + (random.unit() * 4.0F));
            fbe_channel_set_loop(channel, random.below(2) == 0);
            break;
        }
        case 7: {
            const std::uint64_t channel = any(channels, random);
            random.below(2) == 0 ? fbe_channel_fade(channel, random.unit(), random.unit() * 0.02)
                                 : fbe_channel_fade_out(channel, random.unit() * 0.02);
            fbe_channel_set_bus(channel, any(buses, random));
            break;
        }
        case 8: {
            const std::uint64_t bus = fbe_bus_add(random.below(2) == 0 ? any(buses, random) : 0);
            if (bus != 0) {
                buses.push_back(bus);
            }
            break;
        }
        case 9:
            fbe_bus_free(taken(buses, random));
            break;
        case 10: {
            const std::uint64_t bus = any(buses, random);
            fbe_bus_set_volume(bus, random.unit() * 1.5F);
            fbe_bus_set_low_pass(bus,
                                 random.below(3) == 0 ? 0.0F : 100.0F + (random.unit() * 20000.0F));
            if (random.below(3) == 0) {
                fbe_bus_set_reverb(bus, random.unit(), random.unit(), random.unit(), random.unit(),
                                   random.unit());
            } else if (random.below(3) == 0) {
                fbe_bus_clear_reverb(bus);
            }
            break;
        }
        case 11: {
            const std::uint64_t emitter = fbe_emitter_add();
            REQUIRE(emitter != 0);
            emitters.push_back(emitter);
            break;
        }
        case 12: {
            const std::uint64_t emitter = any(emitters, random);
            fbe_emitter_set_position(emitter, (random.unit() - 0.5F) * 40.0F, 0.0F,
                                     (random.unit() - 0.5F) * 40.0F);
            fbe_emitter_set_velocity(emitter, (random.unit() - 0.5F) * 30.0F, 0.0F, 0.0F);
            if (random.below(8) == 0) {
                fbe_emitter_free(taken(emitters, random));
            }
            break;
        }
        case 13:
            fbe_listener_set_position((random.unit() - 0.5F) * 10.0F, 0.0F, 0.0F);
            fbe_listener_set_velocity((random.unit() - 0.5F) * 5.0F, 0.0F, 0.0F);
            break;
        default:
            fbe_audio_simulate(static_cast<std::int32_t>(random.below(5)));
            fbe_audio_recover();
            break;
        }
        // What's ended goes from the game's list, as a game's own would forget it.
        if (call % 64 == 0) {
            std::erase_if(channels,
                          [](std::uint64_t channel) { return !fbe_channel_playing(channel); });
        }
    }
    for (const std::uint64_t channel : channels) {
        fbe_channel_stop(channel);
    }
    for (const std::uint64_t sound : sounds) {
        fbe_sound_free(sound);
    }
    for (const std::uint64_t bus : buses) {
        fbe_bus_free(bus);
    }
    for (const std::uint64_t emitter : emitters) {
        fbe_emitter_free(emitter);
    }
}

} // namespace

TEST_CASE("the game's calls while another thread mixes leave every sample finite", "[stability]") {
    REQUIRE(fbe_audio_start(2, 48000, 2, nullptr, 0.0));
    std::atomic<bool> finite = true;
    std::atomic<std::int64_t> mixes = 0;
    fbe::tests::stopping_thread_t mixing([&](const std::atomic<bool>& stop) {
        std::vector<float> samples(2048);
        random_t sizes(7);
        while (!stop.load()) {
            const auto frames = static_cast<std::int32_t>(1 + sizes.below(1024));
            fbe_audio_mix(samples.data(), frames);
            for (std::size_t i = 0; i < static_cast<std::size_t>(frames) * 2; ++i) {
                if (!std::isfinite(samples[i])) {
                    finite = false;
                }
            }
            ++mixes;
        }
    });
    hammered(0x9E3779B97F4A7C15ULL, 20000, false);
    mixing.stop();
    CHECK(finite.load());
    CHECK(mixes.load() > 0);
    // What's stopped and freed is gone once the mix has run again.
    std::vector<float> samples(2048);
    REQUIRE(fbe_audio_mix(samples.data(), 1024));
    CHECK(fbe_channel_count() == 0);
    fbe_audio_stop();
}

// On the null backend, whose own thread mixes at a device's pace, with the device unplugged,
// plugged in, changed, and stalled among the game's calls; and once it's plugged back in, flips
// make it again, and it mixes.
TEST_CASE("the game's calls while a device's thread mixes, and the device is lost and changed",
          "[stability]") {
    REQUIRE(fbe_audio_start(1, 48000, 2, nullptr, 0.002));
    const std::int64_t started = fbe_audio_frames();
    hammered(0xD1B54A32D192ED03ULL, 20000, true);
    CHECK(fbe_audio_frames() > started);
    REQUIRE(fbe_audio_simulate(FBE_AUDIO_SIMULATE_PLUG_IN));
    // A flip every 10 ms, until the device is back and has mixed since: a stall the game's calls
    // left is lost after a quarter of a second.
    const std::int64_t plugged = fbe_audio_frames();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < until &&
           (fbe_audio_lost() || fbe_audio_frames() <= plugged)) {
        fbe_audio_recover();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(fbe_audio_started());
    CHECK(fbe_audio_frames() > plugged);
    fbe_audio_recover();
    CHECK(!fbe_audio_lost());
    fbe_audio_stop();
    CHECK(fbe_channel_count() == 0);
}
