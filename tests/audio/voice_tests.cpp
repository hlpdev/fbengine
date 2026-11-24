// What voice chat uses of fbengine_audio_native's C API: a fed sound fed while another thread
// mixes, as a device's does, which a build with ThreadSanitizer checks shares its ring buffer
// safely; and the fed sound's, the capture's, and the voice area's refusals.

#include "fbengine_audio.h"
#include "stability.hpp"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::int32_t rate = 48000;
constexpr std::int32_t fed_frames = 20000;

} // namespace

// Every frame the feed took plays once, in order, however the pushes and the mixes fall.
TEST_CASE("a fed sound fed while another thread mixes plays all it took, in order", "[stability]") {
    REQUIRE(fbe_audio_start(2, rate, 1, nullptr, 0.0));
    const std::uint64_t sound = fbe_sound_open_feed(rate, 1, 4800, 0);
    REQUIRE(sound != 0);
    const std::uint64_t channel = fbe_sound_play(sound, 1.0F, 0.0F, 1.0F, false, false, 0.0, 0, 0);
    REQUIRE(channel != 0);
    std::vector<float> heard;
    fbe::tests::stopping_thread_t mixing([&](const std::atomic<bool>& stop) {
        std::array<float, 64> samples{};
        while (!stop.load()) {
            fbe_audio_mix(samples.data(), static_cast<std::int32_t>(samples.size()));
            for (const float sample : samples) {
                if (sample != 0.0F) {
                    heard.push_back(sample);
                }
            }
        }
    });
    std::vector<float> chunk;
    std::int32_t pushed = 0;
    while (pushed < fed_frames) {
        chunk.clear();
        for (std::int32_t i = 0; i < 97 && pushed + i < fed_frames; ++i) {
            chunk.push_back(0.1F + (static_cast<float>(pushed + i) * 0.00002F));
        }
        const std::int32_t took =
            fbe_sound_feed(sound, chunk.data(), static_cast<std::int32_t>(chunk.size()));
        REQUIRE(took >= 0);
        pushed += took;
        if (std::cmp_less(took, chunk.size())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    for (int waited = 0; fbe_sound_queued(sound) > 0 && waited < 5000; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // What it read last is mixed once the mix gets through what it read before.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mixing.stop();
    CHECK(fbe_sound_queued(sound) == 0);
    REQUIRE(heard.size() == static_cast<std::size_t>(fed_frames));
    for (std::size_t i = 1; i < heard.size(); ++i) {
        REQUIRE(heard[i] > heard[i - 1]);
    }
    fbe_channel_stop(channel);
    fbe_sound_free(sound);
    fbe_audio_stop();
}

TEST_CASE("a fed sound refuses values out of range, and a second channel") {
    CHECK(fbe_sound_open_feed(7999, 1, 100, 0) == 0);
    CHECK(fbe_sound_open_feed(rate, 0, 100, 0) == 0);
    CHECK(fbe_sound_open_feed(rate, 9, 100, 0) == 0);
    CHECK(fbe_sound_open_feed(rate, 1, 0, 0) == 0);
    CHECK(fbe_sound_open_feed(rate, 1, (rate * 60) + 1, 0) == 0);
    CHECK(fbe_sound_open_feed(rate, 1, 100, 101) == 0);
    CHECK(fbe_sound_open_feed(rate, 1, 100, -1) == 0);
    const std::uint64_t sound = fbe_sound_open_feed(rate, 1, 100, 100);
    REQUIRE(sound != 0);
    CHECK(fbe_sound_fed(sound));
    CHECK(fbe_sound_feed(sound, nullptr, -1) == -1);
    CHECK(fbe_sound_feed(sound, nullptr, 1) == -1);
    CHECK(fbe_sound_feed(sound, nullptr, 0) == 0);
    CHECK(fbe_sound_feed(0, nullptr, 0) == -1);
    REQUIRE(fbe_audio_start(2, rate, 1, nullptr, 0.0));
    const std::uint64_t first = fbe_sound_play(sound, 1.0F, 0.0F, 1.0F, false, false, 0.0, 0, 0);
    CHECK(first != 0);
    CHECK(fbe_sound_play(sound, 1.0F, 0.0F, 1.0F, false, false, 0.0, 0, 0) == 0);
    CHECK_FALSE(fbe_channel_seek(first, 0.0));
    fbe_channel_stop(first);
    const std::uint64_t second = fbe_sound_play(sound, 1.0F, 0.0F, 1.0F, false, false, 0.0, 0, 0);
    CHECK(second != 0);
    fbe_audio_stop();
    // The audio's stop ends its channel, so another can play it after.
    REQUIRE(fbe_audio_start(2, rate, 1, nullptr, 0.0));
    CHECK(fbe_sound_play(sound, 1.0F, 0.0F, 1.0F, false, false, 0.0, 0, 0) != 0);
    fbe_audio_stop();
    fbe_sound_free(sound);
}

TEST_CASE("the capture refuses values out of range, and a second start") {
    CHECK_FALSE(fbe_capture_start(3, nullptr, 0, 0, 0.0));
    CHECK_FALSE(fbe_capture_start(2, nullptr, 7999, 0, 0.0));
    CHECK_FALSE(fbe_capture_start(2, nullptr, 0, 9, 0.0));
    CHECK_FALSE(fbe_capture_start(2, nullptr, 0, -1, 0.0));
    CHECK_FALSE(fbe_capture_start(2, nullptr, 0, 0, 0.001));
    CHECK_FALSE(fbe_capture_start(2, nullptr, 0, 0, 61.0));
    CHECK_FALSE(fbe_capture_start(2, nullptr, 0, 0, std::nan("")));
    CHECK_FALSE(fbe_capture_start(2, "a microphone", 0, 0, 0.0));
    CHECK(fbe_capture_read(nullptr, 0) == -1);
    CHECK_FALSE(fbe_capture_hear(nullptr, 0));
    CHECK(fbe_capture_device() == -1);
    REQUIRE(fbe_capture_start(2, nullptr, 0, 0, 0.0));
    CHECK_FALSE(fbe_capture_start(2, nullptr, 0, 0, 0.0));
    CHECK(fbe_capture_sample_rate() == 48000);
    CHECK(fbe_capture_channels() == 1);
    CHECK(fbe_capture_read(nullptr, -1) == -1);
    CHECK_FALSE(fbe_capture_hear(nullptr, -1));
    CHECK_FALSE(fbe_capture_simulate_unplug());
    fbe_capture_stop();
    fbe_capture_stop();
    CHECK_FALSE(fbe_capture_started());
}

// The null backend's input runs on its own thread, which the main thread reads beside.
TEST_CASE("the null backend's input is read while it hears", "[stability]") {
    REQUIRE(fbe_capture_start(1, nullptr, rate, 1, 0.1));
    std::vector<float> samples(480);
    std::int64_t read = 0;
    for (int waited = 0; read < rate / 10 && waited < 2000; ++waited) {
        const std::int32_t got = fbe_capture_read(samples.data(), 480);
        REQUIRE(got >= 0);
        read += got;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(read >= rate / 10);
    CHECK_FALSE(fbe_capture_lost());
    fbe_capture_stop();
}

TEST_CASE("the voice area refuses values out of range, and handles of another kind") {
    const std::int32_t before = fbe_voice_count();
    CHECK(fbe_voice_encoder_add(44100, 1, 24000) == 0);
    CHECK(fbe_voice_encoder_add(rate, 3, 24000) == 0);
    CHECK(fbe_voice_encoder_add(rate, 1, 5999) == 0);
    CHECK(fbe_voice_encoder_add(rate, 1, 510001) == 0);
    CHECK(fbe_voice_decoder_add(22050, 1) == 0);
    CHECK(fbe_voice_decoder_add(rate, 0) == 0);
    const std::uint64_t encoder = fbe_voice_encoder_add(rate, 1, 24000);
    const std::uint64_t decoder = fbe_voice_decoder_add(rate, 1);
    const std::uint64_t denoiser = fbe_voice_denoiser_add();
    REQUIRE(encoder != 0);
    REQUIRE(decoder != 0);
    REQUIRE(denoiser != 0);
    CHECK(fbe_voice_count() == before + 3);
    CHECK_FALSE(fbe_voice_set_loss(encoder, -1));
    CHECK_FALSE(fbe_voice_set_loss(encoder, 101));
    CHECK_FALSE(fbe_voice_set_loss(decoder, 10));
    std::vector<float> samples(FBE_VOICE_DENOISE_FRAMES);
    std::array<std::uint8_t, 4000> packet{};
    // 7.5 ms isn't a frame Opus takes.
    CHECK(fbe_voice_encode(encoder, samples.data(), 360, packet.data(), 4000) == -1);
    CHECK(fbe_voice_encode(encoder, samples.data(), 480, packet.data(), 0) == -1);
    CHECK(fbe_voice_encode(encoder, nullptr, 480, packet.data(), 4000) == -1);
    CHECK(fbe_voice_encode(decoder, samples.data(), 480, packet.data(), 4000) == -1);
    const std::int32_t size = fbe_voice_encode(encoder, samples.data(), 480, packet.data(), 4000);
    REQUIRE(size > 0);
    CHECK(fbe_voice_decode(encoder, packet.data(), size, false, samples.data(), 480) == -1);
    CHECK(fbe_voice_decode(decoder, packet.data(), size, false, samples.data(), 240) == -1);
    CHECK(fbe_voice_decode(decoder, packet.data(), size, false, samples.data(), 480) == 480);
    CHECK(fbe_voice_decode(decoder, packet.data(), -1, false, samples.data(), 480) == -1);
    CHECK(fbe_voice_denoise(encoder, samples.data()) == -1.0F);
    CHECK(fbe_voice_denoise(denoiser, nullptr) == -1.0F);
    const float voice = fbe_voice_denoise(denoiser, samples.data());
    CHECK((voice >= 0.0F && voice <= 1.0F));
    fbe_voice_free(encoder);
    fbe_voice_free(decoder);
    fbe_voice_free(denoiser);
    fbe_voice_free(denoiser);
    fbe_voice_free(0);
    CHECK(fbe_voice_count() == before);
    CHECK(fbe_voice_encode(encoder, samples.data(), 480, packet.data(), 4000) == -1);
}
