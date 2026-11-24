// Voice: Opus's encoders and decoders, and RNNoise's denoisers, each in a pool of its own.

#include "audio/mixer.hpp"
#include "audio/pool.hpp"
#include "fbengine_audio.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <memory>
#include <new>
#include <opus.h>
#include <rnnoise.h>
#include <span>
#include <string_view>

namespace {

/// The rates Opus works at.
constexpr std::array<std::int32_t, 5> opus_rates = { 8000, 12000, 16000, 24000, 48000 };
constexpr std::int32_t fewest_bits = 6000;
constexpr std::int32_t most_bits = 510000;
constexpr std::int32_t most_loss = 100;
/// RNNoise's samples are 16-bit PCM's, as floats.
constexpr float pcm_scale = 32768.0F;

struct encoder_t {
    OpusEncoder* opus = nullptr;
    std::int32_t channels = 0;

    encoder_t() = default;
    encoder_t(const encoder_t&) = delete;
    encoder_t& operator=(const encoder_t&) = delete;
    encoder_t(encoder_t&&) = delete;
    encoder_t& operator=(encoder_t&&) = delete;

    ~encoder_t() {
        opus_encoder_destroy(opus);
    }
};

struct decoder_t {
    OpusDecoder* opus = nullptr;
    std::int32_t channels = 0;

    decoder_t() = default;
    decoder_t(const decoder_t&) = delete;
    decoder_t& operator=(const decoder_t&) = delete;
    decoder_t(decoder_t&&) = delete;
    decoder_t& operator=(decoder_t&&) = delete;

    ~decoder_t() {
        opus_decoder_destroy(opus);
    }
};

struct denoiser_t {
    DenoiseState* rnnoise = nullptr;

    denoiser_t() = default;
    denoiser_t(const denoiser_t&) = delete;
    denoiser_t& operator=(const denoiser_t&) = delete;
    denoiser_t(denoiser_t&&) = delete;
    denoiser_t& operator=(denoiser_t&&) = delete;

    ~denoiser_t() {
        if (rnnoise != nullptr) {
            rnnoise_destroy(rnnoise);
        }
    }
};

/// The pools, made once and never let go, so what an FB program lets go as it ends finds them.
fbe::audio::pool_t<encoder_t>& encoders() {
    static auto* made = new fbe::audio::pool_t<encoder_t>(fbe::audio::kind_t::ENCODER);
    return *made;
}

fbe::audio::pool_t<decoder_t>& decoders() {
    static auto* made = new fbe::audio::pool_t<decoder_t>(fbe::audio::kind_t::DECODER);
    return *made;
}

fbe::audio::pool_t<denoiser_t>& denoisers() {
    static auto* made = new fbe::audio::pool_t<denoiser_t>(fbe::audio::kind_t::DENOISER);
    return *made;
}

/// Whether Opus takes `sample_rate` and `channels`, or false, with fbe_audio_error's message.
bool check(std::int32_t sample_rate, std::int32_t channels) {
    if (std::ranges::find(opus_rates, sample_rate) == opus_rates.end()) {
        return fbe::audio::fail(
            std::format("a voice's sample rate is 8000, 12000, 16000, 24000, or 48000 Hz, not {}",
                        sample_rate));
    }
    if (channels != 1 && channels != 2) {
        return fbe::audio::fail(std::format("a voice has 1 or 2 channels, not {}", channels));
    }
    return true;
}

/// Opus's error `code`, for fbe_audio_error, with what was being done; false.
bool fail_opus(std::string_view doing, int code) {
    return fbe::audio::fail(std::format("{}: {}", doing, opus_strerror(code)));
}

} // namespace

extern "C" {

FBE_AUDIO_API uint64_t fbe_voice_encoder_add(int32_t sample_rate, int32_t channels,
                                             int32_t bitrate) try {
    if (!check(sample_rate, channels)) {
        return 0;
    }
    if (bitrate < fewest_bits || bitrate > most_bits) {
        fbe::audio::fail(std::format("a voice's bitrate is {} to {} bits a second, not {}",
                                     fewest_bits, most_bits, bitrate));
        return 0;
    }
    auto made = std::make_unique<encoder_t>();
    int error = OPUS_OK;
    made->opus = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_VOIP, &error);
    if (made->opus == nullptr) {
        fail_opus("couldn't make a voice encoder", error);
        return 0;
    }
    made->channels = channels;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): Opus's settings are varargs.
    opus_encoder_ctl(made->opus, OPUS_SET_BITRATE(bitrate));
    return encoders().add(std::move(made));
} catch (const std::bad_alloc&) {
    fbe::audio::out_of_memory();
    return 0;
}

