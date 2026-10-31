// The mix: fbe_audio_start makes miniaudio's engine, on the OS's default device, on the null
// backend, or on none, where fbe_audio_mix pulls each frame, as tests and offline renders do.

#include "audio/mixer.hpp"
#include "channel/voices.hpp"
#include "fbengine_audio.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace {

/// fbe_audio_start's devices.
constexpr std::int32_t default_device = 0;
constexpr std::int32_t null_device = 1;
constexpr std::int32_t no_device = 2;

/// What a mix with no device runs at when it isn't told.
constexpr std::int32_t standard_rate = 48000;
constexpr std::int32_t standard_channels = 2;
constexpr std::int32_t most_channels = 8;
constexpr std::int32_t slowest = ma_standard_sample_rate_min;
constexpr std::int32_t fastest = ma_standard_sample_rate_max;

thread_local std::string last_error;

/// The started mix, which stays where it's made, as miniaudio's device, engine, and context are
/// pointed at from its threads. The device is its own, not the engine's, so it counts what it
/// mixes. miniaudio's init functions clear each of its structs first.
struct mixer_t {
    ma_context context;
    bool has_context = false;
    ma_device device;
    bool has_device = false;
    ma_engine engine;
    bool has_engine = false;
    std::int32_t kind = -1;
    std::string device_name;
    std::atomic<std::int64_t> frames = 0;

    mixer_t() = default;
    mixer_t(const mixer_t&) = delete;
    mixer_t& operator=(const mixer_t&) = delete;
    mixer_t(mixer_t&&) = delete;
    mixer_t& operator=(mixer_t&&) = delete;

    // The device stops before the engine it mixes goes.
    ~mixer_t() {
        if (has_device) {
            ma_device_uninit(&device);
        }
        if (has_engine) {
            ma_engine_uninit(&engine);
        }
        if (has_context) {
            ma_context_uninit(&context);
        }
    }
};

/// The started mix, or null. A program that ends with it started leaves it be, since its device's
/// thread may still be mixing as the C++ runtime tears down.
mixer_t* mixer = nullptr;
float volume = 1.0F;

/// The device's callback, on its own thread: the next frames of the mix. The output starts
/// silent, so what the engine doesn't make stays silence.
// NOLINTNEXTLINE(misc-const-correctness): miniaudio's callbacks take a ma_device*.
void play(ma_device* device, void* output, const void* /*input*/, ma_uint32 frames) {
    auto* playing = static_cast<mixer_t*>(device->pUserData);
    fbe::audio::before_mix();
    ma_engine_read_pcm_frames(&playing->engine, output, frames, nullptr);
    fbe::audio::after_mix();
    playing->frames.fetch_add(frames, std::memory_order_relaxed);
}

/// Opens `made`'s device, on the null backend or the OS's default; false if it won't open.
bool open_device(mixer_t& made, std::int32_t kind, std::int32_t sample_rate,
                 std::int32_t channels) {
    if (kind == null_device) {
        const std::array<ma_backend, 1> backends = { ma_backend_null };
        const ma_result result =
            ma_context_init(backends.data(), backends.size(), nullptr, &made.context);
        if (result != MA_SUCCESS) {
            return fbe::audio::fail_ma("couldn't start the null backend", result);
        }
        made.has_context = true;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = static_cast<ma_uint32>(channels);
    config.sampleRate = static_cast<ma_uint32>(sample_rate);
    config.dataCallback = play;
    config.pUserData = &made;
    const ma_result result =
        ma_device_init(made.has_context ? &made.context : nullptr, &config, &made.device);
    if (result != MA_SUCCESS) {
        return fbe::audio::fail_ma("couldn't open the audio device", result);
    }
    made.has_device = true;

    std::array<char, MA_MAX_DEVICE_NAME_LENGTH + 1> name{};
    if (ma_device_get_name(&made.device, ma_device_type_playback, name.data(), name.size(),
                           nullptr) == MA_SUCCESS) {
        made.device_name = name.data();
    }
    return true;
}

} // namespace

namespace fbe::audio {

bool fail(std::string_view message) {
    last_error = message;
    return false;
}

bool fail_ma(std::string_view doing, ma_result result) {
    return fail(std::format("{}: {}", doing, ma_result_description(result)));
}

ma_engine* engine() {
    return mixer != nullptr ? &mixer->engine : nullptr;
}

} // namespace fbe::audio

