#pragma once

// Where Vulkan is on macOS when the system's search doesn't find it: dlopen's own places leave out
// Homebrew's on Apple silicon, and a game that ships Vulkan keeps it in its app's Frameworks.

#include <string>
#include <string_view>
#include <vector>

namespace fbe::gpu {

/// The places Vulkan's loader, or MoltenVK alone, may be on macOS, in the order they're tried:
/// beside the program and in its app's Frameworks, from `base`, the program's folder with its
/// slash as SDL_GetBasePath gives it, or none of those when it's empty; then Homebrew's, on Apple
/// silicon and on Intel, where the LunarG SDK's installer puts it too. The loader comes before
/// MoltenVK alone in each, as only the loader has the layers.
std::vector<std::string> apple_vulkan_libraries(std::string_view base);

} // namespace fbe::gpu
