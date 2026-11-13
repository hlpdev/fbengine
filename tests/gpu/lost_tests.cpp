// A GPU lost by fbe_gpu_simulate_loss, as a game's tests lose it: every public call that would
// reach the device refuses, saying the GPU was lost, or does nothing, and none asks anything of
// the driver, so a test of a game under a loss sees what a real one would do.

#include "fbengine.h"
#include "gpu/vulkan.hpp"
#include "gpu_testing.hpp"
#include "test_shaders.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using fbe::tests::require;
using fbe::tests::stop_gpu;

namespace {

/// A file as CookShader makes of a game's, of `words`.
std::vector<std::uint8_t> cooked_shader(std::span<const std::uint32_t> words) {
    const std::string_view magic = "FBESHADR";
    const std::string_view version = fbe_version();
    const std::string_view stamp = fbe_shader_stamp();
    std::vector<std::uint8_t> bytes(magic.begin(), magic.end());
    const auto length = static_cast<std::uint32_t>(version.size());
    bytes.resize(bytes.size() + sizeof(length));
    std::memcpy(bytes.data() + magic.size(), &length, sizeof(length));
    bytes.insert(bytes.end(), version.begin(), version.end());
    bytes.insert(bytes.end(), stamp.begin(), stamp.end());
    const std::size_t at = bytes.size();
    bytes.resize(at + words.size_bytes());
    std::memcpy(bytes.data() + at, words.data(), words.size_bytes());
    return bytes;
}

/// What a call does while the GPU's lost, as its docs say: refuses, saying so, or does nothing.
enum class lost_t : std::uint8_t {
    REFUSES,
    NOTHING,
};

/// A public call that reaches the device, and what it does while the GPU's lost.
struct call_t {
    std::string name;
    lost_t does;
    std::function<bool()> call;
};

std::string reached(const fbe::gpu::reached_t& reached) {
    return "allocations " + std::to_string(reached.allocations) + ", submits " +
           std::to_string(reached.submits) + ", pipelines " + std::to_string(reached.pipelines) +
           ", recordings " + std::to_string(reached.recordings);
}

} // namespace

