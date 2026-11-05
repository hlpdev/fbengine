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

// Khronos's sample models, as tests/khronos saves them, which holds them to more than this. Golden
// images are Mesa 26.2.4's, and Linux's CI has Ubuntu's 25.2.8, whose vkCmdBlitImage, what makes
// mipmaps, rounds an 8-bit linear format's half a 255th high at each level, though sRGB's are the
// same. So a normal or metal-rough map differs from far off, moving a floor's highlights and a
// reflected horizon. On Linux, a frame with those can differ by the most CI measures there, past
// linux-arm64's 1.39%, 0.44%, 0.06%, and 0.07% of its pixels, and everywhere else not at all.
TEST_CASE("Khronos's sample models' frames match their golden images", "[.example]") {
#ifdef __linux__
    constexpr bool older_mesa = true;
#else
    constexpr bool older_mesa = false;
#endif
    struct model_frame_t {
        std::string_view frame;
        double share_on_older_mesa = 0.0;
    };
    REQUIRE(fbe_window_set_headless(true));
    require(fbe_gpu_start());
    for (const auto& [frame, share_on_older_mesa] : {
             model_frame_t{ .frame = "AlphaBlendModeTest", .share_on_older_mesa = 0.015 },
             model_frame_t{ .frame = "EmissiveStrengthTest" },
             model_frame_t{ .frame = "MetalRoughSpheres" },
             model_frame_t{ .frame = "MetalRoughSpheresNoTextures" },
             model_frame_t{ .frame = "NormalTangentMirrorTest", .share_on_older_mesa = 0.005 },
             model_frame_t{ .frame = "NormalTangentTest", .share_on_older_mesa = 0.001 },
             model_frame_t{ .frame = "TextureLinearInterpolationTest" },
             model_frame_t{ .frame = "TextureSettingsTest" },
             model_frame_t{ .frame = "TextureTransformMultiTest", .share_on_older_mesa = 0.001 },
             model_frame_t{ .frame = "TextureTransformTest" },
             model_frame_t{ .frame = "Worked" },
         }) {
        INFO("frame " << frame);
        const std::filesystem::path path = std::filesystem::path(FBENGINE_EXAMPLE_FRAMES) /
                                           "khronos" / std::format("frame_{}.png", frame);
        fbe::golden::check(std::format("khronos_{}", frame), saved(path),
                           { .channel = 2, .share = older_mesa ? share_on_older_mesa : 0.0 });
    }
    stop_gpu();
}

// The editor's panes, and what opens over them, as `--save` saves them. Its grid's lines are
// thinner than a pixel far off, so which pixels they cover is a near thing: at one size of its
// view, lavapipe on Linux covered 2 others than it does on Windows, from the same camera, whatever
// its Mesa or the natives' build. So 8 of a frame's pixels can differ.
TEST_CASE("the editor's frames match their golden images", "[.example]") {
    constexpr fbe::golden::tolerance_t near_thing{ .channel = 2, .share = 8.0 / (1280.0 * 720.0) };
    REQUIRE(fbe_window_set_headless(true));
    require(fbe_gpu_start());
    for (const std::string_view frame : {
             "welcome",
             "nothing_selected",
             "move",
             "turn",
             "scale",
             "inspector",
             "menu",
             "dialog",
         }) {
        INFO("frame " << frame);
        const std::filesystem::path path = std::filesystem::path(FBENGINE_EXAMPLE_FRAMES) /
                                           "editor" / std::format("frame_{}.png", frame);
        fbe::golden::check(std::format("editor_{}", frame), saved(path), near_thing);
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
