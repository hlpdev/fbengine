// Channels: each a miniaudio sound in the mix, over a decoded sound's frames or a decoder of a
// streamed sound's file, then a node of fbe's that gives each side its gain, its volume and pan.
// One that has ended is let go the next time it's asked about or another plays, so finished
// channels never pile up and their handles find nothing. Around each mix, on the thread that mixes,
// before_mix does the seeks asked for and after_mix keeps where each channel is, since miniaudio's
// own seek and cursor leave out what it has read ahead.

#include "audio/mixer.hpp"
#include "audio/pool.hpp"
#include "channel/voices.hpp"
#include "fbengine_audio.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <new>
#include <numbers>
#include <utility>
#include <vector>

namespace {

using fbe::audio::highest_pitch;
using fbe::audio::lowest_pitch;
constexpr std::int64_t no_seek = -1;

/// fbe_channel_set_pan_law's laws.
constexpr std::int32_t balance_law = 0;
constexpr std::int32_t constant_power_law = 1;
constexpr std::int32_t linear_law = 2;

/// How a mono channel's pan is heard in a stereo mix; the main thread's.
std::int32_t pan_law = balance_law;

/// Where a channel's going quiet is, which its gains ramp to silence for before it pauses or goes,
/// so neither clicks. The main thread starts it and the mixing thread ends it, each under the live
/// lock.
enum class quieting_t : std::uint8_t {
    /// Playing, or paused before it started.
    NONE,
    /// Ramping to silence, to pause.
    PAUSING,
    /// Silent, its sound stopped by the mixing thread, to resume.
    PAUSED,
    /// Ramping to silence, stopped: its handle finds nothing, and it goes once it's silent.
    STOPPING,
    /// Silent and stopped, for the main thread to let go.
    QUIET,
};

/// Two floats as one 64-bit value, so the main thread sets both at once and the mixing thread
/// reads both at once.
std::uint64_t pack(float left, float right) {
    return std::bit_cast<std::uint32_t>(left) |
           (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(right)) << 32U);
}

float unpack_left(std::uint64_t both) {
    return std::bit_cast<float>(static_cast<std::uint32_t>(both));
}

float unpack_right(std::uint64_t both) {
    return std::bit_cast<float>(static_cast<std::uint32_t>(both >> 32U));
}

/// How long a change of a channel's gains is ramped over, however many mixes that takes.
constexpr double ramp_seconds = 0.005;
/// A sound's stop time when nothing's to stop it, as miniaudio makes one.
constexpr ma_uint64 never = ~ma_uint64{ 0 };

/// A channel's gains: a node after its sound, in the mix's channels, that multiplies each side of
/// a stereo mix by its own gain, the channel's volume and pan together, or every channel of
/// another mix by its volume. The main thread sets the two gains as one value, so the mixing
/// thread never hears a pan half changed; a change is ramped over the next 5 ms of the mix, 240
/// frames at 48000 Hz, carried across as many stretches as that takes, so it doesn't click however
/// short each is. miniaudio's own volume and pan on the sound stay 1 and 0.
struct gains_t {
    /// First, as miniaudio's nodes' are.
    ma_node_base base;
    /// The main thread's: the gains it's to have, left and right.
    std::atomic<std::uint64_t> target = pack(1.0F, 1.0F);
    /// The mixing thread's: the gains it has, those it's ramping to, how far a frame moves each,
    /// and the frames of the ramp left; and a ramp's frames, at the mix's rate.
    float left = 1.0F;
    float right = 1.0F;
    float to_left = 1.0F;
    float to_right = 1.0F;
    float left_step = 0.0F;
    float right_step = 0.0F;
    ma_uint32 ramp_left = 0;
    ma_uint32 ramp_frames = 1;
    ma_uint32 channels = 0;
    /// Whether it's been mixed since its sound first started, before which nothing of it has been
    /// heard, so its first mix takes its gains at once, as a play and then a volume set make them:
    /// the mixing thread sets it, and a paused play's first resume clears it, since miniaudio
    /// mixes the node while its sound is stopped too.
    std::atomic<bool> mixed = false;
    /// The mixing thread's, under the live lock: whether a pause silences it, whatever its gains.
    std::atomic<bool> silenced = false;
};

