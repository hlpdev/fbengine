// Gamepads, SDL's mapped ones, as the events fbe_frame_flip reads say: their buttons, their axes
// past the dead zones, rumble, and which connected.

#include "core/engine.hpp"
#include "fbengine.h"
#include "input/input.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <SDL3/SDL.h>
#include <string>
#include <vector>

namespace {

// A gamepad: SDL's, and what the events read say of its buttons and axes.
struct pad_t {
    SDL_JoystickID id = 0;
    SDL_Gamepad* gamepad = nullptr;
    std::array<fbe::button_t, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    std::array<std::int16_t, SDL_GAMEPAD_AXIS_COUNT> axes{};
};

bool started = false;
// In the order they connected.
std::vector<pad_t> pads;
fbe::frame_counts_t counts;
std::vector<SDL_JoystickID> added;
std::vector<SDL_JoystickID> removed;

float stick_zone = 0.2F;
float trigger_zone = 0.1F;

// fbe_input_gamepad_name's, which the next call replaces.
std::string name;

/// The gamepad `pad` names, or null if it isn't connected.
pad_t* pad_of(std::uint64_t pad) {
    const auto found = std::ranges::find(pads, pad, &pad_t::id);
    return found != pads.end() ? &*found : nullptr;
}

bool valid_button(std::int32_t button) {
    return button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT;
}

bool valid_axis(std::int32_t axis) {
    return axis >= 0 && axis < SDL_GAMEPAD_AXIS_COUNT;
}

bool is_trigger(std::int32_t axis) {
    return axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
}

void zero_counts() {
    for (pad_t& pad : pads) {
        for (fbe::button_t& button : pad.buttons) {
            button.zero();
        }
    }
    added.clear();
    removed.clear();
}

/// Zeroes what was counted when this is the first event of a Flip.
void count() {
    if (!counts.start()) {
        zero_counts();
    }
}

/// An axis as SDL gives it, -1 to 1 for a stick and 0 to 1 for a trigger.
float raw(std::int16_t value) {
    return std::max(static_cast<float>(value) / 32767.0F, -1.0F);
}

/// How far past `zone` a value of `length` is, from 0 to 1.
float past(float length, float zone) {
    return length <= zone ? 0.0F : std::min((length - zone) / (1.0F - zone), 1.0F);
}

/// Opens the gamepad, if it's not open already; false if it can't.
bool open(SDL_JoystickID id) {
    if (pad_of(id) != nullptr) {
        return true;
    }
    SDL_Gamepad* gamepad = SDL_OpenGamepad(id);
    if (gamepad == nullptr) {
        return false;
    }
    pad_t& pad = pads.emplace_back();
    pad.id = id;
    pad.gamepad = gamepad;
    for (std::size_t button = 0; button < pad.buttons.size(); ++button) {
        pad.buttons[button].down =
            SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(button));
    }
    for (std::size_t axis = 0; axis < pad.axes.size(); ++axis) {
        pad.axes[axis] = SDL_GetGamepadAxis(gamepad, static_cast<SDL_GamepadAxis>(axis));
    }
    return true;
}

void close(SDL_JoystickID id) {
    const auto found = std::ranges::find(pads, id, &pad_t::id);
    if (found != pads.end()) {
        SDL_CloseGamepad(found->gamepad);
        pads.erase(found);
    }
}

std::uint64_t at(const std::vector<SDL_JoystickID>& ids, std::int32_t index) {
    const bool there =
        counts.current() && index >= 0 && static_cast<std::size_t>(index) < ids.size();
    return there ? ids[static_cast<std::size_t>(index)] : 0;
}

std::int32_t count_of(const std::vector<SDL_JoystickID>& ids) {
    return counts.current() ? static_cast<std::int32_t>(ids.size()) : 0;
}

Uint16 strength(float value) {
    return static_cast<Uint16>(std::lround(std::clamp(value, 0.0F, 1.0F) * 65535.0F));
}

/// A rumble's time, 0 or more seconds, in SDL's milliseconds: at least 1, since SDL's 0 never
/// stops, and at most SDL's most, 65.535 seconds.
Uint32 milliseconds(double seconds) {
    constexpr double most = 65535.0;
    return seconds > 0.0 ? static_cast<Uint32>(std::min(std::ceil(seconds * 1000.0), most)) : 0;
}

