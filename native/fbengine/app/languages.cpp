// The player's languages, as the OS prefers them.

#include "app/app.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_stdinc.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool alphabetic(std::string_view text) {
    return std::ranges::all_of(
        text, [](char each) { return std::isalpha(static_cast<unsigned char>(each)) != 0; });
}

bool numeric(std::string_view text) {
    return std::ranges::all_of(
        text, [](char each) { return std::isdigit(static_cast<unsigned char>(each)) != 0; });
}

/// `piece` up to its first character that isn't a letter or a digit, as "US" of "US.UTF-8".
std::string_view kept(std::string_view piece) {
    const auto end = std::ranges::find_if(
        piece, [](char each) { return std::isalnum(static_cast<unsigned char>(each)) == 0; });
    return piece.substr(0, static_cast<std::size_t>(end - piece.begin()));
}

/// A subtag after the language in BCP 47's usual case: a script, four letters, in title case, a
/// region, two letters or three digits, in uppercase, and anything else in lowercase.
std::string cased(std::string_view piece) {
    const bool script = piece.size() == 4 && alphabetic(piece);
    const bool region =
        (piece.size() == 2 && alphabetic(piece)) || (piece.size() == 3 && numeric(piece));
    std::string made;
    for (std::size_t at = 0; at < piece.size(); ++at) {
        const auto each = static_cast<unsigned char>(piece[at]);
        const bool upper = region || (script && at == 0);
        made += static_cast<char>(upper ? std::toupper(each) : std::tolower(each));
    }
    return made;
}

/// Whether `a` is `b`, in any case.
bool named(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

} // namespace

namespace fbe {

std::vector<std::string> languages() {
    std::vector<std::string> found;
    int count = 0;
    SDL_Locale** locales = SDL_GetPreferredLocales(&count);
    if (locales == nullptr) {
        return found;
    }
    for (int i = 0; i < count; ++i) {
        const SDL_Locale* locale = locales[i];
        if (locale == nullptr || locale->language == nullptr) {
            continue;
        }
        // SDL reads the hint's empty entries into the next one's language, as ",en", and gives
        // a script with the language, as macOS's "zh-Hant" with "TW", which are its pieces.
        std::string_view language = locale->language;
        language.remove_prefix(std::min(language.find_first_not_of(", "), language.size()));
        std::vector<std::string_view> pieces;
        for (std::size_t start = 0; start <= language.size();) {
            const std::size_t dash = std::min(language.find('-', start), language.size());
            pieces.push_back(language.substr(start, dash - start));
            start = dash + 1;
        }
        if (locale->country != nullptr && *locale->country != '\0') {
            pieces.emplace_back(locale->country);
        }
        // The language, its letters in lowercase; POSIX's "C" and "POSIX" aren't one.
        const std::string_view first = kept(pieces.front());
        if (first.empty() || !alphabetic(first) || named(first, "c") || named(first, "posix")) {
            continue;
        }
        std::string tag;
        for (const char each : first) {
            tag += static_cast<char>(std::tolower(static_cast<unsigned char>(each)));
        }
        for (std::size_t at = 1; at < pieces.size(); ++at) {
            const std::string_view piece = kept(pieces[at]);
            if (!piece.empty()) {
                tag += '-';
                tag += cased(piece);
            }
        }
        if (std::ranges::find(found, tag) == found.end()) {
            found.push_back(tag);
        }
    }
    SDL_free(static_cast<void*>(locales));
    return found;
}

} // namespace fbe
