// A GPU lost by fbe_gpu_simulate_loss, as a game's tests lose it: every public call that would
// reach the device refuses, saying the GPU was lost, or does nothing, and none asks anything of
// the driver, so a test of a game under a loss sees what a real one would do. The table is every
// such call: the headers whose calls can reach the GPU are read, and each of their exports is a
// row or one that never asks the driver anything.

#include "assets/image.hpp"
#include "assets/loads.hpp"
#include "fbengine.h"
#include "gpu/vulkan.hpp"
#include "gpu_testing.hpp"
#include "test_shaders.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
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

/// A public call that reaches the device, named for its export and what's particular to the row,
/// what it does while the GPU's lost, and, if it has one, a misuse of it, which is refused as it
/// is on a GPU that isn't lost.
struct call_t {
    std::string name;
    lost_t does;
    std::function<bool()> call;
    std::function<bool()> misuse;
};

/// The areas whose calls can reach the GPU, each of whose exports is a row or one of
/// `asks_nothing_names`.
/// The rest never do: app, clip, collide, core, input, jobs, morph, player, skin, and vfs never
/// touch it; assets asks only what formats the device samples, which a lost one answers as it
/// did; scene keeps what's drawn, and the textures and shaders it lets go of are freed as
/// fbe_texture_release's are, at a flip, that row; and text lays out on the CPU, its atlas's
/// pages reaching the GPU only as fbe_2d_text draws them, that row.
constexpr std::string_view reaching_areas =
    "2d environment frame gpu model render shader texture window";

/// The exports of those areas that never ask the driver anything: what they say is kept on the
/// CPU, and what they set is used as the GPU draws next. And fbe_gpu_stop, which is how a lost
/// GPU's made again.
constexpr std::string_view asks_nothing_names =
    "fbe_environment_count fbe_environment_irradiance fbe_environment_levels "
    "fbe_environment_specular fbe_environment_table fbe_frame_count fbe_frame_delta "
    "fbe_frame_fixed_delta fbe_frame_limit fbe_frame_rate fbe_frame_set_fixed_delta "
    "fbe_frame_set_limit fbe_frame_vsync fbe_gpu_anisotropy "
    "fbe_gpu_counters fbe_gpu_device fbe_gpu_device_kind fbe_gpu_device_lacks fbe_gpu_device_name "
    "fbe_gpu_disabled fbe_gpu_lod_bias fbe_gpu_lost fbe_gpu_max_anisotropy fbe_gpu_pipeline_cache "
    "fbe_gpu_pipelines fbe_gpu_presented fbe_gpu_render_height fbe_gpu_render_samples "
    "fbe_gpu_render_width fbe_gpu_scaling fbe_gpu_scaling_smooth fbe_gpu_screen_area "
    "fbe_gpu_set_device fbe_gpu_set_disabled fbe_gpu_set_pipeline_cache fbe_gpu_set_scaling "
    "fbe_gpu_started fbe_gpu_stop fbe_gpu_stops fbe_gpu_swapchain_headless "
    "fbe_gpu_swapchain_height fbe_gpu_swapchain_vsync fbe_gpu_swapchain_width fbe_gpu_validating "
    "fbe_gpu_validation_errors fbe_gpu_waiting_frees fbe_model_drop_images fbe_model_geometry "
    "fbe_model_image_levels fbe_model_image_name fbe_model_invalid fbe_model_material "
    "fbe_model_material_image fbe_model_material_name fbe_model_material_same fbe_model_scene "
    "fbe_model_scene_count fbe_model_spawn fbe_render_draw_lights fbe_render_draws "
    "fbe_render_lights fbe_render_set_shadow_atlas_limit fbe_render_shadow_atlas_limit "
    "fbe_material_parameter fbe_material_set_parameter fbe_material_set_shader fbe_material_shader "
    "fbe_shader_alive fbe_shader_check fbe_shader_is_pass fbe_shader_path fbe_shader_retain "
    "fbe_shader_stamp fbe_texture_alpha fbe_texture_count fbe_texture_filter fbe_texture_format "
    "fbe_texture_has_depth fbe_texture_height fbe_texture_is_target fbe_texture_levels "
    "fbe_texture_load_invalid fbe_texture_path fbe_texture_pixels_stamp fbe_texture_retain "
    "fbe_texture_samples fbe_texture_set_alpha fbe_texture_set_filter fbe_texture_set_path "
    "fbe_texture_set_wrap fbe_texture_srgb fbe_texture_vulkan_format fbe_texture_width "
    "fbe_texture_wrap_u fbe_texture_wrap_v fbe_display_count fbe_display_desktop "
    "fbe_display_mode_at fbe_display_mode_count fbe_display_name fbe_window_closed "
    "fbe_window_focused fbe_window_headless fbe_window_height fbe_window_is_open "
    "fbe_window_keep_open fbe_window_minimized fbe_window_minimum_height fbe_window_minimum_width "
    "fbe_window_mode fbe_window_pixel_height fbe_window_pixel_width fbe_window_place_shows "
    "fbe_window_resizable fbe_window_scale fbe_window_set_headless fbe_window_set_minimum_size "
    "fbe_window_set_mode fbe_window_set_position fbe_window_set_resizable fbe_window_set_title "
    "fbe_window_simulate fbe_window_title fbe_window_width fbe_window_x fbe_window_y";

