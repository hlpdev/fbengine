// The mix: fbe_audio_start makes miniaudio's engine, on the OS's default device, on the null
// backend, or on none, where fbe_audio_mix pulls each frame, as tests and offline renders do.

#include "audio/backends.hpp"
#include "audio/mixer.hpp"
#include "bus/buses.hpp"
#include "channel/voices.hpp"
#include "fbengine_audio.h"
#include "listener/listener.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// fbe_audio_start's devices.
constexpr std::int32_t default_device = 0;
constexpr std::int32_t null_device = 1;
constexpr std::int32_t no_device = 2;

/// What a mix with no device runs at when it isn't told.
constexpr std::int32_t standard_rate = 48000;
constexpr std::int32_t standard_channels = 2;
constexpr std::int32_t most_channels = 8;
/// The seconds a device mixes at a time, as a start can ask for.
constexpr double shortest_period = 0.001;
constexpr double longest_period = 1.0;
constexpr double milliseconds = 1000.0;
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
    /// The seconds Start asked the device to mix at a time, which a device made again keeps.
    double period = 0.0;
    /// The main thread's: whether the last fbe_audio_recover made the device again, which keeps it
    /// lost until the next, and whether fbe_audio_simulate unplugged every output.
    bool fell_back = false;
    bool unplugged_all = false;
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
    fbe::audio::update_buses(&playing->engine);
    fbe::audio::update_listener(&playing->engine);
    ma_engine_read_pcm_frames(&playing->engine, output, frames, nullptr);
    fbe::audio::after_mix();
    playing->frames.fetch_add(frames, std::memory_order_relaxed);
}

/// The outputs fbe_audio_list_outputs found, the main thread's.
std::vector<std::string> outputs;

/// Opens `context` on `kind`'s backends: the null one, or the OS's own; false if there's none.
bool open_context(ma_context& context, std::int32_t kind) {
    const std::vector<ma_backend> backends = kind == null_device
                                                 ? std::vector<ma_backend>{ ma_backend_null }
                                                 : fbe::audio::output_backends();
    const ma_result result = ma_context_init(
        backends.data(), static_cast<ma_uint32>(backends.size()), nullptr, &context);
    if (result != MA_SUCCESS) {
        return fbe::audio::fail_ma(kind == null_device ? "couldn't start the null backend"
                                                       : "there's no audio output to play to",
                                   result);
    }
    return true;
}

/// How a device is opened: its rate and channels, 0 for its own, the output named, "" for the
/// OS's default, and the seconds it mixes at a time, 0 for its own.
struct opening_t {
    std::int32_t sample_rate = 0;
    std::int32_t channels = 0;
    std::string_view output;
    double period = 0.0;
};

/// Opens `made`'s device on its context; false if it won't open, as on a machine with no output for
/// the OS's, or with no output of the name asked for.
bool open_device(mixer_t& made, const opening_t& how) {
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = static_cast<ma_uint32>(how.channels);
    config.sampleRate = static_cast<ma_uint32>(how.sample_rate);
    config.periodSizeInMilliseconds =
        static_cast<ma_uint32>(std::llround(how.period * milliseconds));
    config.dataCallback = play;
    config.pUserData = &made;
    ma_device_id id{};
    if (!how.output.empty()) {
        ma_device_info* found = nullptr;
        ma_uint32 count = 0;
        if (ma_context_get_devices(&made.context, &found, &count, nullptr, nullptr) != MA_SUCCESS) {
            count = 0;
        }
        const std::span<ma_device_info> infos(found, count);
        const auto named = std::ranges::find_if(
            infos, [&](const ma_device_info& info) { return how.output == info.name; });
        if (named == infos.end()) {
            return fbe::audio::fail(std::format("there's no audio output named {}", how.output));
        }
        id = named->id;
        config.playback.pDeviceID = &id;
    }
    const ma_result result = ma_device_init(&made.context, &config, &made.device);
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

FBE_AUDIO_API bool fbe_audio_start(int32_t device, int32_t sample_rate, int32_t channels,
                                   const char* output, double period) {
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
    if (period != 0.0 &&
        (std::isnan(period) || period < shortest_period || period > longest_period)) {
        return fbe::audio::fail(std::format("a period is 0, for the device's, or {} to {} seconds, "
                                            "not {}",
                                            shortest_period, longest_period, period));
    }

    auto made = std::make_unique<mixer_t>();
    ma_engine_config config = ma_engine_config_init();
    if (device == no_device) {
        config.noDevice = MA_TRUE;
        config.sampleRate = static_cast<ma_uint32>(sample_rate != 0 ? sample_rate : standard_rate);
        config.channels = static_cast<ma_uint32>(channels != 0 ? channels : standard_channels);
    } else {
        const opening_t how = {
            .sample_rate = sample_rate,
            .channels = channels,
            .output = output != nullptr ? output : "",
            .period = period,
        };
        if (!open_context(made->context, device)) {
            return false;
        }
        made->has_context = true;
        if (!open_device(*made, how)) {
            return false;
        }
        made->period = period;
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
    fbe::audio::start_listener(&made->engine);
    // The buses' nodes are made in the mix before its device starts.
    mixer = made.release();
    if (!fbe::audio::start_buses()) {
        fbe_audio_stop();
        return false;
    }
    if (mixer->has_device) {
        const ma_result started = ma_device_start(&mixer->device);
        if (started != MA_SUCCESS) {
            fbe_audio_stop();
            return fbe::audio::fail_ma("couldn't start the audio device", started);
        }
    }
    return true;
}

FBE_AUDIO_API void fbe_audio_stop(void) {
    fbe::audio::end_channels();
    fbe::audio::stop_buses();
    delete mixer;
    mixer = nullptr;
}

FBE_AUDIO_API bool fbe_audio_started(void) {
    return mixer != nullptr;
}

FBE_AUDIO_API int32_t fbe_audio_list_outputs(int32_t device) {
    outputs.clear();
    if (device == no_device) {
        return 0;
    }
    if (device < default_device || device > no_device) {
        fbe::audio::fail(std::format("there's no audio device {}", device));
        return -1;
    }
    ma_context context;
    if (!open_context(context, device)) {
        return -1;
    }
    ma_device_info* found = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&context, &found, &count, nullptr, nullptr) == MA_SUCCESS) {
        for (const ma_device_info& info : std::span<ma_device_info>(found, count)) {
            outputs.emplace_back(info.name);
        }
    }
    ma_context_uninit(&context);
    return static_cast<int32_t>(outputs.size());
}