// miniaudio's signature for a node's processing.
// NOLINTBEGIN(misc-const-correctness, readability-non-const-parameter)
void process_gains(ma_node* node, const float** in, ma_uint32* /*in_count*/, float** out,
                   ma_uint32* out_count) {
    // NOLINTEND(misc-const-correctness, readability-non-const-parameter)
    auto* gains = static_cast<gains_t*>(node);
    const std::uint64_t target = gains->target.load(std::memory_order_acquire);
    const bool silenced = gains->silenced.load(std::memory_order_acquire);
    const float left = silenced ? 0.0F : unpack_left(target);
    const float right = silenced ? 0.0F : unpack_right(target);
    // A new pair is ramped to from where the gains are, over a whole ramp.
    if (!gains->mixed.load(std::memory_order_acquire)) {
        gains->left = gains->to_left = left;
        gains->right = gains->to_right = right;
        gains->ramp_left = 0;
        gains->mixed.store(true, std::memory_order_release);
    } else if (left != gains->to_left || right != gains->to_right) {
        const auto steps = static_cast<float>(gains->ramp_frames);
        gains->to_left = left;
        gains->to_right = right;
        gains->left_step = (left - gains->left) / steps;
        gains->right_step = (right - gains->right) / steps;
        gains->ramp_left = gains->ramp_frames;
    }
    const ma_uint32 frames = *out_count;
    const ma_uint32 channels = gains->channels;
    const float* from = in[0];
    float* to = out[0];
    for (ma_uint32 frame = 0; frame < frames; ++frame) {
        if (gains->ramp_left > 0) {
            --gains->ramp_left;
            // Its last frame lands on the pair exactly.
            gains->left = gains->ramp_left == 0 ? gains->to_left : gains->left + gains->left_step;
            gains->right =
                gains->ramp_left == 0 ? gains->to_right : gains->right + gains->right_step;
        }
        const std::size_t at = static_cast<std::size_t>(frame) * channels;
        if (channels == 2) {
            to[at] = from[at] * gains->left;
            to[at + 1] = from[at + 1] * gains->right;
        } else {
            for (ma_uint32 channel = 0; channel < channels; ++channel) {
                to[at + channel] = from[at + channel] * gains->left;
            }
        }
    }
}

ma_node_vtable gains_vtable = {
    .onProcess = process_gains,
    .onGetRequiredInputFrameCount = nullptr,
    .inputBusCount = 1,
    .outputBusCount = 1,
    .flags = 0,
};