TEST_CASE("a GPU lost for a test asks nothing of the driver, and each call refuses or does nothing",
          "[gpu][stability]") {
    REQUIRE(fbe_window_set_headless(true));
    fbe_gpu_set_pipeline_cache("");
    require(fbe_window_open(64, 32, FBE_WINDOW_WINDOWED));
    require(fbe_gpu_start());
    // What the calls draw with, made and drawn once while the GPU's whole.
    const std::uint64_t texture = fbe_texture_create(8, 8, FBE_TEXTURE_RGBA8, false);
    require(texture != 0);
    const std::uint64_t mipmapped = fbe_texture_create(8, 8, FBE_TEXTURE_RGBA8, true);
    require(mipmapped != 0);
    const std::uint64_t floats = fbe_texture_create(4, 2, FBE_TEXTURE_RGBA16F, false);
    require(floats != 0);
    const std::uint64_t target = fbe_texture_create_target(16, 16, FBE_TEXTURE_RGBA8, 1, true);
    require(target != 0);
    const std::uint64_t font = fbe_text_embedded_font(12.0F, 0);
    require(font != 0);
    const std::vector<std::uint8_t> shader_bytes =
        cooked_shader(fbe::test_shaders::game_shader.code);
    const auto shader_size = static_cast<std::int64_t>(shader_bytes.size());
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t camera = fbe_entity_create(world, 0, FBE_ENTITY_CAMERA);
    require(fbe_camera_set_target(camera, target));
    const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    const std::uint64_t cube = fbe_geometry_cube();
    require(fbe_mesh_set_geometry(mesh, cube));
    require(fbe_entity_set_position(mesh, 0.0F, 0.0F, 4.0F, FBE_SPACE_WORLD));
    require(fbe_render_world(world, 1.0F));
    fbe_frame_flip(false);
    // And what's new to the GPU once it's lost: a material of a shader it hasn't made a pipeline
    // of, a geometry it hasn't uploaded, and glyphs it hasn't drawn.
    const std::uint64_t shaded = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    const std::uint64_t material = fbe_material_create();
    const std::uint64_t sphere = fbe_geometry_sphere(8);
    require(fbe_mesh_set_geometry(shaded, sphere));
    require(fbe_mesh_set_material(shaded, material));
    require(fbe_entity_set_position(shaded, 1.0F, 0.0F, 4.0F, FBE_SPACE_WORLD));
    const std::array<std::uint8_t, 256> pixels{};
    std::array<std::uint8_t, 256> read{};
    const std::array<float, 32> float_pixels{};
    const std::array<float, 6> triangle{ 0.0F, 0.0F, 8.0F, 0.0F, 0.0F, 8.0F };
    const fbe_2d_pen_t ink{ .red = 255, .green = 255, .blue = 255, .alpha = 255 };
    const fbe_2d_sprite_t sprite{ .scale_x = 1.0F, .scale_y = 1.0F };
    const std::int64_t presented = fbe_gpu_presented();
    std::uint64_t made_shader = 0;

    const std::vector<call_t> calls{
        {
            .name = "fbe_texture_create",
            .does = lost_t::REFUSES,
            .call = [] { return fbe_texture_create(8, 8, FBE_TEXTURE_RGBA8, false) != 0; },
        },
        {
            .name = "fbe_texture_create_target",
            .does = lost_t::REFUSES,
            .call = [] { return fbe_texture_create_target(8, 8, FBE_TEXTURE_RGBA8, 4, true) != 0; },
        },
        {
            .name = "fbe_texture_write",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_texture_write(texture, 0, 0, 8, 8, pixels.data(), 32); },
        },
        {
            .name = "fbe_texture_write of a mipmapped texture",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_texture_write(mipmapped, 0, 0, 8, 8, pixels.data(), 32); },
        },
        {
            .name = "fbe_texture_write_floats",
            .does = lost_t::NOTHING,
            .call =
                [&] { return fbe_texture_write_floats(floats, 0, 0, 4, 2, float_pixels.data()); },
        },
        {
            .name = "fbe_texture_write_level",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_texture_write_level(mipmapped, 1, 4, 4, pixels.data(), 16); },
        },
        {
            .name = "fbe_texture_clear",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_texture_clear(texture, 1.0F, 0.0F, 0.0F, 1.0F); },
        },
        {
            .name = "fbe_texture_copy",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_texture_copy(target, 0, 0, texture, 0, 0, 8, 8); },
        },
        {
            .name = "fbe_texture_read",
            .does = lost_t::REFUSES,
            .call = [&] { return fbe_texture_read(texture, 0, read.data(), read.size()); },
        },
        {
            .name = "fbe_texture_pixels",
            .does = lost_t::REFUSES,
            .call =
                [&] {
                    std::int64_t size = 0;
                    return fbe_texture_pixels(mipmapped, &size) != nullptr;
                },
        },
        {
            .name = "fbe_texture_pixels_written",
            .does = lost_t::NOTHING,
            .call =
                [&] {
                    std::int64_t size = 0;
                    return fbe_texture_pixels(texture, &size) == nullptr ||
                           fbe_texture_pixels_written(texture, 0, 0, 8, 8);
                },
        },
        {
            .name = "fbe_texture_set_filter",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_texture_set_filter(texture, FBE_TEXTURE_NEAREST); },
        },
        {
            .name = "fbe_2d_clear",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_clear(target, &ink); },
        },
        {
            .name = "fbe_2d_plot",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_plot(target, &ink, 1, 1); },
        },
        {
            .name = "fbe_2d_line",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_line(target, &ink, 0, 0, 8, 8); },
        },
        {
            .name = "fbe_2d_rect",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_rect(target, &ink, 0, 0, 8, 8, true); },
        },
        {
            .name = "fbe_2d_oval",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_oval(target, &ink, 0, 0, 8, 8, true); },
        },
        {
            .name = "fbe_2d_polygon",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_polygon(target, &ink, triangle.data(), 3, true); },
        },
        {
            .name = "fbe_2d_texture",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_texture(target, &ink, texture, &sprite, 0.0F, 0.0F); },
        },
        {
            .name = "fbe_2d_tile",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_tile(target, &ink, texture, &sprite, 0.0F, 0.0F); },
        },
        {
            .name = "fbe_2d_rect on the screen",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_rect(0, &ink, 0, 0, 8, 8, true); },
        },
        {
            .name = "fbe_2d_text",
            .does = lost_t::REFUSES,
            .call = [&] { return fbe_2d_text(target, &ink, font, "Lost", 0, 0); },
        },
        {
            .name = "fbe_text_embedded_font",
            .does = lost_t::NOTHING,
            .call =
                [&] {
                    const std::uint64_t made = fbe_text_embedded_font(20.0F, 0);
                    return made != 0 && fbe_text_free_font(made);
                },
        },
        {
            .name = "fbe_shader_create",
            .does = lost_t::NOTHING,
            .call =
                [&] {
                    made_shader =
                        fbe_shader_create(shader_bytes.data(), shader_size, "lost_shader");
                    return made_shader != 0 && fbe_material_set_shader(material, made_shader);
                },
        },
        {
            .name = "fbe_render_world",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_render_world(world, 1.0F); },
        },
        {
            .name = "fbe_environment_bake",
            .does = lost_t::REFUSES,
            .call = [&] { return fbe_environment_bake(floats, 16, 4) != 0; },
        },
        {
            .name = "fbe_window_set_size",
            .does = lost_t::NOTHING,
            .call = [] { return fbe_window_set_size(48, 24); },
        },
        {
            .name = "fbe_frame_flip",
            .does = lost_t::NOTHING,
            .call =
                [&] {
                    fbe_frame_flip(false);
                    return fbe_gpu_presented() == presented;
                },
        },
        {
            .name = "fbe_texture_release",
            .does = lost_t::NOTHING,
            .call =
                [&] {
                    const bool released = fbe_texture_release(mipmapped);
                    fbe_frame_flip(false);
                    return released;
                },
        },
    };

    CHECK(fbe_gpu_simulate_loss());
    for (const call_t& each : calls) {
        CAPTURE(each.name);
        // fbe_error says something else first, so what it says after is the call's.
        CHECK_FALSE(fbe_texture_retain(0));
        const fbe::gpu::reached_t before = fbe::gpu::reached();
        const bool worked = each.call();
        const fbe::gpu::reached_t after = fbe::gpu::reached();
        INFO("asked of the driver before: " << reached(before) << "; after: " << reached(after));
        CHECK(after == before);
        if (each.does == lost_t::REFUSES) {
            CHECK_FALSE(worked);
            CHECK(std::string_view(fbe_error()).starts_with("the GPU was lost"));
        } else {
            INFO(fbe_error());
            CHECK(worked);
        }
    }
    CHECK(fbe_gpu_lost());
    CHECK(fbe_gpu_presented() == presented);

    // Stopped and started, everything's made again, and draws.
    if (made_shader != 0) {
        fbe_shader_release(made_shader);
    }
    fbe_world_free(world);
    std::array<std::uint64_t, 64> some{};
    while (fbe_entity_freed(some.data(), some.size()) != 0) {}
    fbe_material_release(material);
    fbe_geometry_release(cube);
    fbe_geometry_release(sphere);
    fbe_text_free_font(font);
    fbe_texture_release(texture);
    fbe_texture_release(floats);
    fbe_texture_release(target);
    stop_gpu();
    require(fbe_gpu_start());
    CHECK_FALSE(fbe_gpu_lost());
    const fbe::gpu::reached_t restarted = fbe::gpu::reached();
    const std::uint64_t again = fbe_texture_create_target(8, 8, FBE_TEXTURE_RGBA8, 1, false);
    require(again != 0);
    const fbe_2d_pen_t red{ .red = 255, .alpha = 255 };
    require(fbe_2d_clear(again, &red));
    require(fbe_texture_read(again, 0, read.data(), read.size()));
    CHECK(read[0] == 255);
    CHECK(read[1] == 0);
    const std::int64_t shown = fbe_gpu_presented();
    fbe_frame_flip(false);
    CHECK(fbe_gpu_presented() == shown + 1);
    CHECK(fbe::gpu::reached().submits > restarted.submits);
    CHECK(fbe_texture_release(again));
    stop_gpu();
    fbe_window_close();
}
