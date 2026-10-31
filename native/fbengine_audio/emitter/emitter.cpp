// Emitters: a place, a velocity, and a falloff, which each channel played from one is given as it
// starts and again as the emitter changes. Each keeps its channels' handles, and lets go of those
// that have finished as it goes over them. The Doppler effect is worked out here, not by
// miniaudio, whose pitch goes infinite at the speed of sound, and is held to 1/16 to 16.

#include "audio/mixer.hpp"
#include "audio/pool.hpp"
#include "channel/voices.hpp"
#include "emitter/channels.hpp"
#include "emitter/emitters.hpp"
#include "fbengine_audio.h"
#include "listener/listener.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace fbe::audio {

struct emitter_t {
    std::array<float, 3> position{};
    std::array<float, 3> velocity{};
    float nearest = 1.0F;
    float farthest = std::numeric_limits<float>::max();
    float rolloff = 1.0F;
    float doppler = 1.0F;
    std::int32_t falloff = 0;
    /// Its channels, some of which may have finished.
    channel_list_t channels;
};

} // namespace fbe::audio

namespace {

using fbe::audio::emitter_t;

/// fbe_emitter_set_falloff's falloffs, as miniaudio's models.
constexpr std::array<ma_attenuation_model, 3> models = {
    ma_attenuation_model_inverse,
    ma_attenuation_model_linear,
    ma_attenuation_model_exponential,
};

/// The emitters, made once and never let go, so an Emitter an FB program lets go as it ends
/// finds them.
fbe::audio::pool_t<emitter_t>& emitters() {
    static auto* made = new fbe::audio::pool_t<emitter_t>(fbe::audio::kind_t::EMITTER);
    return *made;
}

/// Runs `change` on each of `emitter`'s channels that's playing, letting go of those that aren't.
template <typename Change>
void each_channel(emitter_t& emitter, Change change) {
    emitter.channels.each([&](std::uint64_t channel) {
        ma_sound* sound = fbe::audio::channel_sound(channel);
        if (sound == nullptr) {
            return false;
        }
        change(*sound);
        return true;
    });
}

float dot(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

/// How `emitter`'s and the listener's velocities pitch its channels, as OpenAL works the Doppler
/// effect out, held to 1/16 to 16: at or past the speed of sound toward the listener, the pitch
/// would be infinite, or negative.
float doppler_pitch(const emitter_t& emitter) {
    const std::array<float, 3> listener = fbe::audio::listener_position();
    const std::array<float, 3> toward = {
        listener[0] - emitter.position[0],
        listener[1] - emitter.position[1],
        listener[2] - emitter.position[2],
    };
    const float distance = std::sqrt(dot(toward, toward));
    const float factor = emitter.doppler;
    if (factor == 0.0F || !(distance > 0.0F)) {
        return 1.0F;
    }
    const float speed = fbe::audio::speed_of_sound();
    const float fastest = speed / factor;
    const float hearing =
        std::min(dot(toward, fbe::audio::listener_velocity()) / distance, fastest);
    const float coming = std::min(dot(toward, emitter.velocity) / distance, fastest);
    const float pitch = (speed - (factor * hearing)) / (speed - (factor * coming));
    if (std::isnan(pitch)) {
        return 1.0F;
    }
    return std::clamp(pitch, fbe::audio::lowest_pitch, fbe::audio::highest_pitch);
}

/// Gives each of `emitter`'s channels its Doppler pitch, letting go of those that have finished.
void pitch_channels(emitter_t& emitter) {
    const float pitch = doppler_pitch(emitter);
    emitter.channels.each(
        [&](std::uint64_t channel) { return fbe::audio::set_doppler(channel, pitch); });
}

bool check_positive(float value, const char* what) {
    if (!std::isfinite(value) || value < 0.0F) {
        return fbe::audio::fail(std::format("an emitter's {} is 0 or more, not {}", what, value));
    }
    return true;
}

} // namespace

namespace fbe::audio {

emitter_t* find_emitter(std::uint64_t handle) {
    return emitters().find(handle);
}

float place(const emitter_t& emitter, ma_sound& sound) {
    ma_sound_set_position(&sound, emitter.position[0], emitter.position[1], emitter.position[2]);
    ma_sound_set_attenuation_model(&sound, models.at(static_cast<std::size_t>(emitter.falloff)));
    ma_sound_set_min_distance(&sound, emitter.nearest);
    ma_sound_set_max_distance(&sound, emitter.farthest);
    ma_sound_set_rolloff(&sound, emitter.rolloff);
    // The Doppler effect is fbe's, folded into the channel's pitch.
    ma_sound_set_doppler_factor(&sound, 0.0F);
    return doppler_pitch(emitter);
}

void join(emitter_t& emitter, std::uint64_t channel) {
    emitter.channels.add(channel, [](std::uint64_t playing) {
        return fbe::audio::channel_sound(playing) != nullptr;
    });
}

void pitch_every_emitter() {
    emitters().each([](emitter_t& emitter) { pitch_channels(emitter); });
}

} // namespace fbe::audio

extern "C" {

FBE_AUDIO_API uint64_t fbe_emitter_add(void) {
    return emitters().add(std::make_unique<emitter_t>());
}

FBE_AUDIO_API void fbe_emitter_free(uint64_t emitter) {
    emitter_t* found = emitters().find(emitter);
    if (found == nullptr) {
        return;
    }
    // Its channels stay where it was, at rest.
    found->velocity = {};
    pitch_channels(*found);
    emitters().remove(emitter);
}

FBE_AUDIO_API void fbe_emitter_set_position(uint64_t emitter, float x, float y, float z) {
    emitter_t* found = emitters().find(emitter);
    if (found == nullptr) {
        return;
    }
    found->position = { x, y, z };
    each_channel(*found, [&](ma_sound& sound) { ma_sound_set_position(&sound, x, y, z); });
    pitch_channels(*found);
}

FBE_AUDIO_API void fbe_emitter_place_all(const uint64_t* emitters_placed, const float* positions,
                                         const float* velocities, int32_t count) {
    if (emitters_placed == nullptr || positions == nullptr || velocities == nullptr || count <= 0) {
        return;
    }
    const auto placed = static_cast<std::size_t>(count);
    const std::span<const uint64_t> handles(emitters_placed, placed);
    const std::span<const float> wheres(positions, placed * 3);
    const std::span<const float> moves(velocities, placed * 3);
    for (std::size_t i = 0; i < placed; ++i) {
        emitter_t* found = emitters().find(handles[i]);
        if (found == nullptr) {
            continue;
        }
        const std::array<float, 3> position = {
            wheres[i * 3],
            wheres[(i * 3) + 1],
            wheres[(i * 3) + 2],
        };
        found->position = position;
        found->velocity = { moves[i * 3], moves[(i * 3) + 1], moves[(i * 3) + 2] };
        each_channel(*found, [&](ma_sound& sound) {
            ma_sound_set_position(&sound, position[0], position[1], position[2]);
        });
        pitch_channels(*found);
    }
}

FBE_AUDIO_API void fbe_emitter_set_velocity(uint64_t emitter, float x, float y, float z) {
    emitter_t* found = emitters().find(emitter);
    if (found == nullptr) {
        return;
    }
    found->velocity = { x, y, z };
    pitch_channels(*found);
}

FBE_AUDIO_API bool fbe_emitter_set_range(uint64_t emitter, float nearest, float farthest) {
    if (!std::isfinite(nearest) || std::isnan(farthest) || nearest <= 0.0F || farthest < nearest) {
        return fbe::audio::fail(
            std::format("an emitter's range is from more than 0 to as far or farther, not {} to {}",
                        nearest, farthest));
    }
    emitter_t* found = emitters().find(emitter);
    if (found == nullptr) {
        return true;
    }
    found->nearest = nearest;
    found->farthest = farthest;
    each_channel(*found, [&](ma_sound& sound) {
        ma_sound_set_min_distance(&sound, nearest);
        ma_sound_set_max_distance(&sound, farthest);
    });
    return true;
}

FBE_AUDIO_API float fbe_emitter_nearest(uint64_t emitter) {
    const emitter_t* found = emitters().find(emitter);
    return found != nullptr ? found->nearest : 0.0F;
}

FBE_AUDIO_API float fbe_emitter_farthest(uint64_t emitter) {
    const emitter_t* found = emitters().find(emitter);
    return found != nullptr ? found->farthest : 0.0F;
}

FBE_AUDIO_API bool fbe_emitter_set_rolloff(uint64_t emitter, float rolloff) {
    if (!check_positive(rolloff, "rolloff")) {
        return false;
    }
    emitter_t* found = emitters().find(emitter);
    if (found != nullptr) {
        found->rolloff = rolloff;
        each_channel(*found, [&](ma_sound& sound) { ma_sound_set_rolloff(&sound, rolloff); });
    }
    return true;
}

FBE_AUDIO_API float fbe_emitter_rolloff(uint64_t emitter) {
    const emitter_t* found = emitters().find(emitter);
    return found != nullptr ? found->rolloff : 0.0F;
}

FBE_AUDIO_API bool fbe_emitter_set_falloff(uint64_t emitter, int32_t falloff) {
    if (falloff < 0 || std::cmp_greater_equal(falloff, models.size())) {
        return fbe::audio::fail(std::format("there's no falloff {}", falloff));
    }
    emitter_t* found = emitters().find(emitter);
    if (found != nullptr) {
        found->falloff = falloff;
        const ma_attenuation_model model = models.at(static_cast<std::size_t>(falloff));
        each_channel(*found,
                     [&](ma_sound& sound) { ma_sound_set_attenuation_model(&sound, model); });
    }
    return true;
}

FBE_AUDIO_API int32_t fbe_emitter_falloff(uint64_t emitter) {
    const emitter_t* found = emitters().find(emitter);
    return found != nullptr ? found->falloff : 0;
}

FBE_AUDIO_API bool fbe_emitter_set_doppler(uint64_t emitter, float factor) {
    if (!check_positive(factor, "Doppler factor")) {
        return false;
    }
    emitter_t* found = emitters().find(emitter);
    if (found != nullptr) {
        found->doppler = factor;
        pitch_channels(*found);
    }
    return true;
}

FBE_AUDIO_API float fbe_emitter_doppler(uint64_t emitter) {
    const emitter_t* found = emitters().find(emitter);
    return found != nullptr ? found->doppler : 0.0F;
}

} // extern "C"
