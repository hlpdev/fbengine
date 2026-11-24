// Capture: a miniaudio capture device of its own, apart from the mix's, whose callback puts what
// it hears into a ring buffer, which the main thread reads; on device 2 there's no device, and
// fbe_capture_hear fills the ring instead.

#include "audio/backends.hpp"
#include "audio/mixer.hpp"
#include "audio/outputs.hpp"
#include "fbengine_audio.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <miniaudio.h>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// fbe_capture_start's devices, as fbe_audio_start's.
constexpr std::int32_t default_device = 0;
constexpr std::int32_t null_device = 1;
constexpr std::int32_t no_device = 2;

/// What a capture with no device hears at when it isn't told.
constexpr std::int32_t standard_rate = 48000;
constexpr std::int32_t standard_channels = 1;
constexpr std::int32_t most_channels = 8;
constexpr std::int32_t slowest = ma_standard_sample_rate_min;
constexpr std::int32_t fastest = ma_standard_sample_rate_max;
/// The seconds it keeps unread, and what it keeps when it isn't told.
constexpr double fewest_seconds = 0.01;
constexpr double most_seconds = 60.0;
constexpr double standard_seconds = 1.0;

/// The started capture, which stays where it's made, as miniaudio's device and context are
/// pointed at from its threads. miniaudio's init functions clear each of its structs first.
struct capture_t {
    ma_context context;
    ma_device device;
    /// What it's heard and not been read: the device's thread writes it, the main thread reads it.
    ma_pcm_rb ring;
    std::string name;
    std::int32_t kind = -1;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    std::atomic<std::int64_t> dropped = 0;
    /// The device's threads': whether it stopped on its own.
    std::atomic<bool> lost = false;
    /// The main thread's, for the device's threads: whether it's stopping it, which isn't a loss.
    std::atomic<bool> stopping = false;
    bool has_context = false;
    bool has_device = false;
    bool has_ring = false;

    capture_t() = default;
    capture_t(const capture_t&) = delete;
    capture_t& operator=(const capture_t&) = delete;
    capture_t(capture_t&&) = delete;
    capture_t& operator=(capture_t&&) = delete;

    // The device stops before the ring it writes goes.
    ~capture_t() {
        stopping.store(true, std::memory_order_release);
        if (has_device) {
            ma_device_uninit(&device);
        }
        if (has_ring) {
            ma_pcm_rb_uninit(&ring);
        }
        if (has_context) {
            ma_context_uninit(&context);
        }
    }

    /// Keeps the `frames` frames at `samples`, as many as there's room for, and counts the rest
    /// as dropped. The one thread that writes it's.
    void keep(const float* samples, ma_uint32 frames) {
        ma_uint32 done = 0;
        while (done < frames) {
            ma_uint32 count = frames - done;
            void* room = nullptr;
            if (ma_pcm_rb_acquire_write(&ring, &count, &room) != MA_SUCCESS || count == 0) {
                break;
            }
            std::copy_n(samples + (static_cast<std::size_t>(done) * channels),
                        static_cast<std::size_t>(count) * channels, static_cast<float*>(room));
            ma_pcm_rb_commit_write(&ring, count);
            done += count;
        }
        dropped.fetch_add(frames - done, std::memory_order_relaxed);
    }
};

/// The started capture, or null. A program that ends with it started leaves it be, as the mix's,
/// since its device's thread may still be capturing as the C++ runtime tears down.
capture_t* capturing = nullptr;

/// The inputs fbe_capture_list_inputs found, the main thread's.
std::vector<std::string> inputs;

/// The device's callback, on its own thread: what it heard.
// NOLINTNEXTLINE(misc-const-correctness): miniaudio's callbacks take a ma_device*.
void hear(ma_device* device, void* /*output*/, const void* input, ma_uint32 frames) {
    auto* capture = static_cast<capture_t*>(device->pUserData);
    capture->keep(static_cast<const float*>(input), frames);
}

/// The device's notifications, on its threads: one that stops while it isn't being stopped is
/// lost, as one whose microphone's unplugged.
void notified(const ma_device_notification* notification) {
    auto* capture = static_cast<capture_t*>(notification->pDevice->pUserData);
    if (notification->type == ma_device_notification_type_stopped &&
        !capture->stopping.load(std::memory_order_acquire)) {
        capture->lost.store(true, std::memory_order_release);
    }
}

