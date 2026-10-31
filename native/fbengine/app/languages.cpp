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
        // SDL reads the hint's empty entries into the next one's language, as ",en".
        std::string_view language = locale->language;
        language.remove_prefix(std::min(language.find_first_not_of(", "), language.size()));
        const auto alphabetic = [](char each) {
            return std::isalpha(static_cast<unsigned char>(each)) != 0;
        };
        if (language.empty() || !std::ranges::all_of(language, alphabetic)) {
            continue;
        }
        // BCP 47's usual case: the language's in lowercase, and after it, a country's in
        // uppercase, or a script's, four letters, in title case, as an OS without a country gives.
        std::string tag;
        for (const char each : language) {
            tag += static_cast<char>(std::tolower(static_cast<unsigned char>(each)));
        }
        const std::string_view country = locale->country != nullptr ? locale->country : "";
        if (!country.empty()) {
            tag += '-';
            for (std::size_t at = 0; at < country.size(); ++at) {
                const auto letter = static_cast<unsigned char>(country[at]);
                const bool upper = country.size() != 4 || at == 0;
                tag += static_cast<char>(upper ? std::toupper(letter) : std::tolower(letter));
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