FBE_AUDIO_API const char* fbe_audio_output_name(int32_t index) {
    if (index < 0 || std::cmp_greater_equal(index, outputs.size())) {
        return "";
    }
    return outputs[static_cast<std::size_t>(index)].c_str();
}

FBE_AUDIO_API double fbe_audio_period(void) {
    if (mixer == nullptr || !mixer->has_device) {
        return 0.0;
    }
    return static_cast<double>(mixer->device.playback.internalPeriodSizeInFrames) /
           static_cast<double>(mixer->device.sampleRate);
}

FBE_AUDIO_API int32_t fbe_audio_device(void) {
    return mixer != nullptr ? mixer->kind : -1;
}

FBE_AUDIO_API const char* fbe_audio_device_name(void) {
    return mixer != nullptr ? mixer->device_name.c_str() : "";
}

FBE_AUDIO_API bool fbe_audio_lost(void) {
    if (mixer == nullptr || mixer->kind == no_device) {
        return false;
    }
    return !mixer->has_device || mixer->fell_back ||
           ma_device_get_state(&mixer->device) != ma_device_state_started;
}

FBE_AUDIO_API void fbe_audio_recover(void) {
    if (mixer == nullptr || mixer->kind == no_device) {
        return;
    }
    mixer->fell_back = false;
    if (mixer->has_device) {
        // Started, or starting or stopping on one of miniaudio's threads, it's left as it is. A
        // stopped one stays where it is, so the engine's pointer to it holds.
        if (ma_device_get_state(&mixer->device) != ma_device_state_stopped) {
            return;
        }
        ma_device_uninit(&mixer->device);
        mixer->has_device = false;
        mixer->device_name.clear();
    }
    if (mixer->unplugged_all) {
        return;
    }
    const opening_t how = {
        .sample_rate = static_cast<std::int32_t>(ma_engine_get_sample_rate(&mixer->engine)),
        .channels = static_cast<std::int32_t>(ma_engine_get_channels(&mixer->engine)),
        .output = "",
        .period = mixer->period,
    };
    // One that won't open or start is tried again at the next flip.
    if (open_device(*mixer, how) && ma_device_start(&mixer->device) == MA_SUCCESS) {
        mixer->fell_back = true;
    }
}

FBE_AUDIO_API bool fbe_audio_simulate(int32_t event) {
    if (mixer == nullptr || mixer->kind == no_device) {
        return fbe::audio::fail("only a device's output is unplugged; start the audio on one");
    }
    // As miniaudio does when the OS says an output's gone: it stops the device. When it says one's
    // back, miniaudio starts only a default output again, so fbe_audio_recover makes it again.
    switch (event) {
    case FBE_AUDIO_SIMULATE_UNPLUG_ALL:
        mixer->unplugged_all = true;
        [[fallthrough]];
    case FBE_AUDIO_SIMULATE_UNPLUG: {
        if (!mixer->has_device) {
            return true;
        }
        const ma_result stopped = ma_device_stop(&mixer->device);
        return stopped == MA_SUCCESS ||
               fbe::audio::fail_ma("couldn't stop the audio device", stopped);
    }
    case FBE_AUDIO_SIMULATE_PLUG_IN:
        mixer->unplugged_all = false;
        return true;
    default:
        return fbe::audio::fail(std::format("there's no audio event {}", event));
    }
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
    fbe::audio::update_buses(&mixer->engine);
    fbe::audio::update_listener(&mixer->engine);
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
    // Clipped to -1 to 1, as a device's output is.
    ma_clip_samples_f32(samples, samples, static_cast<ma_uint64>(frames) * channels);
    mixer->frames.fetch_add(frames, std::memory_order_relaxed);
    return true;
}

} // extern "C"