/// Whether `seconds` is a rumble's time, saying why not if it isn't.
bool valid_time(double seconds) {
    // Written so a NaN isn't one either.
    if (!(seconds >= 0.0)) {
        return fbe::fail(std::format("a rumble lasts 0 seconds or more, not {}", seconds));
    }
    return true;
}

std::string guid_text;

} // namespace

namespace fbe {

bool start_gamepads() {
    if (started) {
        return true;
    }
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        return fail_sdl("couldn't start the gamepads");
    }
    // The pads and joysticks connected already are opened from the events SDL queues for them as
    // it starts, as a pad connected later is, so each connects once, at the next Flip.
    started = true;
    return true;
}

void gamepad_event(const SDL_Event& event) {
    if (!started) {
        return;
    }
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        if (open(event.gdevice.which)) {
            count();
            added.push_back(event.gdevice.which);
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad_of(event.gdevice.which) != nullptr) {
            count();
            close(event.gdevice.which);
            removed.push_back(event.gdevice.which);
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        pad_t* pad = pad_of(event.gbutton.which);
        if (pad != nullptr && valid_button(event.gbutton.button)) {
            count();
            fbe::button_t& button = pad->buttons[event.gbutton.button];
            if (event.gbutton.down) {
                button.press();
            } else {
                button.release();
            }
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        pad_t* pad = pad_of(event.gaxis.which);
        if (pad != nullptr && valid_axis(event.gaxis.axis)) {
            pad->axes[event.gaxis.axis] = event.gaxis.value;
        }
        break;
    }
    default:
        break;
    }
}

} // namespace fbe

extern "C" {

FBE_API int32_t fbe_input_gamepad_count(void) {
    if (!fbe::start_gamepads()) {
        return -1;
    }
    return static_cast<std::int32_t>(pads.size());
}

FBE_API uint64_t fbe_input_gamepad_at(int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= pads.size()) {
        return 0;
    }
    return pads[static_cast<std::size_t>(index)].id;
}

FBE_API int32_t fbe_input_gamepads_added_count(void) {
    if (!fbe::start_gamepads()) {
        return 0;
    }
    return count_of(added);
}

FBE_API uint64_t fbe_input_gamepad_added_at(int32_t index) {
    return at(added, index);
}

FBE_API int32_t fbe_input_gamepads_removed_count(void) {
    if (!fbe::start_gamepads()) {
        return 0;
    }
    return count_of(removed);
}

FBE_API uint64_t fbe_input_gamepad_removed_at(int32_t index) {
    return at(removed, index);
}

FBE_API bool fbe_input_gamepad_connected(uint64_t pad) {
    return pad_of(pad) != nullptr;
}

FBE_API const char* fbe_input_gamepad_name(uint64_t pad) {
    const pad_t* found = pad_of(pad);
    const char* named = found != nullptr ? SDL_GetGamepadName(found->gamepad) : nullptr;
    name = named != nullptr ? named : "";
    return name.c_str();
}

FBE_API int32_t fbe_input_gamepad_type(uint64_t pad) {
    const pad_t* found = pad_of(pad);
    return found != nullptr ? SDL_GetGamepadType(found->gamepad) : SDL_GAMEPAD_TYPE_UNKNOWN;
}

FBE_API bool fbe_input_gamepad_down(uint64_t pad, int32_t button) {
    const pad_t* found = pad_of(pad);
    return found != nullptr && valid_button(button) &&
           found->buttons[static_cast<std::size_t>(button)].down;
}

FBE_API int32_t fbe_input_gamepad_hits(uint64_t pad, int32_t button) {
    const pad_t* found = pad_of(pad);
    const bool there = found != nullptr && valid_button(button) && counts.current();
    return there ? found->buttons[static_cast<std::size_t>(button)].hits : 0;
}

FBE_API int32_t fbe_input_gamepad_ups(uint64_t pad, int32_t button) {
    const pad_t* found = pad_of(pad);
    const bool there = found != nullptr && valid_button(button) && counts.current();
    return there ? found->buttons[static_cast<std::size_t>(button)].ups : 0;
}

FBE_API float fbe_input_gamepad_raw_axis(uint64_t pad, int32_t axis) {
    const pad_t* found = pad_of(pad);
    return found != nullptr && valid_axis(axis) ? raw(found->axes[static_cast<std::size_t>(axis)])
                                                : 0.0F;
}