/// A channel: its sound's frames, read through a buffer or decoded as they play, into a miniaudio
/// sound in the mix. miniaudio's init functions clear each of its structs first.
struct voice_t {
    /// The main thread's, for the mixing thread: the frame to seek to.
    std::atomic<std::int64_t> seek_to = no_seek;
    /// The mixing thread's, for the main thread: the frame of its sound it has played to.
    std::atomic<std::uint64_t> position = 0;
    std::shared_ptr<const fbe::audio::sound_data_t> data;
    /// The volume and pan it was given, which its pan law makes its gains.
    float volume = 1.0F;
    float pan = 0.0F;
    /// The pitch it was given, and its emitter's Doppler pitch, which it plays at the two of.
    float pitch = 1.0F;
    float doppler = 1.0F;
    /// What it plays into, which it keeps while it plays, or null for the whole mix.
    std::shared_ptr<fbe::audio::bus_t> bus;
    ma_audio_buffer_ref buffer;
    ma_decoder decoder;
    ma_sound sound;
    gains_t gains;
    bool has_buffer = false;
    bool has_decoder = false;
    bool has_sound = false;
    bool has_gains = false;
    bool joined = false;
    bool paused = false;
    /// The main thread's: whether its sound has been started, so something of it may have been
    /// heard.
    bool started = false;
    /// Under the live lock: whether the mixing thread has silenced its gains and set its sound to
    /// stop on the pause's ramp's last frame.
    bool pause_set = false;
    /// Whether it plays from an emitter, which pans it itself.
    bool spatial = false;
    /// Whether its gains go to silence, whatever its volume and pan, as it stops; a pause's are its
    /// gains' silenced.
    bool muted = false;
    /// Where its going quiet is, under the live lock.
    quieting_t quieting = quieting_t::NONE;
    /// The main thread's, for the mixing thread: whether it finishes once its fade is silent.
    std::atomic<bool> fading_out = false;
    /// The main thread's, for the mixing thread: whether its loop was turned off since the last
    /// mix, so what it read ahead past its end, back at its start, doesn't play.
    std::atomic<bool> unlooped = false;
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

/// The channels, those stopped that are still going quiet, and those the mixing thread sees, each
/// joining once it's made and leaving before it goes; made once and never let go, since a program
/// that ends with the audio started has its device's thread mixing as the C++ runtime tears down.
struct channels_t {
    std::mutex live_lock;
    std::vector<voice_t*> live;
    fbe::audio::pool_t<voice_t> voices{ fbe::audio::kind_t::CHANNEL };
    std::vector<std::unique_ptr<voice_t>> stopping;
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
    if (has_gains) {
        ma_node_uninit(&gains.base, nullptr);
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

/// The gains a pan of `pan` gives each side of a stereo mix: a mono channel not from an emitter's,
/// as the pan law has it; any other's as a balance, which takes from the other side only.
std::pair<float, float> sides(const voice_t& voice) {
    const float pan = voice.pan;
    if (pan_law == balance_law || voice.data->channels != 1 || voice.spatial) {
        return { std::min(1.0F - pan, 1.0F), std::min(1.0F + pan, 1.0F) };
    }
    if (pan_law == constant_power_law) {
        const float angle = (pan + 1.0F) * std::numbers::pi_v<float> / 4.0F;
        return { std::max(std::cos(angle), 0.0F), std::max(std::sin(angle), 0.0F) };
    }
    return { (1.0F - pan) / 2.0F, (1.0F + pan) / 2.0F };
}

/// Gives `voice`'s gains its volume and pan, both at once, which a mix that isn't stereo has its
/// volume only of, or silence while it's going quiet.
void apply_pan(voice_t& voice) {
    float left = voice.muted ? 0.0F : voice.volume;
    float right = left;
    if (voice.gains.channels == 2) {
        const auto [left_side, right_side] = sides(voice);
        left *= left_side;
        right *= right_side;
    }
    voice.gains.target.store(pack(left, right), std::memory_order_release);
}

/// Gives `voice`'s sound its pitch times its Doppler pitch, held to the pitches a channel plays at.
void apply_pitch(voice_t& voice) {
    ma_sound_set_pitch(&voice.sound,
                       std::clamp(voice.pitch * voice.doppler, lowest_pitch, highest_pitch));
}

/// Ends `voice`'s loop where it is: what miniaudio read ahead, which a loop turned off near its
/// sound's end has already taken back round to its start, is cut at the end, and what it reads
/// next is the end, so it finishes there rather than playing its sound again. The cache holds the
/// frames up to the cursor, so it starts `cached` frames back, in a loop of `frames`. The mixing
/// thread's.
void end_loop(voice_t& voice) {
    const std::uint64_t frames = voice.data->frames;
    ma_uint64 cursor = 0;
    if (frames == 0 ||
        ma_data_source_get_cursor_in_pcm_frames(voice.source(), &cursor) != MA_SUCCESS) {
        return;
    }
    const std::uint64_t cached = voice.sound.processingCacheFramesRemaining;
    const std::uint64_t start = (cursor + frames - (cached % frames)) % frames;
    const std::uint64_t kept = std::min(cached, frames - start);
    if (kept < cached) {
        voice.sound.processingCacheFramesRemaining = static_cast<ma_uint32>(kept);
        ma_data_source_seek_to_pcm_frame(voice.source(), frames);
    }
}

/// Lets go of the stopped channels that have gone quiet, or whose sounds ended as they did.
void let_quiet_go() {
    channels_t& all = channels();
    std::vector<std::unique_ptr<voice_t>> quiet;
    // With no room to take them out, they wait for the next.
    try {
        quiet.reserve(all.stopping.size());
    } catch (const std::bad_alloc&) {
        return;
    }
    {
        const std::scoped_lock lock(all.live_lock);
        std::erase_if(all.stopping, [&](std::unique_ptr<voice_t>& voice) {
            if (voice->quieting != quieting_t::QUIET && !voice->ended()) {
                return false;
            }
            quiet.push_back(std::move(voice));
            return true;
        });
    }
    // They go outside the lock, which their leaving the mixing thread's list takes.
}

/// Stops the channel `handle` is for: its handle finds nothing from now, and it goes once it has
/// ramped to silence, or now if it's silent already, paused or ended.
void stop(std::uint64_t handle) {
    // Room for it among the stopping first, which can run out of memory before it's changed.
    channels().stopping.reserve(channels().stopping.size() + 1);
    std::unique_ptr<voice_t> stopped = channels().voices.remove(handle);
    if (stopped == nullptr || stopped->ended()) {
        return;
    }
    stopped->muted = true;
    apply_pan(*stopped);
    const std::scoped_lock lock(channels().live_lock);
    if (stopped->paused && stopped->quieting != quieting_t::PAUSING) {
        // It goes as this returns, after the lock, which its leaving the mixing thread's list
        // takes.
        stopped->quieting = quieting_t::QUIET;
        channels().stopping.push_back(std::move(stopped));
        return;
    }
    stopped->quieting = quieting_t::STOPPING;
    channels().stopping.push_back(std::move(stopped));
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
    let_quiet_go();

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
    voice->volume = how.volume;
    voice->pan = how.pan;
    voice->spatial = how.emitter != nullptr;

    // Its gains first, at what they start at, into its bus, then its sound into them.
    voice->gains.channels = ma_engine_get_channels(mix);
    apply_pan(*voice);
    const std::uint64_t start = voice->gains.target.load(std::memory_order_relaxed);
    voice->gains.left = voice->gains.to_left = unpack_left(start);
    voice->gains.right = voice->gains.to_right = unpack_right(start);
    voice->gains.ramp_frames = std::max<ma_uint32>(
        1, static_cast<ma_uint32>(std::lround(ramp_seconds * ma_engine_get_sample_rate(mix))));
    ma_node_config gains_config = ma_node_config_init();
    gains_config.vtable = &gains_vtable;
    gains_config.pInputChannels = &voice->gains.channels;
    gains_config.pOutputChannels = &voice->gains.channels;
    result =
        ma_node_init(ma_engine_get_node_graph(mix), &gains_config, nullptr, &voice->gains.base);
    if (result != MA_SUCCESS) {
        fail_ma("couldn't play the sound", result);
        return 0;
    }
    voice->has_gains = true;
    ma_node_attach_output_bus(&voice->gains.base, 0, bus_input(voice->bus.get()), 0);

    // Only a channel played from an emitter is in the world.
    auto flags = static_cast<ma_uint32>(MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT);
    if (how.emitter == nullptr) {
        flags |= static_cast<ma_uint32>(MA_SOUND_FLAG_NO_SPATIALIZATION);
    }
    result = ma_sound_init_from_data_source(mix, voice->source(), flags, nullptr, &voice->sound);
    if (result != MA_SUCCESS) {
        fail_ma("couldn't play the sound", result);
        return 0;
    }
    voice->has_sound = true;
    ma_node_attach_output_bus(&voice->sound, 0, &voice->gains.base, 0);
    ma_sound_set_looping(&voice->sound, how.loop ? MA_TRUE : MA_FALSE);
    voice->pitch = how.pitch;
    if (how.emitter != nullptr) {
        voice->doppler = place(*how.emitter, voice->sound);
    }
    apply_pitch(*voice);
    if (how.fade_in > 0.0) {
        ma_sound_set_fade_in_pcm_frames(&voice->sound, 0.0F, 1.0F, fade_frames(how.fade_in));
    }
    {
        const std::scoped_lock lock(channels().live_lock);
        channels().live.push_back(voice.get());
        voice->joined = true;
    }
    voice->paused = how.paused;
    voice->started = !how.paused;
    if (!how.paused) {
        result = ma_sound_start(&voice->sound);
        if (result != MA_SUCCESS) {
            fail_ma("couldn't play the sound", result);
            return 0;
        }
    }
    return channels().voices.add(std::move(voice));
}

ma_sound* channel_sound(std::uint64_t handle) {
    voice_t* voice = find(handle);
    return voice != nullptr ? &voice->sound : nullptr;
}

bool set_doppler(std::uint64_t handle, float doppler) {
    voice_t* voice = find(handle);
    if (voice == nullptr) {
        return false;
    }
    voice->doppler = doppler;
    apply_pitch(*voice);
    return true;
}

void end_channels() {
    channels().voices.clear();
    std::vector<std::unique_ptr<voice_t>> stopping;
    {
        const std::scoped_lock lock(channels().live_lock);
        stopping.swap(channels().stopping);
    }
}

void before_mix() {
    channels_t& all = channels();
    const std::scoped_lock lock(all.live_lock);
    for (voice_t* voice : all.live) {
        // A pause's ramp starts with this mix, and its sound stops on the ramp's last frame, so
        // what's read of it is what's heard.
        if (voice->quieting == quieting_t::PAUSING && !voice->pause_set) {
            voice->gains.silenced.store(true, std::memory_order_release);
            ma_sound_set_stop_time_in_pcm_frames(
                &voice->sound,
                ma_engine_get_time_in_pcm_frames(engine()) + voice->gains.ramp_frames);
            voice->pause_set = true;
        }
        if (voice->unlooped.exchange(false, std::memory_order_acq_rel)) {
            end_loop(*voice);
        }
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
        // Gone quiet, a pausing channel's sound stops, and a stopped one's is ready to go.
        const bool silent = voice->gains.left == 0.0F && voice->gains.right == 0.0F;
        if (silent && voice->quieting == quieting_t::PAUSING) {
            ma_sound_stop(&voice->sound);
            voice->quieting = quieting_t::PAUSED;
        } else if (silent && voice->quieting == quieting_t::STOPPING) {
            ma_sound_stop(&voice->sound);
            voice->quieting = quieting_t::QUIET;
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

FBE_AUDIO_API void fbe_channel_stop(uint64_t channel) try {
    stop(channel);
} catch (const std::bad_alloc&) {
    // With no room to ramp it out, it goes at once.
    channels().voices.remove(channel);
    fbe::audio::out_of_memory();
}

// A pause before anything of it is heard stops its sound at once. Otherwise it ramps to silence
// from the next mix, and its sound stops on the ramp's last frame, which the mixing thread sets.
FBE_AUDIO_API void fbe_channel_pause(uint64_t channel) {
    voice_t* voice = find(channel);
    if (voice == nullptr || voice->paused) {
        return;
    }
    voice->paused = true;
    const std::scoped_lock lock(channels().live_lock);
    if (!voice->started || !voice->gains.mixed.load(std::memory_order_acquire)) {
        ma_sound_stop(&voice->sound);
        // Unheard, it's as if it never started: its first resume takes its gains at once.
        voice->started = false;
        voice->quieting = quieting_t::PAUSED;
        return;
    }
    voice->quieting = quieting_t::PAUSING;
}

// A resume ramps back from silence: a pause still ramping only turns round, its stop taken back;
// one the mixing thread finished starts its sound again; one never heard starts at its gains.
FBE_AUDIO_API void fbe_channel_resume(uint64_t channel) {
    voice_t* voice = find(channel);
    if (voice == nullptr || !voice->paused) {
        return;
    }
    {
        const std::scoped_lock lock(channels().live_lock);
        ma_sound_set_stop_time_in_pcm_frames(&voice->sound, never);
        voice->gains.silenced.store(false, std::memory_order_release);
        voice->pause_set = false;
        if (voice->quieting != quieting_t::PAUSING) {
            if (!voice->started) {
                voice->gains.mixed.store(false, std::memory_order_release);
            }
            ma_sound_start(&voice->sound);
            voice->started = true;
        }
        voice->quieting = quieting_t::NONE;
    }
    voice->paused = false;
}

FBE_AUDIO_API bool fbe_channel_set_volume(uint64_t channel, float volume) try {
    if (!fbe::audio::check_volume(volume)) {
        return false;
    }
    voice_t* voice = find(channel);
    if (voice != nullptr) {
        voice->volume = volume;
        apply_pan(*voice);
    }
    return true;
} catch (const std::bad_alloc&) {
    return fbe::audio::out_of_memory();
}

FBE_AUDIO_API float fbe_channel_volume(uint64_t channel) {
    const voice_t* voice = find(channel);
    return voice != nullptr ? voice->volume : 0.0F;
}

FBE_AUDIO_API bool fbe_channel_set_pan(uint64_t channel, float pan) try {
    if (!fbe::audio::check_pan(pan)) {
        return false;
    }
    voice_t* voice = find(channel);
    if (voice != nullptr) {
        voice->pan = pan;
        apply_pan(*voice);
    }
    return true;
} catch (const std::bad_alloc&) {
    return fbe::audio::out_of_memory();
}

FBE_AUDIO_API float fbe_channel_pan(uint64_t channel) {
    const voice_t* voice = find(channel);
    return voice != nullptr ? voice->pan : 0.0F;
}

FBE_AUDIO_API bool fbe_channel_set_pitch(uint64_t channel, float pitch) try {
    if (!fbe::audio::check_pitch(pitch)) {
        return false;
    }
    voice_t* voice = find(channel);
    if (voice != nullptr) {
        voice->pitch = pitch;
        apply_pitch(*voice);
    }
    return true;
} catch (const std::bad_alloc&) {
    return fbe::audio::out_of_memory();
}

FBE_AUDIO_API float fbe_channel_pitch(uint64_t channel) {
    const voice_t* voice = find(channel);
    return voice != nullptr ? voice->pitch : 0.0F;
}

FBE_AUDIO_API void fbe_channel_set_loop(uint64_t channel, bool loop) {
    voice_t* voice = find(channel);
    if (voice == nullptr) {
        return;
    }
    const bool was = ma_sound_is_looping(&voice->sound) != MA_FALSE;
    ma_sound_set_looping(&voice->sound, loop ? MA_TRUE : MA_FALSE);
    voice->unlooped.store(was && !loop, std::memory_order_release);
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

FBE_AUDIO_API bool fbe_channel_seek(uint64_t channel, double seconds) try {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return fbe::audio::fail(
            std::format("a seek is 0 seconds to its sound's length, not {}", seconds));
    }
    // A finished channel's seek is nothing, however far, as its other calls are.
    voice_t* voice = find(channel);
    if (voice == nullptr) {
        return true;
    }
    const std::int64_t frame = voice->frame_at(seconds);
    if (frame < 0) {
        return fbe::audio::fail(
            std::format("a seek is 0 seconds to its sound's length, not {}", seconds));
    }
    voice->position.store(static_cast<std::uint64_t>(frame), std::memory_order_relaxed);
    voice->seek_to.store(frame, std::memory_order_release);
    return true;
} catch (const std::bad_alloc&) {
    return fbe::audio::out_of_memory();
}

FBE_AUDIO_API bool fbe_channel_fade(uint64_t channel, float level, double seconds) try {
    if (!fbe::audio::check_volume(level) || !check_fade(seconds)) {
        return false;
    }
    voice_t* voice = find(channel);
    if (voice != nullptr) {
        voice->fading_out.store(false, std::memory_order_release);
        ma_sound_set_fade_in_pcm_frames(&voice->sound, -1.0F, level, fade_frames(seconds));
    }
    return true;
} catch (const std::bad_alloc&) {
    return fbe::audio::out_of_memory();
}

FBE_AUDIO_API bool fbe_channel_fade_out(uint64_t channel, double seconds) try {
    if (!check_fade(seconds)) {
        return false;
    }
    voice_t* voice = find(channel);
    if (voice == nullptr) {
        return true;
    }
    if (seconds == 0.0) {
        stop(channel);
        return true;
    }
    ma_sound_set_fade_in_pcm_frames(&voice->sound, -1.0F, 0.0F, fade_frames(seconds));
    voice->fading_out.store(true, std::memory_order_release);
    return true;
} catch (const std::bad_alloc&) {
    return fbe::audio::out_of_memory();
}

FBE_AUDIO_API float fbe_channel_fade_level(uint64_t channel) {
    const voice_t* voice = find(channel);
    return voice != nullptr ? ma_sound_get_current_fade_volume(&voice->sound) : 0.0F;
}

FBE_AUDIO_API bool fbe_channel_set_bus(uint64_t channel, uint64_t bus) try {
    std::shared_ptr<fbe::audio::bus_t> into = fbe::audio::find_bus(bus);
    if (bus != 0 && into == nullptr) {
        return fbe::audio::fail("the bus was freed");
    }
    voice_t* voice = find(channel);
    if (voice != nullptr) {
        ma_node_attach_output_bus(&voice->gains.base, 0, fbe::audio::bus_input(into.get()), 0);
        voice->bus = std::move(into);
    }
    return true;
} catch (const std::bad_alloc&) {
    return fbe::audio::out_of_memory();
}

FBE_AUDIO_API bool fbe_channel_set_pan_law(int32_t law) try {
    if (law < balance_law || law > linear_law) {
        return fbe::audio::fail(std::format("there's no pan law {}", law));
    }
    pan_law = law;
    channels().voices.remove_if([](const voice_t& voice) { return voice.ended(); });
    channels().voices.each([](voice_t& voice) { apply_pan(voice); });
    return true;
} catch (const std::bad_alloc&) {
    return fbe::audio::out_of_memory();
}

FBE_AUDIO_API int32_t fbe_channel_pan_law(void) {
    return pan_law;
}

FBE_AUDIO_API int32_t fbe_channel_count(void) {
    channels().voices.remove_if([](const voice_t& voice) { return voice.ended(); });
    let_quiet_go();
    return static_cast<int32_t>(channels().voices.count());
}

} // extern "C"
