// app's guard and links, apart from the DLL: whether it shows nothing for SDL's video driver
// hints, and, on Windows, that each path's link reads back through the shell as the path it
// opens. Nothing here shows a message box or opens a link.

#include "app/app.hpp"

#include <catch2/catch_test_macros.hpp>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <array>
#include <shlwapi.h>
#include <windows.h>
#endif

namespace {

/// Sets the video driver hint over the environment's, which ctest sets to "offscreen".
void set_driver(const char* driver) {
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, driver, SDL_HINT_OVERRIDE);
}

} // namespace

TEST_CASE("nothing's shown headless, or on a video driver with no screen") {
    set_driver("windows");
    CHECK_FALSE(fbe::shows_nothing(false));
    CHECK(fbe::shows_nothing(true));
    set_driver("offscreen");
    CHECK(fbe::shows_nothing(false));
    set_driver("dummy");
    CHECK(fbe::shows_nothing(false));
    set_driver("x11");
    CHECK_FALSE(fbe::shows_nothing(false));
    // SDL reads the hint as a list, the first it can start winning, in any case.
    set_driver("offscreen,windows");
    CHECK(fbe::shows_nothing(false));
    set_driver("windows,offscreen");
    CHECK_FALSE(fbe::shows_nothing(false));
    set_driver("OFFSCREEN");
    CHECK(fbe::shows_nothing(false));
    set_driver("offscreenish");
    CHECK_FALSE(fbe::shows_nothing(false));
    set_driver("offscreen");
}

TEST_CASE("once the video's started, the driver it started is the one that counts") {
    // SDL's offscreen driver starts only for its name in lowercase in the hint, whatever case its
    // list's matched in; "OFFSCREEN" shows nothing above all the same, so a test never waits.
    set_driver("offscreen");
    const bool started = SDL_InitSubSystem(SDL_INIT_VIDEO);
    INFO(SDL_GetError());
    REQUIRE(started);
    CHECK(fbe::shows_nothing(false));
    // A hint set after it started changes nothing SDL does, nor this.
    set_driver("windows");
    CHECK(fbe::shows_nothing(false));
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    CHECK_FALSE(fbe::shows_nothing(false));
    set_driver("offscreen");
}

TEST_CASE("a scheme is two letters or more before a colon, and a drive isn't one") {
    CHECK(fbe::has_scheme("https://example.com"));
    CHECK(fbe::has_scheme("mailto:someone@example.com"));
    CHECK(fbe::has_scheme("x-game+v1.2:open"));
    CHECK_FALSE(fbe::has_scheme("C:\\Games\\x.txt"));
    CHECK_FALSE(fbe::has_scheme("C:x.txt"));
    CHECK_FALSE(fbe::has_scheme("notes 1:2.txt"));
    CHECK_FALSE(fbe::has_scheme("1http:x"));
    CHECK_FALSE(fbe::has_scheme("saves/slot 1.sav"));
}

TEST_CASE("a link with a scheme is opened as it's given") {
    const fbe::opening_t opening = fbe::opening("https://example.com/a page?x=1", "");
    CHECK(opening.link == "https://example.com/a page?x=1");
    CHECK(opening.opened == opening.link);
}

#ifdef _WIN32

namespace {

std::wstring wide(std::string_view text) {
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring made(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), made.data(), size);
    return made;
}

/// The path the shell reads `link` as, without opening it.
std::wstring path_of(const std::string& link) {
    std::array<wchar_t, 2048> path{};
    auto size = static_cast<DWORD>(path.size());
    REQUIRE(PathCreateFromUrlW(wide(link).c_str(), path.data(), &size, 0) == S_OK);
    return { path.data(), size };
}

/// Checks that `path`'s link reads back as the path it opens, and gives the link.
std::string round_trip(const char* path) {
    const fbe::opening_t opening = fbe::opening(path, "");
    INFO(opening.link);
    CHECK(path_of(opening.link) == wide(opening.opened));
    return opening.link;
}

} // namespace

TEST_CASE("a Windows path's link reads back through the shell as the path it opens") {
    // é, which the shell reads in %XX as the ANSI code page's two characters, so it's left as
    // it is, as UrlCreateFromPathW leaves it.
    CHECK(round_trip("C:\\Games\\\xC3\xA9.txt") == "file:///C:/Games/\xC3\xA9.txt");
    // Cyrillic: a user folder and a save's name.
    round_trip("C:\\Users\\\xD0\x98\xD0\xB2\xD0\xB0\xD0\xBD\\\xD1\x81\xD0\xBE\xD1\x85\xD1\x80"
               "\xD0\xB0\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5.sav");
    CHECK(round_trip("C:\\Games\\a b #1.txt") == "file:///C:/Games/a%20b%20%231.txt");
    // A UNC path's server is the link's host.
    CHECK(round_trip("\\\\server\\share\\a b.txt") == "file://server/share/a%20b.txt");
    // A long path's prefix stays on what's opened, which ShellExecuteW takes, and not its link.
    const fbe::opening_t long_path = fbe::opening(R"(\\?\C:\Games\x.txt)", "");
    CHECK(long_path.link == "file:///C:/Games/x.txt");
    CHECK(path_of(long_path.link) == L"C:\\Games\\x.txt");
}

TEST_CASE("a relative Windows path is the OS's own full one") {
    // A drive's own folder, not the current folder with "C:x.txt" on its end.
    const fbe::opening_t drive = fbe::opening("C:x.txt", "");
    CHECK(drive.opened.starts_with("C:\\"));
    CHECK(drive.opened.ends_with("\\x.txt"));
    CHECK_FALSE(drive.opened.contains("C:x"));
    const fbe::opening_t relative = fbe::opening("saves\\slot 1.sav", "");
    CHECK(relative.opened.find(":\\") == 1);
    CHECK(relative.link.ends_with("/saves/slot%201.sav"));
    CHECK(path_of(relative.link) == wide(relative.opened));
}

#else

TEST_CASE("a path's link is RFC 8089's, its UTF-8 in %XX") {
    const fbe::opening_t absolute = fbe::opening("/home/player/My Saves/\xC3\xA9 1.sav", "/x/");
    CHECK(absolute.link == "file:///home/player/My%20Saves/%C3%A9%201.sav");
    CHECK(absolute.opened == absolute.link);
    // From the current folder, and a backslash is a name's own on POSIX.
    const fbe::opening_t relative = fbe::opening("saves\\a #1.sav", "/srv/game/");
    CHECK(relative.link == "file:///srv/game/saves%5Ca%20%231.sav");
}

#endif