/// Opens `context` on `kind`'s backends: the null one, or the OS's own; false if there's none.
bool open_context(ma_context& context, std::int32_t kind) {
    const std::vector<ma_backend> backends = kind == null_device
                                                 ? std::vector<ma_backend>{ ma_backend_null }
                                                 : fbe::audio::output_backends();
    const ma_result result = ma_context_init(
        backends.data(), static_cast<ma_uint32>(backends.size()), nullptr, &context);
    if (result != MA_SUCCESS) {
        return fbe::audio::fail_ma(kind == null_device ? "couldn't start the null backend"
                                                       : "there's no audio input to capture from",
                                   result);
    }
    return true;
}

/// The inputs `context` has, by the names they're listed and chosen by, with their ids.
std::pair<std::vector<std::string>, std::vector<ma_device_id>> list(ma_context& context) {
    ma_device_info* found = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&context, nullptr, nullptr, &found, &count) != MA_SUCCESS) {
        return {};
    }
    std::vector<std::string> names;
    std::vector<ma_device_id> ids;
    for (const ma_device_info& info : std::span<ma_device_info>(found, count)) {
        names.emplace_back(info.name);
        ids.push_back(info.id);
    }
    return { fbe::audio::output_names(names), std::move(ids) };
}

/// Opens `made`'s device on its context, from `input`, or the default for "", at `sample_rate`
/// and `channels`, 0 for its own; false if it won't open.
bool open_device(capture_t& made, std::string_view input, std::int32_t sample_rate,
                 std::int32_t channels) {
    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_f32;
    config.capture.channels = static_cast<ma_uint32>(channels);
    config.sampleRate = static_cast<ma_uint32>(sample_rate);
    config.dataCallback = hear;
    config.notificationCallback = notified;
    config.pUserData = &made;
    ma_device_id id{};
    if (!input.empty()) {
        const auto [names, ids] = list(made.context);
        const auto named = std::ranges::find(names, input);
        if (named == names.end()) {
            return fbe::audio::fail(std::format("there's no audio input named {}", input));
        }
        id = ids[static_cast<std::size_t>(named - names.begin())];
        config.capture.pDeviceID = &id;
    }
    const ma_result result = ma_device_init(&made.context, &config, &made.device);
    if (result != MA_SUCCESS) {
        return fbe::audio::fail_ma("couldn't open the audio input", result);
    }
    made.has_device = true;
    made.sample_rate = made.device.sampleRate;
    made.channels = made.device.capture.channels;
    std::array<char, MA_MAX_DEVICE_NAME_LENGTH + 1> name{};
    if (ma_device_get_name(&made.device, ma_device_type_capture, name.data(), name.size(),
                           nullptr) == MA_SUCCESS) {
        made.name = name.data();
    }
    return true;
}

/// Whether each of fbe_capture_start's values is in range, or false, with fbe_audio_error's
/// message.
bool check(std::int32_t device, std::int32_t sample_rate, std::int32_t channels, double seconds) {
    if (device < default_device || device > no_device) {
        return fbe::audio::fail(std::format("there's no audio device {}", device));
    }
    if (sample_rate != 0 && (sample_rate < slowest || sample_rate > fastest)) {
        return fbe::audio::fail(std::format("a capture's sample rate is {} to {} Hz, or 0, not {}",
                                            slowest, fastest, sample_rate));
    }
    if (channels < 0 || channels > most_channels) {
        return fbe::audio::fail(
            std::format("a capture has 1 to {} channels, or 0, not {}", most_channels, channels));
    }
    if (seconds != 0.0 &&
        (std::isnan(seconds) || seconds < fewest_seconds || seconds > most_seconds)) {
        return fbe::audio::fail(std::format("a capture keeps {} to {} seconds, or 0, not {}",
                                            fewest_seconds, most_seconds, seconds));
    }
    return true;
}

} // namespace

