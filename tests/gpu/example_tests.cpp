// The examples' frames against their golden images: frames an example saved as PNGs, playing
// itself headless as ctest runs it, read back and checked as a test's own frames are. Each is a
// hidden test, run by name after its example by a ctest of its own (tests/CMakeLists.txt). Each
// test ends by stopping the GPU, which has to leave nothing behind.

#include "golden/golden.hpp"
#include "gpu_testing.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <fbengine.h>
#include <filesystem>
#include <format>
#include <stb_image.h>
#include <string>
#include <string_view>
#include <utility>

using fbe::tests::require;
using fbe::tests::stop_gpu;

namespace {

/// The PNG an example saved, as a frame.
fbe::golden::frame_t saved(const std::filesystem::path& path) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* loaded = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    INFO(path.string() << " is what the test before this saves");
    REQUIRE(loaded != nullptr);
    fbe::golden::frame_t frame{
        .width = static_cast<std::uint32_t>(width),
        .height = static_cast<std::uint32_t>(height),
    };
    frame.rgba.assign(loaded, loaded + (std::size_t{ frame.width } * frame.height * 4));
    stbi_image_free(loaded);
    return frame;
}

} // namespace

// The editor's panes, and what opens over them, as `--save` saves them.
TEST_CASE("the editor's frames match their golden images", "[.example]") {
    REQUIRE(fbe_window_set_headless(true));
    require(fbe_gpu_start());
    for (const std::string_view frame :
         { "welcome", "nothing_selected", "move", "turn", "scale", "component", "light", "camera",
           "many", "menu", "dialog" }) {
        INFO("frame " << frame);
        const std::filesystem::path path = std::filesystem::path(FBENGINE_EXAMPLE_FRAMES) /
                                           "editor" / std::format("frame_{}.png", frame);
        fbe::golden::check(std::format("editor_{}", frame), saved(path));
    }
    stop_gpu();
}

// The arcade example's frames: its pilot's run's 1, 150, and 300; and with no one flying, a rock
// crossing the ground and the game's end. The GPU started here is the one the example drew on, as
// ctest gives both the same Vulkan, which says how strictly they're compared.
TEST_CASE("the arcade example's frames match their golden images", "[.example]") {
    REQUIRE(fbe_window_set_headless(true));
    require(fbe_gpu_start());
    for (const std::string_view frame : { "1", "150", "300", "ground", "over" }) {
        INFO("frame " << frame);
        const std::filesystem::path path = std::filesystem::path(FBENGINE_EXAMPLE_FRAMES) /
                                           "arcade" / std::format("frame_{}.png", frame);
        fbe::golden::check(std::format("arcade_{}", frame), saved(path));
    }
    stop_gpu();
}

// Khronos's sample models' frames, as models.sample saves them; where they should be is that
// test's.
TEST_CASE("the sample models' frames match their golden images", "[.example]") {
    REQUIRE(fbe_window_set_headless(true));
    require(fbe_gpu_start());
    for (const auto& [name, frames] : {
             std::pair{ "SimpleSkin", 6 },
             std::pair{ "SimpleMorph", 7 },
             std::pair{ "RiggedSimple", 5 },
             std::pair{ "MorphPrimitivesTest", 3 },
         }) {
        for (int frame = 0; frame < frames; ++frame) {
            const std::string file = std::format("model_{}_{}", name, frame);
            INFO("frame " << file);
            fbe::golden::check(file, saved(std::filesystem::path(FBENGINE_EXAMPLE_FRAMES) /
                                           "models" / (file + ".png")));
        }
    }
    stop_gpu();
}