FBE_AUDIO_API bool fbe_voice_set_loss(uint64_t encoder, int32_t percent) {
    const encoder_t* found = encoders().find(encoder);
    if (found == nullptr) {
        return fbe::audio::fail("the voice encoder was freed");
    }
    if (percent < 0 || percent > most_loss) {
        return fbe::audio::fail(
            std::format("a voice's loss is 0 to 100 percent of its packets, not {}", percent));
    }
    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg): Opus's settings are varargs.
    opus_encoder_ctl(found->opus, OPUS_SET_PACKET_LOSS_PERC(percent));
    opus_encoder_ctl(found->opus, OPUS_SET_INBAND_FEC(percent > 0 ? 1 : 0));
    // NOLINTEND(cppcoreguidelines-pro-type-vararg)
    return true;
}

FBE_AUDIO_API int32_t fbe_voice_encode(uint64_t encoder, const float* samples, int32_t frames,
                                       uint8_t* packet, int32_t room) {
    const encoder_t* found = encoders().find(encoder);
    if (found == nullptr) {
        fbe::audio::fail("the voice encoder was freed");
        return -1;
    }
    if (samples == nullptr || packet == nullptr || frames <= 0 || room <= 0) {
        fbe::audio::fail("there's nothing to encode, or no room for it");
        return -1;
    }
    const opus_int32 made = opus_encode_float(found->opus, samples, frames, packet, room);
    if (made < 0) {
        fail_opus("couldn't encode the voice", made);
        return -1;
    }
    return made;
}

FBE_AUDIO_API uint64_t fbe_voice_decoder_add(int32_t sample_rate, int32_t channels) try {
    if (!check(sample_rate, channels)) {
        return 0;
    }
    auto made = std::make_unique<decoder_t>();
    int error = OPUS_OK;
    made->opus = opus_decoder_create(sample_rate, channels, &error);
    if (made->opus == nullptr) {
        fail_opus("couldn't make a voice decoder", error);
        return 0;
    }
    made->channels = channels;
    return decoders().add(std::move(made));
} catch (const std::bad_alloc&) {
    fbe::audio::out_of_memory();
    return 0;
}

FBE_AUDIO_API int32_t fbe_voice_decode(uint64_t decoder, const uint8_t* packet, int32_t size,
                                       bool fec, float* samples, int32_t frames) {
    const decoder_t* found = decoders().find(decoder);
    if (found == nullptr) {
        fbe::audio::fail("the voice decoder was freed");
        return -1;
    }
    if (samples == nullptr || frames <= 0 || size < 0) {
        fbe::audio::fail("there's no room to decode the voice into");
        return -1;
    }
    const bool lost = packet == nullptr || size == 0;
    const int made = opus_decode_float(found->opus, lost ? nullptr : packet, lost ? 0 : size,
                                       samples, frames, fec && !lost ? 1 : 0);
    if (made < 0) {
        fail_opus("couldn't decode the voice", made);
        return -1;
    }
    return made;
}

FBE_AUDIO_API uint64_t fbe_voice_denoiser_add(void) try {
    auto made = std::make_unique<denoiser_t>();
    made->rnnoise = rnnoise_create(nullptr);
    if (made->rnnoise == nullptr) {
        fbe::audio::out_of_memory();
        return 0;
    }
    return denoisers().add(std::move(made));
} catch (const std::bad_alloc&) {
    fbe::audio::out_of_memory();
    return 0;
}

FBE_AUDIO_API float fbe_voice_denoise(uint64_t denoiser, float* samples) {
    const denoiser_t* found = denoisers().find(denoiser);
    if (found == nullptr || samples == nullptr) {
        fbe::audio::fail(found == nullptr ? "the denoiser was freed"
                                          : "there's nothing to denoise");
        return -1.0F;
    }
    const std::span<float, FBE_VOICE_DENOISE_FRAMES> frame(samples, FBE_VOICE_DENOISE_FRAMES);
    for (float& sample : frame) {
        sample *= pcm_scale;
    }
    const float voice = rnnoise_process_frame(found->rnnoise, frame.data(), frame.data());
    for (float& sample : frame) {
        sample = std::clamp(sample / pcm_scale, -1.0F, 1.0F);
    }
    return voice;
}

FBE_AUDIO_API void fbe_voice_free(uint64_t handle) {
    encoders().remove(handle);
    decoders().remove(handle);
    denoisers().remove(handle);
}

FBE_AUDIO_API int32_t fbe_voice_count(void) {
    return static_cast<int32_t>(encoders().count() + decoders().count() + denoisers().count());
}

} // extern "C"
