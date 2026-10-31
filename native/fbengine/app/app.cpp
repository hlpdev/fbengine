// The OS's message box, and opening links and files, which headless only say what they'd do.

#include "app/app.hpp"

#include "core/engine.hpp"
#include "fbengine.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <SDL3/SDL.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

// The links fbe_app_open_url kept, headless.
std::vector<std::string> opened;

/// The current folder, ending in its separator, which a relative path is from.
std::string current_folder() {
    char* current = SDL_GetCurrentDirectory();
    std::string folder = current != nullptr ? current : "";
    SDL_free(current);
    return folder;
}

} // namespace

extern "C" {

FBE_API bool fbe_app_show_message(const char* title, const char* text, int32_t kind) {
    SDL_MessageBoxFlags flags = 0;
    switch (kind) {
    case FBE_APP_MESSAGE_ERROR:
        flags = SDL_MESSAGEBOX_ERROR;
        break;
    case FBE_APP_MESSAGE_WARNING:
        flags = SDL_MESSAGEBOX_WARNING;
        break;
    case FBE_APP_MESSAGE_INFORMATION:
        flags = SDL_MESSAGEBOX_INFORMATION;
        break;
    default:
        return fbe::fail(std::format("{} isn't a MessageKind", kind));
    }
    const char* heading = title != nullptr ? title : "";
    const char* body = text != nullptr ? text : "";
    if (fbe::shows_nothing(fbe_window_headless()) ||
        !SDL_ShowSimpleMessageBox(flags, heading, body, fbe::window())) {
        // std::print would be the way, but MinGW's static libstdc++ hasn't its Unicode half.
        std::fputs(std::format("{}: {}\n", heading, body).c_str(), stderr);
        std::fflush(stderr);
    }
    return true;
}

FBE_API bool fbe_app_open_url(const char* url) {
    const std::string_view given = url != nullptr ? url : "";
    if (given.empty()) {
        return fbe::fail("there's no link to open");
    }
    const fbe::opening_t opening = fbe::opening(given, current_folder());
    if (fbe::shows_nothing(fbe_window_headless())) {
        opened.push_back(opening.link);
        return true;
    }
    return SDL_OpenURL(opening.opened.c_str())
               ? true
               : fbe::fail_sdl(std::format("couldn't open {}", opening.opened));
}

FBE_API int32_t fbe_app_opened_count(void) {
    return static_cast<std::int32_t>(opened.size());
}

FBE_API const char* fbe_app_opened_at(int32_t index) {
    const bool there = index >= 0 && static_cast<std::size_t>(index) < opened.size();
    return there ? opened[static_cast<std::size_t>(index)].c_str() : "";
}

} // extern "C"
