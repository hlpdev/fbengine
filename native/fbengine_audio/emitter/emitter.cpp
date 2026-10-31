// Emitters: a place, a velocity, and a falloff, which each channel played from one is given as it
// starts and again as the emitter changes. Each keeps its channels' handles, and lets go of those
// that have finished as it goes over them.

#include "audio/mixer.hpp"
#include "audio/pool.hpp"
#include "channel/voices.hpp"
#include "emitter/emitters.hpp"
#include "fbengine_audio.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
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
    std::vector<std::uint64_t> channels;
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
    static auto* made = new fbe::audio::pool_t<emitter_t>();
    return *made;
}

/// Runs `change` on each of `emitter`'s channels that's playing, letting go of those that aren't.
template <typename Change>
void each_channel(emitter_t& emitter, Change change) {
    std::erase_if(emitter.channels, [&](std::uint64_t channel) {
        ma_sound* sound = fbe::audio::channel_sound(channel);
        if (sound == nullptr) {
            return true;
        }
        change(*sound);
        return false;
    });
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

void place(const emitter_t& emitter, ma_sound& sound) {
    ma_sound_set_position(&sound, emitter.position[0], emitter.position[1], emitter.position[2]);
    ma_sound_set_velocity(&sound, emitter.velocity[0], emitter.velocity[1], emitter.velocity[2]);
    ma_sound_set_attenuation_model(&sound, models.at(static_cast<std::size_t>(emitter.falloff)));
    ma_sound_set_min_distance(&sound, emitter.nearest);
    ma_sound_set_max_distance(&sound, emitter.farthest);
    ma_sound_set_rolloff(&sound, emitter.rolloff);
    ma_sound_set_doppler_factor(&sound, emitter.doppler);
}

void join(emitter_t& emitter, std::uint64_t channel) {
    emitter.channels.push_back(channel);
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
    each_channel(*found, [](ma_sound& sound) { ma_sound_set_velocity(&sound, 0.0F, 0.0F, 0.0F); });
    emitters().remove(emitter);
}

FBE_AUDIO_API void fbe_emitter_set_position(uint64_t emitter, float x, float y, float z) {
    emitter_t* found = emitters().find(emitter);
    if (found == nullptr) {
        return;
    }
    found->position = { x, y, z };
    each_channel(*found, [&](ma_sound& sound) { ma_sound_set_position(&sound, x, y, z); });
}

FBE_AUDIO_API void fbe_emitter_set_velocity(uint64_t emitter, float x, float y, float z) {
    emitter_t* found = emitters().find(emitter);
    if (found == nullptr) {
        return;
    }
    found->velocity = { x, y, z };
    each_channel(*found, [&](ma_sound& sound) { ma_sound_set_velocity(&sound, x, y, z); });
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
        each_channel(*found, [&](ma_sound& sound) { ma_sound_set_doppler_factor(&sound, factor); });
    }
    return true;
}

FBE_AUDIO_API float fbe_emitter_doppler(uint64_t emitter) {
    const emitter_t* found = emitters().find(emitter);
    return found != nullptr ? found->doppler : 0.0F;
}

} // extern "C"
