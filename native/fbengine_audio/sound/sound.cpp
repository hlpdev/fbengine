// Sounds: fbe_sound_load decodes a whole sound with miniaudio's decoders, into 32-bit floats at
// its own rate and channels, which the channels playing it resample to the mix's.

#include "sound/sound.hpp"

#include "audio/mixer.hpp"
#include "audio/pool.hpp"
#include "channel/voices.hpp"
#include "fbengine_audio.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <utility>

namespace {

/// The most channels a sound has, as the mix.
constexpr ma_uint32 most_channels = 8;
/// How many frames a decode reads at a time when the decoder can't say how long the sound is.
constexpr ma_uint64 chunk = 4096;

/// What a sound handle holds: its share of the decoded sound.
struct sound_t {
    std::shared_ptr<const fbe::audio::sound_data_t> data;
};

fbe::audio::pool_t<sound_t> sounds;

/// A decoder over bytes in memory, let go however decoding ends.
struct decoder_t {
    ma_decoder decoder;
    bool open = false;

    decoder_t() = default;
    decoder_t(const decoder_t&) = delete;
    decoder_t& operator=(const decoder_t&) = delete;
    decoder_t(decoder_t&&) = delete;
    decoder_t& operator=(decoder_t&&) = delete;

    ~decoder_t() {
        if (open) {
            ma_decoder_uninit(&decoder);
        }
    }
};

/// Every frame `decoder` has, or null, with fbe_audio_error's message.
std::unique_ptr<fbe::audio::sound_data_t> decode(ma_decoder& decoder) {
    auto data = std::make_unique<fbe::audio::sound_data_t>();
    ma_format format = ma_format_unknown;
    ma_result result = ma_decoder_get_data_format(&decoder, &format, &data->channels,
                                                  &data->sample_rate, nullptr, 0);
    if (result != MA_SUCCESS) {
        fbe::audio::fail_ma("couldn't read the sound", result);
        return nullptr;
    }
    if (data->channels == 0 || data->channels > most_channels) {
        fbe::audio::fail(
            std::format("a sound has 1 to {} channels, not {}", most_channels, data->channels));
        return nullptr;
    }

    ma_uint64 length = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &length) != MA_SUCCESS) {
        length = 0;
    }
    const std::size_t channels = data->channels;
    data->samples.resize(static_cast<std::size_t>(length != 0 ? length : chunk) * channels);
    ma_uint64 frames = 0;
    for (;;) {
        if (frames * channels == data->samples.size()) {
            data->samples.resize(data->samples.size() + (chunk * channels));
        }
        const ma_uint64 room = (data->samples.size() / channels) - frames;
        ma_uint64 read = 0;
        result =
            ma_decoder_read_pcm_frames(&decoder, &data->samples[frames * channels], room, &read);
        frames += read;
        if (result == MA_AT_END || read == 0) {
            break;
        }
        if (result != MA_SUCCESS) {
            fbe::audio::fail_ma("couldn't decode the sound", result);
            return nullptr;
        }
    }
    if (frames == 0) {
        fbe::audio::fail("the sound has no frames");
        return nullptr;
    }
    data->samples.resize(frames * channels);
    data->samples.shrink_to_fit();
    data->frames = frames;
    return data;
}

} // namespace

namespace fbe::audio {

std::shared_ptr<const sound_data_t> find_sound(std::uint64_t sound) {
    const sound_t* found = sounds.find(sound);
    return found != nullptr ? found->data : nullptr;
}

} // namespace fbe::audio

extern "C" {

FBE_AUDIO_API uint64_t fbe_sound_load(const uint8_t* data, int64_t size) {
    if (data == nullptr || size <= 0) {
        fbe::audio::fail("there are no bytes to read a sound from");
        return 0;
    }
    decoder_t decoder;
    const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    const ma_result result =
        ma_decoder_init_memory(data, static_cast<std::size_t>(size), &config, &decoder.decoder);
    if (result != MA_SUCCESS) {
        if (result == MA_NO_BACKEND) {
            fbe::audio::fail("couldn't read the sound: it isn't WAV, FLAC, or Ogg Vorbis");
        } else {
            fbe::audio::fail_ma("couldn't read the sound", result);
        }
        return 0;
    }
    decoder.open = true;

    std::unique_ptr<fbe::audio::sound_data_t> decoded = decode(decoder.decoder);
    if (decoded == nullptr) {
        return 0;
    }
    return sounds.add(std::make_unique<sound_t>(std::move(decoded)));
}

FBE_AUDIO_API void fbe_sound_free(uint64_t sound) {
    sounds.remove(sound);
}

FBE_AUDIO_API int32_t fbe_sound_sample_rate(uint64_t sound) {
    const sound_t* found = sounds.find(sound);
    return found != nullptr ? static_cast<int32_t>(found->data->sample_rate) : 0;
}

FBE_AUDIO_API int32_t fbe_sound_channels(uint64_t sound) {
    const sound_t* found = sounds.find(sound);
    return found != nullptr ? static_cast<int32_t>(found->data->channels) : 0;
}

FBE_AUDIO_API int64_t fbe_sound_frames(uint64_t sound) {
    const sound_t* found = sounds.find(sound);
    return found != nullptr ? static_cast<int64_t>(found->data->frames) : 0;
}

FBE_AUDIO_API uint64_t fbe_sound_play(uint64_t sound, float volume, float pan, float pitch,
                                      bool loop, bool paused) {
    if (!fbe::audio::check_volume(volume) || !fbe::audio::check_pan(pan) ||
        !fbe::audio::check_pitch(pitch)) {
        return 0;
    }
    std::shared_ptr<const fbe::audio::sound_data_t> data = fbe::audio::find_sound(sound);
    if (data == nullptr) {
        fbe::audio::fail("the sound was freed");
        return 0;
    }
    const fbe::audio::play_t how = {
        .volume = volume,
        .pan = pan,
        .pitch = pitch,
        .loop = loop,
        .paused = paused,
    };
    return fbe::audio::play(std::move(data), how);
}

} // extern "C"
