// Channels: each a miniaudio sound over a decoded sound's frames, in the mix. One whose sound has
// ended is let go the next time it's asked about or another plays, so finished channels never
// pile up and their handles find nothing.

#include "audio/mixer.hpp"
#include "audio/pool.hpp"
#include "channel/voices.hpp"
#include "fbengine_audio.h"

#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <utility>

namespace {

constexpr float lowest_pitch = 1.0F / 16.0F;
constexpr float highest_pitch = 16.0F;

/// A channel: its sound's frames, read through a buffer, into a miniaudio sound in the mix.
/// miniaudio's init functions clear each of its structs first.
struct voice_t {
    std::shared_ptr<const fbe::audio::sound_data_t> data;
    ma_audio_buffer_ref buffer;
    bool has_buffer = false;
    ma_sound sound;
    bool has_sound = false;
    bool paused = false;

    voice_t() = default;
    voice_t(const voice_t&) = delete;
    voice_t& operator=(const voice_t&) = delete;
    voice_t(voice_t&&) = delete;
    voice_t& operator=(voice_t&&) = delete;

    // The sound leaves the mix, which waits for the audio thread to be done with it, before the
    // buffer it reads goes.
    ~voice_t() {
        if (has_sound) {
            ma_sound_uninit(&sound);
        }
        if (has_buffer) {
            ma_audio_buffer_ref_uninit(&buffer);
        }
    }

    /// Whether its sound has played to its end.
    [[nodiscard]] bool ended() const {
        return ma_sound_at_end(&sound) != MA_FALSE;
    }
};

fbe::audio::pool_t<voice_t> voices;

/// The channel `handle` is for, or null for one that's finished, which goes now if it ended.
voice_t* find(std::uint64_t handle) {
    voice_t* voice = voices.find(handle);
    if (voice != nullptr && voice->ended()) {
        voices.remove(handle);
        return nullptr;
    }
    return voice;
}

} // namespace

namespace fbe::audio {

bool check_volume(float volume) {
    if (!std::isfinite(volume) || volume < 0.0F) {
        return fail(std::format("a volume is 0 or more, not {}", volume));
    }
    return true;
}

bool check_pan(float pan) {
    if (std::isnan(pan) || pan < -1.0F || pan > 1.0F) {
        return fail(std::format("a pan is -1, the left, to 1, the right, not {}", pan));
    }
    return true;
}

bool check_pitch(float pitch) {
    if (std::isnan(pitch) || pitch < lowest_pitch || pitch > highest_pitch) {
        return fail(std::format("a pitch is 1/16 to 16, not {}", pitch));
    }
    return true;
}

std::uint64_t play(std::shared_ptr<const sound_data_t> data, const play_t& how) {
    ma_engine* mix = engine();
    if (mix == nullptr) {
        fail("the audio isn't started");
        return 0;
    }
    voices.remove_if([](const voice_t& voice) { return voice.ended(); });

    auto voice = std::make_unique<voice_t>();
    voice->data = std::move(data);
    ma_result result =
        ma_audio_buffer_ref_init(ma_format_f32, voice->data->channels, voice->data->samples.data(),
                                 voice->data->frames, &voice->buffer);
    if (result != MA_SUCCESS) {
        fail_ma("couldn't play the sound", result);
        return 0;
    }
    voice->has_buffer = true;
    // A buffer's rate isn't one of its init's arguments, and 0 would be the mix's.
    voice->buffer.sampleRate = voice->data->sample_rate;
    result = ma_sound_init_from_data_source(mix, &voice->buffer, MA_SOUND_FLAG_NO_SPATIALIZATION,
                                            nullptr, &voice->sound);
    if (result != MA_SUCCESS) {
        fail_ma("couldn't play the sound", result);
        return 0;
    }
    voice->has_sound = true;

    ma_sound_set_volume(&voice->sound, how.volume);
    ma_sound_set_pan(&voice->sound, how.pan);
    ma_sound_set_pitch(&voice->sound, how.pitch);
    ma_sound_set_looping(&voice->sound, how.loop ? MA_TRUE : MA_FALSE);
    voice->paused = how.paused;
    if (!how.paused) {
        result = ma_sound_start(&voice->sound);
        if (result != MA_SUCCESS) {
            fail_ma("couldn't play the sound", result);
            return 0;
        }
    }
    return voices.add(std::move(voice));
}

void end_channels() {
    voices.clear();
}

} // namespace fbe::audio

