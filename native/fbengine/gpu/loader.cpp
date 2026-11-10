#include "gpu/loader.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace fbe::gpu {

std::vector<std::string> apple_vulkan_libraries(std::string_view base) {
    constexpr std::array<std::string_view, 2> names{ "libvulkan.1.dylib", "libMoltenVK.dylib" };
    constexpr std::array<std::string_view, 2> homebrew{ "/opt/homebrew/lib/", "/usr/local/lib/" };
    std::vector<std::string> found;
    if (!base.empty()) {
        for (const std::string_view folder :
             { std::string_view{}, std::string_view{ "../Frameworks/" } }) {
            for (const std::string_view name : names) {
                found.push_back(std::string(base) + std::string(folder) + std::string(name));
            }
        }
    }
    for (const std::string_view name : names) {
        for (const std::string_view folder : homebrew) {
            found.push_back(std::string(folder) + std::string(name));
        }
    }
    return found;
}

} // namespace fbe::gpu