/// The words of `listed`, one by one.
std::set<std::string, std::less<>> words_of(std::string_view listed) {
    std::set<std::string, std::less<>> names;
    std::string_view left = listed;
    while (!left.empty()) {
        const std::size_t end = std::min(left.find(' '), left.size());
        names.emplace(left.substr(0, end));
        left.remove_prefix(std::min(end + 1, left.size()));
    }
    return names;
}

/// The functions an area's header exports, as its FBE_API lines name them.
std::vector<std::string> exports_of(std::string_view area) {
    const std::filesystem::path header = std::filesystem::path(FBENGINE_NATIVE) /
                                         std::string(area) / ("fbe_" + std::string(area) + ".h");
    std::ifstream file(header);
    INFO(header.string());
    REQUIRE(file.is_open());
    std::vector<std::string> names;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.starts_with("FBE_API ")) {
            continue;
        }
        const std::size_t open = line.find('(');
        const std::size_t name = line.rfind(' ', open);
        REQUIRE(open != std::string::npos);
        REQUIRE(name != std::string::npos);
        // A pointer's star goes with the type.
        std::string found = line.substr(name + 1, open - name - 1);
        std::erase(found, '*');
        names.push_back(std::move(found));
    }
    return names;
}

/// The export a row's named for, its name's first word.
std::string_view export_of(std::string_view name) {
    return name.substr(0, name.find(' '));
}

std::string reached(const fbe::gpu::reached_t& reached) {
    return "allocations " + std::to_string(reached.allocations) + ", submits " +
           std::to_string(reached.submits) + ", pipelines " + std::to_string(reached.pipelines) +
           ", recordings " + std::to_string(reached.recordings) + ", samplers " +
           std::to_string(reached.samplers) + ", views " + std::to_string(reached.views) +
           ", descriptor writes " + std::to_string(reached.descriptor_writes) + ", waits " +
           std::to_string(reached.waits);
}

/// A load of a 4x4 sRGB RGBA8 image of `bytes`, done, as a file's would be.
std::uint64_t image_load(const std::vector<std::uint8_t>& bytes) {
    auto load = std::make_shared<fbe::assets::load_t>();
    load->kind = fbe::assets::load_kind_t::IMAGE;
    load->image = {
        .format = fbe::assets::formats::rgba8_srgb,
        .width = 4,
        .height = 4,
        .levels = { { .width = 4, .height = 4, .size = bytes.size() } },
        .texels = fbe::assets::texels_t(nullptr, std::span(bytes)),
    };
    load->done = true;
    return fbe::assets::loads().add(std::move(load));
}

/// A load of a model of that image and a material whose base color reads it.
std::uint64_t model_load(const std::vector<std::uint8_t>& bytes) {
    auto load = std::make_shared<fbe::assets::load_t>();
    load->kind = fbe::assets::load_kind_t::MODEL;
    load->model.images.push_back({
        .format = fbe::assets::formats::rgba8_srgb,
        .width = 4,
        .height = 4,
        .levels = { { .width = 4, .height = 4, .size = bytes.size() } },
        .texels = fbe::assets::texels_t(nullptr, std::span(bytes)),
    });
    fbe::assets::material_t material;
    material.base_color_texture.image = 0;
    load->model.materials.push_back(material);
    load->done = true;
    return fbe::assets::loads().add(std::move(load));
}

/// Whether fbe_error says the GPU was lost.
bool says_lost() {
    return std::string_view(fbe_error()).starts_with("the GPU was lost");
}

