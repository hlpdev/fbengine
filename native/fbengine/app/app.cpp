// The OS's message box, and opening links and files, which headless only say what they'd do.

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

/// Whether nothing's to be shown: headless, or SDL's video driver set to one with no screen, as
/// tests set it, so a test can't open a message box that waits for a player.
bool headless() {
    if (fbe_window_headless()) {
        return true;
    }
    const char* driver = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    const std::string_view named = driver != nullptr ? driver : "";
    return named == "offscreen" || named == "dummy";
}

/// Whether `url` starts with a scheme, like "https:" or "mailto:", which a path doesn't: a
/// Windows drive, "C:", is one letter.
bool has_scheme(std::string_view url) {
    const std::size_t colon = url.find(':');
    if (colon == std::string_view::npos || colon < 2) {
        return false;
    }
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (!letter(url[0])) {
        return false;
    }
    for (std::size_t at = 1; at < colon; ++at) {
        const char c = url[at];
        const bool digit = c >= '0' && c <= '9';
        const bool in_scheme = letter(c) || digit || c == '+' || c == '-' || c == '.';
        if (!in_scheme) {
            return false;
        }
    }
    return true;
}

/// Whether `path` is absolute: from a slash or a backslash, or a Windows drive and either.
bool absolute(std::string_view path) {
    if (!path.empty() && (path.front() == '/' || path.front() == '\\')) {
        return true;
    }
    return path.size() >= 3 && path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

/// `path` as a file:// link, from the current folder when it isn't absolute, its backslashes
/// slashes, and what a link can't hold as it is, spaces and the rest, in %XX.
std::string file_link(std::string_view path) {
    std::string full;
    if (!absolute(path)) {
        char* current = SDL_GetCurrentDirectory();
        if (current != nullptr) {
            full = current;
        }
        SDL_free(current);
    }
    full += path;
    std::string link = "file://";
    // A Windows path starts with its drive; a link's path starts with a slash.
    if (full.empty() || (full.front() != '/' && full.front() != '\\')) {
        link += '/';
    }
    constexpr std::string_view hex = "0123456789ABCDEF";
    for (const char unit : full) {
        const auto byte = static_cast<unsigned char>(unit);
        const bool plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                           (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
                           byte == '_' || byte == '~' || byte == '/' || byte == ':';
        if (byte == '\\') {
            link += '/';
        } else if (plain) {
            link += static_cast<char>(byte);
        } else {
            link += '%';
            link += hex[byte >> 4U];
            link += hex[byte & 15U];
        }
    }
    return link;
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
    if (headless() || !SDL_ShowSimpleMessageBox(flags, heading, body, fbe::window())) {
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
    const std::string link = has_scheme(given) ? std::string(given) : file_link(given);
    if (headless()) {
        opened.push_back(link);
        return true;
    }
    return SDL_OpenURL(link.c_str()) ? true : fbe::fail_sdl(std::format("couldn't open {}", link));
}

FBE_API int32_t fbe_app_opened_count(void) {
    return static_cast<std::int32_t>(opened.size());
}

FBE_API const char* fbe_app_opened_at(int32_t index) {
    const bool there = index >= 0 && static_cast<std::size_t>(index) < opened.size();
    return there ? opened[static_cast<std::size_t>(index)].c_str() : "";
}

} // extern "C"
