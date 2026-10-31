// Sounds: fbe_sound_load decodes a whole sound with miniaudio's decoders, into 32-bit floats at
// its own rate and channels, which the channels playing it resample to the mix's; fbe_sound_stream
// keeps its file, which each channel playing it decodes as it plays.

#include "sound/sound.hpp"

#include "audio/mixer.hpp"
#include "audio/pool.hpp"
#include "channel/voices.hpp"
#include "emitter/emitters.hpp"
#include "fbengine_audio.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <utility>
#include <vector>

namespace {

/// The most channels a sound has, as the mix.
constexpr ma_uint32 most_channels = 8;
/// How many frames a decode reads at a time when the decoder can't say how long the sound is.
constexpr ma_uint64 chunk = 4096;

/// What a sound handle holds: its share of the sound.
struct sound_t {
    std::shared_ptr<const fbe::audio::sound_data_t> data;
};

/// The sounds, made once and never let go, so a Sound an FB program lets go as it ends finds them.
fbe::audio::pool_t<sound_t>& sounds() {
    static auto* made = new fbe::audio::pool_t<sound_t>(fbe::audio::kind_t::SOUND);
    return *made;
}

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

/// Opens `decoder` over the `size` bytes at `data`; false, with fbe_audio_error's message, if they
/// aren't a sound.
bool open(decoder_t& decoder, const std::uint8_t* data, std::int64_t size) {
    if (data == nullptr || size <= 0) {
        return fbe::audio::fail("there are no bytes to read a sound from");
    }
    const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    const ma_result result =
        ma_decoder_init_memory(data, static_cast<std::size_t>(size), &config, &decoder.decoder);
    if (result == MA_NO_BACKEND) {
        return fbe::audio::fail("couldn't read the sound: it isn't WAV, FLAC, or Ogg Vorbis");
    }
    if (result != MA_SUCCESS) {
        return fbe::audio::fail_ma("couldn't read the sound", result);
    }
    decoder.open = true;
    return true;
}

/// A sound's rate and channels, from its decoder, or null, with fbe_audio_error's message, for
/// one with too many channels.
std::unique_ptr<fbe::audio::sound_data_t> describe(ma_decoder& decoder) {
    auto data = std::make_unique<fbe::audio::sound_data_t>();
    ma_format format = ma_format_unknown;
    const ma_result result = ma_decoder_get_data_format(&decoder, &format, &data->channels,
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
    return data;
}

/// Every frame `decoder` has, or null, with fbe_audio_error's message.
std::unique_ptr<fbe::audio::sound_data_t> decode(ma_decoder& decoder) {
    std::unique_ptr<fbe::audio::sound_data_t> data = describe(decoder);
    if (data == nullptr) {
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
        const ma_result result =
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

/// How many frames `decoder` has: what it says, or for a file that doesn't, what it reads.
ma_uint64 count_frames(ma_decoder& decoder, std::uint32_t channels) {
    ma_uint64 frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) == MA_SUCCESS && frames != 0) {
        return frames;
    }
    frames = 0;
    std::vector<float> scratch(chunk * channels);
    ma_uint64 read = 0;
    do {
        read = 0;
        ma_decoder_read_pcm_frames(&decoder, scratch.data(), chunk, &read);
        frames += read;
    } while (read != 0);
    return frames;
}

} // namespace

namespace fbe::audio {

std::shared_ptr<const sound_data_t> find_sound(std::uint64_t sound) {
    const sound_t* found = sounds().find(sound);
    return found != nullptr ? found->data : nullptr;
}

} // namespace fbe::audio

extern "C" {

FBE_AUDIO_API uint64_t fbe_sound_load(const uint8_t* data, int64_t size) {
    decoder_t decoder;
    if (!open(decoder, data, size)) {
        return 0;
    }
    std::unique_ptr<fbe::audio::sound_data_t> decoded = decode(decoder.decoder);
    if (decoded == nullptr) {
        return 0;
    }
    return sounds().add(std::make_unique<sound_t>(std::move(decoded)));
}

FBE_AUDIO_API uint64_t fbe_sound_stream(const uint8_t* data, int64_t size) {
    decoder_t decoder;
    if (!open(decoder, data, size)) {
        return 0;
    }
    std::unique_ptr<fbe::audio::sound_data_t> streamed = describe(decoder.decoder);
    if (streamed == nullptr) {
        return 0;
    }
    streamed->frames = count_frames(decoder.decoder, streamed->channels);
    if (streamed->frames == 0) {
        fbe::audio::fail("the sound has no frames");
        return 0;
    }
    streamed->encoded.assign(data, data + size);
    return sounds().add(std::make_unique<sound_t>(std::move(streamed)));
}

FBE_AUDIO_API bool fbe_sound_streamed(uint64_t sound) {
    const sound_t* found = sounds().find(sound);
    return found != nullptr && found->data->streamed();
}

FBE_AUDIO_API void fbe_sound_free(uint64_t sound) {
    sounds().remove(sound);
}

FBE_AUDIO_API int32_t fbe_sound_sample_rate(uint64_t sound) {
    const sound_t* found = sounds().find(sound);
    return found != nullptr ? static_cast<int32_t>(found->data->sample_rate) : 0;
}

FBE_AUDIO_API int32_t fbe_sound_channels(uint64_t sound) {
    const sound_t* found = sounds().find(sound);
    return found != nullptr ? static_cast<int32_t>(found->data->channels) : 0;
}

FBE_AUDIO_API int64_t fbe_sound_frames(uint64_t sound) {
    const sound_t* found = sounds().find(sound);
    return found != nullptr ? static_cast<int64_t>(found->data->frames) : 0;
}

FBE_AUDIO_API uint64_t fbe_sound_play(uint64_t sound, float volume, float pan, float pitch,
                                      bool loop, bool paused, double fade_in, uint64_t bus,
                                      uint64_t emitter) {
    if (!fbe::audio::check_volume(volume) || !fbe::audio::check_pan(pan) ||
        !fbe::audio::check_pitch(pitch)) {
        return 0;
    }
    std::shared_ptr<const fbe::audio::sound_data_t> data = fbe::audio::find_sound(sound);
    if (data == nullptr) {
        fbe::audio::fail("the sound was freed");
        return 0;
    }
    std::shared_ptr<fbe::audio::bus_t> into = fbe::audio::find_bus(bus);
    if (bus != 0 && into == nullptr) {
        fbe::audio::fail("the bus was freed");
        return 0;
    }
    fbe::audio::emitter_t* from = fbe::audio::find_emitter(emitter);
    if (emitter != 0 && from == nullptr) {
        fbe::audio::fail("the emitter was freed");
        return 0;
    }
    const fbe::audio::play_t how = {
        .volume = volume,
        .pan = pan,
        .pitch = pitch,
        .loop = loop,
        .paused = paused,
        .fade_in = fade_in,
        .bus = std::move(into),
        .emitter = from,
    };
    const std::uint64_t channel = fbe::audio::play(std::move(data), how);
    if (channel != 0 && from != nullptr) {
        fbe::audio::join(*from, channel);
    }
    return channel;
}

} // extern "C"