extern "C" {

FBE_AUDIO_API const char* fbe_audio_version(void) {
    return FBE_VERSION;
}

FBE_AUDIO_API const char* fbe_audio_error(void) {
    return last_error.c_str();
}

FBE_AUDIO_API bool fbe_audio_start(int32_t device, int32_t sample_rate, int32_t channels) {
    if (mixer != nullptr) {
        return fbe::audio::fail("the audio is started already; stop it first");
    }
    if (device < default_device || device > no_device) {
        return fbe::audio::fail(std::format("there's no audio device {}", device));
    }
    if (sample_rate != 0 && (sample_rate < slowest || sample_rate > fastest)) {
        return fbe::audio::fail(std::format("a sample rate is 0, for the device's, or {} to {} Hz, "
                                            "not {}",
                                            slowest, fastest, sample_rate));
    }
    if (channels < 0 || channels > most_channels) {
        return fbe::audio::fail(std::format("the audio has 0, for the device's, to {} channels, "
                                            "not {}",
                                            most_channels, channels));
    }

    auto made = std::make_unique<mixer_t>();
    ma_engine_config config = ma_engine_config_init();
    if (device == no_device) {
        config.noDevice = MA_TRUE;
        config.sampleRate = static_cast<ma_uint32>(sample_rate != 0 ? sample_rate : standard_rate);
        config.channels = static_cast<ma_uint32>(channels != 0 ? channels : standard_channels);
    } else {
        if (!open_device(*made, device, sample_rate, channels)) {
            return false;
        }
        config.pDevice = &made->device;
        config.noAutoStart = MA_TRUE;
    }

    const ma_result result = ma_engine_init(&config, &made->engine);
    if (result != MA_SUCCESS) {
        return fbe::audio::fail_ma("couldn't start the audio", result);
    }
    made->has_engine = true;
    made->kind = device;
    ma_engine_set_volume(&made->engine, volume);
    if (made->has_device) {
        const ma_result started = ma_device_start(&made->device);
        if (started != MA_SUCCESS) {
            return fbe::audio::fail_ma("couldn't start the audio device", started);
        }
    }
    mixer = made.release();
    return true;
}

FBE_AUDIO_API void fbe_audio_stop(void) {
    fbe::audio::end_channels();
    delete mixer;
    mixer = nullptr;
}

FBE_AUDIO_API bool fbe_audio_started(void) {
    return mixer != nullptr;
}

FBE_AUDIO_API int32_t fbe_audio_device(void) {
    return mixer != nullptr ? mixer->kind : -1;
}

FBE_AUDIO_API const char* fbe_audio_device_name(void) {
    return mixer != nullptr ? mixer->device_name.c_str() : "";
}

FBE_AUDIO_API int32_t fbe_audio_sample_rate(void) {
    return mixer != nullptr ? static_cast<int32_t>(ma_engine_get_sample_rate(&mixer->engine)) : 0;
}

FBE_AUDIO_API int32_t fbe_audio_channels(void) {
    return mixer != nullptr ? static_cast<int32_t>(ma_engine_get_channels(&mixer->engine)) : 0;
}

FBE_AUDIO_API int64_t fbe_audio_frames(void) {
    return mixer != nullptr ? mixer->frames.load(std::memory_order_relaxed) : 0;
}

FBE_AUDIO_API bool fbe_audio_set_volume(float to) {
    if (!std::isfinite(to) || to < 0.0F) {
        return fbe::audio::fail(std::format("a volume is 0 or more, not {}", to));
    }
    volume = to;
    if (mixer != nullptr) {
        ma_engine_set_volume(&mixer->engine, volume);
    }
    return true;
}

FBE_AUDIO_API float fbe_audio_volume(void) {
    return volume;
}

FBE_AUDIO_API bool fbe_audio_mix(float* samples, int32_t frames) {
    if (mixer == nullptr || mixer->kind != no_device) {
        return fbe::audio::fail("only audio started with no device is mixed by a call");
    }
    if (frames < 0) {
        return fbe::audio::fail(std::format("a mix is 0 frames or more, not {}", frames));
    }
    if (frames == 0) {
        return true;
    }
    ma_uint64 read = 0;
    fbe::audio::before_mix();
    const ma_result result =
        ma_engine_read_pcm_frames(&mixer->engine, samples, static_cast<ma_uint64>(frames), &read);
    fbe::audio::after_mix();
    if (result != MA_SUCCESS && result != MA_AT_END) {
        return fbe::audio::fail_ma("couldn't mix", result);
    }
    // What the graph didn't make is silence.
    const ma_uint32 channels = ma_engine_get_channels(&mixer->engine);
    ma_silence_pcm_frames(ma_offset_pcm_frames_ptr_f32(samples, read, channels),
                          static_cast<ma_uint64>(frames) - read, ma_format_f32, channels);
    mixer->frames.fetch_add(frames, std::memory_order_relaxed);
    return true;
}

} // extern "C"
