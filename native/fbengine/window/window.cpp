// The window, which fbe_window_open opens, and the displays it can go on.

#include "core/engine.hpp"
#include "fbengine.h"

#include <cstdint>
#include <format>
#include <SDL3/SDL.h>
#include <string>

namespace {

// The largest side a window can have, which SDL and every GPU takes.
constexpr std::int32_t largest = 16384;

SDL_Window* window = nullptr;
std::string title;
std::int32_t current_mode = FBE_WINDOW_WINDOWED;
bool resizable = false;

// What the OS's events, which fbe_frame_flip reads, said of the window: its close button or the
// app's quit, its focus, and whether it's minimized.
bool closed = false;
bool focused = false;
bool minimized = false;

/// The window's display, or the main one.
SDL_DisplayID display_of_window() {
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    return display != 0 ? display : SDL_GetPrimaryDisplay();
}

/// Whether the window's display lists modes for fullscreen, as some, like Wayland's and
/// SDL's offscreen one, don't.
bool lists_modes() {
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display_of_window(), &count);
    SDL_free(static_cast<void*>(modes));
    return modes != nullptr && count > 0;
}

/// Puts the window in `wanted`: for fullscreen, at the display mode closest to its size, or at
/// the desktop's on a display that lists none.
bool apply_mode(std::int32_t wanted) {
    if (wanted == FBE_WINDOW_WINDOWED) {
        if (!SDL_SetWindowFullscreen(window, false)) {
            return fbe::fail_sdl("couldn't leave fullscreen");
        }
    } else if (wanted == FBE_WINDOW_BORDERLESS || !lists_modes()) {
        if (!SDL_SetWindowFullscreenMode(window, nullptr) ||
            !SDL_SetWindowFullscreen(window, true)) {
            return fbe::fail_sdl("couldn't make the window fill the screen");
        }
    } else {
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window, &width, &height);
        SDL_DisplayMode closest{};
        if (!SDL_GetClosestFullscreenDisplayMode(display_of_window(), width, height, 0.0F, true,
                                                 &closest)) {
            return fbe::fail_sdl(
                std::format("the display has no fullscreen mode near {}x{}", width, height));
        }
        if (!SDL_SetWindowFullscreenMode(window, &closest) ||
            !SDL_SetWindowFullscreen(window, true)) {
            return fbe::fail_sdl("couldn't make the window fullscreen");
        }
    }
    SDL_SyncWindow(window);
    current_mode = wanted;
    return true;
}

bool valid_mode(std::int32_t wanted) {
    return wanted == FBE_WINDOW_WINDOWED || wanted == FBE_WINDOW_FULLSCREEN ||
           wanted == FBE_WINDOW_BORDERLESS;
}

/// The display at `index` in SDL's list, or 0.
SDL_DisplayID display_at(std::int32_t index) {
    if (!fbe::start_video() || index < 0) {
        return 0;
    }
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    const SDL_DisplayID display = displays != nullptr && index < count ? displays[index] : 0;
    SDL_free(displays);
    return display;
}

void copy_mode(const SDL_DisplayMode& from, fbe_display_mode_t* to) {
    to->width = from.w;
    to->height = from.h;
    to->refresh_rate = from.refresh_rate;
    to->pixel_density = from.pixel_density;
}

} // namespace

namespace fbe {

SDL_Window* window() {
    return ::window;
}

void window_event(const SDL_Event& event) {
    const bool ours = window() != nullptr && event.window.windowID == SDL_GetWindowID(window());
    switch (event.type) {
    case SDL_EVENT_QUIT:
        closed = closed || window() != nullptr;
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        closed = closed || ours;
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        focused = focused || ours;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        focused = focused && !ours;
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
        minimized = minimized || ours;
        break;
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
        minimized = minimized && !ours;
        break;
    default:
        break;
    }
}

} // namespace fbe

