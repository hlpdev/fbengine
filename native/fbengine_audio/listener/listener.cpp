// The listener: miniaudio's first listener, whose settings are kept here while the audio's stopped
// and given to the mix as it starts. Its place and direction miniaudio takes from any thread; which
// way is up, which it keeps as plain floats, waits for the mixing thread. Its velocity and the
// speed of sound are the emitters' Doppler effect, which fbe works out.

#include "listener/listener.hpp"

#include "audio/mixer.hpp"
#include "emitter/emitters.hpp"
#include "fbengine_audio.h"

#include <array>
#include <cmath>
#include <format>
#include <mutex>
#include <new>

namespace {

/// Meters a second, as miniaudio has it.
constexpr float meters_a_second = 343.3F;

std::array<float, 3> position = { 0.0F, 0.0F, 0.0F };
std::array<float, 3> forward = { 0.0F, 0.0F, -1.0F };
std::array<float, 3> velocity = { 0.0F, 0.0F, 0.0F };
float speed = meters_a_second;

/// Which way is up for the listener, the main thread's to set and the mixing thread's to give
/// miniaudio, under the lock.
struct up_t {
    std::mutex lock;
    std::array<float, 3> up = { 0.0F, 1.0F, 0.0F };
    bool changed = false;
};

/// Made once and never let go, as the mixing thread can run while the C++ runtime tears down.
up_t& up() {
    static auto* made = new up_t();
    return *made;
}

} // namespace

namespace fbe::audio {

void start_listener(ma_engine* mix) {
    ma_engine_listener_set_position(mix, 0, position[0], position[1], position[2]);
    ma_engine_listener_set_direction(mix, 0, forward[0], forward[1], forward[2]);
    const std::scoped_lock lock(up().lock);
    ma_engine_listener_set_world_up(mix, 0, up().up[0], up().up[1], up().up[2]);
    up().changed = false;
}

void update_listener(ma_engine* mix) {
    const std::scoped_lock lock(up().lock);
    if (up().changed) {
        ma_engine_listener_set_world_up(mix, 0, up().up[0], up().up[1], up().up[2]);
        up().changed = false;
    }
}

std::array<float, 3> listener_position() {
    return position;
}

std::array<float, 3> listener_velocity() {
    return velocity;
}

float speed_of_sound() {
    return speed;
}

} // namespace fbe::audio

extern "C" {

FBE_AUDIO_API void fbe_listener_set_position(float x, float y, float z) {
    position = { x, y, z };
    if (ma_engine* mix = fbe::audio::engine(); mix != nullptr) {
        ma_engine_listener_set_position(mix, 0, x, y, z);
    }
    fbe::audio::pitch_every_emitter();
}

FBE_AUDIO_API void fbe_listener_set_direction(float forward_x, float forward_y, float forward_z,
                                              float up_x, float up_y, float up_z) {
    forward = { forward_x, forward_y, forward_z };
    if (ma_engine* mix = fbe::audio::engine(); mix != nullptr) {
        ma_engine_listener_set_direction(mix, 0, forward_x, forward_y, forward_z);
    }
    const std::scoped_lock lock(up().lock);
    up().up = { up_x, up_y, up_z };
    up().changed = true;
}

FBE_AUDIO_API void fbe_listener_set_velocity(float x, float y, float z) {
    velocity = { x, y, z };
    fbe::audio::pitch_every_emitter();
}

FBE_AUDIO_API bool fbe_listener_set_speed_of_sound(float units_per_second) try {
    if (!std::isfinite(units_per_second) || units_per_second <= 0.0F) {
        return fbe::audio::fail(
            std::format("sound goes more than 0 units a second, not {}", units_per_second));
    }
    speed = units_per_second;
    fbe::audio::pitch_every_emitter();
    return true;
} catch (const std::bad_alloc&) {
    return fbe::audio::out_of_memory();
}

FBE_AUDIO_API float fbe_listener_speed_of_sound(void) {
    return speed;
}

} // extern "C"
