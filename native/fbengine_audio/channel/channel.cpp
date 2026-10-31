// Channels: each a miniaudio sound in the mix, over a decoded sound's frames or a decoder of a
// streamed sound's file. One that has ended is let go the next time it's asked about or another
// plays, so finished channels never pile up and their handles find nothing. Around each mix, on
// the thread that mixes, before_mix does the seeks asked for and after_mix keeps where each
// channel is, since miniaudio's own seek and cursor leave out what it has read ahead.

#include "audio/mixer.hpp"
#include "audio/pool.hpp"
#include "channel/voices.hpp"
#include "fbengine_audio.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace {

constexpr float lowest_pitch = 1.0F / 16.0F;
constexpr float highest_pitch = 16.0F;
constexpr std::int64_t no_seek = -1;

/// A channel: its sound's frames, read through a buffer or decoded as they play, into a miniaudio
/// sound in the mix. miniaudio's init functions clear each of its structs first.
struct voice_t {
    /// The main thread's, for the mixing thread: the frame to seek to.
    std::atomic<std::int64_t> seek_to = no_seek;
    /// The mixing thread's, for the main thread: the frame of its sound it has played to.
    std::atomic<std::uint64_t> position = 0;
    std::shared_ptr<const fbe::audio::sound_data_t> data;
    /// What it plays into, which it keeps while it plays, or null for the whole mix.
    std::shared_ptr<fbe::audio::bus_t> bus;
    ma_audio_buffer_ref buffer;
    ma_decoder decoder;
    ma_sound sound;
    bool has_buffer = false;
    bool has_decoder = false;
    bool has_sound = false;
    bool joined = false;
    bool paused = false;
    /// The main thread's, for the mixing thread: whether it finishes once its fade is silent.
    std::atomic<bool> fading_out = false;
    /// The mixing thread's, for the main thread: whether it faded out.
    std::atomic<bool> faded_out = false;

    voice_t() = default;
    voice_t(const voice_t&) = delete;
    voice_t& operator=(const voice_t&) = delete;
    voice_t(voice_t&&) = delete;
    voice_t& operator=(voice_t&&) = delete;

    ~voice_t();

    /// What its sound reads from.
    ma_data_source* source() {
        if (has_decoder) {
            return &decoder;
        }
        return &buffer;
    }

    /// Whether its sound has played to its end, or faded out.
    [[nodiscard]] bool ended() const {
        return ma_sound_at_end(&sound) != MA_FALSE || faded_out.load(std::memory_order_acquire);
    }

    /// The frames of its sound in `seconds`, or -1 when it's not 0 to its length.
    [[nodiscard]] std::int64_t frame_at(double seconds) const {
        const double frame = std::round(seconds * data->sample_rate);
        if (std::isnan(frame) || frame < 0.0 || frame > static_cast<double>(data->frames)) {
            return -1;
        }
        return static_cast<std::int64_t>(frame);
    }
};

/// The channels, and those the mixing thread sees, each joining once it's made and leaving
/// before it goes; made once and never let go, since a program that ends with the audio started
/// has its device's thread mixing as the C++ runtime tears down.
struct channels_t {
    std::mutex live_lock;
    std::vector<voice_t*> live;
    fbe::audio::pool_t<voice_t> voices;
};

channels_t& channels() {
    static auto* made = new channels_t();
    return *made;
}

// It leaves the mixing thread's list, then the mix, which waits for the mixing thread to be done
// with it, before what it reads goes.
voice_t::~voice_t() {
    if (joined) {
        const std::scoped_lock lock(channels().live_lock);
        std::erase(channels().live, this);
    }
    if (has_sound) {
        ma_sound_uninit(&sound);
    }
    if (has_decoder) {
        ma_decoder_uninit(&decoder);
    }
    if (has_buffer) {
        ma_audio_buffer_ref_uninit(&buffer);
    }
}

/// The channel `handle` is for, or null for one that's finished, which goes now if it ended.
voice_t* find(std::uint64_t handle) {
    voice_t* voice = channels().voices.find(handle);
    if (voice != nullptr && voice->ended()) {
        channels().voices.remove(handle);
        return nullptr;
    }
    return voice;
}