extern "C" {

FBE_API bool fbe_window_open(int32_t width, int32_t height, int32_t wanted) {
    if (width < 1 || height < 1 || width > largest || height > largest) {
        return fbe::fail(
            std::format("a window is 1 to {} pixels on a side, not {}x{}", largest, width, height));
    }
    if (!valid_mode(wanted)) {
        return fbe::fail(std::format("{} isn't a WindowMode", wanted));
    }
    if (!fbe::start_video()) {
        return false;
    }

    if (window == nullptr) {
        SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (resizable) {
            flags |= SDL_WINDOW_RESIZABLE;
        }
        window = SDL_CreateWindow(title.c_str(), width, height, flags);
        if (window == nullptr) {
            return fbe::fail_sdl("couldn't open the window");
        }
        const SDL_WindowFlags now = SDL_GetWindowFlags(window);
        closed = false;
        focused = (now & SDL_WINDOW_INPUT_FOCUS) != 0;
        minimized = (now & SDL_WINDOW_MINIMIZED) != 0;
    } else {
        // A fullscreen window's size is its mode's, so it's windowed while its size changes.
        if (!apply_mode(FBE_WINDOW_WINDOWED)) {
            return false;
        }
        if (!SDL_SetWindowSize(window, width, height)) {
            return fbe::fail_sdl("couldn't resize the window");
        }
        SDL_SyncWindow(window);
    }
    if (wanted != FBE_WINDOW_WINDOWED && !apply_mode(wanted)) {
        return false;
    }
    current_mode = wanted;
    return true;
}

FBE_API void fbe_window_close(void) {
    if (window != nullptr) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    current_mode = FBE_WINDOW_WINDOWED;
    closed = false;
    focused = false;
    minimized = false;
}

FBE_API bool fbe_window_closed(void) {
    return closed;
}

FBE_API bool fbe_window_focused(void) {
    return focused;
}

FBE_API bool fbe_window_minimized(void) {
    return minimized;
}

FBE_API bool fbe_window_simulate(int32_t event) {
    if (window == nullptr) {
        return fbe::fail("there's no window; Graphics opens one");
    }
    SDL_Event pushed{};
    switch (event) {
    case FBE_WINDOW_SIMULATE_CLOSE:
        pushed.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        break;
    case FBE_WINDOW_SIMULATE_FOCUS:
        pushed.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
        break;
    case FBE_WINDOW_SIMULATE_BLUR:
        pushed.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        break;
    case FBE_WINDOW_SIMULATE_MINIMIZE:
        pushed.type = SDL_EVENT_WINDOW_MINIMIZED;
        break;
    case FBE_WINDOW_SIMULATE_RESTORE:
        pushed.type = SDL_EVENT_WINDOW_RESTORED;
        break;
    default:
        return fbe::fail(std::format("{} isn't a window event", event));
    }
    pushed.window.windowID = SDL_GetWindowID(window);
    pushed.window.timestamp = SDL_GetTicksNS();
    return SDL_PushEvent(&pushed) ? true : fbe::fail_sdl("couldn't push the event");
}

FBE_API bool fbe_window_is_open(void) {
    return window != nullptr;
}

FBE_API void fbe_window_set_title(const char* text) {
    title = text != nullptr ? text : "";
    if (window != nullptr) {
        SDL_SetWindowTitle(window, title.c_str());
    }
}

FBE_API const char* fbe_window_title(void) {
    return title.c_str();
}

FBE_API int32_t fbe_window_width(void) {
    int width = 0;
    if (window != nullptr) {
        SDL_GetWindowSize(window, &width, nullptr);
    }
    return width;
}

FBE_API int32_t fbe_window_height(void) {
    int height = 0;
    if (window != nullptr) {
        SDL_GetWindowSize(window, nullptr, &height);
    }
    return height;
}

FBE_API int32_t fbe_window_pixel_width(void) {
    int width = 0;
    if (window != nullptr) {
        SDL_GetWindowSizeInPixels(window, &width, nullptr);
    }
    return width;
}

FBE_API int32_t fbe_window_pixel_height(void) {
    int height = 0;
    if (window != nullptr) {
        SDL_GetWindowSizeInPixels(window, nullptr, &height);
    }
    return height;
}

FBE_API float fbe_window_scale(void) {
    return window != nullptr ? SDL_GetWindowPixelDensity(window) : 0.0F;
}

FBE_API bool fbe_window_set_mode(int32_t wanted) {
    if (!valid_mode(wanted)) {
        return fbe::fail(std::format("{} isn't a WindowMode", wanted));
    }
    if (window == nullptr) {
        return fbe::fail("there's no window; Graphics opens one");
    }
    return apply_mode(wanted);
}

FBE_API int32_t fbe_window_mode(void) {
    return current_mode;
}

FBE_API void fbe_window_set_resizable(bool on) {
    resizable = on;
    if (window != nullptr) {
        SDL_SetWindowResizable(window, on);
    }
}

FBE_API bool fbe_window_resizable(void) {
    return resizable;
}

FBE_API int32_t fbe_display_count(void) {
    if (!fbe::start_video()) {
        return -1;
    }
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    SDL_free(displays);
    if (displays == nullptr) {
        fbe::fail_sdl("couldn't list the displays");
        return -1;
    }
    return count;
}

FBE_API const char* fbe_display_name(int32_t index) {
    const SDL_DisplayID display = display_at(index);
    const char* name = display != 0 ? SDL_GetDisplayName(display) : nullptr;
    return name != nullptr ? name : "";
}

FBE_API bool fbe_display_desktop(int32_t index, fbe_display_mode_t* desktop) {
    const SDL_DisplayID display = display_at(index);
    const SDL_DisplayMode* mode = display != 0 ? SDL_GetDesktopDisplayMode(display) : nullptr;
    if (mode == nullptr) {
        return fbe::fail(std::format("there's no display {}", index));
    }
    copy_mode(*mode, desktop);
    return true;
}

FBE_API int32_t fbe_display_mode_count(int32_t index) {
    const SDL_DisplayID display = display_at(index);
    if (display == 0) {
        fbe::fail(std::format("there's no display {}", index));
        return -1;
    }
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count);
    SDL_free(static_cast<void*>(modes));
    return modes != nullptr ? count : 0;
}

FBE_API bool fbe_display_mode_at(int32_t index, int32_t at, fbe_display_mode_t* found) {
    const SDL_DisplayID display = display_at(index);
    if (display == 0) {
        return fbe::fail(std::format("there's no display {}", index));
    }
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count);
    const bool there = modes != nullptr && at >= 0 && at < count;
    if (there) {
        copy_mode(*modes[at], found);
    }
    SDL_free(static_cast<void*>(modes));
    return there ? true : fbe::fail(std::format("display {} has no mode {}", index, at));
}

} // extern "C"