extern "C" {

FBE_AUDIO_API int32_t fbe_capture_list_inputs(int32_t device) try {
    inputs.clear();
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
    try {
        inputs = list(context).first;
    } catch (const std::bad_alloc&) {
        ma_context_uninit(&context);
        throw;
    }
    ma_context_uninit(&context);
    return static_cast<int32_t>(inputs.size());
} catch (const std::bad_alloc&) {
    inputs.clear();
    fbe::audio::out_of_memory();
    return -1;
}

FBE_AUDIO_API const char* fbe_capture_input_name(int32_t index) {
    if (index < 0 || std::cmp_greater_equal(index, inputs.size())) {
        return "";
    }
    return inputs[static_cast<std::size_t>(index)].c_str();
}

FBE_AUDIO_API bool fbe_capture_start(int32_t device, const char* input, int32_t sample_rate,
                                     int32_t channels, double seconds) try {
    if (capturing != nullptr) {
        return fbe::audio::fail("the capture's started already");
    }
    if (!check(device, sample_rate, channels, seconds)) {
        return false;
    }
    const std::string_view named = input != nullptr ? input : "";
    if (device != default_device && !named.empty()) {
        return fbe::audio::fail("only the OS's device has inputs to name");
    }
    auto made = std::make_unique<capture_t>();
    made->kind = device;
    if (device == no_device) {
        made->sample_rate =
            static_cast<std::uint32_t>(sample_rate != 0 ? sample_rate : standard_rate);
        made->channels = static_cast<std::uint32_t>(channels != 0 ? channels : standard_channels);
    } else {
        if (!open_context(made->context, device)) {
            return false;
        }
        made->has_context = true;
        if (!open_device(*made, named, sample_rate, channels)) {
            return false;
        }
    }
    const double kept = seconds != 0.0 ? seconds : standard_seconds;
    const auto room =
        std::max<ma_uint32>(1, static_cast<ma_uint32>(std::lround(kept * made->sample_rate)));
    const ma_result result =
        ma_pcm_rb_init(ma_format_f32, made->channels, room, nullptr, nullptr, &made->ring);
    if (result != MA_SUCCESS) {
        return fbe::audio::fail_ma("couldn't start capturing", result);
    }
    made->has_ring = true;
    if (made->has_device) {
        const ma_result started = ma_device_start(&made->device);
        if (started != MA_SUCCESS) {
            return fbe::audio::fail_ma("couldn't start the audio input", started);
        }
    }
    capturing = made.release();
    return true;
} catch (const std::bad_alloc&) {
    return fbe::audio::out_of_memory();
}

FBE_AUDIO_API void fbe_capture_stop(void) {
    delete capturing;
    capturing = nullptr;
}

FBE_AUDIO_API bool fbe_capture_started(void) {
    return capturing != nullptr;
}

FBE_AUDIO_API int32_t fbe_capture_device(void) {
    return capturing != nullptr ? capturing->kind : -1;
}

FBE_AUDIO_API const char* fbe_capture_input(void) {
    return capturing != nullptr ? capturing->name.c_str() : "";
}

FBE_AUDIO_API int32_t fbe_capture_sample_rate(void) {
    return capturing != nullptr ? static_cast<int32_t>(capturing->sample_rate) : 0;
}

FBE_AUDIO_API int32_t fbe_capture_channels(void) {
    return capturing != nullptr ? static_cast<int32_t>(capturing->channels) : 0;
}

FBE_AUDIO_API int32_t fbe_capture_available(void) {
    return capturing != nullptr ? static_cast<int32_t>(ma_pcm_rb_available_read(&capturing->ring))
                                : 0;
}

FBE_AUDIO_API int32_t fbe_capture_read(float* samples, int32_t frames) {
    if (capturing == nullptr) {
        fbe::audio::fail("the capture isn't started");
        return -1;
    }
    if (frames < 0 || (samples == nullptr && frames > 0)) {
        fbe::audio::fail(std::format("a read is 0 frames or more, not {}", frames));
        return -1;
    }
    const std::size_t channels = capturing->channels;
    std::int32_t done = 0;
    while (done < frames) {
        auto count = static_cast<ma_uint32>(frames - done);
        void* chunk = nullptr;
        if (ma_pcm_rb_acquire_read(&capturing->ring, &count, &chunk) != MA_SUCCESS || count == 0) {
            break;
        }
        std::copy_n(static_cast<const float*>(chunk), count * channels,
                    samples + (static_cast<std::size_t>(done) * channels));
        ma_pcm_rb_commit_read(&capturing->ring, count);
        done += static_cast<std::int32_t>(count);
    }
    return done;
}

FBE_AUDIO_API int64_t fbe_capture_dropped(void) {
    return capturing != nullptr ? capturing->dropped.load(std::memory_order_relaxed) : 0;
}

FBE_AUDIO_API bool fbe_capture_lost(void) {
    return capturing != nullptr && capturing->lost.load(std::memory_order_acquire);
}

FBE_AUDIO_API bool fbe_capture_hear(const float* samples, int32_t frames) {
    if (capturing == nullptr || capturing->kind != no_device) {
        return fbe::audio::fail("only a capture started on device 2 is heard by hand");
    }
    if (frames < 0 || (samples == nullptr && frames > 0)) {
        return fbe::audio::fail(std::format("a hearing is 0 frames or more, not {}", frames));
    }
    capturing->keep(samples, static_cast<ma_uint32>(frames));
    return true;
}

FBE_AUDIO_API bool fbe_capture_simulate_unplug(void) {
    if (capturing == nullptr || !capturing->has_device) {
        return fbe::audio::fail("the capture isn't started on a device");
    }
    ma_device_stop(&capturing->device);
    capturing->lost.store(true, std::memory_order_release);
    return true;
}

} // extern "C"
