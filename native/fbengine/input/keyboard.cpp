// The keyboard and text input, as the events fbe_frame_flip reads say, and Simulate's keys.

#include "core/engine.hpp"
#include "fbengine.h"
#include "input/input.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <SDL3/SDL.h>
#include <string>
#include <vector>

namespace {

// The most presses a Flip keeps in order, for fbe_input_key_hit_at.
constexpr std::size_t most_hits = 64;

// A key: whether it's held, and what the last Flip counted of it.
struct key_t {
    bool down = false;
    std::int32_t hits = 0;
    std::int32_t ups = 0;
    std::int32_t typed = 0;
};

std::array<key_t, SDL_SCANCODE_COUNT> keys{};
fbe::frame_counts_t counts;
std::vector<std::int32_t> hit_order;
std::string text;

// The IME's composition, which stays until it changes.
std::string composition;
std::int32_t composition_start = -1;
std::int32_t composition_length = -1;

// fbe_input_key_name's, which the next call replaces.
std::string name;

bool valid_key(std::int32_t key) {
    return key > SDL_SCANCODE_UNKNOWN && key < SDL_SCANCODE_COUNT;
}

/// A valid key's state.
key_t& state_of(std::int32_t key) {
    return keys[static_cast<std::size_t>(key)];
}

void zero_counts() {
    for (key_t& key : keys) {
        key.hits = 0;
        key.ups = 0;
        key.typed = 0;
    }
    hit_order.clear();
    text.clear();
}

/// Zeroes what was counted when this is the first event of a Flip.
void count() {
    if (!counts.start()) {
        zero_counts();
    }
}

void key_event(const SDL_KeyboardEvent& event) {
    if (!valid_key(event.scancode)) {
        return;
    }
    count();
    key_t& key = state_of(event.scancode);
    if (event.down) {
        ++key.typed;
        // A key that's held already is the OS's repeat, whatever the event says.
        if (!event.repeat && !key.down) {
            ++key.hits;
            if (hit_order.size() < most_hits) {
                hit_order.push_back(event.scancode);
            }
        }
        key.down = true;
    } else if (key.down) {
        ++key.ups;
        key.down = false;
    }
}

/// Lets go of every key, as a window that loses the keyboard's focus, or closes, never hears them
/// go up.
void release_keys() {
    count();
    for (key_t& key : keys) {
        if (key.down) {
            ++key.ups;
            key.down = false;
        }
    }
}

/// Whether the event's window has text input on, which text counts only while.
bool typing(const SDL_Event& event) {
    SDL_Window* window = SDL_GetWindowFromEvent(&event);
    return window != nullptr && SDL_TextInputActive(window);
}

void clear_composition() {
    composition.clear();
    composition_start = -1;
    composition_length = -1;
}

bool push(SDL_Event& event) {
    return SDL_PushEvent(&event) ? true : fbe::fail_sdl("couldn't push the event");
}

// The text of the events Simulate pushed, which SDL leaves to whoever pushed them, kept until
// they're read, each on the heap so its characters stay put.
std::vector<std::unique_ptr<std::string>> pushed_text;

/// A copy of `from` that lives until the event that holds it is read.
const char* event_string(const char* from) {
    pushed_text.push_back(std::make_unique<std::string>(from));
    return pushed_text.back()->c_str();
}

/// Lets go of the copy Simulate made for an event, once it's read.
void read_string(const char* read) {
    for (auto at = pushed_text.begin(); at != pushed_text.end(); ++at) {
        if (at->get()->c_str() == read) {
            pushed_text.erase(at);
            return;
        }
    }
}

/// Pushes an event holding event_string's last copy, which goes if it can't be pushed.
bool push_text(SDL_Event& event) {
    if (!push(event)) {
        pushed_text.pop_back();
        return false;
    }
    return true;
}

SDL_Window* simulated_window() {
    SDL_Window* window = fbe::input_window();
    if (window == nullptr) {
        fbe::fail("there's no window; Graphics opens one");
    }
    return window;
}

} // namespace

namespace fbe {

void keyboard_event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        key_event(event.key);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_DESTROYED:
        release_keys();
        break;
    case SDL_EVENT_TEXT_INPUT:
        if (typing(event) && event.text.text != nullptr) {
            count();
            text += event.text.text;
            clear_composition();
        }
        read_string(event.text.text);
        break;
    case SDL_EVENT_TEXT_EDITING:
        if (typing(event)) {
            composition = event.edit.text != nullptr ? event.edit.text : "";
            composition_start = event.edit.start;
            composition_length = event.edit.length;
        }
        read_string(event.edit.text);
        break;
    default:
        break;
    }
}

} // namespace fbe

