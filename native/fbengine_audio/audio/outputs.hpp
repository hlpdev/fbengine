#pragma once

// The names outputs are listed and chosen by: the OS's, made one of each, as two of one model of
// headphones have the same.

#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace fbe::audio {

/// `names`, the OS's for each output in order, with each one that's already been named, after
/// the first, given the first number from 2 that makes it unlike every other, "Headphones (2)",
/// so each output can be chosen by its own.
inline std::vector<std::string> output_names(const std::vector<std::string>& names) {
    std::vector<std::string> made;
    made.reserve(names.size());
    const auto taken = [&](const std::string& name) {
        return std::ranges::find(made, name) != made.end() ||
               std::ranges::find(names, name) != names.end();
    };
    for (const std::string& name : names) {
        if (std::ranges::find(made, name) == made.end()) {
            made.push_back(name);
            continue;
        }
        for (int number = 2;; ++number) {
            std::string numbered = std::format("{} ({})", name, number);
            if (!taken(numbered)) {
                made.push_back(std::move(numbered));
                break;
            }
        }
    }
    return made;
}

} // namespace fbe::audio