FBE_API float fbe_input_gamepad_axis(uint64_t pad, int32_t axis) {
    const pad_t* found = pad_of(pad);
    if (found == nullptr || !valid_axis(axis)) {
        return 0.0F;
    }
    const float value = raw(found->axes[static_cast<std::size_t>(axis)]);
    if (is_trigger(axis)) {
        return past(value, trigger_zone);
    }
    // A stick's dead zone is round: both its axes, so a diagonal isn't cut to the nearest one.
    const std::size_t first = static_cast<std::size_t>(axis) & ~std::size_t{ 1 };
    const float across = raw(found->axes[first]);
    const float along = raw(found->axes[first + 1]);
    const float length = std::hypot(across, along);
    return length > 0.0F ? value * past(length, stick_zone) / length : 0.0F;
}

FBE_API bool fbe_input_set_gamepad_dead_zones(float stick, float trigger) {
    // Written so a NaN is out of range too.
    const bool in_range = stick >= 0.0F && stick < 1.0F && trigger >= 0.0F && trigger < 1.0F;
    if (!in_range) {
        return fbe::fail(
            std::format("a dead zone is from 0 to under 1, not {} and {}", stick, trigger));
    }
    stick_zone = stick;
    trigger_zone = trigger;
    return true;
}

FBE_API float fbe_input_gamepad_stick_dead_zone(void) {
    return stick_zone;
}

FBE_API float fbe_input_gamepad_trigger_dead_zone(void) {
    return trigger_zone;
}

FBE_API bool fbe_input_gamepad_rumble(uint64_t pad, float low, float high, double seconds) {
    const pad_t* found = pad_of(pad);
    if (found == nullptr) {
        return fbe::fail("the gamepad isn't connected");
    }
    if (!valid_time(seconds)) {
        return false;
    }
    // No time is no rumble, as SDL's 0 would rumble for good.
    const Uint32 time = milliseconds(seconds);
    const bool rumbled =
        time > 0 ? SDL_RumbleGamepad(found->gamepad, strength(low), strength(high), time)
                 : SDL_RumbleGamepad(found->gamepad, 0, 0, 0);
    return rumbled ? true : fbe::fail_sdl("the gamepad can't rumble");
}

FBE_API bool fbe_input_gamepad_rumble_triggers(uint64_t pad, float left, float right,
                                               double seconds) {
    const pad_t* found = pad_of(pad);
    if (found == nullptr) {
        return fbe::fail("the gamepad isn't connected");
    }
    if (!valid_time(seconds)) {
        return false;
    }
    const Uint32 time = milliseconds(seconds);
    const bool rumbled =
        time > 0 ? SDL_RumbleGamepadTriggers(found->gamepad, strength(left), strength(right), time)
                 : SDL_RumbleGamepadTriggers(found->gamepad, 0, 0, 0);
    return rumbled ? true : fbe::fail_sdl("the gamepad's triggers can't rumble");
}

FBE_API const char* fbe_input_gamepad_guid(uint64_t pad) {
    const pad_t* found = pad_of(pad);
    guid_text.clear();
    if (found != nullptr) {
        std::array<char, 33> text{};
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(found->id), text.data(),
                         static_cast<int>(text.size()));
        guid_text = text.data();
    }
    return guid_text.c_str();
}

FBE_API const char* fbe_input_gamepad_serial(uint64_t pad) {
    const pad_t* found = pad_of(pad);
    const char* serial = found != nullptr ? SDL_GetGamepadSerial(found->gamepad) : nullptr;
    name = serial != nullptr ? serial : "";
    return name.c_str();
}

FBE_API int32_t fbe_input_gamepad_player_index(uint64_t pad) {
    const pad_t* found = pad_of(pad);
    return found != nullptr ? SDL_GetGamepadPlayerIndex(found->gamepad) : -1;
}

FBE_API bool fbe_input_gamepad_set_player_index(uint64_t pad, int32_t index) {
    const pad_t* found = pad_of(pad);
    if (found == nullptr) {
        return fbe::fail("the gamepad isn't connected");
    }
    if (index < -1) {
        return fbe::fail(
            std::format("a player's index is 0 or more, or -1 for none, not {}", index));
    }
    return SDL_SetGamepadPlayerIndex(found->gamepad, index)
               ? true
               : fbe::fail_sdl("couldn't set the gamepad's player");
}

FBE_API void fbe_input_flush_gamepads(void) {
    zero_counts();
    fbe::flush_joysticks();
}

} // extern "C"