/// The frames a fade of `seconds` takes in the mix.
ma_uint64 fade_frames(double seconds) {
    return static_cast<ma_uint64>(
        std::llround(seconds * ma_engine_get_sample_rate(fbe::audio::engine())));
}

bool check_fade(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return fbe::audio::fail(std::format("a fade takes 0 seconds or more, not {}", seconds));
    }
    return true;
}

/// Where `voice` has played to, from the cursor of what it reads, less what miniaudio has read
/// ahead of the mix: what's in its sound's cache and its resampler. The mixing thread's.
std::uint64_t heard(voice_t& voice) {
    ma_uint64 cursor = 0;
    if (ma_data_source_get_cursor_in_pcm_frames(voice.source(), &cursor) != MA_SUCCESS) {
        return voice.position.load(std::memory_order_relaxed);
    }
    const ma_uint64 ahead = voice.sound.processingCacheFramesRemaining +
                            ma_resampler_get_input_latency(&voice.sound.engineNode.resampler);
    if (cursor >= ahead) {
        return cursor - ahead;
    }
    // A looping sound read ahead past its end, back to its start.
    const std::uint64_t frames = voice.data->frames;
    if (ma_sound_is_looping(&voice.sound) == MA_FALSE || frames == 0) {
        return 0;
    }
    return (cursor + frames - (ahead % frames)) % frames;
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
    if (!check_fade(how.fade_in)) {
        return 0;
    }
    channels().voices.remove_if([](const voice_t& voice) { return voice.ended(); });

    auto voice = std::make_unique<voice_t>();
    voice->data = std::move(data);
    ma_result result = MA_SUCCESS;
    if (voice->data->streamed()) {
        const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
        result = ma_decoder_init_memory(voice->data->encoded.data(), voice->data->encoded.size(),
                                        &config, &voice->decoder);
        voice->has_decoder = result == MA_SUCCESS;
    } else {
        result = ma_audio_buffer_ref_init(ma_format_f32, voice->data->channels,
                                          voice->data->samples.data(), voice->data->frames,
                                          &voice->buffer);
        voice->has_buffer = result == MA_SUCCESS;
        // A buffer's rate isn't one of its init's arguments, and 0 would be the mix's.
        voice->buffer.sampleRate = voice->data->sample_rate;
    }
    if (result != MA_SUCCESS) {
        fail_ma("couldn't play the sound", result);
        return 0;
    }
    voice->bus = how.bus;
    result = ma_sound_init_from_data_source(mix, voice->source(), MA_SOUND_FLAG_NO_SPATIALIZATION,
                                            bus_group(voice->bus.get()), &voice->sound);
    if (result != MA_SUCCESS) {
        fail_ma("couldn't play the sound", result);
        return 0;
    }
    voice->has_sound = true;

    ma_sound_set_volume(&voice->sound, how.volume);
    ma_sound_set_pan(&voice->sound, how.pan);
    ma_sound_set_pitch(&voice->sound, how.pitch);
    ma_sound_set_looping(&voice->sound, how.loop ? MA_TRUE : MA_FALSE);
    if (how.fade_in > 0.0) {
        ma_sound_set_fade_in_pcm_frames(&voice->sound, 0.0F, 1.0F, fade_frames(how.fade_in));
    }
    {
        const std::scoped_lock lock(channels().live_lock);
        channels().live.push_back(voice.get());
        voice->joined = true;
    }
    voice->paused = how.paused;
    if (!how.paused) {
        result = ma_sound_start(&voice->sound);
        if (result != MA_SUCCESS) {
            fail_ma("couldn't play the sound", result);
            return 0;
        }
    }
    return channels().voices.add(std::move(voice));
}

void end_channels() {
    channels().voices.clear();
}

