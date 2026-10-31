// Joysticks, every one SDL sees, unmapped: wheels, flight sticks, and pads with no mapping, as the
// events fbe_frame_flip reads say.

#include "core/engine.hpp"
#include "fbengine.h"
#include "input/input.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <SDL3/SDL.h>
#include <string>
#include <vector>

namespace {

// A joystick: SDL's, and what the events read say of its axes, buttons, and hats.
struct stick_t {
    SDL_JoystickID id = 0;
    SDL_Joystick* joystick = nullptr;
    std::vector<std::int16_t> axes;
    std::vector<fbe::button_t> buttons;
    std::vector<std::uint8_t> hats;
};

// In the order they connected.
std::vector<stick_t> sticks;
fbe::frame_counts_t counts;
std::vector<SDL_JoystickID> added;
std::vector<SDL_JoystickID> removed;

// fbe_input_joystick_name's, which the next call replaces.
std::string name;

stick_t* stick_of(std::uint64_t stick) {
    const auto found = std::ranges::find(sticks, stick, &stick_t::id);
    return found != sticks.end() ? &*found : nullptr;
}

/// Whether `index` is one of `items`'.
template <typename Items>
bool in(const Items& items, std::int32_t index) {
    return index >= 0 && static_cast<std::size_t>(index) < items.size();
}

void zero_counts() {
    for (stick_t& stick : sticks) {
        for (fbe::button_t& button : stick.buttons) {
            button.zero();
        }
    }
    added.clear();
    removed.clear();
}

void count() {
    if (!counts.start()) {
        zero_counts();
    }
}

std::size_t at_least_none(int count) {
    return static_cast<std::size_t>(std::max(count, 0));
}

bool open(SDL_JoystickID id) {
    if (stick_of(id) != nullptr) {
        return true;
    }
    SDL_Joystick* joystick = SDL_OpenJoystick(id);
    if (joystick == nullptr) {
        return false;
    }
    stick_t& stick = sticks.emplace_back();
    stick.id = id;
    stick.joystick = joystick;
    stick.axes.resize(at_least_none(SDL_GetNumJoystickAxes(joystick)));
    stick.buttons.resize(at_least_none(SDL_GetNumJoystickButtons(joystick)));
    stick.hats.resize(at_least_none(SDL_GetNumJoystickHats(joystick)));
    for (std::size_t axis = 0; axis < stick.axes.size(); ++axis) {
        stick.axes[axis] = SDL_GetJoystickAxis(joystick, static_cast<int>(axis));
    }
    for (std::size_t button = 0; button < stick.buttons.size(); ++button) {
        stick.buttons[button].down = SDL_GetJoystickButton(joystick, static_cast<int>(button));
    }
    for (std::size_t hat = 0; hat < stick.hats.size(); ++hat) {
        stick.hats[hat] = SDL_GetJoystickHat(joystick, static_cast<int>(hat));
    }
    return true;
}

void close(SDL_JoystickID id) {
    const auto found = std::ranges::find(sticks, id, &stick_t::id);
    if (found != sticks.end()) {
        SDL_CloseJoystick(found->joystick);
        sticks.erase(found);
    }
}

std::uint64_t at(const std::vector<SDL_JoystickID>& ids, std::int32_t index) {
    return counts.current() && in(ids, index) ? ids[static_cast<std::size_t>(index)] : 0;
}

std::int32_t count_of(const std::vector<SDL_JoystickID>& ids) {
    return counts.current() ? static_cast<std::int32_t>(ids.size()) : 0;
}

/// The button, if the joystick and its button are there.
const fbe::button_t* button_of(std::uint64_t stick, std::int32_t button) {
    const stick_t* found = stick_of(stick);
    return found != nullptr && in(found->buttons, button)
               ? &found->buttons[static_cast<std::size_t>(button)]
               : nullptr;
}

} // namespace

namespace fbe {

void open_joysticks() {
    int found = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&found);
    for (int index = 0; ids != nullptr && index < found; ++index) {
        open(ids[index]);
    }
    SDL_free(ids);
}

void flush_joysticks() {
    zero_counts();
}