/// A fixed delta, taken away as its scope ends, however it ends.
struct fixed_delta_t {
    explicit fixed_delta_t(double seconds) {
        require(fbe_frame_set_fixed_delta(seconds));
    }
    fixed_delta_t(const fixed_delta_t&) = delete;
    fixed_delta_t& operator=(const fixed_delta_t&) = delete;
    fixed_delta_t(fixed_delta_t&&) = delete;
    fixed_delta_t& operator=(fixed_delta_t&&) = delete;
    ~fixed_delta_t() {
        fbe_frame_set_fixed_delta(0.0);
    }
};

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
    const std::uint64_t multisampled = fbe_texture_create_target(8, 8, FBE_TEXTURE_RGBA8, 4, false);
    require(multisampled != 0);
    // Its pixels on the CPU, so a write of them is the GPU's to send.
    const std::uint64_t locked = fbe_texture_create(8, 8, FBE_TEXTURE_RGBA8, false);
    require(locked != 0);
    require(fbe_texture_pixels(locked, nullptr) != nullptr);
    const std::uint64_t font = fbe_text_embedded_font(12.0F, 0);
    require(font != 0);
    const std::vector<std::uint8_t> shader_bytes =
        cooked_shader(fbe::test_shaders::game_shader.code);
    const auto shader_size = static_cast<std::int64_t>(shader_bytes.size());
    const std::uint64_t shader = fbe_shader_create(shader_bytes.data(), shader_size, "shader");
    require(shader != 0);
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t camera = fbe_entity_create(world, 0, FBE_ENTITY_CAMERA);
    require(fbe_camera_set_target(camera, target));
    const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    const std::uint64_t cube = fbe_geometry_cube();
    require(fbe_mesh_set_geometry(mesh, cube));
    require(fbe_entity_set_position(mesh, 0.0F, 0.0F, 4.0F, FBE_SPACE_WORLD));
    require(fbe_render_world(world, 1.0F));
    const std::uint64_t environment = fbe_environment_bake(floats, 16, 1);
    require(environment != 0);
    fbe_frame_flip(false);
    // An image and a model of one, whose texture's sampled otherwise than its material says, so
    // the material reads a texture of its own.
    const std::vector<std::uint8_t> image_bytes(64, 0x80);
    const std::uint64_t image = image_load(image_bytes);
    const std::uint64_t model = model_load(image_bytes);
    const std::uint64_t model_material = fbe_model_material(model, 0);
    require(model_material != 0);
    const std::uint64_t model_texture = fbe_model_texture(model, 0, false);
    require(model_texture != 0);
    require(fbe_texture_set_filter(model_texture, FBE_TEXTURE_NEAREST));
    require(fbe_vfs_mount_folder("res://lost-images", FBENGINE_GOLDEN_IMAGES));
    const std::uint64_t mount = fbe_vfs_last_mount();
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
    std::array<std::uint64_t, FBE_MODEL_MAPS> others{};
    std::array<std::int32_t, FBE_MODEL_MAPS> other_images{};
    std::int32_t others_made = 0;

    const std::vector<call_t> calls{
        {
            .name = "fbe_texture_create",
            .does = lost_t::REFUSES,
            .call = [] { return fbe_texture_create(8, 8, FBE_TEXTURE_RGBA8, false) != 0; },
            .misuse = [] { return fbe_texture_create(0, 8, FBE_TEXTURE_RGBA8, false) != 0; },
        },
        {
            .name = "fbe_texture_create_target",
            .does = lost_t::REFUSES,
            .call = [] { return fbe_texture_create_target(8, 8, FBE_TEXTURE_RGBA8, 4, true) != 0; },
            .misuse =
                [] { return fbe_texture_create_target(8, 8, FBE_TEXTURE_RGBA8, 3, true) != 0; },
        },
        {
            .name = "fbe_texture_write",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_texture_write(texture, 0, 0, 8, 8, pixels.data(), 32); },
            .misuse = [&] { return fbe_texture_write(texture, 0, 0, 8, 8, pixels.data(), 4); },
        },
        {
            .name = "fbe_texture_write of a mipmapped texture",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_texture_write(mipmapped, 0, 0, 8, 8, pixels.data(), 32); },
            .misuse = [&] { return fbe_texture_write(mipmapped, 4, 4, 8, 8, pixels.data(), 32); },
        },
        {
            .name = "fbe_texture_write_floats",
            .does = lost_t::NOTHING,
            .call =
                [&] { return fbe_texture_write_floats(floats, 0, 0, 4, 2, float_pixels.data()); },
            .misuse =
                [&] { return fbe_texture_write_floats(texture, 0, 0, 4, 2, float_pixels.data()); },
        },
        {
            .name = "fbe_texture_write_level",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_texture_write_level(mipmapped, 1, 4, 4, pixels.data(), 16); },
            .misuse =
                [&] { return fbe_texture_write_level(mipmapped, 9, 4, 4, pixels.data(), 16); },
        },
        {
            .name = "fbe_texture_clear",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_texture_clear(texture, 1.0F, 0.0F, 0.0F, 1.0F); },
            .misuse = [] { return fbe_texture_clear(0, 1.0F, 0.0F, 0.0F, 1.0F); },
        },
        {
            .name = "fbe_texture_copy",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_texture_copy(target, 0, 0, texture, 0, 0, 8, 8); },
            .misuse = [&] { return fbe_texture_copy(target, 0, 0, floats, 0, 0, 4, 2); },
        },
        {
            .name = "fbe_texture_read",
            .does = lost_t::REFUSES,
            .call = [&] { return fbe_texture_read(texture, 0, read.data(), read.size()); },
            .misuse = [&] { return fbe_texture_read(texture, 0, read.data(), 4); },
        },
        {
            .name = "fbe_texture_pixels",
            .does = lost_t::REFUSES,
            .call =
                [&] {
                    std::int64_t size = 0;
                    return fbe_texture_pixels(mipmapped, &size) != nullptr;
                },
            .misuse = [&] { return fbe_texture_pixels(multisampled, nullptr) != nullptr; },
        },
        {
            .name = "fbe_texture_pixels_written",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_texture_pixels_written(locked, 0, 0, 8, 8); },
            .misuse = [&] { return fbe_texture_pixels_written(locked, -1, 0, 8, 8); },
        },
        {
            .name = "fbe_texture_from_load",
            .does = lost_t::REFUSES,
            .call = [&] { return fbe_texture_from_load(image, true, false) != 0; },
            .misuse = [&] { return fbe_texture_from_load(model, true, false) != 0; },
        },
        {
            .name = "fbe_texture_from_model",
            .does = lost_t::REFUSES,
            .call = [&] { return fbe_texture_from_model(model, 0, true, false) != 0; },
            .misuse = [&] { return fbe_texture_from_model(model, 5, true, false) != 0; },
        },
        {
            .name = "fbe_texture_reload",
            .does = lost_t::REFUSES,
            .call = [&] { return fbe_texture_reload(texture, image, true, false); },
            .misuse = [&] { return fbe_texture_reload(target, image, true, false); },
        },
        {
            .name = "fbe_texture_start_loads",
            .does = lost_t::NOTHING,
            .call = [] { return fbe_texture_start_loads(); },
        },
        {
            .name = "fbe_model_texture",
            .does = lost_t::REFUSES,
            .call = [&] { return fbe_model_texture(model, 0, false) != 0; },
            .misuse = [&] { return fbe_model_texture(model, 5, false) != 0; },
        },
        {
            .name = "fbe_model_image_texture",
            .does = lost_t::REFUSES,
            .call =
                [] {
                    return fbe_model_image_texture("res://lost-images/anim_arm_end.png", true,
                                                   FBE_TEXTURE_LINEAR, FBE_TEXTURE_REPEAT,
                                                   FBE_TEXTURE_REPEAT, true) != 0;
                },
            .misuse =
                [] {
                    return fbe_model_image_texture(nullptr, true, FBE_TEXTURE_LINEAR,
                                                   FBE_TEXTURE_REPEAT, FBE_TEXTURE_REPEAT,
                                                   true) != 0;
                },
        },
        {
            .name = "fbe_model_material_textures",
            .does = lost_t::REFUSES,
            .call =
                [&] {
                    return fbe_model_material_textures(model, 0, model_material, &model_texture, 1,
                                                       others.data(), other_images.data(),
                                                       FBE_MODEL_MAPS, &others_made);
                },
            .misuse =
                [&] {
                    return fbe_model_material_textures(model, 3, model_material, &model_texture, 1,
                                                       others.data(), other_images.data(),
                                                       FBE_MODEL_MAPS, &others_made);
                },
        },
        {
            .name = "fbe_2d_clear",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_clear(target, &ink); },
            .misuse = [&] { return fbe_2d_clear(target, nullptr); },
        },
        {
            .name = "fbe_2d_plot",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_plot(target, &ink, 1, 1); },
            .misuse = [&] { return fbe_2d_plot(target, nullptr, 1, 1); },
        },
        {
            .name = "fbe_2d_line",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_line(target, &ink, 0, 0, 8, 8); },
            .misuse = [&] { return fbe_2d_line(target, nullptr, 0, 0, 8, 8); },
        },
        {
            .name = "fbe_2d_rect",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_rect(target, &ink, 0, 0, 8, 8, true); },
            .misuse = [&] { return fbe_2d_rect(texture, &ink, 0, 0, 8, 8, true); },
        },
        {
            .name = "fbe_2d_oval",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_oval(target, &ink, 0, 0, 8, 8, true); },
            .misuse = [&] { return fbe_2d_oval(target, nullptr, 0, 0, 8, 8, true); },
        },
        {
            .name = "fbe_2d_polygon",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_polygon(target, &ink, triangle.data(), 3, true); },
            .misuse = [&] { return fbe_2d_polygon(target, &ink, triangle.data(), 2, true); },
        },
        {
            .name = "fbe_2d_texture",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_texture(target, &ink, texture, &sprite, 0.0F, 0.0F); },
            .misuse = [&] { return fbe_2d_texture(target, &ink, 0, &sprite, 0.0F, 0.0F); },
        },
        {
            .name = "fbe_2d_tile",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_tile(target, &ink, texture, &sprite, 0.0F, 0.0F); },
            .misuse = [&] { return fbe_2d_tile(target, &ink, 0, &sprite, 0.0F, 0.0F); },
        },
        {
            .name = "fbe_2d_rect on the screen",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_rect(0, &ink, 0, 0, 8, 8, true); },
        },
        {
            .name = "fbe_2d_text",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_text(target, &ink, font, "Lost", 0, 0); },
            .misuse = [&] { return fbe_2d_text(target, &ink, font, nullptr, 0, 0); },
        },
        {
            .name = "fbe_2d_text again, its misuse a font that isn't one",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_2d_text(target, &ink, font, "Lost too", 0, 12); },
            .misuse = [&] { return fbe_2d_text(target, &ink, 0, "Lost", 0, 0); },
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
            .misuse = [] { return fbe_shader_create(nullptr, 0, "nothing") != 0; },
        },
        {
            .name = "fbe_shader_replace",
            .does = lost_t::NOTHING,
            .call =
                [&] {
                    return fbe_shader_replace(shader, shader_bytes.data(), shader_size, "again");
                },
            .misuse = [&] { return fbe_shader_replace(shader, nullptr, 0, "nothing"); },
        },
        {
            .name = "fbe_shader_release",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_shader_release(shader); },
            .misuse = [] { return fbe_shader_release(0); },
        },
        {
            .name = "fbe_render_camera",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_render_camera(camera, 1.0F); },
            .misuse = [&] { return fbe_render_camera(camera, 2.0F); },
        },
        {
            .name = "fbe_render_world",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_render_world(world, 1.0F); },
            .misuse = [] { return fbe_render_world(0, 1.0F); },
        },
        {
            .name = "fbe_environment_bake",
            .does = lost_t::REFUSES,
            .call = [&] { return fbe_environment_bake(floats, 16, 4) != 0; },
            .misuse = [&] { return fbe_environment_bake(floats, 12, 4) != 0; },
        },
        {
            .name = "fbe_environment_bake again, its misuse a source that isn't one",
            .does = lost_t::REFUSES,
            .call = [&] { return fbe_environment_bake(floats, 32, 1) != 0; },
            .misuse = [] { return fbe_environment_bake(0, 16, 4) != 0; },
        },
        {
            .name = "fbe_environment_free",
            .does = lost_t::NOTHING,
            .call = [&] { return fbe_environment_free(environment); },
            .misuse = [] { return fbe_environment_free(0); },
        },
        {
            .name = "fbe_gpu_device_count",
            .does = lost_t::NOTHING,
            .call = [] { return fbe_gpu_device_count() > 0; },
        },
        {
            .name = "fbe_gpu_start",
            .does = lost_t::NOTHING,
            .call = [] { return fbe_gpu_start(); },
        },
        {
            .name = "fbe_gpu_simulate_loss",
            .does = lost_t::NOTHING,
            .call = [] { return fbe_gpu_simulate_loss(0); },
        },
        {
            .name = "fbe_gpu_memory_budget",
            .does = lost_t::NOTHING,
            .call = [] { return fbe_gpu_memory_budget() > 0; },
        },
        {
            .name = "fbe_gpu_memory_usage",
            .does = lost_t::NOTHING,
            .call = [] { return fbe_gpu_memory_usage() >= 0; },
        },
        {
            .name = "fbe_gpu_set_lod_bias",
            .does = lost_t::NOTHING,
            .call = [] { return fbe_gpu_set_lod_bias(1.0F); },
            .misuse = [] { return fbe_gpu_set_lod_bias(20.0F); },
        },
        {
            .name = "fbe_gpu_set_anisotropy",
            .does = lost_t::NOTHING,
            .call = [] { return fbe_gpu_set_anisotropy(2.0F); },
            .misuse = [] { return fbe_gpu_set_anisotropy(0.5F); },
        },
        {
            .name = "fbe_gpu_screen",
            .does = lost_t::NOTHING,
            .call = [] { return fbe_gpu_screen() != 0; },
        },
        {
            .name = "fbe_gpu_set_render_size",
            .does = lost_t::NOTHING,
            .call = [] { return fbe_gpu_set_render_size(32, 16); },
            .misuse = [] { return fbe_gpu_set_render_size(-1, 16); },
        },
        {
            .name = "fbe_gpu_set_render_samples",
            .does = lost_t::NOTHING,
            .call = [] { return fbe_gpu_set_render_samples(4); },
            .misuse = [] { return fbe_gpu_set_render_samples(3); },
        },
        {
            .name = "fbe_window_set_size",
            .does = lost_t::NOTHING,
            .call = [] { return fbe_window_set_size(48, 24); },
            .misuse = [] { return fbe_window_set_size(0, 24); },
        },
        {
            .name = "fbe_window_close",
            .does = lost_t::NOTHING,
            .call =
                [] {
                    fbe_window_close();
                    return !fbe_window_is_open();
                },
        },
        {
            .name = "fbe_window_open",
            .does = lost_t::NOTHING,
            .call = [] { return fbe_window_open(64, 32, FBE_WINDOW_WINDOWED); },
            .misuse = [] { return fbe_window_open(0, 32, FBE_WINDOW_WINDOWED); },
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
            .misuse = [] { return fbe_texture_release(0); },
        },
    };

    // The table is every call that can reach the GPU: each export of the areas whose calls can is
    // a row, or never asks the driver anything.
    std::set<std::string, std::less<>> rowed;
    for (const call_t& each : calls) {
        rowed.emplace(export_of(each.name));
    }
    const std::set<std::string, std::less<>> quiet = words_of(asks_nothing_names);
    std::size_t exports = 0;
    for (const std::string& area : words_of(reaching_areas)) {
        for (const std::string& name : exports_of(area)) {
            CAPTURE(name);
            ++exports;
            CHECK(rowed.contains(name) != quiet.contains(name));
        }
    }
    CHECK(exports == 174);
    CHECK(calls.size() == 52);
    CHECK(rowed.size() == 48);
    CHECK(rowed.size() + quiet.size() == exports);

    // Each misuse is refused, saying why, on a whole GPU, and the same way on a lost one.
    std::vector<std::string> misused(calls.size());
    for (std::size_t at = 0; at < calls.size(); ++at) {
        if (calls[at].misuse) {
            CAPTURE(calls[at].name);
            // fbe_error says something no row's misuse does first, so what it says after is the
            // misuse's.
            CHECK_FALSE(fbe_gpu_set_scaling(-1, false));
            const std::string before = fbe_error();
            CHECK_FALSE(calls[at].misuse());
            misused[at] = fbe_error();
            CHECK(misused[at] != before);
        }
    }

    CHECK(fbe_gpu_simulate_loss(0));
    for (std::size_t at = 0; at < calls.size(); ++at) {
        const call_t& each = calls[at];
        CAPTURE(each.name);
        if (each.misuse) {
            CHECK_FALSE(fbe_gpu_set_scaling(-1, false));
            const fbe::gpu::reached_t before = fbe::gpu::reached();
            CHECK_FALSE(each.misuse());
            INFO("asked of the driver before: " << reached(before)
                                                << "; after: " << reached(fbe::gpu::reached()));
            CHECK(fbe::gpu::reached() == before);
            CHECK(std::string(fbe_error()) == misused[at]);
        }
        // fbe_error says something else first, so what it says after is the call's.
        CHECK_FALSE(fbe_texture_retain(0));
        const fbe::gpu::reached_t before = fbe::gpu::reached();
        const bool worked = each.call();
        const fbe::gpu::reached_t after = fbe::gpu::reached();
        INFO("asked of the driver before: " << reached(before) << "; after: " << reached(after));
        CHECK(after == before);
        if (each.does == lost_t::REFUSES) {
            CHECK_FALSE(worked);
            CHECK(says_lost());
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
    fbe_material_release(model_material);
    fbe_geometry_release(cube);
    fbe_geometry_release(sphere);
    fbe_text_free_font(font);
    for (const std::uint64_t made :
         { texture, floats, target, multisampled, locked, model_texture }) {
        fbe_texture_release(made);
    }
    fbe_assets_free(image);
    fbe_assets_free(model);
    CHECK(fbe_vfs_unmount_id(mount));
    stop_gpu();
    require(fbe_gpu_set_render_size(0, 0));
    require(fbe_gpu_set_render_samples(1));
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
    require(fbe_gpu_set_lod_bias(0.0F));
    require(fbe_gpu_set_anisotropy(1.0F));
    stop_gpu();
    fbe_window_close();
}

TEST_CASE("a write or draw while the GPU's lost leaves its pixels on the CPU from before older",
          "[gpu][stability]") {
    require(fbe_gpu_start());
    const std::array<std::uint8_t, 4> pixel{ 255, 0, 0, 255 };
    const fbe_2d_pen_t ink{ .red = 255, .alpha = 255 };
    const std::uint64_t written = fbe_texture_create(4, 4, FBE_TEXTURE_RGBA8, false);
    const std::uint64_t drawn = fbe_texture_create_target(16, 16, FBE_TEXTURE_RGBA8, 1, false);
    const std::uint64_t texted = fbe_texture_create_target(32, 16, FBE_TEXTURE_RGBA8, 1, false);
    const std::uint64_t font = fbe_text_embedded_font(12.0F, 0);
    require(written != 0 && drawn != 0 && texted != 0 && font != 0);
    require(fbe_2d_clear(drawn, &ink));
    for (const std::uint64_t texture : { written, drawn, texted }) {
        require(fbe_texture_pixels(texture, nullptr) != nullptr);
    }
    CHECK(fbe_gpu_simulate_loss(0));
    // Taken again with nothing since, it's what it was.
    CHECK(fbe_texture_pixels(written, nullptr) != nullptr);
    require(fbe_texture_write(written, 0, 0, 1, 1, pixel.data(), 4));
    require(fbe_2d_rect(drawn, &ink, 0, 0, 4, 4, true));
    require(fbe_2d_text(texted, &ink, font, "Lost", 0, 0));
    for (const std::uint64_t texture : { written, drawn, texted }) {
        CAPTURE(texture == written ? "written" : texture == drawn ? "drawn into" : "text drawn");
        CHECK(fbe_texture_pixels(texture, nullptr) == nullptr);
        CHECK(says_lost());
    }
    fbe_text_free_font(font);
    for (const std::uint64_t texture : { written, drawn, texted }) {
        fbe_texture_release(texture);
    }
    stop_gpu();
}

TEST_CASE("the LOD bias and anisotropy set while the GPU's lost are in force once it's made again",
          "[gpu][stability]") {
    require(fbe_gpu_start());
    const float bias = fbe::gpu::samplers_lod_bias();
    const float anisotropy = fbe::gpu::samplers_anisotropy();
    CHECK(fbe_gpu_simulate_loss(0));
    const fbe::gpu::reached_t before = fbe::gpu::reached();
    require(fbe_gpu_set_lod_bias(2.0F));
    require(fbe_gpu_set_anisotropy(4.0F));
    INFO("asked of the driver before: " << reached(before)
                                        << "; after: " << reached(fbe::gpu::reached()));
    CHECK(fbe::gpu::reached() == before);
    // Kept, not made on the lost device.
    CHECK(fbe_gpu_lod_bias() == 2.0F);
    CHECK(fbe_gpu_anisotropy() == 4.0F);
    CHECK(fbe::gpu::samplers_lod_bias() == bias);
    CHECK(fbe::gpu::samplers_anisotropy() == anisotropy);
    stop_gpu();
    require(fbe_gpu_start());
    const fbe::gpu::device_t& gpu = *fbe::gpu::device();
    CHECK(fbe::gpu::samplers_lod_bias() == std::min(2.0F, gpu.max_lod_bias));
    CHECK(fbe::gpu::samplers_anisotropy() == std::min(4.0F, gpu.max_anisotropy));
    // Stopped, no samplers are in force, and the settings are kept for the next start.
    stop_gpu();
    CHECK(fbe::gpu::samplers_lod_bias() == 0.0F);
    CHECK(fbe::gpu::samplers_anisotropy() == 1.0F);
    CHECK(fbe_gpu_lod_bias() == 2.0F);
    CHECK(fbe_gpu_anisotropy() == 4.0F);
    require(fbe_gpu_set_lod_bias(0.0F));
    require(fbe_gpu_set_anisotropy(1.0F));
}

TEST_CASE("a camera with no target and no window is refused, lost or not", "[gpu][stability]") {
    fbe_window_close();
    require(fbe_gpu_start());
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t camera = fbe_entity_create(world, 0, FBE_ENTITY_CAMERA);
    CHECK_FALSE(fbe_render_camera(camera, 1.0F));
    const std::string whole = fbe_error();
    CHECK(whole == "a camera with no target draws into the screen, and there's no window open");
    CHECK(fbe_gpu_simulate_loss(0));
    CHECK_FALSE(fbe_render_camera(camera, 1.0F));
    CHECK(std::string(fbe_error()) == whole);
    CHECK_FALSE(fbe_render_world(world, 1.0F));
    CHECK(std::string(fbe_error()) == whole);
    fbe_world_free(world);
    std::array<std::uint64_t, 64> some{};
    while (fbe_entity_freed(some.data(), some.size()) != 0) {}
    stop_gpu();
}

TEST_CASE("a start that failed is tried again once the GPU's stopped, and the GPU's made again",
          "[gpu][stability]") {
    REQUIRE(fbe_window_set_headless(true));
    require(fbe_window_open(32, 16, FBE_WINDOW_WINDOWED));
    require(fbe_gpu_start());
    CHECK_FALSE(fbe_gpu_simulate_loss(-1));
    CHECK_FALSE(fbe_gpu_lost());
    require(fbe_gpu_simulate_loss(1));
    // A game's loop, as Gpu.Lost's doc has it: stopped and started, and on a failure stopped and
    // started again a second later, counted in the delta, which 1/32 counts out exactly.
    const fixed_delta_t fixed(1.0 / 32.0);
    int starts = 0;
    int failures = 0;
    int made_at = -1;
    bool remaking = false;
    double remake_in = 0.0;
    for (int frame = 0; frame < 100 && made_at < 0; ++frame) {
        CAPTURE(frame);
        fbe_frame_flip(false);
        if (fbe_gpu_lost()) {
            fbe_gpu_stop();
            remaking = true;
            remake_in = 0.0;
        }
        if (!remaking) {
            continue;
        }
        remake_in -= fbe_frame_delta();
        if (remake_in > 0.0) {
            // Nothing starts it by itself meanwhile, the screen neither: it doesn't try.
            CHECK_FALSE(fbe_texture_retain(0));
            const std::string before = fbe_error();
            CHECK(fbe_gpu_screen() == 0);
            CHECK(std::string(fbe_error()) == before);
            CHECK_FALSE(fbe_gpu_started());
            continue;
        }
        fbe_gpu_stop();
        ++starts;
        if (fbe_gpu_start()) {
            made_at = frame;
            remaking = false;
        } else {
            ++failures;
            INFO(fbe_error());
            CHECK(std::string_view(fbe_error()).ends_with("(VkResult -4)"));
            remake_in = 1.0;
        }
    }
    CHECK(starts == 2);
    CHECK(failures == 1);
    CHECK(made_at == 32);
    // Made again, it draws.
    REQUIRE(fbe_gpu_started());
    CHECK_FALSE(fbe_gpu_lost());
    CHECK(fbe_gpu_screen() != 0);
    const std::uint64_t again = fbe_texture_create_target(8, 8, FBE_TEXTURE_RGBA8, 1, false);
    require(again != 0);
    const fbe_2d_pen_t red{ .red = 255, .alpha = 255 };
    require(fbe_2d_clear(again, &red));
    // 8 by 8 pixels of 4 bytes.
    std::array<std::uint8_t, 256> read{};
    require(fbe_texture_read(again, 0, read.data(), read.size()));
    CHECK(read[0] == 255);
    CHECK(read[1] == 0);
    const std::int64_t shown = fbe_gpu_presented();
    fbe_frame_flip(false);
    CHECK(fbe_gpu_presented() == shown + 1);
    CHECK(fbe_texture_release(again));
    stop_gpu();
    fbe_window_close();
}

// Starts a simulated loss has still to fail last across stops, as a game's retry stops before
// each start, and a test's end forgets them, so none fails in a later test.
TEST_CASE("a test's end forgets the starts a simulated loss has still to fail",
          "[gpu][stability]") {
    REQUIRE(fbe_window_set_headless(true));
    require(fbe_gpu_start());
    require(fbe_gpu_simulate_loss(3));
    fbe_gpu_stop();
    fbe_gpu_stop();
    CHECK_FALSE(fbe_gpu_start());
    CHECK(std::string_view(fbe_error()).ends_with("(VkResult -4)"));
    // Two still to fail, which the test's end forgets.
    stop_gpu();
    INFO(fbe_error());
    CHECK(fbe_gpu_start());
    CHECK_FALSE(fbe_gpu_lost());
    stop_gpu();
}

TEST_CASE("a camera's render while the GPU's lost leaves its target's pixels on the CPU older",
          "[gpu][stability]") {
    require(fbe_gpu_start());
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t first = fbe_entity_create(world, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t second = fbe_entity_create(world, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t first_target =
        fbe_texture_create_target(16, 16, FBE_TEXTURE_RGBA8, 1, true);
    const std::uint64_t second_target =
        fbe_texture_create_target(16, 16, FBE_TEXTURE_RGBA8, 1, true);
    require(first_target != 0 && second_target != 0);
    require(fbe_camera_set_target(first, first_target));
    require(fbe_camera_set_target(second, second_target));
    require(fbe_render_world(world, 1.0F));
    for (const std::uint64_t target : { first_target, second_target }) {
        require(fbe_texture_pixels(target, nullptr) != nullptr);
    }
    require(fbe_gpu_simulate_loss(0));
    // The one camera's render changes its target alone.
    require(fbe_render_camera(first, 1.0F));
    CHECK(fbe_texture_pixels(first_target, nullptr) == nullptr);
    CHECK(says_lost());
    CHECK(fbe_texture_pixels(second_target, nullptr) != nullptr);
    // The world's changes every camera's.
    require(fbe_render_world(world, 1.0F));
    CHECK(fbe_texture_pixels(second_target, nullptr) == nullptr);
    CHECK(says_lost());
    fbe_world_free(world);
    std::array<std::uint64_t, 64> some{};
    while (fbe_entity_freed(some.data(), some.size()) != 0) {}
    fbe_texture_release(first_target);
    fbe_texture_release(second_target);
    stop_gpu();
}
