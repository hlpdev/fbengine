// Where Vulkan's looked for on macOS after the system's own search: beside the program and in its
// app's Frameworks, then Homebrew's, the loader before MoltenVK alone. Only the list is checked
// here, as it's the same on every system; loading from it is macOS CI's.

#include "gpu/loader.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

TEST_CASE("a program's own folder and its app's Frameworks come before Homebrew's") {
    const std::vector<std::string> expected{
        "/Games/X.app/Contents/Resources/libvulkan.1.dylib",
        "/Games/X.app/Contents/Resources/libMoltenVK.dylib",
        "/Games/X.app/Contents/Resources/../Frameworks/libvulkan.1.dylib",
        "/Games/X.app/Contents/Resources/../Frameworks/libMoltenVK.dylib",
        "/opt/homebrew/lib/libvulkan.1.dylib",
        "/usr/local/lib/libvulkan.1.dylib",
        "/opt/homebrew/lib/libMoltenVK.dylib",
        "/usr/local/lib/libMoltenVK.dylib",
    };
    CHECK(fbe::gpu::apple_vulkan_libraries("/Games/X.app/Contents/Resources/") == expected);
}

TEST_CASE("with no program folder, only Homebrew's are tried") {
    const std::vector<std::string> expected{
        "/opt/homebrew/lib/libvulkan.1.dylib",
        "/usr/local/lib/libvulkan.1.dylib",
        "/opt/homebrew/lib/libMoltenVK.dylib",
        "/usr/local/lib/libMoltenVK.dylib",
    };
    CHECK(fbe::gpu::apple_vulkan_libraries("") == expected);
}
