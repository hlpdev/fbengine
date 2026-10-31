// Simulate's gamepads and joysticks: SDL's virtual devices, which SDL reads as it reads real
// ones, so their input reaches the game the same way.

#include "core/engine.hpp"
#include "fbengine.h"
#include "input/input.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <SDL3/SDL.h>
#include <utility>
#include <vector>

namespace {

// A virtual device: the joystick that sets its state, and how hard SDL last told its motors to
// rumble: low and high frequency, then the left and right triggers'.
struct device_t {
    SDL_JoystickID id = 0;
    SDL_Joystick* joystick = nullptr;
    bool gamepad = false;
    std::array<float, 4> rumble{};
};

// Each on the heap, where SDL's callbacks find it.
std::vector<std::unique_ptr<device_t>> devices;

auto find(std::uint64_t device) {
    return std::ranges::find_if(devices, [device](const auto& each) { return each->id == device; });
}

device_t* device_of(std::uint64_t device) {
    const auto found = find(device);
    return found != devices.end() ? found->get() : nullptr;
}

float strength(Uint16 value) {
    return static_cast<float>(value) / 65535.0F;
}

bool SDLCALL rumble(void* userdata, Uint16 low, Uint16 high) {
    auto* device = static_cast<device_t*>(userdata);
    device->rumble[0] = strength(low);
    device->rumble[1] = strength(high);
    return true;
}

bool SDLCALL rumble_triggers(void* userdata, Uint16 left, Uint16 right) {
    auto* device = static_cast<device_t*>(userdata);
    device->rumble[2] = strength(left);
    device->rumble[3] = strength(right);
    return true;
}

/// Attaches a device as `description` says, and opens it to set its state; its handle, or 0.
std::uint64_t attach(SDL_VirtualJoystickDesc& description, bool gamepad) {
    if (!fbe::start_gamepads()) {
        return 0;
    }
    auto device = std::make_unique<device_t>();
    device->gamepad = gamepad;
    description.userdata = device.get();
    description.Rumble = rumble;
    description.RumbleTriggers = rumble_triggers;
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&description);
    if (id == 0) {
        fbe::fail_sdl("couldn't connect the virtual device");
        return 0;
    }
    device->joystick = SDL_OpenJoystick(id);
    if (device->joystick == nullptr) {
        fbe::fail_sdl("couldn't open the virtual device");
        SDL_DetachVirtualJoystick(id);
        return 0;
    }
    device->id = id;
    devices.push_back(std::move(device));
    return id;
}

Sint16 axis_value(float value) {
    return static_cast<Sint16>(std::lround(std::clamp(value, -1.0F, 1.0F) * 32767.0F));
}

/// A trigger's value, 0 to 1, as a virtual gamepad's axis, which SDL maps from its whole range.
Sint16 trigger_value(float value) {
    const float scaled = (std::clamp(value, 0.0F, 1.0F) * 65535.0F) - 32768.0F;
    return static_cast<Sint16>(std::lround(scaled));
}

} // namespace

extern "C" {

FBE_API uint64_t fbe_input_simulate_gamepad(const char* name) {
    SDL_VirtualJoystickDesc description{};
    SDL_INIT_INTERFACE(&description);
    description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    description.naxes = SDL_GAMEPAD_AXIS_COUNT;
    description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    description.name = name;
    return attach(description, true);
}

FBE_API uint64_t fbe_input_simulate_joystick(const char* name, int32_t type, int32_t axes,
                                             int32_t buttons, int32_t hats) {
    constexpr std::int32_t most = 255;
    if (type < SDL_JOYSTICK_TYPE_UNKNOWN || type >= SDL_JOYSTICK_TYPE_COUNT || axes < 0 ||
        axes > most || buttons < 0 || buttons > most || hats < 0 || hats > most) {
        fbe::fail(std::format("a virtual joystick has 0 to {} axes, buttons, and hats, not {}, {}, "
                              "and {}, of a JoystickType, not {}",
                              most, axes, buttons, hats, type));
        return 0;
    }
    SDL_VirtualJoystickDesc description{};
    SDL_INIT_INTERFACE(&description);
    description.type = static_cast<Uint16>(type);
    description.naxes = static_cast<Uint16>(axes);
    description.nbuttons = static_cast<Uint16>(buttons);
    description.nhats = static_cast<Uint16>(hats);
    description.name = name;
    return attach(description, false);
}

FBE_API bool fbe_input_simulate_unplug(uint64_t device) {
    const device_t* found = device_of(device);
    if (found == nullptr) {
        return fbe::fail("there's no such virtual device");
    }
    SDL_CloseJoystick(found->joystick);
    SDL_DetachVirtualJoystick(static_cast<SDL_JoystickID>(device));
    devices.erase(find(device));
    return true;
}

FBE_API bool fbe_input_simulate_device_button(uint64_t device, int32_t button, bool down) {
    const device_t* found = device_of(device);
    if (found == nullptr) {
        return fbe::fail("there's no such virtual device");
    }
    return SDL_SetJoystickVirtualButton(found->joystick, button, down)
               ? true
               : fbe::fail_sdl(std::format("couldn't set button {}", button));
}

FBE_API bool fbe_input_simulate_device_axis(uint64_t device, int32_t axis, float value) {
    const device_t* found = device_of(device);
    if (found == nullptr) {
        return fbe::fail("there's no such virtual device");
    }
    const bool trigger = found->gamepad && (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                                            axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    const Sint16 raw = trigger ? trigger_value(value) : axis_value(value);
    return SDL_SetJoystickVirtualAxis(found->joystick, axis, raw)
               ? true
               : fbe::fail_sdl(std::format("couldn't set axis {}", axis));
}

FBE_API bool fbe_input_simulate_device_hat(uint64_t device, int32_t hat, int32_t value) {
    const device_t* found = device_of(device);
    if (found == nullptr) {
        return fbe::fail("there's no such virtual device");
    }
    constexpr std::int32_t every_way = SDL_HAT_UP | SDL_HAT_RIGHT | SDL_HAT_DOWN | SDL_HAT_LEFT;
    if (value < 0 || value > every_way) {
        return fbe::fail(std::format("{} isn't a hat's direction", value));
    }
    return SDL_SetJoystickVirtualHat(found->joystick, hat, static_cast<Uint8>(value))
               ? true
               : fbe::fail_sdl(std::format("couldn't set hat {}", hat));
}

FBE_API float fbe_input_simulate_device_rumble(uint64_t device, int32_t motor) {
    const device_t* found = device_of(device);
    if (found == nullptr || motor < 0 || std::cmp_greater_equal(motor, found->rumble.size())) {
        return 0.0F;
    }
    return found->rumble[static_cast<std::size_t>(motor)];
}

} // extern "C"