extern "C" {

FBE_AUDIO_API bool fbe_channel_playing(uint64_t channel) {
    return find(channel) != nullptr;
}

FBE_AUDIO_API bool fbe_channel_paused(uint64_t channel) {
    const voice_t* voice = find(channel);
    return voice != nullptr && voice->paused;
}

FBE_AUDIO_API void fbe_channel_stop(uint64_t channel) {
    voices.remove(channel);
}

FBE_AUDIO_API void fbe_channel_pause(uint64_t channel) {
    voice_t* voice = find(channel);
    if (voice != nullptr && !voice->paused) {
        ma_sound_stop(&voice->sound);
        voice->paused = true;
    }
}

FBE_AUDIO_API void fbe_channel_resume(uint64_t channel) {
    voice_t* voice = find(channel);
    if (voice != nullptr && voice->paused) {
        ma_sound_start(&voice->sound);
        voice->paused = false;
    }
}

FBE_AUDIO_API bool fbe_channel_set_volume(uint64_t channel, float volume) {
    if (!fbe::audio::check_volume(volume)) {
        return false;
    }
    voice_t* voice = find(channel);
    if (voice != nullptr) {
        ma_sound_set_volume(&voice->sound, volume);
    }
    return true;
}

FBE_AUDIO_API float fbe_channel_volume(uint64_t channel) {
    const voice_t* voice = find(channel);
    return voice != nullptr ? ma_sound_get_volume(&voice->sound) : 0.0F;
}

FBE_AUDIO_API bool fbe_channel_set_pan(uint64_t channel, float pan) {
    if (!fbe::audio::check_pan(pan)) {
        return false;
    }
    voice_t* voice = find(channel);
    if (voice != nullptr) {
        ma_sound_set_pan(&voice->sound, pan);
    }
    return true;
}

FBE_AUDIO_API float fbe_channel_pan(uint64_t channel) {
    const voice_t* voice = find(channel);
    return voice != nullptr ? ma_sound_get_pan(&voice->sound) : 0.0F;
}

FBE_AUDIO_API bool fbe_channel_set_pitch(uint64_t channel, float pitch) {
    if (!fbe::audio::check_pitch(pitch)) {
        return false;
    }
    voice_t* voice = find(channel);
    if (voice != nullptr) {
        ma_sound_set_pitch(&voice->sound, pitch);
    }
    return true;
}

FBE_AUDIO_API float fbe_channel_pitch(uint64_t channel) {
    const voice_t* voice = find(channel);
    return voice != nullptr ? ma_sound_get_pitch(&voice->sound) : 0.0F;
}

FBE_AUDIO_API void fbe_channel_set_loop(uint64_t channel, bool loop) {
    voice_t* voice = find(channel);
    if (voice != nullptr) {
        ma_sound_set_looping(&voice->sound, loop ? MA_TRUE : MA_FALSE);
    }
}

FBE_AUDIO_API bool fbe_channel_looping(uint64_t channel) {
    const voice_t* voice = find(channel);
    return voice != nullptr && ma_sound_is_looping(&voice->sound) != MA_FALSE;
}

FBE_AUDIO_API int32_t fbe_channel_count(void) {
    voices.remove_if([](const voice_t& voice) { return voice.ended(); });
    return static_cast<int32_t>(voices.count());
}

} // extern "C"
