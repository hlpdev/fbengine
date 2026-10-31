#pragma once

// What app's calls work out before they reach the OS, apart from the engine, so its tests can
// check them without opening anything: whether to show nothing, what a link opens, and the
// player's languages.

#include <string>
#include <string_view>
#include <vector>

namespace fbe {

/// Whether nothing's to be shown: `headless`, or SDL's video driver one with no screen,
/// "offscreen" or "dummy", in any case, as tests set it, so a test can't open a message box that
/// waits for a player: the driver SDL started, once its video has, and before, the first in its
/// hint's list (the SDL_VIDEO_DRIVER environment variable included), which it'd try first.
bool shows_nothing(bool headless);

/// Whether `url` starts with a scheme, like "https:" or "mailto:", which a path doesn't: a
/// Windows drive, "C:", is one letter.
bool has_scheme(std::string_view url);

/// What fbe_app_open_url does with what it's given: the link it keeps, and what it hands the OS.
struct opening_t {
    std::string link;
    std::string opened;
};

/// A link as it is, or a path made absolute, from `folder` (the current one, ending in a
/// separator) when it isn't, as its file:// link. On Windows, the path is the OS's own full one,
/// which the OS opens, and its link is UrlCreateFromPathW's, a UNC path's host after "file://"
/// and other than ASCII left as it is, which the shell reads back; elsewhere, the link is opened,
/// with its UTF-8 in %XX, as RFC 8089's are.
opening_t opening(std::string_view url, std::string_view folder);

/// The player's languages, as the OS prefers them, the first most, each once, as BCP 47's tags
/// in their usual case: "en-US", "fr". SDL_HINT_PREFERRED_LOCALES, set, is in the OS's place.
std::vector<std::string> languages();

} // namespace fbe