void joystick_event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_JOYSTICK_ADDED:
        if (SDL_WasInit(SDL_INIT_JOYSTICK) != 0 && open(event.jdevice.which)) {
            count();
            added.push_back(event.jdevice.which);
        }
        break;
    case SDL_EVENT_JOYSTICK_REMOVED:
        if (stick_of(event.jdevice.which) != nullptr) {
            count();
            close(event.jdevice.which);
            removed.push_back(event.jdevice.which);
        }
        break;
    case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
        stick_t* stick = stick_of(event.jaxis.which);
        if (stick != nullptr && in(stick->axes, event.jaxis.axis)) {
            stick->axes[event.jaxis.axis] = event.jaxis.value;
        }
        break;
    }
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP: {
        stick_t* stick = stick_of(event.jbutton.which);
        if (stick != nullptr && in(stick->buttons, event.jbutton.button)) {
            count();
            fbe::button_t& button = stick->buttons[event.jbutton.button];
            if (event.jbutton.down) {
                button.press();
            } else {
                button.release();
            }
        }
        break;
    }
    case SDL_EVENT_JOYSTICK_HAT_MOTION: {
        stick_t* stick = stick_of(event.jhat.which);
        if (stick != nullptr && in(stick->hats, event.jhat.hat)) {
            stick->hats[event.jhat.hat] = event.jhat.value;
        }
        break;
    }
    default:
        break;
    }
}

} // namespace fbe

extern "C" {

FBE_API int32_t fbe_input_joystick_count(void) {
    if (!fbe::start_gamepads()) {
        return -1;
    }
    return static_cast<std::int32_t>(sticks.size());
}

FBE_API uint64_t fbe_input_joystick_at(int32_t index) {
    return in(sticks, index) ? sticks[static_cast<std::size_t>(index)].id : 0;
}

FBE_API int32_t fbe_input_joysticks_added_count(void) {
    return count_of(added);
}

FBE_API uint64_t fbe_input_joystick_added_at(int32_t index) {
    return at(added, index);
}

FBE_API int32_t fbe_input_joysticks_removed_count(void) {
    return count_of(removed);
}

FBE_API uint64_t fbe_input_joystick_removed_at(int32_t index) {
    return at(removed, index);
}

FBE_API bool fbe_input_joystick_connected(uint64_t stick) {
    return stick_of(stick) != nullptr;
}

FBE_API const char* fbe_input_joystick_name(uint64_t stick) {
    const stick_t* found = stick_of(stick);
    const char* named = found != nullptr ? SDL_GetJoystickName(found->joystick) : nullptr;
    name = named != nullptr ? named : "";
    return name.c_str();
}

FBE_API int32_t fbe_input_joystick_type(uint64_t stick) {
    const stick_t* found = stick_of(stick);
    return found != nullptr ? SDL_GetJoystickType(found->joystick) : SDL_JOYSTICK_TYPE_UNKNOWN;
}

FBE_API bool fbe_input_joystick_is_gamepad(uint64_t stick) {
    return stick_of(stick) != nullptr && SDL_IsGamepad(static_cast<SDL_JoystickID>(stick));
}

FBE_API int32_t fbe_input_joystick_axis_count(uint64_t stick) {
    const stick_t* found = stick_of(stick);
    return found != nullptr ? static_cast<std::int32_t>(found->axes.size()) : 0;
}

FBE_API int32_t fbe_input_joystick_button_count(uint64_t stick) {
    const stick_t* found = stick_of(stick);
    return found != nullptr ? static_cast<std::int32_t>(found->buttons.size()) : 0;
}

FBE_API int32_t fbe_input_joystick_hat_count(uint64_t stick) {
    const stick_t* found = stick_of(stick);
    return found != nullptr ? static_cast<std::int32_t>(found->hats.size()) : 0;
}

FBE_API float fbe_input_joystick_axis(uint64_t stick, int32_t axis) {
    const stick_t* found = stick_of(stick);
    if (found == nullptr || !in(found->axes, axis)) {
        return 0.0F;
    }
    const auto value = static_cast<float>(found->axes[static_cast<std::size_t>(axis)]);
    return std::max(value / 32767.0F, -1.0F);
}

FBE_API bool fbe_input_joystick_down(uint64_t stick, int32_t button) {
    const fbe::button_t* found = button_of(stick, button);
    return found != nullptr && found->down;
}

FBE_API int32_t fbe_input_joystick_hits(uint64_t stick, int32_t button) {
    const fbe::button_t* found = button_of(stick, button);
    return found != nullptr && counts.current() ? found->hits : 0;
}

FBE_API int32_t fbe_input_joystick_ups(uint64_t stick, int32_t button) {
    const fbe::button_t* found = button_of(stick, button);
    return found != nullptr && counts.current() ? found->ups : 0;
}

FBE_API int32_t fbe_input_joystick_hat(uint64_t stick, int32_t hat) {
    const stick_t* found = stick_of(stick);
    return found != nullptr && in(found->hats, hat) ? found->hats[static_cast<std::size_t>(hat)]
                                                    : 0;
}

} // extern "C"
