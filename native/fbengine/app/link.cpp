// Whether app's calls show nothing, and what a link or a path opens, apart from the rest of the
// engine, which app_tests compiles with SDL alone.

#include "app/app.hpp"

#include <algorithm>
#include <cstddef>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <vector>
#include <windows.h>
#endif

namespace {

constexpr std::string_view hex = "0123456789ABCDEF";

/// Whether a link can hold `byte` as it is: what RFC 3986 leaves unreserved, and a path's
/// slashes and its drive's colon.
bool plain(unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' ||
           byte == '~' || byte == '/' || byte == ':';
}

/// Whether `named` is `name`, in any case, as SDL compares its drivers' names.
bool named_as(std::string_view named, std::string_view name) {
    const auto lower = [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    return std::ranges::equal(named, name, {}, lower, lower);
}

void escape(std::string& link, unsigned char byte) {
    link += '%';
    link += hex[byte >> 4U];
    link += hex[byte & 15U];
}

#ifdef _WIN32

std::wstring wide(std::string_view text) {
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring made(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), made.data(), size);
    return made;
}

std::string narrow(std::wstring_view text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string made(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), made.data(), size,
                        nullptr, nullptr);
    return made;
}

/// The OS's own full path for `path`, which resolves a drive's own folder ("C:x.txt"), the
/// current drive ("\x.txt"), and "..", as the shell does; `path` as it is if the OS can't.
std::wstring full_path(const std::wstring& path) {
    const DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (size == 0) {
        return path;
    }
    std::vector<wchar_t> full(size);
    const DWORD written = GetFullPathNameW(path.c_str(), size, full.data(), nullptr);
    return written > 0 && written < size ? std::wstring(full.data(), written) : path;
}

/// A full Windows path's link, as UrlCreateFromPathW makes it: a UNC path's server as its host,
/// backslashes as slashes, ASCII a link can't hold in %XX, and the rest as it is, since the shell
/// reads %XX in the ANSI code page, not as UTF-8.
std::string windows_link(std::string_view path) {
    std::string link = "file://";
    std::string_view rest = path;
    // A long path's own prefix isn't the path's: "\\?\C:\x" is C:\x, and "\\?\UNC\server\x"
    // is \\server\x.
    if (rest.starts_with(R"(\\?\UNC\)")) {
        rest.remove_prefix(8);
    } else if (rest.starts_with(R"(\\?\)")) {
        rest.remove_prefix(4);
        link += '/';
    } else if (rest.starts_with("\\\\")) {
        rest.remove_prefix(2);
    } else {
        link += '/';
    }
    for (const char unit : rest) {
        const auto byte = static_cast<unsigned char>(unit);
        if (byte == '\\') {
            link += '/';
        } else if (plain(byte) || byte >= 0x80U) {
            link += unit;
        } else {
            escape(link, byte);
        }
    }
    return link;
}

#else

/// A full path's link, as RFC 8089's are, its UTF-8 in %XX.
std::string posix_link(std::string_view path) {
    std::string link = "file://";
    for (const char unit : path) {
        const auto byte = static_cast<unsigned char>(unit);
        if (plain(byte)) {
            link += unit;
        } else {
            escape(link, byte);
        }
    }
    return link;
}

#endif

} // namespace

namespace fbe {

bool shows_nothing(bool headless) {
    if (headless) {
        return true;
    }
    // The driver SDL started, once it has; before, the first it'd try, as it reads the hint: a
    // list, by commas, in any case.
    std::string_view named;
    if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
        const char* current = SDL_GetCurrentVideoDriver();
        named = current != nullptr ? current : "";
    } else {
        const char* hint = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
        named = hint != nullptr ? hint : "";
        named = named.substr(0, named.find(','));
    }
    return named_as(named, "offscreen") || named_as(named, "dummy");
}

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

opening_t opening(std::string_view url, [[maybe_unused]] std::string_view folder) {
    if (has_scheme(url)) {
        return opening_t{ .link = std::string(url), .opened = std::string(url) };
    }
#ifdef _WIN32
    const std::string full = narrow(full_path(wide(url)));
    return opening_t{ .link = windows_link(full), .opened = full };
#else
    std::string full = url.starts_with('/') ? std::string() : std::string(folder);
    full += url;
    std::string link = posix_link(full);
    return opening_t{ .link = link, .opened = link };
#endif
}

} // namespace fbe
