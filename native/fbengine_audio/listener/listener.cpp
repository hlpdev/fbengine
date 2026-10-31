// The listener: miniaudio's first listener, whose settings are kept here while the audio's stopped
// and given to the mix as it starts. Its place, direction, and velocity miniaudio takes from any
// thread; the speed of sound waits for the mixing thread.

#include "listener/listener.hpp"

#include "audio/mixer.hpp"
#include "fbengine_audio.h"

#include <array>
#include <atomic>
#include <cmath>
#include <format>

namespace {

/// Meters a second, as miniaudio has it.
constexpr float meters_a_second = 343.3F;

std::array<float, 3> position = { 0.0F, 0.0F, 0.0F };
std::array<float, 3> forward = { 0.0F, 0.0F, -1.0F };
std::array<float, 3> up = { 0.0F, 1.0F, 0.0F };
std::array<float, 3> velocity = { 0.0F, 0.0F, 0.0F };
std::atomic<float> speed = meters_a_second;
std::atomic<bool> speed_changed = false;

/// Gives the mix where the listener is, which way it faces, and how it moves.
void apply(ma_engine* mix) {
    ma_engine_listener_set_position(mix, 0, position[0], position[1], position[2]);
    ma_engine_listener_set_direction(mix, 0, forward[0], forward[1], forward[2]);
    ma_engine_listener_set_world_up(mix, 0, up[0], up[1], up[2]);
    ma_engine_listener_set_velocity(mix, 0, velocity[0], velocity[1], velocity[2]);
}

} // namespace

namespace fbe::audio {

void start_listener(ma_engine* mix) {
    apply(mix);
    ma_spatializer_listener_set_speed_of_sound(&mix->listeners[0], speed.load());
    speed_changed = false;
}

void update_listener(ma_engine* mix) {
    if (speed_changed.exchange(false)) {
        ma_spatializer_listener_set_speed_of_sound(&mix->listeners[0], speed.load());
    }
}

} // namespace fbe::audio

extern "C" {

FBE_AUDIO_API void fbe_listener_set_position(float x, float y, float z) {
    position = { x, y, z };
    if (ma_engine* mix = fbe::audio::engine(); mix != nullptr) {
        ma_engine_listener_set_position(mix, 0, x, y, z);
    }
}

FBE_AUDIO_API void fbe_listener_set_direction(float forward_x, float forward_y, float forward_z,
                                              float up_x, float up_y, float up_z) {
    forward = { forward_x, forward_y, forward_z };
    up = { up_x, up_y, up_z };
    if (ma_engine* mix = fbe::audio::engine(); mix != nullptr) {
        ma_engine_listener_set_direction(mix, 0, forward_x, forward_y, forward_z);
        ma_engine_listener_set_world_up(mix, 0, up_x, up_y, up_z);
    }
}

FBE_AUDIO_API void fbe_listener_set_velocity(float x, float y, float z) {
    velocity = { x, y, z };
    if (ma_engine* mix = fbe::audio::engine(); mix != nullptr) {
        ma_engine_listener_set_velocity(mix, 0, x, y, z);
    }
}

FBE_AUDIO_API bool fbe_listener_set_speed_of_sound(float units_per_second) {
    if (!std::isfinite(units_per_second) || units_per_second <= 0.0F) {
        return fbe::audio::fail(
            std::format("sound goes more than 0 units a second, not {}", units_per_second));
    }
    speed = units_per_second;
    speed_changed = true;
    return true;
}

FBE_AUDIO_API float fbe_listener_speed_of_sound(void) {
    return speed.load();
}

} // extern "C"