extern "C" {

FBE_API bool fbe_input_key_down(int32_t key) {
    return valid_key(key) && state_of(key).down;
}

FBE_API int32_t fbe_input_key_hits(int32_t key) {
    return valid_key(key) && counts.current() ? state_of(key).hits : 0;
}

FBE_API int32_t fbe_input_key_ups(int32_t key) {
    return valid_key(key) && counts.current() ? state_of(key).ups : 0;
}

FBE_API int32_t fbe_input_key_typed(int32_t key) {
    return valid_key(key) && counts.current() ? state_of(key).typed : 0;
}

FBE_API int32_t fbe_input_keys_hit_count(void) {
    return counts.current() ? static_cast<std::int32_t>(hit_order.size()) : 0;
}

FBE_API int32_t fbe_input_key_hit_at(int32_t index) {
    const bool there = counts.current() && index >= 0 &&
                       static_cast<std::size_t>(index) < hit_order.size();
    return there ? hit_order[static_cast<std::size_t>(index)] : 0;
}

FBE_API void fbe_input_flush_keys(void) {
    zero_counts();
}

FBE_API const char* fbe_input_key_name(int32_t key) {
    name.clear();
    if (valid_key(key)) {
        const auto scancode = static_cast<SDL_Scancode>(key);
        const SDL_Keycode code = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
        const char* named = code != SDLK_UNKNOWN ? SDL_GetKeyName(code) : nullptr;
        if (named == nullptr || *named == '\0') {
            named = SDL_GetScancodeName(scancode);
        }
        name = named != nullptr ? named : "";
    }
    return name.c_str();
}

FBE_API bool fbe_input_start_text(void) {
    SDL_Window* window = fbe::input_window();
    if (window == nullptr) {
        return fbe::fail("there's no window; Graphics opens one");
    }
    return SDL_StartTextInput(window) ? true : fbe::fail_sdl("couldn't start text input");
}

FBE_API void fbe_input_stop_text(void) {
    SDL_Window* window = fbe::input_window();
    if (window != nullptr) {
        SDL_StopTextInput(window);
    }
    clear_composition();
}

FBE_API bool fbe_input_text_active(void) {
    SDL_Window* window = fbe::input_window();
    return window != nullptr && SDL_TextInputActive(window);
}

FBE_API bool fbe_input_set_text_area(int32_t x, int32_t y, int32_t width, int32_t height,
                                     int32_t cursor) {
    SDL_Window* window = fbe::input_window();
    if (window == nullptr) {
        return fbe::fail("there's no window; Graphics opens one");
    }
    const SDL_Rect area{ .x = x, .y = y, .w = width, .h = height };
    return SDL_SetTextInputArea(window, &area, cursor) ? true
                                                       : fbe::fail_sdl("couldn't set the text's area");
}

FBE_API const char* fbe_input_text(void) {
    return counts.current() ? text.c_str() : "";
}

FBE_API const char* fbe_input_composition(void) {
    return composition.c_str();
}

FBE_API int32_t fbe_input_composition_start(void) {
    return composition_start;
}

FBE_API int32_t fbe_input_composition_length(void) {
    return composition_length;
}

FBE_API bool fbe_input_simulate_key(int32_t key, bool down, bool repeat) {
    if (!valid_key(key)) {
        return fbe::fail(std::format("{} isn't a Key", key));
    }
    SDL_Window* window = simulated_window();
    if (window == nullptr) {
        return false;
    }
    const auto scancode = static_cast<SDL_Scancode>(key);
    SDL_Event pushed{};
    pushed.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    pushed.key.timestamp = SDL_GetTicksNS();
    pushed.key.windowID = SDL_GetWindowID(window);
    pushed.key.scancode = scancode;
    pushed.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, true);
    pushed.key.down = down;
    pushed.key.repeat = down && repeat;
    return push(pushed);
}

FBE_API bool fbe_input_simulate_text(const char* typed) {
    SDL_Window* window = simulated_window();
    if (window == nullptr) {
        return false;
    }
    SDL_Event pushed{};
    pushed.type = SDL_EVENT_TEXT_INPUT;
    pushed.text.timestamp = SDL_GetTicksNS();
    pushed.text.windowID = SDL_GetWindowID(window);
    pushed.text.text = event_string(typed != nullptr ? typed : "");
    return push_text(pushed);
}

FBE_API bool fbe_input_simulate_composition(const char* composed, int32_t start, int32_t length) {
    SDL_Window* window = simulated_window();
    if (window == nullptr) {
        return false;
    }
    SDL_Event pushed{};
    pushed.type = SDL_EVENT_TEXT_EDITING;
    pushed.edit.timestamp = SDL_GetTicksNS();
    pushed.edit.windowID = SDL_GetWindowID(window);
    pushed.edit.text = event_string(composed != nullptr ? composed : "");
    pushed.edit.start = start;
    pushed.edit.length = length;
    return push_text(pushed);
}

} // extern "C"