void before_mix() {
    channels_t& all = channels();
    const std::scoped_lock lock(all.live_lock);
    for (voice_t* voice : all.live) {
        const std::int64_t target = voice->seek_to.load(std::memory_order_acquire);
        if (target == no_seek) {
            continue;
        }
        ma_data_source_seek_to_pcm_frame(voice->source(), static_cast<ma_uint64>(target));
        // miniaudio's own seek would play what it read ahead first.
        voice->sound.processingCacheFramesRemaining = 0;
        ma_resampler_reset(&voice->sound.engineNode.resampler);
        // A seek asked for since stays for the next mix.
        std::int64_t done = target;
        voice->seek_to.compare_exchange_strong(done, no_seek, std::memory_order_acq_rel);
    }
}

void after_mix() {
    channels_t& all = channels();
    const std::scoped_lock lock(all.live_lock);
    for (voice_t* voice : all.live) {
        // One asked for since this mix began is where it was asked to be until the next.
        if (voice->seek_to.load(std::memory_order_acquire) == no_seek) {
            voice->position.store(heard(*voice), std::memory_order_relaxed);
        }
        if (voice->fading_out.load(std::memory_order_acquire) &&
            ma_sound_get_current_fade_volume(&voice->sound) == 0.0F) {
            ma_sound_stop(&voice->sound);
            voice->faded_out.store(true, std::memory_order_release);
        }
    }
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
    channels().voices.remove(channel);
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

FBE_AUDIO_API double fbe_channel_position(uint64_t channel) {
    const voice_t* voice = find(channel);
    if (voice == nullptr) {
        return 0.0;
    }
    return static_cast<double>(voice->position.load(std::memory_order_relaxed)) /
           voice->data->sample_rate;
}

FBE_AUDIO_API bool fbe_channel_seek(uint64_t channel, double seconds) {
    voice_t* voice = channels().voices.find(channel);
    const std::int64_t frame = voice != nullptr ? voice->frame_at(seconds) : 0;
    if (!std::isfinite(seconds) || seconds < 0.0 || frame < 0) {
        return fbe::audio::fail(
            std::format("a seek is 0 seconds to its sound's length, not {}", seconds));
    }
    voice = find(channel);
    if (voice != nullptr) {
        voice->position.store(static_cast<std::uint64_t>(frame), std::memory_order_relaxed);
        voice->seek_to.store(frame, std::memory_order_release);
    }
    return true;
}

FBE_AUDIO_API bool fbe_channel_fade(uint64_t channel, float level, double seconds) {
    if (!fbe::audio::check_volume(level) || !check_fade(seconds)) {
        return false;
    }
    voice_t* voice = find(channel);
    if (voice != nullptr) {
        voice->fading_out.store(false, std::memory_order_release);
        ma_sound_set_fade_in_pcm_frames(&voice->sound, -1.0F, level, fade_frames(seconds));
    }
    return true;
}

FBE_AUDIO_API bool fbe_channel_fade_out(uint64_t channel, double seconds) {
    if (!check_fade(seconds)) {
        return false;
    }
    voice_t* voice = find(channel);
    if (voice == nullptr) {
        return true;
    }
    if (seconds == 0.0) {
        channels().voices.remove(channel);
        return true;
    }
    ma_sound_set_fade_in_pcm_frames(&voice->sound, -1.0F, 0.0F, fade_frames(seconds));
    voice->fading_out.store(true, std::memory_order_release);
    return true;
}

FBE_AUDIO_API float fbe_channel_fade_level(uint64_t channel) {
    const voice_t* voice = find(channel);
    return voice != nullptr ? ma_sound_get_current_fade_volume(&voice->sound) : 0.0F;
}

FBE_AUDIO_API bool fbe_channel_set_bus(uint64_t channel, uint64_t bus) {
    std::shared_ptr<fbe::audio::bus_t> into = fbe::audio::find_bus(bus);
    if (bus != 0 && into == nullptr) {
        return fbe::audio::fail("the bus was freed");
    }
    voice_t* voice = find(channel);
    if (voice != nullptr) {
        ma_node_attach_output_bus(&voice->sound, 0, fbe::audio::bus_input(into.get()), 0);
        voice->bus = std::move(into);
    }
    return true;
}

FBE_AUDIO_API int32_t fbe_channel_count(void) {
    channels().voices.remove_if([](const voice_t& voice) { return voice.ended(); });
    return static_cast<int32_t>(channels().voices.count());
}

} // extern "C"
