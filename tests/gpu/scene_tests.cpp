// Drawing the scene on the GPU, through the C API: materials, their layers and shading models,
// Its culling is the scene's as it is now, so that follows an edit at once; what's drawn waits.
// lights, fog, order, cameras into textures, sprites, planes, terrains, and mirrors, each checked
// by the pixels its formula says it draws, and a golden image of each.

#include "engine_shaders.hpp"
#include "fbengine.h"
#include "golden/golden.hpp"
#include "gpu/buffer.hpp"
#include "gpu/graph.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/vulkan.hpp"
#include "gpu_testing.hpp"
#include "render/drawing.hpp"
#include "render/render.hpp"
#include "scene/scene.hpp"
#include "shader/shader.hpp"
#include "test_shaders.hpp"
#include "texture/texture.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <random>
#include <SDL3/SDL.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using fbe::tests::require;
using fbe::tests::stop_gpu;

constexpr std::int32_t frame_size = 128;

/// A world of the test's own and the GPU, both gone at its end, leaving nothing behind.
struct scene_t {
    std::uint64_t world = fbe_world_create();
    std::uint64_t target =
        fbe_texture_create_target(frame_size, frame_size, FBE_TEXTURE_RGBA8, 1, true);
    std::uint64_t camera = fbe_entity_create(world, 0, FBE_ENTITY_CAMERA);
    /// Whether the GPU stops as it goes: unless a test of many scenes in turn keeps it, for the
    /// next to draw with the pipelines this one made, which lavapipe takes seconds over.
    bool stops_gpu = true;

    scene_t() {
        require(target != 0);
        require(fbe_camera_set_target(camera, target));
        // Nothing but what a test lights.
        require(fbe_world_set_ambient(world, 0.0F, 0.0F, 0.0F));
    }
    explicit scene_t(bool stops) : scene_t() {
        stops_gpu = stops;
    }
    scene_t(const scene_t&) = delete;
    scene_t& operator=(const scene_t&) = delete;
    scene_t(scene_t&&) = delete;
    scene_t& operator=(scene_t&&) = delete;

    ~scene_t() {
        fbe_world_free(world);
        std::array<std::uint64_t, 64> some{};
        while (fbe_entity_freed(some.data(), some.size()) != 0) {}
        fbe_texture_release(target);
        // As a failure leaves it too, so what's after starts as it would.
        if (stops_gpu || std::uncaught_exceptions() > 0) {
            stop_gpu();
        }
    }

    /// What's been drawn into the target since it was last cleared, once its camera's drawn.
    [[nodiscard]] fbe::golden::frame_t drawn(float tween = 1.0F) const {
        require(fbe_render_world(world, tween));
        fbe::golden::frame_t frame = read(target);
        // A frame of its own, since what's changed in a geometry waits for the next.
        fbe_frame_flip(false);
        return frame;
    }

    static fbe::golden::frame_t read(std::uint64_t texture) {
        const auto width = static_cast<std::uint32_t>(fbe_texture_width(texture));
        const auto height = static_cast<std::uint32_t>(fbe_texture_height(texture));
        fbe::golden::frame_t frame{ .width = width, .height = height };
        frame.rgba.resize(std::size_t{ width } * height * 4);
        require(fbe_texture_read(texture, 0, frame.rgba.data(),
                                 static_cast<std::int64_t>(frame.rgba.size())));
        return frame;
    }
};

using rgba_t = std::array<std::uint8_t, 4>;

/// The count `variable` asks for, as a longer run does, or `otherwise`.
int asked_for(const char* variable, int otherwise) {
    const char* asked = SDL_getenv(variable);
    int count = otherwise;
    if (asked != nullptr) {
        std::from_chars(asked, asked + std::strlen(asked), count);
    }
    return count;
}

/// A part of a test, by its name, and what it does in a scene of its own.
template <typename Scene>
using part_t = std::pair<std::string_view, std::function<void(const Scene&)>>;

/// Each part in a scene of its own, made by Scene(whether it stops the GPU), in turn, on one GPU:
/// lavapipe takes seconds to make a game shader's pipelines as it first draws with them, so that's
/// once, not once a part, as sections would, each starting the GPU again.
template <typename Scene>
void on_one_gpu(const std::vector<part_t<Scene>>& parts) {
    for (std::size_t at = 0; at < parts.size(); ++at) {
        INFO(parts[at].first);
        {
            const Scene scene(at + 1 == parts.size());
            parts[at].second(scene);
        }
        // Kept for the next part, and stopped after the last.
        CHECK(fbe_gpu_started() == (at + 1 < parts.size()));
    }
}

rgba_t pixel(const fbe::golden::frame_t& frame, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{ y } * frame.width) + x) * 4;
    return { frame.rgba[at], frame.rgba[at + 1], frame.rgba[at + 2], frame.rgba[at + 3] };
}

/// The middle pixel.
rgba_t middle(const fbe::golden::frame_t& frame) {
    return pixel(frame, frame.width / 2, frame.height / 2);
}

/// What linear light is stored as in an sRGB byte.
std::uint8_t srgb(float linear) {
    const float clamped = std::clamp(linear, 0.0F, 1.0F);
    const float encoded = clamped <= 0.0031308F
                              ? clamped * 12.92F
                              : (1.055F * std::pow(clamped, 1.0F / 2.4F)) - 0.055F;
    return static_cast<std::uint8_t>(std::lround(encoded * 255.0F));
}

/// What glTF's BRDF, height-correlated as pbr.slang's, gives one channel of a light, times the
/// cosine, by the dots of its normal with the light, the view, and what's halfway, and of the view
/// with what's halfway.
float brdf(float base, float metallic, float roughness, float n_l, float n_v, float n_h,
           float v_h) {
    const float alpha = std::max(roughness * roughness, 0.002F);
    const float squared = alpha * alpha;
    const float across = (n_h * n_h * (squared - 1.0F)) + 1.0F;
    const float distribution = squared / (std::numbers::pi_v<float> * across * across);
    const float viewed = n_l * std::sqrt((n_v * n_v * (1.0F - squared)) + squared);
    const float lit = n_v * std::sqrt((n_l * n_l * (1.0F - squared)) + squared);
    const float visibility = 0.5F / (viewed + lit);
    const float f0 = 0.04F + ((base - 0.04F) * metallic);
    const float fresnel = f0 + ((1.0F - f0) * std::pow(1.0F - v_h, 5.0F));
    const float diffuse = (1.0F - fresnel) * base * (1.0F - metallic) / std::numbers::pi_v<float>;
    return (diffuse + (fresnel * visibility * distribution)) * n_l;
}

/// What it gives a surface a new material's, white and a rough dielectric, lit from the eye, seen
/// at a cosine of `facing`.
float rough_white(float facing) {
    return brdf(1.0F, 0.0F, 1.0F, facing, facing, facing, 1.0F);
}

/// How tight the scene shader's fixed-function highlight is.
constexpr float highlight_power = 100.0F;

/// What's stored for an encoded value 0 to 1, as UNLIT and VERTEX work in.
std::uint8_t stored(float encoded) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(encoded, 0.0F, 1.0F) * 255.0F));
}

/// Whether a pixel's within a couple of bytes of what's expected, its alpha aside.
bool near(rgba_t found, std::array<std::uint8_t, 3> expected, int within = 2) {
    return std::abs(found[0] - expected[0]) <= within &&
           std::abs(found[1] - expected[1]) <= within && std::abs(found[2] - expected[2]) <= within;
}

/// A cube, -1 to 1, in a material, 0 for the default, at a place.
std::uint64_t cube(std::uint64_t world, float x, float y, float z, std::uint64_t material = 0) {
    const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    const std::uint64_t geometry = fbe_geometry_cube();
    require(fbe_mesh_set_geometry(mesh, geometry));
    fbe_geometry_release(geometry);
    require(fbe_mesh_set_material(mesh, material));
    require(fbe_entity_set_position(mesh, x, y, z, FBE_SPACE_WORLD));
    return mesh;
}

/// A big square facing +Z at a depth, -1 to 1 scaled by `half`, in a material, whose UVs are 0
/// to 1.
std::uint64_t wall(std::uint64_t world, float z, float half, std::uint64_t material = 0) {
    const std::uint64_t geometry = fbe_geometry_create();
    const std::int32_t surface = fbe_geometry_add_surface(geometry);
    const auto corner = [&](float x, float y, float u, float v) {
        return fbe_vertex_t{
            .position = { .x = x * half, .y = y * half, .z = 0.0F },
            .normal = { .x = 0.0F, .y = 0.0F, .z = 1.0F },
            .uv0 = { .x = u, .y = v },
            .uv1 = { .x = u, .y = v },
            .color = { .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F },
        };
    };
    const fbe_vertex_t low_left = corner(-1.0F, -1.0F, 0.0F, 1.0F);
    const fbe_vertex_t low_right = corner(1.0F, -1.0F, 1.0F, 1.0F);
    const fbe_vertex_t high_right = corner(1.0F, 1.0F, 1.0F, 0.0F);
    const fbe_vertex_t high_left = corner(-1.0F, 1.0F, 0.0F, 0.0F);
    const std::array vertices{ low_left, low_right, high_right, high_left };
    fbe_surface_add_vertices(geometry, surface, vertices.data(), 4);
    const std::array<std::uint32_t, 6> indices{ 0, 1, 2, 0, 2, 3 };
    fbe_surface_add_triangles(geometry, surface, indices.data(), 6);
    const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    require(fbe_mesh_set_geometry(mesh, geometry));
    fbe_geometry_release(geometry);
    require(fbe_mesh_set_material(mesh, material));
    require(fbe_entity_set_position(mesh, 0.0F, 0.0F, z, FBE_SPACE_WORLD));
    return mesh;
}

std::uint64_t unlit(float r, float g, float b, float a = 1.0F) {
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_shading(material, FBE_SHADING_UNLIT));
    require(fbe_material_set_color(material, r, g, b, a));
    return material;
}

/// A texture of one color, its bytes as given.
std::uint64_t flat_texture(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a,
                           std::int32_t format = FBE_TEXTURE_RGBA8) {
    const std::uint64_t texture = fbe_texture_create(2, 2, format, false);
    require(texture != 0);
    std::array<std::uint8_t, 16> pixels{};
    for (std::size_t i = 0; i < 4; ++i) {
        pixels[i * 4] = r;
        pixels[(i * 4) + 1] = g;
        pixels[(i * 4) + 2] = b;
        pixels[(i * 4) + 3] = a;
    }
    require(fbe_texture_write(texture, 0, 0, 2, 2, pixels.data(), 8));
    return texture;
}

/// A texture of four pixels, row by row.
std::uint64_t four_pixels(rgba_t a, rgba_t b, rgba_t c, rgba_t d) {
    const std::uint64_t texture = fbe_texture_create(2, 2, FBE_TEXTURE_RGBA8, false);
    require(texture != 0);
    std::array<std::uint8_t, 16> pixels{};
    std::ranges::copy(a, pixels.begin());
    std::ranges::copy(b, pixels.begin() + 4);
    std::ranges::copy(c, pixels.begin() + 8);
    std::ranges::copy(d, pixels.begin() + 12);
    require(fbe_texture_write(texture, 0, 0, 2, 2, pixels.data(), 8));
    return texture;
}

std::uint64_t light(std::uint64_t world, std::int32_t type, float x, float y, float z,
                    float range) {
    const std::uint64_t made = fbe_entity_create(world, 0, FBE_ENTITY_LIGHT);
    require(fbe_light_set_type(made, type));
    require(fbe_light_set_range(made, range));
    require(fbe_entity_set_position(made, x, y, z, FBE_SPACE_WORLD));
    return made;
}

/// glTF's falloff.
float falloff(float apart, float range) {
    const float part = apart / range;
    const float window = std::clamp(1.0F - (part * part * part * part), 0.0F, 1.0F);
    return window * window / std::max(apart * apart, 0.0001F);
}

} // namespace

TEST_CASE("an unlit material draws its color, and a camera clears to its own", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    cube(scene.world, 0.0F, 0.0F, -6.0F, red);
    const fbe::golden::frame_t frame = scene.drawn();
    CHECK(middle(frame) == rgba_t{ 255, 0, 0, 255 });
    CHECK(pixel(frame, 1, 1) == rgba_t{ 0, 0, 255, 255 });
    fbe_material_release(red);
}

TEST_CASE("nothing's drawn by a hidden camera, and a bad tween or freed camera fails",
          "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 1.0F, 0.0F, 1.0F));
    fbe_entity_set_hidden(scene.camera, true);
    CHECK(middle(scene.drawn())[3] == 0);
    fbe_entity_set_hidden(scene.camera, false);
    CHECK_FALSE(fbe_render_camera(scene.camera, 2.0F));
    CHECK_FALSE(fbe_render_world(scene.world, -1.0F));
    const std::uint64_t other = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
    fbe_entity_free(other);
    CHECK_FALSE(fbe_render_camera(other, 1.0F));
    // A camera with no target draws into the screen, and there's no window.
    const std::uint64_t lost = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
    CHECK_FALSE(fbe_render_camera(lost, 1.0F));
    fbe_entity_free(lost);
    // Only a target with depth is one a camera can draw into.
    const std::uint64_t flat = fbe_texture_create(4, 4, FBE_TEXTURE_RGBA8, false);
    const std::uint64_t shallow = fbe_texture_create_target(4, 4, FBE_TEXTURE_RGBA8, 1, false);
    CHECK_FALSE(fbe_camera_set_target(scene.camera, flat));
    CHECK_FALSE(fbe_camera_set_target(scene.camera, shallow));
    CHECK(fbe_camera_target(scene.camera) == scene.target);
    fbe_texture_release(flat);
    fbe_texture_release(shallow);
}

TEST_CASE("layers go over the lit color, each as its blend says, by its UV set", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t material = unlit(1.0F, 1.0F, 1.0F);
    const std::uint64_t quarter = flat_texture(64, 64, 64, 255, FBE_TEXTURE_RGBA8_LINEAR);
    const std::uint64_t half = flat_texture(128, 128, 128, 255, FBE_TEXTURE_RGBA8_LINEAR);
    const std::uint64_t red = flat_texture(255, 0, 0, 128, FBE_TEXTURE_RGBA8_LINEAR);
    wall(scene.world, -3.0F, 10.0F, material);
    // No layers: white.
    CHECK(near(middle(scene.drawn()), { 255, 255, 255 }));
    // Multiplying by a quarter.
    require(fbe_material_set_texture(material, 0, quarter));
    CHECK(near(middle(scene.drawn()), { 64, 64, 64 }, 1));
    // Twice over a half leaves it.
    require(fbe_material_set_texture(material, 1, half));
    require(fbe_material_set_layer(material, 1, FBE_LAYER_MULTIPLY_2, 0));
    CHECK(near(middle(scene.drawn()), { 64, 64, 64 }, 1));
    // Adding a half.
    require(fbe_material_set_layer(material, 1, FBE_LAYER_ADD, 0));
    CHECK(near(middle(scene.drawn()), { 192, 192, 192 }, 1));
    // Laid over by its alpha: half red, half what's under.
    require(fbe_material_set_texture(material, 1, red));
    require(fbe_material_set_layer(material, 1, FBE_LAYER_ALPHA, 0));
    const float over = 128.0F / 255.0F;
    const float under = 64.0F / 255.0F;
    CHECK(near(middle(scene.drawn()),
               { stored((under * (1.0F - over)) + over), stored(under * (1.0F - over)),
                 stored(under * (1.0F - over)) },
               1));
    // A layer with no texture isn't there; the one above moves down.
    require(fbe_material_set_texture(material, 0, 0));
    require(fbe_material_set_layer(material, 1, FBE_LAYER_MULTIPLY, 0));
    CHECK(near(middle(scene.drawn()), { 255, 0, 0 }));
    for (const std::uint64_t texture : { quarter, half, red }) {
        fbe_texture_release(texture);
    }
    fbe_material_release(material);
}

TEST_CASE("a layer reads the UV set it says, through what turns them", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = unlit(1.0F, 1.0F, 1.0F);
    // Left half red, right half green, nearest, repeating.
    const std::uint64_t texture = fbe_texture_create(2, 1, FBE_TEXTURE_RGBA8, false);
    const std::array<std::uint8_t, 8> pixels{ 255, 0, 0, 255, 0, 255, 0, 255 };
    require(fbe_texture_write(texture, 0, 0, 2, 1, pixels.data(), 8));
    require(fbe_texture_set_filter(texture, FBE_TEXTURE_NEAREST));
    require(fbe_material_set_texture(material, 0, texture));
    const std::uint64_t mesh = wall(scene.world, -1.0F, 2.0F, material);
    // The view at a distance of 1 is 1.15 high, so the wall fills it; its UV 0.5 at the middle.
    // UV set 1 is the same as set 0 here but for being moved half across.
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    std::array<fbe_vertex_t, 4> vertices{};
    require(fbe_surface_vertices(geometry, 0, 0, vertices.data(), 4));
    for (fbe_vertex_t& vertex : vertices) {
        vertex.uv1.x = vertex.uv0.x + 0.5F;
        vertex.uv1.y = vertex.uv0.y;
    }
    require(fbe_surface_set_vertices(geometry, 0, 0, vertices.data(), 4));
    const std::uint32_t left = frame_size / 4;
    const std::uint32_t right = (frame_size * 3) / 4;
    const std::uint32_t row = frame_size / 2;
    fbe::golden::frame_t frame = scene.drawn();
    CHECK(pixel(frame, left, row) == rgba_t{ 255, 0, 0, 255 });
    CHECK(pixel(frame, right, row) == rgba_t{ 0, 255, 0, 255 });
    require(fbe_material_set_layer(material, 0, FBE_LAYER_MULTIPLY, 1));
    frame = scene.drawn();
    CHECK(pixel(frame, left, row) == rgba_t{ 0, 255, 0, 255 });
    CHECK(pixel(frame, right, row) == rgba_t{ 255, 0, 0, 255 });
    // Moved back half across, set 1 reads as set 0 did; scaled to 2, it's half as many texels.
    require(fbe_material_set_layer_uv(material, 0, 1.0F, 1.0F, 0.0F, 0.5F, 0.0F));
    frame = scene.drawn();
    CHECK(pixel(frame, left, row) == rgba_t{ 255, 0, 0, 255 });
    std::array<float, 5> values{};
    require(fbe_material_layer_uv(material, 0, values.data()));
    CHECK(values[3] == 0.5F);
    CHECK_FALSE(fbe_material_set_layer_uv(material, 0, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F));
    CHECK_FALSE(fbe_material_set_texture(material, 8, texture));
    fbe_texture_release(texture);
    fbe_material_release(material);
}

namespace {

std::uint64_t game_shader();

} // namespace

// With its samplers biasing or not, as Metal's can't through MoltenVK, a layer's mipmaps are biased
// the same: at 7 texels a pixel, a checker's mipmap is all gray; biased sharper, its own texels;
// blurrier, gray again. So too for what a game's shader reads of a layer.
TEST_CASE("the LOD bias reaches a layer through its samplers, or its shader", "[gpu][scene]") {
    const bool samplers_can = GENERATE(true, false);
    const bool by_its_own_shader = GENERATE(false, true);
    if (!samplers_can) {
        fbe::gpu::inject(fbe::gpu::fault_t::NO_LOD_BIAS);
    }
    const scene_t scene;
    if (samplers_can && fbe::gpu::device()->max_lod_bias == 0.0F) {
        // A GPU whose samplers haven't any is the other case already.
        return;
    }
    CHECK(fbe::gpu::lod_bias_left() == 0.0F);
    const std::uint64_t material = unlit(1.0F, 1.0F, 1.0F);
    const std::uint64_t texture = fbe_texture_create(64, 64, FBE_TEXTURE_RGBA8, true);
    std::vector<std::uint8_t> pixels;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const std::uint8_t level = ((x + y) % 2) == 0 ? 255 : 0;
            pixels.insert(pixels.end(), { level, level, level, 255 });
        }
    }
    require(fbe_texture_write(texture, 0, 0, 64, 64, pixels.data(), 64 * 4));
    require(fbe_texture_set_filter(texture, FBE_TEXTURE_LINEAR));
    require(fbe_material_set_texture(material, 0, texture));
    wall(scene.world, -1.0F, 2.0F, material);
    // The wall's 4 across, the view's 1.15, so 0.2875 of its UVs show.
    const float texels = 0.2875F * 64.0F;
    const float scale = texels / (static_cast<float>(frame_size) * 7.0F);
    require(fbe_material_set_layer_uv(material, 0, scale, scale, 0.0F, 0.0F, 0.0F));
    if (by_its_own_shader) {
        const std::uint64_t shader = game_shader();
        require(shader != 0);
        require(fbe_material_set_shader(material, shader));
        require(fbe_shader_release(shader));
        require(fbe_material_set_parameter(material, 0, 8.0F, 0.0F, 0.0F, 0.0F));
        // A layer's scale divides its UVs, as these are multiplied.
        require(fbe_material_set_parameter(material, 3, 0.0F, 1.0F / scale, 0.0F, 0.0F));
    }
    const auto spread = [&scene] {
        const fbe::golden::frame_t frame = scene.drawn();
        int lowest = 255;
        int highest = 0;
        for (std::uint32_t y = 0; y < 8; ++y) {
            for (std::uint32_t x = 0; x < 8; ++x) {
                const rgba_t found = pixel(frame, (frame_size / 2) + x, (frame_size / 2) + y);
                lowest = std::min<int>(lowest, found[0]);
                highest = std::max<int>(highest, found[0]);
            }
        }
        return highest - lowest;
    };
    CHECK(spread() <= 8);
    require(fbe_gpu_set_lod_bias(-3.0F));
    CHECK((fbe::gpu::lod_bias_left() != 0.0F) == !samplers_can);
    CHECK(spread() > 16);
    require(fbe_gpu_set_lod_bias(3.0F));
    CHECK(spread() <= 8);
    require(fbe_gpu_set_lod_bias(0.0F));
    CHECK(spread() <= 8);
    fbe_texture_release(texture);
    fbe_material_release(material);
}

TEST_CASE("a bump layer shifts the next layer's UVs and scales its light", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = unlit(1.0F, 1.0F, 1.0F);
    // Red then green across: a shift of a quarter across moves the wall's left onto green.
    const std::uint64_t texture = fbe_texture_create(2, 1, FBE_TEXTURE_RGBA8, false);
    const std::array<std::uint8_t, 8> pixels{ 255, 0, 0, 255, 0, 255, 0, 255 };
    require(fbe_texture_write(texture, 0, 0, 2, 1, pixels.data(), 8));
    require(fbe_texture_set_filter(texture, FBE_TEXTURE_NEAREST));
    // All red up from the middle, 1 once centered, and half blue.
    const std::uint64_t bump = flat_texture(255, 128, 128, 255, FBE_TEXTURE_RGBA8_LINEAR);
    require(fbe_material_set_texture(material, 0, bump));
    require(fbe_material_set_layer(material, 0, FBE_LAYER_BUMP, 0));
    require(fbe_material_set_texture(material, 1, texture));
    wall(scene.world, -1.0F, 2.0F, material);
    const std::uint32_t left = (frame_size * 3) / 8;
    const std::uint32_t row = frame_size / 2;
    // None of it at first: left of the middle is red.
    CHECK(pixel(scene.drawn(), left, row) == rgba_t{ 255, 0, 0, 255 });
    require(fbe_material_set_layer_bump(material, 0, 0.25F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F));
    CHECK(pixel(scene.drawn(), left, row) == rgba_t{ 0, 255, 0, 255 });
    // Its blue, about half, scaled by 1 and offset by none: half the next layer's light.
    require(fbe_material_set_layer_bump(material, 0, 0.25F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F));
    CHECK(near(pixel(scene.drawn(), left, row), { 0, 128, 0 }, 1));
    std::array<float, 6> values{};
    require(fbe_material_layer_bump(material, 0, values.data()));
    CHECK(values[0] == 0.25F);
    CHECK(values[4] == 1.0F);
    fbe_texture_release(texture);
    fbe_texture_release(bump);
    fbe_material_release(material);
}

TEST_CASE("a bump map's flat middle gray shifts nothing, in either format", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = unlit(1.0F, 1.0F, 1.0F);
    const std::uint64_t texture = fbe_texture_create(2, 1, FBE_TEXTURE_RGBA8, false);
    const std::array<std::uint8_t, 8> pixels{ 255, 0, 0, 255, 0, 255, 0, 255 };
    require(fbe_texture_write(texture, 0, 0, 2, 1, pixels.data(), 8));
    require(fbe_texture_set_filter(texture, FBE_TEXTURE_NEAREST));
    require(fbe_material_set_layer(material, 0, FBE_LAYER_BUMP, 0));
    require(fbe_material_set_layer_bump(material, 0, 100.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F));
    require(fbe_material_set_texture(material, 1, texture));
    wall(scene.world, -1.0F, 2.0F, material);
    for (const std::int32_t format : { FBE_TEXTURE_RGBA8, FBE_TEXTURE_RGBA8_LINEAR }) {
        const std::uint64_t bump = flat_texture(128, 128, 128, 255, format);
        require(fbe_material_set_texture(material, 0, bump));
        CAPTURE(format);
        CHECK(pixel(scene.drawn(), (frame_size * 3) / 8, frame_size / 2) ==
              rgba_t{ 255, 0, 0, 255 });
        fbe_texture_release(bump);
    }
    fbe_texture_release(texture);
    fbe_material_release(material);
}

TEST_CASE("a material without alpha, one with a cutoff, and what's tinted draw as they say",
          "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    // Half alpha, cut off at more: nothing drawn.
    const std::uint64_t cut = unlit(1.0F, 0.0F, 0.0F, 1.0F);
    const std::uint64_t half = flat_texture(255, 255, 255, 128);
    require(fbe_material_set_texture(cut, 0, half));
    require(fbe_material_set_alpha_cutoff(cut, 0.75F));
    const std::uint64_t mesh = cube(scene.world, 0.0F, 0.0F, -6.0F, cut);
    CHECK(middle(scene.drawn()) == rgba_t{ 0, 0, 255, 255 });
    require(fbe_material_set_alpha_cutoff(cut, 0.25F));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    // A tint.
    require(fbe_mesh_set_tint(mesh, 0.0F, 1.0F, 1.0F, 1.0F));
    CHECK(middle(scene.drawn()) == rgba_t{ 0, 0, 0, 255 });
    // OPAQUE leaves the textures' alpha out, whatever the cutoff.
    require(fbe_mesh_set_tint(mesh, 1.0F, 1.0F, 1.0F, 1.0F));
    require(fbe_material_set_blend(cut, FBE_BLEND_OPAQUE));
    require(fbe_material_set_alpha_cutoff(cut, 0.75F));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    fbe_texture_release(half);
    fbe_material_release(cut);
}

TEST_CASE("vertex colors multiply a material's, if it takes them", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = unlit(1.0F, 1.0F, 1.0F);
    const std::uint64_t mesh = wall(scene.world, -3.0F, 10.0F, material);
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    std::array<fbe_vertex_t, 4> vertices{};
    require(fbe_surface_vertices(geometry, 0, 0, vertices.data(), 4));
    for (fbe_vertex_t& vertex : vertices) {
        vertex.color = { .r = 0.5F, .g = 0.25F, .b = 0.0F, .a = 1.0F };
    }
    require(fbe_surface_set_vertices(geometry, 0, 0, vertices.data(), 4));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 255, 255, 255 });
    require(fbe_material_set_vertex_colors(material, true));
    CHECK(near(middle(scene.drawn()), { srgb(0.5F), srgb(0.25F), 0 }));
    fbe_material_release(material);
}

TEST_CASE("a one-sided surface shows only its front, a two-sided one both, mirrored or not",
          "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t material = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t mesh = wall(scene.world, -3.0F, 10.0F, material);
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    // Turned around: its back.
    require(fbe_entity_set_rotation(mesh, 0.0F, 1.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    CHECK(middle(scene.drawn()) == rgba_t{ 0, 0, 255, 255 });
    // Mirrored too, its front's its front again, turned toward us.
    require(fbe_entity_set_scale(mesh, -1.0F, 1.0F, -1.0F, FBE_SPACE_WORLD));
    require(fbe_entity_set_rotation(mesh, 0.0F, 0.0F, 0.0F, 1.0F, FBE_SPACE_WORLD));
    require(fbe_entity_set_scale(mesh, -1.0F, 1.0F, 1.0F, FBE_SPACE_WORLD));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    require(fbe_entity_set_rotation(mesh, 0.0F, 1.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    CHECK(middle(scene.drawn()) == rgba_t{ 0, 0, 255, 255 });
    require(fbe_material_set_two_sided(material, true));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    fbe_material_release(material);
}

TEST_CASE("point lights fall off per pixel as glTF's", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    wall(scene.world, -6.0F, 20.0F, material);
    // 2 straight in front of the wall's middle, range 8.
    const std::uint64_t lamp = light(scene.world, FBE_LIGHT_POINT, 0.0F, 0.0F, -4.0F, 8.0F);
    require(fbe_light_set_intensity(lamp, 2.0F));
    const float expected = 2.0F * falloff(2.0F, 8.0F) * rough_white(1.0F);
    CHECK(near(middle(scene.drawn()), { srgb(expected), srgb(expected), srgb(expected) }));
    // Past its range: nothing but the ambient, which there isn't.
    require(fbe_light_set_range(lamp, 1.5F));
    CHECK(near(middle(scene.drawn()), { 0, 0, 0 }));
    fbe_material_release(material);
}

TEST_CASE("per vertex lighting falls off as range over distance, with no end", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_shading(material, FBE_SHADING_VERTEX));
    // A small wall, all its vertices about as far from the lamp.
    wall(scene.world, -1.0F, 0.05F, material);
    const std::uint64_t lamp = light(scene.world, FBE_LIGHT_POINT, 0.0F, 0.0F, -1.0F, 1.5F);
    // At 3, half: what's stored.
    require(fbe_entity_set_position(lamp, 0.0F, 0.0F, 2.0F, FBE_SPACE_WORLD));
    CHECK(near(middle(scene.drawn()), { 128, 128, 128 }, 1));
    // At 1.2, inside its range, all of it.
    require(fbe_entity_set_position(lamp, 0.0F, 0.0F, 0.2F, FBE_SPACE_WORLD));
    CHECK(near(middle(scene.drawn()), { 255, 255, 255 }, 1));
    // Clamped at the vertices, before any texture: a second lamp adds nothing past white.
    const std::uint64_t half = flat_texture(128, 128, 128, 255, FBE_TEXTURE_RGBA8_LINEAR);
    require(fbe_material_set_texture(material, 0, half));
    light(scene.world, FBE_LIGHT_POINT, 0.0F, 0.0F, 0.2F, 1.5F);
    CHECK(near(middle(scene.drawn()), { 128, 128, 128 }, 1));
    fbe_texture_release(half);
    fbe_material_release(material);
}

TEST_CASE("per vertex lighting is worked out at the vertices", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_shading(material, FBE_SHADING_VERTEX));
    wall(scene.world, -3.0F, 10.0F, material);
    // Close to the middle of a big wall: per pixel it's bright there, per vertex as its far
    // corners.
    light(scene.world, FBE_LIGHT_POINT, 0.0F, 0.0F, -2.5F, 1.0F);
    const std::uint8_t dim = middle(scene.drawn())[0];
    require(fbe_material_set_shading(material, FBE_SHADING_PBR));
    const std::uint8_t bright = middle(scene.drawn())[0];
    CHECK(dim < 40);
    CHECK(bright > dim + 40);
    fbe_material_release(material);
}

// As roughness says, on PBR: a peak head on, falling away at the view's edge, where the way halfway
// to the eye is 15 degrees off the normal.
TEST_CASE("a highlight's where a surface turns its light to the eye", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    wall(scene.world, -3.0F, 10.0F, material);
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    require(fbe_light_set_intensity(sun, 0.25F));
    const auto at_middle = [](float roughness) {
        return srgb(0.25F * brdf(1.0F, 0.0F, roughness, 1.0F, 1.0F, 1.0F, 1.0F));
    };
    require(fbe_material_set_metal_rough(material, 0.0F, 0.3F));
    const fbe::golden::frame_t shiny = scene.drawn();
    const std::uint8_t peak = at_middle(0.3F);
    CHECK(near(middle(shiny), { peak, peak, peak }, 4));
    const float half_way = 0.2618F;
    const std::uint8_t edge = srgb(0.25F * brdf(1.0F, 0.0F, 0.3F, 1.0F, std::cos(half_way * 2.0F),
                                                std::cos(half_way), std::cos(half_way)));
    CHECK(near(pixel(shiny, 1, frame_size / 2), { edge, edge, edge }, 4));
    fbe_material_release(material);
}

// Per vertex, in what's stored: its highlight's as bright as its light, at its middle, and only as
// its shininess says. A PBR material's shininess is nothing.
TEST_CASE("a VERTEX material's highlight is at its vertices, as its shininess says",
          "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_shading(material, FBE_SHADING_VERTEX));
    wall(scene.world, -3.0F, 0.05F, material);
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    require(fbe_light_set_intensity(sun, 0.25F));
    const float color = static_cast<float>(srgb(0.25F)) / 255.0F;
    const std::uint8_t plain = stored(color);
    CHECK(near(middle(scene.drawn()), { plain, plain, plain }));
    require(fbe_material_set_shininess(material, 1.0F));
    const float off = std::pow(std::cos(0.0167F), highlight_power);
    const std::uint8_t shiny = stored(color * (1.0F + off));
    CHECK(near(middle(scene.drawn()), { shiny, shiny, shiny }, 3));
    require(fbe_material_set_shading(material, FBE_SHADING_PBR));
    const std::uint8_t lit = srgb(0.25F * rough_white(1.0F));
    CHECK(near(middle(scene.drawn()), { lit, lit, lit }));
    fbe_material_release(material);
}

TEST_CASE("a layer's UVs turn and scale with its angle and scale", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = unlit(1.0F, 1.0F, 1.0F);
    // Across: red then green. Down: red then blue.
    const rgba_t reds{ 255, 0, 0, 255 };
    const rgba_t greens{ 0, 255, 0, 255 };
    const rgba_t blues{ 0, 0, 255, 255 };
    const rgba_t whites{ 255, 255, 255, 255 };
    const std::uint64_t texture = four_pixels(reds, greens, blues, whites);
    require(fbe_texture_set_filter(texture, FBE_TEXTURE_NEAREST));
    require(fbe_texture_set_wrap(texture, FBE_TEXTURE_CLAMP, FBE_TEXTURE_CLAMP));
    require(fbe_material_set_texture(material, 0, texture));
    // 1.15 high at a distance of 1, so a wall half 0.5 fills all but the view's edges.
    wall(scene.world, -1.0F, 0.5F, material);
    const std::uint32_t low = (frame_size * 3) / 8;
    const std::uint32_t high = (frame_size * 5) / 8;
    fbe::golden::frame_t frame = scene.drawn();
    CHECK(pixel(frame, low, low) == reds);
    CHECK(pixel(frame, high, low) == greens);
    CHECK(pixel(frame, low, high) == blues);
    // Scaled to 2, it looks twice as big: from the wall's UV 0 to 0.5, all of it in the first
    // texel.
    require(fbe_material_set_layer_uv(material, 0, 2.0F, 2.0F, 0.0F, 0.0F, 0.0F));
    frame = scene.drawn();
    CHECK(pixel(frame, low, low) == reds);
    CHECK(pixel(frame, high, low) == reds);
    CHECK(pixel(frame, low, high) == reds);
    // Turned a quarter: u = cos(-a) x + sin(-a) y, v = -sin(-a) x + cos(-a) y, around UV 0: at a
    // quarter turn, u = -v0 and v = u0, so clamped to the first column, the row by its u.
    require(fbe_material_set_layer_uv(material, 0, 1.0F, 1.0F, 1.5707964F, 0.0F, 0.0F));
    frame = scene.drawn();
    CHECK(pixel(frame, low, low) == reds);
    CHECK(pixel(frame, high, low) == blues);
    CHECK(pixel(frame, low, high) == reds);
    // Scaled first, then turned: a scale of 2 across and a quarter turn take a UV of (0.625, 0.375)
    // to (-0.375, 0.3125), so the first row, where turning first would take it to (-0.1875, 0.625),
    // the second.
    require(fbe_material_set_layer_uv(material, 0, 2.0F, 1.0F, 1.5707964F, 0.0F, 0.0F));
    frame = scene.drawn();
    CHECK(pixel(frame, high, low) == reds);
    fbe_texture_release(texture);
    fbe_material_release(material);
}

TEST_CASE("a spot light has its cones, a directional one its way, and ambient lights everything",
          "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    wall(scene.world, -3.0F, 10.0F, material);
    require(fbe_world_set_ambient(scene.world, 0.25F, 0.0F, 0.0F));
    CHECK(near(middle(scene.drawn()), { srgb(0.25F), 0, 0 }));
    require(fbe_world_set_ambient(scene.world, 0.0F, 0.0F, 0.0F));
    // A directional light along its -Z, straight onto the wall's front.
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    require(fbe_light_set_intensity(sun, 2.0F));
    const std::uint8_t lit = srgb(2.0F * rough_white(1.0F));
    CHECK(near(middle(scene.drawn()), { lit, lit, lit }));
    // From behind the wall's front: nothing.
    require(fbe_entity_set_rotation(sun, 0.0F, 1.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    CHECK(near(middle(scene.drawn()), { 0, 0, 0 }));
    fbe_entity_free(sun);
    // A spot straight at the middle from 1 away: whole inside its inner cone, out past its outer.
    const std::uint64_t spot = light(scene.world, FBE_LIGHT_SPOT, 0.0F, 0.0F, -2.0F, 100.0F);
    require(fbe_light_set_cone(spot, 0.1F, 0.2F));
    const fbe::golden::frame_t frame = scene.drawn();
    const std::uint8_t whole = srgb(falloff(1.0F, 100.0F) * rough_white(1.0F));
    CHECK(near(middle(frame), { whole, whole, whole }));
    CHECK(near(pixel(frame, 2, frame_size / 2), { 0, 0, 0 }));
    fbe_material_release(material);
}

TEST_CASE("many small lights each light only what they reach", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    wall(scene.world, -3.0F, 10.0F, material);
    // A row of colored lamps across the wall.
    for (int i = -4; i <= 4; ++i) {
        const float x = static_cast<float>(i) * 0.3F;
        const std::uint64_t lamp = light(scene.world, FBE_LIGHT_POINT, x, 0.0F, -2.9F, 0.15F);
        const bool odd = (i % 2) != 0;
        require(fbe_light_set_color(lamp, odd ? 1.0F : 0.0F, odd ? 0.0F : 1.0F, 0.0F));
        require(fbe_light_set_intensity(lamp, 0.02F));
    }
    const fbe::golden::frame_t frame = scene.drawn();
    fbe::golden::check("scene_small_lights", frame);
    // Above the row, no lamp reaches.
    CHECK(near(pixel(frame, frame_size / 2, 8), { 0, 0, 0 }));
    fbe_material_release(material);
}

TEST_CASE("fog thickens with depth, each mode by its formula", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_fog_color(scene.camera, 0.0F, 1.0F, 0.0F));
    require(fbe_camera_set_fog_range(scene.camera, 2.0F, 12.0F));
    require(fbe_camera_set_fog_density(scene.camera, 0.1F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t mesh = wall(scene.world, -7.0F, 50.0F, red);
    const float depth = 7.0F;
    // On what's stored for UNLIT, in linear light for PBR.
    const auto check_mode = [&](std::int32_t mode, float thick) {
        require(fbe_camera_set_fog_mode(scene.camera, mode));
        CAPTURE(mode);
        CHECK(near(middle(scene.drawn()), { stored(1.0F - thick), stored(thick), 0 }));
    };
    check_mode(FBE_FOG_NONE, 0.0F);
    const float linear_fog = (depth - 2.0F) / 10.0F;
    check_mode(FBE_FOG_LINEAR, linear_fog);
    check_mode(FBE_FOG_EXPONENTIAL, 1.0F - std::exp(-0.1F * depth));
    check_mode(FBE_FOG_EXPONENTIAL_SQUARED, 1.0F - std::exp(-(0.1F * depth) * (0.1F * depth)));
    // A material that isn't fogged.
    require(fbe_material_set_fogged(red, false));
    CHECK(near(middle(scene.drawn()), { 255, 0, 0 }));
    // PBR's, lit by ambient alone, in linear light.
    const std::uint64_t lit = fbe_material_create();
    require(fbe_material_set_color(lit, 1.0F, 0.0F, 0.0F, 1.0F));
    require(fbe_world_set_ambient(scene.world, 1.0F, 1.0F, 1.0F));
    require(fbe_mesh_set_material(mesh, lit));
    require(fbe_camera_set_fog_mode(scene.camera, FBE_FOG_LINEAR));
    CHECK(near(middle(scene.drawn()), { srgb(1.0F - linear_fog), srgb(linear_fog), 0 }));
    fbe_material_release(lit);
    fbe_material_release(red);
}

TEST_CASE("blends draw over what's behind them, and what adds fades in fog", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.25F, 0.25F, 0.25F, 1.0F));
    const std::uint64_t pane = unlit(0.5F, 0.5F, 0.0F, 0.5F);
    wall(scene.world, -3.0F, 10.0F, pane);
    const auto check_blend = [&](std::int32_t blend, std::array<float, 3> linear) {
        require(fbe_material_set_blend(pane, blend));
        CAPTURE(blend);
        CHECK(near(middle(scene.drawn()), { srgb(linear[0]), srgb(linear[1]), srgb(linear[2]) }));
    };
    // By its alpha.
    check_blend(FBE_BLEND_ALPHA, { 0.375F, 0.375F, 0.125F });
    // Its light, scaled by its alpha, added.
    check_blend(FBE_BLEND_ADD, { 0.5F, 0.5F, 0.25F });
    // What's behind multiplied by it, scaled by its alpha, on what's stored: 0.735 stored for 0.5.
    const auto of_stored = [](float encoded) {
        return encoded <= 0.04045F ? encoded / 12.92F : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
    };
    const float half_stored = (1.055F * std::pow(0.5F, 1.0F / 2.4F)) - 0.055F;
    const float scaled = 0.25F * of_stored(1.0F - (0.5F * (1.0F - half_stored)));
    check_blend(FBE_BLEND_MULTIPLY, { scaled, scaled, 0.25F * of_stored(0.5F) });
    // In whole fog, what adds adds nothing, what multiplies leaves what's behind, and what's laid
    // over by alpha lays the fog's color over.
    require(fbe_camera_set_fog_mode(scene.camera, FBE_FOG_LINEAR));
    require(fbe_camera_set_fog_range(scene.camera, 0.5F, 1.0F));
    require(fbe_camera_set_fog_color(scene.camera, 0.0F, 0.0F, 1.0F));
    check_blend(FBE_BLEND_ADD, { 0.25F, 0.25F, 0.25F });
    check_blend(FBE_BLEND_MULTIPLY, { 0.25F, 0.25F, 0.25F });
    check_blend(FBE_BLEND_ALPHA, { 0.125F, 0.125F, 0.625F });
    fbe_material_release(pane);
}

TEST_CASE("order groups draw behind the scene or over it, far to near", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    // A sky, close up but behind everything; a floor; and an overlay, far off but over everything.
    const std::uint64_t sky = unlit(0.0F, 0.0F, 1.0F);
    require(fbe_material_set_order(sky, -1));
    const std::uint64_t scenery = unlit(0.0F, 1.0F, 0.0F);
    const std::uint64_t overlay = unlit(1.0F, 0.0F, 0.0F);
    require(fbe_material_set_order(overlay, 1));
    wall(scene.world, -1.0F, 10.0F, sky);
    cube(scene.world, 0.0F, 0.0F, -6.0F, scenery);
    const std::uint64_t over = cube(scene.world, 0.0F, 0.0F, -40.0F, overlay);
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    fbe_entity_set_hidden(over, true);
    const fbe::golden::frame_t frame = scene.drawn();
    CHECK(middle(frame) == rgba_t{ 0, 255, 0, 255 });
    CHECK(pixel(frame, 1, 1) == rgba_t{ 0, 0, 255, 255 });
    // Far to near, in an order: the nearer of two over the further, though it's made first.
    fbe_entity_set_hidden(over, false);
    const std::uint64_t blue = unlit(0.0F, 0.0F, 1.0F);
    require(fbe_material_set_order(blue, 1));
    cube(scene.world, 0.0F, 0.0F, -20.0F, blue);
    CHECK(middle(scene.drawn()) == rgba_t{ 0, 0, 255, 255 });
    for (const std::uint64_t material : { sky, scenery, overlay, blue }) {
        fbe_material_release(material);
    }
}

TEST_CASE("what's blended draws after what's opaque, far to near, testing its depth",
          "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t glass = unlit(1.0F, 0.0F, 0.0F, 0.5F);
    const std::uint64_t solid = unlit(0.0F, 1.0F, 0.0F);
    // Glass in front of the solid, and glass behind it, which doesn't show.
    wall(scene.world, -2.0F, 10.0F, glass);
    wall(scene.world, -3.0F, 10.0F, solid);
    wall(scene.world, -4.0F, 10.0F, glass);
    CHECK(near(middle(scene.drawn()), { srgb(0.5F), srgb(0.5F), 0 }));
    fbe_material_release(glass);
    fbe_material_release(solid);
}

TEST_CASE("a camera renders into a texture another camera draws", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    // The monitor's camera, drawn first, looks at a red cube off in the distance.
    const std::uint64_t monitor_target =
        fbe_texture_create_target(32, 32, FBE_TEXTURE_RGBA8, 1, true);
    const std::uint64_t monitor = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
    require(fbe_camera_set_target(monitor, monitor_target));
    require(fbe_camera_set_order(monitor, -1));
    require(fbe_camera_set_clear_color(monitor, 0.0F, 0.0F, 1.0F, 1.0F));
    require(fbe_entity_set_position(monitor, 100.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    cube(scene.world, 100.0F, 0.0F, -4.0F, red);
    // The main camera sees a wall with it on, smaller than its view: the cube in the middle, the
    // monitor's own clear color by the wall's corner, and the main camera's past it.
    const std::uint64_t screen = unlit(1.0F, 1.0F, 1.0F);
    require(fbe_material_set_texture(screen, 0, monitor_target));
    wall(scene.world, -1.0F, 0.4F, screen);
    const fbe::golden::frame_t frame = scene.drawn();
    CHECK(middle(frame) == rgba_t{ 255, 0, 0, 255 });
    CHECK(pixel(frame, 24, 24) == rgba_t{ 0, 0, 255, 255 });
    CHECK(pixel(frame, 2, 2) == rgba_t{ 0, 0, 0, 255 });
    fbe::golden::check("scene_monitor", frame);
    // A material sampling its own camera's target draws white, not a feedback loop.
    require(fbe_material_set_texture(screen, 0, scene.target));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 255, 255, 255 });
    fbe_entity_free(monitor);
    fbe_texture_release(monitor_target);
    fbe_material_release(red);
    fbe_material_release(screen);
}

TEST_CASE("cameras share a target by their viewports, clearing only what's theirs",
          "[gpu][scene]") {
    const scene_t scene;
    const auto half = static_cast<float>(frame_size) / 2.0F;
    require(
        fbe_camera_set_viewport(scene.camera, 0.0F, 0.0F, half, static_cast<float>(frame_size)));
    require(fbe_camera_set_clear_color(scene.camera, 1.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t right = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
    require(fbe_camera_set_target(right, scene.target));
    require(fbe_camera_set_viewport(right, half, 0.0F, half, static_cast<float>(frame_size)));
    require(fbe_camera_set_clear_color(right, 0.0F, 1.0F, 0.0F, 1.0F));
    const fbe::golden::frame_t frame = scene.drawn();
    CHECK(pixel(frame, 2, 2) == rgba_t{ 255, 0, 0, 255 });
    CHECK(pixel(frame, frame_size - 3, 2) == rgba_t{ 0, 255, 0, 255 });
    // An overlay, orthographic, clearing nothing, over a part of it, sees what's in its own order.
    const std::uint64_t overlay = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
    require(fbe_camera_set_target(overlay, scene.target));
    require(fbe_camera_set_order(overlay, 1));
    require(fbe_camera_set_projection(overlay, FBE_PROJECTION_ORTHOGRAPHIC));
    require(fbe_camera_set_orthographic_height(overlay, 4.0F));
    require(fbe_camera_set_clear(overlay, false, false));
    require(fbe_entity_set_position(overlay, 50.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    const std::uint64_t blue = unlit(0.0F, 0.0F, 1.0F);
    cube(scene.world, 50.0F, 0.0F, -5.0F, blue);
    const fbe::golden::frame_t over = scene.drawn();
    CHECK(middle(over) == rgba_t{ 0, 0, 255, 255 });
    CHECK(pixel(over, 2, 2) == rgba_t{ 255, 0, 0, 255 });
    fbe::golden::check("scene_viewports", over);
    fbe_material_release(blue);
}

TEST_CASE("the frame copied into a texture is drawn back, as feedback effects do", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t copy = fbe_texture_create(frame_size, frame_size, FBE_TEXTURE_RGBA8, false);
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t mesh = cube(scene.world, 0.0F, 0.0F, -6.0F, red);
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    require(fbe_texture_copy(copy, 0, 0, scene.target, 0, 0, frame_size, frame_size));
    // Then that copy on a wall, with the cube gone, half as bright.
    fbe_entity_free(mesh);
    const std::uint64_t feedback = unlit(0.5F, 0.5F, 0.5F);
    require(fbe_material_set_texture(feedback, 0, copy));
    wall(scene.world, -1.0F, 2.0F, feedback);
    CHECK(near(middle(scene.drawn()), { srgb(0.5F), 0, 0 }));
    fbe_texture_release(copy);
    fbe_material_release(red);
    fbe_material_release(feedback);
}

TEST_CASE("a multisampled target's camera draws and resolves", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t target = fbe_texture_create_target(32, 32, FBE_TEXTURE_RGBA8, 4, true);
    require(target != 0);
    require(fbe_camera_set_target(scene.camera, target));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    cube(scene.world, 0.0F, 0.0F, -2.0F, red);
    require(fbe_render_world(scene.world, 1.0F));
    const fbe::golden::frame_t frame = scene_t::read(target);
    CHECK(pixel(frame, 16, 16) == rgba_t{ 255, 0, 0, 255 });
    require(fbe_camera_set_target(scene.camera, scene.target));
    fbe_texture_release(target);
    fbe_material_release(red);
}

TEST_CASE("sprites draw in bulk, as instances of one quad, facing the camera", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    // With no material, a sprite's full-bright white, tinted.
    const std::uint64_t glow = fbe_entity_create(scene.world, 0, FBE_ENTITY_SPRITE);
    require(fbe_entity_set_position(glow, 0.0F, 0.0F, -6.0F, FBE_SPACE_WORLD));
    require(fbe_sprite_set_tint(glow, 0.0F, 1.0F, 0.0F, 1.0F));
    fbe::golden::frame_t frame = scene.drawn();
    CHECK(middle(frame) == rgba_t{ 0, 255, 0, 255 });
    CHECK(pixel(frame, 2, 2) == rgba_t{ 0, 0, 0, 255 });
    // A thousand, added, in one draw of the pass.
    const std::uint64_t spark = unlit(0.1F, 0.0F, 0.0F, 1.0F);
    require(fbe_material_set_blend(spark, FBE_BLEND_ADD));
    for (int i = 0; i < 1000; ++i) {
        const std::uint64_t made = fbe_entity_create(scene.world, 0, FBE_ENTITY_SPRITE);
        require(fbe_sprite_set_material(made, spark));
        require(fbe_entity_set_position(made, 0.0F, 0.0F, -5.0F - (static_cast<float>(i) * 0.001F),
                                        FBE_SPACE_WORLD));
    }
    frame = scene.drawn();
    CHECK(middle(frame)[0] == 255);
    fbe_gpu_counters_t counters{};
    require(fbe_gpu_counters(&counters));
    CHECK(counters.triangles >= 2000);
    CHECK(counters.draws <= 2);
    fbe_material_release(spark);
}

TEST_CASE("a ground plane goes on to the far plane, its UVs its own x and z", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t material = unlit(1.0F, 1.0F, 1.0F);
    const rgba_t reds{ 255, 0, 0, 255 };
    const rgba_t greens{ 0, 255, 0, 255 };
    const std::uint64_t texture = four_pixels(reds, greens, greens, reds);
    require(fbe_texture_set_filter(texture, FBE_TEXTURE_NEAREST));
    require(fbe_material_set_texture(material, 0, texture));
    const std::uint64_t ground = fbe_entity_create(scene.world, 0, FBE_ENTITY_PLANE);
    require(fbe_plane_set_material(ground, material));
    require(fbe_entity_set_position(ground, 0.0F, -1.0F, 0.0F, FBE_SPACE_WORLD));
    require(fbe_camera_set_range(scene.camera, 0.1F, 200.0F));
    const fbe::golden::frame_t frame = scene.drawn();
    // The horizon's above the bottom: sky at the top, ground below.
    CHECK(pixel(frame, 2, 2) == rgba_t{ 0, 0, 255, 255 });
    const rgba_t low = pixel(frame, frame_size / 2, frame_size - 2);
    CHECK((low == rgba_t{ 255, 0, 0, 255 } || low == rgba_t{ 0, 255, 0, 255 }));
    fbe::golden::check("scene_plane", frame, { .channel = 2, .share = 0.01 });
    // From under it, none of it, as it's one-sided.
    require(fbe_entity_set_position(scene.camera, 0.0F, -3.0F, 0.0F, FBE_SPACE_WORLD));
    CHECK(pixel(scene.drawn(), frame_size / 2, 2) == rgba_t{ 0, 0, 255, 255 });
    fbe_texture_release(texture);
    fbe_material_release(material);
}

TEST_CASE("a terrain's drawn from its heights, finer where it's near", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    // Lit, so its heights' normals show.
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_color(material, 0.3F, 0.8F, 0.3F, 1.0F));
    require(fbe_world_set_ambient(scene.world, 0.1F, 0.1F, 0.1F));
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    require(fbe_entity_set_rotation(sun, -0.5F, 0.5F, 0.5F, 0.5F, FBE_SPACE_WORLD));
    const std::uint64_t terrain = fbe_entity_create(scene.world, 0, FBE_ENTITY_TERRAIN);
    require(fbe_terrain_set_size(terrain, 256));
    require(fbe_terrain_set_material(terrain, material));
    std::vector<float> heights(std::size_t{ 257 } * 257);
    for (std::size_t z = 0; z <= 256; ++z) {
        for (std::size_t x = 0; x <= 256; ++x) {
            const float across = std::sin(static_cast<float>(x) * 0.2F);
            const float along = std::cos(static_cast<float>(z) * 0.2F);
            heights[(z * 257) + x] = 8.0F * across * along;
        }
    }
    require(fbe_terrain_set_heights(terrain, heights.data(),
                                    static_cast<std::int32_t>(heights.size())));
    require(fbe_entity_set_position(terrain, -128.0F, -14.0F, -256.0F, FBE_SPACE_WORLD));
    require(fbe_camera_set_range(scene.camera, 0.5F, 500.0F));
    const fbe::golden::frame_t frame = scene.drawn();
    const rgba_t near_it = pixel(frame, frame_size / 2, frame_size - 2);
    CHECK(near_it[1] > near_it[2]);
    CHECK(pixel(frame, frame_size / 2, 2) == rgba_t{ 0, 0, 255, 255 });
    fbe::golden::check("scene_terrain", frame, { .channel = 2, .share = 0.01 });
    fbe_gpu_counters_t counters{};
    require(fbe_gpu_counters(&counters));
    // Whole, it'd be 2 x 256 x 256 triangles.
    CHECK(counters.triangles < std::int64_t{ 2 } * 256 * 256);
    CHECK(counters.triangles > 1000);
    fbe_material_release(material);
}

TEST_CASE("a mirror reflects the scene first, which what's on its plane lets through",
          "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    // A red cube above, and a half clear floor.
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    cube(scene.world, 0.0F, 1.5F, -8.0F, red);
    require(fbe_entity_set_position(scene.camera, 0.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    const std::uint64_t mirror = fbe_entity_create(scene.world, 0, FBE_ENTITY_MIRROR);
    require(fbe_entity_set_position(mirror, 0.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    const fbe::golden::frame_t frame = scene.drawn();
    fbe::golden::check("scene_mirror", frame);
    // Looking level, the cube above the middle, its reflection below it, as far.
    const std::uint32_t x = frame_size / 2;
    std::uint32_t above = 0;
    std::uint32_t below = 0;
    constexpr std::uint32_t last = frame_size - 1;
    for (std::uint32_t y = 0; y <= last / 2; ++y) {
        above += pixel(frame, x, y)[0] > 128 ? 1U : 0U;
        below += pixel(frame, x, last - y)[0] > 128 ? 1U : 0U;
    }
    CHECK(above > 0);
    CHECK(above == below);
    fbe_material_release(red);
}

TEST_CASE("a geometry changed is drawn changed, and one freed is let go of", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t mesh = wall(scene.world, -3.0F, 0.1F, red);
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    // Moved off to the side.
    std::array<fbe_vertex_t, 4> vertices{};
    require(fbe_surface_vertices(geometry, 0, 0, vertices.data(), 4));
    for (fbe_vertex_t& vertex : vertices) {
        vertex.position.x += 1.0F;
    }
    require(fbe_surface_set_vertices(geometry, 0, 0, vertices.data(), 4));
    CHECK(middle(scene.drawn()) == rgba_t{ 0, 0, 255, 255 });
    fbe_entity_free(mesh);
    CHECK(middle(scene.drawn()) == rgba_t{ 0, 0, 255, 255 });
    fbe_material_release(red);
}

TEST_CASE("a lightmapped, fogged, lit room matches its golden image", "[gpu][scene][golden]") {
    const scene_t scene;
    // Off to a side and a little turned, so nothing in the room's square to it: with the camera in
    // the middle, the checks' edges land on pixels' centers, where a driver's rounding picks a
    // side.
    require(fbe_entity_set_position(scene.camera, 0.137F, 0.071F, -0.21F, FBE_SPACE_WORLD));
    require(fbe_entity_set_rotation(scene.camera, 0.0123F, 0.0271F, 0.0057F, 0.99954F,
                                    FBE_SPACE_WORLD));
    require(fbe_camera_set_clear_color(scene.camera, 0.02F, 0.02F, 0.04F, 1.0F));
    require(fbe_camera_set_fog_mode(scene.camera, FBE_FOG_LINEAR));
    require(fbe_camera_set_fog_range(scene.camera, 4.0F, 30.0F));
    require(fbe_camera_set_fog_color(scene.camera, 0.05F, 0.05F, 0.08F));
    require(fbe_world_set_ambient(scene.world, 0.05F, 0.05F, 0.05F));
    // The room's material, as SCP:CB's: a doubled ambient, a lightmap added on set 1, the diffuse
    // multiplied on set 0. (2 x 50 + lightmap) x diffuse, on what's stored.
    const std::uint64_t room = unlit(1.0F, 1.0F, 1.0F);
    const std::uint64_t ambient = flat_texture(50, 50, 50, 255);
    const rgba_t light_gray{ 200, 200, 200, 255 };
    const rgba_t dark_gray{ 120, 120, 120, 255 };
    const std::uint64_t diffuse = four_pixels(light_gray, dark_gray, dark_gray, light_gray);
    require(fbe_texture_set_filter(diffuse, FBE_TEXTURE_NEAREST));
    const rgba_t lit_up{ 255, 220, 180, 255 };
    const rgba_t lit_some{ 60, 40, 30, 255 };
    const rgba_t lit_little{ 20, 20, 20, 255 };
    const std::uint64_t lightmap = four_pixels(lit_up, lit_some, lit_some, lit_little);
    require(fbe_material_set_texture(room, 0, ambient));
    require(fbe_material_set_layer(room, 0, FBE_LAYER_MULTIPLY_2, 0));
    require(fbe_material_set_texture(room, 1, lightmap));
    require(fbe_material_set_layer(room, 1, FBE_LAYER_ADD, 1));
    require(fbe_material_set_texture(room, 2, diffuse));
    require(fbe_material_set_layer(room, 2, FBE_LAYER_MULTIPLY, 0));
    require(fbe_material_set_layer_uv(room, 2, 0.5F, 0.5F, 0.0F, 0.0F, 0.0F));
    // Its walls: a cube seen from inside, two-sided.
    require(fbe_material_set_two_sided(room, true));
    const std::uint64_t shell = cube(scene.world, 0.0F, 0.0F, -6.0F, room);
    require(fbe_entity_set_scale(shell, 6.0F, 3.0F, 9.0F, FBE_SPACE_WORLD));
    // Its lightmap's UVs its own, its set 1 across its faces differently from set 0.
    const std::uint64_t shell_geometry = fbe_mesh_geometry(shell);
    const std::int32_t shell_vertices = fbe_surface_vertex_count(shell_geometry, 0);
    std::vector<fbe_vertex_t> corners(static_cast<std::size_t>(shell_vertices));
    require(fbe_surface_vertices(shell_geometry, 0, 0, corners.data(), shell_vertices));
    for (fbe_vertex_t& corner : corners) {
        corner.uv1 = { .x = corner.uv0.y * 0.5F, .y = 1.0F - corner.uv0.x };
    }
    require(fbe_surface_set_vertices(shell_geometry, 0, 0, corners.data(), shell_vertices));
    // A lit wall the spot falls on, off to the right.
    const std::uint64_t painted = fbe_material_create();
    require(fbe_material_set_color(painted, 0.7F, 0.7F, 0.7F, 1.0F));
    wall(scene.world, -11.0F, 2.8F, painted);
    const std::uint64_t spot = light(scene.world, FBE_LIGHT_SPOT, 0.8F, 0.3F, -7.0F, 12.0F);
    require(fbe_light_set_cone(spot, 0.25F, 0.4F));
    require(fbe_light_set_color(spot, 0.4F, 0.6F, 1.0F));
    require(fbe_light_set_intensity(spot, 16.0F));
    // A shiny crate, its lamp in front, seen from its near face, and a VERTEX pillar by the lamp.
    const std::uint64_t crate = fbe_material_create();
    require(fbe_material_set_color(crate, 0.8F, 0.6F, 0.4F, 1.0F));
    require(fbe_material_set_shininess(crate, 1.0F));
    const std::uint64_t box = cube(scene.world, -1.6F, -1.9F, -6.0F, crate);
    require(fbe_entity_set_scale(box, 1.0F, 1.0F, 0.2F, FBE_SPACE_WORLD));
    const std::uint64_t lamp = light(scene.world, FBE_LIGHT_POINT, -1.1F, -1.3F, -3.0F, 8.0F);
    require(fbe_light_set_color(lamp, 1.0F, 0.8F, 0.6F));
    require(fbe_light_set_intensity(lamp, 12.0F));
    const std::uint64_t retro = fbe_material_create();
    require(fbe_material_set_shading(retro, FBE_SHADING_VERTEX));
    require(fbe_material_set_color(retro, 0.3F, 0.5F, 0.3F, 1.0F));
    const std::uint64_t pillar = wall(scene.world, -5.0F, 1.0F, retro);
    require(fbe_entity_set_position(pillar, 1.9F, -1.7F, -5.0F, FBE_SPACE_WORLD));
    const std::uint64_t by_pillar = light(scene.world, FBE_LIGHT_POINT, 1.9F, -1.7F, -4.6F, 0.3F);
    require(fbe_light_set_intensity(by_pillar, 0.2F));
    // An unlit sign out of the fog, and glass.
    const std::uint64_t sign = unlit(0.1F, 0.9F, 0.2F);
    require(fbe_material_set_fogged(sign, false));
    const std::uint64_t exit = cube(scene.world, 0.0F, 2.2F, -12.0F, sign);
    require(fbe_entity_set_scale(exit, 0.8F, 0.3F, 0.1F, FBE_SPACE_WORLD));
    const std::uint64_t glass = unlit(0.3F, 0.5F, 0.6F, 0.35F);
    const std::uint64_t pane = cube(scene.world, 0.7F, -1.8F, -4.5F, glass);
    require(fbe_entity_set_scale(pane, 0.4F, 0.7F, 0.02F, FBE_SPACE_WORLD));
    // And a monitor on the wall, showing the crate from above.
    const std::uint64_t monitor = fbe_texture_create_target(64, 64, FBE_TEXTURE_RGBA8, 1, true);
    const std::uint64_t watching = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
    require(fbe_camera_set_target(watching, monitor));
    require(fbe_camera_set_order(watching, -1));
    require(fbe_entity_set_position(watching, -1.6F, 1.5F, -3.0F, FBE_SPACE_WORLD));
    require(fbe_entity_set_rotation(watching, -0.25882F, 0.0F, 0.0F, 0.96593F, FBE_SPACE_WORLD));
    const std::uint64_t screen = unlit(1.0F, 1.0F, 1.0F);
    require(fbe_material_set_texture(screen, 0, monitor));
    const std::uint64_t tv = wall(scene.world, -14.9F, 1.5F, screen);
    require(fbe_entity_set_position(tv, -3.5F, 2.0F, -14.0F, FBE_SPACE_WORLD));
    fbe::golden::check("scene_room", scene.drawn());
    fbe_entity_free(watching);
    fbe_texture_release(monitor);
    for (const std::uint64_t texture : { ambient, diffuse, lightmap }) {
        fbe_texture_release(texture);
    }
    for (const std::uint64_t material : { room, crate, retro, sign, glass, screen, painted }) {
        fbe_material_release(material);
    }
}

TEST_CASE("textures' layers go over a lit color too", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    const std::uint64_t half = flat_texture(128, 128, 128, 255, FBE_TEXTURE_RGBA8_LINEAR);
    require(fbe_material_set_texture(material, 0, half));
    wall(scene.world, -3.0F, 10.0F, material);
    require(fbe_world_set_ambient(scene.world, 0.5F, 0.5F, 0.5F));
    const float expected = 0.5F * 128.0F / 255.0F;
    CHECK(near(middle(scene.drawn()), { srgb(expected), srgb(expected), srgb(expected) }));
    fbe_texture_release(half);
    fbe_material_release(material);
}

TEST_CASE("a material's clone holds its textures, and a layer holds a released texture",
          "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = unlit(1.0F, 1.0F, 1.0F);
    const std::uint64_t red = flat_texture(255, 0, 0, 255);
    require(fbe_material_set_texture(material, 0, red));
    CHECK(fbe_material_texture(material, 0) == red);
    const std::uint64_t clone = fbe_material_clone(material);
    // Only they hold it now.
    fbe_texture_release(red);
    fbe_material_release(material);
    wall(scene.world, -3.0F, 10.0F, clone);
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    // A camera holds its target too.
    const std::uint64_t held = fbe_texture_create_target(8, 8, FBE_TEXTURE_RGBA8, 1, true);
    const std::uint64_t other = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
    require(fbe_camera_set_target(other, held));
    fbe_texture_release(held);
    CHECK(fbe_texture_width(held) == 8);
    fbe_entity_free(other);
    std::array<std::uint64_t, 16> freed{};
    while (fbe_entity_freed(freed.data(), freed.size()) != 0) {}
    CHECK(fbe_texture_width(held) == -1);
    fbe_material_release(clone);
}

TEST_CASE("a camera with no target draws into the screen", "[gpu][scene]") {
    // SDL's offscreen video, as ctest runs them: no window shows.
    require(fbe_window_open(64, 64, FBE_WINDOW_WINDOWED));
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t camera = fbe_entity_create(world, 0, FBE_ENTITY_CAMERA);
    require(fbe_camera_set_clear_color(camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    cube(world, 0.0F, 0.0F, -6.0F, red);
    require(fbe_render_world(world, 1.0F));
    const fbe::golden::frame_t frame = scene_t::read(fbe_gpu_screen());
    CHECK(middle(frame) == rgba_t{ 255, 0, 0, 255 });
    CHECK(pixel(frame, 1, 1) == rgba_t{ 0, 0, 255, 255 });
    fbe_frame_flip(false);
    fbe_material_release(red);
    fbe_world_free(world);
    std::array<std::uint64_t, 16> some{};
    while (fbe_entity_freed(some.data(), some.size()) != 0) {}
    fbe_window_close();
    stop_gpu();
}

TEST_CASE("the GPU stopped and started again draws what's left, with new textures",
          "[gpu][scene]") {
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t camera = fbe_entity_create(world, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t first = fbe_texture_create_target(16, 16, FBE_TEXTURE_RGBA8, 1, true);
    require(fbe_camera_set_target(camera, first));
    const std::uint64_t material = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t layer = flat_texture(0, 255, 0, 255);
    require(fbe_material_set_texture(material, 0, layer));
    cube(world, 0.0F, 0.0F, -3.0F, material);
    require(fbe_render_world(world, 1.0F));
    fbe_texture_read(first, 0, std::array<std::uint8_t, 1024>{}.data(), 1024);
    stop_gpu();
    // Its target went with it, which it says; a new one draws its geometry, sent again, and its
    // layer's gone white.
    CHECK_FALSE(fbe_render_world(world, 1.0F));
    const std::uint64_t second = fbe_texture_create_target(16, 16, FBE_TEXTURE_RGBA8, 1, true);
    require(second != 0);
    require(fbe_camera_set_target(camera, second));
    require(fbe_render_world(world, 1.0F));
    const fbe::golden::frame_t frame = scene_t::read(second);
    CHECK(pixel(frame, 8, 8) == rgba_t{ 255, 0, 0, 255 });
    fbe_world_free(world);
    std::array<std::uint64_t, 16> some{};
    while (fbe_entity_freed(some.data(), some.size()) != 0) {}
    fbe_texture_release(second);
    fbe_material_release(material);
    stop_gpu();
}

TEST_CASE("a skinned surface moves by its skin's joints, before its own matrix", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    // Small, in the middle, and all on its second joint, which starts where it is.
    const std::uint64_t mesh = wall(scene.world, -3.0F, 0.1F, red);
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    std::array<fbe_vertex_joints_t, 4> joints{};
    for (fbe_vertex_joints_t& vertex : joints) {
        vertex.joints[0] = 1;
        vertex.weights[0] = 1.0F;
    }
    require(fbe_surface_set_joints(geometry, 0, joints.data(), 4));
    CHECK_FALSE(fbe_surface_set_joints(geometry, 0, joints.data(), 3));
    std::array<fbe_vertex_joints_t, 4> read_back{};
    CHECK(fbe_surface_joints(geometry, 0, read_back.data(), 4) == 4);
    CHECK(read_back[2].joints[0] == 1);
    const std::uint64_t bones = fbe_entity_create(scene.world, 0, FBE_ENTITY_PIVOT);
    const std::uint64_t still = fbe_entity_create(scene.world, bones, FBE_ENTITY_PIVOT);
    const std::uint64_t moving = fbe_entity_create(scene.world, bones, FBE_ENTITY_PIVOT);
    require(fbe_entity_set_name(still, "still"));
    require(fbe_entity_set_name(moving, "moving"));
    const std::uint64_t skin = fbe_skin_create();
    // Bound at the mesh's own middle, so a joint where the mesh is leaves it where it is.
    std::array<float, 16> inverse_bind{};
    inverse_bind[0] = 1.0F;
    inverse_bind[5] = 1.0F;
    inverse_bind[10] = 1.0F;
    inverse_bind[15] = 1.0F;
    require(fbe_skin_add_joint(skin, "still", inverse_bind.data()));
    require(fbe_skin_add_joint(skin, "moving", inverse_bind.data()));
    require(fbe_entity_set_position(still, 0.0F, 0.0F, -3.0F, FBE_SPACE_WORLD));
    require(fbe_entity_set_position(moving, 0.0F, 0.0F, -3.0F, FBE_SPACE_WORLD));
    // With no skin, it's where its vertices are; with one, as its joints are.
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    require(fbe_skin_attach(mesh, skin, bones));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    require(fbe_entity_set_position(moving, 1.0F, 0.0F, -3.0F, FBE_SPACE_WORLD));
    const fbe::golden::frame_t moved = scene.drawn();
    CHECK(middle(moved) == rgba_t{ 0, 0, 255, 255 });
    // 1 across at a distance of 3, in a view 3.46 across there, is 0.29 of the way to its right.
    const float across_view = 1.0F / std::numbers::sqrt3_v<float>;
    const auto right_of_middle = static_cast<std::uint32_t>(64.0F + (64.0F * across_view));
    CHECK(pixel(moved, right_of_middle, 64) == rgba_t{ 255, 0, 0, 255 });
    // Half on each joint: half way.
    for (fbe_vertex_joints_t& vertex : joints) {
        vertex.joints[1] = 0;
        vertex.weights[0] = 0.5F;
        vertex.weights[1] = 0.5F;
    }
    require(fbe_surface_set_joints(geometry, 0, joints.data(), 4));
    const auto half_way = static_cast<std::uint32_t>(64.0F + (32.0F * across_view));
    CHECK(pixel(scene.drawn(), half_way, 64) == rgba_t{ 255, 0, 0, 255 });
    fbe_skin_release(skin);
    fbe_material_release(red);
}

TEST_CASE("UNLIT and VERTEX work on what's stored", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const auto stored_by = [&](std::int32_t shading, std::array<std::uint64_t, 3> layers,
                               std::array<std::int32_t, 3> blends) {
        const std::uint64_t material = fbe_material_create();
        require(fbe_material_set_shading(material, shading));
        for (std::int32_t i = 0; i < 3; ++i) {
            const auto at = static_cast<std::size_t>(i);
            require(fbe_material_set_texture(material, i, layers[at]));
            require(fbe_material_set_layer(material, i, blends[at], 0));
        }
        const std::uint64_t mesh = wall(scene.world, -3.0F, 10.0F, material);
        const rgba_t found = middle(scene.drawn());
        fbe_entity_free(mesh);
        fbe_material_release(material);
        return found;
    };
    const std::uint64_t fifty = flat_texture(50, 50, 50, 255);
    const std::uint64_t sixty = flat_texture(60, 60, 60, 255);
    const std::uint64_t two_hundred = flat_texture(200, 200, 200, 255);
    const std::uint64_t middling = flat_texture(128, 128, 128, 255);
    require(fbe_world_set_ambient(scene.world, 1.0F, 1.0F, 1.0F));
    for (const std::int32_t shading : { FBE_SHADING_UNLIT, FBE_SHADING_VERTEX }) {
        CAPTURE(shading);
        // Ambient 50 doubled is 100.
        CHECK(near(stored_by(shading, { fifty, 0, 0 }, { FBE_LAYER_MULTIPLY_2, 0, 0 }),
                   { 100, 100, 100 }, 1));
        // (2 x 50 + lightmap 60) x diffuse 200 is 125.
        CHECK(near(stored_by(shading, { fifty, sixty, two_hundred },
                             { FBE_LAYER_MULTIPLY_2, FBE_LAYER_ADD, FBE_LAYER_MULTIPLY }),
                   { 125, 125, 125 }, 1));
        // 128 doubled over white is clamped to white.
        CHECK(near(stored_by(shading, { middling, 0, 0 }, { FBE_LAYER_MULTIPLY_2, 0, 0 }),
                   { 255, 255, 255 }, 1));
    }
    for (const std::uint64_t texture : { fifty, sixty, two_hundred, middling }) {
        fbe_texture_release(texture);
    }
}

TEST_CASE("a texture's alpha flag blends a material that's AUTO", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t material = unlit(1.0F, 1.0F, 1.0F);
    const std::uint64_t half = flat_texture(255, 255, 255, 128);
    require(fbe_material_set_texture(material, 0, half));
    const std::uint64_t mesh = wall(scene.world, -3.0F, 10.0F, material);
    // Opaque till it's said.
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 255, 255, 255 });
    CHECK_FALSE(fbe_texture_alpha(half));
    require(fbe_texture_set_alpha(half, true));
    CHECK(fbe_texture_alpha(half));
    const float over = 128.0F / 255.0F;
    CHECK(near(middle(scene.drawn()), { srgb(over), srgb(over), 255 }));
    CHECK(fbe_mesh_blend(mesh, 0) == FBE_BLEND_ALPHA);
    // A cutoff takes it as a mask instead.
    require(fbe_material_set_alpha_cutoff(material, 0.25F));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 255, 255, 255 });
    fbe_texture_release(half);
    fbe_material_release(material);
}

namespace {

/// A small wall on a skin of two joints, by name under `bones`, each where the mesh is.
struct skinned_wall_t {
    std::uint64_t mesh = 0;
    std::uint64_t geometry = 0;
    std::uint64_t skin = 0;
    std::uint64_t still = 0;
    std::uint64_t moving = 0;
};

skinned_wall_t skinned_wall(std::uint64_t world, std::uint64_t material, float z, float half) {
    skinned_wall_t made;
    made.mesh = wall(world, z, half, material);
    made.geometry = fbe_mesh_geometry(made.mesh);
    const std::uint64_t bones = fbe_entity_create(world, 0, FBE_ENTITY_PIVOT);
    made.still = fbe_entity_create(world, bones, FBE_ENTITY_PIVOT);
    made.moving = fbe_entity_create(world, bones, FBE_ENTITY_PIVOT);
    require(fbe_entity_set_name(made.still, "still"));
    require(fbe_entity_set_name(made.moving, "moving"));
    require(fbe_entity_set_position(made.still, 0.0F, 0.0F, z, FBE_SPACE_WORLD));
    require(fbe_entity_set_position(made.moving, 0.0F, 0.0F, z, FBE_SPACE_WORLD));
    made.skin = fbe_skin_create();
    std::array<float, 16> inverse_bind{};
    inverse_bind[0] = 1.0F;
    inverse_bind[5] = 1.0F;
    inverse_bind[10] = 1.0F;
    inverse_bind[15] = 1.0F;
    require(fbe_skin_add_joint(made.skin, "still", inverse_bind.data()));
    require(fbe_skin_add_joint(made.skin, "moving", inverse_bind.data()));
    require(fbe_skin_attach(made.mesh, made.skin, bones));
    return made;
}

void all_on(std::uint64_t geometry, std::uint16_t joint, float weight) {
    std::array<fbe_vertex_joints_t, 4> joints{};
    for (fbe_vertex_joints_t& vertex : joints) {
        vertex.joints[0] = joint;
        vertex.weights[0] = weight;
    }
    require(fbe_surface_set_joints(geometry, 0, joints.data(), 4));
}

} // namespace

TEST_CASE("a joint past the palette is its last, never a read past it", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    const skinned_wall_t made = skinned_wall(scene.world, red, -3.0F, 0.1F);
    all_on(made.geometry, 65000, 1.0F);
    require(fbe_entity_set_position(made.moving, 1.0F, 0.0F, -3.0F, FBE_SPACE_WORLD));
    const fbe::golden::frame_t frame = scene.drawn();
    // As the last joint: moved 1 across.
    CHECK(middle(frame) == rgba_t{ 0, 0, 255, 255 });
    const float across_view = 1.0F / std::numbers::sqrt3_v<float>;
    CHECK(pixel(frame, static_cast<std::uint32_t>(64.0F + (64.0F * across_view)), 64) ==
          rgba_t{ 255, 0, 0, 255 });
    fbe_skin_release(made.skin);
    fbe_material_release(red);
}

TEST_CASE("a vertex with no weight stays where it is", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    const skinned_wall_t made = skinned_wall(scene.world, red, -3.0F, 0.1F);
    all_on(made.geometry, 1, 0.0F);
    std::array<fbe_vertex_joints_t, 4> read_back{};
    REQUIRE(fbe_surface_joints(made.geometry, 0, read_back.data(), 4) == 4);
    CHECK(read_back[0].joints[0] == FBE_NO_JOINT);
    CHECK(read_back[0].weights[0] == 1.0F);
    require(fbe_entity_set_position(made.moving, 1.0F, 0.0F, -3.0F, FBE_SPACE_WORLD));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    fbe_skin_release(made.skin);
    fbe_material_release(red);
}

TEST_CASE("a skinned mesh moved into view by its joints is drawn", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    // Its bind pose far behind the camera, its joints in front of it.
    const skinned_wall_t made = skinned_wall(scene.world, red, 50.0F, 0.1F);
    all_on(made.geometry, 1, 1.0F);
    require(fbe_entity_set_position(made.moving, 0.0F, 0.0F, -3.0F, FBE_SPACE_WORLD));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    fbe_skin_release(made.skin);
    fbe_material_release(red);
}

TEST_CASE("a skinned normal leans as a bone scaled along an axis has it", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    // A surface whose normal leans at 45 degrees, on a bone scaled by 4 across.
    const skinned_wall_t made = skinned_wall(scene.world, material, -3.0F, 0.1F);
    std::array<fbe_vertex_t, 4> vertices{};
    require(fbe_surface_vertices(made.geometry, 0, 0, vertices.data(), 4));
    const float lean = std::numbers::sqrt2_v<float> / 2.0F;
    for (fbe_vertex_t& vertex : vertices) {
        vertex.normal = { .x = lean, .y = 0.0F, .z = lean };
    }
    require(fbe_surface_set_vertices(made.geometry, 0, 0, vertices.data(), 4));
    all_on(made.geometry, 1, 1.0F);
    require(fbe_entity_set_scale(made.moving, 4.0F, 1.0F, 1.0F, FBE_SPACE_WORLD));
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    (void)sun;
    // The cofactor's (1/4, 0, 1) direction: its normal's z of 1 over the length of that.
    const float x = lean / 4.0F;
    const float facing = lean / std::sqrt((x * x) + (lean * lean));
    const std::uint8_t lit = srgb(rough_white(facing));
    CHECK(near(middle(scene.drawn()), { lit, lit, lit }));
    fbe_skin_release(made.skin);
    fbe_material_release(material);
}

TEST_CASE("a mirror scaled to nothing reflects nothing", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    cube(scene.world, 0.0F, 2.0F, -8.0F, red);
    const std::uint64_t mirror = fbe_entity_create(scene.world, 0, FBE_ENTITY_MIRROR);
    const fbe::golden::frame_t both = scene.drawn();
    CHECK(pixel(both, frame_size / 2, 80)[0] == 255);
    require(fbe_entity_set_scale(mirror, 1.0F, 0.0F, 1.0F, FBE_SPACE_WORLD));
    const fbe::golden::frame_t without = scene.drawn();
    CHECK(pixel(without, frame_size / 2, 80) == rgba_t{ 0, 0, 255, 255 });
    fbe_material_release(red);
}

TEST_CASE("a viewport past the device's limits draws what it can, and no validation error",
          "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 1.0F, 0.0F, 1.0F));
    require(fbe_camera_set_viewport(scene.camera, 0.0F, 0.0F, 100000.0F, 100000.0F));
    CHECK(pixel(scene.drawn(), 5, 5) == rgba_t{ 0, 255, 0, 255 });
    CHECK(fbe_gpu_validation_errors() == 0);
    require(fbe_camera_set_viewport(scene.camera, -3e9F, -3e9F, 6e9F, 6e9F));
    CHECK(pixel(scene.drawn(), 5, 5) == rgba_t{ 0, 255, 0, 255 });
    CHECK(fbe_gpu_validation_errors() == 0);
}

TEST_CASE("a layer sampling what's drawn into leaves what's under it, by its blend",
          "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t material = unlit(0.25F, 0.25F, 0.25F);
    const float expected_linear = 0.25F;
    wall(scene.world, -3.0F, 10.0F, material);
    require(fbe_material_set_texture(material, 0, scene.target));
    for (const std::int32_t blend :
         { FBE_LAYER_MULTIPLY, FBE_LAYER_ADD, FBE_LAYER_ALPHA, FBE_LAYER_MULTIPLY_2 }) {
        require(fbe_material_set_layer(material, 0, blend, 0));
        CAPTURE(blend);
        CHECK(near(middle(scene.drawn()),
                   { srgb(expected_linear), srgb(expected_linear), srgb(expected_linear) }));
    }
    // A bump layer's gray shifts nothing.
    const std::uint64_t texture = fbe_texture_create(2, 1, FBE_TEXTURE_RGBA8, false);
    const std::array<std::uint8_t, 8> pixels{ 255, 0, 0, 255, 0, 255, 0, 255 };
    require(fbe_texture_write(texture, 0, 0, 2, 1, pixels.data(), 8));
    require(fbe_texture_set_filter(texture, FBE_TEXTURE_NEAREST));
    require(fbe_material_set_color(material, 1.0F, 1.0F, 1.0F, 1.0F));
    require(fbe_material_set_layer(material, 0, FBE_LAYER_BUMP, 0));
    require(fbe_material_set_layer_bump(material, 0, 100.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F));
    require(fbe_material_set_texture(material, 1, texture));
    fbe_texture_release(texture);
    const std::uint64_t small = wall(scene.world, -1.0F, 2.0F, material);
    (void)small;
    CHECK(pixel(scene.drawn(), (frame_size * 3) / 8, frame_size / 2) == rgba_t{ 255, 0, 0, 255 });
    fbe_material_release(material);
}

TEST_CASE("a sky in an order below 0 hides a mirror's reflection", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    cube(scene.world, 0.0F, 2.0F, -8.0F, red);
    fbe_entity_create(scene.world, 0, FBE_ENTITY_MIRROR);
    CHECK(pixel(scene.drawn(), frame_size / 2, 80)[0] == 255);
    const std::uint64_t sky = unlit(0.0F, 0.0F, 1.0F);
    require(fbe_material_set_order(sky, -1));
    wall(scene.world, -50.0F, 200.0F, sky);
    CHECK(pixel(scene.drawn(), frame_size / 2, 80) == rgba_t{ 0, 0, 255, 255 });
    fbe_material_release(red);
    fbe_material_release(sky);
}

TEST_CASE("vertices added let go of a surface's joints", "[gpu][scene]") {
    const std::uint64_t geometry = fbe_geometry_cube();
    const std::int32_t count = fbe_surface_vertex_count(geometry, 0);
    std::vector<fbe_vertex_joints_t> joints(static_cast<std::size_t>(count));
    require(fbe_surface_set_joints(geometry, 0, joints.data(), count));
    CHECK(fbe_surface_joints(geometry, 0, nullptr, 0) == count);
    std::array<fbe_vertex_t, 1> one{};
    REQUIRE(fbe_surface_add_vertices(geometry, 0, one.data(), 1) == count);
    CHECK(fbe_surface_joints(geometry, 0, nullptr, 0) == 0);
    fbe_geometry_release(geometry);
}

TEST_CASE("a wireframe material draws lines, where the device can", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t mesh = wall(scene.world, -3.0F, 0.5F, red);
    (void)mesh;
    // A quarter of the way along the wall's bottom edge's height: inside a triangle, off its lines.
    const auto reds = [&] {
        const fbe::golden::frame_t frame = scene.drawn();
        int count = 0;
        for (std::size_t at = 0; at < frame.rgba.size(); at += 4) {
            count += frame.rgba[at] > 128 ? 1 : 0;
        }
        return count;
    };
    const int filled = reds();
    CHECK(fbe_material_set_wireframe(red, true));
    CHECK(fbe_material_wireframe(red));
    const int lined = reds();
    CHECK(fbe_gpu_validation_errors() == 0);
    if (fbe::gpu::device()->wireframe) {
        CHECK(lined < filled / 2);
        CHECK(lined > 0);
    }
    fbe_material_release(red);
}

TEST_CASE("a geometry edited between two cameras is the next frame's, for both", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t second_target =
        fbe_texture_create_target(frame_size, frame_size, FBE_TEXTURE_RGBA8, 1, true);
    const std::uint64_t second = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
    require(fbe_camera_set_target(second, second_target));
    require(fbe_camera_set_clear_color(second, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t mesh = wall(scene.world, -3.0F, 0.1F, red);
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    require(fbe_render_camera(scene.camera, 1.0F));
    // Moved off to the side, between them: the second draws as the first did.
    std::array<fbe_vertex_t, 4> vertices{};
    require(fbe_surface_vertices(geometry, 0, 0, vertices.data(), 4));
    for (fbe_vertex_t& vertex : vertices) {
        vertex.position.x += 1.0F;
    }
    require(fbe_surface_set_vertices(geometry, 0, 0, vertices.data(), 4));
    require(fbe_render_camera(second, 1.0F));
    CHECK(middle(scene_t::read(scene.target)) == rgba_t{ 255, 0, 0, 255 });
    CHECK(middle(scene_t::read(second_target)) == rgba_t{ 255, 0, 0, 255 });
    // The next frame, both have it.
    fbe_frame_flip(false);
    require(fbe_render_camera(second, 1.0F));
    CHECK(middle(scene_t::read(second_target)) == rgba_t{ 0, 0, 255, 255 });
    fbe_entity_free(second);
    std::array<std::uint64_t, 16> some{};
    while (fbe_entity_freed(some.data(), some.size()) != 0) {}
    fbe_texture_release(second_target);
    fbe_material_release(red);
}

namespace {

/// The pixel a place in the world is at, as a camera sees it.
std::array<std::uint32_t, 2> pixel_at(std::uint64_t camera, float x, float y, float z) {
    fbe_vec3_t found{};
    REQUIRE(fbe_camera_project(camera, frame_size, frame_size, x, y, z, &found) == 1);
    REQUIRE(found.x >= 0.0F);
    REQUIRE(found.y >= 0.0F);
    REQUIRE(found.x < static_cast<float>(frame_size));
    REQUIRE(found.y < static_cast<float>(frame_size));
    return { static_cast<std::uint32_t>(found.x), static_cast<std::uint32_t>(found.y) };
}

rgba_t pixel_of(const fbe::golden::frame_t& frame, std::array<std::uint32_t, 2> at) {
    return pixel(frame, at[0], at[1]);
}

/// A sun from the right, toward -X and -Z, whose shadows are on, of intensity 1.
std::uint64_t sun_from_right(std::uint64_t world) {
    const std::uint64_t sun = light(world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    const float half = std::numbers::pi_v<float> / 8.0F;
    require(
        fbe_entity_set_rotation(sun, 0.0F, std::sin(half), 0.0F, std::cos(half), FBE_SPACE_WORLD));
    require(fbe_light_set_shadows(sun, true));
    return sun;
}

/// What a new material's surface, facing the eye, gives back of a sun of 1 at 45 degrees to it.
float sun_lit() {
    const float halfway = std::cos(std::numbers::pi_v<float> / 8.0F);
    return brdf(1.0F, 0.0F, 1.0F, 0.70710678F, 1.0F, halfway, halfway);
}

} // namespace

TEST_CASE("a directional light's shadow falls where it should, and nowhere else", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -10.0F, 30.0F, paint);
    const std::uint64_t sun = sun_from_right(scene.world);
    // Out of the camera's sight, off to the right, on the sun's way to the wall's middle.
    cube(scene.world, 5.0F, 0.0F, -5.0F, paint);
    const std::uint8_t lit = srgb(sun_lit());
    const fbe::golden::frame_t frame = scene.drawn();
    CHECK(near(middle(frame), { 0, 0, 0 }));
    // On either side, away from its edges, no surface shadows itself.
    int wrong = 0;
    for (std::uint32_t y = 0; y < frame_size; y += 3) {
        for (std::uint32_t x = 0; x < 24; x += 3) {
            wrong += near(pixel(frame, x, y), { lit, lit, lit }) ? 0 : 1;
            wrong += near(pixel(frame, frame_size - 1 - x, y), { lit, lit, lit }) ? 0 : 1;
        }
    }
    CHECK(wrong == 0);
    fbe::golden::check("scene_shadow_sun", frame);
    // With them off, it's all lit.
    require(fbe_light_set_shadows(sun, false));
    CHECK(near(middle(scene.drawn()), { lit, lit, lit }));
    fbe_material_release(paint);
}

TEST_CASE("what's turned to a light isn't shadowed by itself, at any angle", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    const std::uint64_t box = cube(scene.world, 0.0F, 0.0F, -6.0F, paint);
    sun_from_right(scene.world);
    // Its near face, turned, is lit evenly across.
    for (const float turn : { 0.0F, 0.2F, 0.5F }) {
        require(fbe_entity_set_rotation(box, 0.0F, std::sin(turn * 0.5F), 0.0F,
                                        std::cos(turn * 0.5F), FBE_SPACE_WORLD));
        const fbe::golden::frame_t frame = scene.drawn();
        const float eighth = std::numbers::pi_v<float> / 8.0F;
        const std::uint8_t lit =
            srgb(brdf(1.0F, 0.0F, 1.0F, std::cos((eighth * 2.0F) - turn), std::cos(turn),
                      std::cos(eighth - turn), std::cos(eighth)));
        CAPTURE(turn);
        CHECK(near(middle(frame), { lit, lit, lit }));
    }
    fbe_material_release(paint);
}

TEST_CASE("a directional light's cascades reach as far as it says", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_range(scene.camera, 0.5F, 400.0F));
    const std::uint64_t paint = fbe_material_create();
    const std::uint64_t far_wall = wall(scene.world, -150.0F, 400.0F, paint);
    const std::uint64_t sun = sun_from_right(scene.world);
    // A big block, on the sun's way to the far wall's middle, out of sight.
    const std::uint64_t block = cube(scene.world, 100.0F, 0.0F, -50.0F, paint);
    require(fbe_entity_set_scale(block, 8.0F, 8.0F, 8.0F, FBE_SPACE_WORLD));
    require(fbe_light_set_shadow_distance(sun, 300.0F));
    for (const std::int32_t cascades : { 1, 2, 3, 4 }) {
        require(fbe_light_set_cascades(sun, cascades));
        CAPTURE(cascades);
        CHECK(near(middle(scene.drawn()), { 0, 0, 0 }, 3));
    }
    // Past where they reach, it's lit.
    require(fbe_light_set_shadow_distance(sun, 100.0F));
    const std::uint8_t lit = srgb(sun_lit());
    CHECK(near(middle(scene.drawn()), { lit, lit, lit }));
    // Nearer, in the first cascade, a small block's shadow is sharp enough to see.
    require(fbe_light_set_shadow_distance(sun, 100.0F));
    require(fbe_entity_set_position(far_wall, 0.0F, 0.0F, -6.0F, FBE_SPACE_WORLD));
    require(fbe_entity_set_position(block, 3.0F, 0.0F, -3.0F, FBE_SPACE_WORLD));
    require(fbe_entity_set_scale(block, 0.25F, 0.25F, 0.25F, FBE_SPACE_WORLD));
    CHECK(near(middle(scene.drawn()), { 0, 0, 0 }, 3));
    fbe_material_release(paint);
}

TEST_CASE("a spot light's shadow falls from where it is", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_entity_set_position(scene.camera, 4.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -10.0F, 30.0F, paint);
    const std::uint64_t spot = light(scene.world, FBE_LIGHT_SPOT, 0.0F, 0.0F, -2.0F, 30.0F);
    require(fbe_light_set_cone(spot, 0.5F, 0.7F));
    require(fbe_light_set_intensity(spot, 100.0F));
    require(fbe_light_set_shadows(spot, true));
    // On its axis, half way to the wall: out of the camera's sight.
    cube(scene.world, 0.0F, 0.0F, -6.0F, paint);
    const fbe::golden::frame_t frame = scene.drawn();
    CHECK(near(pixel_of(frame, pixel_at(scene.camera, 0.0F, 0.0F, -10.0F)), { 0, 0, 0 }));
    // The cube's shadow's twice its size on the wall, and just past that it's lit.
    CHECK(pixel_of(frame, pixel_at(scene.camera, 0.0F, 3.0F, -10.0F))[0] > 100);
    fbe::golden::check("scene_shadow_spot", frame);
    fbe_material_release(paint);
}

TEST_CASE("a point light's shadows fall on all six of its sides", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    require(fbe_material_set_two_sided(paint, true));
    // A room around it, and a block along each axis.
    const std::uint64_t room = cube(scene.world, 0.0F, 0.0F, 0.0F, paint);
    require(fbe_entity_set_scale(room, 10.0F, 10.0F, 10.0F, FBE_SPACE_WORLD));
    fbe_entity_set_casts_shadows(room, false);
    const std::uint64_t lamp = light(scene.world, FBE_LIGHT_POINT, 0.0F, 0.0F, 0.0F, 30.0F);
    require(fbe_light_set_intensity(lamp, 100.0F));
    require(fbe_light_set_shadows(lamp, true));
    const std::array<float, 3> right{ 1.0F, 0.0F, 0.0F };
    const std::array<float, 3> left{ -1.0F, 0.0F, 0.0F };
    const std::array<float, 3> up{ 0.0F, 1.0F, 0.0F };
    const std::array<float, 3> down{ 0.0F, -1.0F, 0.0F };
    const std::array<float, 3> near_way{ 0.0F, 0.0F, 1.0F };
    const std::array<float, 3> far_way{ 0.0F, 0.0F, -1.0F };
    const std::array ways{ right, left, up, down, near_way, far_way };
    for (const auto& way : ways) {
        const std::uint64_t block =
            cube(scene.world, way[0] * 4.0F, way[1] * 4.0F, way[2] * 4.0F, paint);
        require(fbe_entity_set_scale(block, 0.5F, 0.5F, 0.5F, FBE_SPACE_WORLD));
    }
    for (const auto& way : ways) {
        CAPTURE(way[0], way[1], way[2]);
        // From off to the side of the lamp, looking at what's along that way: where it falls, and a
        // little past that. The block's in the way of nothing, seen from here.
        const float side_x = way[1] != 0.0F ? 3.0F : 0.0F;
        const float side_y = way[1] != 0.0F ? 0.0F : 3.0F;
        const std::array<float, 2> side{ side_x, side_y };
        require(fbe_entity_set_position(scene.camera, side[0], side[1], 0.0F, FBE_SPACE_WORLD));
        // Turned from -Z to the way.
        const float x = way[1];
        const float y = -way[0];
        const float w = -way[2];
        const float size = std::sqrt((x * x) + (y * y) + ((w + 1.0F) * (w + 1.0F)));
        if (way[2] > 0.5F) {
            require(fbe_entity_set_rotation(scene.camera, 0.0F, 1.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
        } else {
            require(fbe_entity_set_rotation(scene.camera, x / size, y / size, 0.0F,
                                            (w + 1.0F) / size, FBE_SPACE_WORLD));
        }
        const fbe::golden::frame_t frame = scene.drawn();
        const std::array<std::uint32_t, 2> dark =
            pixel_at(scene.camera, way[0] * 10.0F, way[1] * 10.0F, way[2] * 10.0F);
        CHECK(near(pixel_of(frame, dark), { 0, 0, 0 }, 3));
        // A way along the wall, toward the camera's side: past the shadow, and lit.
        const std::array<std::uint32_t, 2> lit =
            pixel_at(scene.camera, (way[0] * 10.0F) + (side[0] * 0.8F),
                     (way[1] * 10.0F) + (side[1] * 0.8F), way[2] * 10.0F);
        CHECK(pixel_of(frame, lit)[0] > 60);
    }
    fbe_material_release(paint);
}

TEST_CASE("only what casts does, and only what's lit per pixel has shadows", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    const std::uint64_t receiver = wall(scene.world, -10.0F, 30.0F, paint);
    sun_from_right(scene.world);
    const std::uint64_t caster_paint = fbe_material_create();
    const std::uint64_t block = cube(scene.world, 5.0F, 0.0F, -5.0F, caster_paint);
    const std::uint8_t lit = srgb(sun_lit());
    CHECK(near(middle(scene.drawn()), { 0, 0, 0 }));
    // Told not to.
    CHECK(fbe_entity_casts_shadows(block));
    require(fbe_entity_set_casts_shadows(block, false));
    CHECK_FALSE(fbe_entity_casts_shadows(block));
    CHECK(near(middle(scene.drawn()), { lit, lit, lit }));
    require(fbe_entity_set_casts_shadows(block, true));
    // What blends doesn't cast, nor what's in an order but 0.
    require(fbe_material_set_blend(caster_paint, FBE_BLEND_ALPHA));
    CHECK(near(middle(scene.drawn()), { lit, lit, lit }));
    require(fbe_material_set_blend(caster_paint, FBE_BLEND_AUTO));
    require(fbe_material_set_order(caster_paint, 1));
    CHECK(near(middle(scene.drawn()), { lit, lit, lit }));
    require(fbe_material_set_order(caster_paint, 0));
    // Hidden, it doesn't.
    fbe_entity_set_hidden(block, true);
    CHECK(near(middle(scene.drawn()), { lit, lit, lit }));
    fbe_entity_set_hidden(block, false);
    // A cut-out's hole lets it through.
    const std::uint64_t see_through = flat_texture(255, 255, 255, 0);
    require(fbe_material_set_texture(caster_paint, 0, see_through));
    require(fbe_material_set_alpha_cutoff(caster_paint, 0.5F));
    CHECK(near(middle(scene.drawn()), { lit, lit, lit }));
    const std::uint64_t solid = flat_texture(255, 255, 255, 255);
    require(fbe_material_set_texture(caster_paint, 0, solid));
    CHECK(near(middle(scene.drawn()), { 0, 0, 0 }));
    // VERTEX is the fixed-function look, with none; its light's where it falls.
    const std::uint64_t old = fbe_material_create();
    require(fbe_material_set_shading(old, FBE_SHADING_VERTEX));
    require(fbe_mesh_set_material(receiver, old));
    CHECK(middle(scene.drawn())[0] > 100);
    fbe_texture_release(see_through);
    fbe_texture_release(solid);
    fbe_material_release(old);
    fbe_material_release(paint);
    fbe_material_release(caster_paint);
}

TEST_CASE("a terrain casts, and a skinned mesh where its joints have it", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -10.0F, 30.0F, paint);
    sun_from_right(scene.world);
    // A slab of terrain stood on end, by a turn, between the sun and the wall.
    const std::uint64_t terrain = fbe_entity_create(scene.world, 0, FBE_ENTITY_TERRAIN);
    require(fbe_terrain_set_size(terrain, 32));
    require(fbe_entity_set_scale(terrain, 0.25F, 1.0F, 0.25F, FBE_SPACE_WORLD));
    const float half = std::numbers::pi_v<float> * 0.25F;
    require(fbe_entity_set_rotation(terrain, std::sin(half), 0.0F, 0.0F, std::cos(half),
                                    FBE_SPACE_WORLD));
    require(fbe_entity_set_position(terrain, 1.0F, 4.0F, -5.0F, FBE_SPACE_WORLD));
    const std::uint8_t lit = srgb(sun_lit());
    require(fbe_entity_set_casts_shadows(terrain, false));
    CHECK(near(middle(scene.drawn()), { lit, lit, lit }));
    require(fbe_entity_set_casts_shadows(terrain, true));
    CHECK(near(middle(scene.drawn()), { 0, 0, 0 }, 3));
    fbe_entity_set_hidden(terrain, true);
    // A mesh bound far off, its joint on the sun's way.
    const skinned_wall_t made = skinned_wall(scene.world, paint, 50.0F, 2.0F);
    all_on(made.geometry, 1, 1.0F);
    require(fbe_entity_set_position(made.moving, 5.0F, 0.0F, -5.0F, FBE_SPACE_WORLD));
    const std::uint64_t two_sided = fbe_material_create();
    require(fbe_material_set_two_sided(two_sided, true));
    require(fbe_mesh_set_material(made.mesh, two_sided));
    CHECK(near(middle(scene.drawn()), { 0, 0, 0 }, 3));
    fbe_skin_release(made.skin);
    fbe_material_release(two_sided);
    fbe_material_release(paint);
}

TEST_CASE("more shadows than the atlas holds lose some, with no error", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -10.0F, 30.0F, paint);
    cube(scene.world, 5.0F, 0.0F, -5.0F, paint);
    const std::uint64_t sun = sun_from_right(scene.world);
    require(fbe_light_set_shadow_size(sun, 2048));
    for (int i = 0; i < 12; ++i) {
        const std::uint64_t lamp = light(scene.world, FBE_LIGHT_POINT, 0.0F, 200.0F, -5.0F, 1.0F);
        require(fbe_light_set_shadows(lamp, true));
        require(fbe_light_set_shadow_size(lamp, 8192));
    }
    CHECK(near(middle(scene.drawn()), { 0, 0, 0 }));
    CHECK(fbe_gpu_validation_errors() == 0);
    CHECK_FALSE(fbe_light_set_shadow_size(sun, 8));
    CHECK_FALSE(fbe_light_set_cascades(sun, 5));
    CHECK_FALSE(fbe_light_set_shadow_distance(sun, 0.0F));
    CHECK_FALSE(fbe_light_set_shadow_bias(sun, -1.0F, 0.0F));
    fbe_material_release(paint);
}

TEST_CASE("two cameras each have their shadows in a frame", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -10.0F, 30.0F, paint);
    cube(scene.world, 5.0F, 0.0F, -5.0F, paint);
    sun_from_right(scene.world);
    const std::uint64_t second_target =
        fbe_texture_create_target(frame_size, frame_size, FBE_TEXTURE_RGBA8, 1, true);
    const std::uint64_t second = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
    require(fbe_camera_set_target(second, second_target));
    const fbe::golden::frame_t frame = scene.drawn();
    CHECK(near(middle(frame), { 0, 0, 0 }));
    CHECK(near(middle(scene_t::read(second_target)), { 0, 0, 0 }));
    fbe_entity_free(second);
    std::array<std::uint64_t, 16> some{};
    while (fbe_entity_freed(some.data(), some.size()) != 0) {}
    fbe_texture_release(second_target);
    fbe_material_release(paint);
}

namespace {

/// An emitter, that spawns nothing by itself, at a place, of still white particles.
std::uint64_t still_emitter(std::uint64_t world, float x, float y, float z, float half) {
    const std::uint64_t emitter = fbe_entity_create(world, 0, FBE_ENTITY_PARTICLES);
    require(fbe_particles_set_capacity(emitter, 16));
    require(fbe_particles_set_rate(emitter, 0.0F));
    require(fbe_particles_set_motion(emitter, 0.0F, 0.0F, 0.0F, 0.0F));
    require(fbe_particles_set_sizes(emitter, half, half));
    const fbe_color_t white{ .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F };
    require(fbe_particles_set_colors(emitter, &white, &white));
    require(fbe_particles_set_life(emitter, 10.0F, 10.0F));
    require(fbe_entity_set_position(emitter, x, y, z, FBE_SPACE_WORLD));
    return emitter;
}

/// How many pixels a unit is at a distance of 1, in a new camera's view: 60 degrees up and down.
constexpr float pixels_at_1 = (frame_size / 2.0F) * std::numbers::sqrt3_v<float>;

/// How many pixels aren't black.
int lit_pixels(const fbe::golden::frame_t& frame) {
    int count = 0;
    for (std::size_t at = 0; at < frame.rgba.size(); at += 4) {
        if (frame.rgba[at] != 0 || frame.rgba[at + 1] != 0 || frame.rgba[at + 2] != 0) {
            ++count;
        }
    }
    return count;
}

/// The middle of what's lit, in pixels, x and y.
std::array<float, 2> lit_middle(const fbe::golden::frame_t& frame) {
    float across = 0.0F;
    float down = 0.0F;
    int count = 0;
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            if (pixel(frame, x, y)[0] != 0) {
                across += static_cast<float>(x) + 0.5F;
                down += static_cast<float>(y) + 0.5F;
                ++count;
            }
        }
    }
    require(count != 0);
    return { across / static_cast<float>(count), down / static_cast<float>(count) };
}

/// A sky of four pixels' colors: up to the left and right, down to the left and right.
std::uint64_t sky_texture(rgba_t a, rgba_t b, rgba_t c, rgba_t d) {
    const std::uint64_t texture = four_pixels(a, b, c, d);
    require(fbe_texture_set_filter(texture, FBE_TEXTURE_NEAREST));
    return texture;
}

/// Turns an entity about X, so its -Z comes up by `radians`.
void pitch(std::uint64_t entity, float radians) {
    require(fbe_entity_set_rotation(entity, std::sin(radians / 2.0F), 0.0F, 0.0F,
                                    std::cos(radians / 2.0F), FBE_SPACE_WORLD));
}

} // namespace

TEST_CASE("a particle draws a square facing the camera, as big as its size says", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, -4.0F, 1.0F);
    CHECK(lit_pixels(scene.drawn()) == 0);
    require(fbe_particles_burst(emitter, 1));
    const fbe::golden::frame_t frame = scene.drawn();
    // A half-size of 1 at a depth of 4.
    CHECK(near(middle(frame), { 255, 255, 255 }));
    const auto side = static_cast<int>(pixels_at_1 / 2.0F);
    CHECK(lit_pixels(frame) >= (side - 2) * (side - 2));
    CHECK(lit_pixels(frame) <= (side + 2) * (side + 2));
}

TEST_CASE("a particle moves by its formula: velocity, gravity, and drag", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const float depth = 8.0F;
    const float to_pixels = pixels_at_1 / depth;
    const auto seen_at = [&](float x, float y) {
        const float across = (frame_size / 2.0F) + (x * to_pixels);
        const float down = (frame_size / 2.0F) - (y * to_pixels);
        return std::array{ across, down };
    };
    const auto close_to = [](std::array<float, 2> a, std::array<float, 2> b) {
        return std::fabs(a[0] - b[0]) < 1.0F && std::fabs(a[1] - b[1]) < 1.0F;
    };

    SECTION("gravity, and its velocity along its forward axis") {
        const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, -depth, 0.5F);
        // Its forward axis the world's +X.
        const float half = std::numbers::pi_v<float> / 4.0F;
        require(fbe_entity_set_rotation(emitter, 0.0F, -std::sin(half), 0.0F, std::cos(half),
                                        FBE_SPACE_WORLD));
        require(fbe_particles_set_motion(emitter, 2.0F, 2.0F, 0.0F, 0.0F));
        require(fbe_particles_set_forces(emitter, 0.0F, -4.0F, 0.0F, 0.0F));
        require(fbe_particles_burst(emitter, 1));
        require(fbe_world_step_particles(scene.world, 1.0F));
        CHECK(close_to(lit_middle(scene.drawn()), seen_at(2.0F, -2.0F)));
    }
    SECTION("drag") {
        const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, -depth, 0.5F);
        const float half = std::numbers::pi_v<float> / 4.0F;
        require(fbe_entity_set_rotation(emitter, 0.0F, -std::sin(half), 0.0F, std::cos(half),
                                        FBE_SPACE_WORLD));
        require(fbe_particles_set_motion(emitter, 4.0F, 4.0F, 0.0F, 0.0F));
        require(fbe_particles_set_forces(emitter, 0.0F, -2.0F, 0.0F, 1.0F));
        require(fbe_particles_burst(emitter, 1));
        require(fbe_world_step_particles(scene.world, 2.0F));
        const float left = 1.0F - std::exp(-2.0F);
        CHECK(close_to(lit_middle(scene.drawn()), seen_at(4.0F * left, -2.0F * (2.0F - left))));
    }
}

TEST_CASE("a particle's color and size go from its birth to its death", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, -4.0F, 1.0F);
    require(fbe_particles_set_life(emitter, 2.0F, 2.0F));
    require(fbe_particles_set_sizes(emitter, 1.0F, 0.0F));
    const fbe_color_t red{ .r = 1.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F };
    const fbe_color_t none{ .r = 1.0F, .g = 0.0F, .b = 0.0F, .a = 0.0F };
    require(fbe_particles_set_colors(emitter, &red, &none));
    require(fbe_particles_burst(emitter, 1));
    const int whole = lit_pixels(scene.drawn());
    require(fbe_world_step_particles(scene.world, 1.0F));
    const fbe::golden::frame_t half = scene.drawn();
    // Half its width each way, a quarter of the pixels; half its alpha over black, blended linear.
    CHECK(std::abs(lit_pixels(half) - (whole / 4)) < whole / 16);
    CHECK(near(middle(half), { srgb(0.5F), 0, 0 }));
}

TEST_CASE("what's dead draws nothing, and comes back at no turn of the clock", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, -4.0F, 1.0F);
    require(fbe_particles_set_life(emitter, 1.0F, 1.0F));
    require(fbe_particles_burst(emitter, 1));
    CHECK(lit_pixels(scene.drawn()) != 0);
    require(fbe_world_step_particles(scene.world, 1.5F));
    CHECK(lit_pixels(scene.drawn()) == 0);
    // The clock comes round to when it was born, 1024 seconds on: past its half and its end.
    for (int i = 0; i < 3; ++i) {
        require(fbe_world_step_particles(scene.world, 341.0F));
        (void)scene.drawn();
    }
    require(fbe_world_step_particles(scene.world, 0.0F));
    CHECK(lit_pixels(scene.drawn()) == 0);
    require(fbe_world_step_particles(scene.world, 0.75F));
    CHECK(lit_pixels(scene.drawn()) == 0);
    // And one born across the clock's turn still shows, its age right.
    require(fbe_world_step_particles(scene.world, 1020.0F));
    (void)scene.drawn();
    require(fbe_particles_set_life(emitter, 10.0F, 10.0F));
    require(fbe_particles_burst(emitter, 1));
    require(fbe_world_step_particles(scene.world, 8.0F));
    CHECK(lit_pixels(scene.drawn()) != 0);
}

TEST_CASE("a particle stays where it was born when its emitter moves on", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, -8.0F, 1.0F);
    require(fbe_particles_burst(emitter, 1));
    (void)scene.drawn();
    require(fbe_entity_set_position(emitter, 4.0F, 0.0F, -8.0F, FBE_SPACE_WORLD));
    const std::array<float, 2> at = lit_middle(scene.drawn());
    CHECK(std::fabs(at[0] - (frame_size / 2.0F)) < 1.0F);
}

TEST_CASE("a step's particles spawn along where the emitter went", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t emitter = still_emitter(scene.world, -4.0F, 0.0F, -8.0F, 0.25F);
    require(fbe_particles_set_rate(emitter, 4.0F));
    require(fbe_world_step_particles(scene.world, 0.0F));
    require(fbe_entity_set_position(emitter, 4.0F, 0.0F, -8.0F, FBE_SPACE_WORLD));
    require(fbe_world_step_particles(scene.world, 1.0F));
    const fbe::golden::frame_t frame = scene.drawn();
    // Evenly: at -3, -1, 1, and 3, and none at the middle.
    const auto at = [&](float x) {
        return pixel(frame, static_cast<std::uint32_t>(64.0F + (x * pixels_at_1 / 8.0F)), 64)[0];
    };
    CHECK(at(-3.0F) == 255);
    CHECK(at(-1.0F) == 255);
    CHECK(at(1.0F) == 255);
    CHECK(at(3.0F) == 255);
    CHECK(at(0.0F) == 0);
    CHECK(at(-2.0F) == 0);
}

TEST_CASE("drawing between steps shows what's born only once it has been", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, -4.0F, 1.0F);
    require(fbe_particles_set_rate(emitter, 2.0F));
    require(fbe_world_capture(scene.world));
    require(fbe_world_step_particles(scene.world, 1.0F));
    // One at a quarter of the step and one at three quarters.
    CHECK(lit_pixels(scene.drawn(0.0F)) == 0);
    CHECK(lit_pixels(scene.drawn(0.5F)) != 0);
    CHECK(lit_pixels(scene.drawn(1.0F)) != 0);
}

TEST_CASE("an emitter out of view has no draw, and comes back with what it had", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, 10.0F, 1.0F);
    require(fbe_particles_burst(emitter, 1));
    std::array<fbe_draw_t, 4> draws{};
    const auto count = [&] {
        return fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, draws.data(), 4);
    };
    CHECK(count() == 0);
    (void)scene.drawn();
    // Its box moves as what's born goes, and the camera can turn to it.
    pitch(scene.camera, std::numbers::pi_v<float>);
    CHECK(count() == 1);
    CHECK(draws[0].kind == FBE_ENTITY_PARTICLES);
    CHECK(near(middle(scene.drawn()), { 255, 255, 255 }));
}

TEST_CASE("particles take a material's textures and blend, and fog", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, -4.0F, 1.0F);
    require(fbe_particles_burst(emitter, 1));

    SECTION("AUTO's alpha, the square's color and texture together") {
        const std::uint64_t material = fbe_material_create();
        require(fbe_material_set_shading(material, FBE_SHADING_UNLIT));
        const std::uint64_t texture = flat_texture(255, 0, 0, 128);
        require(fbe_material_set_texture(material, 0, texture));
        require(fbe_particles_set_material(emitter, material));
        const rgba_t seen = middle(scene.drawn());
        CHECK(seen[0] > 100);
        CHECK(seen[2] > 100);
        fbe_texture_release(texture);
        fbe_material_release(material);
    }
    SECTION("ADD") {
        const std::uint64_t material = unlit(1.0F, 0.0F, 0.0F);
        require(fbe_material_set_blend(material, FBE_BLEND_ADD));
        require(fbe_particles_set_material(emitter, material));
        CHECK(near(middle(scene.drawn()), { 255, 0, 255 }));
        fbe_material_release(material);
    }
    SECTION("OPAQUE, its alpha left out, and drawing depth") {
        const std::uint64_t material = unlit(0.0F, 1.0F, 0.0F);
        require(fbe_material_set_blend(material, FBE_BLEND_OPAQUE));
        require(fbe_particles_set_material(emitter, material));
        const fbe_color_t faint{ .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 0.1F };
        require(fbe_particles_set_colors(emitter, &faint, &faint));
        CHECK(near(middle(scene.drawn()), { 0, 255, 0 }));
        fbe_material_release(material);
    }
    SECTION("a mesh in front covers them, whatever the order they're made in") {
        const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
        wall(scene.world, -2.0F, 4.0F, red);
        CHECK(near(middle(scene.drawn()), { 255, 0, 0 }));
        fbe_material_release(red);
    }
}

TEST_CASE("a mirror's reflection has particles too", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 2.0F, -8.0F, 1.0F);
    require(fbe_particles_burst(emitter, 1));
    fbe_entity_create(scene.world, 0, FBE_ENTITY_MIRROR);
    const fbe::golden::frame_t frame = scene.drawn();
    CHECK(near(pixel(frame, 64, 48), { 255, 255, 255 }));
    CHECK(near(pixel(frame, 64, 80), { 255, 255, 255 }));
}

TEST_CASE("an emitter freed lets its buffer go, and a new one on its slot starts clean",
          "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t first = still_emitter(scene.world, 0.0F, 0.0F, -4.0F, 1.0F);
    require(fbe_particles_burst(first, 8));
    CHECK(lit_pixels(scene.drawn()) != 0);
    require(fbe_entity_free(first));
    (void)scene.drawn();
    const std::uint64_t second = still_emitter(scene.world, 0.0F, 0.0F, -4.0F, 1.0F);
    require(fbe_particles_burst(second, 1));
    require(fbe_entity_set_position(second, 0.0F, 0.0F, -4.0F, FBE_SPACE_WORLD));
    CHECK(lit_pixels(scene.drawn()) != 0);
    // Set to hold another many, its old ones are gone.
    require(fbe_particles_set_capacity(second, 32));
    CHECK(lit_pixels(scene.drawn()) == 0);
    require(fbe_entity_free(second));
    (void)scene.drawn();
}

TEST_CASE("a million particles draw", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, -4.0F, 0.5F);
    require(fbe_particles_set_capacity(emitter, 1048576));
    // What it holds is a million quads' worth of draw; what's born's a few.
    require(fbe_particles_burst(emitter, 1));
    CHECK(fbe_particles_alive(emitter) == 1);
    CHECK(near(middle(scene.drawn()), { 255, 255, 255 }));
}

TEST_CASE("a texture's sky is behind what's opaque, along each pixel's way", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const rgba_t red{ 255, 0, 0, 255 };
    const rgba_t green{ 0, 255, 0, 255 };
    const rgba_t blue{ 0, 0, 255, 255 };
    const rgba_t white{ 255, 255, 255, 255 };
    const std::uint64_t texture = sky_texture(red, green, blue, white);
    require(fbe_world_set_sky_texture(scene.world, texture, 1.0F));
    const fbe::golden::frame_t frame = scene.drawn();
    // Straight ahead's the texture's middle: up to the left, up to the right, and so on.
    CHECK(near(pixel(frame, 32, 32), { 255, 0, 0 }));
    CHECK(near(pixel(frame, 96, 32), { 0, 255, 0 }));
    CHECK(near(pixel(frame, 32, 96), { 0, 0, 255 }));
    CHECK(near(pixel(frame, 96, 96), { 255, 255, 255 }));
    // Turned around, what's up to the left is behind where it was, so across the back's the seam.
    SECTION("turned") {
        require(fbe_entity_set_rotation(scene.camera, 0.0F, 1.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
        const fbe::golden::frame_t behind = scene.drawn();
        CHECK(near(pixel(behind, 32, 32), { 0, 255, 0 }));
        CHECK(near(pixel(behind, 96, 32), { 255, 0, 0 }));
    }
    SECTION("what's opaque covers it, what blends goes over it, and an order below 0 is under it") {
        const std::uint64_t solid = unlit(1.0F, 1.0F, 0.0F);
        wall(scene.world, -4.0F, 1.0F, solid);
        const std::uint64_t over = unlit(0.0F, 0.0F, 0.0F, 0.5F);
        require(fbe_material_set_blend(over, FBE_BLEND_ALPHA));
        const std::uint64_t pane = wall(scene.world, -8.0F, 200.0F, over);
        (void)pane;
        const std::uint64_t under = unlit(1.0F, 0.0F, 1.0F);
        require(fbe_material_set_order(under, -1));
        wall(scene.world, -30.0F, 200.0F, under);
        const fbe::golden::frame_t seen = scene.drawn();
        CHECK(near(middle(seen), { 255, 255, 0 }));
        // Half of it left in encoded values at most: red's dimmed, and none of magenta's blue.
        const rgba_t dimmed = pixel(seen, 16, 16);
        CHECK(dimmed[0] > 0);
        CHECK(dimmed[0] < 255);
        CHECK(dimmed[2] == 0);
        fbe_material_release(solid);
        fbe_material_release(over);
        fbe_material_release(under);
    }
    SECTION("clearing it goes back to the clear color") {
        require(fbe_world_clear_sky(scene.world));
        CHECK(lit_pixels(scene.drawn()) == 0);
    }
    SECTION("its brightness scales it") {
        require(fbe_world_set_sky_texture(scene.world, texture, 0.0F));
        CHECK(lit_pixels(scene.drawn()) == 0);
    }
    fbe_texture_release(texture);
}

TEST_CASE("a sky keeps to its camera's viewport", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t texture = flat_texture(255, 255, 255, 255);
    require(fbe_world_set_sky_texture(scene.world, texture, 1.0F));
    require(fbe_camera_set_viewport(scene.camera, 0.0F, 0.0F, 64.0F, 128.0F));
    const fbe::golden::frame_t frame = scene.drawn();
    CHECK(near(pixel(frame, 32, 64), { 255, 255, 255 }));
    CHECK(near(pixel(frame, 96, 64), { 0, 0, 0 }));
    fbe_texture_release(texture);
}

TEST_CASE("a sky holds its texture, and one that's its camera's target isn't sampled",
          "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t texture = flat_texture(0, 255, 0, 255);
    require(fbe_world_set_sky_texture(scene.world, texture, 1.0F));
    // The sky has it now, whatever the game does.
    fbe_texture_release(texture);
    CHECK(near(middle(scene.drawn()), { 0, 255, 0 }));
    require(fbe_world_set_sky_texture(scene.world, scene.target, 1.0F));
    // Draws what can't be sampled as white, and no validation error.
    CHECK(near(middle(scene.drawn()), { 255, 255, 255 }));
    require(fbe_world_clear_sky(scene.world));
}

TEST_CASE("through a mirror's plane is its reflection of the sky", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const rgba_t red{ 255, 0, 0, 255 };
    const rgba_t blue{ 0, 0, 255, 255 };
    const std::uint64_t texture = sky_texture(red, red, blue, blue);
    require(fbe_world_set_sky_texture(scene.world, texture, 1.0F));
    const std::uint64_t mirror = fbe_entity_create(scene.world, 0, FBE_ENTITY_MIRROR);
    require(fbe_entity_set_position(mirror, 0.0F, -1.0F, 0.0F, FBE_SPACE_WORLD));
    const fbe::golden::frame_t frame = scene.drawn();
    // Above: the sky's top; below the mirror's horizon: its top again, reflected, not its bottom.
    CHECK(near(pixel(frame, 64, 16), { 255, 0, 0 }));
    CHECK(near(pixel(frame, 64, 112), { 255, 0, 0 }));
    // With no mirror, its bottom.
    require(fbe_entity_free(mirror));
    CHECK(near(pixel(scene.drawn(), 64, 112), { 0, 0, 255 }));
    fbe_texture_release(texture);
}

TEST_CASE("the atmosphere is blue above, bright by the sun, and dark once it's set",
          "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    require(fbe_world_set_sky_atmosphere(scene.world, 0, 20.0F));
    pitch(scene.camera, std::numbers::pi_v<float> / 4.0F);
    const rgba_t noon = middle(scene.drawn());
    CHECK(noon[2] > noon[1]);
    CHECK(noon[1] > noon[0]);
    CHECK(noon[3] == 255);
    const std::uint64_t sun = fbe_entity_create(scene.world, 0, FBE_ENTITY_LIGHT);
    require(fbe_light_set_type(sun, FBE_LIGHT_DIRECTIONAL));

    SECTION("the sun low ahead makes the sky by it warmer and brighter than the sky behind it") {
        // Low ahead along the camera's -Z, so shining back along +Z and a little down.
        pitch(sun, std::numbers::pi_v<float> + 0.1F);
        require(fbe_world_set_sky_atmosphere(scene.world, sun, 20.0F));
        pitch(scene.camera, 0.1F);
        const rgba_t toward = middle(scene.drawn());
        pitch(scene.camera, std::numbers::pi_v<float> - 0.1F);
        const rgba_t away = middle(scene.drawn());
        CHECK(toward[0] > away[0]);
        CHECK(static_cast<float>(toward[0]) / static_cast<float>(toward[2] + 1) >
              static_cast<float>(away[0]) / static_cast<float>(away[2] + 1));
    }
    SECTION("the sun under the ground leaves it dark") {
        // Shining up.
        pitch(sun, 1.0F);
        require(fbe_world_set_sky_atmosphere(scene.world, sun, 20.0F));
        const rgba_t night = middle(scene.drawn());
        CHECK(night[2] < noon[2] / 4);
    }
}

TEST_CASE("a decal draws on what it was made of, in a blended material", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t white = unlit(1.0F, 1.0F, 1.0F);
    wall(scene.world, -4.0F, 4.0F, white);
    // A half-size of 1 at a depth of 4: about 28 pixels each way around the middle.
    std::array<float, 16> box{};
    box[0] = 1.0F;
    box[5] = 1.0F;
    box[10] = 1.0F;
    box[12] = 0.0F;
    box[13] = 0.0F;
    box[14] = -4.0F;
    box[15] = 1.0F;
    fbe_mat4_t matrix{};
    std::ranges::copy(box, matrix.m);
    const std::uint64_t geometry = fbe_geometry_decal(scene.world, &matrix, 0.01F, 1.309F);
    REQUIRE(geometry != 0);
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F, 0.5F);
    require(fbe_material_set_blend(red, FBE_BLEND_ALPHA));
    const std::uint64_t mesh = fbe_entity_create(scene.world, 0, FBE_ENTITY_MESH);
    require(fbe_mesh_set_geometry(mesh, geometry));
    require(fbe_mesh_set_material(mesh, red));
    const fbe::golden::frame_t frame = scene.drawn();
    const rgba_t on = middle(frame);
    CHECK(on[0] == 255);
    CHECK(on[1] < 255);
    CHECK(on[1] > 0);
    CHECK(near(pixel(frame, 64 + 40, 64), { 255, 255, 255 }));
    fbe_geometry_release(geometry);
    fbe_material_release(red);
    fbe_material_release(white);
}

namespace {

/// How many pixels' red is above a value.
int reds_above(const fbe::golden::frame_t& frame, int above) {
    int count = 0;
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const int red = pixel(frame, x, y)[0];
            count += red > above ? 1 : 0;
        }
    }
    return count;
}

/// Turns an entity by an angle about an axis of length 1.
void turn_about(std::uint64_t entity, float x, float y, float z, float angle) {
    const float half = angle * 0.5F;
    const float part = std::sin(half);
    require(fbe_entity_set_rotation(entity, x * part, y * part, z * part, std::cos(half),
                                    FBE_SPACE_WORLD));
}

/// A floor facing +Y of many small squares, so no one triangle's big.
std::uint64_t grid_floor(std::uint64_t world, float y, int cells, float cell,
                         std::uint64_t material) {
    const std::uint64_t geometry = fbe_geometry_create();
    const std::int32_t surface = fbe_geometry_add_surface(geometry);
    std::vector<fbe_vertex_t> vertices;
    std::vector<std::uint32_t> indices;
    const float half = static_cast<float>(cells) * cell * 0.5F;
    for (int z = 0; z <= cells; ++z) {
        for (int x = 0; x <= cells; ++x) {
            const float across = (static_cast<float>(x) * cell) - half;
            const float along = (static_cast<float>(z) * cell) - half;
            vertices.push_back({
                .position = { .x = across, .y = 0.0F, .z = along },
                .normal = { .x = 0.0F, .y = 1.0F, .z = 0.0F },
                .uv0 = { .x = 0.0F, .y = 0.0F },
                .uv1 = { .x = 0.0F, .y = 0.0F },
                .color = { .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F },
            });
        }
    }
    const auto squares = static_cast<std::uint32_t>(cells);
    const std::uint32_t row = squares + 1;
    for (std::uint32_t z = 0; z < squares; ++z) {
        for (std::uint32_t x = 0; x < squares; ++x) {
            const std::uint32_t a = (z * row) + x;
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + row;
            const std::uint32_t d = c + 1;
            indices.insert(indices.end(), { a, c, d, a, d, b });
        }
    }
    fbe_surface_add_vertices(geometry, surface, vertices.data(),
                             static_cast<std::int32_t>(vertices.size()));
    fbe_surface_add_triangles(geometry, surface, indices.data(),
                              static_cast<std::int32_t>(indices.size()));
    const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    require(fbe_mesh_set_geometry(mesh, geometry));
    fbe_geometry_release(geometry);
    require(fbe_mesh_set_material(mesh, material));
    require(fbe_entity_set_position(mesh, 0.0F, y, 0.0F, FBE_SPACE_WORLD));
    return mesh;
}

/// A wall whose vertices all have this alpha.
std::uint64_t alpha_wall(std::uint64_t world, float x, float z, float half, float alpha,
                         std::uint64_t material, float uv_across = 0.0F) {
    const std::uint64_t geometry = fbe_geometry_create();
    const std::int32_t surface = fbe_geometry_add_surface(geometry);
    const auto corner = [&](float across, float up) {
        return fbe_vertex_t{
            .position = { .x = across * half, .y = up * half, .z = 0.0F },
            .normal = { .x = 0.0F, .y = 0.0F, .z = 1.0F },
            .uv0 = { .x = (across + 1.0F) * 0.5F * uv_across, .y = (up + 1.0F) * 0.5F * uv_across },
            .uv1 = { .x = 0.0F, .y = 0.0F },
            .color = { .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = alpha },
        };
    };
    const fbe_vertex_t low_left = corner(-1.0F, -1.0F);
    const fbe_vertex_t low_right = corner(1.0F, -1.0F);
    const fbe_vertex_t high_right = corner(1.0F, 1.0F);
    const fbe_vertex_t high_left = corner(-1.0F, 1.0F);
    const std::array vertices{ low_left, low_right, high_right, high_left };
    fbe_surface_add_vertices(geometry, surface, vertices.data(), 4);
    const std::array<std::uint32_t, 6> indices{ 0, 1, 2, 0, 2, 3 };
    fbe_surface_add_triangles(geometry, surface, indices.data(), 6);
    const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    require(fbe_mesh_set_geometry(mesh, geometry));
    fbe_geometry_release(geometry);
    require(fbe_mesh_set_material(mesh, material));
    require(fbe_entity_set_position(mesh, x, 0.0F, z, FBE_SPACE_WORLD));
    return mesh;
}

/// A wall, a block on the sun's way to its middle, and the sun: where a shadow's easy to look for.
struct shadowed_wall_t {
    const scene_t& scene;
    std::uint64_t paint = fbe_material_create();
    std::uint64_t sun = 0;

    explicit shadowed_wall_t(const scene_t& in) : scene(in) {
        wall(scene.world, -10.0F, 30.0F, paint);
        cube(scene.world, 5.0F, 0.0F, -5.0F, paint);
        sun = sun_from_right(scene.world);
    }
    shadowed_wall_t(const shadowed_wall_t&) = delete;
    shadowed_wall_t& operator=(const shadowed_wall_t&) = delete;
    shadowed_wall_t(shadowed_wall_t&&) = delete;
    shadowed_wall_t& operator=(shadowed_wall_t&&) = delete;

    ~shadowed_wall_t() {
        fbe_material_release(paint);
    }

    /// Whether the wall's middle's in shadow.
    [[nodiscard]] bool shadowed() const {
        return middle(scene.drawn())[0] < srgb(sun_lit()) / 2;
    }
};

} // namespace

TEST_CASE("a big caster keeps its shadow, however far past the near plane it goes",
          "[gpu][scene]") {
    SECTION("a point light's, flat to a face and across one") {
        const float half = GENERATE(5.0F, 8.0F, 20.0F, 100.0F);
        const scene_t scene;
        const std::uint64_t paint = fbe_material_create();
        wall(scene.world, -6.0F, 200.0F, paint);
        wall(scene.world, -3.0F, half, paint);
        const std::uint64_t lamp = light(scene.world, FBE_LIGHT_POINT, 0.0F, 0.0F, 0.0F, 100.0F);
        require(fbe_light_set_intensity(lamp, 300.0F));
        require(fbe_light_set_shadows(lamp, true));
        require(fbe_camera_set_range(scene.camera, 0.1F, 50.0F));
        require(fbe_entity_set_position(scene.camera, 3.0F, 0.0F, -4.0F, FBE_SPACE_WORLD));
        CHECK(middle(scene.drawn())[0] == 0);
        if (half > 4.0F) {
            require(fbe_entity_set_position(scene.camera, 8.0F, 0.0F, -4.0F, FBE_SPACE_WORLD));
            CHECK(middle(scene.drawn())[0] == 0);
        }
        fbe_material_release(paint);
    }
    SECTION("a spot light's, the wall's far end behind it") {
        const float half = GENERATE(12.0F, 40.0F, 100.0F);
        const scene_t scene;
        const std::uint64_t paint = fbe_material_create();
        wall(scene.world, -6.0F, 200.0F, paint);
        wall(scene.world, -3.0F, half, paint);
        const std::uint64_t spot = light(scene.world, FBE_LIGHT_SPOT, 0.0F, 0.0F, 0.0F, 100.0F);
        require(fbe_light_set_intensity(spot, 300.0F));
        require(fbe_light_set_cone(spot, 0.3F, 0.5F));
        require(fbe_light_set_shadows(spot, true));
        const float sixty = std::numbers::pi_v<float> / 3.0F;
        turn_about(spot, 0.0F, 1.0F, 0.0F, -sixty);
        require(fbe_camera_set_range(scene.camera, 0.1F, 50.0F));
        require(fbe_entity_set_position(scene.camera, 6.0F * std::tan(sixty), 0.0F, -4.0F,
                                        FBE_SPACE_WORLD));
        CHECK(middle(scene.drawn())[0] == 0);
        fbe_material_release(paint);
    }
    SECTION("a directional light's, far bigger than its cascade") {
        const float half = GENERATE(6.0F, 20.0F, 100.0F, 1000.0F);
        const scene_t scene;
        const std::uint64_t paint = fbe_material_create();
        wall(scene.world, -10.0F, 2000.0F, paint);
        wall(scene.world, -5.0F, half, paint);
        require(fbe_entity_set_position(scene.camera, 0.0F, 0.0F, -6.0F, FBE_SPACE_WORLD));
        sun_from_right(scene.world);
        CHECK(middle(scene.drawn())[0] == 0);
        fbe_material_release(paint);
    }
    SECTION("far up the sun's way, past its tile's near plane altogether") {
        const scene_t scene;
        const std::uint64_t paint = fbe_material_create();
        wall(scene.world, -10.0F, 30.0F, paint);
        sun_from_right(scene.world);
        cube(scene.world, 505.0F, 0.0F, 495.0F, paint);
        CHECK(middle(scene.drawn())[0] == 0);
        fbe_material_release(paint);
    }
    SECTION("with no depth clamp, each cascade's near plane goes back to what casts") {
        const scene_t scene;
        require(fbe_gpu_started() || fbe_gpu_start());
        fbe::gpu::device()->depth_clamp = false;
        const std::uint64_t paint = fbe_material_create();
        wall(scene.world, -10.0F, 2000.0F, paint);
        wall(scene.world, -5.0F, 100.0F, paint);
        require(fbe_entity_set_position(scene.camera, 0.0F, 0.0F, -6.0F, FBE_SPACE_WORLD));
        sun_from_right(scene.world);
        CHECK(middle(scene.drawn())[0] == 0);
        fbe::gpu::device()->depth_clamp = true;
        fbe_material_release(paint);
    }
}

TEST_CASE("a directional light's cascades end where its shadows do", "[gpu][scene]") {
    SECTION("past them, it's lit, not by the next light's") {
        const scene_t scene;
        require(fbe_camera_set_range(scene.camera, 0.5F, 400.0F));
        const std::uint64_t paint = fbe_material_create();
        wall(scene.world, -60.0F, 300.0F, paint);
        const std::uint64_t first = sun_from_right(scene.world);
        require(fbe_light_set_shadow_distance(first, 10.0F));
        const std::uint64_t second =
            light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
        turn_about(second, 0.0F, 1.0F, 0.0F, -std::numbers::pi_v<float> / 4.0F);
        require(fbe_light_set_shadows(second, true));
        require(fbe_light_set_shadow_distance(second, 300.0F));
        const std::uint64_t block = cube(scene.world, -20.0F, 0.0F, -40.0F, paint);
        require(fbe_entity_set_scale(block, 8.0F, 8.0F, 8.0F, FBE_SPACE_WORLD));
        // The first's shadows end at 10; the wall's at 60, in only the second's shadow.
        CHECK(near(middle(scene.drawn()), { srgb(sun_lit()), srgb(sun_lit()), srgb(sun_lit()) }));
        require(fbe_light_set_shadows(second, false));
        CHECK(near(middle(scene.drawn()),
                   { srgb(sun_lit() * 2.0F), srgb(sun_lit() * 2.0F), srgb(sun_lit() * 2.0F) }));
        fbe_material_release(paint);
    }
    SECTION("the last of them, not the camera's record after it") {
        const scene_t scene;
        require(fbe_camera_set_range(scene.camera, 0.1F, 100.0F));
        const std::uint64_t paint = fbe_material_create();
        wall(scene.world, -0.8F, 30.0F, paint);
        const std::uint64_t sun = sun_from_right(scene.world);
        require(fbe_light_set_shadow_distance(sun, 0.5F));
        const std::uint64_t block = cube(scene.world, 6.0F, 0.0F, 5.2F, paint);
        require(fbe_entity_set_scale(block, 4.0F, 4.0F, 4.0F, FBE_SPACE_WORLD));
        const fbe::golden::frame_t frame = scene.drawn();
        CHECK(reds_above(frame, srgb(sun_lit()) - 5) == frame_size * frame_size);
        fbe_material_release(paint);
    }
    SECTION("ending before the near plane, it has none, and no slices running backward") {
        const scene_t scene;
        const shadowed_wall_t over(scene);
        require(fbe_camera_set_range(scene.camera, 2.0F, 100.0F));
        require(fbe_light_set_shadow_distance(over.sun, 1.0F));
        CHECK_FALSE(over.shadowed());
        CHECK(fbe_gpu_validation_errors() == 0);
    }
}

TEST_CASE("a terrain doesn't shadow itself, however coarsely it casts", "[gpu][scene]") {
    const std::int32_t size = GENERATE(128, 256, 512, 1024);
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    const std::uint64_t terrain = fbe_entity_create(scene.world, 0, FBE_ENTITY_TERRAIN);
    require(fbe_terrain_set_size(terrain, size));
    // Rolling hills, a wave of 40 squares, 4 high: slopes under 20 degrees, a sun at 60.
    std::vector<float> heights;
    for (std::int32_t z = 0; z <= size; ++z) {
        for (std::int32_t x = 0; x <= size; ++x) {
            heights.push_back(2.0F * std::sin(static_cast<float>(x) * 0.157F) *
                              std::cos(static_cast<float>(z) * 0.157F));
        }
    }
    require(fbe_terrain_set_heights(terrain, heights.data(),
                                    static_cast<std::int32_t>(heights.size())));
    require(fbe_terrain_set_material(terrain, paint));
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    turn_about(sun, 1.0F, 0.0F, 0.0F, -std::numbers::pi_v<float> / 3.0F);
    require(fbe_light_set_shadows(sun, true));
    require(fbe_camera_set_range(scene.camera, 0.1F, 300.0F));
    const float middle_place = static_cast<float>(size) * 0.5F;
    require(
        fbe_entity_set_position(scene.camera, middle_place, 12.0F, middle_place, FBE_SPACE_WORLD));
    turn_about(scene.camera, 1.0F, 0.0F, 0.0F, -1.2F);
    const fbe::golden::frame_t with = scene.drawn();
    require(fbe_light_set_shadows(sun, false));
    const fbe::golden::frame_t without = scene.drawn();
    CHECK(fbe::golden::compare(without, with, 12).pixels < 200);
    fbe_material_release(paint);
}

TEST_CASE("a terrain of any size casts, each of its pieces", "[gpu][scene]") {
    SECTION("whole") {
        const std::int32_t size = GENERATE(1, 8, 15, 16, 17, 20, 32, 100);
        const scene_t scene;
        const std::uint64_t paint = fbe_material_create();
        wall(scene.world, -10.0F, 30.0F, paint);
        sun_from_right(scene.world);
        const std::uint64_t terrain = fbe_entity_create(scene.world, 0, FBE_ENTITY_TERRAIN);
        require(fbe_terrain_set_size(terrain, size));
        const float scale = 8.0F / static_cast<float>(size);
        require(fbe_entity_set_scale(terrain, scale, 1.0F, scale, FBE_SPACE_WORLD));
        const float half = std::numbers::pi_v<float> * 0.25F;
        require(fbe_entity_set_rotation(terrain, std::sin(half), 0.0F, 0.0F, std::cos(half),
                                        FBE_SPACE_WORLD));
        require(fbe_entity_set_position(terrain, 1.0F, 4.0F, -5.0F, FBE_SPACE_WORLD));
        CHECK(middle(scene.drawn())[0] == 0);
        fbe_material_release(paint);
    }
    SECTION("only its last corner on the sun's way") {
        const std::int32_t size = GENERATE(20, 100, 150);
        const scene_t scene;
        const std::uint64_t paint = fbe_material_create();
        wall(scene.world, -10.0F, 30.0F, paint);
        sun_from_right(scene.world);
        const std::uint64_t terrain = fbe_entity_create(scene.world, 0, FBE_ENTITY_TERRAIN);
        require(fbe_terrain_set_size(terrain, size));
        const float half = std::numbers::pi_v<float> * 0.25F;
        require(fbe_entity_set_rotation(terrain, std::sin(half), 0.0F, 0.0F, std::cos(half),
                                        FBE_SPACE_WORLD));
        // Stood on its edge, mirrored so all the rest of it's off the camera's way, with only its
        // last squares, (size, size), around where the sun's way to the wall's middle crosses.
        const auto far_corner = static_cast<float>(size);
        require(fbe_entity_set_scale(terrain, -1.0F, 1.0F, 1.0F, FBE_SPACE_WORLD));
        require(fbe_entity_set_position(terrain, 4.0F + far_corner, far_corner - 1.0F, -5.0F,
                                        FBE_SPACE_WORLD));
        CHECK(middle(scene.drawn())[0] == 0);
        fbe_material_release(paint);
    }
}

TEST_CASE("an atlas full of some lights' tiles keeps what it can of the others'", "[gpu][scene]") {
    SECTION("a sun alone, too big for it, shrinks") {
        const scene_t scene;
        const shadowed_wall_t over(scene);
        require(fbe_light_set_shadow_size(over.sun, 8192));
        CHECK(over.shadowed());
    }
    SECTION("a lamp too big doesn't take the sun's place") {
        const scene_t scene;
        const shadowed_wall_t over(scene);
        const std::uint64_t lamp = light(scene.world, FBE_LIGHT_POINT, 0.0F, 0.0F, -9.0F, 3.0F);
        require(fbe_light_set_intensity(lamp, 0.0001F));
        require(fbe_light_set_shadows(lamp, true));
        require(fbe_light_set_shadow_size(lamp, 8192));
        CHECK(over.shadowed());
    }
    SECTION("lamps out of reach of what's seen have no tiles at all") {
        const scene_t scene;
        const shadowed_wall_t over(scene);
        for (int i = 0; i < 12; ++i) {
            const std::uint64_t lamp =
                light(scene.world, FBE_LIGHT_POINT, 500.0F + (static_cast<float>(i) * 50.0F), 0.0F,
                      0.0F, 5.0F);
            require(fbe_light_set_shadows(lamp, true));
            require(fbe_light_set_shadow_size(lamp, 2048));
        }
        CHECK(over.shadowed());
        CHECK(fbe::render::shadow_atlas_side() == 2048);
    }
    SECTION("the nearest lamp keeps its shadow, wherever it is in the tree") {
        const scene_t scene;
        const std::uint64_t paint = fbe_material_create();
        wall(scene.world, -10.0F, 60.0F, paint);
        // Far off in the view, each taking all its share of the atlas, and made first.
        for (int i = 0; i < 12; ++i) {
            const std::uint64_t far =
                light(scene.world, FBE_LIGHT_POINT, (static_cast<float>(i) - 4.0F) * 12.0F, 0.0F,
                      -60.0F, 6.0F);
            require(fbe_light_set_shadows(far, true));
            require(fbe_light_set_shadow_size(far, 1024));
        }
        cube(scene.world, 2.0F, 0.0F, -7.0F, paint);
        require(fbe_entity_set_position(scene.camera, 4.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
        const std::uint64_t near_lamp =
            light(scene.world, FBE_LIGHT_POINT, 0.0F, 0.0F, -4.0F, 20.0F);
        require(fbe_light_set_intensity(near_lamp, 30.0F));
        require(fbe_light_set_shadows(near_lamp, true));
        require(fbe_light_set_shadow_size(near_lamp, 1024));
        CHECK(middle(scene.drawn())[0] == 0);
        CHECK(fbe_gpu_validation_errors() == 0);
        fbe_material_release(paint);
    }
}

TEST_CASE("a morphed mesh casts where its morph has it", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -10.0F, 30.0F, paint);
    sun_from_right(scene.world);
    // Well above the sun's way to the wall's middle, until its morph brings it down.
    const std::uint64_t quad = wall(scene.world, -5.0F, 3.0F, paint);
    require(fbe_entity_set_position(quad, 5.0F, 30.0F, -5.0F, FBE_SPACE_WORLD));
    std::array<float, 12> down{};
    for (std::size_t corner = 0; corner < 4; ++corner) {
        down[(corner * 3) + 1] = -30.0F;
    }
    require(fbe_morph_add_target(fbe_mesh_geometry(quad), 0, down.data(), nullptr, 4) == 0);
    CHECK(middle(scene.drawn())[0] != 0);
    const std::array<float, 1> weights{ 1.0F };
    require(fbe_morph_set_weights(quad, weights.data(), 1));
    (void)scene.drawn();
    CHECK(middle(scene.drawn())[0] == 0);
    fbe_material_release(paint);
}

TEST_CASE("what casts is culled by its geometry's cull box, as what's drawn is", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -10.0F, 30.0F, paint);
    sun_from_right(scene.world);
    const std::uint64_t quad = wall(scene.world, -5.0F, 3.0F, paint);
    require(fbe_entity_set_position(quad, 5.0F, 0.0F, -5.0F, FBE_SPACE_WORLD));
    CHECK(middle(scene.drawn())[0] == 0);
    // A box far above where it is: it isn't looked for on the sun's way.
    const fbe_aabb_t above{
        .min = { .x = -1.0F, .y = 500.0F, .z = -1.0F },
        .max = { .x = 1.0F, .y = 501.0F, .z = 1.0F },
    };
    require(fbe_geometry_set_cull_box(fbe_mesh_geometry(quad), &above));
    CHECK(middle(scene.drawn())[0] != 0);
    require(fbe_geometry_clear_cull_box(fbe_mesh_geometry(quad)));
    CHECK(middle(scene.drawn())[0] == 0);
    fbe_material_release(paint);
}

TEST_CASE("what faces away from a light casts as well as what faces it", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -10.0F, 30.0F, paint);
    sun_from_right(scene.world);
    const std::uint64_t quad = wall(scene.world, -5.0F, 3.0F, paint);
    require(fbe_entity_set_position(quad, 5.0F, 0.0F, -5.0F, FBE_SPACE_WORLD));
    CHECK(middle(scene.drawn())[0] == 0);
    turn_about(quad, 0.0F, 1.0F, 0.0F, std::numbers::pi_v<float>);
    CHECK(middle(scene.drawn())[0] == 0);
    fbe_material_release(paint);
}

TEST_CASE("a mirror's reflection has shadows of its own, outside the camera's cascades",
          "[gpu][scene]") {
    const auto seen = [](bool caster) {
        const scene_t scene;
        const std::uint64_t paint = fbe_material_create();
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
        require(fbe_camera_set_field_of_view(scene.camera, 0.7F));
        const std::uint64_t back_wall = wall(scene.world, 25.0F, 30.0F, paint);
        turn_about(back_wall, 0.0F, 1.0F, 0.0F, std::numbers::pi_v<float>);
        const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
        turn_about(sun, 0.0F, 1.0F, 0.0F,
                   std::numbers::pi_v<float> + (std::numbers::pi_v<float> / 4.0F));
        require(fbe_light_set_shadows(sun, true));
        require(fbe_light_set_shadow_distance(sun, 40.0F));
        require(fbe_camera_set_range(scene.camera, 0.1F, 100.0F));
        if (caster) {
            // Whichever way's the sun's, one's on it.
            cube(scene.world, 5.0F, 0.0F, 20.0F, paint);
            cube(scene.world, -5.0F, 0.0F, 20.0F, paint);
        }
        const std::uint64_t mirror = fbe_entity_create(scene.world, 0, FBE_ENTITY_MIRROR);
        turn_about(mirror, 1.0F, 0.0F, 0.0F, std::numbers::pi_v<float> / 2.0F);
        require(fbe_entity_set_position(mirror, 0.0F, 0.0F, -2.5F, FBE_SPACE_WORLD));
        const rgba_t found = middle(scene.drawn());
        fbe_material_release(paint);
        return found;
    };
    CHECK(seen(false)[0] > 100);
    CHECK(seen(true)[0] == 0);
}

TEST_CASE("the LOD bias reaches what a cut-out's cut by in its shadow", "[gpu][scene]") {
    const bool samplers_can = GENERATE(true, false);
    if (!samplers_can) {
        fbe::gpu::inject(fbe::gpu::fault_t::NO_LOD_BIAS);
    }
    const scene_t scene;
    if (samplers_can && fbe::gpu::device()->max_lod_bias == 0.0F) {
        return;
    }
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -10.0F, 30.0F, paint);
    sun_from_right(scene.world);
    // Whole in its middle, so a shadow; a quarter whole by its last level, which the cutoff cuts. A
    // bias is on a level found from how UVs change, so they have to.
    const std::uint64_t texture = fbe_texture_create(8, 8, FBE_TEXTURE_RGBA8, true);
    std::vector<std::uint8_t> pixels;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const bool whole = x >= 2 && x < 6 && y >= 2 && y < 6;
            pixels.insert(pixels.end(),
                          { 255, 255, 255, static_cast<std::uint8_t>(whole ? 255 : 0) });
        }
    }
    require(fbe_texture_write(texture, 0, 0, 8, 8, pixels.data(), 8 * 4));
    const std::uint64_t caster = fbe_material_create();
    require(fbe_material_set_alpha_cutoff(caster, 0.5F));
    require(fbe_material_set_texture(caster, 0, texture));
    alpha_wall(scene.world, 5.0F, -5.0F, 3.0F, 1.0F, caster, 1.0F);
    CHECK(middle(scene.drawn())[0] == 0);
    require(fbe_gpu_set_lod_bias(16.0F));
    CHECK(middle(scene.drawn())[0] > 0);
    require(fbe_gpu_set_lod_bias(0.0F));
    fbe_texture_release(texture);
    fbe_material_release(caster);
    fbe_material_release(paint);
}

TEST_CASE("the LOD bias leaves a sky's texture at its first level", "[gpu][scene]") {
    const bool samplers_can = GENERATE(true, false);
    if (!samplers_can) {
        fbe::gpu::inject(fbe::gpu::fault_t::NO_LOD_BIAS);
    }
    const scene_t scene;
    if (samplers_can && fbe::gpu::device()->max_lod_bias == 0.0F) {
        return;
    }
    const std::uint64_t texture = fbe_texture_create(64, 64, FBE_TEXTURE_RGBA8, true);
    std::vector<std::uint8_t> pixels;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const std::uint8_t level = ((x + y) % 2) == 0 ? 255 : 0;
            pixels.insert(pixels.end(), { level, level, level, 255 });
        }
    }
    require(fbe_texture_write(texture, 0, 0, 64, 64, pixels.data(), 64 * 4));
    require(fbe_texture_set_filter(texture, FBE_TEXTURE_NEAREST));
    require(fbe_world_set_sky_texture(scene.world, texture, 1.0F));
    const auto spread = [&scene] {
        const fbe::golden::frame_t frame = scene.drawn();
        int lowest = 255;
        int highest = 0;
        for (std::uint32_t y = 0; y < frame_size; ++y) {
            const rgba_t found = pixel(frame, frame_size / 2, y);
            lowest = std::min<int>(lowest, found[0]);
            highest = std::max<int>(highest, found[0]);
        }
        return highest - lowest;
    };
    CHECK(spread() > 128);
    for (const float bias : { 8.0F, -8.0F }) {
        require(fbe_gpu_set_lod_bias(bias));
        CHECK(spread() > 128);
    }
    require(fbe_gpu_set_lod_bias(0.0F));
    fbe_texture_release(texture);
}

TEST_CASE("a cut-out casts as it's drawn", "[gpu][scene]") {
    const auto check = [](bool drawn, auto&& set_up) {
        const scene_t scene;
        const std::uint64_t paint = fbe_material_create();
        wall(scene.world, -10.0F, 30.0F, paint);
        sun_from_right(scene.world);
        const std::uint64_t caster_paint = fbe_material_create();
        require(fbe_material_set_color(caster_paint, 1.0F, 0.0F, 0.0F, 1.0F));
        require(fbe_material_set_alpha_cutoff(caster_paint, 0.5F));
        set_up(scene.world, caster_paint);
        const rgba_t on_wall = middle(scene.drawn());
        require(fbe_entity_set_position(scene.camera, 5.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
        const rgba_t head_on = middle(scene.drawn());
        // The caster's red, but for what its surface reflects, as the wall's gray.
        CHECK((head_on[1] < head_on[0]) == drawn);
        CHECK((on_wall[0] == 0) == drawn);
        fbe_material_release(paint);
        fbe_material_release(caster_paint);
    };
    const auto with_texture = [](std::uint64_t material, std::int32_t layer, std::uint8_t alpha) {
        const std::uint64_t texture = flat_texture(255, 255, 255, alpha);
        require(fbe_material_set_texture(material, layer, texture));
        fbe_texture_release(texture);
    };
    SECTION("OPAQUE leaves its texture's alpha out") {
        check(true, [&](std::uint64_t world, std::uint64_t material) {
            with_texture(material, 0, 0);
            require(fbe_material_set_blend(material, FBE_BLEND_OPAQUE));
            alpha_wall(world, 5.0F, -5.0F, 3.0F, 1.0F, material);
        });
    }
    SECTION("vertex colors' alpha") {
        check(false, [&](std::uint64_t world, std::uint64_t material) {
            require(fbe_material_set_vertex_colors(material, true));
            alpha_wall(world, 5.0F, -5.0F, 3.0F, 0.0F, material);
        });
    }
    SECTION("a vertex's alpha, with no vertex colors, isn't anything") {
        check(true, [&](std::uint64_t world, std::uint64_t material) {
            alpha_wall(world, 5.0F, -5.0F, 3.0F, 0.0F, material);
        });
    }
    SECTION("a second layer's alpha") {
        check(false, [&](std::uint64_t world, std::uint64_t material) {
            with_texture(material, 0, 255);
            with_texture(material, 1, 0);
            alpha_wall(world, 5.0F, -5.0F, 3.0F, 1.0F, material);
        });
    }
    SECTION("an ALPHA layer leaves alpha as it is") {
        check(true, [&](std::uint64_t world, std::uint64_t material) {
            with_texture(material, 0, 0);
            require(fbe_material_set_layer(material, 0, FBE_LAYER_ALPHA, 0));
            alpha_wall(world, 5.0F, -5.0F, 3.0F, 1.0F, material);
        });
    }
    SECTION("its first layer's") {
        check(false, [&](std::uint64_t world, std::uint64_t material) {
            with_texture(material, 0, 0);
            alpha_wall(world, 5.0F, -5.0F, 3.0F, 1.0F, material);
        });
    }
}

TEST_CASE("a surface far turned from its light isn't shadowed by itself", "[gpu][scene]") {
    const float degrees = GENERATE(5.0F, 10.0F, 20.0F, 45.0F, 70.0F);
    const std::int32_t size = GENERATE(256, 1024);
    const float distance = GENERATE(30.0F, 200.0F);
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    grid_floor(scene.world, -2.0F, 60, 8.0F, paint);
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    const float radians = degrees * std::numbers::pi_v<float> / 180.0F;
    turn_about(sun, 1.0F, 0.0F, 0.0F, -radians);
    require(fbe_light_set_shadows(sun, true));
    require(fbe_light_set_shadow_size(sun, size));
    require(fbe_light_set_shadow_distance(sun, distance));
    require(fbe_camera_set_range(scene.camera, 0.1F, 300.0F));
    turn_about(scene.camera, 1.0F, 0.0F, 0.0F, -0.25F);
    const fbe::golden::frame_t frame = scene.drawn();
    require(fbe_light_set_shadows(sun, false));
    const fbe::golden::frame_t unshadowed = scene.drawn();
    int wrong = 0;
    for (std::uint32_t y = frame.height / 2; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            wrong += std::abs(pixel(frame, x, y)[0] - pixel(unshadowed, x, y)[0]) >= 3 ? 1 : 0;
        }
    }
    CHECK(wrong == 0);
    fbe_material_release(paint);
}

TEST_CASE("a shadow starts at what casts it, and ends where it should", "[gpu][scene]") {
    // A thin wall on a floor, the sun 45 degrees over it along +X, seen from straight above.
    const auto seen = [](float depth_bias) {
        const scene_t scene;
        const std::uint64_t paint = fbe_material_create();
        grid_floor(scene.world, 0.0F, 40, 1.0F, paint);
        const std::uint64_t thin = wall(scene.world, -6.0F, 1.0F, paint);
        turn_about(thin, 0.0F, 1.0F, 0.0F, std::numbers::pi_v<float> * 0.5F);
        require(fbe_entity_set_position(thin, 1.0F, 1.0F, -6.0F, FBE_SPACE_WORLD));
        const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
        require(fbe_entity_point_at(sun, 1.0F, -1.0F, 0.0F, 0.0F));
        require(fbe_light_set_shadows(sun, true));
        require(fbe_light_set_shadow_distance(sun, 30.0F));
        require(fbe_light_set_shadow_size(sun, 2048));
        require(fbe_light_set_shadow_bias(sun, depth_bias, 1.5F));
        require(fbe_entity_set_position(scene.camera, 1.5F, 8.0F, -6.0F, FBE_SPACE_WORLD));
        turn_about(scene.camera, 1.0F, 0.0F, 0.0F, -std::numbers::pi_v<float> * 0.5F);
        const fbe::golden::frame_t frame = scene.drawn();
        fbe_material_release(paint);
        return frame;
    };
    // The floor at x, seen from 8 over x = 1.5.
    const auto at = [](float x) {
        return static_cast<std::uint32_t>(64.0F + ((x - 1.5F) * pixels_at_1 / 8.0F));
    };
    const fbe::golden::frame_t right = seen(0.02F);
    CHECK(pixel(right, at(1.15F), 64)[0] < 8);
    CHECK(pixel(right, at(2.8F), 64)[0] < 8);
    CHECK(pixel(right, at(3.3F), 64)[0] > 100);
    // Compared too far off the floor, it comes away from the block's foot.
    CHECK(pixel(seen(1.0F), at(1.15F), 64)[0] > 100);
}

TEST_CASE("a faded-out caster doesn't cast for the camera it's faded from", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -10.0F, 30.0F, paint);
    sun_from_right(scene.world);
    const std::uint64_t block = cube(scene.world, 5.0F, 0.0F, -5.0F, paint);
    require(fbe_entity_set_auto_fade(block, 1.0F, 8.0F));
    // Too far off for it, and another camera that isn't, in one World's drawing.
    require(fbe_entity_set_position(scene.camera, 0.0F, 0.0F, 10.0F, FBE_SPACE_WORLD));
    const std::uint64_t other_target =
        fbe_texture_create_target(frame_size, frame_size, FBE_TEXTURE_RGBA8, 1, true);
    const std::uint64_t other = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
    require(fbe_camera_set_target(other, other_target));
    CHECK(fbe_entity_fade(block, scene.camera) == 0.0F);
    CHECK(fbe_entity_fade(block, other) > 0.0F);
    const fbe::golden::frame_t far_off = scene.drawn();
    CHECK(near(middle(far_off), { srgb(sun_lit()), srgb(sun_lit()), srgb(sun_lit()) }));
    CHECK(middle(scene_t::read(other_target))[0] == 0);
    require(fbe_entity_free(other));
    fbe_texture_release(other_target);
    fbe_material_release(paint);
}

TEST_CASE("a point light's faces meet with no leak, at any size", "[gpu][scene]") {
    const std::int32_t size = GENERATE(32, 64, 128, 256, 1024);
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    grid_floor(scene.world, -3.0F, 40, 1.0F, paint);
    const std::uint64_t slab = grid_floor(scene.world, -1.0F, 19, 1.0F, paint);
    require(fbe_entity_set_position(slab, 10.0F, -1.0F, 0.0F, FBE_SPACE_WORLD));
    const std::uint64_t lamp = light(scene.world, FBE_LIGHT_POINT, 0.0F, 0.0F, 0.0F, 100.0F);
    require(fbe_light_set_intensity(lamp, 300.0F));
    require(fbe_light_set_shadows(lamp, true));
    require(fbe_light_set_shadow_size(lamp, size));
    require(fbe_camera_set_range(scene.camera, 0.1F, 50.0F));
    require(fbe_entity_set_position(scene.camera, 3.0F, -2.0F, 0.0F, FBE_SPACE_WORLD));
    turn_about(scene.camera, 1.0F, 0.0F, 0.0F, -std::numbers::pi_v<float> / 2.0F);
    CHECK(reds_above(scene.drawn(), 20) == 0);
    fbe_material_release(paint);
}

TEST_CASE("a point light's face filters on past its edge", "[gpu][scene]") {
    // A strip only the next face looks straight at, and a pixel of the floor just inside this one,
    // whose filter reaches it. With no offset, so it's the filter that does.
    const auto seen = [](bool strip) {
        const scene_t scene;
        const std::uint64_t paint = fbe_material_create();
        grid_floor(scene.world, -3.0F, 40, 1.0F, paint);
        if (strip) {
            const std::uint64_t over = grid_floor(scene.world, -1.0F, 1, 1.0F, paint);
            require(fbe_entity_set_scale(over, 0.2F, 1.0F, 40.0F, FBE_SPACE_WORLD));
            require(fbe_entity_set_position(over, 1.1F, -1.0F, 0.0F, FBE_SPACE_WORLD));
        }
        const std::uint64_t lamp = light(scene.world, FBE_LIGHT_POINT, 0.0F, 0.0F, 0.0F, 100.0F);
        require(fbe_light_set_intensity(lamp, 15.0F));
        require(fbe_light_set_shadows(lamp, true));
        require(fbe_light_set_shadow_size(lamp, 32));
        require(fbe_light_set_shadow_bias(lamp, 0.02F, 0.0F));
        require(fbe_camera_set_range(scene.camera, 0.1F, 50.0F));
        require(fbe_entity_set_position(scene.camera, 2.97F, -2.0F, 0.0F, FBE_SPACE_WORLD));
        turn_about(scene.camera, 1.0F, 0.0F, 0.0F, -std::numbers::pi_v<float> / 2.0F);
        const rgba_t found = middle(scene.drawn());
        fbe_material_release(paint);
        return found[0];
    };
    CHECK(seen(true) + 8 < seen(false));
}

TEST_CASE("with no normal offset, a surface's held off itself by its depth bias", "[gpu][scene]") {
    const auto wrong_with = [](float depth_bias) {
        const scene_t scene;
        const std::uint64_t paint = fbe_material_create();
        const std::uint64_t slope = wall(scene.world, -10.0F, 30.0F, paint);
        turn_about(slope, 0.0F, 1.0F, 0.0F, 0.35F);
        const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
        require(fbe_light_set_shadows(sun, true));
        require(fbe_light_set_shadow_bias(sun, depth_bias, 0.0F));
        const fbe::golden::frame_t frame = scene.drawn();
        require(fbe_light_set_shadows(sun, false));
        const fbe::golden::frame_t unshadowed = scene.drawn();
        int wrong = 0;
        for (std::uint32_t y = 0; y < frame.height; ++y) {
            for (std::uint32_t x = 0; x < frame.width; ++x) {
                wrong += std::abs(pixel(frame, x, y)[0] - pixel(unshadowed, x, y)[0]) > 3 ? 1 : 0;
            }
        }
        fbe_material_release(paint);
        return wrong;
    };
    CHECK(wrong_with(0.2F) == 0);
    CHECK(wrong_with(0.0F) > 100);
}

TEST_CASE("a spot light's wide cone is shadowed right to its side", "[gpu][scene]") {
    const float x = GENERATE(5.0F, 10.0F, 16.0F, 25.0F);
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -1.0F, 200.0F, paint);
    wall(scene.world, -0.5F, 30.0F, paint);
    const std::uint64_t spot = light(scene.world, FBE_LIGHT_SPOT, 0.0F, 0.0F, 0.0F, 100.0F);
    require(fbe_light_set_intensity(spot, 100000.0F));
    require(fbe_light_set_cone(spot, 1.4F, std::numbers::pi_v<float> / 2.0F));
    require(fbe_light_set_shadows(spot, true));
    require(fbe_camera_set_range(scene.camera, 0.05F, 50.0F));
    require(fbe_entity_set_position(scene.camera, x, 0.0F, -0.7F, FBE_SPACE_WORLD));
    INFO("at x " << x);
    CHECK(middle(scene.drawn())[0] == 0);
    fbe_material_release(paint);
}

TEST_CASE("a directional light's shadow doesn't crawl as the camera moves, anywhere",
          "[gpu][scene]") {
    const float origin = GENERATE(0.0F, 2000.0F, 20000.0F);
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    require(fbe_camera_set_projection(scene.camera, FBE_PROJECTION_ORTHOGRAPHIC));
    require(fbe_camera_set_orthographic_height(scene.camera, 8.0F));
    require(fbe_camera_set_range(scene.camera, 1.0F, 200.0F));
    wall(scene.world, origin - 60.0F, 30.0F, paint);
    const std::uint64_t sun = sun_from_right(scene.world);
    require(fbe_light_set_cascades(sun, 1));
    require(fbe_light_set_shadow_distance(sun, 150.0F));
    require(fbe_light_set_shadow_size(sun, 256));
    const std::uint64_t block = cube(scene.world, 5.13F, 0.37F, origin - 55.0F, paint);
    turn_about(block, 0.3F, 0.6F, 0.74162F, 0.5F);
    require(fbe_entity_set_position(scene.camera, 0.0F, 0.0F, origin, FBE_SPACE_WORLD));
    const fbe::golden::frame_t first = scene.drawn();
    std::size_t most = 0;
    for (int step = 1; step <= 30; ++step) {
        require(fbe_entity_set_position(scene.camera, 0.0F, 0.0F,
                                        origin - (static_cast<float>(step) * 0.37F),
                                        FBE_SPACE_WORLD));
        most = std::max(most, fbe::golden::compare(first, scene.drawn(), 2).pixels);
    }
    CHECK(most == 0);
    fbe_material_release(paint);
}

TEST_CASE("the shadow atlas stays at its largest, then goes", "[gpu][scene]") {
    const scene_t scene;
    const shadowed_wall_t over(scene);
    const std::uint64_t other = fbe_world_create();
    const std::uint64_t other_target =
        fbe_texture_create_target(frame_size, frame_size, FBE_TEXTURE_RGBA8, 1, true);
    const std::uint64_t other_camera = fbe_entity_create(other, 0, FBE_ENTITY_CAMERA);
    require(fbe_camera_set_target(other_camera, other_target));
    const std::uint64_t other_sun = sun_from_right(other);
    require(fbe_light_set_shadow_size(other_sun, 128));
    for (int frame = 0; frame < 4; ++frame) {
        require(fbe_render_world(scene.world, 1.0F));
        CHECK(fbe::render::shadow_atlas_side() == 2048);
        require(fbe_render_world(other, 1.0F));
        CHECK(fbe::render::shadow_atlas_side() == 2048);
        fbe_frame_flip(false);
    }
    // Nothing wants so much of it now, then nothing wants it.
    require(fbe_light_set_shadow_size(over.sun, 128));
    for (int frame = 0; frame < 130; ++frame) {
        require(fbe_render_world(scene.world, 1.0F));
        fbe_frame_flip(false);
    }
    CHECK(fbe::render::shadow_atlas_side() == 256);
    require(fbe_light_set_shadows(over.sun, false));
    for (int frame = 0; frame < 130; ++frame) {
        require(fbe_render_world(scene.world, 1.0F));
        fbe_frame_flip(false);
    }
    CHECK(fbe::render::shadow_atlas_side() == 0);
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_world_free(other);
    std::array<std::uint64_t, 16> some{};
    while (fbe_entity_freed(some.data(), some.size()) != 0) {}
    fbe_texture_release(other_target);
}

TEST_CASE("the shadow atlas keeps to its limit, and a smaller one for no memory", "[gpu][scene]") {
    const scene_t scene;
    const shadowed_wall_t over(scene);
    SECTION("its limit") {
        CHECK(fbe_render_shadow_atlas_limit() == 4096);
        CHECK_FALSE(fbe_render_set_shadow_atlas_limit(128));
        CHECK_FALSE(fbe_render_set_shadow_atlas_limit(16384));
        require(fbe_render_set_shadow_atlas_limit(1024));
        CHECK(over.shadowed());
        CHECK(fbe::render::shadow_atlas_side() == 1024);
        require(fbe_render_set_shadow_atlas_limit(4096));
    }
    SECTION("the GPU refusing it") {
        require(fbe_gpu_started() || fbe_gpu_start());
        fbe::render::refuse_shadow_atlas(1024);
        CHECK(over.shadowed());
        CHECK(fbe::render::shadow_atlas_side() == 512);
    }
    SECTION("the GPU refusing all of it: no shadows, and the camera draws") {
        require(fbe_gpu_started() || fbe_gpu_start());
        fbe::render::refuse_shadow_atlas(256);
        CHECK_FALSE(over.shadowed());
        CHECK(fbe::render::shadow_atlas_side() == 0);
    }
}

TEST_CASE("R32F is the engine's", "[gpu][scene]") {
    const scene_t scene;
    CHECK(fbe_texture_create(2, 2, fbe::texture::shadow_format, false) == 0);
    CHECK(fbe_texture_create_target(2, 2, fbe::texture::shadow_format, 1, false) == 0);
}

TEST_CASE("a light too small to have a tile has no shadow, and no bad matrix", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -4.0F, 20.0F, paint);
    SECTION("a range of next to nothing") {
        const float range = GENERATE(0.0001F, 0.01F, 0.02F);
        const std::uint64_t lamp = light(scene.world, FBE_LIGHT_POINT, 0.0F, 0.0F, 0.0F, range);
        require(fbe_light_set_shadows(lamp, true));
        CHECK(near(middle(scene.drawn()), { 0, 0, 0 }));
    }
    SECTION("a cone of nothing") {
        const std::uint64_t spot = light(scene.world, FBE_LIGHT_SPOT, 0.0F, 0.0F, 0.0F, 20.0F);
        require(fbe_light_set_cone(spot, 0.0F, 0.0F));
        require(fbe_light_set_shadows(spot, true));
        (void)scene.drawn();
    }
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(paint);
}

TEST_CASE("a camera with no pixels draws no shadows", "[gpu][scene]") {
    const scene_t scene;
    const shadowed_wall_t over(scene);
    require(fbe_camera_set_viewport(scene.camera, 1000.0F, 1000.0F, 64.0F, 64.0F));
    (void)scene.drawn();
    CHECK(fbe::render::shadow_atlas_side() == 0);
}

namespace {

/// An RGBA16F texture of one color, of light past what a byte holds.
std::uint64_t even_light(float r, float g, float b, std::int32_t width = 4,
                         std::int32_t height = 2) {
    const std::uint64_t texture = fbe_texture_create(width, height, FBE_TEXTURE_RGBA16F, false);
    require(texture != 0);
    std::vector<float> pixels;
    for (std::int32_t at = 0; at < width * height; ++at) {
        pixels.insert(pixels.end(), { r, g, b, 1.0F });
    }
    require(fbe_texture_write_floats(texture, 0, 0, width, height, pixels.data()));
    return texture;
}

/// A surface's tangents, all of them the same: +X, w as given.
void tangents_of(std::uint64_t mesh, float w) {
    const std::array<fbe_vertex_tangent_t, 4> tangents{
        fbe_vertex_tangent_t{ .x = 1.0F, .y = 0.0F, .z = 0.0F, .w = w },
        fbe_vertex_tangent_t{ .x = 1.0F, .y = 0.0F, .z = 0.0F, .w = w },
        fbe_vertex_tangent_t{ .x = 1.0F, .y = 0.0F, .z = 0.0F, .w = w },
        fbe_vertex_tangent_t{ .x = 1.0F, .y = 0.0F, .z = 0.0F, .w = w },
    };
    require(fbe_surface_set_tangents(fbe_mesh_geometry(mesh), 0, tangents.data(), 4));
}

} // namespace

TEST_CASE("a PBR material's settings are what they're set to, and hold its maps", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    CHECK(fbe_material_metallic(material) == 0.0F);
    CHECK(fbe_material_roughness(material) == 1.0F);
    CHECK(fbe_material_normal_scale(material) == 1.0F);
    CHECK(fbe_material_occlusion_strength(material) == 1.0F);
    fbe_color_t color{};
    CHECK(fbe_material_emissive(material, &color));
    CHECK(color.r == 0.0F);
    CHECK(fbe_material_set_metal_rough(material, 0.25F, 0.75F));
    CHECK(fbe_material_metallic(material) == 0.25F);
    CHECK(fbe_material_roughness(material) == 0.75F);
    CHECK(!fbe_material_set_metal_rough(material, -0.1F, 0.5F));
    CHECK(!fbe_material_set_metal_rough(material, 0.5F, 1.5F));
    CHECK(!fbe_material_set_metal_rough(material, std::nanf(""), 0.5F));
    CHECK(fbe_material_metallic(material) == 0.25F);
    CHECK(fbe_material_set_emissive(material, 2.0F, 3.0F, 4.0F));
    CHECK(fbe_material_emissive(material, &color));
    CHECK(color.b == 4.0F);
    CHECK(!fbe_material_set_emissive(material, -1.0F, 0.0F, 0.0F));
    CHECK(fbe_material_set_normal_scale(material, -2.0F));
    CHECK(fbe_material_normal_scale(material) == -2.0F);
    CHECK(!fbe_material_set_normal_scale(material, std::nanf("")));
    CHECK(fbe_material_set_occlusion_strength(material, 0.5F));
    CHECK(fbe_material_occlusion_strength(material) == 0.5F);
    CHECK(!fbe_material_set_occlusion_strength(material, 1.5F));

    const std::uint64_t texture = flat_texture(255, 255, 255, 255, FBE_TEXTURE_RGBA8_LINEAR);
    const std::int32_t textures = fbe_texture_count();
    CHECK(fbe_material_map(material, FBE_MAP_NORMAL) == 0);
    CHECK(fbe_material_set_map(material, FBE_MAP_NORMAL, texture, 1));
    CHECK(fbe_material_map(material, FBE_MAP_NORMAL) == texture);
    CHECK(fbe_material_map_uv_set(material, FBE_MAP_NORMAL) == 1);
    CHECK(!fbe_material_set_map(material, FBE_MAP_NORMAL, texture, 2));
    CHECK(!fbe_material_set_map(material, FBE_MATERIAL_MAPS, texture, 0));
    CHECK(!fbe_material_set_map(material, -1, texture, 0));
    CHECK(fbe_material_set_map_uv(material, FBE_MAP_NORMAL, 2.0F, 3.0F, 0.5F, 0.25F, 0.125F));
    std::array<float, 5> uv{};
    CHECK(fbe_material_map_uv(material, FBE_MAP_NORMAL, uv.data()));
    CHECK(uv == std::array{ 2.0F, 3.0F, 0.5F, 0.25F, 0.125F });
    CHECK(!fbe_material_set_map_uv(material, FBE_MAP_NORMAL, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F));
    // A clone holds its maps too, and each holds it apart: the last to go lets it go.
    const std::uint64_t clone = fbe_material_clone(material);
    CHECK(fbe_material_map(clone, FBE_MAP_NORMAL) == texture);
    CHECK(fbe_material_metallic(clone) == 0.25F);
    require(fbe_texture_release(texture));
    CHECK(fbe_texture_count() == textures);
    fbe_material_release(material);
    CHECK(fbe_texture_count() == textures);
    fbe_material_release(clone);
    CHECK(fbe_texture_count() == textures - 1);
}

TEST_CASE("a surface's tangents are one for each of its vertices, or none", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t mesh = wall(scene.world, -3.0F, 1.0F);
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    CHECK(fbe_surface_tangents(geometry, 0, nullptr, 0) == 0);
    tangents_of(mesh, -1.0F);
    std::array<fbe_vertex_tangent_t, 4> found{};
    CHECK(fbe_surface_tangents(geometry, 0, found.data(), 4) == 4);
    CHECK(found[3].w == -1.0F);
    CHECK(!fbe_surface_set_tangents(geometry, 0, found.data(), 3));
    CHECK(fbe_surface_set_tangents(geometry, 0, nullptr, 0));
    CHECK(fbe_surface_tangents(geometry, 0, nullptr, 0) == 0);
    // Vertices added let go of them.
    tangents_of(mesh, 1.0F);
    const fbe_vertex_t another{};
    CHECK(fbe_surface_add_vertices(geometry, 0, &another, 1) == 4);
    CHECK(fbe_surface_tangents(geometry, 0, nullptr, 0) == 0);
}

// Colored, at each metalness and roughness, against the same equations in the test.
TEST_CASE("metal and roughness shade as glTF's BRDF says", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_color(material, 0.8F, 0.5F, 0.2F, 1.0F));
    wall(scene.world, -3.0F, 10.0F, material);
    light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    for (const auto& [metallic, roughness] :
         { std::pair{ 0.0F, 1.0F }, std::pair{ 1.0F, 0.5F }, std::pair{ 0.5F, 0.7F } }) {
        INFO("metallic " << metallic << ", roughness " << roughness);
        require(fbe_material_set_metal_rough(material, metallic, roughness));
        const auto channel = [metallic, roughness](float base) {
            return srgb(brdf(base, metallic, roughness, 1.0F, 1.0F, 1.0F, 1.0F));
        };
        CHECK(near(middle(scene.drawn()), { channel(0.8F), channel(0.5F), channel(0.2F) }, 3));
    }
    fbe_material_release(material);
}

// A map's green multiplies roughness and its blue metalness, its red being nothing; occlusion's red
// darkens ambient and environment light and not a light's; and emission's on top of it all.
TEST_CASE("a PBR material's maps do as glTF's do", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    wall(scene.world, -3.0F, 10.0F, material);
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    require(fbe_material_set_metal_rough(material, 1.0F, 1.0F));
    const std::uint64_t mixed = flat_texture(9, 64, 192, 255, FBE_TEXTURE_RGBA8_LINEAR);
    require(fbe_material_set_map(material, FBE_MAP_METAL_ROUGH, mixed, 0));
    const float half = 128.0F / 255.0F;
    const std::uint8_t both =
        srgb(brdf(1.0F, 192.0F / 255.0F, 64.0F / 255.0F, 1.0F, 1.0F, 1.0F, 1.0F));
    CHECK(near(middle(scene.drawn()), { both, both, both }, 3));
    require(fbe_material_set_map(material, FBE_MAP_METAL_ROUGH, 0, 0));

    // Occlusion: all of a light, and half of ambient at a texel of 0.5, or none at no strength.
    require(fbe_material_set_metal_rough(material, 0.0F, 1.0F));
    require(fbe_world_set_ambient(scene.world, 0.5F, 0.5F, 0.5F));
    const std::uint64_t occluded = flat_texture(128, 9, 9, 255, FBE_TEXTURE_RGBA8_LINEAR);
    require(fbe_material_set_map(material, FBE_MAP_OCCLUSION, occluded, 0));
    const std::uint8_t darkened = srgb(rough_white(1.0F) + (0.5F * half));
    CHECK(near(middle(scene.drawn()), { darkened, darkened, darkened }, 3));
    require(fbe_material_set_occlusion_strength(material, 0.0F));
    const std::uint8_t whole = srgb(rough_white(1.0F) + 0.5F);
    CHECK(near(middle(scene.drawn()), { whole, whole, whole }, 3));
    require(fbe_world_set_ambient(scene.world, 0.0F, 0.0F, 0.0F));
    fbe_entity_free(sun);

    // Emission, sRGB in its map, multiplying what it emits, with nothing lighting it, past white
    // where it's bright.
    require(fbe_material_set_emissive(material, 0.5F, 2.0F, 0.0F));
    CHECK(near(middle(scene.drawn()), { srgb(0.5F), 255, 0 }));
    const std::uint64_t glow = flat_texture(128, 128, 128, 255, FBE_TEXTURE_RGBA8);
    require(fbe_material_set_map(material, FBE_MAP_EMISSIVE, glow, 0));
    const float decoded = 0.2158605F;
    CHECK(near(middle(scene.drawn()), { srgb(0.5F * decoded), srgb(2.0F * decoded), 0 }, 3));
    for (const std::uint64_t texture : { mixed, occluded, glow }) {
        fbe_texture_release(texture);
    }
    fbe_material_release(material);
}

// A map's own UV set and turn: red on its left half, read by the second set scaled to its right.
TEST_CASE("a map reads the UV set it says, through what turns them", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    const std::uint64_t mesh = wall(scene.world, -3.0F, 10.0F, material);
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    std::array<fbe_vertex_t, 4> vertices{};
    require(fbe_surface_vertices(geometry, 0, 0, vertices.data(), 4));
    for (fbe_vertex_t& vertex : vertices) {
        vertex.uv0 = { .x = 0.25F, .y = 0.25F };
        vertex.uv1 = { .x = 0.75F, .y = 0.25F };
    }
    require(fbe_surface_set_vertices(geometry, 0, 0, vertices.data(), 4));
    require(fbe_material_set_emissive(material, 1.0F, 1.0F, 1.0F));
    const rgba_t red{ 255, 0, 0, 255 };
    const rgba_t green{ 0, 255, 0, 255 };
    const std::uint64_t texture = four_pixels(red, green, red, green);
    require(fbe_texture_set_filter(texture, FBE_TEXTURE_NEAREST));
    require(fbe_material_set_map(material, FBE_MAP_EMISSIVE, texture, 0));
    CHECK(near(middle(scene.drawn()), { 255, 0, 0 }));
    require(fbe_material_set_map(material, FBE_MAP_EMISSIVE, texture, 1));
    CHECK(near(middle(scene.drawn()), { 0, 255, 0 }));
    // Moved back by half: the left again.
    require(fbe_material_set_map_uv(material, FBE_MAP_EMISSIVE, 1.0F, 1.0F, 0.0F, 0.5F, 0.0F));
    CHECK(near(middle(scene.drawn()), { 255, 0, 0 }));
    fbe_texture_release(texture);
    fbe_material_release(material);
}

// A flat map's nothing; a tilted one turns the normal up its tangent frame: red along a model's
// tangent, green along its bitangent, the way its w says, against what the UVs go down, and what a
// pixel's UVs make of them when it has no tangents the same as a model's.
TEST_CASE("a normal map turns a surface in its tangent frame", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    const std::uint64_t mesh = wall(scene.world, -3.0F, 10.0F, material);
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    const float root = std::numbers::sqrt2_v<float> / 2.0F;
    const auto shine_from = [&](float x, float y) {
        // Turned so it shines from (x, y, root), from +X with x, +Y with y.
        const float half = std::atan2(std::hypot(x, y), root) / 2.0F;
        const float sine = std::sin(half);
        const float apart = std::hypot(x, y);
        require(fbe_entity_set_rotation(sun, -y / apart * sine, x / apart * sine, 0.0F,
                                        std::cos(half), FBE_SPACE_WORLD));
    };
    const std::uint64_t flat = flat_texture(128, 128, 255, 255, FBE_TEXTURE_RGBA8_LINEAR);
    const std::uint64_t right = flat_texture(255, 128, 255, 255, FBE_TEXTURE_RGBA8_LINEAR);
    const std::uint64_t up = flat_texture(128, 255, 255, 255, FBE_TEXTURE_RGBA8_LINEAR);
    // Against the same pixel of no map at all, under a light it's turned toward.
    shine_from(root, 0.0F);
    const std::uint8_t plain = middle(scene.drawn())[0];
    require(fbe_material_set_map(material, FBE_MAP_NORMAL, flat, 0));
    CHECK(near(middle(scene.drawn()), { plain, plain, plain }, 1));
    tangents_of(mesh, 1.0F);
    CHECK(near(middle(scene.drawn()), { plain, plain, plain }, 1));

    // Turned to +X, toward the light, by its tangent, or with none, by its UVs' u.
    const auto toward = [&](std::uint64_t map) {
        require(fbe_material_set_map(material, FBE_MAP_NORMAL, map, 0));
        return middle(scene.drawn())[0];
    };
    const std::uint8_t model_right = toward(right);
    CHECK(model_right > plain + 10);
    require(fbe_surface_set_tangents(fbe_mesh_geometry(mesh), 0, nullptr, 0));
    CHECK(std::abs(toward(right) - model_right) <= 1);

    // Up, +Y, against the UVs' v, by w of +1 toward a light above, and -1 away from it.
    shine_from(0.0F, root);
    const std::uint8_t from_above = toward(up);
    CHECK(from_above > plain + 10);
    tangents_of(mesh, 1.0F);
    CHECK(std::abs(toward(up) - from_above) <= 1);
    tangents_of(mesh, -1.0F);
    CHECK(toward(up) < plain - 10);

    // Its scale takes it back to flat.
    require(fbe_material_set_normal_scale(material, 0.0F));
    CHECK(near(middle(scene.drawn()), { plain, plain, plain }, 1));
    require(fbe_material_set_normal_scale(material, 1.0F));

    // A mirror across X turns its tangent with it, and from the front, its bitangent stays up.
    tangents_of(mesh, 1.0F);
    require(fbe_entity_set_scale(mesh, -1.0F, 1.0F, 1.0F, FBE_SPACE_WORLD));
    require(fbe_material_set_two_sided(material, true));
    const std::uint8_t mirrored_model = toward(up);
    require(fbe_surface_set_tangents(fbe_mesh_geometry(mesh), 0, nullptr, 0));
    CHECK(std::abs(toward(up) - mirrored_model) <= 1);
    for (const std::uint64_t texture : { flat, right, up }) {
        fbe_texture_release(texture);
    }
    fbe_material_release(material);
}

// A bone's turn turns a surface's tangents with its normals.
TEST_CASE("a skin turns a surface's tangents", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    const skinned_wall_t made = skinned_wall(scene.world, material, -3.0F, 10.0F);
    all_on(made.geometry, 1, 1.0F);
    tangents_of(made.mesh, 1.0F);
    const std::uint64_t right = flat_texture(255, 128, 255, 255, FBE_TEXTURE_RGBA8_LINEAR);
    require(fbe_material_set_map(material, FBE_MAP_NORMAL, right, 0));
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    // A half turn about Z, in its plane: its tangent's -X, so a light from +X is away from it, and
    // from -X toward it.
    require(fbe_entity_set_rotation(made.moving, 0.0F, 0.0F, 1.0F, 0.0F, FBE_SPACE_WORLD));
    const float half = 0.3927F;
    require(
        fbe_entity_set_rotation(sun, 0.0F, std::sin(half), 0.0F, std::cos(half), FBE_SPACE_WORLD));
    const std::uint8_t from_right = middle(scene.drawn())[0];
    require(
        fbe_entity_set_rotation(sun, 0.0F, -std::sin(half), 0.0F, std::cos(half), FBE_SPACE_WORLD));
    const std::uint8_t from_left = middle(scene.drawn())[0];
    CHECK(from_left > from_right + 10);
    fbe_texture_release(right);
    fbe_skin_release(made.skin);
    fbe_material_release(material);
}

// An even environment on a surface in it comes back as its own color, metal or not; dimmed by its
// intensity; none once it's freed.
TEST_CASE("a World's environment lights its PBR materials", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    wall(scene.world, -3.0F, 10.0F, material);
    const std::uint64_t source = even_light(0.5F, 0.25F, 1.0F);
    const std::uint64_t environment = fbe_environment_bake(source, 16, 16);
    REQUIRE(environment != 0);
    fbe_texture_release(source);
    CHECK(fbe_world_environment(scene.world) == 0);
    CHECK(near(middle(scene.drawn()), { 0, 0, 0 }));
    require(fbe_world_set_environment(scene.world, environment, 1.0F));
    CHECK(fbe_world_environment(scene.world) == environment);
    CHECK(fbe_world_environment_intensity(scene.world) == 1.0F);
    for (const float metallic : { 0.0F, 1.0F }) {
        INFO("metallic " << metallic);
        require(fbe_material_set_metal_rough(material, metallic, 0.5F));
        CHECK(near(middle(scene.drawn()), { srgb(0.5F), srgb(0.25F), srgb(1.0F) }, 3));
    }
    require(fbe_world_set_environment(scene.world, environment, 0.5F));
    CHECK(near(middle(scene.drawn()), { srgb(0.25F), srgb(0.125F), srgb(0.5F) }, 3));
    CHECK(!fbe_world_set_environment(scene.world, environment, -1.0F));
    CHECK(!fbe_world_set_environment(scene.world, scene.world, 1.0F));
    // UNLIT and VERTEX aren't lit by it.
    require(fbe_material_set_shading(material, FBE_SHADING_VERTEX));
    CHECK(near(middle(scene.drawn()), { 0, 0, 0 }));
    require(fbe_material_set_shading(material, FBE_SHADING_PBR));
    require(fbe_environment_free(environment));
    CHECK(near(middle(scene.drawn()), { 0, 0, 0 }));
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(material);
}

// What's lit comes from the world's way, whichever way the camera looks: a sky above and dark
// below, on a surface facing up and on one facing down, seen from a camera turned to each.
TEST_CASE("an environment's directions are the world's, however the camera's turned",
          "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_metal_rough(material, 0.0F, 1.0F));
    const std::uint64_t mesh = wall(scene.world, 0.0F, 10.0F, material);
    const std::uint64_t source = fbe_texture_create(4, 4, FBE_TEXTURE_RGBA16F, false);
    REQUIRE(source != 0);
    std::vector<float> pixels;
    for (std::int32_t y = 0; y < 4; ++y) {
        for (std::int32_t x = 0; x < 4; ++x) {
            const float lit = y < 2 ? 1.0F : 0.0F;
            pixels.insert(pixels.end(), { lit, lit, lit, 1.0F });
        }
    }
    require(fbe_texture_write_floats(source, 0, 0, 4, 4, pixels.data()));
    const std::uint64_t environment = fbe_environment_bake(source, 16, 64);
    REQUIRE(environment != 0);
    fbe_texture_release(source);
    require(fbe_world_set_environment(scene.world, environment, 1.0F));
    const float root = std::numbers::sqrt2_v<float> / 2.0F;
    // The camera 3 from the wall, looking at its front, whichever way it's turned.
    const auto seen = [&](float x, float y, float z, float w, float along_x, float along_y,
                          float along_z) {
        require(fbe_entity_set_rotation(mesh, x, y, z, w, FBE_SPACE_WORLD));
        require(fbe_entity_set_rotation(scene.camera, x, y, z, w, FBE_SPACE_WORLD));
        require(fbe_entity_set_position(scene.camera, along_x * 3.0F, along_y * 3.0F,
                                        along_z * 3.0F, FBE_SPACE_WORLD));
        return middle(scene.drawn())[0];
    };
    // Its +Z up: seen from above, the whole sky. Its +Z down: seen from below, none of it.
    const std::uint8_t up = seen(-root, 0.0F, 0.0F, root, 0.0F, 1.0F, 0.0F);
    const std::uint8_t down = seen(root, 0.0F, 0.0F, root, 0.0F, -1.0F, 0.0F);
    CHECK(up > srgb(0.9F));
    CHECK(down < srgb(0.1F));
    // Its +Z along the horizon, whichever way: half the sky.
    const std::uint8_t facing = seen(0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F);
    const std::uint8_t turned_round = seen(0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, -1.0F);
    const std::uint8_t half_sky = srgb(0.5F);
    CHECK(near({ facing, facing, facing, 255 }, { half_sky, half_sky, half_sky }, 6));
    CHECK(near({ turned_round, turned_round, turned_round, 255 }, { half_sky, half_sky, half_sky },
               6));
    fbe_environment_free(environment);
    fbe_material_release(material);
}

// One image for both: what a mirror shows, whichever way it's turned, is what's behind whoever
// looks at it, as the sky has it there. A band of a color each way round, so a quarter turn between
// them shows.
TEST_CASE("a World's sky and its environment are the same image, the same way round",
          "[gpu][scene]") {
    const scene_t scene;
    constexpr std::int32_t across = 64;
    constexpr std::int32_t down = 32;
    const std::uint64_t source = fbe_texture_create(across, down, FBE_TEXTURE_RGBA16F, false);
    REQUIRE(source != 0);
    const std::array<std::array<float, 3>, 4> bands{
        {
            { 0.8F, 0.1F, 0.1F },
            { 0.1F, 0.8F, 0.1F },
            { 0.1F, 0.1F, 0.8F },
            { 0.8F, 0.8F, 0.1F },
        },
    };
    std::vector<float> pixels;
    for (std::int32_t y = 0; y < down; ++y) {
        for (std::int32_t x = 0; x < across; ++x) {
            const auto band = bands.at(static_cast<std::size_t>(((x + 8) / 16) % 4));
            pixels.insert(pixels.end(), { band[0], band[1], band[2], 1.0F });
        }
    }
    require(fbe_texture_write_floats(source, 0, 0, across, down, pixels.data()));
    const std::uint64_t environment = fbe_environment_bake(source, 64, 64);
    REQUIRE(environment != 0);
    require(fbe_world_set_sky_texture(scene.world, source, 1.0F));
    require(fbe_world_set_environment(scene.world, environment, 1.0F));
    fbe_texture_release(source);
    const std::uint64_t mirror = fbe_material_create();
    require(fbe_material_set_metal_rough(mirror, 1.0F, 0.0F));
    const std::uint64_t mesh = wall(scene.world, 0.0F, 10.0F, mirror);
    const float root = std::numbers::sqrt2_v<float> / 2.0F;
    // A turn about Y, and which way it looks then.
    struct turn_t {
        float y;
        float w;
        float along_x;
        float along_z;
    };
    const std::array<turn_t, 4> turns{
        {
            { .y = 0.0F, .w = 1.0F, .along_x = 0.0F, .along_z = -1.0F },
            { .y = root, .w = root, .along_x = -1.0F, .along_z = 0.0F },
            { .y = 1.0F, .w = 0.0F, .along_x = 0.0F, .along_z = 1.0F },
            { .y = -root, .w = root, .along_x = 1.0F, .along_z = 0.0F },
        },
    };
    std::array<rgba_t, 4> skies{};
    require(fbe_entity_set_hidden(mesh, true));
    for (std::size_t at = 0; at < turns.size(); ++at) {
        require(fbe_entity_set_rotation(scene.camera, 0.0F, turns.at(at).y, 0.0F, turns.at(at).w,
                                        FBE_SPACE_WORLD));
        skies.at(at) = middle(scene.drawn());
    }
    require(fbe_entity_set_hidden(mesh, false));
    for (std::size_t at = 0; at < turns.size(); ++at) {
        const turn_t& turn = turns.at(at);
        require(fbe_entity_set_rotation(scene.camera, 0.0F, turn.y, 0.0F, turn.w, FBE_SPACE_WORLD));
        require(fbe_entity_set_rotation(mesh, 0.0F, turn.y, 0.0F, turn.w, FBE_SPACE_WORLD));
        require(fbe_entity_set_position(mesh, turn.along_x * 3.0F, 0.0F, turn.along_z * 3.0F,
                                        FBE_SPACE_WORLD));
        const auto seen = middle(scene.drawn());
        const auto behind = skies.at((at + 2) % 4);
        INFO("turn " << at);
        CHECK(near(seen, { behind[0], behind[1], behind[2] }, 12));
        // And the four are different, or this says nothing.
        CHECK(!near(
            skies.at(at),
            { skies.at((at + 1) % 4)[0], skies.at((at + 1) % 4)[1], skies.at((at + 1) % 4)[2] },
            30));
    }
    fbe_environment_free(environment);
    fbe_material_release(mirror);
}

// Spheres, the scene's own, smooth to rough across and dielectric to metal up, under a sky and a
// sun, as the shaders' own test has them on analytic ones.
TEST_CASE("a scene of PBR spheres matches its golden image", "[gpu][scene][golden]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.02F, 0.02F, 0.03F, 1.0F));
    constexpr std::int32_t across = 512;
    constexpr std::int32_t down = 256;
    const std::uint64_t sky = fbe_texture_create(across, down, FBE_TEXTURE_RGBA16F, true);
    REQUIRE(sky != 0);
    std::vector<float> pixels;
    pixels.reserve(std::size_t{ across } * down * 4);
    for (std::int32_t y = 0; y < down; ++y) {
        for (std::int32_t x = 0; x < across; ++x) {
            const float height = 1.0F - (static_cast<float>(y) / (down / 2.0F));
            const bool sun = std::abs(x - 160) < 8 && std::abs(y - 70) < 8;
            const float blue = 0.3F + (0.4F * std::max(height, 0.0F));
            std::array<float, 3> color{ 0.1F, 0.08F, 0.06F };
            if (sun) {
                color = { 20.0F, 19.0F, 16.0F };
            } else if (height > 0.0F) {
                color = { 0.7F - (blue / 2.0F), 0.8F - (blue / 3.0F), 1.0F };
            }
            pixels.insert(pixels.end(), { color[0], color[1], color[2], 1.0F });
        }
    }
    require(fbe_texture_write_floats(sky, 0, 0, across, down, pixels.data()));
    const std::uint64_t environment = fbe_environment_bake(sky, 64, 128);
    REQUIRE(environment != 0);
    fbe_texture_release(sky);
    require(fbe_world_set_environment(scene.world, environment, 1.0F));
    const std::uint64_t ball = fbe_geometry_sphere(24);
    std::vector<std::uint64_t> materials;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const std::uint64_t material = fbe_material_create();
            require(fbe_material_set_color(material, 1.0F, 0.6F, 0.2F, 1.0F));
            require(fbe_material_set_metal_rough(material, static_cast<float>(row) / 2.0F,
                                                 static_cast<float>(column) / 2.0F));
            const std::uint64_t mesh = fbe_entity_create(scene.world, 0, FBE_ENTITY_MESH);
            require(fbe_mesh_set_geometry(mesh, ball));
            require(fbe_mesh_set_material(mesh, material));
            require(fbe_entity_set_position(mesh, (static_cast<float>(column) - 1.0F) * 2.2F,
                                            (static_cast<float>(row) - 1.0F) * 2.2F, -8.0F,
                                            FBE_SPACE_WORLD));
            materials.push_back(material);
        }
    }
    fbe_geometry_release(ball);
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    require(fbe_entity_set_rotation(sun, -0.2F, 0.3F, 0.0F, 0.93F, FBE_SPACE_WORLD));
    fbe::golden::check("scene_pbr", scene.drawn());
    CHECK(fbe_gpu_validation_errors() == 0);
    for (const std::uint64_t material : materials) {
        fbe_material_release(material);
    }
    fbe_environment_free(environment);
}

namespace {

/// A wall as bright as it says, in nothing but what it emits, in front of an HDR camera.
std::uint64_t glowing_wall(const scene_t& scene, float r, float g, float b) {
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_emissive(material, r, g, b));
    wall(scene.world, -3.0F, 10.0F, material);
    require(fbe_camera_set_hdr(scene.camera, true));
    return material;
}

/// Narkowicz's fit, with its exposure of 0.6, as tonemap.slang has it.
float aces(float light) {
    const float x = light * 0.6F;
    return std::clamp((x * ((x * 2.51F) + 0.03F)) / ((x * ((x * 2.43F) + 0.59F)) + 0.14F), 0.0F,
                      1.0F);
}

/// Khronos's PBR Neutral of a gray, as its reference has it.
float neutral(float light) {
    const float offset = light < 0.08F ? light - (6.25F * light * light) : 0.04F;
    const float color = light - offset;
    const float start = 0.8F - 0.04F;
    if (color < start) {
        return color;
    }
    const float left = 1.0F - start;
    const float peak = 1.0F - (left * left / (color + left - start));
    return peak;
}

} // namespace

TEST_CASE("a camera's HDR settings are what they're set to", "[gpu][scene]") {
    const scene_t scene;
    CHECK(!fbe_camera_hdr(scene.camera));
    CHECK(fbe_camera_exposure(scene.camera) == 1.0F);
    CHECK(fbe_camera_tonemap(scene.camera) == FBE_TONEMAP_NONE);
    CHECK(fbe_camera_gamma(scene.camera) == 1.0F);
    CHECK(fbe_camera_set_hdr(scene.camera, true));
    CHECK(fbe_camera_hdr(scene.camera));
    CHECK(fbe_camera_set_exposure(scene.camera, 0.5F));
    CHECK(fbe_camera_exposure(scene.camera) == 0.5F);
    CHECK(!fbe_camera_set_exposure(scene.camera, -1.0F));
    CHECK(!fbe_camera_set_exposure(scene.camera, std::nanf("")));
    CHECK(fbe_camera_exposure(scene.camera) == 0.5F);
    CHECK(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_ACES));
    CHECK(fbe_camera_tonemap(scene.camera) == FBE_TONEMAP_ACES);
    CHECK(!fbe_camera_set_tonemap(scene.camera, -1));
    CHECK(!fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_NEUTRAL + 1));
    CHECK(fbe_camera_set_gamma(scene.camera, 2.2F));
    CHECK(fbe_camera_gamma(scene.camera) == 2.2F);
    CHECK(!fbe_camera_set_gamma(scene.camera, 0.0F));
    CHECK(!fbe_camera_set_gamma(scene.camera, std::nanf("")));
    CHECK(!fbe_camera_set_hdr(scene.world, true));
}

// Light past white stays what it was until it's exposed: drawn straight into bytes, it's white
// however it's dimmed after.
TEST_CASE("an HDR camera keeps light past white for its exposure", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = glowing_wall(scene, 4.0F, 2.0F, 8.0F);
    CHECK(near(middle(scene.drawn()), { 255, 255, 255 }));
    require(fbe_camera_set_exposure(scene.camera, 0.0625F));
    CHECK(near(middle(scene.drawn()), { srgb(0.25F), srgb(0.125F), srgb(0.5F) }));
    require(fbe_camera_set_exposure(scene.camera, 0.0F));
    CHECK(near(middle(scene.drawn()), { 0, 0, 0 }));
    // Not HDR, there's no exposure to speak of.
    require(fbe_camera_set_hdr(scene.camera, false));
    CHECK(near(middle(scene.drawn()), { 255, 255, 255 }));
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(material);
}

// Each by its formula, in the test, at light under white and past it.
TEST_CASE("a tonemap's what its formula says", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = glowing_wall(scene, 0.0F, 0.0F, 0.0F);
    const auto seen = [&](float light) {
        require(fbe_material_set_emissive(material, light, light, light));
        return middle(scene.drawn())[0];
    };
    for (const float light : { 0.05F, 0.5F, 1.0F, 4.0F, 100.0F }) {
        INFO("light " << light);
        require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_NONE));
        CHECK(std::abs(seen(light) - srgb(light)) <= 1);
        require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_REINHARD));
        CHECK(std::abs(seen(light) - srgb(light / (light + 1.0F))) <= 1);
        require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_ACES));
        CHECK(std::abs(seen(light) - srgb(aces(light))) <= 1);
        require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_NEUTRAL));
        CHECK(std::abs(seen(light) - srgb(neutral(light))) <= 1);
    }
    // Exposure goes before it: 2 times 0.5 is 1 through Reinhard.
    require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_REINHARD));
    require(fbe_camera_set_exposure(scene.camera, 2.0F));
    CHECK(std::abs(seen(0.5F) - srgb(0.5F)) <= 1);
    fbe_material_release(material);
}

// Neutral's whole point: a color under its compression comes out as it went in, but for its offset,
// and a saturated one past it goes toward white, unlike clamping.
TEST_CASE("the neutral tonemap keeps a color's hue and goes to white past it", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = glowing_wall(scene, 0.6F, 0.3F, 0.1F);
    require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_NEUTRAL));
    CHECK(near(middle(scene.drawn()), { srgb(0.56F), srgb(0.26F), srgb(0.06F) }));
    require(fbe_material_set_emissive(material, 50.0F, 0.0F, 0.0F));
    const rgba_t bright = middle(scene.drawn());
    CHECK(bright[0] > 250);
    const std::uint8_t toward_white = srgb(0.879F);
    CHECK(std::abs(bright[1] - toward_white) <= 1);
    CHECK(bright[1] == bright[2]);
    fbe_material_release(material);
}

// Last of all, and a brightness setting's: raised to one over it, in linear, before sRGB's
// encoding.
TEST_CASE("a camera's gamma is the last it does", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = glowing_wall(scene, 0.25F, 0.25F, 0.25F);
    require(fbe_camera_set_gamma(scene.camera, 2.0F));
    const std::uint8_t half = srgb(0.5F);
    CHECK(near(middle(scene.drawn()), { half, half, half }));
    require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_REINHARD));
    require(fbe_material_set_emissive(material, 1.0F, 1.0F, 1.0F));
    const std::uint8_t reinhard = srgb(std::sqrt(0.5F));
    CHECK(near(middle(scene.drawn()), { reinhard, reinhard, reinhard }));
    fbe_material_release(material);
}

// A target of half floats keeps what's past white; one stored as written isn't encoded.
TEST_CASE("an HDR camera's target takes it in its own format", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = glowing_wall(scene, 4.0F, 0.5F, 0.0F);
    const std::uint64_t floats = fbe_texture_create_target(8, 8, FBE_TEXTURE_RGBA16F, 1, true);
    require(floats != 0);
    require(fbe_camera_set_target(scene.camera, floats));
    require(fbe_render_world(scene.world, 1.0F));
    std::array<std::uint16_t, std::size_t{ 8 } * 8 * 4> halves{};
    std::array<std::uint8_t, sizeof(halves)> read_bytes{};
    require(fbe_texture_read(floats, 0, read_bytes.data(), sizeof(halves)));
    std::memcpy(halves.data(), read_bytes.data(), sizeof(halves));
    // A GPU may round to either half beside what it worked out: Apple's has 0x37FF for 0.5.
    const auto within_a_unit = [](std::uint16_t half, std::uint16_t exact) {
        return std::abs(static_cast<int>(half) - static_cast<int>(exact)) <= 1;
    };
    CHECK(within_a_unit(halves[((std::size_t{ 4 } * 8) + 4) * 4], 0x4400));
    CHECK(within_a_unit(halves[(((std::size_t{ 4 } * 8) + 4) * 4) + 1], 0x3800));
    const std::uint64_t linear = fbe_texture_create_target(8, 8, FBE_TEXTURE_RGBA8_LINEAR, 1, true);
    require(linear != 0);
    require(fbe_camera_set_target(scene.camera, linear));
    require(fbe_render_world(scene.world, 1.0F));
    const fbe::golden::frame_t frame = scene_t::read(linear);
    CHECK(near(pixel(frame, 4, 4), { 255, 128, 0 }, 1));
    require(fbe_camera_set_target(scene.camera, scene.target));
    fbe_texture_release(floats);
    fbe_texture_release(linear);
    fbe_material_release(material);
}

// No light's so bright a tonemap makes nothing of it, or NaN of it: an infinity's as bright as a
// half float holds, and NaN is no light.
TEST_CASE("an HDR camera's infinity is white, and its NaN black", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = glowing_wall(scene, 1.0F, 1.0F, 1.0F);
    const auto of_halves = [](std::uint16_t half) {
        const std::uint64_t texture = fbe_texture_create(2, 2, FBE_TEXTURE_RGBA16F, false);
        require(texture != 0);
        std::array<std::uint16_t, 16> halves{};
        for (std::size_t at = 0; at < 4; ++at) {
            halves.at(at * 4) = half;
            halves.at((at * 4) + 1) = half;
            halves.at((at * 4) + 2) = half;
            halves.at((at * 4) + 3) = 0x3C00;
        }
        std::array<std::uint8_t, 32> bytes{};
        std::memcpy(bytes.data(), halves.data(), bytes.size());
        require(fbe_texture_write(texture, 0, 0, 2, 2, bytes.data(), 16));
        // Filtering's weights of 0 would make NaN of an infinity.
        require(fbe_texture_set_filter(texture, FBE_TEXTURE_NEAREST));
        return texture;
    };
    const std::uint64_t infinity = of_halves(0x7C00);
    require(fbe_material_set_map(material, FBE_MAP_EMISSIVE, infinity, 0));
    for (const std::int32_t tonemap :
         { FBE_TONEMAP_REINHARD, FBE_TONEMAP_ACES, FBE_TONEMAP_NEUTRAL }) {
        INFO("tonemap " << tonemap);
        require(fbe_camera_set_tonemap(scene.camera, tonemap));
        CHECK(near(middle(scene.drawn()), { 255, 255, 255 }));
    }
    fbe_texture_release(infinity);
    const std::uint64_t not_a_number = of_halves(0x7E00);
    require(fbe_material_set_map(material, FBE_MAP_EMISSIVE, not_a_number, 0));
    CHECK(near(middle(scene.drawn()), { 0, 0, 0 }));
    fbe_texture_release(not_a_number);
    fbe_material_release(material);
}

// Its viewport's its own: what's outside's left, by another camera's or the last's; and one that
// doesn't clear lets what's in the target through where it draws nothing, or through its alpha.
TEST_CASE("an HDR camera shares its target as any camera does", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    require(fbe_camera_set_order(scene.camera, 0));
    const fbe::golden::frame_t blue = scene.drawn();
    CHECK(near(middle(blue), { 0, 0, 255 }));
    // A second, HDR, in the left half, clearing its own.
    const std::uint64_t left = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
    require(fbe_camera_set_target(left, scene.target));
    require(fbe_camera_set_order(left, 1));
    require(fbe_camera_set_hdr(left, true));
    require(fbe_camera_set_viewport(left, 0.0F, 0.0F, frame_size / 2.0F,
                                    static_cast<float>(frame_size)));
    require(fbe_camera_set_clear_color(left, 1.0F, 0.0F, 0.0F, 1.0F));
    const fbe::golden::frame_t both = scene.drawn();
    CHECK(near(pixel(both, 16, frame_size / 2), { 255, 0, 0 }));
    CHECK(near(pixel(both, frame_size - 16, frame_size / 2), { 0, 0, 255 }));
    // Clearing nothing: half-see-through red over what the first camera drew there, the same over
    // blue, so three quarters red and a quarter blue, and only that half.
    require(fbe_camera_set_clear(left, false, true));
    const std::uint64_t halfway = unlit(1.0F, 0.0F, 0.0F, 0.5F);
    require(fbe_material_set_blend(halfway, FBE_BLEND_ALPHA));
    wall(scene.world, -3.0F, 10.0F, halfway);
    const fbe::golden::frame_t over = scene.drawn();
    CHECK(near(pixel(over, 16, frame_size / 2), { srgb(0.75F), 0, srgb(0.25F) }, 3));
    CHECK(near(pixel(over, frame_size - 16, frame_size / 2), { srgb(0.5F), 0, srgb(0.5F) }, 3));
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(halfway);
}

TEST_CASE("an HDR camera draws into a multisampled target", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t target = fbe_texture_create_target(32, 32, FBE_TEXTURE_RGBA8, 4, true);
    require(target != 0);
    require(fbe_camera_set_target(scene.camera, target));
    const std::uint64_t material = glowing_wall(scene, 2.0F, 0.0F, 0.0F);
    require(fbe_camera_set_exposure(scene.camera, 0.25F));
    require(fbe_render_world(scene.world, 1.0F));
    const fbe::golden::frame_t frame = scene_t::read(target);
    CHECK(near(pixel(frame, 16, 16), { srgb(0.5F), 0, 0 }, 3));
    CHECK(fbe_gpu_validation_errors() == 0);
    require(fbe_camera_set_target(scene.camera, scene.target));
    fbe_texture_release(target);
    fbe_material_release(material);
}

// A frame's HDR image is a transient target: a frame after a frame asks for the one it had.
TEST_CASE("an HDR camera's image is kept from frame to frame", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = glowing_wall(scene, 0.5F, 0.5F, 0.5F);
    (void)scene.drawn();
    const std::uint32_t images = fbe::gpu::transient_images();
    CHECK(images > 0);
    for (int frame = 0; frame < 4; ++frame) {
        (void)scene.drawn();
        CHECK(fbe::gpu::transient_images() == images);
    }
    fbe_material_release(material);
}

namespace {

/// Turns a sun so it shines from a way, unit length, not straight behind it.
void shine_from(std::uint64_t sun, float x, float y, float z) {
    const float across = std::hypot(x, y);
    const float half = std::atan2(across, z) / 2.0F;
    const float sine = across > 0.0F ? std::sin(half) / across : 0.0F;
    require(
        fbe_entity_set_rotation(sun, -y * sine, x * sine, 0.0F, std::cos(half), FBE_SPACE_WORLD));
}

/// A normal map of numbers, its X and Y what it's given, -1 to 1, and its Z up to length 1.
std::uint64_t normal_map(float x, float y) {
    const std::uint64_t map = fbe_texture_create(2, 2, FBE_TEXTURE_RGBA16F, false);
    require(map != 0);
    const float z = std::sqrt(std::max(1.0F - (x * x) - (y * y), 0.0F));
    std::array<float, 16> pixels{};
    for (std::size_t at = 0; at < 4; ++at) {
        pixels.at(at * 4) = (x + 1.0F) / 2.0F;
        pixels.at((at * 4) + 1) = (y + 1.0F) / 2.0F;
        pixels.at((at * 4) + 2) = (z + 1.0F) / 2.0F;
        pixels.at((at * 4) + 3) = 1.0F;
    }
    require(fbe_texture_write_floats(map, 0, 0, 2, 2, pixels.data()));
    return map;
}

/// What's in the middle of a mesh with its own tangents and with what a pixel makes of its UVs,
/// which have to be the same, under lights from all sides. Its tangents go, the second time.
void tangents_agree(const scene_t& scene, std::uint64_t mesh, std::uint64_t sun) {
    std::vector<rgba_t> own;
    const std::array<std::array<float, 3>, 4> ways{
        std::array{ 0.6F, 0.0F, 0.8F },
        std::array{ -0.6F, 0.0F, 0.8F },
        std::array{ 0.0F, 0.6F, 0.8F },
        std::array{ 0.0F, -0.6F, 0.8F },
    };
    for (const std::array<float, 3>& way : ways) {
        shine_from(sun, way[0], way[1], way[2]);
        own.push_back(middle(scene.drawn()));
    }
    require(fbe_surface_set_tangents(fbe_mesh_geometry(mesh), 0, nullptr, 0));
    for (std::size_t at = 0; at < ways.size(); ++at) {
        shine_from(sun, ways.at(at)[0], ways.at(at)[1], ways.at(at)[2]);
        const rgba_t none = middle(scene.drawn());
        INFO("way " << at << ": " << int{ own.at(at)[0] } << " and " << int{ none[0] });
        CHECK(std::abs(own.at(at)[0] - none[0]) <= 1);
    }
}

/// A wall with its own tangents in a material with a normal map tilted both ways, and a sun.
struct mapped_wall_t {
    std::uint64_t material = fbe_material_create();
    std::uint64_t map = normal_map(0.5F, -0.3F);
    std::uint64_t mesh = 0;
    std::uint64_t sun = 0;

    mapped_wall_t(const scene_t& scene, float half) {
        mesh = wall(scene.world, -3.0F, half, material);
        sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
        require(fbe_material_set_map(material, FBE_MAP_NORMAL, map, 0));
        tangents_of(mesh, 1.0F);
    }
    mapped_wall_t(const mapped_wall_t&) = delete;
    mapped_wall_t& operator=(const mapped_wall_t&) = delete;
    mapped_wall_t(mapped_wall_t&&) = delete;
    mapped_wall_t& operator=(mapped_wall_t&&) = delete;

    ~mapped_wall_t() {
        fbe_texture_release(map);
        fbe_material_release(material);
    }
};

/// A matrix, by columns, of a turn about Y and a scale across.
fbe_mat4_t turned_y(float angle, float across) {
    fbe_mat4_t m{};
    m.m[0] = std::cos(angle) * across;
    m.m[2] = -std::sin(angle) * across;
    m.m[5] = 1.0F;
    m.m[8] = std::sin(angle);
    m.m[10] = std::cos(angle);
    m.m[15] = 1.0F;
    return m;
}

/// What a half float is.
float float_of_half(std::uint32_t half) {
    const std::uint32_t exponent = (half >> 10U) & 0x1FU;
    const float mantissa = static_cast<float>(half & 0x3FFU) / 1024.0F;
    if (exponent == 31) {
        return std::numeric_limits<float>::infinity();
    }
    const float value = exponent == 0
                            ? std::ldexp(mantissa, -14)
                            : std::ldexp(1.0F + mantissa, static_cast<int>(exponent) - 15);
    return (half & 0x8000U) != 0 ? -value : value;
}

/// What's drawn in the middle of a target of half floats.
std::array<float, 3> middle_light(const scene_t& scene, std::uint64_t floats) {
    require(fbe_render_world(scene.world, 1.0F));
    std::array<std::uint8_t, std::size_t{ 8 } * 8 * 8> bytes{};
    require(fbe_texture_read(floats, 0, bytes.data(), sizeof(bytes)));
    std::array<std::uint16_t, 3> halves{};
    std::memcpy(halves.data(), &bytes[((std::size_t{ 4 } * 8) + 4) * 8], sizeof(halves));
    const auto of = [](std::uint32_t half) {
        const std::uint32_t exponent = (half >> 10U) & 0x1FU;
        const float mantissa = static_cast<float>(half & 0x3FFU) / 1024.0F;
        if (exponent == 31) {
            return std::numeric_limits<float>::infinity();
        }
        const float value = exponent == 0
                                ? std::ldexp(mantissa, -14)
                                : std::ldexp(1.0F + mantissa, static_cast<int>(exponent) - 15);
        return (half & 0x8000U) != 0 ? -value : value;
    };
    return { of(halves[0]), of(halves[1]), of(halves[2]) };
}

} // namespace

namespace {

/// A model's vertices and triangles, as a surface holds them.
struct shape_t {
    std::vector<fbe_vertex_t> vertices;
    std::vector<std::uint32_t> indices;
};

/// A vector through a matrix of 16 floats, its last column's weight `w`.
std::array<float, 3> through(std::span<const float> m, std::array<float, 3> v, float w) {
    return {
        (m[0] * v[0]) + (m[4] * v[1]) + (m[8] * v[2]) + (m[12] * w),
        (m[1] * v[0]) + (m[5] * v[1]) + (m[9] * v[2]) + (m[13] * w),
        (m[2] * v[0]) + (m[6] * v[1]) + (m[10] * v[2]) + (m[14] * w),
    };
}

/// Triangles between rings of `around` vertices each, `rings` of them, outward as `flip` says.
void add_rings(shape_t& shape, std::uint32_t rings, std::uint32_t around, bool flip) {
    for (std::uint32_t ring = 0; ring + 1 < rings; ++ring) {
        for (std::uint32_t side = 0; side < around; ++side) {
            const std::uint32_t next = (side + 1) % around;
            const std::uint32_t a = (ring * around) + side;
            const std::uint32_t b = (ring * around) + next;
            const std::uint32_t c = ((ring + 1) * around) + side;
            const std::uint32_t d = ((ring + 1) * around) + next;
            if (flip) {
                shape.indices.insert(shape.indices.end(), { a, c, b, b, c, d });
            } else {
                shape.indices.insert(shape.indices.end(), { a, b, c, b, d, c });
            }
        }
    }
}

/// A tube along X, 4 long and radius 0.5, in 13 rings of 16.
shape_t arm_shape() {
    shape_t shape;
    for (std::uint32_t ring = 0; ring < 13; ++ring) {
        const float x = -2.0F + (static_cast<float>(ring) / 3.0F);
        for (std::uint32_t side = 0; side < 16; ++side) {
            const float turn = 2.0F * std::numbers::pi_v<float> * static_cast<float>(side) / 16.0F;
            const float y = std::cos(turn);
            const float z = std::sin(turn);
            shape.vertices.push_back({
                .position = { .x = x, .y = y * 0.5F, .z = z * 0.5F },
                .normal = { .x = 0.0F, .y = y, .z = z },
                .uv0 = {},
                .uv1 = {},
                .color = { .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F },
            });
        }
    }
    add_rings(shape, 13, 16, true);
    return shape;
}

/// A ball of radius 1, in 17 rings of 24, pole to pole along Y.
shape_t ball_shape() {
    shape_t shape;
    for (std::uint32_t ring = 0; ring < 17; ++ring) {
        const float down = std::numbers::pi_v<float> * static_cast<float>(ring) / 16.0F;
        for (std::uint32_t side = 0; side < 24; ++side) {
            const float turn = 2.0F * std::numbers::pi_v<float> * static_cast<float>(side) / 24.0F;
            const fbe_vec3_t on{
                .x = std::sin(down) * std::cos(turn),
                .y = std::cos(down),
                .z = std::sin(down) * std::sin(turn),
            };
            shape.vertices.push_back({
                .position = on,
                .normal = on,
                .uv0 = {},
                .uv1 = {},
                .color = { .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F },
            });
        }
    }
    add_rings(shape, 17, 24, false);
    return shape;
}

std::uint64_t geometry_of(const shape_t& shape) {
    const std::uint64_t geometry = fbe_geometry_create();
    const std::int32_t surface = fbe_geometry_add_surface(geometry);
    fbe_surface_add_vertices(geometry, surface, shape.vertices.data(),
                             static_cast<std::int32_t>(shape.vertices.size()));
    fbe_surface_add_triangles(geometry, surface, shape.indices.data(),
                              static_cast<std::int32_t>(shape.indices.size()));
    return geometry;
}

/// The ball's targets: its top lifted, with its normals leaning in, and its sides pushed out in X.
void add_ball_targets(std::uint64_t geometry, const shape_t& shape) {
    std::vector<float> lifted;
    std::vector<float> leaned;
    std::vector<float> wider;
    for (const fbe_vertex_t& vertex : shape.vertices) {
        const float top = std::max(vertex.position.y, 0.0F);
        lifted.insert(lifted.end(), { 0.0F, top * 0.8F, 0.0F });
        leaned.insert(leaned.end(),
                      { -vertex.normal.x * top * 0.5F, 0.0F, -vertex.normal.z * top * 0.5F });
        wider.insert(wider.end(), { vertex.position.x * 0.6F, 0.0F, 0.0F });
    }
    const auto count = static_cast<std::int32_t>(shape.vertices.size());
    require(fbe_morph_add_target(geometry, 0, lifted.data(), leaned.data(), count) == 0);
    require(fbe_morph_add_target(geometry, 0, wider.data(), nullptr, count) == 1);
}

/// The arm's joints: "upper" holds it to the left of its elbow, at its middle, "lower" to the
/// right, and they share it for a ring each side.
void set_arm_joints(std::uint64_t geometry, const shape_t& shape) {
    std::vector<fbe_vertex_joints_t> joints(shape.vertices.size());
    for (std::size_t i = 0; i < joints.size(); ++i) {
        const float lower =
            std::clamp((shape.vertices[i].position.x + (1.0F / 3.0F)) * 1.5F, 0.0F, 1.0F);
        joints[i].joints[0] = 0;
        joints[i].joints[1] = 1;
        joints[i].weights[0] = 1.0F - lower;
        joints[i].weights[1] = lower;
    }
    require(fbe_surface_set_joints(geometry, 0, joints.data(),
                                   static_cast<std::int32_t>(joints.size())));
}

/// An arm: its root, where it's placed; "upper", at its shoulder; "lower", at its elbow, under it;
/// its mesh, under the root; its skin; and a clip of a second turning its elbow a quarter turn
/// about Z, and its shoulder an eighth the other way.
struct arm_t {
    std::uint64_t root = 0;
    std::uint64_t mesh = 0;
    std::uint64_t skin = 0;
    std::uint64_t clip = 0;
    std::uint64_t player = 0;

    arm_t(std::uint64_t world, std::uint64_t geometry) {
        root = fbe_entity_create(world, 0, FBE_ENTITY_PIVOT);
        require(fbe_entity_set_position(root, 0.0F, 0.0F, -8.0F, FBE_SPACE_LOCAL));
        const std::uint64_t upper = fbe_entity_create(world, root, FBE_ENTITY_PIVOT);
        require(fbe_entity_set_name(upper, "upper"));
        require(fbe_entity_set_position(upper, -2.0F, 0.0F, 0.0F, FBE_SPACE_LOCAL));
        const std::uint64_t lower = fbe_entity_create(world, upper, FBE_ENTITY_PIVOT);
        require(fbe_entity_set_name(lower, "lower"));
        require(fbe_entity_set_position(lower, 2.0F, 0.0F, 0.0F, FBE_SPACE_LOCAL));
        mesh = fbe_entity_create(world, root, FBE_ENTITY_MESH);
        require(fbe_mesh_set_geometry(mesh, geometry));
        skin = fbe_skin_create();
        // From the mesh's space to each joint's, where it is in the bind pose.
        const std::array<float, 16> to_upper{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 0, 0, 1 };
        const std::array<float, 16> to_lower{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        require(fbe_skin_add_joint(skin, "upper", to_upper.data()));
        require(fbe_skin_add_joint(skin, "lower", to_lower.data()));
        require(fbe_skin_attach(mesh, skin, root));
        clip = fbe_clip_create("bend", 0.0F);
        const std::array<float, 2> times{ 0.0F, 1.0F };
        const float eighth = std::numbers::pi_v<float> / 8.0F;
        const float quarter = eighth * 2.0F;
        const std::array<float, 8> bent{ 0, 0, 0, 1, 0, 0, std::sin(quarter), std::cos(quarter) };
        const std::array<float, 8> leaned{ 0, 0, 0, 1, 0, 0, -std::sin(eighth), std::cos(eighth) };
        require(fbe_clip_add_channel(clip, "lower", FBE_CLIP_ROTATION, FBE_CLIP_LINEAR, 4,
                                     times.data(), bent.data(), 2));
        require(fbe_clip_add_channel(clip, "upper", FBE_CLIP_ROTATION, FBE_CLIP_LINEAR, 4,
                                     times.data(), leaned.data(), 2));
        player = fbe_player_create(root);
        require(fbe_player_play(player, 0, clip, FBE_PLAYER_ONCE, 1.0F));
    }
    arm_t(const arm_t&) = delete;
    arm_t& operator=(const arm_t&) = delete;
    arm_t(arm_t&&) = delete;
    arm_t& operator=(arm_t&&) = delete;

    ~arm_t() {
        fbe_player_free(player);
        fbe_clip_release(clip);
        fbe_skin_release(skin);
    }
};

/// A scene lit from above and to the side, with a little ambient, on a dark clear.
void light_for_shapes(const scene_t& scene) {
    require(fbe_camera_set_clear_color(scene.camera, 0.05F, 0.05F, 0.1F, 1.0F));
    require(fbe_world_set_ambient(scene.world, 0.15F, 0.15F, 0.15F));
    const std::uint64_t lamp = light(scene.world, FBE_LIGHT_POINT, 4.0F, 4.0F, 0.0F, 30.0F);
    require(fbe_light_set_intensity(lamp, 80.0F));
}

/// `shape` with the vertices its mesh's morph has now, as fbe_morph_vertices reads them.
shape_t morphed(const shape_t& shape, std::uint64_t mesh) {
    const auto count = shape.vertices.size();
    std::vector<float> positions(count * 3);
    std::vector<float> normals(count * 3);
    require(fbe_morph_vertices(mesh, 0, positions.data(), normals.data(),
                               static_cast<std::int32_t>(count)) ==
            static_cast<std::int32_t>(count));
    shape_t made = shape;
    for (std::size_t i = 0; i < count; ++i) {
        made.vertices[i].position = {
            .x = positions[i * 3],
            .y = positions[(i * 3) + 1],
            .z = positions[(i * 3) + 2],
        };
        made.vertices[i].normal = {
            .x = normals[i * 3],
            .y = normals[(i * 3) + 1],
            .z = normals[(i * 3) + 2],
        };
    }
    return made;
}

/// `shape` with its vertices through the skin of a mesh, by its joints and palette, as a skinning
/// shader that's right would have them: its positions by a weighted sum of the matrices, and its
/// normals by that sum's cofactors, with its determinant's sign.
shape_t skinned(const shape_t& shape, std::uint64_t mesh, std::uint64_t geometry) {
    std::vector<float> palette(std::size_t{ 64 } * 16);
    const std::int32_t joints_in_it = fbe_skin_palette(mesh, palette.data(), 64);
    require(joints_in_it > 0);
    std::vector<fbe_vertex_joints_t> joints(shape.vertices.size());
    require(
        fbe_surface_joints(geometry, 0, joints.data(), static_cast<std::int32_t>(joints.size())) ==
        static_cast<std::int32_t>(joints.size()));
    shape_t made = shape;
    for (std::size_t i = 0; i < joints.size(); ++i) {
        std::array<float, 16> sum{};
        for (std::size_t j = 0; j < 4; ++j) {
            const std::span<const float> matrix = std::span<const float>(palette).subspan(
                std::size_t{ joints[i].joints[j] } * 16, 16);
            for (std::size_t k = 0; k < 16; ++k) {
                sum.at(k) += matrix[k] * joints[i].weights[j];
            }
        }
        const fbe_vec3_t& p = shape.vertices[i].position;
        const fbe_vec3_t& n = shape.vertices[i].normal;
        const std::array<float, 3> place = through(sum, { p.x, p.y, p.z }, 1.0F);
        made.vertices[i].position = { .x = place[0], .y = place[1], .z = place[2] };
        const auto cross = [](std::array<float, 3> a, std::array<float, 3> b) {
            return std::array<float, 3>{
                (a[1] * b[2]) - (a[2] * b[1]),
                (a[2] * b[0]) - (a[0] * b[2]),
                (a[0] * b[1]) - (a[1] * b[0]),
            };
        };
        const std::array<float, 3> a{ sum[0], sum[1], sum[2] };
        const std::array<float, 3> b{ sum[4], sum[5], sum[6] };
        const std::array<float, 3> c{ sum[8], sum[9], sum[10] };
        const std::array<float, 3> bc = cross(b, c);
        const std::array<float, 3> ca = cross(c, a);
        const std::array<float, 3> ab = cross(a, b);
        made.vertices[i].normal = {
            .x = (bc[0] * n.x) + (ca[0] * n.y) + (ab[0] * n.z),
            .y = (bc[1] * n.x) + (ca[1] * n.y) + (ab[1] * n.z),
            .z = (bc[2] * n.x) + (ca[2] * n.y) + (ab[2] * n.z),
        };
    }
    return made;
}

/// What `shape` looks like as plain vertices, with nothing to skin or morph it, in place of a mesh
/// that's hidden while it's drawn.
fbe::golden::frame_t drawn_baked(const scene_t& scene, const shape_t& shape, std::uint64_t mesh,
                                 std::uint64_t parent) {
    const std::uint64_t geometry = geometry_of(shape);
    const std::uint64_t baked = fbe_entity_create(scene.world, parent, FBE_ENTITY_MESH);
    require(fbe_mesh_set_geometry(baked, geometry));
    fbe_geometry_release(geometry);
    require(fbe_entity_set_hidden(mesh, true));
    fbe::golden::frame_t frame = scene.drawn();
    require(fbe_entity_set_hidden(mesh, false));
    fbe_entity_free(baked);
    return frame;
}

/// What's at a place on the plane `z` in front of the camera covers the clear, which is bluer than
/// any shape's: by the view's 60 degrees up, at 128 pixels, worked out from the place alone.
bool covered(const fbe::golden::frame_t& frame, float x, float y, float z) {
    const float per = 64.0F / (std::tan(std::numbers::pi_v<float> / 6.0F) * -z);
    const rgba_t at = pixel(frame, static_cast<std::uint32_t>(64.0F + (x * per)),
                            static_cast<std::uint32_t>(64.0F - (y * per)));
    return at[2] <= at[0] + 10;
}

/// An arm at 8 away, `up` degrees at its shoulder and `bent` more at its elbow, is where its bones
/// say: on the line from its shoulder to its elbow to its hand, and not 1.2 to either side of it,
/// its radius being 0.5.
void check_arm_where(const fbe::golden::frame_t& frame, float up, float bent, float forearm) {
    const auto turn = [](float degrees) {
        const float radians = degrees * std::numbers::pi_v<float> / 180.0F;
        return std::array<float, 2>{ std::cos(radians), std::sin(radians) };
    };
    const std::array<float, 2> shoulder{ -2.0F, 0.0F };
    const std::array<float, 2> upper = turn(up);
    const std::array<float, 2> lower = turn(up + bent);
    const float elbow_x = shoulder[0] + (2.0F * upper[0]);
    const float elbow_y = shoulder[1] + (2.0F * upper[1]);
    const std::array<float, 2> elbow{ elbow_x, elbow_y };
    const auto check_along = [&](std::array<float, 2> from, std::array<float, 2> along,
                                 float length, std::array<float, 2> other) {
        for (const float part : { 0.2F, 0.5F, 0.8F }) {
            const float x = from[0] + (along[0] * length * part);
            const float y = from[1] + (along[1] * length * part);
            CAPTURE(x, y);
            CHECK(covered(frame, x, y, -8.0F));
            // On whichever side the other part of the arm isn't.
            const float side =
                (((other[0] * -along[1]) + (other[1] * along[0])) > 0.0F) ? -1.2F : 1.2F;
            CHECK_FALSE(covered(frame, x - (along[1] * side), y + (along[0] * side), -8.0F));
        }
    };
    check_along(shoulder, upper, 2.0F, lower);
    check_along(elbow, lower, forearm, std::array<float, 2>{ -upper[0], -upper[1] });
}

/// A ball at 6 away, `lift` higher at its top and `wide` wider in X, has its silhouette where that
/// puts it, 3 pixels in and 5 out for its widening a little in the view.
void check_ball_where(const fbe::golden::frame_t& frame, float lift, float wide) {
    const float per = 64.0F / (std::tan(std::numbers::pi_v<float> / 6.0F) * 6.0F);
    const float across = (1.0F + wide) * per;
    const float top = 64.0F - ((1.0F + lift) * per);
    const float bottom = 64.0F + per;
    const auto at = [&](float x, float y) {
        const rgba_t found =
            pixel(frame, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
        return found[2] <= found[0] + 10;
    };
    const float widest = 64.0F;
    CHECK(at(64.0F - across + 3.0F, widest));
    CHECK(at(64.0F + across - 3.0F, widest));
    CHECK_FALSE(at(64.0F - across - 5.0F, widest));
    CHECK_FALSE(at(64.0F + across + 5.0F, widest));
    CHECK(at(64.0F, top + 3.0F));
    CHECK(at(64.0F, bottom - 3.0F));
    CHECK_FALSE(at(64.0F, top - 5.0F));
    CHECK_FALSE(at(64.0F, bottom + 5.0F));
}

/// `frame` is the same picture as `reference` but for a few pixels at edges, where a vertex's
/// rounding can tip one.
void check_same_picture(const fbe::golden::frame_t& reference, const fbe::golden::frame_t& frame) {
    const fbe::golden::difference_t difference = fbe::golden::compare(reference, frame, 3);
    CAPTURE(difference.pixels, difference.furthest);
    CHECK(difference.share <= 0.002);
}

} // namespace

// R1: stored as color it's decoded, which tilts a flat normal 40 degrees, so it's refused; the same
// numbers either way it's allowed.
TEST_CASE("a map that holds numbers can't be stored as color", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    const std::uint64_t color = flat_texture(128, 128, 255, 255, FBE_TEXTURE_RGBA8);
    const std::uint64_t numbers = flat_texture(128, 128, 255, 255, FBE_TEXTURE_RGBA8_LINEAR);
    const std::uint64_t one_channel = fbe_texture_create(2, 2, FBE_TEXTURE_R8, false);
    const std::uint64_t floats =
        normal_map((128.0F / 255.0F * 2.0F) - 1.0F, (128.0F / 255.0F * 2.0F) - 1.0F);
    for (const std::int32_t map : { FBE_MAP_METAL_ROUGH, FBE_MAP_NORMAL, FBE_MAP_OCCLUSION }) {
        INFO("map " << map);
        CHECK(!fbe_material_set_map(material, map, color, 0));
        CHECK(std::string(fbe_error()).contains("aren't colors"));
        CHECK(fbe_material_map(material, map) == 0);
        CHECK(fbe_material_set_map(material, map, numbers, 0));
        CHECK(fbe_material_set_map(material, map, floats, 0));
        CHECK(fbe_material_set_map(material, map, 0, 0));
    }
    CHECK(!fbe_material_set_map(material, FBE_MAP_NORMAL, one_channel, 0));
    CHECK(!fbe_material_set_map(material, FBE_MAP_METAL_ROUGH, one_channel, 0));
    CHECK(fbe_material_set_map(material, FBE_MAP_OCCLUSION, one_channel, 0));
    CHECK(fbe_material_set_map(material, FBE_MAP_OCCLUSION, 0, 0));
    CHECK(fbe_material_set_map(material, FBE_MAP_EMISSIVE, color, 0));
    CHECK(fbe_material_set_map(material, FBE_MAP_EMISSIVE, 0, 0));
    // The same pixel from each that's allowed.
    wall(scene.world, -3.0F, 10.0F, material);
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    shine_from(sun, 0.6F, 0.0F, 0.8F);
    require(fbe_material_set_map(material, FBE_MAP_NORMAL, numbers, 0));
    const rgba_t bytes = middle(scene.drawn());
    require(fbe_material_set_map(material, FBE_MAP_NORMAL, floats, 0));
    CHECK(near(middle(scene.drawn()), { bytes[0], bytes[1], bytes[2] }, 1));
    for (const std::uint64_t texture : { color, numbers, one_channel, floats }) {
        fbe_texture_release(texture);
    }
    fbe_material_release(material);
}

// R2, R4: what moves a surface's vertices moves its tangents, as its UVs say they should be.
TEST_CASE("a surface's tangents go with its geometry", "[gpu][scene]") {
    SECTION("turned") {
        const scene_t scene;
        const mapped_wall_t made(scene, 2.0F);
        const fbe_mat4_t turn = turned_y(0.6F, 1.0F);
        require(fbe_geometry_transform(fbe_mesh_geometry(made.mesh), &turn));
        tangents_agree(scene, made.mesh, made.sun);
    }
    SECTION("mirrored") {
        const scene_t scene;
        const mapped_wall_t made(scene, 2.0F);
        const fbe_mat4_t mirror = turned_y(0.0F, -1.0F);
        require(fbe_geometry_transform(fbe_mesh_geometry(made.mesh), &mirror));
        std::array<fbe_vertex_tangent_t, 4> found{};
        CHECK(fbe_surface_tangents(fbe_mesh_geometry(made.mesh), 0, found.data(), 4) == 4);
        CHECK(found[0].x == -1.0F);
        CHECK(found[0].w == -1.0F);
        tangents_agree(scene, made.mesh, made.sun);
    }
    SECTION("added to another") {
        const scene_t scene;
        const mapped_wall_t made(scene, 2.0F);
        const std::uint64_t whole = fbe_geometry_create();
        const fbe_mat4_t turn = turned_y(0.6F, 1.0F);
        require(fbe_geometry_add(whole, fbe_mesh_geometry(made.mesh), &turn));
        require(fbe_mesh_set_geometry(made.mesh, whole));
        fbe_geometry_release(whole);
        tangents_agree(scene, made.mesh, made.sun);
    }
    SECTION("flipped") {
        const scene_t scene;
        const mapped_wall_t made(scene, 2.0F);
        require(fbe_geometry_flip(fbe_mesh_geometry(made.mesh)));
        require(fbe_entity_set_rotation(made.mesh, 0.0F, 1.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
        std::array<fbe_vertex_tangent_t, 4> found{};
        CHECK(fbe_surface_tangents(fbe_mesh_geometry(made.mesh), 0, found.data(), 4) == 4);
        CHECK(found[0].w == -1.0F);
        tangents_agree(scene, made.mesh, made.sun);
    }
}

// R3: a bone that mirrors, two-sided since it turns the surface around.
TEST_CASE("a bone that mirrors turns a surface's bitangent", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    const skinned_wall_t made = skinned_wall(scene.world, material, -3.0F, 2.0F);
    all_on(made.geometry, 1, 1.0F);
    tangents_of(made.mesh, 1.0F);
    const std::uint64_t map = normal_map(0.5F, -0.3F);
    require(fbe_material_set_map(material, FBE_MAP_NORMAL, map, 0));
    require(fbe_material_set_two_sided(material, true));
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    require(fbe_entity_set_scale(made.moving, -1.0F, 1.0F, 1.0F, FBE_SPACE_WORLD));
    tangents_agree(scene, made.mesh, sun);
    fbe_texture_release(map);
    fbe_skin_release(made.skin);
    fbe_material_release(material);
}

// R5: glTF's back face has -xT -yB -zN: all of it turned around, so what's tilted toward its right
// on its front, seen from behind, is tilted away from the same world's way.
TEST_CASE("a two-sided surface's back turns its normal map around with it", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_two_sided(material, true));
    const std::uint64_t mesh = wall(scene.world, -3.0F, 2.0F, material);
    // Turned half way about Y: its back's to the eye, its tangent's -X, and its bitangent's +Y.
    require(fbe_entity_set_rotation(mesh, 0.0F, 1.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    const std::uint64_t right = normal_map(0.7F, 0.0F);
    const std::uint64_t up = normal_map(0.0F, 0.7F);
    const auto seen = [&](float x, float y) {
        shine_from(sun, x, y, 0.8F);
        return int{ middle(scene.drawn())[0] };
    };
    for (const bool own : { true, false }) {
        INFO("its own tangents: " << own);
        if (own) {
            tangents_of(mesh, 1.0F);
        } else {
            require(fbe_surface_set_tangents(fbe_mesh_geometry(mesh), 0, nullptr, 0));
        }
        // -xT, and T's -X: toward +X.
        require(fbe_material_set_map(material, FBE_MAP_NORMAL, right, 0));
        CHECK(seen(0.6F, 0.0F) > seen(-0.6F, 0.0F) + 10);
        // -yB, and B's +Y: toward -Y.
        require(fbe_material_set_map(material, FBE_MAP_NORMAL, up, 0));
        CHECK(seen(0.0F, -0.6F) > seen(0.0F, 0.6F) + 10);
    }
    fbe_texture_release(right);
    fbe_texture_release(up);
    fbe_material_release(material);
}

// R6: its own tangents go with what turns a map's UVs, as what a pixel makes of them does; on its
// second UV set, they aren't that set's.
TEST_CASE("a surface's tangents turn with a normal map's UVs", "[gpu][scene]") {
    for (const auto& [scale_u, scale_v, angle] : {
             std::tuple{ 1.0F, 1.0F, 0.7F },
             std::tuple{ -1.0F, 1.0F, 0.0F },
             std::tuple{ 2.0F, 0.5F, 0.0F },
             std::tuple{ -1.0F, 2.0F, 2.1F },
         }) {
        INFO("scale " << scale_u << ", " << scale_v << ", angle " << angle);
        const scene_t scene;
        const mapped_wall_t made(scene, 2.0F);
        require(fbe_material_set_map_uv(made.material, FBE_MAP_NORMAL, scale_u, scale_v, angle,
                                        0.25F, 0.0F));
        tangents_agree(scene, made.mesh, made.sun);
    }
    const scene_t scene;
    const mapped_wall_t made(scene, 2.0F);
    const std::uint64_t geometry = fbe_mesh_geometry(made.mesh);
    std::array<fbe_vertex_t, 4> vertices{};
    require(fbe_surface_vertices(geometry, 0, 0, vertices.data(), 4));
    for (fbe_vertex_t& vertex : vertices) {
        vertex.uv1 = { .x = vertex.uv0.y, .y = vertex.uv0.x };
    }
    require(fbe_surface_set_vertices(geometry, 0, 0, vertices.data(), 4));
    require(fbe_material_set_map(made.material, FBE_MAP_NORMAL, made.map, 1));
    tangents_agree(scene, made.mesh, made.sun);
}

// R7: what a tangent can't be, and a w that's only a way.
TEST_CASE("a tangent's finite, and its w a way", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t mesh = wall(scene.world, -3.0F, 1.0F);
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    const auto refused = [&](float x, float w) {
        const std::array<fbe_vertex_tangent_t, 4> tangents{
            fbe_vertex_tangent_t{ .x = 1.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F },
            fbe_vertex_tangent_t{ .x = 1.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F },
            fbe_vertex_tangent_t{ .x = 1.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F },
            fbe_vertex_tangent_t{ .x = x, .y = 0.0F, .z = 0.0F, .w = w },
        };
        return !fbe_surface_set_tangents(geometry, 0, tangents.data(), 4) &&
               fbe_surface_tangents(geometry, 0, nullptr, 0) == 0;
    };
    CHECK(refused(std::nanf(""), 1.0F));
    CHECK(refused(std::numeric_limits<float>::infinity(), 1.0F));
    CHECK(refused(1.0F, 0.25F));
    CHECK(refused(1.0F, 0.0F));
    CHECK(refused(1.0F, std::nanf("")));
    CHECK(!refused(1.0F, -1.0F));
}

// R8: more base color than there is light comes to as much as there is: never less than none, as
// Fresnel past 1 was, nor all multiple scattering's division.
TEST_CASE("a base color past white gives back all there is", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t floats = fbe_texture_create_target(8, 8, FBE_TEXTURE_RGBA16F, 1, true);
    require(floats != 0);
    require(fbe_camera_set_target(scene.camera, floats));
    const std::uint64_t material = fbe_material_create();
    wall(scene.world, -3.0F, 10.0F, material);
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    require(fbe_material_set_metal_rough(material, 0.5F, 1.0F));
    require(fbe_material_set_color(material, 1.0F, 1.0F, 1.0F, 1.0F));
    const std::array<float, 3> white = middle_light(scene, floats);
    require(fbe_material_set_color(material, 4.0F, 4.0F, 4.0F, 1.0F));
    const std::array<float, 3> past = middle_light(scene, floats);
    CHECK(past[0] > 0.0F);
    CHECK(std::abs(past[0] - white[0]) < 0.001F);
    fbe_entity_free(sun);
    const std::uint64_t source = even_light(1.0F, 1.0F, 1.0F);
    const std::uint64_t environment = fbe_environment_bake(source, 16, 16);
    REQUIRE(environment != 0);
    fbe_texture_release(source);
    require(fbe_world_set_environment(scene.world, environment, 1.0F));
    require(fbe_material_set_metal_rough(material, 1.0F, 1.0F));
    require(fbe_material_set_color(material, 2.0F, 2.0F, 2.0F, 1.0F));
    const std::array<float, 3> furnace = middle_light(scene, floats);
    CHECK(std::abs(furnace[0] - 1.0F) < 0.02F);
    require(fbe_camera_set_target(scene.camera, scene.target));
    fbe_texture_release(floats);
    fbe_environment_free(environment);
    fbe_material_release(material);
}

// R9: a scale that great's as tilted as can be, not nothing; and a UV scale that small's refused.
TEST_CASE("a normal scale past what a square holds tilts all the way", "[gpu][scene]") {
    const scene_t scene;
    const mapped_wall_t made(scene, 10.0F);
    const std::uint64_t right = normal_map(0.5F, 0.0F);
    require(fbe_material_set_map(made.material, FBE_MAP_NORMAL, right, 0));
    shine_from(made.sun, 0.6F, 0.0F, 0.8F);
    require(fbe_material_set_normal_scale(made.material, 1e6F));
    const rgba_t far_enough = middle(scene.drawn());
    require(fbe_material_set_normal_scale(made.material, 1e30F));
    CHECK(near(middle(scene.drawn()), { far_enough[0], far_enough[1], far_enough[2] }, 1));
    CHECK(far_enough[0] > 0);
    CHECK(!fbe_material_set_map_uv(made.material, FBE_MAP_NORMAL, 1e-40F, 1.0F, 0.0F, 0.0F, 0.0F));
    CHECK(!fbe_material_set_layer_uv(made.material, 0, 1.0F, 1e-40F, 0.0F, 0.0F, 0.0F));
    fbe_texture_release(right);
}

// R10: an entity scaled small has short tangents, whose map's as good.
TEST_CASE("a normal map tilts a surface however small it's scaled", "[gpu][scene]") {
    const scene_t scene;
    const mapped_wall_t made(scene, 2.0F);
    shine_from(made.sun, 0.6F, 0.0F, 0.8F);
    const rgba_t whole = middle(scene.drawn());
    constexpr float small = 5e-5F;
    require(fbe_camera_set_range(scene.camera, 1e-6F, 1.0F));
    require(fbe_entity_set_scale(made.mesh, small, small, small, FBE_SPACE_WORLD));
    require(fbe_entity_set_position(made.mesh, 0.0F, 0.0F, -3.0F * small, FBE_SPACE_WORLD));
    CHECK(near(middle(scene.drawn()), { whole[0], whole[1], whole[2] }, 1));
    require(fbe_material_set_map(made.material, FBE_MAP_NORMAL, 0, 0));
    CHECK(!near(middle(scene.drawn()), { whole[0], whole[1], whole[2] }, 10));
}

// T2: its eye in the world's too. A mirror turned half up reflects the sky straight above, in a
// world where the eye's along +X.
TEST_CASE("a smooth metal reflects the world's way, under a turned camera", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_metal_rough(material, 1.0F, 0.0F));
    const std::uint64_t mesh = wall(scene.world, 0.0F, 10.0F, material);
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    std::array<fbe_vertex_t, 4> vertices{};
    require(fbe_surface_vertices(geometry, 0, 0, vertices.data(), 4));
    const float root = std::numbers::sqrt2_v<float> / 2.0F;
    for (fbe_vertex_t& vertex : vertices) {
        vertex.normal = { .x = 0.0F, .y = root, .z = root };
    }
    require(fbe_surface_set_vertices(geometry, 0, 0, vertices.data(), 4));
    const std::uint64_t source = fbe_texture_create(4, 4, FBE_TEXTURE_RGBA16F, false);
    REQUIRE(source != 0);
    std::vector<float> pixels;
    for (std::int32_t y = 0; y < 4; ++y) {
        for (std::int32_t x = 0; x < 4; ++x) {
            const float lit = y < 2 ? 1.0F : 0.0F;
            pixels.insert(pixels.end(), { lit, lit, lit, 1.0F });
        }
    }
    require(fbe_texture_write_floats(source, 0, 0, 4, 4, pixels.data()));
    const std::uint64_t environment = fbe_environment_bake(source, 32, 16);
    REQUIRE(environment != 0);
    fbe_texture_release(source);
    require(fbe_world_set_environment(scene.world, environment, 1.0F));
    // Facing +X, both of them turned a quarter about Y, with the eye off along +X.
    const float half = std::numbers::pi_v<float> / 4.0F;
    require(
        fbe_entity_set_rotation(mesh, 0.0F, std::sin(half), 0.0F, std::cos(half), FBE_SPACE_WORLD));
    require(fbe_entity_set_rotation(scene.camera, 0.0F, std::sin(half), 0.0F, std::cos(half),
                                    FBE_SPACE_WORLD));
    require(fbe_entity_set_position(scene.camera, 3.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    CHECK(middle(scene.drawn())[0] > srgb(0.95F));
    fbe_environment_free(environment);
    fbe_material_release(material);
}

// T3: occlusion's of an environment's light as well as ambient's.
TEST_CASE("occlusion darkens an environment", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    wall(scene.world, -3.0F, 10.0F, material);
    const std::uint64_t source = even_light(1.0F, 1.0F, 1.0F);
    const std::uint64_t environment = fbe_environment_bake(source, 16, 16);
    REQUIRE(environment != 0);
    fbe_texture_release(source);
    require(fbe_world_set_environment(scene.world, environment, 1.0F));
    const std::uint64_t half = flat_texture(128, 9, 9, 255, FBE_TEXTURE_RGBA8_LINEAR);
    require(fbe_material_set_map(material, FBE_MAP_OCCLUSION, half, 0));
    const std::uint8_t darkened = srgb(128.0F / 255.0F);
    CHECK(near(middle(scene.drawn()), { darkened, darkened, darkened }, 3));
    fbe_texture_release(half);
    fbe_environment_free(environment);
    fbe_material_release(material);
}

// T4: a tangent's a direction along the surface, which scale takes with it, as a normal's isn't. On
// the diagonal under a scale across, as the UVs go.
TEST_CASE("a surface's scale takes its tangents along it", "[gpu][scene]") {
    const scene_t scene;
    const mapped_wall_t made(scene, 2.0F);
    const std::uint64_t geometry = fbe_mesh_geometry(made.mesh);
    std::array<fbe_vertex_t, 4> vertices{};
    require(fbe_surface_vertices(geometry, 0, 0, vertices.data(), 4));
    for (fbe_vertex_t& vertex : vertices) {
        vertex.uv0 = {
            .x = vertex.position.x + vertex.position.y,
            .y = vertex.position.y - vertex.position.x,
        };
    }
    require(fbe_surface_set_vertices(geometry, 0, 0, vertices.data(), 4));
    const float root = std::numbers::sqrt2_v<float> / 2.0F;
    const fbe_vertex_tangent_t diagonal{ .x = root, .y = root, .z = 0.0F, .w = -1.0F };
    const std::array tangents{ diagonal, diagonal, diagonal, diagonal };
    require(fbe_surface_set_tangents(geometry, 0, tangents.data(), 4));
    require(fbe_entity_set_scale(made.mesh, 1.0F, 4.0F, 1.0F, FBE_SPACE_WORLD));
    tangents_agree(scene, made.mesh, made.sun);
}

// T5.
TEST_CASE("a surface's tangents go with its vertices when it's cleared", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t mesh = wall(scene.world, -3.0F, 1.0F);
    tangents_of(mesh, 1.0F);
    require(fbe_surface_clear(fbe_mesh_geometry(mesh), 0, false));
    CHECK(fbe_surface_tangents(fbe_mesh_geometry(mesh), 0, nullptr, 0) == 4);
    require(fbe_surface_clear(fbe_mesh_geometry(mesh), 0, true));
    CHECK(fbe_surface_tangents(fbe_mesh_geometry(mesh), 0, nullptr, 0) == 0);
}

// T6: what a camera's drawing into can't be read, as a map or a layer: it's no map.
TEST_CASE("a camera drawing into its own material's map draws as with none", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t target = fbe_texture_create_target(8, 8, FBE_TEXTURE_RGBA8_LINEAR, 1, true);
    require(target != 0);
    require(fbe_camera_set_target(scene.camera, target));
    const std::uint64_t material = fbe_material_create();
    wall(scene.world, -3.0F, 10.0F, material);
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    shine_from(sun, 0.6F, 0.0F, 0.8F);
    const auto seen = [&] {
        require(fbe_render_world(scene.world, 1.0F));
        const fbe::golden::frame_t frame = scene_t::read(target);
        return pixel(frame, 4, 4);
    };
    const rgba_t none = seen();
    require(fbe_material_set_map(material, FBE_MAP_NORMAL, target, 0));
    CHECK(near(seen(), { none[0], none[1], none[2] }, 1));
    CHECK(fbe_gpu_validation_errors() == 0);
    require(fbe_material_set_map(material, FBE_MAP_NORMAL, 0, 0));
    require(fbe_camera_set_target(scene.camera, scene.target));
    fbe_texture_release(target);
    fbe_material_release(material);
}

// T7: an environment a stopped GPU lost lights nothing, and stays set till it's set again.
TEST_CASE("an environment a stopped GPU lost lights nothing", "[gpu][scene]") {
    const std::uint64_t world = fbe_world_create();
    require(fbe_world_set_ambient(world, 0.0F, 0.0F, 0.0F));
    const std::uint64_t camera = fbe_entity_create(world, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t first = fbe_texture_create_target(16, 16, FBE_TEXTURE_RGBA8, 1, true);
    require(fbe_camera_set_target(camera, first));
    const std::uint64_t material = fbe_material_create();
    wall(world, -3.0F, 10.0F, material);
    const std::uint64_t source = even_light(1.0F, 1.0F, 1.0F);
    const std::uint64_t environment = fbe_environment_bake(source, 16, 8);
    REQUIRE(environment != 0);
    fbe_texture_release(source);
    require(fbe_world_set_environment(world, environment, 1.0F));
    require(fbe_render_world(world, 1.0F));
    CHECK(near(pixel(scene_t::read(first), 8, 8), { 255, 255, 255 }, 3));
    stop_gpu();
    const std::uint64_t second = fbe_texture_create_target(16, 16, FBE_TEXTURE_RGBA8, 1, true);
    require(fbe_camera_set_target(camera, second));
    require(fbe_camera_set_clear_color(camera, 0.0F, 0.0F, 0.0F, 1.0F));
    require(fbe_render_world(world, 1.0F));
    CHECK(near(pixel(scene_t::read(second), 8, 8), { 0, 0, 0 }));
    CHECK(fbe_world_environment(world) == environment);
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_environment_free(environment);
    fbe_world_free(world);
    std::array<std::uint64_t, 16> some{};
    while (fbe_entity_freed(some.data(), some.size()) != 0) {}
    fbe_texture_release(second);
    fbe_material_release(material);
    stop_gpu();
}

// T7: through a mirror's flipped view, a surface's tangents and an environment are as they should
// be without it: what's reflected from below looks as what's above does from there, seen the same
// way.
TEST_CASE("tangents and an environment hold through a mirror's view", "[gpu][scene]") {
    const auto draw = [](bool mirrored) {
        const scene_t scene;
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
        const std::uint64_t material = fbe_material_create();
        require(fbe_material_set_metal_rough(material, 0.0F, 0.6F));
        const std::uint64_t map = normal_map(0.5F, 0.0F);
        require(fbe_material_set_map(material, FBE_MAP_NORMAL, map, 0));
        // A wall facing +Z, which only its reflection shows, below the plane, from a camera below.
        const std::uint64_t mesh = wall(scene.world, -3.0F, 2.0F, material);
        tangents_of(mesh, 1.0F);
        const std::uint64_t source = fbe_texture_create(4, 4, FBE_TEXTURE_RGBA16F, false);
        REQUIRE(source != 0);
        std::vector<float> pixels;
        for (std::int32_t y = 0; y < 4; ++y) {
            for (std::int32_t x = 0; x < 4; ++x) {
                const float lit = x < 2 ? 1.0F : 0.0F;
                pixels.insert(pixels.end(), { lit, lit, lit, 1.0F });
            }
        }
        require(fbe_texture_write_floats(source, 0, 0, 4, 4, pixels.data()));
        const std::uint64_t environment = fbe_environment_bake(source, 16, 32);
        REQUIRE(environment != 0);
        fbe_texture_release(source);
        require(fbe_world_set_environment(scene.world, environment, 1.0F));
        if (mirrored) {
            // A plane at y = 0, with the wall above it and the eye below looking at its reflection.
            require(fbe_entity_set_position(mesh, 0.0F, 3.0F, -3.0F, FBE_SPACE_WORLD));
            require(fbe_entity_set_position(scene.camera, 0.0F, -3.0F, 0.0F, FBE_SPACE_WORLD));
            const std::uint64_t mirror = fbe_entity_create(scene.world, 0, FBE_ENTITY_MIRROR);
            require(fbe_entity_set_position(mirror, 0.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
        }
        const rgba_t seen = middle(scene.drawn());
        CHECK(fbe_gpu_validation_errors() == 0);
        fbe_environment_free(environment);
        fbe_texture_release(map);
        fbe_material_release(material);
        return seen;
    };
    const rgba_t direct = draw(false);
    const rgba_t reflected = draw(true);
    CHECK(int{ reflected[0] } > 0);
    INFO("direct " << int{ direct[0] } << ", reflected " << int{ reflected[0] });
    CHECK(reflected[0] != 0);
    CHECK(std::abs(direct[0] - reflected[0]) <= 3);
}

// A sprite has no normal, so nothing but ambient light lights it in a PBR material: no direction to
// find an environment's light or a sun's by, which isn't the NaN of finding one.
TEST_CASE("what has no normal is lit only by ambient light", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_color(material, 1.0F, 0.5F, 0.25F, 1.0F));
    const std::uint64_t sprite = fbe_entity_create(scene.world, 0, FBE_ENTITY_SPRITE);
    require(fbe_entity_set_position(sprite, 0.0F, 0.0F, -3.0F, FBE_SPACE_WORLD));
    require(fbe_sprite_set_material(sprite, material));
    require(fbe_world_set_ambient(scene.world, 0.5F, 0.5F, 0.5F));
    light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    const std::uint64_t source = even_light(4.0F, 4.0F, 4.0F);
    const std::uint64_t environment = fbe_environment_bake(source, 16, 8);
    REQUIRE(environment != 0);
    fbe_texture_release(source);
    require(fbe_world_set_environment(scene.world, environment, 1.0F));
    CHECK(near(middle(scene.drawn()), { srgb(0.5F), srgb(0.25F), srgb(0.125F) }));
    fbe_environment_free(environment);
    fbe_material_release(material);
}

TEST_CASE("a skinned arm matches its reference frames", "[gpu][scene][golden]") {
    const scene_t scene;
    light_for_shapes(scene);
    const shape_t shape = arm_shape();
    const std::uint64_t geometry = geometry_of(shape);
    set_arm_joints(geometry, shape);
    const arm_t arm(scene.world, geometry);
    float at = 0.0F;
    for (const auto& [name, step] : {
             std::pair{ "anim_arm_start", 0.0F },
             std::pair{ "anim_arm_half", 0.5F },
             std::pair{ "anim_arm_end", 0.5F },
         }) {
        CAPTURE(name);
        at += step;
        require(fbe_player_advance(scene.world, step));
        const fbe::golden::frame_t frame = scene.drawn();
        fbe::golden::check(name, frame);
        check_arm_where(frame, -45.0F * at, 90.0F * at, 2.0F);
        check_same_picture(
            drawn_baked(scene, skinned(shape, arm.mesh, geometry), arm.mesh, arm.root), frame);
    }
    fbe_geometry_release(geometry);
}

TEST_CASE("a morphing ball matches its reference frames", "[gpu][scene][golden]") {
    const scene_t scene;
    light_for_shapes(scene);
    const shape_t shape = ball_shape();
    const std::uint64_t geometry = geometry_of(shape);
    add_ball_targets(geometry, shape);
    const std::uint64_t place = fbe_entity_create(scene.world, 0, FBE_ENTITY_PIVOT);
    require(fbe_entity_set_position(place, 0.0F, 0.0F, -6.0F, FBE_SPACE_LOCAL));
    const std::uint64_t mesh = fbe_entity_create(scene.world, place, FBE_ENTITY_MESH);
    require(fbe_mesh_set_geometry(mesh, geometry));
    fbe_entity_set_name(mesh, "ball");
    const std::uint64_t clip = fbe_clip_create("breathe", 0.0F);
    const std::array<float, 2> times{ 0.0F, 1.0F };
    const std::array<float, 4> keys{ 0.0F, 0.0F, 1.0F, 1.0F };
    require(fbe_clip_add_channel(clip, "ball", FBE_CLIP_WEIGHTS, FBE_CLIP_LINEAR, 2, times.data(),
                                 keys.data(), 2));
    const std::uint64_t player = fbe_player_create(mesh);
    require(fbe_player_play(player, 0, clip, FBE_PLAYER_ONCE, 1.0F));
    float at = 0.0F;
    for (const auto& [name, step] : {
             std::pair{ "anim_ball_start", 0.0F },
             std::pair{ "anim_ball_half", 0.5F },
             std::pair{ "anim_ball_end", 0.5F },
         }) {
        CAPTURE(name);
        at += step;
        require(fbe_player_advance(scene.world, step));
        const fbe::golden::frame_t frame = scene.drawn();
        fbe::golden::check(name, frame);
        check_ball_where(frame, 0.8F * at, 0.6F * at);
        check_same_picture(drawn_baked(scene, morphed(shape, mesh), mesh, place), frame);
    }
    fbe_player_free(player);
    fbe_clip_release(clip);
    fbe_geometry_release(geometry);
}

TEST_CASE("an arm both morphed and skinned is morphed first", "[gpu][scene][golden]") {
    const scene_t scene;
    light_for_shapes(scene);
    const shape_t shape = arm_shape();
    const std::uint64_t geometry = geometry_of(shape);
    set_arm_joints(geometry, shape);
    // Its forearm grows along X, which a bent elbow takes round with it.
    std::vector<float> longer;
    std::vector<float> leaning;
    for (const fbe_vertex_t& vertex : shape.vertices) {
        longer.insert(longer.end(), { std::max(vertex.position.x, 0.0F) * 0.5F, 0.0F, 0.0F });
        leaning.insert(leaning.end(), { std::max(vertex.position.x, 0.0F) * 0.3F, 0.0F, 0.0F });
    }
    require(fbe_morph_add_target(geometry, 0, longer.data(), leaning.data(),
                                 static_cast<std::int32_t>(shape.vertices.size())) == 0);
    const arm_t arm(scene.world, geometry);
    const std::array<float, 1> weights{ 1.0F };
    require(fbe_morph_set_weights(arm.mesh, weights.data(), 1));
    require(fbe_player_advance(scene.world, 1.0F));
    const fbe::golden::frame_t frame = scene.drawn();
    fbe::golden::check("anim_both", frame);
    check_arm_where(frame, -45.0F, 90.0F, 3.0F);
    const shape_t baked = skinned(morphed(shape, arm.mesh), arm.mesh, geometry);
    check_same_picture(drawn_baked(scene, baked, arm.mesh, arm.root), frame);
    fbe_geometry_release(geometry);
}

namespace {

/// The 4 corners of a wall each move up by `up` in a new target of its surface.
void add_lift(std::uint64_t geometry, float up) {
    const std::array<float, 12> lifted{ 0, up, 0, 0, up, 0, 0, up, 0, 0, up, 0 };
    require(fbe_morph_add_target(geometry, 0, lifted.data(), nullptr, 4) >= 0);
}

} // namespace

TEST_CASE("a surface is drawn with the 8 targets weighted most", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t mesh = wall(scene.world, -3.0F, 0.2F, red);
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    // Eight that hardly move it, and a ninth that takes it out of the middle.
    for (int i = 0; i < 8; ++i) {
        add_lift(geometry, 0.001F);
    }
    add_lift(geometry, 1.0F);
    // The ninth alone moves it.
    std::array<float, 9> weights{ 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    require(fbe_morph_set_weights(mesh, weights.data(), 9));
    CHECK(middle(scene.drawn()) == rgba_t{ 0, 0, 255, 255 });
    // With eight stronger, it's left out; with one of them weaker, it's in.
    weights = { 2, 2, 2, 2, 2, 2, 2, 2, 1 };
    require(fbe_morph_set_weights(mesh, weights.data(), 9));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    weights = { 2, 2, 2, 2, 2, 2, 2, 0.5F, 1 };
    require(fbe_morph_set_weights(mesh, weights.data(), 9));
    CHECK(middle(scene.drawn()) == rgba_t{ 0, 0, 255, 255 });
    // How strong doesn't care which way it goes.
    weights = { 2, 2, 2, 2, 2, 2, 2, -2, -3 };
    require(fbe_morph_set_weights(mesh, weights.data(), 9));
    CHECK(middle(scene.drawn()) == rgba_t{ 0, 0, 255, 255 });
    // As strong as eight before it, it's the one left out.
    weights = { 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    require(fbe_morph_set_weights(mesh, weights.data(), 9));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    // What reads its vertices has them all.
    std::array<float, 12> positions{};
    require(fbe_morph_vertices(mesh, 0, positions.data(), nullptr, 4) == 4);
    CHECK(positions[1] == Catch::Approx(-0.2F + 1.0F + 0.008F));
    fbe_material_release(red);
}

TEST_CASE("a mesh's weights are its own as it's drawn, tweened from its capture") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t first = wall(scene.world, -3.0F, 0.2F, red);
    const std::uint64_t geometry = fbe_mesh_geometry(first);
    add_lift(geometry, 2.0F);
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    // A second of the geometry, lifted, leaves the first where it is; the first lifted leaves both
    // out of the middle.
    const std::uint64_t second = fbe_entity_copy(first, 0, 0);
    const std::array<float, 1> one{ 1.0F };
    require(fbe_morph_set_weights(second, one.data(), 1));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    require(fbe_morph_set_weights(first, one.data(), 1));
    CHECK(middle(scene.drawn()) == rgba_t{ 0, 0, 255, 255 });
    // Past its surface's targets, a weight's nothing.
    const std::array<float, 2> past{ 0.0F, 1.0F };
    require(fbe_morph_set_weights(first, past.data(), 2));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    // And it leaves room for the ones it has, however strong it is, a third of the way up its view.
    const std::array<float, 10> crowded{ 0.5F, 5, 5, 5, 5, 5, 5, 5, 5, 5 };
    require(fbe_morph_set_weights(first, crowded.data(), 10));
    const fbe::golden::frame_t crowded_out = scene.drawn();
    CHECK(middle(crowded_out) == rgba_t{ 0, 0, 255, 255 });
    CHECK(pixel(crowded_out, frame_size / 2, 27) == rgba_t{ 255, 0, 0, 255 });
    // Half way from where it was captured, lifted the whole way to 2, it's lifted 1: gone from the
    // middle, and with only 0.05 of that, in it still.
    const std::array<float, 1> none{ 0.0F };
    require(fbe_morph_set_weights(first, none.data(), 1));
    require(fbe_entity_set_hidden(second, true));
    require(fbe_world_capture(scene.world));
    require(fbe_morph_set_weights(first, one.data(), 1));
    CHECK(middle(scene.drawn(0.5F)) == rgba_t{ 0, 0, 255, 255 });
    CHECK(middle(scene.drawn(0.05F)) == rgba_t{ 255, 0, 0, 255 });
    fbe_material_release(red);
}

TEST_CASE("a morph that takes a mesh into view has it drawn") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    // Far above the view, and brought down into its middle.
    const std::uint64_t mesh = wall(scene.world, -3.0F, 0.2F, red);
    require(fbe_entity_set_position(mesh, 0.0F, 50.0F, -3.0F, FBE_SPACE_WORLD));
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    add_lift(geometry, -50.0F);
    CHECK(middle(scene.drawn()) == rgba_t{ 0, 0, 255, 255 });
    const std::array<float, 1> one{ 1.0F };
    require(fbe_morph_set_weights(mesh, one.data(), 1));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    // The other way, by a weight below 0, from below.
    const std::array<float, 1> back{ -1.0F };
    require(fbe_morph_set_weights(mesh, back.data(), 1));
    require(fbe_entity_set_position(mesh, 0.0F, -50.0F, -3.0F, FBE_SPACE_WORLD));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    // Between steps, half of a capture's way: its tween's what's culled by.
    const std::array<float, 1> none{ 0.0F };
    require(fbe_morph_set_weights(mesh, none.data(), 1));
    require(fbe_entity_set_position(mesh, 0.0F, 25.0F, -3.0F, FBE_SPACE_WORLD));
    require(fbe_world_capture(scene.world));
    require(fbe_morph_set_weights(mesh, one.data(), 1));
    CHECK(middle(scene.drawn(0.5F)) == rgba_t{ 255, 0, 0, 255 });
    // Going the other way, its weight falling: where it's drawn between them, not where it ends.
    require(fbe_world_capture(scene.world));
    require(fbe_morph_set_weights(mesh, none.data(), 1));
    CHECK(middle(scene.drawn(0.5F)) == rgba_t{ 255, 0, 0, 255 });
    // Its geometry scaled, its target scales, and what it can reach with it.
    require(fbe_morph_set_weights(mesh, one.data(), 1));
    const std::array<float, 16> twice{ 2.0F, 0, 0, 0, 0, 2.0F, 0, 0, 0, 0, 2.0F, 0, 0, 0, 0, 1 };
    fbe_mat4_t scaled{};
    std::ranges::copy(twice, std::begin(scaled.m));
    require(fbe_geometry_transform(geometry, &scaled));
    require(fbe_entity_set_position(mesh, 0.0F, 100.0F, -3.0F, FBE_SPACE_WORLD));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    fbe_material_release(red);
}

TEST_CASE("targets go to the GPU as they change, for only the vertices they had") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t mesh = wall(scene.world, -3.0F, 0.2F, red);
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    const std::array<float, 1> one{ 1.0F };
    require(fbe_morph_set_weights(mesh, one.data(), 1));
    // No target yet, and one added after it's been drawn.
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    add_lift(geometry, 2.0F);
    CHECK(middle(scene.drawn()) == rgba_t{ 0, 0, 255, 255 });
    // Vertices added since, in the middle, stay where they are while the first four go.
    std::array<fbe_vertex_t, 4> more{};
    fbe_surface_vertices(geometry, 0, 0, more.data(), 4);
    fbe_surface_add_vertices(geometry, 0, more.data(), 4);
    const std::array<std::uint32_t, 6> triangles{ 4, 5, 6, 4, 6, 7 };
    fbe_surface_add_triangles(geometry, 0, triangles.data(), 6);
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    fbe_material_release(red);
}

TEST_CASE("a surface with no targets beside one with them is drawn as it is") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t mesh = wall(scene.world, -3.0F, 0.2F, red);
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    // Its first surface lifted out of the middle by its target, and a second, in the middle.
    add_lift(geometry, 1.0F);
    const std::int32_t second = fbe_geometry_add_surface(geometry);
    std::array<fbe_vertex_t, 4> same{};
    fbe_surface_vertices(geometry, 0, 0, same.data(), 4);
    fbe_surface_add_vertices(geometry, second, same.data(), 4);
    const std::array<std::uint32_t, 6> triangles{ 0, 1, 2, 0, 2, 3 };
    fbe_surface_add_triangles(geometry, second, triangles.data(), 6);
    const std::array<float, 1> one{ 1.0F };
    require(fbe_morph_set_weights(mesh, one.data(), 1));
    const fbe::golden::frame_t both = scene.drawn();
    CHECK(middle(both) == rgba_t{ 255, 0, 0, 255 });
    // Lifted by 1 at a distance of 3, in a view 3.46 high, 0.58 of the way up its half.
    CHECK(pixel(both, frame_size / 2, 27) == rgba_t{ 255, 0, 0, 255 });
    fbe_material_release(red);
}

TEST_CASE(
    "a target that cancels a normal lights the vertex's own, as reading its vertices has it") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    light(scene.world, FBE_LIGHT_POINT, 0.0F, 0.0F, 0.0F, 30.0F);
    const std::uint64_t mesh = wall(scene.world, -3.0F, 1.0F, 0);
    const rgba_t lit = middle(scene.drawn());
    CHECK(lit[0] > 40);
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    const std::array<float, 12> cancels{ 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1 };
    require(fbe_morph_add_target(geometry, 0, nullptr, cancels.data(), 4) == 0);
    const std::array<float, 1> one{ 1.0F };
    require(fbe_morph_set_weights(mesh, one.data(), 1));
    CHECK(near(middle(scene.drawn()), { lit[0], lit[1], lit[2] }));
    // Half cancelled, and turned by another, it leans as the numbers say it does.
    const std::array<float, 12> turned{ 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0 };
    require(fbe_morph_add_target(geometry, 0, nullptr, turned.data(), 4) == 1);
    const std::array<float, 2> both{ 0.0F, 100.0F };
    require(fbe_morph_set_weights(mesh, both.data(), 2));
    // Nearly along Y, it hardly faces a light in front of it.
    CHECK(middle(scene.drawn())[0] < lit[0] / 4);
}

TEST_CASE("morphed normals of different lengths light as they are back at length 1") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    light(scene.world, FBE_LIGHT_POINT, 0.0F, 0.0F, 0.0F, 30.0F);
    const std::uint64_t mesh = wall(scene.world, -3.0F, 1.0F, 0);
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    // The left two corners' normals lean hard, so they're far longer than the right two's.
    std::array<fbe_vertex_t, 4> vertices{};
    require(fbe_surface_vertices(geometry, 0, 0, vertices.data(), 4));
    std::array<float, 12> leaning{};
    for (std::size_t i = 0; i < 4; ++i) {
        if (vertices.at(i).position.x < 0.0F) {
            leaning.at(i * 3) = -10.0F;
        }
    }
    require(fbe_morph_add_target(geometry, 0, nullptr, leaning.data(), 4) == 0);
    const std::array<float, 1> one{ 1.0F };
    require(fbe_morph_set_weights(mesh, one.data(), 1));
    const fbe::golden::frame_t frame = scene.drawn();
    // The same, by what reading its vertices has, as plain vertices.
    std::array<float, 12> normals{};
    require(fbe_morph_vertices(mesh, 0, nullptr, normals.data(), 4) == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        const std::span<const float> normal = std::span(normals).subspan(i * 3, 3);
        vertices.at(i).normal = { .x = normal[0], .y = normal[1], .z = normal[2] };
    }
    require(fbe_surface_set_vertices(geometry, 0, 0, vertices.data(), 4));
    const std::array<float, 1> none{ 0.0F };
    require(fbe_morph_set_weights(mesh, none.data(), 1));
    check_same_picture(scene.drawn(), frame);
}

TEST_CASE("a morphed normal too long for a float lights the vertex's own, as reading it has it") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    light(scene.world, FBE_LIGHT_POINT, 0.0F, 0.0F, 0.0F, 30.0F);
    const std::uint64_t mesh = wall(scene.world, -3.0F, 1.0F, 0);
    const rgba_t lit = middle(scene.drawn());
    CHECK(lit[0] > 40);
    const std::array<float, 12> up{ 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0 };
    require(fbe_morph_add_target(fbe_mesh_geometry(mesh), 0, nullptr, up.data(), 4) == 0);
    const std::array<float, 1> huge{ 1e20F };
    require(fbe_morph_set_weights(mesh, huge.data(), 1));
    std::array<float, 12> normals{};
    require(fbe_morph_vertices(mesh, 0, nullptr, normals.data(), 4) == 4);
    CHECK(normals[2] == 1.0F);
    CHECK(near(middle(scene.drawn()), { lit[0], lit[1], lit[2] }));
}

TEST_CASE("an edit between cameras waits for the next frame, the first having sent nothing") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t second_target =
        fbe_texture_create_target(frame_size, frame_size, FBE_TEXTURE_RGBA8, 1, true);
    const std::uint64_t second = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
    require(fbe_camera_set_target(second, second_target));
    require(fbe_camera_set_clear_color(second, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t mesh = wall(scene.world, -3.0F, 0.1F, red);
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    // Sent in a frame before, so this frame's first camera sends nothing.
    require(fbe_render_camera(scene.camera, 1.0F));
    fbe_frame_flip(false);
    const auto both_saw_it = [&] {
        CHECK(middle(scene_t::read(scene.target)) == rgba_t{ 255, 0, 0, 255 });
        CHECK(middle(scene_t::read(second_target)) == rgba_t{ 255, 0, 0, 255 });
    };
    // Its vertices moved aside between the two.
    require(fbe_render_camera(scene.camera, 1.0F));
    std::array<fbe_vertex_t, 4> vertices{};
    require(fbe_surface_vertices(geometry, 0, 0, vertices.data(), 4));
    for (fbe_vertex_t& vertex : vertices) {
        vertex.position.x += 1.0F;
    }
    require(fbe_surface_set_vertices(geometry, 0, 0, vertices.data(), 4));
    require(fbe_render_camera(second, 1.0F));
    both_saw_it();
    fbe_frame_flip(false);
    require(fbe_render_camera(scene.camera, 1.0F));
    CHECK(middle(scene_t::read(scene.target)) == rgba_t{ 0, 0, 255, 255 });
    fbe_frame_flip(false);
    // And a target moved between them, the same way.
    for (fbe_vertex_t& vertex : vertices) {
        vertex.position.x -= 1.0F;
    }
    require(fbe_surface_set_vertices(geometry, 0, 0, vertices.data(), 4));
    add_lift(geometry, 0.01F);
    const std::array<float, 1> one{ 1.0F };
    require(fbe_morph_set_weights(mesh, one.data(), 1));
    require(fbe_render_camera(scene.camera, 1.0F));
    fbe_frame_flip(false);
    require(fbe_render_camera(scene.camera, 1.0F));
    const std::array<float, 16> aside{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1 };
    fbe_mat4_t matrix{};
    std::ranges::copy(aside, std::begin(matrix.m));
    require(fbe_geometry_transform(geometry, &matrix));
    require(fbe_render_camera(second, 1.0F));
    both_saw_it();
    fbe_frame_flip(false);
    fbe_entity_free(second);
    std::array<std::uint64_t, 16> some{};
    while (fbe_entity_freed(some.data(), some.size()) != 0) {}
    fbe_texture_release(second_target);
    fbe_material_release(red);
}

TEST_CASE("targets that move nothing leave room for those that do") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t mesh = wall(scene.world, -3.0F, 0.2F, red);
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    // As a glTF body's surface has them: eight of the face's, empty here, one all 0, and one its
    // own.
    for (int i = 0; i < 7; ++i) {
        require(fbe_morph_add_target(geometry, 0, nullptr, nullptr, 4) >= 0);
    }
    add_lift(geometry, 0.0F);
    add_lift(geometry, 1.0F);
    const std::array<float, 9> weights{ 2, 2, 2, 2, 2, 2, 2, 2, 1 };
    require(fbe_morph_set_weights(mesh, weights.data(), 9));
    CHECK(middle(scene.drawn()) == rgba_t{ 0, 0, 255, 255 });
    fbe_material_release(red);
}

TEST_CASE("an entity's in view as far as its weights take it, as it's drawn") {
    const scene_t scene;
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t mesh = wall(scene.world, -3.0F, 0.2F, red);
    require(fbe_entity_set_position(mesh, 0.0F, 50.0F, -3.0F, FBE_SPACE_WORLD));
    add_lift(fbe_mesh_geometry(mesh), -50.0F);
    CHECK(fbe_camera_entity_in_view(scene.camera, frame_size, frame_size, mesh) == 0);
    const std::array<float, 1> one{ 1.0F };
    require(fbe_morph_set_weights(mesh, one.data(), 1));
    CHECK(fbe_camera_entity_in_view(scene.camera, frame_size, frame_size, mesh) == 1);
    fbe_material_release(red);
}

TEST_CASE("culling sums targets, adds surfaces, goes every way, and takes skinned morphs along") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    // Two targets of 30 each bring it down from 60.
    const std::uint64_t mesh = wall(scene.world, -3.0F, 0.2F, red);
    require(fbe_entity_set_position(mesh, 0.0F, 60.0F, -3.0F, FBE_SPACE_WORLD));
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    add_lift(geometry, -30.0F);
    add_lift(geometry, -30.0F);
    const std::array<float, 2> both{ 1.0F, 1.0F };
    require(fbe_morph_set_weights(mesh, both.data(), 2));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    require(fbe_entity_set_hidden(mesh, true));
    // In X, by a weight below 0, from the side.
    const std::uint64_t side = wall(scene.world, -3.0F, 0.2F, red);
    require(fbe_entity_set_position(side, 50.0F, 0.0F, -3.0F, FBE_SPACE_WORLD));
    const std::array<float, 12> over{ 50, 0, 0, 50, 0, 0, 50, 0, 0, 50, 0, 0 };
    require(fbe_morph_add_target(fbe_mesh_geometry(side), 0, over.data(), nullptr, 4) == 0);
    const std::array<float, 1> back{ -1.0F };
    require(fbe_morph_set_weights(side, back.data(), 1));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    require(fbe_entity_set_hidden(side, true));
    // In Z, by a weight below 0, from past the camera's far: a target that goes away, taken back.
    require(fbe_camera_set_range(scene.camera, 0.1F, 100.0F));
    const std::uint64_t far_off = wall(scene.world, -200.0F, 0.2F, red);
    const std::array<float, 12> away{ 0, 0, -197, 0, 0, -197, 0, 0, -197, 0, 0, -197 };
    require(fbe_morph_add_target(fbe_mesh_geometry(far_off), 0, away.data(), nullptr, 4) == 0);
    require(fbe_morph_set_weights(far_off, back.data(), 1));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    require(fbe_entity_set_hidden(far_off, true));
    // Its second surface's target, its first having none.
    const std::uint64_t two = wall(scene.world, -3.0F, 0.2F, red);
    require(fbe_entity_set_position(two, 0.0F, 50.0F, -3.0F, FBE_SPACE_WORLD));
    const std::uint64_t two_geometry = fbe_mesh_geometry(two);
    const std::int32_t second = fbe_geometry_add_surface(two_geometry);
    std::array<fbe_vertex_t, 4> same{};
    require(fbe_surface_vertices(two_geometry, 0, 0, same.data(), 4));
    fbe_surface_add_vertices(two_geometry, second, same.data(), 4);
    const std::array<std::uint32_t, 6> triangles{ 0, 1, 2, 0, 2, 3 };
    fbe_surface_add_triangles(two_geometry, second, triangles.data(), 6);
    const std::array<float, 12> down{ 0, -50, 0, 0, -50, 0, 0, -50, 0, 0, -50, 0 };
    require(fbe_morph_add_target(two_geometry, second, down.data(), nullptr, 4) == 0);
    const std::array<float, 1> one{ 1.0F };
    require(fbe_morph_set_weights(two, one.data(), 1));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    require(fbe_entity_set_hidden(two, true));
    // Skinned and morphed: its joints far above, its morph bringing it down.
    const skinned_wall_t skinned = skinned_wall(scene.world, red, -3.0F, 0.2F);
    require(fbe_entity_set_position(skinned.still, 0.0F, 50.0F, -3.0F, FBE_SPACE_WORLD));
    require(fbe_entity_set_position(skinned.moving, 0.0F, 50.0F, -3.0F, FBE_SPACE_WORLD));
    all_on(skinned.geometry, 0, 1.0F);
    add_lift(skinned.geometry, -50.0F);
    require(fbe_morph_set_weights(skinned.mesh, one.data(), 1));
    CHECK(middle(scene.drawn()) == rgba_t{ 255, 0, 0, 255 });
    fbe_skin_release(skinned.skin);
    fbe_material_release(red);
}

TEST_CASE("what casts goes as far as its morph, as what's drawn does", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -10.0F, 30.0F, paint);
    const auto shadowed = [&](float tween = 1.0F) { return middle(scene.drawn(tween))[0] == 0; };
    // Every one of these ends at 5, 0, -5, on a light's way to the wall's middle, and starts far
    // from it.
    const auto caster = [&](float x, float y, float z) {
        const std::uint64_t quad = wall(scene.world, -5.0F, 3.0F, paint);
        require(fbe_entity_set_position(quad, x, y, z, FBE_SPACE_WORLD));
        return quad;
    };
    const std::array<float, 1> one{ 1.0F };
    const std::array<float, 1> back{ -1.0F };
    SECTION("a sun's") {
        sun_from_right(scene.world);
        CHECK_FALSE(shadowed());
        // Two targets, each its own, bring it down from 60.
        const std::uint64_t summed = caster(5.0F, 60.0F, -5.0F);
        add_lift(fbe_mesh_geometry(summed), -20.0F);
        add_lift(fbe_mesh_geometry(summed), -40.0F);
        CHECK_FALSE(shadowed());
        const std::array<float, 2> both{ 1.0F, 1.0F };
        require(fbe_morph_set_weights(summed, both.data(), 2));
        CHECK(shadowed());
        require(fbe_entity_set_hidden(summed, true));
        // In X, by a weight below 0.
        const std::uint64_t side = caster(55.0F, 0.0F, -5.0F);
        const std::array<float, 12> over{ 50, 0, 0, 50, 0, 0, 50, 0, 0, 50, 0, 0 };
        require(fbe_morph_add_target(fbe_mesh_geometry(side), 0, over.data(), nullptr, 4) == 0);
        CHECK_FALSE(shadowed());
        require(fbe_morph_set_weights(side, back.data(), 1));
        CHECK(shadowed());
        require(fbe_entity_set_hidden(side, true));
        // In Z, the same, from behind the wall.
        const std::uint64_t far_off = caster(5.0F, 0.0F, -105.0F);
        const std::array<float, 12> away{ 0, 0, -100, 0, 0, -100, 0, 0, -100, 0, 0, -100 };
        require(fbe_morph_add_target(fbe_mesh_geometry(far_off), 0, away.data(), nullptr, 4) == 0);
        CHECK_FALSE(shadowed());
        require(fbe_morph_set_weights(far_off, back.data(), 1));
        CHECK(shadowed());
        require(fbe_entity_set_hidden(far_off, true));
        // Its second surface's target, its first having none.
        const std::uint64_t two = caster(5.0F, 50.0F, -5.0F);
        const std::uint64_t two_geometry = fbe_mesh_geometry(two);
        const std::int32_t second = fbe_geometry_add_surface(two_geometry);
        std::array<fbe_vertex_t, 4> same{};
        require(fbe_surface_vertices(two_geometry, 0, 0, same.data(), 4));
        fbe_surface_add_vertices(two_geometry, second, same.data(), 4);
        const std::array<std::uint32_t, 6> triangles{ 0, 1, 2, 0, 2, 3 };
        fbe_surface_add_triangles(two_geometry, second, triangles.data(), 6);
        const std::array<float, 12> down{ 0, -50, 0, 0, -50, 0, 0, -50, 0, 0, -50, 0 };
        require(fbe_morph_add_target(two_geometry, second, down.data(), nullptr, 4) == 0);
        CHECK_FALSE(shadowed());
        require(fbe_morph_set_weights(two, one.data(), 1));
        CHECK(shadowed());
        require(fbe_entity_set_hidden(two, true));
        // The 8 weighted most, not a 9th that'd take it far above, though culling allows for it.
        const std::uint64_t many = caster(5.0F, 40.0F, -5.0F);
        for (int i = 0; i < 8; ++i) {
            add_lift(fbe_mesh_geometry(many), -5.0F);
        }
        add_lift(fbe_mesh_geometry(many), 1000.0F);
        const std::array<float, 9> weights{ 1, 1, 1, 1, 1, 1, 1, 1, 0.5F };
        require(fbe_morph_set_weights(many, weights.data(), 9));
        CHECK(shadowed());
        require(fbe_entity_set_hidden(many, true));
        // Between steps, half of a capture's way: lifted the whole way 60 down, it's 30 down here,
        // and at the step's end past its place.
        const std::uint64_t tweened = caster(5.0F, 30.0F, -5.0F);
        add_lift(fbe_mesh_geometry(tweened), -30.0F);
        require(fbe_world_capture(scene.world));
        const std::array<float, 1> two_of{ 2.0F };
        require(fbe_morph_set_weights(tweened, two_of.data(), 1));
        CHECK(shadowed(0.5F));
        CHECK_FALSE(shadowed());
        require(fbe_entity_set_hidden(tweened, true));
        // Skinned and morphed: its joints far above, its morph bringing it down.
        const skinned_wall_t skinned = skinned_wall(scene.world, paint, -5.0F, 3.0F);
        require(fbe_entity_set_position(skinned.still, 5.0F, 50.0F, -5.0F, FBE_SPACE_WORLD));
        require(fbe_entity_set_position(skinned.moving, 5.0F, 50.0F, -5.0F, FBE_SPACE_WORLD));
        all_on(skinned.geometry, 0, 1.0F);
        add_lift(skinned.geometry, -50.0F);
        CHECK_FALSE(shadowed());
        require(fbe_morph_set_weights(skinned.mesh, one.data(), 1));
        CHECK(shadowed());
        fbe_skin_release(skinned.skin);
    }
    SECTION("a lamp's") {
        const std::uint64_t lamp = light(scene.world, FBE_LIGHT_POINT, 10.0F, 0.0F, 0.0F, 25.0F);
        require(fbe_light_set_intensity(lamp, 30.0F));
        require(fbe_light_set_shadows(lamp, true));
        const std::uint64_t quad = caster(5.0F, 60.0F, -5.0F);
        add_lift(fbe_mesh_geometry(quad), -60.0F);
        CHECK_FALSE(shadowed());
        require(fbe_morph_set_weights(quad, one.data(), 1));
        CHECK(shadowed());
    }
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(paint);
}

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

/// A shader of gpu/shaders/own/fbe_game_shader.slang, which a material asks things of by its
/// parameters: what it draws by the x of its first, how far it moves by its second, and what it
/// draws unless it's asked for something else by its third.
std::uint64_t game_shader() {
    const std::vector<std::uint8_t> bytes = cooked_shader(fbe::test_shaders::game_shader.code);
    return fbe_shader_create(bytes.data(), static_cast<std::int64_t>(bytes.size()), "game_shader");
}

constexpr float shader_draws_its_normal = 1.0F;
constexpr float shader_draws_past_its_parameters = 2.0F;
constexpr float shader_draws_past_its_layers = 3.0F;
constexpr float shader_draws_its_light = 4.0F;

/// A material drawing with game_shader, in an opaque red unless it's told otherwise.
std::uint64_t by_game_shader(float which = 0.0F) {
    const std::uint64_t material = fbe_material_create();
    const std::uint64_t shader = game_shader();
    require(shader != 0);
    require(fbe_material_set_shader(material, shader));
    require(fbe_shader_release(shader));
    require(fbe_material_set_parameter(material, 0, which, 0.0F, 0.0F, 0.0F));
    require(fbe_material_set_parameter(material, 2, 1.0F, 0.0F, 0.0F, 1.0F));
    return material;
}

/// A cull box a shader moving it this far stays in.
void room_to_move(std::uint64_t geometry) {
    const fbe_aabb_t room{
        .min = { .x = -60.0F, .y = -60.0F, .z = -60.0F },
        .max = { .x = 60.0F, .y = 60.0F, .z = 60.0F },
    };
    require(fbe_geometry_set_cull_box(geometry, &room));
}

/// Turned a quarter about Z, as a joint is below, so what's moved before it's skinned goes
/// elsewhere.
void quarter_turned(std::uint64_t entity) {
    const float half = std::numbers::pi_v<float> * 0.25F;
    require(fbe_entity_set_rotation(entity, 0.0F, 0.0F, std::sin(half), std::cos(half),
                                    FBE_SPACE_WORLD));
}

} // namespace

TEST_CASE("the same bytes of a shader are the same code, and a pipeline's key says which") {
    const std::uint64_t first = game_shader();
    const std::uint64_t second = game_shader();
    REQUIRE(first != 0);
    REQUIRE(second != 0);
    CHECK(first != second);
    CHECK(fbe::shaders_own::code_of(first) == fbe::shaders_own::code_of(second));
    fbe_shader_release(first);
    fbe_shader_release(second);
}

TEST_CASE("two materials with one shader read their own parameters", "[gpu][scene]") {
    const scene_t scene;
    SECTION("drawing") {
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
        const std::uint64_t reds = by_game_shader();
        const std::uint64_t greens = by_game_shader();
        require(fbe_material_set_parameter(greens, 2, 0.0F, 1.0F, 0.0F, 1.0F));
        const std::uint64_t left = wall(scene.world, -3.0F, 1.0F, reds);
        require(fbe_entity_set_position(left, -1.0F, 0.0F, -3.0F, FBE_SPACE_WORLD));
        const std::uint64_t right = wall(scene.world, -3.0F, 1.0F, greens);
        require(fbe_entity_set_position(right, 1.0F, 0.0F, -3.0F, FBE_SPACE_WORLD));
        const fbe::golden::frame_t frame = scene.drawn();
        CHECK(near(pixel(frame, 40, 64), { 255, 0, 0 }));
        CHECK(near(pixel(frame, 88, 64), { 0, 255, 0 }));
        fbe_material_release(reds);
        fbe_material_release(greens);
    }
    SECTION("casting") {
        const std::uint64_t paint = fbe_material_create();
        wall(scene.world, -10.0F, 30.0F, paint);
        sun_from_right(scene.world);
        // Far above the sun's way to the wall's middle, each until its own parameter brings it
        // down.
        const std::uint64_t stays = by_game_shader();
        const std::uint64_t comes = by_game_shader();
        const std::uint64_t first = wall(scene.world, -5.0F, 3.0F, stays);
        const std::uint64_t second = wall(scene.world, -5.0F, 3.0F, comes);
        for (const std::uint64_t mesh : { first, second }) {
            require(fbe_entity_set_position(mesh, 5.0F, 30.0F, -5.0F, FBE_SPACE_WORLD));
            room_to_move(fbe_mesh_geometry(mesh));
        }
        CHECK(middle(scene.drawn())[0] != 0);
        require(fbe_material_set_parameter(comes, 1, 0.0F, -30.0F, 0.0F, 0.0F));
        CHECK(middle(scene.drawn())[0] == 0);
        require(fbe_material_set_parameter(comes, 1, 0.0F, 0.0F, 0.0F, 0.0F));
        CHECK(middle(scene.drawn())[0] != 0);
        fbe_material_release(stays);
        fbe_material_release(comes);
        fbe_material_release(paint);
    }
    CHECK(fbe_gpu_validation_errors() == 0);
}

namespace {

/// A scene with a material of the game's shader, for a test whose parts are on one GPU.
struct shader_scene_t : scene_t {
    std::uint64_t material = by_game_shader();

    explicit shader_scene_t(bool stops) : scene_t(stops) {}
    shader_scene_t(const shader_scene_t&) = delete;
    shader_scene_t& operator=(const shader_scene_t&) = delete;
    shader_scene_t(shader_scene_t&&) = delete;
    shader_scene_t& operator=(shader_scene_t&&) = delete;

    ~shader_scene_t() {
        CHECK(fbe_gpu_validation_errors() == 0);
        fbe_material_release(material);
    }
};

} // namespace

TEST_CASE("a shader moves a vertex after its morph and its skin", "[gpu][scene]") {
    std::vector<part_t<shader_scene_t>> parts;
    parts.emplace_back("drawn, skinned", [](const shader_scene_t& scene) {
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
        // A joint to the left, turned: moved right after it, it's in the middle.
        const skinned_wall_t skinned = skinned_wall(scene.world, scene.material, -3.0F, 0.5F);
        all_on(skinned.geometry, 0, 1.0F);
        require(fbe_entity_set_position(skinned.still, -1.0F, 0.0F, -3.0F, FBE_SPACE_WORLD));
        quarter_turned(skinned.still);
        room_to_move(skinned.geometry);
        CHECK(near(middle(scene.drawn()), { 0, 0, 255 }));
        require(fbe_material_set_parameter(scene.material, 1, 1.0F, 0.0F, 0.0F, 0.0F));
        CHECK(near(middle(scene.drawn()), { 255, 0, 0 }));
        fbe_skin_release(skinned.skin);
    });
    parts.emplace_back("drawn, morphed", [](const shader_scene_t& scene) {
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
        const std::uint64_t mesh = wall(scene.world, -3.0F, 0.5F, scene.material);
        require(fbe_entity_set_position(mesh, 0.0F, 60.0F, -3.0F, FBE_SPACE_WORLD));
        add_lift(fbe_mesh_geometry(mesh), -30.0F);
        room_to_move(fbe_mesh_geometry(mesh));
        const std::array<float, 1> one{ 1.0F };
        require(fbe_morph_set_weights(mesh, one.data(), 1));
        CHECK(near(middle(scene.drawn()), { 0, 0, 255 }));
        require(fbe_material_set_parameter(scene.material, 1, 0.0F, -30.0F, 0.0F, 0.0F));
        CHECK(near(middle(scene.drawn()), { 255, 0, 0 }));
    });
    parts.emplace_back("casting, skinned", [](const shader_scene_t& scene) {
        const std::uint64_t paint = fbe_material_create();
        wall(scene.world, -10.0F, 30.0F, paint);
        sun_from_right(scene.world);
        const skinned_wall_t skinned = skinned_wall(scene.world, scene.material, -5.0F, 0.5F);
        all_on(skinned.geometry, 0, 1.0F);
        require(fbe_entity_set_position(skinned.still, 3.0F, 0.0F, -5.0F, FBE_SPACE_WORLD));
        quarter_turned(skinned.still);
        room_to_move(skinned.geometry);
        CHECK(middle(scene.drawn())[0] != 0);
        require(fbe_material_set_parameter(scene.material, 1, 2.0F, 0.0F, 0.0F, 0.0F));
        CHECK(middle(scene.drawn())[0] == 0);
        fbe_skin_release(skinned.skin);
        fbe_material_release(paint);
    });
    parts.emplace_back("casting, morphed", [](const shader_scene_t& scene) {
        const std::uint64_t paint = fbe_material_create();
        wall(scene.world, -10.0F, 30.0F, paint);
        sun_from_right(scene.world);
        const std::uint64_t mesh = wall(scene.world, -5.0F, 3.0F, scene.material);
        require(fbe_entity_set_position(mesh, 5.0F, 60.0F, -5.0F, FBE_SPACE_WORLD));
        add_lift(fbe_mesh_geometry(mesh), -30.0F);
        room_to_move(fbe_mesh_geometry(mesh));
        const std::array<float, 1> one{ 1.0F };
        require(fbe_morph_set_weights(mesh, one.data(), 1));
        CHECK(middle(scene.drawn())[0] != 0);
        require(fbe_material_set_parameter(scene.material, 1, 0.0F, -30.0F, 0.0F, 0.0F));
        CHECK(middle(scene.drawn())[0] == 0);
        fbe_material_release(paint);
    });
    parts.emplace_back("a terrain, drawn and casting", [](const shader_scene_t& scene) {
        const std::uint64_t paint = fbe_material_create();
        wall(scene.world, -10.0F, 30.0F, paint);
        sun_from_right(scene.world);
        // A slab stood on end, as a terrain that casts is above, on the sun's way till it's moved.
        const std::uint64_t terrain = fbe_entity_create(scene.world, 0, FBE_ENTITY_TERRAIN);
        require(fbe_terrain_set_size(terrain, 32));
        require(fbe_terrain_set_material(terrain, scene.material));
        require(fbe_entity_set_scale(terrain, 0.25F, 1.0F, 0.25F, FBE_SPACE_WORLD));
        const float half = std::numbers::pi_v<float> * 0.25F;
        require(fbe_entity_set_rotation(terrain, std::sin(half), 0.0F, 0.0F, std::cos(half),
                                        FBE_SPACE_WORLD));
        require(fbe_entity_set_position(terrain, 1.0F, 4.0F, -5.0F, FBE_SPACE_WORLD));
        CHECK(middle(scene.drawn())[0] == 0);
        // In its own space, whose x is the world's: off the sun's way.
        require(fbe_material_set_parameter(scene.material, 1, 16.0F, 0.0F, 0.0F, 0.0F));
        CHECK(middle(scene.drawn())[0] != 0);
        fbe_material_release(paint);
    });
    on_one_gpu(parts);
}

TEST_CASE("a two-sided material's back has its shader's normal facing the camera", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = by_game_shader(shader_draws_its_normal);
    require(fbe_material_set_two_sided(material, true));
    const std::uint64_t mesh = wall(scene.world, -3.0F, 1.0F, material);
    CHECK(middle(scene.drawn())[2] == 255);
    const float half = std::numbers::pi_v<float> * 0.5F;
    require(
        fbe_entity_set_rotation(mesh, 0.0F, std::sin(half), 0.0F, std::cos(half), FBE_SPACE_WORLD));
    CHECK(middle(scene.drawn())[2] == 255);
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(material);
}

TEST_CASE("a shader's alpha is what's cut, and its blend is the material's", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t material = by_game_shader();
    require(fbe_material_set_parameter(material, 2, 1.0F, 0.0F, 0.0F, 0.5F));
    wall(scene.world, -3.0F, 1.0F, material);
    // Whole by AUTO, which goes by the material's color and textures, not what's returned.
    CHECK(near(middle(scene.drawn()), { 255, 0, 0 }));
    require(fbe_material_set_blend(material, FBE_BLEND_ALPHA));
    CHECK(near(middle(scene.drawn()), { srgb(0.5F), 0, srgb(0.5F) }));
    // Opaque, what's below a cutoff is cut, and what's above it whole.
    require(fbe_material_set_blend(material, FBE_BLEND_OPAQUE));
    require(fbe_material_set_alpha_cutoff(material, 0.75F));
    CHECK(near(middle(scene.drawn()), { 0, 0, 255 }));
    require(fbe_material_set_alpha_cutoff(material, 0.25F));
    CHECK(near(middle(scene.drawn()), { 255, 0, 0 }));
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(material);
}

TEST_CASE("a parameter past the last is the last, and a layer it hasn't is white", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = by_game_shader(shader_draws_past_its_parameters);
    require(fbe_material_set_parameter(material, 7, 0.0F, 1.0F, 0.0F, 1.0F));
    wall(scene.world, -3.0F, 1.0F, material);
    CHECK(near(middle(scene.drawn()), { 0, 255, 0 }));
    require(
        fbe_material_set_parameter(material, 0, shader_draws_past_its_layers, 0.0F, 0.0F, 0.0F));
    // Layer 9's place is where the fifth parameter is, so reading it there would be this red
    // texture.
    const std::uint64_t red = fbe_texture_create(1, 1, FBE_TEXTURE_RGBA8, false);
    const std::array<std::uint8_t, 4> pixels{ 255, 0, 0, 255 };
    require(fbe_texture_write(red, 0, 0, 1, 1, pixels.data(), 4));
    (void)scene.drawn();
    const auto read_as = [](std::uint32_t bits) { return std::bit_cast<float>(bits); };
    require(fbe_material_set_parameter(material, 4, read_as(fbe::texture::index_of(red)),
                                       read_as(0), 0.0F, 0.0F));
    CHECK(near(middle(scene.drawn()), { 255, 255, 255 }));
    fbe_texture_release(red);
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(material);
}

TEST_CASE("the light a shader's told of is linear, whatever its material's shading",
          "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = by_game_shader(shader_draws_its_light);
    wall(scene.world, -3.0F, 3.0F, material);
    const bool sun = GENERATE(false, true);
    const std::uint64_t lamp =
        light(scene.world, sun ? FBE_LIGHT_DIRECTIONAL : FBE_LIGHT_POINT, 0.0F, 0.0F, -1.0F, 10.0F);
    require(fbe_light_set_intensity(lamp, sun ? 0.2F : 2.0F));
    require(fbe_material_set_shading(material, FBE_SHADING_PBR));
    const rgba_t expected = middle(scene.drawn());
    CHECK(expected[0] > 16);
    CHECK(expected[0] < 240);
    require(fbe_material_set_shading(material, FBE_SHADING_VERTEX));
    CHECK(near(middle(scene.drawn()), { expected[0], expected[1], expected[2] }));
    require(fbe_material_set_shading(material, FBE_SHADING_UNLIT));
    CHECK(near(middle(scene.drawn()), { expected[0], expected[1], expected[2] }));
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(material);
}

// A game's shader's SPIR-V mutated: words flipped, set, their counts set to none or past the end,
// and the end cut. Each is checked, and made where it's taken, reading nothing past what it has,
// which ASan checks where it runs. FBE_FUZZ_ROUNDS asks for more than CI's 2000.
namespace {

/// SPIRV-Tools' validator off while it lives, as FBE_SPIRV_VALIDATION=0 has it.
struct unvalidated_t {
    unvalidated_t() {
        fbe::tests::set_variable("FBE_SPIRV_VALIDATION", "0");
    }
    unvalidated_t(const unvalidated_t&) = delete;
    unvalidated_t& operator=(const unvalidated_t&) = delete;
    unvalidated_t(unvalidated_t&&) = delete;
    unvalidated_t& operator=(unvalidated_t&&) = delete;
    ~unvalidated_t() {
        fbe::tests::set_variable("FBE_SPIRV_VALIDATION", "1");
    }
};

} // namespace

// The checks before the validator, which it takes a Debug build a quarter of a second to run on
// each: so they're tried on their own here, as many times as there are rounds.
TEST_CASE("a game's shader's mutants are checked without reading past them",
          "[gpu][scene][stability]") {
    const unvalidated_t unvalidated;
    const int rounds = asked_for("FBE_FUZZ_ROUNDS", 2000);
    const std::span<const std::uint32_t> seed = fbe::test_shaders::game_shader.code;
    REQUIRE(seed.size() > 5);
    // Seeds that repeat, so what a run finds is found again.
    std::mt19937 random(20261106); // NOLINT(bugprone-random-generator-seed)
    const auto at = [&](std::size_t size) {
        return std::uniform_int_distribution<std::size_t>(0, size - 1)(random);
    };
    int taken = 0;
    for (int round = 0; round < rounds; ++round) {
        CAPTURE(round);
        std::vector<std::uint32_t> words(seed.begin(), seed.end());
        const int changes = std::uniform_int_distribution<int>(1, 3)(random);
        for (int change = 0; change < changes && words.size() > 5; ++change) {
            // Past the header's five words, which say it's SPIR-V.
            const std::size_t where = 5 + at(words.size() - 5);
            switch (at(5)) {
            case 0:
                words[where] ^= 1U << at(32);
                break;
            case 1:
                words[where] = static_cast<std::uint32_t>(random());
                break;
            case 2:
                // An instruction's count: none, or more words than there are.
                words[where] = (words[where] & 0xFFFFU) | (at(2) == 0 ? 0U : 0xFFFFU << 16U);
                break;
            case 3:
                words[where] = std::array<std::uint32_t, 4>{ 0, 1, 2, 0xFFFFFFFF }.at(at(4));
                break;
            default:
                words.resize(where);
                break;
            }
        }
        const std::vector<std::uint8_t> bytes = cooked_shader(words);
        if (fbe_shader_check(bytes.data(), static_cast<std::int64_t>(bytes.size()))) {
            ++taken;
        }
    }
    // Some are taken, and some refused, or it's not the check that's tried.
    CAPTURE(taken);
    CHECK(taken > 0);
    CHECK(taken < rounds);
}

// Mutants SPIRV-Tools' validator takes are drawn, as a game would draw them: some are refused,
// saying what the validator says, and what's taken draws, with no validation errors. The first
// three taken are drawn, as each makes pipelines, seconds each on lavapipe.
TEST_CASE("a game's shader's mutants the validator takes are drawn", "[gpu][scene][stability]") {
    const scene_t scene;
    const int rounds = asked_for("FBE_FUZZ_VALIDATED_ROUNDS", 40);
    const std::span<const std::uint32_t> seed = fbe::test_shaders::game_shader.code;
    // Seeds that repeat, so what a run finds is found again.
    std::mt19937 random(20261108); // NOLINT(bugprone-random-generator-seed)
    const auto at = [&](std::size_t size) {
        return std::uniform_int_distribution<std::size_t>(0, size - 1)(random);
    };
    // Where its constants' values are, OpConstant's last word, which changed is still valid SPIR-V,
    // if not the shader it was.
    constexpr std::uint32_t op_constant = 43;
    std::vector<std::size_t> values;
    for (std::size_t word = 5; word < seed.size();) {
        const std::uint32_t count = seed[word] >> 16U;
        if ((seed[word] & 0xFFFFU) == op_constant && count == 4) {
            values.push_back(word + 3);
        }
        word += std::max(count, 1U);
    }
    REQUIRE(!values.empty());
    int refused_by_validator = 0;
    int drawn = 0;
    for (int round = 0; round < rounds; ++round) {
        CAPTURE(round);
        std::vector<std::uint32_t> words(seed.begin(), seed.end());
        // A bit flipped past the header, which the validator mostly refuses, or in a constant's
        // value, which it mostly takes.
        const std::size_t where =
            round % 2 == 0 ? 5 + at(words.size() - 5) : values.at(at(values.size()));
        words[where] ^= 1U << at(32);
        const std::vector<std::uint8_t> bytes = cooked_shader(words);
        const std::uint64_t shader =
            fbe_shader_create(bytes.data(), static_cast<std::int64_t>(bytes.size()), "a mutant");
        if (shader == 0) {
            refused_by_validator +=
                std::string_view(fbe_error()).contains("SPIRV-Tools' validator says") ? 1 : 0;
            continue;
        }
        if (drawn < 3) {
            const std::uint64_t material = fbe_material_create();
            require(fbe_material_set_shader(material, shader));
            const std::uint64_t mesh = cube(scene.world, 0.0F, 0.0F, -6.0F, material);
            static_cast<void>(scene.drawn());
            require(fbe_entity_free(mesh));
            fbe_material_release(material);
            ++drawn;
        }
        require(fbe_shader_release(shader));
    }
    CAPTURE(refused_by_validator, drawn);
    CHECK(refused_by_validator > 0);
    CHECK(drawn > 0);
    CHECK(fbe_gpu_validation_errors() == 0);
}

// A pak's shader whose bytes are broken past what the checks before the validator see, its SPIR-V's
// bound on its ids less than its ids, is refused as it's made, saying what the validator says,
// before any driver's handed it.
TEST_CASE("a pak's shader that isn't valid SPIR-V is refused with the validator's message",
          "[gpu][scene]") {
    std::vector<std::uint32_t> words(fbe::test_shaders::game_shader.code.begin(),
                                     fbe::test_shaders::game_shader.code.end());
    words[3] = 2;
    const std::vector<std::uint8_t> bytes = cooked_shader(words);
    CHECK(fbe_shader_create(bytes.data(), static_cast<std::int64_t>(bytes.size()),
                            "res://broken.slang") == 0);
    const std::string_view why = fbe_error();
    CAPTURE(why);
    CHECK(why.starts_with("the shader isn't valid SPIR-V, as SPIRV-Tools' validator says: "));
    CHECK(why.size() > std::string_view("the shader isn't valid SPIR-V, as SPIRV-Tools' "
                                        "validator says: ")
                           .size());
    CHECK(!fbe_shader_check(bytes.data(), static_cast<std::int64_t>(bytes.size())));
}

TEST_CASE("a shader no pipeline can be made of draws as the scene's own, and isn't tried again",
          "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    // Bytes of its own, so it's a code of its own, by its name.
    std::vector<std::uint32_t> words(fbe::test_shaders::game_shader.code.begin(),
                                     fbe::test_shaders::game_shader.code.end());
    // Its generator's word, which nothing reads.
    words[2] = 0xFB01;
    const std::vector<std::uint8_t> bytes = cooked_shader(words);
    const std::uint64_t shader = fbe_shader_create(
        bytes.data(), static_cast<std::int64_t>(bytes.size()), "res://refused.slang");
    REQUIRE(shader != 0);
    fbe::gpu::refuse_pipelines_of("res://refused.slang");
    const std::uint64_t material = unlit(0.0F, 1.0F, 0.0F);
    require(fbe_material_set_shader(material, shader));
    require(fbe_material_set_parameter(material, 2, 1.0F, 0.0F, 0.0F, 1.0F));
    wall(scene.world, -3.0F, 1.0F, material);
    CHECK(near(middle(scene.drawn()), { 0, 255, 0 }));
    CHECK(std::string_view(fbe_error()).contains("res://refused.slang"));
    CHECK(fbe::gpu::pipelines_refused() == 1);
    CHECK(near(middle(scene.drawn()), { 0, 255, 0 }));
    CHECK(fbe::gpu::pipelines_refused() == 1);
    fbe::gpu::refuse_pipelines_of("");
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_shader_release(shader);
    fbe_material_release(material);
}

namespace {

/// A sky of a texture of `width` by `height` of bytes from a function of the column and row.
template <typename Color>
std::uint64_t sky_of(std::int32_t width, std::int32_t height, Color&& color) {
    const std::uint64_t texture = fbe_texture_create(width, height, FBE_TEXTURE_RGBA8, false);
    require(texture != 0);
    std::vector<std::uint8_t> pixels;
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            const rgba_t one = color(x, y);
            pixels.insert(pixels.end(), one.begin(), one.end());
        }
    }
    require(fbe_texture_write(texture, 0, 0, width, height, pixels.data(), width * 4));
    return texture;
}

/// A mirror facing +Y turned to face +Z, at a depth.
std::uint64_t wall_mirror(std::uint64_t world, float depth) {
    const std::uint64_t mirror = fbe_entity_create(world, 0, FBE_ENTITY_MIRROR);
    turn_about(mirror, 1.0F, 0.0F, 0.0F, std::numbers::pi_v<float> / 2.0F);
    require(fbe_entity_set_position(mirror, 0.0F, 0.0F, depth, FBE_SPACE_WORLD));
    return mirror;
}

std::uint64_t floor_mirror(std::uint64_t world, float height) {
    const std::uint64_t mirror = fbe_entity_create(world, 0, FBE_ENTITY_MIRROR);
    require(fbe_entity_set_position(mirror, 0.0F, height, 0.0F, FBE_SPACE_WORLD));
    return mirror;
}

/// The atmosphere's one scattering, worked out here in doubles, as the sky shader has it.
struct reference_sky_t {
    static constexpr double ground = 6360000.0;
    static constexpr double air = 6420000.0;
    static constexpr double rayleigh_height = 7994.0;
    static constexpr double mie_height = 1200.0;
    static constexpr double mie_scatter = 21e-6;
    static constexpr double mie_shape = 0.76;

    struct way_t {
        double x;
        double y;
        double z;
    };

    static double dot(way_t a, way_t b) {
        return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
    }

    static double leaving(way_t place, way_t way) {
        const double along = dot(place, way);
        const double left = (along * along) - dot(place, place) + (air * air);
        return left > 0.0 ? -along + std::sqrt(left) : 0.0;
    }

    static double to_ground(way_t place, way_t way) {
        const double along = dot(place, way);
        const double left = (along * along) - dot(place, place) + (ground * ground);
        return along < 0.0 && left > 0.0 ? -along - std::sqrt(left) : -1.0;
    }

    /// Red, green, or blue, by its Rayleigh scattering, seen along a way with the sun along
    /// another.
    static double seen(way_t way, way_t sun, double rayleigh_scatter,
                       const std::array<double, 3>& all_rayleigh, std::size_t channel,
                       double brightness) {
        const way_t eye{ .x = 0.0, .y = ground + 1000.0, .z = 0.0 };
        const double hit = to_ground(eye, way);
        const double travel = hit >= 0.0 ? hit : leaving(eye, way);
        const double length = travel / 8.0;
        double rayleigh = 0.0;
        double mie = 0.0;
        double rayleigh_depth = 0.0;
        double mie_depth = 0.0;
        for (int i = 0; i < 8; ++i) {
            const double t = (static_cast<double>(i) + 0.5) * length;
            const double at_x = eye.x + (way.x * t);
            const double at_y = eye.y + (way.y * t);
            const double at_z = eye.z + (way.z * t);
            const way_t place{ .x = at_x, .y = at_y, .z = at_z };
            const double height = std::sqrt(dot(place, place)) - ground;
            const double thin_r = std::exp(-std::max(height, 0.0) / rayleigh_height) * length;
            const double thin_m = std::exp(-std::max(height, 0.0) / mie_height) * length;
            rayleigh_depth += thin_r;
            mie_depth += thin_m;
            if (to_ground(place, sun) >= 0.0) {
                continue;
            }
            const double sun_length = leaving(place, sun) / 4.0;
            double sun_r = 0.0;
            double sun_m = 0.0;
            for (int j = 0; j < 4; ++j) {
                const double s = (static_cast<double>(j) + 0.5) * sun_length;
                const double up_x = place.x + (sun.x * s);
                const double up_y = place.y + (sun.y * s);
                const double up_z = place.z + (sun.z * s);
                const way_t up{ .x = up_x, .y = up_y, .z = up_z };
                const double above = std::sqrt(dot(up, up)) - ground;
                sun_r += std::exp(-std::max(above, 0.0) / rayleigh_height) * sun_length;
                sun_m += std::exp(-std::max(above, 0.0) / mie_height) * sun_length;
            }
            const double tau = (all_rayleigh[channel] * (rayleigh_depth + sun_r)) +
                               (mie_scatter * 1.1 * (mie_depth + sun_m));
            const double through = std::exp(-tau);
            rayleigh += through * thin_r;
            mie += through * thin_m;
        }
        const double cosine = dot(way, sun);
        const double pi = std::numbers::pi;
        const double phase_r = 3.0 / (16.0 * pi) * (1.0 + (cosine * cosine));
        const double g = mie_shape;
        const double phase_m =
            3.0 / (8.0 * pi) * ((1.0 - (g * g)) * (1.0 + (cosine * cosine))) /
            ((2.0 + (g * g)) * std::pow(std::max(1.0 + (g * g) - (2.0 * g * cosine), 0.0001), 1.5));
        return ((rayleigh * rayleigh_scatter * phase_r) + (mie * mie_scatter * phase_m)) *
               brightness;
    }

    /// What's stored for all three channels.
    static std::array<std::uint8_t, 3> stored_for(way_t way, way_t sun, double brightness) {
        const std::array<double, 3> all{ 5.8e-6, 13.5e-6, 33.1e-6 };
        std::array<std::uint8_t, 3> found{};
        for (std::size_t channel = 0; channel < 3; ++channel) {
            found[channel] =
                srgb(static_cast<float>(seen(way, sun, all[channel], all, channel, brightness)));
        }
        return found;
    }
};

} // namespace

TEST_CASE("each mirror's reflection has the sky only through its own plane", "[gpu][scene]") {
    const bool floor_first = GENERATE(true, false);
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t texture =
        sky_texture({ 255, 0, 0, 255 }, { 255, 0, 0, 255 }, { 0, 0, 255, 255 }, { 0, 0, 255, 255 });
    require(fbe_world_set_sky_texture(scene.world, texture, 1.0F));
    if (floor_first) {
        floor_mirror(scene.world, -1.0F);
        wall_mirror(scene.world, -10.0F);
    } else {
        wall_mirror(scene.world, -10.0F);
        floor_mirror(scene.world, -1.0F);
    }
    const fbe::golden::frame_t frame = scene.drawn();
    // Well down: the floor's first, and looks back up. Just down: the wall's first, whose
    // reflection goes on down. Up: the wall's, on up.
    CHECK(near(pixel(frame, 64, 112), { 255, 0, 0 }));
    CHECK(near(pixel(frame, 64, 70), { 0, 0, 255 }));
    CHECK(near(pixel(frame, 64, 16), { 255, 0, 0 }));
    fbe_texture_release(texture);
}

TEST_CASE("a sky's sun is where it's shading from, whatever's above it", "[gpu][scene]") {
    const auto seen_with = [](auto&& place_it) {
        const scene_t scene;
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
        const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
        place_it(scene, sun);
        require(fbe_world_set_sky_atmosphere(scene.world, sun, 20.0F));
        return scene.drawn();
    };
    const auto parented = [](float x, float y, float z, float pitch_by) {
        return [=](const scene_t& scene, std::uint64_t sun) {
            const std::uint64_t above = fbe_entity_create(scene.world, 0, FBE_ENTITY_PIVOT);
            require(fbe_entity_set_parent(sun, above, false));
            pitch(sun, pitch_by);
            require(fbe_entity_set_scale(above, x, y, z, FBE_SPACE_WORLD));
        };
    };
    const auto alone = [](float pitch_by) {
        return [=](const scene_t& /*scene*/, std::uint64_t sun) { pitch(sun, pitch_by); };
    };
    SECTION("scaled to nothing") {
        const fbe::golden::frame_t expected = seen_with(alone(0.4F));
        const fbe::golden::frame_t found = seen_with(parented(0.0F, 0.0F, 0.0F, 0.4F));
        CHECK(fbe::golden::compare(expected, found, 2).pixels == 0);
    }
    SECTION("scaled through itself") {
        const fbe::golden::frame_t expected = seen_with(alone(0.0F));
        const fbe::golden::frame_t found = seen_with(parented(1.0F, 1.0F, -1.0F, 0.0F));
        CHECK(fbe::golden::compare(expected, found, 2).pixels == 0);
    }
}

TEST_CASE("a sky's texture stops at its top and bottom, however it's set to wrap", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t texture = sky_of(4, 2, [](std::int32_t /*x*/, std::int32_t y) {
        return y == 0 ? rgba_t{ 255, 255, 255, 255 } : rgba_t{ 0, 0, 0, 255 };
    });
    require(fbe_texture_set_filter(texture, FBE_TEXTURE_LINEAR));
    require(fbe_texture_set_wrap(texture, FBE_TEXTURE_REPEAT, FBE_TEXTURE_REPEAT));
    require(fbe_world_set_sky_texture(scene.world, texture, 1.0F));
    pitch(scene.camera, std::numbers::pi_v<float> / 2.0F);
    CHECK(near(middle(scene.drawn()), { 255, 255, 255 }));
    fbe_texture_release(texture);
}

TEST_CASE("a sky stays as it was when its sun's freed", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    pitch(sun, std::numbers::pi_v<float> + 0.1F);
    require(fbe_world_set_sky_atmosphere(scene.world, sun, 20.0F));
    pitch(scene.camera, 0.1F);
    const fbe::golden::frame_t low = scene.drawn();
    require(fbe_entity_free(sun));
    CHECK(fbe::golden::compare(low, scene.drawn(), 1).pixels == 0);
    // Which isn't noon's.
    require(fbe_world_set_sky_atmosphere(scene.world, 0, 20.0F));
    CHECK(fbe::golden::compare(low, scene.drawn(), 8).pixels > 100);
}

TEST_CASE("a sky's columns are at the angles they should be", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t texture = sky_of(256, 2, [](std::int32_t x, std::int32_t /*y*/) {
        const auto column = static_cast<std::uint8_t>(x);
        return rgba_t{ column, column, column, 255 };
    });
    require(fbe_texture_set_filter(texture, FBE_TEXTURE_NEAREST));
    require(fbe_world_set_sky_texture(scene.world, texture, 1.0F));
    const fbe::golden::frame_t frame = scene.drawn();
    const float half = std::tan(1.0471976F / 2.0F);
    for (const std::uint32_t x : { 4U, 20U, 40U, 60U, 70U, 90U, 110U, 124U }) {
        const float across = (((static_cast<float>(x) + 0.5F) / 64.0F) - 1.0F) * half;
        const float column =
            (0.5F + (std::atan(across) / (2.0F * std::numbers::pi_v<float>))) * 256.0F;
        INFO("at " << x);
        CHECK(std::fabs(static_cast<float>(pixel(frame, x, 1)[0]) - column) <= 1.5F);
    }
    fbe_texture_release(texture);
}

TEST_CASE("a reflection's sky is the other way around", "[gpu][scene]") {
    const scene_t scene;
    const rgba_t red{ 255, 0, 0, 255 };
    const rgba_t green{ 0, 255, 0, 255 };
    const rgba_t blue{ 0, 0, 255, 255 };
    const rgba_t white{ 255, 255, 255, 255 };
    const std::uint64_t texture = sky_texture(red, green, blue, white);
    require(fbe_world_set_sky_texture(scene.world, texture, 1.0F));
    wall_mirror(scene.world, -10.0F);
    const fbe::golden::frame_t frame = scene.drawn();
    // Off a wall ahead, what's to the right goes on right, behind: the texture's far right, up or
    // down; so the left's its far left.
    CHECK(near(pixel(frame, 96, 32), { 0, 255, 0 }));
    CHECK(near(pixel(frame, 32, 32), { 255, 0, 0 }));
    CHECK(near(pixel(frame, 96, 96), { 255, 255, 255 }));
    CHECK(near(pixel(frame, 32, 96), { 0, 0, 255 }));
    fbe_texture_release(texture);
}

TEST_CASE("the atmosphere is what its formula says", "[gpu][scene]") {
    const scene_t scene;
    using way_t = reference_sky_t::way_t;
    const auto check_at = [&](float looking, way_t sun, double brightness, int within = 3) {
        pitch(scene.camera, looking);
        const rgba_t found = middle(scene.drawn());
        // The middle pixel's own way: half a pixel right of the view's axis, and half a pixel down.
        const double off = 0.5 / 64.0 * std::tan(1.0471976 / 2.0);
        const auto angle = static_cast<double>(looking);
        const double up = std::cos(angle) * -off;
        const double ahead = std::sin(angle) * off;
        const double y = std::sin(angle) + up;
        const double z = -std::cos(angle) + ahead;
        const double long_as = std::sqrt((off * off) + (y * y) + (z * z));
        const way_t way{ .x = off / long_as, .y = y / long_as, .z = z / long_as };
        const std::array<std::uint8_t, 3> expected =
            reference_sky_t::stored_for(way, sun, brightness);
        INFO(static_cast<int>(found[0])
             << "," << static_cast<int>(found[1]) << "," << static_cast<int>(found[2])
             << " against " << static_cast<int>(expected[0]) << "," << static_cast<int>(expected[1])
             << "," << static_cast<int>(expected[2]));
        CHECK(near(found, expected, within));
    };
    const way_t noon{ .x = 0.0, .y = 1.0, .z = 0.0 };
    SECTION("zenith, and near the horizon, at noon") {
        require(fbe_world_set_sky_atmosphere(scene.world, 0, 20.0F));
        check_at(std::numbers::pi_v<float> / 2.0F, noon, 20.0);
        check_at(0.05F, noon, 20.0);
        check_at(-0.2F, noon, 20.0);
    }
    SECTION("a brightness scales it") {
        require(fbe_world_set_sky_atmosphere(scene.world, 0, 5.0F));
        check_at(std::numbers::pi_v<float> / 2.0F, noon, 5.0);
    }
    SECTION("a sun just under the horizon") {
        const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
        // Toward the sun is its +Z: straight ahead and down, a little.
        pitch(sun, std::numbers::pi_v<float> - 0.05F);
        require(fbe_world_set_sky_atmosphere(scene.world, sun, 20.0F));
        const way_t under{ .x = 0.0, .y = -std::sin(0.05), .z = -std::cos(0.05) };
        check_at(0.3F, under, 20.0);
        check_at(1.2F, under, 20.0);
    }
    SECTION("a sun barely under the horizon, looked at, and bright: the ground's in its way") {
        const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
        pitch(sun, std::numbers::pi_v<float> - 0.01F);
        require(fbe_world_set_sky_atmosphere(scene.world, sun, 2000.0F));
        const way_t under{ .x = 0.0, .y = -std::sin(0.01), .z = -std::cos(0.01) };
        // So bright, what a float loses this close to the horizon shows.
        check_at(0.02F, under, 2000.0, 8);
        check_at(-0.05F, under, 2000.0, 8);
    }
}

TEST_CASE("the same seed draws the same particles, and another doesn't", "[gpu][scene]") {
    const auto seen = [](std::uint32_t seed) {
        const scene_t scene;
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
        const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, -8.0F, 0.1F);
        require(fbe_particles_set_seed(emitter, seed));
        require(fbe_particles_set_motion(emitter, 0.5F, 2.0F, 1.2F, 3.0F));
        require(fbe_particles_set_roll(emitter, 1.0F));
        require(fbe_particles_set_rate(emitter, 40.0F));
        require(fbe_world_step_particles(scene.world, 0.5F));
        require(fbe_world_step_particles(scene.world, 0.5F));
        return scene.drawn();
    };
    const fbe::golden::frame_t first = seen(7);
    CHECK(fbe::golden::compare(first, seen(7), 0).pixels == 0);
    CHECK(fbe::golden::compare(first, seen(8), 0).pixels > 20);
}

TEST_CASE("a little drag goes as no drag does", "[gpu][scene]") {
    const auto at_with = [](float drag) {
        const scene_t scene;
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
        const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, -8.0F, 0.3F);
        const float half = std::numbers::pi_v<float> / 4.0F;
        require(fbe_entity_set_rotation(emitter, 0.0F, -std::sin(half), 0.0F, std::cos(half),
                                        FBE_SPACE_WORLD));
        require(fbe_particles_set_motion(emitter, 2.0F, 2.0F, 0.0F, 0.0F));
        require(fbe_particles_set_forces(emitter, 0.0F, -2.0F, 0.0F, drag));
        require(fbe_particles_burst(emitter, 1));
        require(fbe_world_step_particles(scene.world, 1.0F));
        return lit_middle(scene.drawn());
    };
    const std::array<float, 2> none = at_with(0.0F);
    for (const float drag : { 1e-3F, 1e-6F, 1e-8F }) {
        const std::array<float, 2> found = at_with(drag);
        INFO("drag " << drag);
        CHECK(std::fabs(found[0] - none[0]) < 1.0F);
        CHECK(std::fabs(found[1] - none[1]) < 1.0F);
    }
}

TEST_CASE("particles that held off go as one staging write", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, -8.0F, 0.1F);
    require(fbe_particles_set_capacity(emitter, 256));
    require(fbe_particles_set_life(emitter, 1.0F, 512.0F));
    require(fbe_world_step_particles(scene.world, 256.0F));
    require(fbe_particles_burst(emitter, 256));
    (void)scene.drawn();
    // At half the clock's way, those dead are cleared, scattered all through its slots.
    require(fbe_world_step_particles(scene.world, 257.0F));
    const std::int32_t alive = fbe_particles_alive(emitter);
    CHECK(alive > 8);
    CHECK(alive < 248);
    const std::uint64_t before = fbe::gpu::staging_buffers_made();
    (void)scene.drawn();
    CHECK(fbe::gpu::staging_buffers_made() - before == 1);
    CHECK(fbe_gpu_validation_errors() == 0);
}

TEST_CASE("a particle's turned as it's told, no more", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, -4.0F, 1.0F);
    const std::uint64_t paint = unlit(1.0F, 1.0F, 1.0F);
    const std::uint64_t texture =
        sky_texture({ 255, 0, 0, 255 }, { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 255, 0, 255 });
    require(fbe_texture_set_filter(texture, FBE_TEXTURE_NEAREST));
    require(fbe_material_set_texture(paint, 0, texture));
    require(fbe_particles_set_material(emitter, paint));
    require(fbe_particles_set_capacity(emitter, 1));
    require(fbe_particles_burst(emitter, 1));
    const fbe::golden::frame_t still = scene.drawn();
    CHECK(near(pixel(still, 64, 48), { 255, 0, 0 }));
    CHECK(near(pixel(still, 64, 80), { 0, 255, 0 }));
    // Now any way at all: over enough of them, one's the wrong way up.
    require(fbe_particles_set_roll(emitter, std::numbers::pi_v<float>));
    bool upside_down = false;
    for (int i = 0; i < 16 && !upside_down; ++i) {
        require(fbe_particles_burst(emitter, 1));
        upside_down = near(pixel(scene.drawn(), 64, 48), { 0, 255, 0 });
    }
    CHECK(upside_down);
    fbe_texture_release(texture);
    fbe_material_release(paint);
}

TEST_CASE("an emitter draws out of view once its long lives are over", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, 50.0F, 0.1F);
    require(fbe_particles_set_forces(emitter, 0.0F, -0.1F, 0.0F, 0.0F));
    require(fbe_particles_set_life(emitter, 512.0F, 512.0F));
    require(fbe_particles_burst(emitter, 1));
    std::array<fbe_draw_t, 2> draws{};
    const auto count = [&] {
        return fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, draws.data(), 2);
    };
    // Falling for all that time, it could be anywhere.
    CHECK(count() == 1);
    require(fbe_particles_set_life(emitter, 1.0F, 1.0F));
    require(fbe_particles_set_rate(emitter, 2.0F));
    require(fbe_world_step_particles(scene.world, 100.0F));
    CHECK(count() == 1);
    require(fbe_world_step_particles(scene.world, 500.0F));
    require(fbe_world_step_particles(scene.world, 2.0F));
    CHECK(count() == 0);
}

TEST_CASE("a burst between steps shows while they're drawn between", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, -4.0F, 1.0F);
    require(fbe_world_capture(scene.world));
    require(fbe_world_step_particles(scene.world, 1.0F));
    require(fbe_particles_burst(emitter, 1));
    CHECK(reds_above(scene.drawn(0.5F), 0) != 0);
    CHECK(reds_above(scene.drawn(0.0F), 0) != 0);
    CHECK(reds_above(scene.drawn(1.0F), 0) != 0);
    // With its capture long gone, it's still born: it's never dead on arrival.
    require(fbe_world_step_particles(scene.world, 100.0F));
    const std::int32_t before = fbe_particles_alive(emitter);
    require(fbe_particles_burst(emitter, 1));
    CHECK(fbe_particles_alive(emitter) == before + 1);
}

TEST_CASE("an emitter's in view by what it's spread to", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, -10.0F, 0.5F);
    require(fbe_particles_burst(emitter, 1));
    require(fbe_entity_set_position(emitter, 0.0F, 0.0F, 100.0F, FBE_SPACE_WORLD));
    CHECK(fbe_camera_entity_in_view(scene.camera, 128.0F, 128.0F, emitter) == 1);
    fbe_aabb_t box{};
    require(fbe_entity_bounds(emitter, &box));
    CHECK(box.min.z < -9.0F);
    CHECK(box.max.z > -11.0F);
}

TEST_CASE("an emitter draws as many as it's had, not as many as it holds", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, -4.0F, 1.0F);
    require(fbe_particles_set_capacity(emitter, 1048576));
    require(fbe_particles_burst(emitter, 1));
    (void)scene.drawn();
    fbe_gpu_counters_t counters{};
    fbe_gpu_counters(&counters);
    CHECK(counters.triangles == 2);
    require(fbe_particles_burst(emitter, 3));
    (void)scene.drawn();
    fbe_gpu_counters(&counters);
    CHECK(counters.triangles == 8);
}

TEST_CASE("a clock's turn is the same to what's drawn between steps", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t emitter = still_emitter(scene.world, -4.0F, 0.0F, -8.0F, 0.25F);
    // Up to a tenth of a second from its turn.
    require(fbe_world_step_particles(scene.world, 1023.9F));
    require(fbe_world_capture(scene.world));
    // Two born as the step goes across it, a quarter of the way and three quarters: at -2 and 2.
    require(fbe_particles_set_rate(emitter, 10.0F));
    require(fbe_entity_set_position(emitter, 4.0F, 0.0F, -8.0F, FBE_SPACE_WORLD));
    require(fbe_world_step_particles(scene.world, 0.2F));
    const auto lit_at = [&](const fbe::golden::frame_t& frame, float x) {
        return pixel(frame, static_cast<std::uint32_t>(64.0F + (x * pixels_at_1 / 8.0F)), 64)[0] !=
               0;
    };
    const fbe::golden::frame_t early = scene.drawn(0.5F);
    CHECK(lit_at(early, -2.0F));
    CHECK_FALSE(lit_at(early, 2.0F));
    const fbe::golden::frame_t late = scene.drawn(0.9F);
    CHECK(lit_at(late, -2.0F));
    CHECK(lit_at(late, 2.0F));
}

TEST_CASE("an emitter's box goes where its particles can", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    SECTION("falling into view from out of it") {
        const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 40.0F, -8.0F, 1.0F);
        require(fbe_particles_set_forces(emitter, 0.0F, -20.0F, 0.0F, 0.0F));
        require(fbe_particles_burst(emitter, 1));
        CHECK(reds_above(scene.drawn(), 0) == 0);
        require(fbe_world_step_particles(scene.world, 2.0F));
        CHECK(near(middle(scene.drawn()), { 255, 255, 255 }));
    }
    SECTION("in its last ninth of a life, its emitter gone from there") {
        const std::uint64_t emitter = still_emitter(scene.world, 0.0F, 0.0F, -4.0F, 1.0F);
        require(fbe_particles_set_life(emitter, 9.0F, 9.0F));
        // At the end of its first slice, the last born in it.
        require(fbe_world_step_particles(scene.world, 0.99F));
        require(fbe_particles_burst(emitter, 1));
        require(fbe_entity_set_position(emitter, 0.0F, 0.0F, 500.0F, FBE_SPACE_WORLD));
        for (int i = 0; i < 17; ++i) {
            require(fbe_world_step_particles(scene.world, 0.5F));
        }
        CHECK(fbe_particles_alive(emitter) == 1);
        CHECK(near(middle(scene.drawn()), { 255, 255, 255 }));
        // And none after, once what it's left has gone by.
        require(fbe_world_step_particles(scene.world, 12.0F));
        std::array<fbe_draw_t, 2> draws{};
        CHECK(fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, draws.data(), 2) == 0);
    }
}

TEST_CASE("a decal's parented to what it's on, keeping its place, it follows", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t white = unlit(1.0F, 1.0F, 1.0F);
    const std::uint64_t receiver = wall(scene.world, -8.0F, 2.0F, white);
    fbe_mat4_t box{};
    box.m[0] = 0.5F;
    box.m[5] = 0.5F;
    box.m[10] = 0.5F;
    box.m[14] = -8.0F;
    box.m[15] = 1.0F;
    const std::uint64_t geometry = fbe_geometry_decal(scene.world, &box, 0.01F, 1.3F);
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t mesh = fbe_entity_create(scene.world, 0, FBE_ENTITY_MESH);
    require(fbe_mesh_set_geometry(mesh, geometry));
    require(fbe_mesh_set_material(mesh, red));
    fbe_geometry_release(geometry);
    require(fbe_entity_set_parent(mesh, receiver, true));
    CHECK(near(middle(scene.drawn()), { 255, 0, 0 }));
    require(fbe_entity_set_position(receiver, 1.5F, 0.0F, -8.0F, FBE_SPACE_WORLD));
    const fbe::golden::frame_t moved = scene.drawn();
    CHECK(near(middle(moved), { 255, 255, 255 }));
    CHECK(near(pixel_of(moved, pixel_at(scene.camera, 1.5F, 0.0F, -8.0F)), { 255, 0, 0 }));
    fbe_material_release(white);
    fbe_material_release(red);
}

TEST_CASE("a decal stays on a bumpy terrain from however far off", "[gpu][scene]") {
    const float distance = GENERATE(30.0F, 80.0F, 200.0F, 500.0F);
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    require(fbe_camera_set_range(scene.camera, 1.0F, 2000.0F));
    const std::uint64_t white = unlit(1.0F, 1.0F, 1.0F);
    const std::uint64_t terrain = fbe_entity_create(scene.world, 0, FBE_ENTITY_TERRAIN);
    const std::int32_t size = 512;
    require(fbe_terrain_set_size(terrain, size));
    std::vector<float> heights;
    std::uint32_t state = 99;
    for (std::int32_t z = 0; z <= size; ++z) {
        for (std::int32_t x = 0; x <= size; ++x) {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            heights.push_back(0.5F * static_cast<float>(state >> 8U) / 16777216.0F);
        }
    }
    require(fbe_terrain_set_heights(terrain, heights.data(),
                                    static_cast<std::int32_t>(heights.size())));
    require(fbe_terrain_set_material(terrain, white));
    // Over the middle, facing up, 40 across.
    fbe_mat4_t box{};
    box.m[0] = 20.0F;
    box.m[6] = -20.0F;
    box.m[9] = 20.0F;
    box.m[12] = 256.0F;
    box.m[14] = 256.0F;
    box.m[15] = 1.0F;
    const std::uint64_t geometry = fbe_geometry_decal(scene.world, &box, 0.01F, 1.309F);
    REQUIRE(geometry != 0);
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F, 0.99F);
    require(fbe_material_set_blend(red, FBE_BLEND_ALPHA));
    const std::uint64_t mesh = fbe_entity_create(scene.world, 0, FBE_ENTITY_MESH);
    require(fbe_mesh_set_geometry(mesh, geometry));
    require(fbe_mesh_set_material(mesh, red));
    require(fbe_entity_set_position(scene.camera, 256.0F, distance * 0.7F,
                                    256.0F + (distance * 0.7F), FBE_SPACE_WORLD));
    require(fbe_entity_point_at(scene.camera, 256.0F, 0.0F, 256.0F, 0.0F));
    const auto reds_in = [](const fbe::golden::frame_t& frame) {
        int count = 0;
        for (std::size_t at = 0; at < frame.rgba.size(); at += 4) {
            count += frame.rgba[at] > 200 && frame.rgba[at + 1] < 60 ? 1 : 0;
        }
        return count;
    };
    const int shown = reds_in(scene.drawn());
    // Depth left out, it's all there: how many pixels it has.
    require(fbe_material_set_order(red, 1));
    const int all = reds_in(scene.drawn());
    CHECK(all > 0);
    CHECK(static_cast<float>(shown) >= static_cast<float>(all) * 0.98F);
    fbe_geometry_release(geometry);
    fbe_material_release(white);
    fbe_material_release(red);
}

TEST_CASE("each particle sets off at a speed between its least and most", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t emitter = still_emitter(scene.world, -3.0F, 0.0F, -12.0F, 0.15F);
    require(fbe_particles_set_capacity(emitter, 256));
    const float half = std::numbers::pi_v<float> / 4.0F;
    require(fbe_entity_set_rotation(emitter, 0.0F, -std::sin(half), 0.0F, std::cos(half),
                                    FBE_SPACE_WORLD));
    require(fbe_particles_set_motion(emitter, 2.0F, 6.0F, 0.0F, 0.0F));
    require(fbe_particles_burst(emitter, 256));
    require(fbe_world_step_particles(scene.world, 1.0F));
    const fbe::golden::frame_t frame = scene.drawn();
    // All on one line, from -1 to 3: spread along all of it, and none off it.
    const auto column_of = [](float x) {
        return static_cast<int>(64.0F + (x * pixels_at_1 / 12.0F));
    };
    int least = frame_size;
    int most = -1;
    for (int x = 0; x < frame_size; ++x) {
        if (pixel(frame, static_cast<std::uint32_t>(x), 64)[0] != 0) {
            least = std::min(least, x);
            most = std::max(most, x);
        }
    }
    CHECK(std::abs(least - column_of(-1.0F)) <= 3);
    CHECK(std::abs(most - column_of(3.0F)) <= 3);
}

namespace {

/// A matrix at a place, its axes the world's, scaled.
fbe_mat4_t placed(float x, float y, float z, float scale = 1.0F) {
    fbe_mat4_t matrix{};
    matrix.m[0] = scale;
    matrix.m[5] = scale;
    matrix.m[10] = scale;
    matrix.m[12] = x;
    matrix.m[13] = y;
    matrix.m[14] = z;
    matrix.m[15] = 1.0F;
    return matrix;
}

fbe_instance_t instance_at(float x, float y, float z, float scale = 1.0F) {
    return {
        .matrix = placed(x, y, z, scale),
        .color = { .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F },
    };
}

/// A cube's Instances, in a material, at some places.
std::uint64_t cubes_at(std::uint64_t world, std::uint64_t material,
                       const std::vector<fbe_instance_t>& all) {
    const std::uint64_t made = fbe_entity_create(world, 0, FBE_ENTITY_INSTANCES);
    const std::uint64_t geometry = fbe_geometry_cube();
    require(fbe_instances_set_geometry(made, geometry));
    fbe_geometry_release(geometry);
    require(fbe_instances_set_material(made, material));
    require(fbe_instances_set(made, all.data(), static_cast<std::int32_t>(all.size())));
    return made;
}

/// How many draws a frame recorded, after drawing it.
std::int64_t draws_of(const scene_t& scene) {
    fbe_gpu_counters_t counters{};
    (void)scene.drawn();
    fbe_gpu_counters(&counters);
    return counters.draws;
}

} // namespace

TEST_CASE("a geometry's draws go as instances, whatever their materials", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::array geometries{ fbe_geometry_cube(), fbe_geometry_sphere(8) };
    const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t green = unlit(0.0F, 1.0F, 0.0F);
    const std::uint64_t blue = unlit(0.0F, 0.0F, 1.0F);
    const std::array materials{ red, green, blue };
    for (std::size_t i = 0; i < 12; ++i) {
        const std::uint64_t mesh = fbe_entity_create(scene.world, 0, FBE_ENTITY_MESH);
        require(fbe_mesh_set_geometry(mesh, geometries[i % 2]));
        require(fbe_mesh_set_material(mesh, materials[i % 3]));
        // Near and far by turns, so by depth alone they'd go back and forth.
        const float depth = i % 2 == 0 ? -40.0F : -60.0F;
        require(fbe_entity_set_position(mesh, (static_cast<float>(i) - 5.5F) * 4.0F, 0.0F, depth,
                                        FBE_SPACE_WORLD));
    }
    CHECK(draws_of(scene) == 2);
    const fbe::golden::frame_t frame = scene.drawn();
    // Each still in its own material: the first's red, the second's green, the third's blue.
    CHECK(near(pixel_of(frame, pixel_at(scene.camera, -22.0F, 0.0F, -40.0F)), { 255, 0, 0 }));
    CHECK(near(pixel_of(frame, pixel_at(scene.camera, -18.0F, 0.0F, -60.0F)), { 0, 255, 0 }));
    CHECK(near(pixel_of(frame, pixel_at(scene.camera, -14.0F, 0.0F, -40.0F)), { 0, 0, 255 }));
    for (const std::uint64_t geometry : geometries) {
        fbe_geometry_release(geometry);
    }
    for (const std::uint64_t material : materials) {
        fbe_material_release(material);
    }
}

TEST_CASE("an Instances draws its geometry at each of its matrices, in its color", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t paint = unlit(1.0F, 1.0F, 1.0F);
    std::vector<fbe_instance_t> all;
    all.push_back(instance_at(-3.0F, 0.0F, -10.0F));
    all.push_back(instance_at(3.0F, 0.0F, -10.0F));
    all[1].color = { .r = 1.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F };
    const std::uint64_t made = cubes_at(scene.world, paint, all);
    CHECK(fbe_entity_kind(made) == FBE_ENTITY_INSTANCES);
    CHECK(fbe_instances_count(made) == 2);
    const fbe::golden::frame_t frame = scene.drawn();
    CHECK(near(pixel_of(frame, pixel_at(scene.camera, -3.0F, 0.0F, -9.0F)), { 255, 255, 255 }));
    CHECK(near(pixel_of(frame, pixel_at(scene.camera, 3.0F, 0.0F, -9.0F)), { 255, 0, 0 }));
    CHECK(near(middle(frame), { 0, 0, 0 }));

    SECTION("in its entity's space") {
        require(fbe_entity_set_position(made, 0.0F, 3.0F, 0.0F, FBE_SPACE_WORLD));
        const fbe::golden::frame_t moved = scene.drawn();
        CHECK(near(pixel_of(moved, pixel_at(scene.camera, 3.0F, 3.0F, -9.0F)), { 255, 0, 0 }));
        CHECK(pixel_of(moved, pixel_at(scene.camera, 3.0F, 0.0F, -9.0F))[0] == 0);
    }
    SECTION("changed one at a time, by where it was set, whatever its order is") {
        // Set the other way around from where they're kept, near each other along the world.
        const std::array backward{ all[1], all[0] };
        require(fbe_instances_set(made, backward.data(), 2));
        fbe_instance_t first{};
        require(fbe_instances_get(made, 0, &first));
        CHECK(first.matrix.m[12] == 3.0F);
        require(fbe_instances_set(made, all.data(), 2));
        fbe_instance_t one{};
        require(fbe_instances_get(made, 1, &one));
        CHECK(one.color.g == 0.0F);
        CHECK(one.matrix.m[12] == 3.0F);
        one.color = { .r = 0.0F, .g = 1.0F, .b = 0.0F, .a = 1.0F };
        require(fbe_instances_set_one(made, 1, &one));
        CHECK(near(pixel_of(scene.drawn(), pixel_at(scene.camera, 3.0F, 0.0F, -9.0F)),
                   { 0, 255, 0 }));
        CHECK_FALSE(fbe_instances_set_one(made, 2, &one));
        CHECK_FALSE(fbe_instances_get(made, -1, &one));
    }
    SECTION("copied, its own") {
        const std::uint64_t copy = fbe_entity_copy(made, scene.world, 0);
        CHECK(fbe_instances_count(copy) == 2);
        CHECK(fbe_instances_geometry(copy) == fbe_instances_geometry(made));
        require(fbe_entity_free(made));
        require(fbe_entity_set_position(copy, 0.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
        CHECK(near(pixel_of(scene.drawn(), pixel_at(scene.camera, 3.0F, 0.0F, -9.0F)),
                   { 255, 0, 0 }));
    }
    SECTION("emptied, and with no geometry, it draws none") {
        require(fbe_instances_set(made, nullptr, 0));
        CHECK(reds_above(scene.drawn(), 0) == 0);
        require(fbe_instances_set(made, all.data(), 2));
        require(fbe_instances_set_geometry(made, 0));
        CHECK(reds_above(scene.drawn(), 0) == 0);
    }
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(paint);
}

TEST_CASE("an Instances' normals turn with each one, and it's lit", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t paint = fbe_material_create();
    const std::uint64_t shining = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    (void)shining;
    // A cube turned 45 degrees about Y: its faces at the camera are half as lit each way as head
    // on.
    const float c = std::cos(std::numbers::pi_v<float> / 4.0F);
    fbe_instance_t turned = instance_at(0.0F, 0.0F, -6.0F);
    turned.matrix.m[0] = c;
    turned.matrix.m[2] = -c;
    turned.matrix.m[8] = c;
    turned.matrix.m[10] = c;
    cubes_at(scene.world, paint, { turned });
    const rgba_t seen = pixel(scene.drawn(), 64 + 8, 64);
    const std::uint8_t lit = srgb(rough_white(c));
    CHECK(near(seen, { lit, lit, lit }, 4));
    fbe_material_release(paint);
}

TEST_CASE("an Instances' tangents turn with each one, as a mesh's do", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t paint = fbe_material_create();
    const std::uint64_t shining = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    (void)shining;
    // Up each face that's seen, which a normal map leans.
    const std::uint64_t geometry = fbe_geometry_cube();
    for (std::int32_t surface = 0; surface < fbe_geometry_surface_count(geometry); ++surface) {
        const std::vector<fbe_vertex_tangent_t> tangents(
            static_cast<std::size_t>(fbe_surface_vertex_count(geometry, surface)),
            fbe_vertex_tangent_t{ .x = 0.0F, .y = 1.0F, .z = 0.0F, .w = 1.0F });
        require(fbe_surface_set_tangents(geometry, surface, tangents.data(),
                                         static_cast<std::int32_t>(tangents.size())));
    }
    const std::uint64_t leaning = flat_texture(128, 220, 180, 255, FBE_TEXTURE_RGBA8_LINEAR);
    require(fbe_material_set_map(paint, FBE_MAP_NORMAL, leaning, 0));
    const float c = std::cos(std::numbers::pi_v<float> / 4.0F);
    const float half_sine = std::sin(std::numbers::pi_v<float> / 8.0F);
    const float half_cosine = std::cos(std::numbers::pi_v<float> / 8.0F);
    const std::uint64_t mesh = fbe_entity_create(scene.world, 0, FBE_ENTITY_MESH);
    require(fbe_mesh_set_geometry(mesh, geometry));
    require(fbe_mesh_set_material(mesh, paint));
    require(fbe_entity_set_position(mesh, 0.0F, 0.0F, -6.0F, FBE_SPACE_WORLD));
    require(fbe_entity_set_rotation(mesh, 0.0F, half_sine, 0.0F, half_cosine, FBE_SPACE_WORLD));
    const rgba_t drawn = pixel(scene.drawn(), 64 + 8, 64);
    fbe_entity_free(mesh);
    fbe_instance_t turned = instance_at(0.0F, 0.0F, -6.0F);
    turned.matrix.m[0] = c;
    turned.matrix.m[2] = -c;
    turned.matrix.m[8] = c;
    turned.matrix.m[10] = c;
    const std::uint64_t many = fbe_entity_create(scene.world, 0, FBE_ENTITY_INSTANCES);
    require(fbe_instances_set_geometry(many, geometry));
    require(fbe_instances_set_material(many, paint));
    require(fbe_instances_set(many, &turned, 1));
    const rgba_t instanced = pixel(scene.drawn(), 64 + 8, 64);
    // With no map, it's as the face's own normal has it, which the map's leaned from.
    require(fbe_material_set_map(paint, FBE_MAP_NORMAL, 0, 0));
    const rgba_t flat = pixel(scene.drawn(), 64 + 8, 64);
    fbe_entity_free(many);
    INFO("instanced " << int{ instanced[0] } << ", drawn " << int{ drawn[0] } << ", flat "
                      << int{ flat[0] });
    CHECK(near(instanced, { drawn[0], drawn[1], drawn[2] }, 2));
    CHECK(std::abs(int{ drawn[0] } - int{ flat[0] }) > 8);
    fbe_geometry_release(geometry);
    fbe_texture_release(leaning);
    fbe_material_release(paint);
}

TEST_CASE("an Instances draws only its clusters in view", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t paint = unlit(1.0F, 1.0F, 1.0F);
    // Two far apart: a thousand in view, and a thousand behind.
    std::vector<fbe_instance_t> all;
    for (int i = 0; i < 1000; ++i) {
        const auto along = static_cast<float>(i % 32);
        const float across = std::floor(static_cast<float>(i) / 32.0F);
        all.push_back(instance_at((along - 16.0F) * 0.1F, (across - 16.0F) * 0.1F, -10.0F, 0.04F));
        all.push_back(instance_at((along - 16.0F) * 0.1F, (across - 16.0F) * 0.1F, 500.0F, 0.04F));
    }
    const std::uint64_t made = cubes_at(scene.world, paint, all);
    (void)made;
    (void)scene.drawn();
    fbe_gpu_counters_t counters{};
    fbe_gpu_counters(&counters);
    const std::int64_t triangles = counters.triangles;
    CHECK(triangles >= std::int64_t{ 12000 });
    CHECK(triangles <= std::int64_t{ 13200 });
    CHECK(reds_above(scene.drawn(), 0) > 100);
    fbe_material_release(paint);
}

TEST_CASE("an Instances casts, and a hundred thousand draw", "[gpu][scene]") {
    SECTION("casts") {
        const scene_t scene;
        const std::uint64_t paint = fbe_material_create();
        wall(scene.world, -10.0F, 30.0F, paint);
        sun_from_right(scene.world);
        const std::uint64_t made = cubes_at(scene.world, paint, { instance_at(5.0F, 0.0F, -5.0F) });
        CHECK(middle(scene.drawn())[0] == 0);
        require(fbe_entity_set_casts_shadows(made, false));
        CHECK(middle(scene.drawn())[0] != 0);
        fbe_material_release(paint);
    }
    SECTION("a hundred thousand") {
        const scene_t scene;
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
        const std::uint64_t paint = unlit(1.0F, 1.0F, 1.0F);
        std::vector<fbe_instance_t> all;
        for (int i = 0; i < 100000; ++i) {
            const auto x = static_cast<float>(i % 316);
            const float z = std::floor(static_cast<float>(i) / 316.0F);
            all.push_back(instance_at((x - 158.0F) * 2.0F, -3.0F, -2.0F - (z * 2.0F), 0.5F));
        }
        cubes_at(scene.world, paint, all);
        CHECK(reds_above(scene.drawn(), 0) > 100);
        CHECK(fbe_gpu_validation_errors() == 0);
        fbe_material_release(paint);
    }
}

TEST_CASE("a mesh draws what its size in the view has it, of its geometry's levels",
          "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t fine_paint = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t coarse_paint = unlit(0.0F, 1.0F, 0.0F);
    const auto painted_cube = [](std::uint64_t paint) {
        const std::uint64_t made = fbe_geometry_cube();
        require(fbe_surface_set_material(made, 0, paint));
        return made;
    };
    const std::uint64_t fine = painted_cube(fine_paint);
    const std::uint64_t coarse = painted_cube(coarse_paint);
    // A cube's sphere is sqrt 3 across a half, its share that over the view's half and its
    // distance.
    const float half_height = std::tan(1.0471976F / 2.0F);
    const auto share_at = [&](float distance) {
        return std::numbers::sqrt3_v<float> / (half_height * distance);
    };
    require(fbe_geometry_set_lod(fine, 0, coarse, share_at(20.0F)));
    require(fbe_geometry_set_lod(fine, 1, 0, share_at(60.0F)));
    CHECK(fbe_geometry_lod_count(fine) == 2);
    const std::uint64_t mesh = fbe_entity_create(scene.world, 0, FBE_ENTITY_MESH);
    require(fbe_mesh_set_geometry(mesh, fine));
    fbe_geometry_release(coarse);

    const auto seen_at = [&](float distance) {
        require(fbe_entity_set_position(mesh, 0.0F, 0.0F, -distance, FBE_SPACE_WORLD));
        return middle(scene.drawn());
    };
    CHECK(near(seen_at(10.0F), { 255, 0, 0 }));
    CHECK(near(seen_at(30.0F), { 0, 255, 0 }));
    CHECK(near(seen_at(80.0F), { 0, 0, 0 }));

    SECTION("its draw's the level's") {
        require(fbe_entity_set_position(mesh, 0.0F, 0.0F, -30.0F, FBE_SPACE_WORLD));
        std::array<fbe_draw_t, 2> draws{};
        REQUIRE(fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, draws.data(), 2) == 1);
        std::uint64_t level = 0;
        float share = 0.0F;
        require(fbe_geometry_lod(fine, 0, &level, &share));
        CHECK(draws[0].geometry == level);
    }
    SECTION("a narrower view makes it bigger") {
        require(fbe_camera_set_field_of_view(scene.camera, 0.3F));
        CHECK(near(seen_at(30.0F), { 255, 0, 0 }));
    }
    SECTION("what a target's size is doesn't matter") {
        const std::uint64_t small = fbe_texture_create_target(32, 32, FBE_TEXTURE_RGBA8, 1, true);
        require(fbe_camera_set_target(scene.camera, small));
        require(fbe_entity_set_position(mesh, 0.0F, 0.0F, -30.0F, FBE_SPACE_WORLD));
        require(fbe_render_world(scene.world, 1.0F));
        const fbe::golden::frame_t frame = scene_t::read(small);
        fbe_frame_flip(false);
        CHECK(near(middle(frame), { 0, 255, 0 }));
        require(fbe_camera_set_target(scene.camera, scene.target));
        fbe_texture_release(small);
    }
    SECTION("its levels go, and it's itself") {
        require(fbe_geometry_clear_lods(fine));
        CHECK(fbe_geometry_lod_count(fine) == 0);
        CHECK(near(seen_at(30.0F), { 255, 0, 0 }));
    }
    fbe_geometry_release(fine);
    fbe_material_release(fine_paint);
    fbe_material_release(coarse_paint);
}

TEST_CASE("levels of detail are checked, and hold what they draw", "[gpu][scene]") {
    const std::uint64_t base = fbe_geometry_cube();
    const std::uint64_t smaller = fbe_geometry_cube();
    const std::uint64_t smallest = fbe_geometry_cube();
    CHECK_FALSE(fbe_geometry_set_lod(base, 1, smaller, 0.5F));
    CHECK_FALSE(fbe_geometry_set_lod(base, 0, base, 0.5F));
    CHECK_FALSE(fbe_geometry_set_lod(base, 0, smaller, 0.0F));
    CHECK_FALSE(fbe_geometry_set_lod(base, 0, smaller, std::nanf("")));
    require(fbe_geometry_set_lod(base, 0, smaller, 0.5F));
    CHECK_FALSE(fbe_geometry_set_lod(base, 1, smallest, 0.5F));
    CHECK_FALSE(fbe_geometry_set_lod(base, 1, smallest, 0.6F));
    // No chains, so no cycles.
    CHECK_FALSE(fbe_geometry_set_lod(smaller, 0, smallest, 0.1F));
    CHECK_FALSE(fbe_geometry_set_lod(smallest, 0, base, 0.1F));
    require(fbe_geometry_set_lod(base, 1, smallest, 0.1F));
    std::uint64_t found = 0;
    float share = 0.0F;
    require(fbe_geometry_lod(base, 1, &found, &share));
    CHECK(found == smallest);
    CHECK(share == 0.1F);
    CHECK_FALSE(fbe_geometry_lod(base, 2, &found, &share));
    // It holds them; a clone holds them too; freed, they go.
    fbe_geometry_release(smaller);
    CHECK(fbe_geometry_alive(smaller));
    const std::uint64_t clone = fbe_geometry_clone(base);
    CHECK(fbe_geometry_lod_count(clone) == 2);
    fbe_geometry_release(base);
    CHECK(fbe_geometry_alive(smaller));
    fbe_geometry_release(clone);
    CHECK_FALSE(fbe_geometry_alive(smaller));
    CHECK(fbe_geometry_alive(smallest));
    fbe_geometry_release(smallest);
    CHECK_FALSE(fbe_geometry_alive(smallest));
}

TEST_CASE("what's wholly behind an occluder isn't drawn", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t paint = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t blocker = wall(scene.world, -5.0F, 1.0F, paint);
    const std::uint64_t hidden = cube(scene.world, 0.0F, 0.0F, -20.0F, paint);
    const auto drawn = [&] {
        std::array<fbe_draw_t, 8> draws{};
        const std::int32_t count =
            fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, draws.data(), 8);
        return std::ranges::any_of(std::span(draws).first(static_cast<std::size_t>(count)),
                                   [&](const fbe_draw_t& draw) { return draw.entity == hidden; });
    };
    CHECK(drawn());
    require(fbe_entity_set_occluder(blocker, true));
    CHECK(fbe_entity_occluder(blocker));
    CHECK_FALSE(drawn());

    SECTION("not by a camera that doesn't test") {
        require(fbe_camera_set_occlusion(scene.camera, false));
        CHECK_FALSE(fbe_camera_occlusion(scene.camera));
        CHECK(drawn());
    }
    SECTION("not if any of it shows past the edge") {
        // It reaches 1 at a depth of 5: 3.8 at the cube's near side's 19.
        require(fbe_entity_set_position(hidden, 3.5F, 0.0F, -20.0F, FBE_SPACE_WORLD));
        CHECK(drawn());
        require(fbe_entity_set_position(hidden, 2.5F, 0.0F, -20.0F, FBE_SPACE_WORLD));
        CHECK_FALSE(drawn());
    }
    SECTION("not if it's in front") {
        require(fbe_entity_set_position(hidden, 0.0F, 0.0F, -3.0F, FBE_SPACE_WORLD));
        CHECK(drawn());
    }
    SECTION("not when the occluder's back is to the view") {
        turn_about(blocker, 0.0F, 1.0F, 0.0F, std::numbers::pi_v<float>);
        CHECK(drawn());
    }
    SECTION("a two-sided one's back blocks too") {
        turn_about(blocker, 0.0F, 1.0F, 0.0F, std::numbers::pi_v<float>);
        require(fbe_material_set_two_sided(paint, true));
        CHECK_FALSE(drawn());
    }
    SECTION("not by what doesn't draw solid") {
        const std::uint64_t glass = unlit(1.0F, 1.0F, 1.0F, 0.5F);
        require(fbe_material_set_blend(glass, FBE_BLEND_ALPHA));
        require(fbe_mesh_set_material(blocker, glass));
        CHECK(drawn());
        require(fbe_mesh_set_material(blocker, paint));
        fbe_material_release(glass);
    }
    SECTION("not by what's hidden, or in another order") {
        require(fbe_entity_set_hidden(blocker, true));
        CHECK(drawn());
        require(fbe_entity_set_hidden(blocker, false));
        require(fbe_material_set_order(paint, 1));
        CHECK(drawn());
        require(fbe_material_set_order(paint, 0));
    }
    SECTION("by one crossing the near plane") {
        // A floor underfoot, to far away, seen from just over it, with a cube under it.
        const std::uint64_t floor = grid_floor(scene.world, -1.0F, 4, 200.0F, paint);
        require(fbe_entity_set_hidden(blocker, true));
        require(fbe_entity_set_occluder(floor, true));
        require(fbe_entity_set_position(hidden, 0.0F, -5.0F, -30.0F, FBE_SPACE_WORLD));
        CHECK_FALSE(drawn());
    }
    SECTION("its reach goes with it, whatever else is drawn") {
        const std::uint64_t sprite = fbe_entity_create(scene.world, 0, FBE_ENTITY_SPRITE);
        require(fbe_entity_set_position(sprite, 0.0F, 0.0F, -20.0F, FBE_SPACE_WORLD));
        std::array<fbe_draw_t, 8> draws{};
        const std::int32_t count =
            fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, draws.data(), 8);
        CHECK(count == 1);
        CHECK(draws[0].entity == blocker);
    }
    fbe_material_release(paint);
}

TEST_CASE("an Instances set far out of its order changes the one it says", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t paint = unlit(1.0F, 1.0F, 1.0F);
    // Index 0 at the far end of the world's curve from index 1, so they're kept the other way.
    const std::uint64_t made = cubes_at(
        scene.world, paint, { instance_at(3.0F, 3.0F, -10.0F), instance_at(-3.0F, -3.0F, -10.0F) });
    fbe_instance_t red = instance_at(3.0F, 3.0F, -10.0F);
    red.color = { .r = 1.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F };
    require(fbe_instances_set_one(made, 0, &red));
    const fbe::golden::frame_t frame = scene.drawn();
    CHECK(near(pixel_of(frame, pixel_at(scene.camera, 3.0F, 3.0F, -9.0F)), { 255, 0, 0 }));
    CHECK(near(pixel_of(frame, pixel_at(scene.camera, -3.0F, -3.0F, -9.0F)), { 255, 255, 255 }));
    fbe_instance_t found{};
    require(fbe_instances_get(made, 0, &found));
    CHECK(found.color.g == 0.0F);
    fbe_material_release(paint);
}

TEST_CASE("an occluder blocks what it draws, and only that", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = unlit(1.0F, 0.0F, 0.0F);
    const auto drawn_of = [&](std::uint64_t entity) {
        std::array<fbe_draw_t, 8> draws{};
        const std::int32_t count =
            fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, draws.data(), 8);
        return std::ranges::any_of(std::span(draws).first(static_cast<std::size_t>(count)),
                                   [&](const fbe_draw_t& draw) { return draw.entity == entity; });
    };
    SECTION("mirrored, it still faces as it draws") {
        const std::uint64_t blocker = wall(scene.world, -5.0F, 1.0F, paint);
        require(fbe_entity_set_scale(blocker, -1.0F, 1.0F, 1.0F, FBE_SPACE_WORLD));
        require(fbe_entity_set_occluder(blocker, true));
        const std::uint64_t hidden = cube(scene.world, 0.0F, 0.0F, -20.0F, paint);
        CHECK_FALSE(drawn_of(hidden));
        turn_about(blocker, 0.0F, 1.0F, 0.0F, std::numbers::pi_v<float>);
        CHECK(drawn_of(hidden));
    }
    SECTION("a cut-out has holes") {
        const std::uint64_t holes = unlit(1.0F, 0.0F, 0.0F);
        require(fbe_material_set_alpha_cutoff(holes, 0.5F));
        const std::uint64_t blocker = wall(scene.world, -5.0F, 1.0F, holes);
        require(fbe_entity_set_occluder(blocker, true));
        const std::uint64_t behind = cube(scene.world, 0.0F, 0.0F, -20.0F, paint);
        CHECK(drawn_of(behind));
        fbe_material_release(holes);
    }
    SECTION("a slit between two, narrower than its pixels, still shows what's behind") {
        const std::uint64_t left = wall(scene.world, -5.0F, 1.0F, paint);
        const std::uint64_t right = wall(scene.world, -5.0F, 1.0F, paint);
        require(fbe_entity_set_occluder(left, true));
        require(fbe_entity_set_occluder(right, true));
        const std::uint64_t behind = cube(scene.world, 0.0F, 0.0F, -20.0F, paint);
        require(fbe_entity_set_position(left, -1.005F, 0.0F, -5.0F, FBE_SPACE_WORLD));
        require(fbe_entity_set_position(right, 1.005F, 0.0F, -5.0F, FBE_SPACE_WORLD));
        CHECK(drawn_of(behind));
        // Closed, it doesn't.
        require(fbe_entity_set_position(left, -0.9F, 0.0F, -5.0F, FBE_SPACE_WORLD));
        require(fbe_entity_set_position(right, 0.9F, 0.0F, -5.0F, FBE_SPACE_WORLD));
        CHECK_FALSE(drawn_of(behind));
    }
    SECTION("past a curved one's outline by less than a pixel, it shows") {
        // A sphere's silhouette, where its triangles' edges are all shared, 0.2 of the way across
        // the view's tangent: this box's left side a quarter of a pixel past it.
        const std::uint64_t sphere = fbe_geometry_sphere(64);
        const std::uint64_t blocker = fbe_entity_create(scene.world, 0, FBE_ENTITY_MESH);
        require(fbe_mesh_set_geometry(blocker, sphere));
        fbe_geometry_release(sphere);
        require(fbe_mesh_set_material(blocker, paint));
        require(fbe_entity_set_position(blocker, 0.0F, 0.0F, -5.0F, FBE_SPACE_WORLD));
        require(fbe_entity_set_occluder(blocker, true));
        const float view = std::numbers::sqrt3_v<float> / 20.0F;
        const auto world_x = [&](float buffer_x) { return ((buffer_x - 128.0F) / 128.0F) / view; };
        const float from = world_x(82.5F);
        const float to = world_x(90.0F);
        const std::uint64_t behind = cube(scene.world, (from + to) * 0.5F, 0.0F, -20.0F, paint);
        require(fbe_entity_set_scale(behind, (to - from) * 0.5F, 0.05F, 0.01F, FBE_SPACE_WORLD));
        CHECK(drawn_of(behind));
    }
    fbe_material_release(paint);
}

TEST_CASE("culling by occluders changes no pixel", "[gpu][scene]") {
    // Scenes of walls and what's around them, each drawn with and without its test.
    std::uint32_t state = 12345;
    const auto next = [&state] {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return static_cast<float>(state >> 8U) / 16777216.0F;
    };
    int culled_scenes = 0;
    const std::uint64_t round_geometry = fbe_geometry_cylinder(16, true);
    const std::uint64_t ball_geometry = fbe_geometry_sphere(12);
    for (int round = 0; round < 32; ++round) {
        const scene_t scene;
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
        require(fbe_camera_set_range(scene.camera, 0.5F, 200.0F));
        // Seen flat, and across a mirror, some of them.
        if (round % 4 == 1) {
            require(fbe_camera_set_projection(scene.camera, FBE_PROJECTION_ORTHOGRAPHIC));
            require(fbe_camera_set_orthographic_height(scene.camera, 30.0F));
        }
        if (round % 4 == 2) {
            wall_mirror(scene.world, -45.0F);
        }
        std::vector<std::uint64_t> paints;
        paints.reserve(6);
        for (int i = 0; i < 6; ++i) {
            paints.push_back(unlit(next(), next(), next()));
        }
        for (int i = 0; i < 4; ++i) {
            const std::uint64_t blocker =
                wall(scene.world, -3.0F - (next() * 15.0F), 1.0F + (next() * 6.0F),
                     paints[static_cast<std::size_t>(i)]);
            require(fbe_entity_set_position(blocker, (next() - 0.5F) * 10.0F,
                                            (next() - 0.5F) * 10.0F, -3.0F - (next() * 15.0F),
                                            FBE_SPACE_WORLD));
            // Head on, some of them, which rounds as nothing else does.
            const float turned = (next() - 0.5F) * 1.2F;
            turn_about(blocker, 0.0F, 1.0F, 0.0F, round % 2 == 0 ? turned : 0.0F);
            require(fbe_entity_set_occluder(blocker, true));
        }
        // Of welded triangles, whose outlines aren't edges of theirs.
        for (const std::uint64_t welded : { round_geometry, ball_geometry }) {
            const std::uint64_t made = fbe_entity_create(scene.world, 0, FBE_ENTITY_MESH);
            require(fbe_mesh_set_geometry(made, welded));
            require(fbe_mesh_set_material(made, paints[0]));
            require(fbe_entity_set_position(made, (next() - 0.5F) * 10.0F, (next() - 0.5F) * 10.0F,
                                            -4.0F - (next() * 10.0F), FBE_SPACE_WORLD));
            const float size = 1.0F + (next() * 3.0F);
            require(fbe_entity_set_scale(made, size, size, size, FBE_SPACE_WORLD));
            require(fbe_entity_set_occluder(made, true));
        }
        // What's over the scene shows whatever's in front.
        const std::uint64_t over = unlit(next(), next(), next());
        require(fbe_material_set_order(over, 1));
        for (int i = 0; i < 4; ++i) {
            const std::uint64_t block =
                cube(scene.world, (next() - 0.5F) * 10.0F, (next() - 0.5F) * 10.0F,
                     -5.0F - (next() * 30.0F), over);
            require(fbe_entity_set_scale(block, 0.5F, 0.5F, 0.5F, FBE_SPACE_WORLD));
        }
        fbe_material_release(over);
        for (int i = 0; i < 60; ++i) {
            const std::uint64_t block =
                cube(scene.world, (next() - 0.5F) * 30.0F, (next() - 0.5F) * 30.0F,
                     -2.0F - (next() * 40.0F), paints[4 + static_cast<std::size_t>(i % 2)]);
            const float size = 0.1F + (next() * 0.6F);
            require(fbe_entity_set_scale(block, size, size, size, FBE_SPACE_WORLD));
        }
        std::array<fbe_draw_t, 128> draws{};
        const std::int32_t with =
            fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, draws.data(), 128);
        const fbe::golden::frame_t tested = scene.drawn();
        require(fbe_camera_set_occlusion(scene.camera, false));
        const std::int32_t without =
            fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, draws.data(), 128);
        const fbe::golden::frame_t all = scene.drawn();
        CHECK(with <= without);
        culled_scenes += with < without ? 1 : 0;
        CHECK(fbe::golden::compare(all, tested, 0).pixels == 0);
        for (const std::uint64_t paint : paints) {
            fbe_material_release(paint);
        }
    }
    fbe_geometry_release(round_geometry);
    fbe_geometry_release(ball_geometry);
    // It'd pass by doing nothing, so it has to have done something.
    CHECK(culled_scenes > 12);
}

namespace {

/// Whether a camera draws an entity.
bool draws_entity(std::uint64_t camera, std::uint64_t entity) {
    std::array<fbe_draw_t, 64> draws{};
    const std::int32_t count = fbe_render_draws(camera, 128.0F, 128.0F, 1.0F, draws.data(), 64);
    return std::ranges::any_of(
        std::span(draws).first(static_cast<std::size_t>(std::min(count, 64))),
        [&](const fbe_draw_t& draw) { return draw.entity == entity; });
}

/// A number from 0 up to 1, the next of a sequence.
float next_of(std::uint32_t& state) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return static_cast<float>(state >> 8U) / 16777216.0F;
}

/// A turn about Y, then about X in what that's turned.
void set_yaw_then_pitch(std::uint64_t entity, float yaw, float pitch) {
    const float sy = std::sin(yaw * 0.5F);
    const float cy = std::cos(yaw * 0.5F);
    const float sx = std::sin(pitch * 0.5F);
    const float cx = std::cos(pitch * 0.5F);
    require(fbe_entity_set_rotation(entity, cy * sx, sy * cx, -sy * sx, cy * cx, FBE_SPACE_WORLD));
}

} // namespace

TEST_CASE("what's in another order isn't culled by what's in front", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t blue = unlit(0.0F, 0.0F, 1.0F);
    const std::uint64_t blocker = wall(scene.world, -5.0F, 30.0F, blue);
    require(fbe_entity_set_occluder(blocker, true));
    const std::uint64_t over = unlit(1.0F, 0.0F, 0.0F);
    require(fbe_material_set_order(over, 1));
    const std::uint64_t scene_paint = unlit(0.0F, 1.0F, 0.0F);
    SECTION("a mesh's material") {
        const std::uint64_t made = cube(scene.world, 0.0F, 0.0F, -20.0F, over);
        CHECK(draws_entity(scene.camera, made));
        CHECK(near(middle(scene.drawn()), { 255, 0, 0 }));
        require(fbe_mesh_set_material(made, scene_paint));
        CHECK_FALSE(draws_entity(scene.camera, made));
    }
    SECTION("a surface's, the rest of it culled") {
        const std::uint64_t made = cube(scene.world, 0.0F, 0.0F, -20.0F);
        const std::uint64_t geometry = fbe_geometry_cube();
        require(fbe_surface_set_material(geometry, 0, over));
        require(fbe_mesh_set_geometry(made, geometry));
        CHECK(draws_entity(scene.camera, made));
        require(fbe_surface_set_material(geometry, 0, scene_paint));
        CHECK_FALSE(draws_entity(scene.camera, made));
        fbe_geometry_release(geometry);
    }
    SECTION("a sprite's") {
        const std::uint64_t made = fbe_entity_create(scene.world, 0, FBE_ENTITY_SPRITE);
        require(fbe_entity_set_position(made, 0.0F, 0.0F, -20.0F, FBE_SPACE_WORLD));
        require(fbe_sprite_set_material(made, over));
        CHECK(draws_entity(scene.camera, made));
        CHECK(near(middle(scene.drawn()), { 255, 0, 0 }));
        require(fbe_sprite_set_material(made, scene_paint));
        CHECK_FALSE(draws_entity(scene.camera, made));
    }
    SECTION("an emitter's") {
        const std::uint64_t made = still_emitter(scene.world, 0.0F, 0.0F, -20.0F, 1.0F);
        require(fbe_particles_burst(made, 1));
        require(fbe_particles_set_material(made, over));
        CHECK(draws_entity(scene.camera, made));
        require(fbe_particles_set_material(made, scene_paint));
        CHECK_FALSE(draws_entity(scene.camera, made));
    }
    SECTION("an Instances'") {
        const std::uint64_t made = cubes_at(scene.world, over, { instance_at(0.0F, 0.0F, -20.0F) });
        CHECK(draws_entity(scene.camera, made));
        CHECK(near(middle(scene.drawn()), { 255, 0, 0 }));
        require(fbe_instances_set_material(made, scene_paint));
        CHECK_FALSE(draws_entity(scene.camera, made));
    }
    SECTION("a terrain's") {
        const std::uint64_t made = fbe_entity_create(scene.world, 0, FBE_ENTITY_TERRAIN);
        require(fbe_entity_set_position(made, -1.0F, -1.0F, -21.0F, FBE_SPACE_WORLD));
        require(fbe_terrain_set_material(made, over));
        CHECK(draws_entity(scene.camera, made));
        require(fbe_terrain_set_material(made, scene_paint));
        CHECK_FALSE(draws_entity(scene.camera, made));
    }
    fbe_material_release(blue);
    fbe_material_release(over);
    fbe_material_release(scene_paint);
}

TEST_CASE("a big occluder culls neither itself nor what's flush to it", "[gpu][scene]") {
    const float yaw = GENERATE(0.0F, 0.5F, std::numbers::pi_v<float> * 0.5F);
    const scene_t scene;
    require(fbe_camera_set_range(scene.camera, 0.1F, 5000.0F));
    const std::uint64_t paint = unlit(1.0F, 0.0F, 0.0F);
    std::uint32_t state = 4242;
    SECTION("a floor, from above") {
        const std::uint64_t floor_block = cube(scene.world, 0.0F, 0.0F, 0.0F, paint);
        require(fbe_entity_set_scale(floor_block, 1000.0F, 1.0F, 1000.0F, FBE_SPACE_WORLD));
        set_yaw_then_pitch(floor_block, yaw, 0.0F);
        require(fbe_entity_set_occluder(floor_block, true));
        const std::uint64_t flush = cube(scene.world, 0.0F, 0.9F, 0.0F, paint);
        require(fbe_entity_set_scale(flush, 5.0F, 0.1F, 5.0F, FBE_SPACE_WORLD));
        set_yaw_then_pitch(flush, yaw, 0.0F);
        set_yaw_then_pitch(scene.camera, yaw, -std::numbers::pi_v<float> * 0.5F);
        int culled = 0;
        for (int round = 0; round < 300; ++round) {
            const float x = (next_of(state) - 0.5F) * 1200.0F;
            const float z = (next_of(state) - 0.5F) * 1200.0F;
            const float apart = next_of(state);
            require(fbe_entity_set_position(scene.camera, x, 1.5F + (apart * apart * 400.0F), z,
                                            FBE_SPACE_WORLD));
            require(fbe_entity_set_position(flush, x, 0.9F, z, FBE_SPACE_WORLD));
            culled += draws_entity(scene.camera, floor_block) ? 0 : 1;
            culled += draws_entity(scene.camera, flush) ? 0 : 1;
        }
        CHECK(culled == 0);
    }
    SECTION("a wall, head on") {
        const std::uint64_t block = cube(scene.world, 0.0F, 0.0F, 0.0F, paint);
        require(fbe_entity_set_scale(block, 1000.0F, 1000.0F, 1.0F, FBE_SPACE_WORLD));
        set_yaw_then_pitch(block, yaw, 0.0F);
        require(fbe_entity_set_occluder(block, true));
        set_yaw_then_pitch(scene.camera, yaw, 0.0F);
        const float sine = std::sin(yaw);
        const float cosine = std::cos(yaw);
        int culled = 0;
        for (int round = 0; round < 300; ++round) {
            const float along = (next_of(state) - 0.5F) * 1200.0F;
            const float up = (next_of(state) - 0.5F) * 1200.0F;
            const float apart = next_of(state);
            const float out = 1.5F + (apart * apart * 400.0F);
            require(fbe_entity_set_position(scene.camera, (along * cosine) + (out * sine), up,
                                            (out * cosine) - (along * sine), FBE_SPACE_WORLD));
            culled += draws_entity(scene.camera, block) ? 0 : 1;
        }
        CHECK(culled == 0);
    }
    fbe_material_release(paint);
}

TEST_CASE("an Instances' normals are a mesh's with its matrix", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t paint = fbe_material_create();
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    turn_about(sun, 0.3F, 1.0F, 0.2F, 0.9F);
    const std::uint64_t mesh = cube(scene.world, 0.0F, 0.0F, -6.0F, paint);
    set_yaw_then_pitch(mesh, 0.7F, 0.4F);
    require(fbe_entity_set_scale(mesh, 2.0F, 0.5F, 1.0F, FBE_SPACE_WORLD));
    const fbe::golden::frame_t by_mesh = scene.drawn();
    fbe_mat4_t matrix{};
    require(fbe_entity_matrix(mesh, FBE_SPACE_WORLD, &matrix));
    require(fbe_entity_set_hidden(mesh, true));
    fbe_instance_t same = instance_at(0.0F, 0.0F, 0.0F);
    same.matrix = matrix;
    cubes_at(scene.world, paint, { same });
    const fbe::golden::frame_t by_instance = scene.drawn();
    CHECK(reds_above(by_mesh, 0) > 500);
    CHECK(fbe::golden::compare(by_mesh, by_instance, 2).pixels == 0);
    fbe_material_release(paint);
}

TEST_CASE("an Instances' normal map's a mesh's, by its tangents through its matrix",
          "[gpu][scene]") {
    const float x_scale = GENERATE(2.0F, -2.0F);
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t paint = fbe_material_create();
    const std::uint64_t map = normal_map(0.5F, -0.3F);
    require(fbe_material_set_map(paint, FBE_MAP_NORMAL, map, 0));
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    turn_about(sun, 0.3F, 1.0F, 0.2F, 0.9F);
    const std::uint64_t mesh = cube(scene.world, 0.0F, 0.0F, -6.0F, paint);
    // Along what isn't what its UVs have, so going by them instead shows.
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    const auto vertices = static_cast<std::size_t>(fbe_surface_vertex_count(geometry, 0));
    const std::vector<fbe_vertex_tangent_t> along(
        vertices, fbe_vertex_tangent_t{ .x = 0.6F, .y = 0.8F, .z = 0.0F, .w = -1.0F });
    require(
        fbe_surface_set_tangents(geometry, 0, along.data(), static_cast<std::int32_t>(vertices)));
    set_yaw_then_pitch(mesh, 0.7F, 0.4F);
    require(fbe_entity_set_scale(mesh, x_scale, 0.5F, 1.0F, FBE_SPACE_WORLD));
    const fbe::golden::frame_t by_mesh = scene.drawn();
    fbe_mat4_t matrix{};
    require(fbe_entity_matrix(mesh, FBE_SPACE_WORLD, &matrix));
    require(fbe_entity_set_hidden(mesh, true));
    fbe_instance_t same = instance_at(0.0F, 0.0F, 0.0F);
    same.matrix = matrix;
    const std::uint64_t made = fbe_entity_create(scene.world, 0, FBE_ENTITY_INSTANCES);
    require(fbe_instances_set_geometry(made, geometry));
    require(fbe_instances_set_material(made, paint));
    require(fbe_instances_set(made, &same, 1));
    const fbe::golden::frame_t by_instance = scene.drawn();
    CHECK(reds_above(by_mesh, 0) > 500);
    CHECK(fbe::golden::compare(by_mesh, by_instance, 2).pixels == 0);
    fbe_texture_release(map);
    fbe_material_release(paint);
}

TEST_CASE("what an occluder doesn't vouch for doesn't occlude", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t behind = cube(scene.world, 0.0F, 0.0F, -20.0F, paint);
    SECTION("a wireframe's mostly holes") {
        const std::uint64_t lines = unlit(1.0F, 0.0F, 0.0F);
        const std::uint64_t blocker = wall(scene.world, -5.0F, 1.0F, lines);
        require(fbe_entity_set_occluder(blocker, true));
        CHECK_FALSE(draws_entity(scene.camera, behind));
        require(fbe_material_set_wireframe(lines, true));
        CHECK(draws_entity(scene.camera, behind));
        fbe_material_release(lines);
    }
    SECTION("what a camera can fade out") {
        const std::uint64_t blocker = wall(scene.world, -5.0F, 1.0F, paint);
        require(fbe_entity_set_occluder(blocker, true));
        CHECK_FALSE(draws_entity(scene.camera, behind));
        require(fbe_entity_set_auto_fade(blocker, 1.0F, 100.0F));
        CHECK(draws_entity(scene.camera, behind));
    }
    SECTION("what morph targets move") {
        const std::uint64_t blocker = wall(scene.world, -5.0F, 1.0F, paint);
        require(fbe_entity_set_occluder(blocker, true));
        CHECK_FALSE(draws_entity(scene.camera, behind));
        add_lift(fbe_mesh_geometry(blocker), 50.0F);
        const std::array one{ 1.0F };
        require(fbe_morph_set_weights(blocker, one.data(), 1));
        CHECK(draws_entity(scene.camera, behind));
    }
    SECTION("what joints move") {
        const std::uint64_t blocker = wall(scene.world, -5.0F, 1.0F, paint);
        require(fbe_entity_set_occluder(blocker, true));
        CHECK_FALSE(draws_entity(scene.camera, behind));
        require(fbe_entity_free(blocker));
        const skinned_wall_t skinned = skinned_wall(scene.world, paint, -5.0F, 1.0F);
        all_on(skinned.geometry, 0, 1.0F);
        require(fbe_entity_set_occluder(skinned.mesh, true));
        CHECK(draws_entity(scene.camera, behind));
        fbe_skin_release(skinned.skin);
    }
    SECTION("what's past what a view rasterizes") {
        const std::uint64_t many = fbe_geometry_sphere(100);
        REQUIRE(fbe_surface_triangle_count(many, 0) > 16384);
        const std::uint64_t blocker = fbe_entity_create(scene.world, 0, FBE_ENTITY_MESH);
        require(fbe_mesh_set_geometry(blocker, many));
        fbe_geometry_release(many);
        require(fbe_mesh_set_material(blocker, paint));
        require(fbe_entity_set_position(blocker, 0.0F, 0.0F, -5.0F, FBE_SPACE_WORLD));
        require(fbe_entity_set_occluder(blocker, true));
        CHECK(draws_entity(scene.camera, behind));
        // What fits still does.
        const std::uint64_t small = wall(scene.world, -8.0F, 3.0F, paint);
        require(fbe_entity_set_occluder(small, true));
        CHECK_FALSE(draws_entity(scene.camera, behind));
    }
    fbe_material_release(paint);
}

TEST_CASE("a slit between curved occluders shows what's behind it", "[gpu][scene]") {
    // Two cylinders, whose sides' edges are all shared, a part of a pixel apart on the buffer: at
    // a depth of 10, a pixel's 10 tan 30 over 128.
    const float across = GENERATE(0.3F, 0.5F, 0.9F);
    const float pixel_wide = 10.0F * std::tan(std::numbers::pi_v<float> / 6.0F) / 128.0F;
    const float slit = across * pixel_wide;
    const scene_t scene;
    require(fbe_camera_set_range(scene.camera, 0.5F, 200.0F));
    const std::uint64_t paint = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t round = fbe_geometry_cylinder(16, true);
    const auto pillar = [&](float x) {
        const std::uint64_t made = fbe_entity_create(scene.world, 0, FBE_ENTITY_MESH);
        require(fbe_mesh_set_geometry(made, round));
        require(fbe_mesh_set_material(made, paint));
        require(fbe_entity_set_position(made, x, 0.0F, -10.0F, FBE_SPACE_WORLD));
        require(fbe_entity_set_scale(made, 1.0F, 30.0F, 1.0F, FBE_SPACE_WORLD));
        require(fbe_entity_set_occluder(made, true));
        return made;
    };
    const std::uint64_t left = pillar(-1.0F - (slit * 0.5F));
    const std::uint64_t right = pillar(1.0F + (slit * 0.5F));
    // Its seam, which isn't welded, away from the slit.
    turn_about(left, 0.0F, 1.0F, 0.0F, std::numbers::pi_v<float>);
    const std::uint64_t behind = cube(scene.world, 0.0F, 0.0F, -60.0F, paint);
    require(fbe_entity_set_scale(behind, 0.002F, 0.5F, 0.01F, FBE_SPACE_WORLD));
    CHECK(draws_entity(scene.camera, behind));
    // Closed, the pair of them does hide it: it isn't that nothing welded occludes.
    require(fbe_entity_set_position(left, -0.8F, 0.0F, -10.0F, FBE_SPACE_WORLD));
    require(fbe_entity_set_position(right, 0.8F, 0.0F, -10.0F, FBE_SPACE_WORLD));
    CHECK_FALSE(draws_entity(scene.camera, behind));
    fbe_geometry_release(round);
    fbe_material_release(paint);
}

TEST_CASE("a mesh draws, casts, and occludes the same level", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_range(scene.camera, 0.5F, 1000.0F));
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -10.0F, 300.0F, paint);
    const std::uint64_t sun = sun_from_right(scene.world);
    require(fbe_light_set_shadow_distance(sun, 400.0F));
    const float half_height = std::tan(1.0471976F / 2.0F);
    const auto share_at = [&](float distance) {
        return std::numbers::sqrt2_v<float> / (half_height * distance);
    };
    SECTION("skinned, it's where its joints are") {
        // It's nothing past a distance of 20, from its joints, out of sight, its shadow where
        // the camera sees.
        const skinned_wall_t skinned = skinned_wall(scene.world, paint, -5.0F, 1.0F);
        all_on(skinned.geometry, 0, 1.0F);
        require(fbe_geometry_set_lod(skinned.geometry, 0, 0, share_at(20.0F)));
        require(fbe_entity_set_position(skinned.still, 5.0F, 0.0F, -5.0F, FBE_SPACE_WORLD));
        require(fbe_entity_set_position(skinned.moving, 5.0F, 0.0F, -5.0F, FBE_SPACE_WORLD));
        // Near its joints, however far its entity is.
        require(fbe_entity_set_position(skinned.mesh, 0.0F, 0.0F, -500.0F, FBE_SPACE_WORLD));
        CHECK(middle(scene.drawn())[0] == 0);
        // Far from them, however near its entity is, it neither draws nor casts.
        require(fbe_entity_set_position(scene.camera, 5.0F, 0.0F, 200.0F, FBE_SPACE_WORLD));
        require(fbe_entity_set_position(skinned.mesh, 5.0F, 0.0F, 190.0F, FBE_SPACE_WORLD));
        CHECK_FALSE(draws_entity(scene.camera, skinned.mesh));
        CHECK(pixel_of(scene.drawn(), pixel_at(scene.camera, 0.0F, 0.0F, -10.0F))[0] != 0);
        fbe_skin_release(skinned.skin);
    }
    SECTION("each camera of a world, its shadow a level of another shape") {
        // A cube whose shadow's on the wall's middle, a coarser one's well above it.
        const std::uint64_t fine = fbe_geometry_cube();
        const std::uint64_t coarse = fbe_geometry_cube();
        fbe_mat4_t up{};
        up.m[0] = 1.0F;
        up.m[5] = 1.0F;
        up.m[10] = 1.0F;
        up.m[13] = 10.0F;
        up.m[15] = 1.0F;
        require(fbe_geometry_transform(coarse, &up));
        const float cube_share = std::numbers::sqrt3_v<float> / (half_height * 20.0F);
        require(fbe_geometry_set_lod(fine, 0, coarse, cube_share));
        const std::uint64_t mesh = fbe_entity_create(scene.world, 0, FBE_ENTITY_MESH);
        require(fbe_mesh_set_geometry(mesh, fine));
        require(fbe_mesh_set_material(mesh, paint));
        require(fbe_entity_set_position(mesh, 5.0F, 0.0F, -5.0F, FBE_SPACE_WORLD));
        CHECK(middle(scene.drawn())[0] == 0);
        require(fbe_entity_set_position(scene.camera, 0.0F, 0.0F, 40.0F, FBE_SPACE_WORLD));
        CHECK(middle(scene.drawn())[0] != 0);
        // Two cameras of one fbe_render_world, either way around, each with its own.
        const std::uint64_t other_target =
            fbe_texture_create_target(frame_size, frame_size, FBE_TEXTURE_RGBA8, 1, true);
        const std::uint64_t other = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
        require(fbe_camera_set_target(other, other_target));
        require(fbe_camera_set_range(other, 0.5F, 1000.0F));
        for (const float far_one : { 40.0F, 0.0F }) {
            require(fbe_entity_set_position(scene.camera, 0.0F, 0.0F, far_one, FBE_SPACE_WORLD));
            require(fbe_entity_set_position(other, 0.0F, 0.0F, 40.0F - far_one, FBE_SPACE_WORLD));
            require(fbe_render_world(scene.world, 1.0F));
            const fbe::golden::frame_t first = scene_t::read(scene.target);
            const fbe::golden::frame_t second = scene_t::read(other_target);
            fbe_frame_flip(false);
            CAPTURE(far_one);
            CHECK((middle(first)[0] == 0) == (far_one == 0.0F));
            CHECK((middle(second)[0] == 0) == (far_one != 0.0F));
        }
        fbe_texture_release(other_target);
        fbe_geometry_release(fine);
        fbe_geometry_release(coarse);
    }
    SECTION("an occluder blocks with the level it draws") {
        // Its level's small, so what its own bigger shape would have hidden shows.
        const std::uint64_t fine = fbe_geometry_cube();
        const std::uint64_t coarse = fbe_geometry_cube();
        fbe_mat4_t small{};
        small.m[0] = 0.01F;
        small.m[5] = 0.01F;
        small.m[10] = 0.01F;
        small.m[15] = 1.0F;
        require(fbe_geometry_transform(coarse, &small));
        const float cube_share = std::numbers::sqrt3_v<float> / (half_height * 2.0F);
        require(fbe_geometry_set_lod(fine, 0, coarse, cube_share));
        const std::uint64_t mesh = fbe_entity_create(scene.world, 0, FBE_ENTITY_MESH);
        require(fbe_mesh_set_geometry(mesh, fine));
        require(fbe_mesh_set_material(mesh, paint));
        require(fbe_entity_set_position(mesh, 0.0F, 0.0F, -4.0F, FBE_SPACE_WORLD));
        require(fbe_entity_set_occluder(mesh, true));
        const std::uint64_t behind = cube(scene.world, 0.0F, 0.0F, -8.0F, paint);
        require(fbe_entity_set_scale(behind, 0.1F, 0.1F, 0.1F, FBE_SPACE_WORLD));
        CHECK(draws_entity(scene.camera, behind));
        fbe_geometry_release(fine);
        fbe_geometry_release(coarse);
    }
    fbe_material_release(paint);
}

TEST_CASE("only occluders in view spend what a view rasterizes, and it's bounded", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t behind = cube(scene.world, 0.0F, 0.0F, -20.0F, paint);
    SECTION("what's behind the camera spends none of it") {
        const std::uint64_t many = fbe_geometry_sphere(40);
        const auto one = static_cast<std::int64_t>(fbe_surface_triangle_count(many, 0));
        std::int64_t triangles = 0;
        while (triangles <= 17000) {
            const std::uint64_t made = fbe_entity_create(scene.world, 0, FBE_ENTITY_MESH);
            require(fbe_mesh_set_geometry(made, many));
            require(fbe_mesh_set_material(made, paint));
            require(fbe_entity_set_position(made, 0.0F, 0.0F, 3.0F, FBE_SPACE_WORLD));
            require(fbe_entity_set_occluder(made, true));
            triangles += one;
        }
        // As big, further than they are, so past what's left of it were they counted.
        const std::uint64_t ahead = fbe_entity_create(scene.world, 0, FBE_ENTITY_MESH);
        require(fbe_mesh_set_geometry(ahead, many));
        require(fbe_mesh_set_material(ahead, paint));
        require(fbe_entity_set_position(ahead, 0.0F, 0.0F, -5.0F, FBE_SPACE_WORLD));
        require(fbe_entity_set_scale(ahead, 2.0F, 2.0F, 2.0F, FBE_SPACE_WORLD));
        require(fbe_entity_set_occluder(ahead, true));
        fbe_geometry_release(many);
        CHECK_FALSE(draws_entity(scene.camera, behind));
    }
    SECTION("what fills the view spends so many pixels and no more") {
        for (int i = 0; i < 200; ++i) {
            const std::uint64_t made =
                wall(scene.world, -1.0F - (static_cast<float>(i) * 0.01F), 500.0F, paint);
            require(fbe_entity_set_occluder(made, true));
        }
        CHECK_FALSE(draws_entity(scene.camera, behind));
        CHECK(fbe::render::occluder_pixels() > 2000000);
        CHECK(fbe::render::occluder_pixels() < 2097152 + 40000);
    }
    fbe_material_release(paint);
}

TEST_CASE("a box wider than an int counts isn't culled by the cast", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_range(scene.camera, 0.1F, 100.0F));
    const std::uint64_t paint = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t blocker = wall(scene.world, -0.15F, 0.02F, paint);
    require(fbe_entity_set_occluder(blocker, true));
    const std::uint64_t wide = cube(scene.world, 0.0F, 0.0F, -0.21F, paint);
    require(fbe_entity_set_scale(wide, 4000000.0F, 0.01F, 0.01F, FBE_SPACE_WORLD));
    CHECK(draws_entity(scene.camera, wide));
    fbe_material_release(paint);
}

TEST_CASE("what's next to what in an occluder is worked out once for its shape", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t blocker = wall(scene.world, -5.0F, 1.0F, paint);
    require(fbe_entity_set_occluder(blocker, true));
    const std::uint64_t other = unlit(0.0F, 1.0F, 0.0F);
    (void)scene.drawn();
    const std::uint64_t worked_out = fbe::render::occluder_edges_worked_out();
    const std::size_t kept = fbe::render::occluder_edges_kept();
    (void)scene.drawn();
    CHECK(fbe::render::occluder_edges_worked_out() == worked_out);
    // Its version goes up with its materials, which don't change its shape.
    const std::uint64_t geometry = fbe_mesh_geometry(blocker);
    require(fbe_surface_set_material(geometry, 0, other));
    (void)scene.drawn();
    CHECK(fbe::render::occluder_edges_worked_out() == worked_out);
    // Another triangle does.
    const std::array<std::uint32_t, 3> again{ 0, 1, 2 };
    require(fbe_surface_add_triangles(geometry, 0, again.data(), 3) >= 0);
    (void)scene.drawn();
    CHECK(fbe::render::occluder_edges_worked_out() == worked_out + 1);
    // Freed, it's forgotten.
    CHECK(fbe::render::occluder_edges_kept() == kept);
    require(fbe_entity_free(blocker));
    (void)scene.drawn();
    CHECK(fbe::render::occluder_edges_kept() == kept - 1);
    fbe_material_release(paint);
    fbe_material_release(other);
}

TEST_CASE("an Instances with an alpha blends, and doesn't cast", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t white = unlit(1.0F, 1.0F, 1.0F);
    fbe_instance_t one = instance_at(0.0F, 0.0F, -4.0F);
    const std::uint64_t made =
        cubes_at(scene.world, white, { one, instance_at(9.0F, 0.0F, -4.0F) });
    const auto blend_now = [&] {
        std::array<fbe_draw_t, 4> draws{};
        REQUIRE(fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, draws.data(), 4) == 1);
        return draws[0].blend;
    };
    CHECK(blend_now() == FBE_BLEND_OPAQUE);
    CHECK(near(middle(scene.drawn()), { 255, 255, 255 }));
    one.color.a = 0.5F;
    require(fbe_instances_set_one(made, 0, &one));
    CHECK(blend_now() == FBE_BLEND_ALPHA);
    CHECK(near(middle(scene.drawn()), { srgb(0.5F), srgb(0.5F), srgb(0.5F) }));
    SECTION("none of it, none") {
        one.color.a = 0.0F;
        require(fbe_instances_set_one(made, 0, &one));
        CHECK(near(middle(scene.drawn()), { 0, 0, 0 }));
    }
    SECTION("as set all at once") {
        require(fbe_instances_set(made, &one, 1));
        CHECK(blend_now() == FBE_BLEND_ALPHA);
    }
    SECTION("back to whole, it's opaque") {
        one.color.a = 1.0F;
        require(fbe_instances_set_one(made, 0, &one));
        CHECK(blend_now() == FBE_BLEND_OPAQUE);
    }
    SECTION("said to add by its material, it does") {
        require(fbe_material_set_blend(white, FBE_BLEND_ADD));
        CHECK(blend_now() == FBE_BLEND_ADD);
    }
    SECTION("it casts only whole") {
        const std::uint64_t lit = fbe_material_create();
        wall(scene.world, -10.0F, 30.0F, lit);
        sun_from_right(scene.world);
        const std::uint64_t caster = cubes_at(scene.world, lit, { instance_at(5.0F, 0.0F, -5.0F) });
        require(fbe_entity_set_hidden(made, true));
        CHECK(middle(scene.drawn())[0] == 0);
        fbe_instance_t faded = instance_at(5.0F, 0.0F, -5.0F);
        faded.color.a = 0.5F;
        require(fbe_instances_set(caster, &faded, 1));
        CHECK(middle(scene.drawn())[0] != 0);
        fbe_material_release(lit);
    }
    fbe_material_release(white);
}

TEST_CASE("an Instances' clusters share a shadow's draw, and one set sends one", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -30.0F, 60.0F, paint);
    const std::uint64_t sun = sun_from_right(scene.world);
    require(fbe_light_set_cascades(sun, 4));
    std::vector<fbe_instance_t> all;
    all.reserve(10000);
    for (int row = 0; row < 100; ++row) {
        for (int column = 0; column < 100; ++column) {
            all.push_back(instance_at(static_cast<float>(column) * 0.05F, -1.0F,
                                      -5.0F - (static_cast<float>(row) * 0.05F), 0.01F));
        }
    }
    const std::uint64_t made = cubes_at(scene.world, paint, all);
    const std::int64_t casting = draws_of(scene);
    require(fbe_entity_set_casts_shadows(made, false));
    const std::int64_t not_casting = draws_of(scene);
    CHECK(casting > not_casting);
    CHECK(casting - not_casting <= 16);
    require(fbe_entity_set_casts_shadows(made, true));
    (void)scene.drawn();
    (void)scene.drawn();
    fbe_gpu_counters_t counters{};
    require(fbe_gpu_counters(&counters));
    CHECK(counters.uploaded == 0);
    const auto one_record = static_cast<std::int64_t>(sizeof(fbe::render::instance_data_t));
    // One moved sends that one, and its cluster's box goes with it, there and back.
    const fbe_instance_t moved = instance_at(100.0F, 0.0F, -5.0F);
    require(fbe_instances_set_one(made, 7, &moved));
    (void)scene.drawn();
    require(fbe_gpu_counters(&counters));
    CHECK(counters.uploaded == one_record);
    fbe_aabb_t box{};
    require(fbe_entity_bounds(made, &box));
    CHECK(box.max.x > 99.0F);
    fbe_instance_t got{};
    require(fbe_instances_get(made, 7, &got));
    CHECK(got.matrix.m[12] == 100.0F);
    const fbe_instance_t back = all[7];
    require(fbe_instances_set_one(made, 7, &back));
    require(fbe_entity_bounds(made, &box));
    CHECK(box.max.x < 6.0F);
    (void)scene.drawn();
    require(fbe_gpu_counters(&counters));
    CHECK(counters.uploaded == one_record);
    CHECK(fbe_gpu_validation_errors() == 0);
    // Most of them moved send them all, once.
    for (int i = 0; i < 3000; ++i) {
        require(fbe_instances_set_one(made, i, &all[static_cast<std::size_t>(i)]));
    }
    (void)scene.drawn();
    require(fbe_gpu_counters(&counters));
    CHECK(counters.uploaded == one_record * 10000);
    fbe_material_release(paint);
}

TEST_CASE("an Instances refuses a place that isn't a number, and takes as many as it holds",
          "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t paint = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t made = cubes_at(scene.world, paint, {});
    SECTION("no number, or one going on forever, in a place, leaving what it had") {
        std::array<fbe_instance_t, 4> all{};
        all.fill(instance_at(0.0F, 0.0F, -4.0F));
        require(fbe_instances_set(made, all.data(), 4));
        (void)scene.drawn();
        all[1].matrix.m[12] = std::nanf("");
        all[2].matrix.m[13] = std::numeric_limits<float>::infinity();
        all[3].matrix.m[14] = -std::numeric_limits<float>::infinity();
        CHECK_FALSE(fbe_instances_set(made, all.data(), 4));
        CHECK(std::string_view(fbe_error()) == "instance 1's matrix isn't finite");
        CHECK_FALSE(fbe_instances_set_one(made, 2, &all[2]));
        CHECK(std::string_view(fbe_error()) == "instance 2's matrix isn't finite");
        CHECK(fbe_instances_count(made) == 4);
        for (std::int32_t i = 0; i < 4; ++i) {
            fbe_instance_t got{};
            require(fbe_instances_get(made, i, &got));
            CHECK(got.matrix.m[12] == 0.0F);
            CHECK(got.matrix.m[13] == 0.0F);
            CHECK(got.matrix.m[14] == -4.0F);
        }
        (void)scene.drawn();
        CHECK(fbe_gpu_validation_errors() == 0);
    }
    SECTION("past the most") {
        CHECK_FALSE(fbe_instances_set(made, nullptr, 4194305));
        CHECK_FALSE(fbe_instances_set(made, nullptr, -1));
        CHECK(fbe_instances_count(made) == 0);
    }
    fbe_material_release(paint);
}

TEST_CASE("an Instances' clusters draw the level their size in the view has", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_range(scene.camera, 0.5F, 1000.0F));
    const std::uint64_t paint = unlit(1.0F, 0.0F, 0.0F);
    const std::uint64_t coarse_paint = unlit(0.0F, 1.0F, 0.0F);
    const std::uint64_t fine = fbe_geometry_cube();
    const std::uint64_t coarse = fbe_geometry_cube();
    require(fbe_surface_set_material(fine, 0, paint));
    require(fbe_surface_set_material(coarse, 0, coarse_paint));
    const float half_height = std::tan(1.0471976F / 2.0F);
    const auto share_at = [&](float distance) {
        return std::numbers::sqrt3_v<float> / (half_height * distance);
    };
    require(fbe_geometry_set_lod(fine, 0, coarse, share_at(50.0F)));
    require(fbe_geometry_set_lod(fine, 1, 0, share_at(200.0F)));
    const std::uint64_t made = fbe_entity_create(scene.world, 0, FBE_ENTITY_INSTANCES);
    require(fbe_instances_set_geometry(made, fine));
    const auto group_at = [](std::vector<fbe_instance_t>& into, float z) {
        for (int i = 0; i < 128; ++i) {
            into.push_back(instance_at(static_cast<float>(i) * 0.001F, 0.0F, z));
        }
    };
    const auto geometries = [&] {
        std::array<fbe_draw_t, 4> draws{};
        const std::int32_t count =
            fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, draws.data(), 4);
        std::vector<std::uint64_t> found;
        found.reserve(static_cast<std::size_t>(count));
        for (std::int32_t i = 0; i < count; ++i) {
            found.push_back(draws[static_cast<std::size_t>(i)].geometry);
        }
        return found;
    };
    std::vector<fbe_instance_t> all;
    SECTION("near, its own; further, the coarser; past that, nothing") {
        group_at(all, -10.0F);
        require(fbe_instances_set(made, all.data(), 128));
        CHECK(geometries() == std::vector{ fine });
        CHECK(near(middle(scene.drawn()), { 255, 0, 0 }));
        require(fbe_entity_set_position(made, 0.0F, 0.0F, -90.0F, FBE_SPACE_WORLD));
        CHECK(geometries() == std::vector{ coarse });
        CHECK(near(middle(scene.drawn()), { 0, 255, 0 }));
        require(fbe_entity_set_position(made, 0.0F, 0.0F, -390.0F, FBE_SPACE_WORLD));
        CHECK(geometries().empty());
    }
    SECTION("by its cull box, if it has one, as what's moved by its shader is that big") {
        group_at(all, -90.0F);
        require(fbe_instances_set(made, all.data(), 128));
        CHECK(geometries() == std::vector{ coarse });
        // Four times its bounds each way: as big in the view as its own level asks, at 90.
        const fbe_aabb_t bigger{
            .min = { .x = -4.0F, .y = -4.0F, .z = -4.0F },
            .max = { .x = 4.0F, .y = 4.0F, .z = 4.0F },
        };
        require(fbe_geometry_set_cull_box(fine, &bigger));
        CHECK(geometries() == std::vector{ fine });
        require(fbe_geometry_clear_cull_box(fine));
        CHECK(geometries() == std::vector{ coarse });
    }
    SECTION("each cluster its own") {
        group_at(all, -10.0F);
        group_at(all, -100.0F);
        require(fbe_instances_set(made, all.data(), 256));
        const std::vector<std::uint64_t> found = geometries();
        CHECK(found.size() == 2);
        CHECK(std::ranges::count(found, fine) == 1);
        CHECK(std::ranges::count(found, coarse) == 1);
        fbe_gpu_counters_t counters{};
        (void)scene.drawn();
        require(fbe_gpu_counters(&counters));
        CHECK(counters.triangles == std::int64_t{ 128 } * 12 * 2);
    }
    SECTION("its shadow's the level's too, a Null one's none") {
        const std::uint64_t lit = fbe_material_create();
        wall(scene.world, -10.0F, 300.0F, lit);
        const std::uint64_t sun = sun_from_right(scene.world);
        require(fbe_light_set_shadow_distance(sun, 800.0F));
        require(fbe_entity_set_position(scene.camera, 0.0F, 0.0F, 300.0F, FBE_SPACE_WORLD));
        std::vector<fbe_instance_t> one{ instance_at(5.0F, 0.0F, -5.0F) };
        require(fbe_instances_set(made, one.data(), 1));
        CHECK(middle(scene.drawn())[0] != 0);
        fbe_material_release(lit);
    }
    fbe_geometry_release(fine);
    fbe_geometry_release(coarse);
    fbe_material_release(paint);
    fbe_material_release(coarse_paint);
}

TEST_CASE("an emitter's and an Instances' bounds are what they've spread to", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    const std::uint64_t made = cubes_at(scene.world, paint, { instance_at(10.0F, 0.0F, 0.0F) });
    fbe_aabb_t box{};
    require(fbe_entity_bounds(made, &box));
    CHECK(box.min.x == 9.0F);
    CHECK(box.max.x == 11.0F);
    CHECK(fbe_camera_entity_in_view(scene.camera, 128.0F, 128.0F, made) == 0);
    require(fbe_entity_set_position(scene.camera, 10.0F, 0.0F, 10.0F, FBE_SPACE_WORLD));
    CHECK(fbe_camera_entity_in_view(scene.camera, 128.0F, 128.0F, made) == 1);
    const std::uint64_t emitter = still_emitter(scene.world, 20.0F, 0.0F, 0.0F, 1.0F);
    require(fbe_particles_burst(emitter, 1));
    require(fbe_entity_bounds(emitter, &box));
    CHECK(box.max.x > 20.0F);
    CHECK(box.min.x < 20.0F);
    fbe_material_release(paint);
}

namespace {

/// The opcodes of SPIR-V this test goes by.
constexpr std::uint32_t op_entry_point = 15;
constexpr std::uint32_t op_function = 54;
constexpr std::uint32_t op_function_end = 56;
constexpr std::uint32_t op_function_call = 57;
constexpr std::uint32_t op_kill = 252;

/// Whether an opcode reads a texture by its neighbors, or asks for a derivative.
bool reads_its_neighbors(std::uint32_t op) {
    const bool implicit_sample = op == 87 || op == 89 || op == 91 || op == 93;
    const bool derivative = op >= 207 && op <= 215;
    return implicit_sample || derivative;
}

/// What walking a function finds, in its order, its calls' as they come.
struct walked_t {
    std::uint32_t before_kill = 0;
    std::uint32_t after_kill = 0;
    bool killed = false;
};

} // namespace

// Pixels a discard's already ended aren't there for their neighbors' levels and derivatives, so
// everything that needs them comes before it. It's the shader's order, which no driver's pixels
// show.
TEST_CASE("nothing in the scene's fragment shader reads by its neighbors after it discards",
          "[gpu][scene]") {
    const std::span<const std::uint32_t> code = fbe::shaders::scene.code;
    std::unordered_map<std::uint32_t, std::size_t> functions;
    std::uint32_t entry = 0;
    for (std::size_t at = 5; at < code.size(); at += code[at] >> 16U) {
        const std::uint32_t op = code[at] & 0xFFFFU;
        if (op == op_function) {
            functions[code[at + 2]] = at;
        }
        if (op == op_entry_point &&
            std::string_view(reinterpret_cast<const char*>(&code[at + 3])) == "fragmentMain") {
            entry = code[at + 2];
        }
    }
    REQUIRE(functions.contains(entry));
    walked_t walked;
    const auto walk = [&](const auto& self, std::uint32_t function) -> void {
        for (std::size_t at = functions.at(function);; at += code[at] >> 16U) {
            const std::uint32_t op = code[at] & 0xFFFFU;
            if (op == op_function_end) {
                return;
            }
            if (op == op_function_call) {
                self(self, code[at + 3]);
            } else if (op == op_kill) {
                walked.killed = true;
            } else if (reads_its_neighbors(op)) {
                (walked.killed ? walked.after_kill : walked.before_kill) += 1;
            }
        }
    };
    walk(walk, entry);
    CHECK(walked.killed);
    CHECK(walked.before_kill > 0);
    CHECK(walked.after_kill == 0);
}

namespace {

/// A wall at the camera's middle, `half` across at 3 away, of light as bright as it says, on black,
/// and an HDR camera that clears to black.
std::uint64_t glowing_square(const scene_t& scene, float light, float half) {
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_emissive(material, light, light, light));
    wall(scene.world, -3.0F, half, material);
    require(fbe_camera_set_hdr(scene.camera, true));
    return material;
}

/// What light a stored byte is, in a target of sRGB.
float linear_of(std::uint8_t stored_byte) {
    const float encoded = static_cast<float>(stored_byte) / 255.0F;
    return encoded <= 0.04045F ? encoded / 12.92F : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
}

} // namespace

TEST_CASE("a camera's bloom and grade settings are what they're set to", "[gpu][scene]") {
    const scene_t scene;
    CHECK(fbe_camera_bloom_intensity(scene.camera) == 0.0F);
    CHECK(fbe_camera_bloom_threshold(scene.camera) == 1.0F);
    CHECK(fbe_camera_set_bloom(scene.camera, 0.5F, 2.0F));
    CHECK(fbe_camera_bloom_intensity(scene.camera) == 0.5F);
    CHECK(fbe_camera_bloom_threshold(scene.camera) == 2.0F);
    CHECK(!fbe_camera_set_bloom(scene.camera, -1.0F, 1.0F));
    CHECK(!fbe_camera_set_bloom(scene.camera, 1.0F, -1.0F));
    CHECK(!fbe_camera_set_bloom(scene.camera, std::nanf(""), 1.0F));
    CHECK(fbe_camera_bloom_intensity(scene.camera) == 0.5F);
    std::array<float, 10> grade{};
    CHECK(fbe_camera_grade(scene.camera, grade.data()));
    CHECK(grade == std::array{ 1.0F, 1.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F, 1.0F });
    const std::array wanted{ 2.0F, 1.0F, 0.5F, 0.1F, 0.0F, -0.1F, 1.0F, 2.0F, 0.5F, 0.5F };
    CHECK(fbe_camera_set_grade(scene.camera, wanted.data()));
    CHECK(fbe_camera_grade(scene.camera, grade.data()));
    CHECK(grade == wanted);
    for (const std::size_t at : { std::size_t{ 0 }, std::size_t{ 6 }, std::size_t{ 9 } }) {
        std::array bad = wanted;
        bad.at(at) = -1.0F;
        CHECK(!fbe_camera_set_grade(scene.camera, bad.data()));
        bad.at(at) = std::nanf("");
        CHECK(!fbe_camera_set_grade(scene.camera, bad.data()));
    }
    for (const float wrong : { std::nanf(""), std::numeric_limits<float>::infinity() }) {
        std::array bad = wanted;
        bad[3] = wrong;
        CHECK(!fbe_camera_set_grade(scene.camera, bad.data()));
        CHECK(!fbe_camera_set_bloom(scene.camera, wrong, 1.0F));
        CHECK(!fbe_camera_set_bloom(scene.camera, 1.0F, wrong));
    }
    std::array no_power = wanted;
    no_power[7] = 0.0F;
    CHECK(!fbe_camera_set_grade(scene.camera, no_power.data()));
    CHECK(fbe_camera_grade(scene.camera, grade.data()));
    CHECK(grade == wanted);
}

// Light that's even all over stays what it is through every level, so it's what its formula says:
// nothing below its threshold's knee, and all that's past its threshold above it, added.
TEST_CASE("bloom adds what's past its threshold", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = glowing_wall(scene, 0.25F, 0.25F, 0.25F);
    require(fbe_camera_set_bloom(scene.camera, 1.0F, 1.0F));
    // Below the knee, at 0.5 of the threshold: none of it.
    const std::uint8_t quarter = srgb(0.25F);
    CHECK(near(middle(scene.drawn()), { quarter, quarter, quarter }, 1));
    // Past it: 2 is 1 past 1, half of it, so 2 + 1 of light, at a quarter's exposure, 0.75.
    require(fbe_material_set_emissive(material, 2.0F, 2.0F, 2.0F));
    require(fbe_camera_set_exposure(scene.camera, 0.25F));
    const std::uint8_t lit = srgb(0.75F);
    CHECK(near(middle(scene.drawn()), { lit, lit, lit }, 2));
    // Half the intensity adds half.
    require(fbe_camera_set_bloom(scene.camera, 0.5F, 1.0F));
    const std::uint8_t less = srgb(0.625F);
    CHECK(near(middle(scene.drawn()), { less, less, less }, 2));
    // In the knee, at 0.75 of it: a little, (0.25 ^ 2 / 2) / 0.75 of 0.75.
    require(fbe_camera_set_exposure(scene.camera, 1.0F));
    require(fbe_camera_set_bloom(scene.camera, 1.0F, 1.0F));
    require(fbe_material_set_emissive(material, 0.75F, 0.75F, 0.75F));
    const std::uint8_t knee = srgb(0.75F + 0.03125F);
    CHECK(near(middle(scene.drawn()), { knee, knee, knee }, 2));
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(material);
}

// A bright square glows past its edges, less the further, the same each way, and nothing's added
// where there's no bloom.
TEST_CASE("bloom spreads a bright square's light around it", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = glowing_square(scene, 16.0F, 0.2F);
    require(fbe_camera_set_exposure(scene.camera, 0.25F));
    const fbe::golden::frame_t sharp = scene.drawn();
    constexpr std::uint32_t mid = frame_size / 2;
    CHECK(pixel(sharp, mid + 16, mid)[0] == 0);
    require(fbe_camera_set_bloom(scene.camera, 1.0F, 1.0F));
    const fbe::golden::frame_t glowing = scene.drawn();
    const auto red = [&glowing](std::uint32_t x, std::uint32_t y) {
        return pixel(glowing, x, y)[0];
    };
    CHECK(red(mid + 12, mid) > red(mid + 24, mid));
    CHECK(red(mid + 24, mid) > red(mid + 48, mid));
    CHECK(red(mid + 24, mid) > 0);
    CHECK(std::abs(red(mid + 24, mid) - red(mid - 25, mid)) <= 6);
    CHECK(std::abs(red(mid, mid + 24) - red(mid, mid - 25)) <= 6);
    CHECK(std::abs(red(mid + 24, mid) - red(mid, mid + 24)) <= 6);
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(material);
}

// One pixel a million times too bright, a firefly, comes to little more than its neighbors do, for
// Karis's average; and a NaN or an infinity in the image blooms nothing, rather than all of it.
TEST_CASE("a firefly doesn't blow bloom out", "[gpu][scene]") {
    const scene_t scene;
    // A wall on black, 2 across at 3 away, each of its map's texels under a pixel's width, nearest,
    // so one hot texel's one pixel, as bright as a half float goes.
    const std::uint64_t material = glowing_square(scene, 1.0F, 1.0F);
    constexpr std::int32_t across = 128;
    const std::uint64_t hot = fbe_texture_create(across, across, FBE_TEXTURE_RGBA16F, false);
    require(hot != 0);
    std::vector<float> texels(std::size_t{ across } * across * 4, 0.0F);
    for (std::size_t at = 3; at < texels.size(); at += 4) {
        texels[at] = 1.0F;
    }
    const std::size_t one = ((std::size_t{ 64 } * across) + 64) * 4;
    texels[one] = 60000.0F;
    texels[one + 1] = 60000.0F;
    texels[one + 2] = 60000.0F;
    require(fbe_texture_write_floats(hot, 0, 0, across, across, texels.data()));
    require(fbe_texture_set_filter(hot, FBE_TEXTURE_NEAREST));
    require(fbe_material_set_map(material, FBE_MAP_EMISSIVE, hot, 0));
    require(fbe_camera_set_exposure(scene.camera, 0.1F));
    require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_NONE));
    require(fbe_camera_set_bloom(scene.camera, 1.0F, 1.0F));
    const fbe::golden::frame_t frame = scene.drawn();
    constexpr std::uint32_t mid = frame_size / 2;
    CHECK(pixel(frame, mid, mid)[0] == 255);
    // Without Karis's average, 60000 through a few levels' weights would be hundreds, all white.
    // With it, one sample of a group of four comes to about a third, a tenth of it here.
    CHECK(linear_of(pixel(frame, mid + 8, mid)[0]) < 0.05F);
    require(fbe_material_set_map(material, FBE_MAP_EMISSIVE, 0, 0));
    fbe_texture_release(hot);
    const std::uint64_t nan_map = fbe_texture_create(2, 2, FBE_TEXTURE_RGBA16F, false);
    require(nan_map != 0);
    std::array<std::uint16_t, 16> halves{};
    for (std::size_t at = 0; at < 4; ++at) {
        halves.at(at * 4) = 0x7E00;
        halves.at((at * 4) + 1) = 0x7E00;
        halves.at((at * 4) + 2) = 0x7E00;
        halves.at((at * 4) + 3) = 0x3C00;
    }
    std::array<std::uint8_t, 32> bytes{};
    std::memcpy(bytes.data(), halves.data(), bytes.size());
    require(fbe_texture_write(nan_map, 0, 0, 2, 2, bytes.data(), 16));
    require(fbe_texture_set_filter(nan_map, FBE_TEXTURE_NEAREST));
    require(fbe_material_set_map(material, FBE_MAP_EMISSIVE, nan_map, 0));
    const fbe::golden::frame_t none = scene.drawn();
    CHECK(pixel(none, mid + 8, mid)[0] == 0);
    CHECK(pixel(none, mid, mid)[0] == 0);
    fbe_texture_release(nan_map);
    fbe_material_release(material);
}

// Each of a color decision list's parts, by its formula, in order, with gamma after.
TEST_CASE("a color grade's what its formula says", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = glowing_wall(scene, 0.5F, 0.25F, 0.1F);
    const auto seen = [&](const std::array<float, 10>& grade) {
        require(fbe_camera_set_grade(scene.camera, grade.data()));
        return middle(scene.drawn());
    };
    CHECK(near(seen({ 1, 1, 1, 0, 0, 0, 1, 1, 1, 1 }), { srgb(0.5F), srgb(0.25F), srgb(0.1F) }, 1));
    CHECK(near(seen({ 2, 1, 0.5F, 0, 0, 0, 1, 1, 1, 1 }), { srgb(1.0F), srgb(0.25F), srgb(0.05F) },
               1));
    CHECK(near(seen({ 1, 1, 1, 0.1F, -0.1F, 0, 1, 1, 1, 1 }),
               { srgb(0.6F), srgb(0.15F), srgb(0.1F) }, 1));
    CHECK(near(seen({ 1, 1, 1, 0, 0, 0, 2, 1, 0.5F, 1 }),
               { srgb(0.25F), srgb(0.25F), srgb(std::sqrt(0.1F)) }, 1));
    // No saturation is its luma, all of it gray.
    const float luma = (0.5F * 0.2126F) + (0.25F * 0.7152F) + (0.1F * 0.0722F);
    CHECK(near(seen({ 1, 1, 1, 0, 0, 0, 1, 1, 1, 0 }), { srgb(luma), srgb(luma), srgb(luma) }, 1));
    // Slope, then offset, then power, then saturation; each after the last.
    const float slope_offset = (0.5F * 2.0F) + 0.0F;
    CHECK(near(seen({ 2, 2, 2, -0.5F, -0.25F, 0, 2, 2, 2, 1 }),
               { srgb((slope_offset - 0.5F) * (slope_offset - 0.5F)), srgb(0.0625F), srgb(0.04F) },
               1));
    // What an offset takes below 0 has no power to go through: it's 0, not what 0.1 squared is.
    CHECK(near(seen({ 1, 1, 1, -0.6F, 0, 0, 2, 1, 1, 1 }), { 0, srgb(0.25F), srgb(0.1F) }, 1));
    // Not HDR, it isn't there.
    require(fbe_camera_set_hdr(scene.camera, false));
    CHECK(near(seen({ 2, 2, 2, 0, 0, 0, 1, 1, 1, 1 }), { srgb(0.5F), srgb(0.25F), srgb(0.1F) }, 1));
    fbe_material_release(material);
}

namespace {

/// A camera that looks away from everything and clears the target to a color: what's under what an
/// HDR camera that doesn't clear draws.
void under_it(const scene_t& scene, float r, float g, float b, float a = 1.0F) {
    const std::uint64_t under = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
    require(fbe_camera_set_target(under, scene.target));
    require(fbe_camera_set_order(under, -1));
    require(fbe_camera_set_clear_color(under, r, g, b, a));
    require(fbe_entity_set_rotation(under, 0.0F, 1.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    require(fbe_camera_set_clear(scene.camera, false, true));
}

/// What a light's channel comes to through a tonemap.
float through(std::int32_t tonemap, float light) {
    switch (tonemap) {
    case FBE_TONEMAP_REINHARD:
        return light / (light + 1.0F);
    case FBE_TONEMAP_ACES:
        return aces(light);
    case FBE_TONEMAP_NEUTRAL:
        return neutral(light);
    default:
        // What's stored clamps it, after it's blended.
        return light;
    }
}

/// A material half see-through, of light as it says, its alpha by blending, emissive so nothing
/// lights it.
std::uint64_t half_clear(float light) {
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_color(material, 0.0F, 0.0F, 0.0F, 0.5F));
    require(fbe_material_set_blend(material, FBE_BLEND_ALPHA));
    require(fbe_material_set_emissive(material, light, light, light));
    return material;
}

} // namespace

// F1: a curve's of a color, and what's in a camera's image of what's half see-through is that color
// times its alpha, which has to be undone first: T(L) a + (1 - a) under, not T(L a) + (1 - a)
// under.
TEST_CASE("an HDR camera's curves are of a color, not of it times its alpha", "[gpu][scene]") {
    const scene_t scene;
    under_it(scene, 0.2F, 0.4F, 0.1F);
    constexpr float light_of_it = 10.0F;
    const std::uint64_t material = half_clear(light_of_it);
    // It's emissive with its alpha: blending by it, it comes to L a in the image, as the BRDF says
    // nothing else.
    wall(scene.world, -3.0F, 10.0F, material);
    require(fbe_camera_set_hdr(scene.camera, true));
    require(fbe_camera_set_gamma(scene.camera, 2.0F));
    for (const std::int32_t tonemap :
         { FBE_TONEMAP_NONE, FBE_TONEMAP_REINHARD, FBE_TONEMAP_ACES, FBE_TONEMAP_NEUTRAL }) {
        INFO("tonemap " << tonemap);
        require(fbe_camera_set_tonemap(scene.camera, tonemap));
        const float over = std::sqrt(through(tonemap, light_of_it)) * 0.5F;
        CHECK(near(middle(scene.drawn()),
                   { srgb(over + 0.1F), srgb(over + 0.2F), srgb(over + 0.05F) }, 3));
    }
    fbe_material_release(material);
}

// F1: an edge's pixel, in a target of four samples, is its coverage of what's covered.
TEST_CASE("an HDR camera's edge is its coverage of a tonemap's color", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t target = fbe_texture_create_target(32, 32, FBE_TEXTURE_RGBA8, 4, true);
    require(target != 0);
    require(fbe_camera_set_target(scene.camera, target));
    require(fbe_camera_set_clear(scene.camera, true, true));
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 0.0F));
    constexpr float light_of_it = 10.0F;
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_emissive(material, light_of_it, light_of_it, light_of_it));
    const std::uint64_t mesh = wall(scene.world, -3.0F, 0.5F, material);
    require(fbe_entity_set_rotation(mesh, 0.0F, 0.0F, 0.2F, 0.98F, FBE_SPACE_WORLD));
    require(fbe_camera_set_hdr(scene.camera, true));
    require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_REINHARD));
    require(fbe_render_world(scene.world, 1.0F));
    const fbe::golden::frame_t frame = scene_t::read(target);
    const float whole_of_it = through(FBE_TONEMAP_REINHARD, light_of_it);
    int partly = 0;
    for (std::uint32_t y = 0; y < 32; ++y) {
        for (std::uint32_t x = 0; x < 32; ++x) {
            const float found = linear_of(pixel(frame, x, y)[0]);
            if (found <= 0.01F || found >= whole_of_it - 0.02F) {
                continue;
            }
            ++partly;
            const float covered = found / whole_of_it;
            const float nearest = std::round(covered * 4.0F) / 4.0F;
            INFO(x << ", " << y << ": " << found);
            CHECK(std::abs(covered - nearest) < 0.04F);
        }
    }
    CHECK(partly > 4);
    require(fbe_camera_set_target(scene.camera, scene.target));
    fbe_texture_release(target);
    fbe_material_release(material);
}

namespace {

/// A little turned wall with a light of 100, whose tonemap's near 1, in an HDR camera that clears
/// to alpha 1, where what's in a pixel is all surface however little of it's the wall's.
void bright_wall(const scene_t& scene, float clear_alpha = 1.0F) {
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_emissive(material, 100.0F, 100.0F, 100.0F));
    const std::uint64_t mesh = wall(scene.world, -3.0F, 0.5F, material);
    require(fbe_entity_set_rotation(mesh, 0.0F, 0.0F, 0.2F, 0.98F, FBE_SPACE_WORLD));
    fbe_material_release(material);
    require(fbe_camera_set_clear(scene.camera, true, true));
    // Over no alpha, an edge's alpha is what of its samples it covers; and what it tonemaps is
    // its color under its alpha, which is the wall's, whatever reads it.
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, clear_alpha));
    require(fbe_camera_set_hdr(scene.camera, true));
    require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_REINHARD));
}

/// How many pixels of a wall's edge are one, two, and three of four samples of it, each its part of
/// the tonemap's color; and how many are none of those.
/// How many pixels of an edge are each part of `samples` of it, but none or all; how many are
/// between those; and how many of the parts have an alpha that isn't their part's.
struct samples_edge_t {
    std::vector<int> parts;
    int others = 0;
    int alpha_off = 0;
};

/// What's in a frame of `side` pixels of an edge of `samples` over what's `behind` of a tonemap's
/// color, of what's `whole` of it; and, over what has no alpha, its alpha, which is the samples
/// it covers.
samples_edge_t edge_in(const fbe::golden::frame_t& frame, std::uint32_t side, float behind,
                       float whole, std::uint32_t samples = 4, bool alpha_is_coverage = false) {
    samples_edge_t edge;
    edge.parts.assign(samples - 1, 0);
    const auto many = static_cast<float>(samples);
    for (std::uint32_t y = 0; y < side; ++y) {
        for (std::uint32_t x = 0; x < side; ++x) {
            const float part = (linear_of(pixel(frame, x, y)[0]) - behind) / (whole - behind);
            if (part <= 0.02F || part >= 0.98F) {
                continue;
            }
            const float nearest = std::round(part * many);
            if (std::abs(part - (nearest / many)) < 0.03F && nearest >= 1.0F &&
                nearest <= many - 1.0F) {
                ++edge.parts.at(static_cast<std::size_t>(nearest) - 1);
                const float alpha = std::round(nearest / many * 255.0F);
                if (alpha_is_coverage &&
                    std::abs(static_cast<float>(pixel(frame, x, y)[3]) - alpha) > 3.0F) {
                    ++edge.alpha_off;
                }
            } else {
                ++edge.others;
            }
        }
    }
    return edge;
}

/// The edge in a new target of `samples` and `side` pixels, over what's behind it that's `behind`
/// of a tonemap's color; its alpha's checked if `alpha_is_coverage`, where what's behind has none.
samples_edge_t edge_of(const scene_t& scene, std::uint32_t side, float behind = 0.0F,
                       std::uint32_t samples = 4, bool alpha_is_coverage = false) {
    const std::uint64_t target =
        fbe_texture_create_target(static_cast<std::int32_t>(side), static_cast<std::int32_t>(side),
                                  FBE_TEXTURE_RGBA8, static_cast<std::int32_t>(samples), true);
    require(target != 0);
    require(fbe_camera_set_target(scene.camera, target));
    require(fbe_render_world(scene.world, 1.0F));
    const fbe::golden::frame_t frame = scene_t::read(target);
    fbe_frame_flip(false);
    require(fbe_camera_set_target(scene.camera, scene.target));
    fbe_texture_release(target);
    return edge_in(frame, side, behind, through(FBE_TONEMAP_REINHARD, 100.0F), samples,
                   alpha_is_coverage);
}

void reads_its_samples(const samples_edge_t& edge) {
    CAPTURE(edge.parts, edge.others, edge.alpha_off);
    CHECK(edge.others == 0);
    CHECK(std::ranges::all_of(edge.parts, [](int many) { return many > 0; }));
    CHECK(edge.alpha_off == 0);
}

/// What their average's tonemap is: a quarter of the wall's light is nearly all of its color, which
/// is no part of it a sample has.
void reads_their_average(const samples_edge_t& edge) {
    CAPTURE(edge.parts, edge.others);
    CHECK(edge.others > 0);
    CHECK(std::ranges::all_of(edge.parts, [](int many) { return many == 0; }));
}

/// A pass of the shape shader's, of a light of 100 over no light at all, into an image of
/// `samples`.
fbe::gpu::resource_t bright_image(std::uint32_t side, std::uint32_t samples) {
    const fbe::gpu::transient_t made{
        .width = side,
        .height = side,
        .format = FBE_TEXTURE_RGBA16F,
        .samples = samples,
        .reads_samples = samples > 1,
    };
    require(fbe::gpu::reserve_transient(made));
    const fbe::gpu::resource_t image = fbe::gpu::transient_resource(made);
    const auto drawn_as = static_cast<VkSampleCountFlagBits>(samples);
    require(fbe::gpu::add_pass({
        .name = "bright",
        .colors = { {
            .target = image,
            .clear = true,
            .rgba = { 0.0F, 0.0F, 0.0F, 1.0F },
        }, },
        .record =
            [drawn_as](VkCommandBuffer commands) {
                VkPipeline pipeline = fbe::gpu::pipeline({
                    .shader = &fbe::test_shaders::shape,
                    .color = VK_FORMAT_R16G16B16A16_SFLOAT,
                    .samples = drawn_as,
                });
                require(pipeline != VK_NULL_HANDLE);
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                const struct {
                    std::array<float, 4> color;
                    std::array<float, 2> offset;
                    float size;
                    float depth;
                } constants{
                    .color = { 100.0F, 100.0F, 100.0F, 1.0F },
                    .offset = {},
                    .size = 0.8F,
                    .depth = 0.5F,
                };
                vkCmdPushConstants(commands, fbe::gpu::pipeline_layout(),
                                   fbe::gpu::flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL), 0,
                                   sizeof(constants), &constants);
                vkCmdDraw(commands, 3, 1, 0, 0);
            },
    }));
    return image;
}

} // namespace

namespace {

/// What's behind an edge: a wall of `dim` light, none if 0, and the clear's alpha.
struct behind_t {
    float dim = 0.0F;
    float alpha = 1.0F;
};

/// That a bright edge of `samples` is the average of each sample's tonemap, over black, over a
/// dim wall, and over no alpha, where its alpha's what of it each covers.
void bright_edge_at(std::uint32_t samples) {
    const behind_t behind =
        GENERATE(behind_t{}, behind_t{ .dim = 0.5F }, behind_t{ .alpha = 0.0F });
    const float dim = behind.dim;
    CAPTURE(dim, behind.alpha, samples);
    const scene_t scene;
    bright_wall(scene, behind.alpha);
    if (dim > 0.0F) {
        const std::uint64_t dim_material = fbe_material_create();
        require(fbe_material_set_emissive(dim_material, dim, dim, dim));
        wall(scene.world, -6.0F, 20.0F, dim_material);
        fbe_material_release(dim_material);
    }
    const float under = through(FBE_TONEMAP_REINHARD, dim);
    const samples_edge_t edge = edge_of(scene, 32, under, samples, behind.alpha == 0.0F);
    reads_its_samples(edge);
    // Its image again, from how the last frame left it.
    const samples_edge_t again = edge_of(scene, 32, under, samples, behind.alpha == 0.0F);
    CHECK(again.parts == edge.parts);
    CHECK(again.others == 0);
    CHECK(fbe_gpu_validation_errors() == 0);
}

} // namespace

// F6: a tonemap of what's resolved undoes multisampling on a bright edge: a quarter of a light of
// 100 is 25, nearly all of what a tonemap makes of 100. Every GPU draws and reads 4 samples.
TEST_CASE("a bright edge is the average of each sample's tonemap", "[gpu][scene]") {
    bright_edge_at(4);
}

// Of a test of its own, so a GPU without 2, as lavapipe is, skips it, and MoltenVK's log says it
// ran.
TEST_CASE("a bright edge of 2 samples is their average's tonemap too", "[gpu][scene]") {
    bool has_two = false;
    {
        const scene_t scene;
        const VkPhysicalDeviceLimits& limits = fbe::gpu::device()->properties.limits;
        has_two = (limits.framebufferColorSampleCounts & limits.sampledImageColorSampleCounts &
                   static_cast<VkSampleCountFlags>(VK_SAMPLE_COUNT_2_BIT)) != 0;
    }
    if (!has_two) {
        SKIP("this GPU doesn't draw and read 2 samples");
    }
    bright_edge_at(2);
}

// What's all of a pixel has no edge: each of its samples has the bloom a single sample would, on
// the wall, off it where there's a surface to it, and off it where there's none.
TEST_CASE("bloom's on every sample of a multisampled camera", "[gpu][scene]") {
    const float clear_alpha = GENERATE(1.0F, 0.0F);
    CAPTURE(clear_alpha);
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_emissive(material, 20.0F, 20.0F, 20.0F));
    wall(scene.world, -3.0F, 0.5F, material);
    fbe_material_release(material);
    require(fbe_camera_set_clear(scene.camera, true, true));
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, clear_alpha));
    require(fbe_camera_set_hdr(scene.camera, true));
    require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_REINHARD));
    require(fbe_camera_set_bloom(scene.camera, 1.0F, 1.0F));
    const auto drawn_at = [&scene](std::int32_t samples) {
        const std::uint64_t target =
            fbe_texture_create_target(frame_size, frame_size, FBE_TEXTURE_RGBA8, samples, true);
        require(target != 0);
        require(fbe_camera_set_target(scene.camera, target));
        require(fbe_render_world(scene.world, 1.0F));
        fbe::golden::frame_t frame = scene_t::read(target);
        fbe_frame_flip(false);
        require(fbe_camera_set_target(scene.camera, scene.target));
        fbe_texture_release(target);
        return frame;
    };
    const fbe::golden::frame_t one = drawn_at(1);
    const fbe::golden::frame_t four = drawn_at(4);
    const std::uint32_t mid = frame_size / 2;
    const std::uint32_t off = frame_size / 8;
    const rgba_t glowing = pixel(one, off, mid);
    CAPTURE(glowing[0], pixel(four, off, mid)[0]);
    CHECK(glowing[0] > 40);
    // Near, not the same: what bloom starts from is what they resolve to, whose edges are part
    // covered where one sample's are all or nothing.
    CHECK(near(pixel(four, off, mid), { glowing[0], glowing[1], glowing[2] }, 6));
    const rgba_t on_it = pixel(one, mid, mid);
    CHECK(near(pixel(four, mid, mid), { on_it[0], on_it[1], on_it[2] }, 3));
    CHECK(pixel(four, off, mid)[3] == pixel(one, off, mid)[3]);
    CHECK(fbe_gpu_validation_errors() == 0);
}

// As what's drawn after it, as a camera's passes, can have it draw into what has no samples.
TEST_CASE("a tonemap reads the samples its image has, whatever its target draws", "[gpu][scene]") {
    const scene_t scene;
    constexpr std::uint32_t side = 32;
    fbe::scene::camera_t camera;
    camera.tonemap = FBE_TONEMAP_REINHARD;
    const VkRect2D all{ .offset = {}, .extent = { .width = side, .height = side } };
    const float whole = through(FBE_TONEMAP_REINHARD, 100.0F);
    // A target of none, an image of 4.
    const std::uint64_t flat = fbe_texture_create_target(side, side, FBE_TEXTURE_RGBA8, 1, false);
    require(flat != 0);
    require(fbe::render::add_post_passes(camera, bright_image(side, 4),
                                         fbe::gpu::texture_resource(flat), all.extent,
                                         VK_FORMAT_R8G8B8A8_SRGB, VK_SAMPLE_COUNT_1_BIT, all));
    reads_its_samples(edge_in(scene_t::read(flat), side, 0.0F, whole));
    // A target of 4, an image of none.
    const std::uint64_t four = fbe_texture_create_target(side, side, FBE_TEXTURE_RGBA8, 4, false);
    require(four != 0);
    require(fbe::render::add_post_passes(camera, bright_image(side, 1),
                                         fbe::gpu::texture_resource(four), all.extent,
                                         VK_FORMAT_R8G8B8A8_SRGB, VK_SAMPLE_COUNT_4_BIT, all));
    const fbe::golden::frame_t aliased = scene_t::read(four);
    CHECK(linear_of(pixel(aliased, side / 2, side / 2)[0]) == Catch::Approx(whole).margin(0.01F));
    CHECK(edge_in(aliased, side, 0.0F, whole).others == 0);
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_texture_release(flat);
    fbe_texture_release(four);
}

// Their layout, which a software GPU draws the same in any of, and validation can't see through a
// bindless set: what reads samples has them moved for it, and what can't read them leaves them.
TEST_CASE("a tonemap's samples are moved for it, where it reads them", "[gpu][scene]") {
    const scene_t scene;
    fbe::scene::camera_t camera;
    camera.tonemap = FBE_TONEMAP_REINHARD;
    const auto barriers_at = [&camera](std::uint32_t side) {
        const std::uint64_t flat =
            fbe_texture_create_target(static_cast<std::int32_t>(side),
                                      static_cast<std::int32_t>(side), FBE_TEXTURE_RGBA8, 1, false);
        require(flat != 0);
        const VkRect2D all{ .offset = {}, .extent = { .width = side, .height = side } };
        require(fbe::render::add_post_passes(camera, bright_image(side, 4),
                                             fbe::gpu::texture_resource(flat), all.extent,
                                             VK_FORMAT_R8G8B8A8_SRGB, VK_SAMPLE_COUNT_1_BIT, all));
        require(fbe::gpu::finish_frame());
        const auto barriers = fbe::gpu::last_recorded().barriers;
        fbe_texture_release(flat);
        return barriers;
    };
    const auto read = barriers_at(32);
    fbe::gpu::inject(fbe::gpu::fault_t::NO_READ_SAMPLES);
    const auto unread = barriers_at(33);
    CHECK(read == unread + 1);
    CHECK(fbe_gpu_validation_errors() == 0);
}

// F6's review: only what reads its samples takes a slot for them, as there are only so many.
TEST_CASE("a camera that doesn't read its image's samples takes no slot for them", "[gpu][scene]") {
    const scene_t scene;
    bright_wall(scene);
    const std::uint32_t free = fbe::gpu::free_samples_slot_count();
    const auto slots_drawn = [&scene] {
        const std::uint64_t target = fbe_texture_create_target(32, 32, FBE_TEXTURE_RGBA8, 4, true);
        require(target != 0);
        require(fbe_camera_set_target(scene.camera, target));
        require(fbe_render_world(scene.world, 1.0F));
        fbe_frame_flip(false);
        const std::uint32_t taken = fbe::gpu::free_samples_slot_count();
        require(fbe_camera_set_target(scene.camera, scene.target));
        fbe_texture_release(target);
        return taken;
    };
    // Temporal antialiasing reads what they resolve to. First, as an image that read them would be
    // the one the HDR camera's tonemap had, kept for its next frame, slot and all.
    require(fbe_camera_set_temporal_aa(scene.camera, true));
    CHECK(slots_drawn() == free);
    // An HDR camera's tonemap reads them.
    require(fbe_camera_set_temporal_aa(scene.camera, false));
    CHECK(slots_drawn() == free - 1);
    // FXAA, without HDR, reads what's tonemapped, taking none more than the HDR camera's.
    require(fbe_camera_set_hdr(scene.camera, false));
    require(fbe_camera_set_fxaa(scene.camera, true));
    CHECK(slots_drawn() == free - 1);
    CHECK(fbe_gpu_validation_errors() == 0);
}

// F6's review: a free slot points at a multisampled image that's there, so a shader that read one
// would read that, not what was freed.
TEST_CASE("a free slot for samples points at a placeholder, not what was freed", "[gpu][scene]") {
    const scene_t scene;
    auto* const placeholder = fbe::gpu::free_samples_view();
    REQUIRE(placeholder != VK_NULL_HANDLE);
    const auto pointing_elsewhere = [placeholder] {
        int elsewhere = 0;
        for (std::uint32_t slot = 0; slot < fbe::gpu::samples_slots; ++slot) {
            elsewhere += fbe::gpu::samples_slot_view(slot) != placeholder ? 1 : 0;
        }
        return elsewhere;
    };
    CHECK(pointing_elsewhere() == 0);
    bright_wall(scene);
    reads_its_samples(edge_of(scene, 32));
    CHECK(pointing_elsewhere() == 1);
    // Once the camera's image isn't asked for, frames on, it goes, and its slot points at the
    // placeholder.
    for (int frames = 0; frames < 10; ++frames) {
        require(fbe_render_world(scene.world, 1.0F));
        fbe_frame_flip(false);
    }
    CHECK(fbe::gpu::free_samples_slot_count() == fbe::gpu::samples_slots);
    CHECK(pointing_elsewhere() == 0);
    CHECK(fbe_gpu_validation_errors() == 0);
}

TEST_CASE("what can't read a camera's samples reads what they resolve to", "[gpu][scene]") {
    const scene_t scene;
    bright_wall(scene);
    fbe::gpu::inject(fbe::gpu::fault_t::NO_READ_SAMPLES);
    reads_their_average(edge_of(scene, 32));
    CHECK(fbe_gpu_validation_errors() == 0);
}

TEST_CASE("a camera past the set's samples reads what they resolve to, until there's room",
          "[gpu][scene]") {
    // A GPU started again has as many as the first had.
    { const scene_t before; }
    const scene_t scene;
    bright_wall(scene);
    // A game's own targets take none of them.
    std::vector<std::uint64_t> own;
    for (std::uint32_t at = 0; at < fbe::gpu::samples_slots; ++at) {
        own.push_back(fbe_texture_create_target(8, 8, FBE_TEXTURE_RGBA8, 4, false));
        require(own.back() != 0);
    }
    reads_its_samples(edge_of(scene, 32));
    for (const std::uint64_t target : own) {
        fbe_texture_release(target);
    }
    for (std::uint32_t at = 0; at < fbe::gpu::samples_slots; ++at) {
        require(fbe::gpu::reserve_transient({
            .width = 8 + at,
            .height = 8,
            .format = FBE_TEXTURE_RGBA16F,
            .samples = 4,
            .reads_samples = true,
        }));
    }
    reads_their_average(edge_of(scene, 33));
    CHECK(fbe_gpu_validation_errors() == 0);
    // Those nothing's asked for go, and what comes after has their room.
    for (int frames = 0; frames < 10; ++frames) {
        fbe_frame_flip(false);
    }
    reads_its_samples(edge_of(scene, 32));
    CHECK(fbe_gpu_validation_errors() == 0);
}

// F2: what multiplies what's behind it has nothing behind it in the image, so it draws nothing.
TEST_CASE("what multiplies, in an HDR camera that doesn't clear, draws nothing", "[gpu][scene]") {
    const scene_t scene;
    under_it(scene, 0.0F, 0.0F, 1.0F);
    const std::uint64_t material = unlit(0.5F, 0.5F, 0.5F);
    require(fbe_material_set_blend(material, FBE_BLEND_MULTIPLY));
    wall(scene.world, -3.0F, 10.0F, material);
    require(fbe_camera_set_hdr(scene.camera, true));
    CHECK(near(middle(scene.drawn()), { 0, 0, 255 }));
    fbe_material_release(material);
}

// F3: past what a float holds, through every curve: as white as it gets, and as finite in a float
// target; and a gamma too small to take one over isn't one.
TEST_CASE("no exposure's so great a curve makes nothing of it", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = glowing_wall(scene, 2.0F, 2.0F, 2.0F);
    for (const float exposure : { 1e30F, std::numeric_limits<float>::max() }) {
        require(fbe_camera_set_exposure(scene.camera, exposure));
        for (const std::int32_t tonemap :
             { FBE_TONEMAP_NONE, FBE_TONEMAP_REINHARD, FBE_TONEMAP_ACES, FBE_TONEMAP_NEUTRAL }) {
            INFO("exposure " << exposure << ", tonemap " << tonemap);
            require(fbe_camera_set_tonemap(scene.camera, tonemap));
            CHECK(near(middle(scene.drawn()), { 255, 255, 255 }));
        }
    }
    const std::uint64_t floats = fbe_texture_create_target(8, 8, FBE_TEXTURE_RGBA16F, 1, true);
    require(floats != 0);
    require(fbe_camera_set_target(scene.camera, floats));
    require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_NONE));
    const std::array<float, 3> held = middle_light(scene, floats);
    CHECK(std::isfinite(held[0]));
    // As much as a half float holds, but for pow's rounding at the very top of it.
    CHECK(held[0] >= 65000.0F);
    require(fbe_camera_set_target(scene.camera, scene.target));
    fbe_texture_release(floats);
    CHECK(!fbe_camera_set_gamma(scene.camera, 1e-40F));
    CHECK(fbe_camera_gamma(scene.camera) == 1.0F);
    fbe_material_release(material);
}

// F4: an alpha past 1 is 1, as bytes have it, HDR or not, into bytes or floats; so a blend doesn't
// go negative.
TEST_CASE("an alpha past 1 draws the same in an HDR camera", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.5F, 0.0F, 1.0F));
    const std::uint64_t material = unlit(0.5F, 0.0F, 0.0F);
    require(fbe_material_set_blend(material, FBE_BLEND_ALPHA));
    require(fbe_material_set_vertex_colors(material, true));
    const std::uint64_t mesh = wall(scene.world, -3.0F, 10.0F, material);
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    std::array<fbe_vertex_t, 4> vertices{};
    require(fbe_surface_vertices(geometry, 0, 0, vertices.data(), 4));
    for (fbe_vertex_t& vertex : vertices) {
        vertex.color = { .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 2.0F };
    }
    require(fbe_surface_set_vertices(geometry, 0, 0, vertices.data(), 4));
    const rgba_t bytes = middle(scene.drawn());
    CHECK(near(bytes, { srgb(0.5F), 0, 0 }, 2));
    require(fbe_camera_set_hdr(scene.camera, true));
    CHECK(near(middle(scene.drawn()), { bytes[0], bytes[1], bytes[2] }, 1));
    const std::uint64_t floats = fbe_texture_create_target(8, 8, FBE_TEXTURE_RGBA16F, 1, true);
    require(floats != 0);
    require(fbe_camera_set_target(scene.camera, floats));
    const std::array<float, 3> held = middle_light(scene, floats);
    CHECK(std::abs(held[0] - 0.5F) < 0.01F);
    CHECK(held[1] == 0.0F);
    require(fbe_camera_set_target(scene.camera, scene.target));
    fbe_texture_release(floats);
    fbe_material_release(material);
}

// F5: an image that can't be made fails where a camera's drawn, not silently in the frame; and it's
// there next time, if it can be.
TEST_CASE("an HDR camera whose image can't be made says so to its caller", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = glowing_wall(scene, 0.5F, 0.5F, 0.5F);
    fbe::gpu::inject(fbe::gpu::fault_t::VIEW);
    CHECK(!fbe_render_world(scene.world, 1.0F));
    CHECK(std::string(fbe_error()).starts_with("couldn't make a texture's view"));
    const std::uint8_t half = srgb(0.5F);
    CHECK(near(middle(scene.drawn()), { half, half, half }, 1));
    // So too bloom's.
    const scene_t& same = scene;
    require(fbe_camera_set_bloom(same.camera, 1.0F, 1.0F));
    fbe::gpu::inject(fbe::gpu::fault_t::VIEW);
    CHECK(!fbe_render_world(scene.world, 1.0F));
    CHECK(near(middle(scene.drawn()), { half, half, half }, 1));
    fbe_material_release(material);
}

// F8: what goes through it all, a fixed-function color and a clear color, is what its curve says.
TEST_CASE("an HDR camera's curve takes UNLIT and its clear color", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_hdr(scene.camera, true));
    require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_NEUTRAL));
    require(fbe_camera_set_clear_color(scene.camera, 1.0F, 1.0F, 1.0F, 1.0F));
    const std::uint8_t white = srgb(neutral(1.0F));
    CHECK(near(middle(scene.drawn()), { white, white, white }, 1));
    const std::uint64_t material = unlit(1.0F, 1.0F, 1.0F);
    wall(scene.world, -3.0F, 10.0F, material);
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    CHECK(near(middle(scene.drawn()), { white, white, white }, 1));
    require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_ACES));
    const std::uint8_t filmic = srgb(0.673F);
    CHECK(near(middle(scene.drawn()), { filmic, filmic, filmic }, 2));
    require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_REINHARD));
    const std::uint8_t half = srgb(0.5F);
    CHECK(near(middle(scene.drawn()), { half, half, half }, 1));
    require(fbe_camera_set_exposure(scene.camera, 0.0F));
    require(fbe_camera_set_clear_color(scene.camera, 1.0F, 1.0F, 1.0F, 1.0F));
    CHECK(near(pixel(scene.drawn(), 1, 1), { 0, 0, 0 }));
    fbe_material_release(material);
}

// F10: an HDR camera with no curve draws what one without it draws, a pixel at a time, however its
// picture's laid out, so a flip, a swapped size, or a wrong place shows.
namespace {

/// Whether the picture tests below bloom, with nothing past a threshold so far up, so they draw
/// what they would otherwise, by way of a chain for each layout.
bool pictures_bloom = false;

/// Sets a camera HDR or not, with what blooms if its tests' section does.
bool hdr_with_bloom(std::uint64_t camera, bool hdr) {
    return fbe_camera_set_hdr(camera, hdr) &&
           (!pictures_bloom || fbe_camera_set_bloom(camera, 1.0F, 10000.0F));
}

} // namespace

TEST_CASE("an HDR camera's picture is what one without it draws", "[gpu][scene]") {
    pictures_bloom = GENERATE(false, true);
    const auto same = [](const fbe::golden::frame_t& plain, const fbe::golden::frame_t& hdr,
                         int within) {
        REQUIRE(plain.width == hdr.width);
        REQUIRE(plain.height == hdr.height);
        int differing = 0;
        for (std::size_t at = 0; at < plain.rgba.size(); ++at) {
            differing += std::abs(plain.rgba[at] - hdr.rgba[at]) > within ? 1 : 0;
        }
        CHECK(differing == 0);
    };
    // Different colors in different corners of a target wider than high, in a viewport away from
    // its corner, on a color of its own.
    const auto picture = [](const scene_t& scene, std::uint64_t red, std::uint64_t green,
                            std::uint64_t blue) {
        cube(scene.world, -1.5F, 0.8F, -8.0F, red);
        cube(scene.world, 1.8F, -0.7F, -9.0F, green);
        cube(scene.world, 0.5F, 2.5F, -12.0F, blue);
    };
    struct colors_t {
        std::uint64_t red = unlit(1.0F, 0.0F, 0.0F);
        std::uint64_t green = unlit(0.0F, 0.5F, 0.0F);
        std::uint64_t blue = unlit(0.0F, 0.25F, 0.75F);
    };
    SECTION("in a viewport, off a corner, of a target wider than it's high") {
        const auto draw = [&picture](bool hdr) {
            const scene_t scene;
            const colors_t colors;
            const std::uint64_t target =
                fbe_texture_create_target(64, 32, FBE_TEXTURE_RGBA8, 1, true);
            require(target != 0);
            require(fbe_camera_set_target(scene.camera, target));
            require(fbe_camera_set_viewport(scene.camera, 8.0F, 4.0F, 40.0F, 20.0F));
            require(fbe_camera_set_clear_color(scene.camera, 0.1F, 0.1F, 0.2F, 1.0F));
            require(hdr_with_bloom(scene.camera, hdr));
            picture(scene, colors.red, colors.green, colors.blue);
            require(fbe_render_world(scene.world, 1.0F));
            const fbe::golden::frame_t frame = scene_t::read(target);
            require(fbe_camera_set_target(scene.camera, scene.target));
            fbe_texture_release(target);
            return frame;
        };
        const fbe::golden::frame_t plain = draw(false);
        // Not nothing: all three colors are there, and outside the viewport isn't.
        CHECK(pixel(plain, 2, 2)[0] == 0);
        same(plain, draw(true), 1);
    }
    SECTION("two cameras overlapping") {
        const auto draw = [&picture](bool hdr) {
            const scene_t scene;
            const colors_t colors;
            picture(scene, colors.red, colors.green, colors.blue);
            require(hdr_with_bloom(scene.camera, hdr));
            require(fbe_camera_set_viewport(scene.camera, 0.0F, 0.0F, 100.0F, 100.0F));
            const std::uint64_t over = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
            require(fbe_camera_set_target(over, scene.target));
            require(fbe_camera_set_order(over, 1));
            require(hdr_with_bloom(over, hdr));
            require(fbe_camera_set_viewport(over, 40.0F, 30.0F, 80.0F, 90.0F));
            require(fbe_camera_set_clear_color(over, 0.3F, 0.0F, 0.3F, 1.0F));
            return scene.drawn();
        };
        same(draw(false), draw(true), 1);
    }
    SECTION("a mirror") {
        const auto draw = [&picture](bool hdr) {
            const scene_t scene;
            const colors_t colors;
            picture(scene, colors.red, colors.green, colors.blue);
            require(fbe_entity_set_position(scene.camera, 0.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
            const std::uint64_t mirror = fbe_entity_create(scene.world, 0, FBE_ENTITY_MIRROR);
            require(fbe_entity_set_position(mirror, 0.0F, -0.5F, 0.0F, FBE_SPACE_WORLD));
            require(hdr_with_bloom(scene.camera, hdr));
            return scene.drawn();
        };
        same(draw(false), draw(true), 1);
    }
    SECTION("sharing a target's depth with the camera before it") {
        const auto draw = [&picture](bool hdr) {
            const scene_t scene;
            const colors_t colors;
            picture(scene, colors.red, colors.green, colors.blue);
            const std::uint64_t over = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
            require(fbe_camera_set_target(over, scene.target));
            require(fbe_camera_set_order(over, 1));
            require(fbe_camera_set_clear(over, false, false));
            require(hdr_with_bloom(over, hdr));
            require(fbe_entity_set_position(over, 0.3F, 0.0F, 0.5F, FBE_SPACE_WORLD));
            return scene.drawn();
        };
        same(draw(false), draw(true), 2);
    }
    SECTION("not clearing a target of four samples") {
        const auto draw = [&picture](bool hdr) {
            const scene_t scene;
            const colors_t colors;
            const std::uint64_t target =
                fbe_texture_create_target(64, 64, FBE_TEXTURE_RGBA8, 4, true);
            require(target != 0);
            require(fbe_camera_set_target(scene.camera, target));
            require(fbe_camera_set_order(scene.camera, 1));
            require(fbe_camera_set_clear(scene.camera, false, true));
            require(hdr_with_bloom(scene.camera, hdr));
            picture(scene, colors.red, colors.green, colors.blue);
            const std::uint64_t under = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
            require(fbe_camera_set_target(under, target));
            require(fbe_camera_set_clear_color(under, 0.0F, 0.2F, 0.2F, 1.0F));
            require(fbe_entity_set_rotation(under, 0.0F, 1.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
            require(fbe_render_world(scene.world, 1.0F));
            const fbe::golden::frame_t frame = scene_t::read(target);
            require(fbe_camera_set_target(scene.camera, scene.target));
            require(fbe_camera_set_target(under, scene.target));
            fbe_texture_release(target);
            return frame;
        };
        same(draw(false), draw(true), 3);
    }
    SECTION("a target sampled after") {
        const auto draw = [&picture](bool hdr) {
            const scene_t scene;
            const colors_t colors;
            picture(scene, colors.red, colors.green, colors.blue);
            const std::uint64_t monitor_target =
                fbe_texture_create_target(32, 32, FBE_TEXTURE_RGBA8, 1, true);
            require(monitor_target != 0);
            const std::uint64_t monitor = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
            require(fbe_camera_set_target(monitor, monitor_target));
            require(fbe_camera_set_order(monitor, -1));
            require(hdr_with_bloom(monitor, hdr));
            const std::uint64_t screen = unlit(1.0F, 1.0F, 1.0F);
            require(fbe_material_set_texture(screen, 0, monitor_target));
            wall(scene.world, -7.0F, 0.5F, screen);
            const fbe::golden::frame_t frame = scene.drawn();
            fbe_material_release(screen);
            require(fbe_camera_set_target(monitor, scene.target));
            fbe_texture_release(monitor_target);
            return frame;
        };
        same(draw(false), draw(true), 2);
    }
    SECTION("the window's screen, at another size") {
        const auto draw = [](bool hdr) {
            const scene_t scene;
            const colors_t colors;
            require(fbe_window_open(64, 48, FBE_WINDOW_WINDOWED));
            require(fbe_camera_set_target(scene.camera, 0));
            require(hdr_with_bloom(scene.camera, hdr));
            cube(scene.world, -1.5F, 0.8F, -8.0F, colors.red);
            cube(scene.world, 1.8F, -0.7F, -9.0F, colors.green);
            require(fbe_render_world(scene.world, 1.0F));
            const fbe::golden::frame_t first = scene_t::read(fbe_gpu_screen());
            fbe_frame_flip(false);
            fbe_window_close();
            require(fbe_window_open(96, 32, FBE_WINDOW_WINDOWED));
            fbe_frame_flip(false);
            require(fbe_render_world(scene.world, 1.0F));
            const fbe::golden::frame_t second = scene_t::read(fbe_gpu_screen());
            fbe_frame_flip(false);
            fbe_window_close();
            require(fbe_camera_set_target(scene.camera, scene.target));
            return std::pair{ first, second };
        };
        const auto [plain_first, plain_second] = draw(false);
        const auto [hdr_first, hdr_second] = draw(true);
        same(plain_first, hdr_first, 1);
        same(plain_second, hdr_second, 1);
    }
}

// F11: colors, each channel by its curve; nothing below none, nor an alpha that's NaN, comes
// through.
TEST_CASE("a curve takes each of a color's channels, and nothing below none", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = glowing_wall(scene, 0.5F, 2.0F, 8.0F);
    require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_REINHARD));
    CHECK(
        near(middle(scene.drawn()),
             { srgb(through(FBE_TONEMAP_REINHARD, 0.5F)), srgb(through(FBE_TONEMAP_REINHARD, 2.0F)),
               srgb(through(FBE_TONEMAP_REINHARD, 8.0F)) },
             1));
    require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_ACES));
    CHECK(near(middle(scene.drawn()), { srgb(aces(0.5F)), srgb(aces(2.0F)), srgb(aces(8.0F)) }, 1));
    const auto of_halves = [](std::uint16_t color, std::uint16_t alpha) {
        const std::uint64_t texture = fbe_texture_create(2, 2, FBE_TEXTURE_RGBA16F, false);
        require(texture != 0);
        std::array<std::uint16_t, 16> halves{};
        for (std::size_t at = 0; at < 4; ++at) {
            halves.at(at * 4) = color;
            halves.at((at * 4) + 1) = color;
            halves.at((at * 4) + 2) = color;
            halves.at((at * 4) + 3) = alpha;
        }
        std::array<std::uint8_t, 32> bytes{};
        std::memcpy(bytes.data(), halves.data(), bytes.size());
        require(fbe_texture_write(texture, 0, 0, 2, 2, bytes.data(), 16));
        require(fbe_texture_set_filter(texture, FBE_TEXTURE_NEAREST));
        return texture;
    };
    // -1, as a half float, as what's added through a map.
    const std::uint64_t below = of_halves(0xBC00, 0x3C00);
    require(fbe_material_set_map(material, FBE_MAP_EMISSIVE, below, 0));
    for (const std::int32_t tonemap :
         { FBE_TONEMAP_NONE, FBE_TONEMAP_REINHARD, FBE_TONEMAP_ACES, FBE_TONEMAP_NEUTRAL }) {
        INFO("tonemap " << tonemap);
        require(fbe_camera_set_tonemap(scene.camera, tonemap));
        CHECK(near(middle(scene.drawn()), { 0, 0, 0 }));
    }
    require(fbe_material_set_map(material, FBE_MAP_EMISSIVE, 0, 0));
    // An alpha of NaN, through a layer, in an image that lets the green under it show.
    under_it(scene, 0.0F, 1.0F, 0.0F);
    const std::uint64_t nan_alpha = of_halves(0x3C00, 0x7E00);
    require(fbe_material_set_blend(material, FBE_BLEND_ALPHA));
    require(fbe_material_set_texture(material, 0, nan_alpha));
    require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_NONE));
    CHECK(near(middle(scene.drawn()), { 0, 255, 0 }));
    fbe_texture_release(below);
    fbe_texture_release(nan_alpha);
    fbe_material_release(material);
}

// F12: the tonemap's a draw.
TEST_CASE("a tonemap's counted as the frame's draws", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = glowing_wall(scene, 0.5F, 0.5F, 0.5F);
    const auto draws = [&scene] {
        require(fbe_render_world(scene.world, 1.0F));
        fbe_frame_flip(false);
        fbe_gpu_counters_t counters{};
        require(fbe_gpu_counters(&counters));
        return counters.draws;
    };
    const auto on = draws();
    require(fbe_camera_set_hdr(scene.camera, false));
    const auto off = draws();
    CHECK(on == off + 1);
    fbe_material_release(material);
}

// What adds has no alpha in the image, which is no reason to divide by it: it adds what its curve
// makes of it, over what's there.
TEST_CASE("what adds, in an HDR camera that doesn't clear, adds", "[gpu][scene]") {
    const scene_t scene;
    under_it(scene, 0.0F, 0.0F, 0.5F);
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_blend(material, FBE_BLEND_ADD));
    require(fbe_material_set_emissive(material, 0.25F, 0.25F, 0.25F));
    wall(scene.world, -3.0F, 10.0F, material);
    require(fbe_camera_set_hdr(scene.camera, true));
    CHECK(near(middle(scene.drawn()), { srgb(0.25F), srgb(0.25F), srgb(0.75F) }, 2));
    fbe_material_release(material);
}

// A gamma small enough raises anything past 1 past what a float holds, which a float target's not
// to take.
TEST_CASE("a small gamma leaves a float target finite", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = glowing_wall(scene, 2.0F, 2.0F, 2.0F);
    const std::uint64_t floats = fbe_texture_create_target(8, 8, FBE_TEXTURE_RGBA16F, 1, true);
    require(floats != 0);
    require(fbe_camera_set_target(scene.camera, floats));
    require(fbe_camera_set_gamma(scene.camera, 0.001F));
    const std::array<float, 3> held = middle_light(scene, floats);
    CHECK(std::isfinite(held[0]));
    CHECK(held[0] >= 65000.0F);
    require(fbe_camera_set_target(scene.camera, scene.target));
    fbe_texture_release(floats);
    fbe_material_release(material);
}

namespace {

/// A lit wall behind a cube touching it, by an ambient light of a quarter, which is all that lights
/// them: around the cube's outline, the wall's sky is half cut off.
struct cornered_t {
    std::uint64_t paint = fbe_material_create();
    std::uint64_t back = 0;
    std::uint64_t block = 0;

    explicit cornered_t(const scene_t& scene) {
        require(fbe_world_set_ambient(scene.world, 0.25F, 0.25F, 0.25F));
        require(fbe_camera_set_ambient_occlusion_radius(scene.camera, 3.0F));
        back = wall(scene.world, -10.0F, 30.0F, paint);
        block = cube(scene.world, 0.0F, 0.0F, -9.0F, paint);
    }
    cornered_t(const cornered_t&) = delete;
    cornered_t& operator=(const cornered_t&) = delete;
    cornered_t(cornered_t&&) = delete;
    cornered_t& operator=(cornered_t&&) = delete;

    ~cornered_t() {
        fbe_material_release(paint);
    }
};

/// A pixel of the wall by the cube's outline, and one of the wall far from it.
const std::array<std::uint32_t, 2> beside_block{ 64 + 16, 64 };
const std::array<std::uint32_t, 2> open_wall{ 4, 4 };

/// What a camera's frame's passes are, once it's drawn.
std::vector<std::string_view> passes_of(const scene_t& scene) {
    (void)scene.drawn();
    return fbe::gpu::last_recorded().passes;
}

} // namespace

TEST_CASE("a camera's ambient occlusion and antialiasing over frames are what they're set to",
          "[gpu][scene]") {
    const scene_t scene;
    CHECK(fbe_camera_ambient_occlusion(scene.camera) == 0.0F);
    CHECK(fbe_camera_ambient_occlusion_radius(scene.camera) == 1.0F);
    CHECK_FALSE(fbe_camera_temporal_aa(scene.camera));
    require(fbe_camera_set_ambient_occlusion(scene.camera, 2.0F));
    require(fbe_camera_set_ambient_occlusion_radius(scene.camera, 0.5F));
    require(fbe_camera_set_temporal_aa(scene.camera, true));
    CHECK(fbe_camera_ambient_occlusion(scene.camera) == 2.0F);
    CHECK(fbe_camera_ambient_occlusion_radius(scene.camera) == 0.5F);
    CHECK(fbe_camera_temporal_aa(scene.camera));
    CHECK_FALSE(fbe_camera_set_ambient_occlusion(scene.camera, -1.0F));
    CHECK_FALSE(fbe_camera_set_ambient_occlusion(scene.camera, std::nanf("")));
    CHECK_FALSE(fbe_camera_set_ambient_occlusion_radius(scene.camera, 0.0F));
    // A radius so small its half and its square are no numbers to go by.
    CHECK_FALSE(fbe_camera_set_ambient_occlusion_radius(scene.camera, 1e-40F));
    CHECK_FALSE(fbe_camera_set_ambient_occlusion_radius(scene.camera,
                                                        std::numeric_limits<float>::infinity()));
    CHECK(fbe_camera_ambient_occlusion(scene.camera) == 2.0F);
    CHECK(fbe_camera_ambient_occlusion_radius(scene.camera) == 0.5F);
    CHECK(fbe_camera_reset_history(scene.camera));
    const std::uint64_t pivot = fbe_entity_create(scene.world, 0, FBE_ENTITY_PIVOT);
    CHECK_FALSE(fbe_camera_set_ambient_occlusion(pivot, 1.0F));
    CHECK_FALSE(fbe_camera_set_temporal_aa(pivot, true));
    CHECK_FALSE(fbe_camera_reset_history(pivot));
}

TEST_CASE("ambient occlusion darkens what's near a surface, and nothing else", "[gpu][scene]") {
    const scene_t scene;
    const cornered_t room(scene);
    const fbe::golden::frame_t without = scene.drawn();
    const std::uint8_t lit = srgb(0.25F);
    CHECK(near(pixel_of(without, open_wall), { lit, lit, lit }));
    CHECK(near(pixel_of(without, beside_block), { lit, lit, lit }));
    require(fbe_camera_set_ambient_occlusion(scene.camera, 1.0F));
    const fbe::golden::frame_t with = scene.drawn();
    SECTION("what's open keeps all its light, exactly") {
        CHECK(pixel_of(with, open_wall) == pixel_of(without, open_wall));
        CHECK(middle(with) == middle(without));
    }
    SECTION("what's by what stands off it has less") {
        CHECK(pixel_of(with, beside_block)[0] < lit - 8);
        fbe::golden::check("scene_ambient_occlusion", with);
    }
    SECTION("what slopes a little is open too, far off, where half floats go in steps") {
        require(fbe_entity_set_hidden(room.block, true));
        require(fbe_entity_set_scale(room.back, 100.0F, 100.0F, 1.0F, FBE_SPACE_WORLD));
        turn_about(room.back, 0.0F, 1.0F, 0.0F, 0.05F);
        require(fbe_camera_set_range(scene.camera, 0.1F, 1000.0F));
        require(fbe_camera_set_ambient_occlusion_radius(scene.camera, 30.0F));
        require(fbe_entity_set_position(room.back, 0.0F, 0.0F, -300.0F, FBE_SPACE_WORLD));
        const fbe::golden::frame_t sloped = scene.drawn();
        require(fbe_camera_set_ambient_occlusion(scene.camera, 0.0F));
        CHECK(fbe::golden::compare(scene.drawn(), sloped, 1).pixels == 0);
    }
    SECTION("more strength, less") {
        require(fbe_camera_set_ambient_occlusion(scene.camera, 3.0F));
        CHECK(pixel_of(scene.drawn(), beside_block)[0] < pixel_of(with, beside_block)[0]);
    }
    SECTION("only within its radius") {
        // The cube stands 2 off the wall, so nothing of it is within 1.
        require(fbe_camera_set_ambient_occlusion_radius(scene.camera, 1.0F));
        CHECK(pixel_of(scene.drawn(), beside_block) == pixel_of(without, beside_block));
    }
    SECTION("its passes are there only while it is") {
        const std::vector<std::string_view> on = passes_of(scene);
        CHECK(std::ranges::count(on, std::string_view("prepass")) == 1);
        CHECK(std::ranges::count(on, std::string_view("ambient occlusion")) == 3);
        require(fbe_camera_set_ambient_occlusion(scene.camera, 0.0F));
        const std::vector<std::string_view> off = passes_of(scene);
        CHECK(std::ranges::count(off, std::string_view("prepass")) == 0);
        CHECK(std::ranges::count(off, std::string_view("ambient occlusion")) == 0);
    }
    SECTION("seen flat, too") {
        require(fbe_camera_set_projection(scene.camera, FBE_PROJECTION_ORTHOGRAPHIC));
        require(fbe_camera_set_orthographic_height(scene.camera, 12.0F));
        require(fbe_camera_set_ambient_occlusion(scene.camera, 0.0F));
        const fbe::golden::frame_t flat = scene.drawn();
        require(fbe_camera_set_ambient_occlusion(scene.camera, 1.0F));
        const fbe::golden::frame_t flat_with = scene.drawn();
        CHECK(pixel_of(flat_with, open_wall) == pixel_of(flat, open_wall));
        // Half the frame's 12 across is 64 pixels, so the cube's edge is at 64 + 10.67.
        const std::array<std::uint32_t, 2> by_it{ 64 + 13, 64 };
        CHECK(pixel_of(flat_with, by_it)[0] < pixel_of(flat, by_it)[0] - 4);
    }
    SECTION("in its viewport, wherever that is") {
        require(fbe_camera_set_viewport(scene.camera, 64.0F, 32.0F, 64.0F, 64.0F));
        require(fbe_camera_set_ambient_occlusion(scene.camera, 0.0F));
        const fbe::golden::frame_t small = scene.drawn();
        require(fbe_camera_set_ambient_occlusion(scene.camera, 1.0F));
        const fbe::golden::frame_t small_with = scene.drawn();
        const std::array<std::uint32_t, 2> far_off{ 66, 34 };
        const std::array<std::uint32_t, 2> by_it{ 96 + 8, 64 };
        CHECK(pixel_of(small_with, far_off) == pixel_of(small, far_off));
        CHECK(pixel_of(small_with, by_it)[0] < pixel_of(small, by_it)[0] - 4);
    }
    SECTION("what's drawn by a target of many samples takes it") {
        const std::uint64_t many = fbe_texture_create_target(128, 128, FBE_TEXTURE_RGBA8, 4, true);
        require(fbe_camera_set_target(scene.camera, many));
        require(fbe_render_world(scene.world, 1.0F));
        const fbe::golden::frame_t by_many = scene_t::read(many);
        fbe_frame_flip(false);
        const std::uint8_t dark = pixel_of(with, beside_block)[0];
        CHECK(near(pixel_of(by_many, beside_block), { dark, dark, dark }, 2));
        require(fbe_camera_set_target(scene.camera, scene.target));
        fbe_texture_release(many);
    }
    CHECK(fbe_gpu_validation_errors() == 0);
}

TEST_CASE("direct light takes no ambient occlusion, and what blends or has no lighting none",
          "[gpu][scene]") {
    const scene_t scene;
    const cornered_t room(scene);
    const auto same_with_it = [&] {
        require(fbe_camera_set_ambient_occlusion(scene.camera, 0.0F));
        const fbe::golden::frame_t without = scene.drawn();
        require(fbe_camera_set_ambient_occlusion(scene.camera, 1.0F));
        return fbe::golden::compare(without, scene.drawn(), 0).pixels == 0;
    };
    CHECK_FALSE(same_with_it());
    SECTION("a sun's") {
        require(fbe_world_set_ambient(scene.world, 0.0F, 0.0F, 0.0F));
        light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
        CHECK(same_with_it());
    }
    SECTION("what's unlit") {
        require(fbe_material_set_shading(room.paint, FBE_SHADING_UNLIT));
        CHECK(same_with_it());
    }
    SECTION("what's lit by vertex") {
        require(fbe_material_set_shading(room.paint, FBE_SHADING_VERTEX));
        CHECK(same_with_it());
    }
    SECTION("what blends gives none") {
        const std::uint64_t see_through = fbe_material_create();
        require(fbe_material_set_blend(see_through, FBE_BLEND_ALPHA));
        require(fbe_mesh_set_material(room.block, see_through));
        CHECK(same_with_it());
        fbe_material_release(see_through);
    }
    SECTION("what's in another order gives none") {
        const std::uint64_t over = fbe_material_create();
        require(fbe_material_set_order(over, 1));
        require(fbe_mesh_set_material(room.block, over));
        CHECK(same_with_it());
        fbe_material_release(over);
    }
    const auto pane_keeps_its_light = [&](std::uint64_t pane_paint) {
        // A whole pane, over all that's darkened, of its own light.
        wall(scene.world, -5.0F, 30.0F, pane_paint);
        require(fbe_camera_set_ambient_occlusion(scene.camera, 1.0F));
        const fbe::golden::frame_t frame = scene.drawn();
        const std::uint8_t lit = srgb(0.25F);
        CHECK(near(pixel_of(frame, beside_block), { lit, lit, lit }, 1));
    };
    SECTION("what blends over it takes none") {
        const std::uint64_t pane = fbe_material_create();
        require(fbe_material_set_blend(pane, FBE_BLEND_ALPHA));
        pane_keeps_its_light(pane);
        fbe_material_release(pane);
    }
    SECTION("what's in another order takes none") {
        const std::uint64_t pane = fbe_material_create();
        require(fbe_material_set_order(pane, 1));
        pane_keeps_its_light(pane);
        fbe_material_release(pane);
    }
    SECTION("what a cut-out cuts out gives none") {
        const std::uint64_t holes = fbe_material_create();
        require(fbe_material_set_alpha_cutoff(holes, 0.5F));
        const std::uint64_t clear = flat_texture(255, 255, 255, 0);
        require(fbe_material_set_texture(holes, 0, clear));
        require(fbe_mesh_set_material(room.block, holes));
        CHECK(same_with_it());
        fbe_texture_release(clear);
        fbe_material_release(holes);
    }
}

TEST_CASE("an Instances and what's skinned give ambient occlusion as what they draw",
          "[gpu][scene]") {
    const scene_t scene;
    const cornered_t room(scene);
    require(fbe_camera_set_ambient_occlusion(scene.camera, 1.0F));
    const fbe::golden::frame_t by_mesh = scene.drawn();
    REQUIRE(pixel_of(by_mesh, beside_block)[0] < srgb(0.25F) - 8);
    require(fbe_entity_set_hidden(room.block, true));
    SECTION("an Instances") {
        cubes_at(scene.world, room.paint, { instance_at(0.0F, 0.0F, -9.0F) });
        CHECK(fbe::golden::compare(by_mesh, scene.drawn(), 1).pixels == 0);
    }
    SECTION("a mesh on joints moved to where it's seen") {
        const skinned_wall_t skinned = skinned_wall(scene.world, room.paint, -8.0F, 1.0F);
        all_on(skinned.geometry, 0, 1.0F);
        // Its joints have it 30 to the right, then back here.
        require(fbe_entity_set_position(skinned.mesh, 30.0F, 0.0F, -8.0F, FBE_SPACE_WORLD));
        const std::array<std::uint32_t, 2> by_wall{ 64 + 16, 64 };
        CHECK(pixel_of(scene.drawn(), by_wall)[0] < srgb(0.25F) - 8);
        fbe_skin_release(skinned.skin);
    }
    SECTION("what morph targets moved") {
        const std::uint64_t thin = wall(scene.world, -8.0F, 1.0F, room.paint);
        add_lift(fbe_mesh_geometry(thin), 100.0F);
        require(fbe_entity_set_position(thin, 0.0F, -100.0F, -8.0F, FBE_SPACE_WORLD));
        const std::array one{ 1.0F };
        require(fbe_morph_set_weights(thin, one.data(), 1));
        const std::array<std::uint32_t, 2> by_wall{ 64 + 16, 64 };
        CHECK(pixel_of(scene.drawn(), by_wall)[0] < srgb(0.25F) - 8);
    }
    CHECK(fbe_gpu_validation_errors() == 0);
}

namespace {

/// A white wall on black, seen by an HDR camera, its left edge in the middle of a pixel: half of
/// what's there's white, and with one sample a pixel's all or none of it.
struct edge_t {
    std::uint64_t paint = unlit(1.0F, 1.0F, 1.0F);
    std::uint64_t white = 0;
    /// How wide a pixel is at the wall's depth.
    float pixel = 10.0F * std::tan(std::numbers::pi_v<float> / 6.0F) / 64.0F;

    explicit edge_t(const scene_t& scene) {
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
        require(fbe_camera_set_hdr(scene.camera, true));
        require(fbe_camera_set_temporal_aa(scene.camera, true));
        white = wall(scene.world, -10.0F, 3.0F, paint);
        place(0.0F);
    }
    edge_t(const edge_t&) = delete;
    edge_t& operator=(const edge_t&) = delete;
    edge_t(edge_t&&) = delete;
    edge_t& operator=(edge_t&&) = delete;

    ~edge_t() {
        fbe_material_release(paint);
    }

    /// Has it the way along of `pixels`: its left edge in the middle of pixel 64 + `pixels`.
    void place(float pixels) const {
        require(fbe_entity_set_position(white, 3.0F + ((0.5F + pixels) * pixel), 0.0F, -10.0F,
                                        FBE_SPACE_WORLD));
    }
};

constexpr std::uint32_t edge_at = 64;
constexpr std::uint32_t edge_row = 64;

/// What the resolve comes to on a pixel an edge's 0.45 of the way across, white on its right, after
/// some frames: whether its middle's covered goes by Halton's 2 of each frame's place, and its
/// neighbors, one of each, hold nothing back.
float settled_edge(int frames) {
    float held = 0.0F;
    for (int frame = 0; frame < frames; ++frame) {
        float halton = 0.0F;
        float part = 1.0F;
        for (int left = (frame % 8) + 1; left > 0; left /= 2) {
            part *= 0.5F;
            halton += part * static_cast<float>(left % 2);
        }
        const float now = 0.45F + (halton - 0.5F) <= 0.5F ? 1.0F : 0.0F;
        const float weight = std::max(0.1F, 1.0F / static_cast<float>(frame + 1));
        const float now_part = weight / (1.0F + now);
        const float held_part = (1.0F - weight) / (1.0F + held);
        held = ((now * now_part) + (held * held_part)) / (now_part + held_part);
    }
    return held;
}

/// How far, in the 0 to 1 of linear light, what's in a pixel's red is from `expected`.
float linear_off(rgba_t found, float expected) {
    const float value = static_cast<float>(found[0]) / 255.0F;
    const float linear =
        value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
    return std::fabs(linear - expected);
}

} // namespace

TEST_CASE("an edge comes out as what's in each pixel of it, over frames", "[gpu][scene]") {
    const scene_t scene;
    const edge_t edge(scene);
    SECTION("with one sample and no frames behind it, it's all or nothing") {
        require(fbe_camera_set_temporal_aa(scene.camera, false));
        const rgba_t only = pixel(scene.drawn(), edge_at, edge_row);
        CHECK((only[0] == 0 || only[0] == 255));
    }
    SECTION("its first frame's what's drawn, then it settles on what its frames say") {
        edge.place(-0.05F);
        const rgba_t first = pixel(scene.drawn(), edge_at, edge_row);
        CHECK((first[0] == 0 || first[0] == 255));
        fbe::golden::frame_t last = scene.drawn();
        for (int i = 0; i < 24; ++i) {
            last = scene.drawn();
        }
        CAPTURE(pixel(last, edge_at, edge_row)[0]);
        CHECK(linear_off(pixel(last, edge_at, edge_row), settled_edge(26)) < 0.02F);
        // Away from it, what's still is exactly as it was, as near as next to it: its jitter
        // goes no more than half a pixel either way.
        CHECK(pixel(last, edge_at - 8, edge_row)[0] == 0);
        CHECK(pixel(last, edge_at + 8, edge_row)[0] == 255);
        CHECK(linear_off(pixel(last, edge_at - 2, edge_row), 0.0F) < 0.02F);
        CHECK(linear_off(pixel(last, edge_at + 2, edge_row), 1.0F) < 0.02F);
        // And it doesn't crawl.
        const fbe::golden::frame_t next = scene.drawn();
        CHECK(std::abs(pixel(next, edge_at, edge_row)[0] - pixel(last, edge_at, edge_row)[0]) < 40);
        SECTION("told to forget, it's what's drawn again") {
            require(fbe_camera_reset_history(scene.camera));
            const rgba_t forgot = pixel(scene.drawn(), edge_at, edge_row);
            CHECK((forgot[0] == 0 || forgot[0] == 255));
        }
        SECTION("a new viewport forgets too") {
            require(fbe_camera_set_viewport(scene.camera, 0.0F, 0.0F, 128.0F, 127.0F));
            const rgba_t forgot = pixel(scene.drawn(), edge_at, edge_row);
            CHECK((forgot[0] == 0 || forgot[0] == 255));
        }
        SECTION("a new size forgets too") {
            const std::uint64_t bigger =
                fbe_texture_create_target(256, 256, FBE_TEXTURE_RGBA8, 1, true);
            require(fbe_camera_set_target(scene.camera, bigger));
            require(fbe_render_world(scene.world, 1.0F));
            const fbe::golden::frame_t big = scene_t::read(bigger);
            fbe_frame_flip(false);
            const rgba_t forgot = pixel(big, (edge_at * 2) + 1, edge_row * 2);
            CHECK((forgot[0] == 0 || forgot[0] == 255));
            CHECK(fbe::render::history_width(scene.camera) == 256);
            require(fbe_camera_set_target(scene.camera, scene.target));
            fbe_texture_release(bigger);
        }
    }
    SECTION("through many samples, and with bloom after it") {
        const std::uint64_t many = fbe_texture_create_target(128, 128, FBE_TEXTURE_RGBA8, 4, true);
        require(fbe_camera_set_target(scene.camera, many));
        require(fbe_camera_set_bloom(scene.camera, 0.1F, 2.0F));
        require(fbe_camera_set_ambient_occlusion(scene.camera, 1.0F));
        // Pixels too: a whole edge of what many samples of a frame's jitter and its frames say.
        fbe::golden::frame_t last;
        for (int i = 0; i < 24; ++i) {
            require(fbe_render_world(scene.world, 1.0F));
            last = scene_t::read(many);
            fbe_frame_flip(false);
        }
        CHECK(pixel(last, edge_at - 8, edge_row)[0] == 0);
        CHECK(pixel(last, edge_at + 8, edge_row)[0] > 250);
        CHECK(linear_off(pixel(last, edge_at, edge_row), 0.5F) < 0.1F);
        const std::vector<std::string_view> passes = passes_of(scene);
        const auto at = [&](std::string_view name) { return std::ranges::find(passes, name); };
        CHECK(at("prepass") < at("ambient occlusion"));
        CHECK(at("ambient occlusion") < at("camera"));
        CHECK(at("camera") < at("temporal resolve"));
        CHECK(at("temporal resolve") < at("bloom"));
        CHECK(at("bloom") < at("tonemap"));
        require(fbe_camera_set_target(scene.camera, scene.target));
        fbe_texture_release(many);
    }
    SECTION("each frame's a part of a pixel off the last, either way, each way") {
        const auto both_ways = [&](std::uint32_t x, std::uint32_t y) {
            bool had_none = false;
            bool had_all = false;
            for (int frame = 0; frame < 8; ++frame) {
                require(fbe_camera_reset_history(scene.camera));
                const std::uint8_t red = pixel(scene.drawn(), x, y)[0];
                had_none = had_none || red == 0;
                had_all = had_all || red == 255;
            }
            return had_none && had_all;
        };
        CHECK(both_ways(edge_at, edge_row));
        // Its top edge in the middle of a pixel, y going down.
        require(fbe_entity_set_position(edge.white, 0.0F, -3.0F - (0.5F * edge.pixel), -10.0F,
                                        FBE_SPACE_WORLD));
        CHECK(both_ways(edge_at, edge_row));
    }
    SECTION("only an HDR camera has one") {
        (void)scene.drawn();
        CHECK(fbe::render::history_count() == 1);
        require(fbe_camera_set_hdr(scene.camera, false));
        const fbe::golden::frame_t plain = scene.drawn();
        CHECK(fbe::render::history_count() == 0);
        // And none of its jitter.
        CHECK(fbe::golden::compare(plain, scene.drawn(), 0).pixels == 0);
    }
    SECTION("what's gone from the world has none") {
        (void)scene.drawn();
        CHECK(fbe::render::history_count() == 1);
        const std::uint64_t other_camera = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
        require(fbe_camera_set_target(other_camera, scene.target));
        require(fbe_camera_set_hdr(other_camera, true));
        require(fbe_camera_set_temporal_aa(other_camera, true));
        require(fbe_render_camera(other_camera, 1.0F));
        fbe_frame_flip(false);
        CHECK(fbe::render::history_count() == 2);
        require(fbe_entity_free(other_camera));
        (void)scene.drawn();
        CHECK(fbe::render::history_count() == 1);
    }
    CHECK(fbe_gpu_validation_errors() == 0);
}

TEST_CASE("what moves keeps what the frames before had of it, as it moves", "[gpu][scene]") {
    // Two whole pixels a frame, so each pixel's history is the one it moved from, whose edge has
    // settled as a still one's does, if the motion's right, and is all or nothing if it isn't.
    const scene_t scene;
    const edge_t edge(scene);
    const auto settled = [&](auto&& move_to) {
        fbe::golden::frame_t last = scene.drawn();
        for (int frame = 0; frame <= 24; ++frame) {
            move_to(static_cast<float>(frame - 24) * 2.0F);
            last = scene.drawn();
        }
        return pixel(last, edge_at, edge_row);
    };
    SECTION("an entity") {
        const rgba_t found = settled([&](float pixels) { edge.place(pixels); });
        CAPTURE(found[0]);
        CHECK(linear_off(found, 0.5F) < 0.2F);
    }
    SECTION("the camera") {
        const rgba_t found = settled([&](float pixels) {
            require(fbe_entity_set_position(scene.camera, -pixels * edge.pixel, 0.0F, 0.0F,
                                            FBE_SPACE_WORLD));
        });
        CAPTURE(found[0]);
        CHECK(linear_off(found, 0.5F) < 0.2F);
    }
    SECTION("what its joints move") {
        const skinned_wall_t skinned = skinned_wall(scene.world, edge.paint, -10.0F, 3.0F);
        require(fbe_entity_set_hidden(edge.white, true));
        all_on(skinned.geometry, 0, 1.0F);
        const rgba_t found = settled([&](float pixels) {
            const float x = 3.0F + ((0.5F + pixels) * edge.pixel);
            require(fbe_entity_set_position(skinned.still, x, 0.0F, -10.0F, FBE_SPACE_WORLD));
        });
        CAPTURE(found[0]);
        CHECK(linear_off(found, 0.5F) < 0.2F);
        fbe_skin_release(skinned.skin);
    }
    SECTION("what's gone from a pixel doesn't stay there") {
        (void)scene.drawn();
        for (int i = 0; i < 8; ++i) {
            (void)scene.drawn();
        }
        // All of it moved off, by more than any neighbor would hold on to.
        edge.place(-40.0F);
        const fbe::golden::frame_t moved = scene.drawn();
        CHECK(pixel(moved, edge_at + 8, edge_row)[0] == 255);
        edge.place(40.0F);
        const fbe::golden::frame_t back = scene.drawn();
        CHECK(pixel(back, edge_at + 8, edge_row)[0] == 0);
    }
    SECTION("a camera that turns finds the sky where it went") {
        // Stripes some pixels wide, so an edge's history going by the wrong way shows.
        const std::uint64_t texture =
            sky_of(1024, 4, [](std::int32_t x, std::int32_t /*y*/) -> rgba_t {
                const std::uint8_t value = (x / 8) % 2 == 0 ? 255 : 0;
                return { value, value, value, 255 };
            });
        require(fbe_world_set_sky_texture(scene.world, texture, 1.0F));
        require(fbe_entity_set_hidden(edge.white, true));
        for (int frame = 0; frame < 16; ++frame) {
            turn_about(scene.camera, 0.0F, 1.0F, 0.0F, static_cast<float>(frame) * 0.01F);
            (void)scene.drawn();
        }
        const fbe::golden::frame_t turning = scene.drawn();
        require(fbe_camera_set_temporal_aa(scene.camera, false));
        const fbe::golden::frame_t still = scene.drawn();
        // Nothing of it jitters, so all that's been is what's now, where it's gone to.
        // Its edges are a little soft, having been moved, but they're where they are.
        CHECK(fbe::golden::compare(still, turning, 60).pixels < 2000);
        fbe_texture_release(texture);
    }
    CHECK(fbe_gpu_validation_errors() == 0);
}

TEST_CASE("what's no number in one frame doesn't stay in those after", "[gpu][scene]") {
    const scene_t scene;
    const edge_t edge(scene);
    require(fbe_camera_set_clear_color(scene.camera, 0.5F, 0.5F, 0.5F, 1.0F));
    const std::uint64_t none = even_light(std::nanf(""), std::nanf(""), std::nanf(""));
    const std::uint64_t paint = unlit(1.0F, 1.0F, 1.0F);
    require(fbe_material_set_texture(paint, 0, none));
    const std::uint64_t bad = cube(scene.world, 0.0F, 0.0F, -5.0F, paint);
    require(fbe_entity_set_hidden(edge.white, true));
    (void)scene.drawn();
    // Nor does it spread to what's around it, which is held to what's around that.
    const fbe::golden::frame_t second = scene.drawn();
    const std::uint32_t beside = 64 + 29;
    CHECK(near(pixel(second, beside, 64), { srgb(0.5F), srgb(0.5F), srgb(0.5F) }, 3));
    CHECK(near(pixel(second, 2, 2), { srgb(0.5F), srgb(0.5F), srgb(0.5F) }, 3));
    require(fbe_entity_set_hidden(bad, true));
    const fbe::golden::frame_t last = scene.drawn();
    const std::uint8_t gray = srgb(0.5F);
    CHECK(near(middle(last), { gray, gray, gray }, 3));
    fbe_material_release(paint);
    fbe_texture_release(none);
}

namespace {

/// A plane of gray light, row by row from the top, as the rig below draws and reads it.
struct plane_t {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<float> light;

    plane_t(std::int32_t across, std::int32_t down, float each = 0.0F)
        : width(across), height(down),
          light(static_cast<std::size_t>(across) * static_cast<std::size_t>(down), each) {}

    [[nodiscard]] float& at(std::int32_t x, std::int32_t y) {
        return light[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
                     static_cast<std::size_t>(x)];
    }

    [[nodiscard]] float at(std::int32_t x, std::int32_t y) const {
        return light[(static_cast<std::size_t>(std::clamp(y, 0, height - 1)) *
                      static_cast<std::size_t>(width)) +
                     static_cast<std::size_t>(std::clamp(x, 0, width - 1))];
    }

    /// Filtered, as a linear sampler that clamps to its edge is.
    [[nodiscard]] float sampled(float u, float v) const {
        const float x = (u * static_cast<float>(width)) - 0.5F;
        const float y = (v * static_cast<float>(height)) - 0.5F;
        const float left = std::floor(x);
        const float top = std::floor(y);
        const auto from_x = static_cast<std::int32_t>(left);
        const auto from_y = static_cast<std::int32_t>(top);
        return std::lerp(std::lerp(at(from_x, from_y), at(from_x + 1, from_y), x - left),
                         std::lerp(at(from_x, from_y + 1), at(from_x + 1, from_y + 1), x - left),
                         y - top);
    }
};

/// What of a sample bloom's first level takes, with a threshold of 1.
float past_one(float sample) {
    const float knee = 0.5F;
    const float soft = std::clamp(sample - 1.0F + knee, 0.0F, 2.0F * knee);
    const float curve = soft * soft / ((4.0F * knee) + 0.0001F);
    return sample * (std::max(curve, sample - 1.0F) / std::max(sample, 0.0001F));
}

/// Karis's average of four samples.
float karis(float a, float b, float c, float d) {
    float sum = 0.0F;
    float weights = 0.0F;
    for (const float sample : { past_one(a), past_one(b), past_one(c), past_one(d) }) {
        const float weight = 1.0F / (1.0F + sample);
        sum += sample * weight;
        weights += weight;
    }
    return sum / weights;
}

/// What bloom does, on the CPU, so a picture's what this says pixel by pixel, wherever it's bright:
/// of a picture, the light it adds, at half the picture's size, and how many passes that takes.
struct mirror_t {
    plane_t added;
    std::size_t passes = 0;
};

mirror_t bloom_mirror(const plane_t& picture) {
    std::vector<plane_t> down;
    const plane_t* source = &picture;
    while (std::min(source->width, source->height) / 2 >= 8) {
        plane_t level(source->width / 2, source->height / 2);
        const float t_x = 1.0F / static_cast<float>(source->width);
        const float t_y = 1.0F / static_cast<float>(source->height);
        const bool first = down.empty();
        for (std::int32_t y = 0; y < level.height; ++y) {
            for (std::int32_t x = 0; x < level.width; ++x) {
                const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(level.width);
                const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(level.height);
                const auto take = [&](float along, float across) {
                    return source->sampled(u + (along * t_x), v + (across * t_y));
                };
                const float a = take(-2, -2);
                const float b = take(0, -2);
                const float c = take(2, -2);
                const float d = take(-2, 0);
                const float e = take(0, 0);
                const float f = take(2, 0);
                const float g = take(-2, 2);
                const float h = take(0, 2);
                const float i = take(2, 2);
                const float j = take(-1, -1);
                const float k = take(1, -1);
                const float l = take(-1, 1);
                const float m = take(1, 1);
                level.at(x, y) =
                    first ? (karis(j, k, l, m) * 0.5F) + ((karis(a, b, d, e) + karis(b, c, e, f) +
                                                           karis(d, e, g, h) + karis(e, f, h, i)) *
                                                          0.125F)
                          : ((j + k + l + m) * 0.125F) + ((a + c + g + i) * 0.03125F) +
                                ((b + d + f + h) * 0.0625F) + (e * 0.125F);
            }
        }
        down.push_back(std::move(level));
        source = &down.back();
    }
    REQUIRE(!down.empty());
    plane_t below = down.back();
    const std::size_t levels = down.size();
    for (std::size_t level = levels - 1; level-- > 0;) {
        const auto coarser = static_cast<float>(levels - 1 - level);
        const float worth_below = 1.0F - std::pow(0.8F, coarser);
        const float worth = level == 0 ? 1.0F : 1.0F - std::pow(0.8F, coarser + 1.0F);
        const float spread = worth_below / worth;
        plane_t raised(down[level].width, down[level].height);
        const float t_x = 1.0F / static_cast<float>(below.width);
        const float t_y = 1.0F / static_cast<float>(below.height);
        for (std::int32_t y = 0; y < raised.height; ++y) {
            for (std::int32_t x = 0; x < raised.width; ++x) {
                const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(raised.width);
                const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(raised.height);
                float tent = below.sampled(u, v) * 4.0F;
                for (const float along : { -1.0F, 1.0F }) {
                    for (const float across : { -1.0F, 1.0F }) {
                        tent += below.sampled(u + (along * t_x), v + (across * t_y));
                        tent += below.sampled(u + (along * t_x), v);
                        tent += below.sampled(u, v + (across * t_y));
                    }
                }
                raised.at(x, y) = std::lerp(down[level].at(x, y), tent / 16.0F, spread);
            }
        }
        below = std::move(raised);
    }
    return { .added = std::move(below), .passes = (levels * 2) - 1 };
}

/// A float target of light, and a wall of a picture filling an orthographic HDR camera's view, each
/// of its texels a pixel, with NONE's curve, exposure 1, and gamma 1.
struct light_rig_t {
    const scene_t& scene;
    std::int32_t width;
    std::int32_t height;
    std::uint64_t target;
    std::uint64_t material = fbe_material_create();
    std::uint64_t map = 0;
    std::uint64_t mesh = 0;

    light_rig_t(const scene_t& in, std::int32_t across, std::int32_t down)
        : scene(in), width(across), height(down),
          target(fbe_texture_create_target(across, down, FBE_TEXTURE_RGBA16F, 1, true)) {
        require(target != 0);
        require(fbe_camera_set_target(scene.camera, target));
        require(fbe_camera_set_projection(scene.camera, FBE_PROJECTION_ORTHOGRAPHIC));
        require(fbe_camera_set_orthographic_height(scene.camera, 2.0F));
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
        require(fbe_camera_set_hdr(scene.camera, true));
        require(fbe_material_set_emissive(material, 1.0F, 1.0F, 1.0F));
        mesh = wall(scene.world, -3.0F, 1.0F, material);
    }
    light_rig_t(const light_rig_t&) = delete;
    light_rig_t& operator=(const light_rig_t&) = delete;
    light_rig_t(light_rig_t&&) = delete;
    light_rig_t& operator=(light_rig_t&&) = delete;

    ~light_rig_t() {
        fbe_camera_set_target(scene.camera, scene.target);
        fbe_material_set_map(material, FBE_MAP_EMISSIVE, 0, 0);
        fbe_material_release(material);
        fbe_texture_release(map);
        fbe_texture_release(target);
    }

    /// Shows a picture on the wall, as wide for its height as it is.
    void show(const plane_t& picture) {
        require(fbe_entity_set_scale(
            mesh, static_cast<float>(picture.width) / static_cast<float>(picture.height), 1.0F,
            1.0F, FBE_SPACE_WORLD));
        const std::uint64_t made =
            fbe_texture_create(picture.width, picture.height, FBE_TEXTURE_RGBA16F, false);
        require(made != 0);
        std::vector<float> texels;
        texels.reserve(picture.light.size() * 4);
        for (const float light : picture.light) {
            texels.insert(texels.end(), { light, light, light, 1.0F });
        }
        require(fbe_texture_write_floats(made, 0, 0, picture.width, picture.height, texels.data()));
        require(fbe_texture_set_filter(made, FBE_TEXTURE_NEAREST));
        require(fbe_material_set_map(material, FBE_MAP_EMISSIVE, made, 0));
        fbe_texture_release(map);
        map = made;
    }

    /// What the camera draws, red's light, of a part of the target.
    [[nodiscard]] plane_t drawn(std::int32_t from_x, std::int32_t from_y, std::int32_t across,
                                std::int32_t down) const {
        require(fbe_render_world(scene.world, 1.0F));
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width) *
                                        static_cast<std::size_t>(height) * 8);
        require(fbe_texture_read(target, 0, bytes.data(), static_cast<std::int64_t>(bytes.size())));
        fbe_frame_flip(false);
        plane_t out(across, down);
        for (std::int32_t y = 0; y < down; ++y) {
            for (std::int32_t x = 0; x < across; ++x) {
                std::uint16_t half = 0;
                std::memcpy(&half,
                            &bytes[(((static_cast<std::size_t>(from_y + y) *
                                      static_cast<std::size_t>(width)) +
                                     static_cast<std::size_t>(from_x + x)) *
                                    8)],
                            sizeof(half));
                out.at(x, y) = float_of_half(half);
            }
        }
        return out;
    }

    [[nodiscard]] plane_t drawn() const {
        return drawn(0, 0, width, height);
    }
};

/// A picture dark but for a block and a hot pixel, off center, so what's swapped or flipped shows.
plane_t off_center(std::int32_t width, std::int32_t height) {
    plane_t picture(width, height, 0.1F);
    for (std::int32_t y = height / 4; y < (height / 4) + std::max(2, height / 8); ++y) {
        for (std::int32_t x = width / 5; x < (width / 5) + std::max(2, width / 8); ++x) {
            picture.at(x, y) = 6.0F;
        }
    }
    picture.at((width * 3) / 4, (height * 2) / 3) = 40.0F;
    return picture;
}

/// How many of the last recording's passes are bloom's.
std::size_t bloom_passes() {
    return static_cast<std::size_t>(std::ranges::count(fbe::gpu::last_recorded().passes, "bloom"));
}

/// How many pixels aren't a picture plus what a mirror of it adds, at `intensity`.
int off_the_mirror(const plane_t& seen, const plane_t& picture, const plane_t& added,
                   float intensity) {
    int wrong = 0;
    for (std::int32_t y = 0; y < seen.height; ++y) {
        for (std::int32_t x = 0; x < seen.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(seen.width);
            const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(seen.height);
            const float wanted = picture.at(x, y) + (added.sampled(u, v) * intensity);
            wrong += std::abs(seen.at(x, y) - wanted) > 0.002F + (wanted * 0.02F) ? 1 : 0;
        }
    }
    return wrong;
}

} // namespace

// T4: the whole chain, on sizes neither square nor even, against the mirror: a swap of x and y, a
// flip, a level's weight, Karis's, the tent, or the wrong level's texel all show.
TEST_CASE("bloom's what its passes say, pixel by pixel", "[gpu][scene]") {
    const std::int32_t width = GENERATE(96, 17, 16, 64, 40);
    const std::int32_t height = GENERATE_COPY(width == 96   ? 40
                                              : width == 17 ? 33
                                              : width == 16 ? 16
                                              : width == 40 ? 96
                                                            : 64);
    CAPTURE(width, height);
    const scene_t scene;
    light_rig_t rig(scene, width, height);
    const plane_t picture = off_center(width, height);
    rig.show(picture);
    // Without it, the picture's what's drawn, where it should be, to a unit of a half float, which
    // is at most 1/1024th of what it holds, and at 40 more than 0.01.
    int wrong = 0;
    const plane_t plain = rig.drawn();
    for (std::size_t at = 0; at < plain.light.size(); ++at) {
        const float room = 0.01F + (picture.light[at] / 1024.0F);
        wrong += std::abs(plain.light[at] - picture.light[at]) > room ? 1 : 0;
    }
    CHECK(wrong == 0);
    require(fbe_camera_set_bloom(scene.camera, 0.5F, 1.0F));
    const mirror_t mirror = bloom_mirror(picture);
    const plane_t seen = rig.drawn();
    // B5: every image it needs, up and down, is made before the frame's recorded.
    CHECK(fbe::gpu::last_recorded().made == 0);
    CHECK(bloom_passes() == mirror.passes);
    CHECK(off_the_mirror(seen, picture, mirror.added, 0.5F) == 0);
    CHECK(fbe_gpu_validation_errors() == 0);
}

// B1: only what's in its viewport blooms, in levels of its size, the same wherever it is and
// whatever's around it, or beside it, on one target.
TEST_CASE("a viewport's bloom is what a target its size has", "[gpu][scene]") {
    const plane_t picture = off_center(64, 64);
    const auto alone = [&picture] {
        const scene_t scene;
        light_rig_t rig(scene, 64, 64);
        rig.show(picture);
        require(fbe_camera_set_clear_color(scene.camera, 8.0F, 8.0F, 8.0F, 1.0F));
        require(fbe_camera_set_bloom(scene.camera, 1.0F, 1.0F));
        return rig.drawn();
    }();
    const auto same_as_alone = [&alone](const plane_t& seen) {
        int wrong = 0;
        for (std::size_t at = 0; at < seen.light.size(); ++at) {
            wrong += std::abs(seen.light[at] - alone.light[at]) > 0.002F + (alone.light[at] * 0.02F)
                         ? 1
                         : 0;
        }
        return wrong;
    };
    const scene_t scene;
    light_rig_t rig(scene, 128, 128);
    rig.show(picture);
    require(fbe_camera_set_clear_color(scene.camera, 8.0F, 8.0F, 8.0F, 1.0F));
    require(fbe_camera_set_bloom(scene.camera, 1.0F, 1.0F));
    require(fbe_camera_set_viewport(scene.camera, 32.0F, 32.0F, 64.0F, 64.0F));
    SECTION("away from the target's corner, with bright all around it") {
        CHECK(same_as_alone(rig.drawn(32, 32, 64, 64)) == 0);
        // Its levels, 32, 16, and 8, and nothing of the target's size but its image.
        int larger = 0;
        for (const auto& size : fbe::gpu::last_recorded().transient_sizes) {
            larger += size[0] > 32 && size[0] != 128 ? 1 : 0;
        }
        CHECK(larger == 0);
        CHECK(bloom_passes() == 5);
    }
    SECTION("beside another camera's, each by its own") {
        require(fbe_camera_set_viewport(scene.camera, 0.0F, 0.0F, 64.0F, 64.0F));
        const std::uint64_t other = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
        require(fbe_camera_set_target(other, rig.target));
        require(fbe_camera_set_projection(other, FBE_PROJECTION_ORTHOGRAPHIC));
        require(fbe_camera_set_orthographic_height(other, 2.0F));
        require(fbe_camera_set_clear_color(other, 8.0F, 8.0F, 8.0F, 1.0F));
        require(fbe_camera_set_hdr(other, true));
        require(fbe_camera_set_bloom(other, 1.0F, 1.0F));
        require(fbe_camera_set_viewport(other, 64.0F, 64.0F, 64.0F, 64.0F));
        // Both are drawn, so a read's after the second, of each part of one frame.
        CHECK(same_as_alone(rig.drawn(64, 64, 64, 64)) == 0);
        CHECK(same_as_alone(rig.drawn(0, 0, 64, 64)) == 0);
        require(fbe_entity_free(other));
    }
    CHECK(fbe_gpu_validation_errors() == 0);
}

// B2: however many levels a bigger view has, its glow's at the same parts of it, as bright.
TEST_CASE("bloom's glow is at the same part of a view at any size", "[gpu][scene]") {
    const auto glow_of = [](std::int32_t side) {
        const scene_t scene;
        light_rig_t rig(scene, side, side);
        plane_t picture(side, side);
        for (std::int32_t y = (side * 15) / 32; y < (side * 17) / 32; ++y) {
            for (std::int32_t x = (side * 15) / 32; x < (side * 17) / 32; ++x) {
                picture.at(x, y) = 4.0F;
            }
        }
        rig.show(picture);
        require(fbe_camera_set_bloom(scene.camera, 1.0F, 1.0F));
        plane_t seen = rig.drawn();
        CHECK(bloom_passes() == (side == 256 ? 9U : 15U));
        return seen;
    };
    const plane_t small = glow_of(256);
    const plane_t big = glow_of(2048);
    for (const float part : { 0.125F, 0.1875F, 0.25F, 0.375F }) {
        const auto at_part = [part](const plane_t& seen) {
            const auto from =
                static_cast<std::int32_t>((0.5F + part) * static_cast<float>(seen.width));
            return seen.at(from, seen.height / 2);
        };
        CAPTURE(part, at_part(small), at_part(big));
        CHECK(at_part(small) > 0.0F);
        // Within what their levels' different texels make of a block's edge: before, a part ten
        // times less at 2048 than 1024.
        CHECK(std::abs(at_part(small) - at_part(big)) <= at_part(small) * 0.25F);
    }
    // A long thin one goes down by its short side.
    {
        const scene_t scene;
        light_rig_t rig(scene, 1024, 64);
        rig.show(plane_t(1024, 64, 2.0F));
        require(fbe_camera_set_bloom(scene.camera, 1.0F, 1.0F));
        const plane_t seen = rig.drawn();
        CHECK(bloom_passes() == 5);
        CHECK(std::abs(seen.at(512, 32) - 3.0F) < 0.02F);
    }
}

TEST_CASE("what's too small for bloom has none, and what isn't HDR has none", "[gpu][scene]") {
    const std::int32_t side = GENERATE(8, 15);
    const scene_t scene;
    light_rig_t rig(scene, side, side);
    rig.show(plane_t(side, side, 2.0F));
    require(fbe_camera_set_bloom(scene.camera, 1.0F, 1.0F));
    CHECK(std::abs(rig.drawn().at(side / 2, side / 2) - 2.0F) < 0.01F);
    CHECK(bloom_passes() == 0);
    require(fbe_camera_set_viewport(scene.camera, 0.0F, 0.0F, 4.0F, 4.0F));
    CHECK(std::abs(rig.drawn().at(2, 2) - 2.0F) < 0.01F);
    require(fbe_camera_set_viewport(scene.camera, 0.0F, 0.0F, static_cast<float>(side),
                                    static_cast<float>(side)));
    require(fbe_camera_set_hdr(scene.camera, false));
    CHECK(std::abs(rig.drawn().at(side / 2, side / 2) - 2.0F) < 0.01F);
    CHECK(bloom_passes() == 0);
}

// B3: what's under a camera that doesn't clear stays what it is, whatever its grade's offset; what
// adds adds what its light comes to, over what no light comes to.
TEST_CASE("a grade's offset lights no empty pixel", "[gpu][scene]") {
    const scene_t scene;
    under_it(scene, 0.2F, 0.3F, 0.4F);
    require(fbe_camera_set_hdr(scene.camera, true));
    const fbe::golden::frame_t under = scene.drawn();
    const auto is_under = [&under](const fbe::golden::frame_t& frame) {
        return std::ranges::equal(frame.rgba, under.rgba);
    };
    CHECK(near(middle(under), { srgb(0.2F), srgb(0.3F), srgb(0.4F) }, 1));
    const std::array offset{ 1.0F, 1.0F, 1.0F, 0.1F, 0.1F, 0.1F, 1.0F, 1.0F, 1.0F, 1.0F };
    require(fbe_camera_set_grade(scene.camera, offset.data()));
    CHECK(is_under(scene.drawn()));
    // Through a power, what's no light is what the power makes of the offset.
    const std::array powered{ 1.0F, 1.0F, 1.0F, 0.5F, 0.5F, 0.5F, 2.0F, 2.0F, 2.0F, 0.5F };
    require(fbe_camera_set_grade(scene.camera, powered.data()));
    CHECK(is_under(scene.drawn()));
    // An ADD wall: 0.2 of light is 0.3 graded, 0.2 more than the offset alone.
    require(fbe_camera_set_grade(scene.camera, offset.data()));
    const std::uint64_t adding = fbe_material_create();
    require(fbe_material_set_emissive(adding, 0.2F, 0.2F, 0.2F));
    require(fbe_material_set_blend(adding, FBE_BLEND_ADD));
    const std::uint64_t over = wall(scene.world, -3.0F, 10.0F, adding);
    CHECK(near(middle(scene.drawn()), { srgb(0.4F), srgb(0.5F), srgb(0.6F) }, 2));
    fbe_entity_free(over);
    // What's a surface takes all of it: an opaque wall of 0.5 is 0.6.
    const std::uint64_t solid = glowing_wall(scene, 0.5F, 0.5F, 0.5F);
    CHECK(near(middle(scene.drawn()), { srgb(0.6F), srgb(0.6F), srgb(0.6F) }, 2));
    fbe_material_release(solid);
    fbe_material_release(adding);
}

// Past 1, saturation takes a channel below 0, which is 0: what an offset leaves with no light has
// no less than none of it to take from what's added.
TEST_CASE("a grade's saturation adds no light by what it takes from none", "[gpu][scene]") {
    const scene_t scene;
    under_it(scene, 0.2F, 0.3F, 0.4F);
    require(fbe_camera_set_hdr(scene.camera, true));
    const std::array grade{ 1.0F, 1.0F, 1.0F, 0.5F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F, 2.0F };
    require(fbe_camera_set_grade(scene.camera, grade.data()));
    const std::uint64_t adding = fbe_material_create();
    require(fbe_material_set_emissive(adding, 0.0F, 0.1F, 0.0F));
    require(fbe_material_set_blend(adding, FBE_BLEND_ADD));
    wall(scene.world, -3.0F, 10.0F, adding);
    // (0.5, 0.1, 0) has a luma of 0.17782, so its green's 0.2 - that; (0.5, 0, 0)'s green is 0.
    CHECK(near(middle(scene.drawn()), { srgb(0.2F), srgb(0.3F + 0.2F - 0.17782F), srgb(0.4F) }, 1));
    fbe_material_release(adding);
}

TEST_CASE("a clearing camera's offset stores nothing above its clear alpha of 0", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t floats =
        fbe_texture_create_target(frame_size, frame_size, FBE_TEXTURE_RGBA16F, 1, true);
    require(floats != 0);
    require(fbe_camera_set_target(scene.camera, floats));
    require(fbe_camera_set_hdr(scene.camera, true));
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 0.0F));
    const std::array offset{ 1.0F, 1.0F, 1.0F, 0.1F, 0.1F, 0.1F, 1.0F, 1.0F, 1.0F, 1.0F };
    require(fbe_camera_set_grade(scene.camera, offset.data()));
    require(fbe_render_world(scene.world, 1.0F));
    std::vector<std::uint8_t> bytes(std::size_t{ frame_size } * frame_size * 8);
    require(fbe_texture_read(floats, 0, bytes.data(), static_cast<std::int64_t>(bytes.size())));
    CHECK(std::ranges::all_of(bytes, [](std::uint8_t each) { return each == 0; }));
    require(fbe_camera_set_target(scene.camera, scene.target));
    fbe_texture_release(floats);
}

// B4: a sheet of 1% alpha isn't a hole in the glow of what's beside it, through a curve.
TEST_CASE("bloom's a layer of its own, over a sheet of little alpha as off it", "[gpu][scene]") {
    const scene_t scene;
    under_it(scene, 0.0F, 0.0F, 0.0F);
    require(fbe_camera_set_hdr(scene.camera, true));
    require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_REINHARD));
    require(fbe_camera_set_bloom(scene.camera, 1.0F, 1.0F));
    require(fbe_camera_set_projection(scene.camera, FBE_PROJECTION_ORTHOGRAPHIC));
    require(fbe_camera_set_orthographic_height(scene.camera, 2.0F));
    // Bright to the left, and to its right the sheet's right half of the view.
    const std::uint64_t bright = fbe_material_create();
    require(fbe_material_set_emissive(bright, 16.0F, 16.0F, 16.0F));
    const std::uint64_t source = wall(scene.world, -3.0F, 0.25F, bright);
    require(fbe_entity_set_position(source, -0.3F, 0.0F, -3.0F, FBE_SPACE_WORLD));
    const std::uint64_t thin = fbe_material_create();
    require(fbe_material_set_color(thin, 0.0F, 0.0F, 0.0F, 0.01F));
    require(fbe_material_set_blend(thin, FBE_BLEND_ALPHA));
    const std::uint64_t sheet = wall(scene.world, -2.0F, 1.0F, thin);
    require(fbe_entity_set_position(sheet, 1.25F, 0.0F, -2.0F, FBE_SPACE_WORLD));
    const fbe::golden::frame_t frame = scene.drawn();
    fbe_entity_set_hidden(sheet, true);
    const fbe::golden::frame_t bare = scene.drawn();
    // The sheet's left edge is at x = 0.25 of a view 2 across, at the view's 5/8. Past it, the
    // glow's what it is with no sheet there, but for the 1% of black the sheet is.
    const std::uint32_t edge = (frame_size * 5) / 8;
    for (const std::uint32_t x : { edge + 1, edge + 3, edge + 12 }) {
        const int without = pixel(bare, x, frame_size / 2)[0];
        const int with = pixel(frame, x, frame_size / 2)[0];
        CAPTURE(x, without, with);
        CHECK(without > 40);
        CHECK(std::abs(without - with) <= 3);
    }
    fbe_material_release(bright);
    fbe_material_release(thin);
}

// B5: a level's second image, the one its way up draws into, fails where the camera's drawn.
TEST_CASE("a bloom level's images are made where a camera's drawn", "[gpu][scene]") {
    const scene_t scene;
    light_rig_t rig(scene, 64, 64);
    rig.show(plane_t(64, 64, 2.0F));
    require(fbe_camera_set_bloom(scene.camera, 1.0F, 1.0F));
    // Its image and its first level's first are there, so what fails is the second.
    const fbe::gpu::transient_t image{
        .width = 64,
        .height = 64,
        .format = FBE_TEXTURE_RGBA16F,
    };
    const fbe::gpu::transient_t level{
        .width = 32,
        .height = 32,
        .format = FBE_TEXTURE_RGBA16F,
    };
    require(fbe::gpu::reserve_transient(image));
    require(fbe::gpu::reserve_transient(level));
    fbe::gpu::inject(fbe::gpu::fault_t::VIEW);
    CHECK(!fbe_render_world(scene.world, 1.0F));
    CHECK(std::string(fbe_error()).starts_with("couldn't make a texture's view"));
    // And it's there next time, if it can be.
    CHECK(std::abs(rig.drawn().at(32, 32) - 3.0F) < 0.02F);
    CHECK(fbe::gpu::last_recorded().dropped == 0);
}

// B6: light a grade's power overflows on is as bright as there is, not black, and takes nothing of
// the others' with it.
TEST_CASE("a grade's overflow is white", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = glowing_wall(scene, 2.0F, 2.0F, 2.0F);
    const std::array all{ 1.0F, 1.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1000.0F, 1000.0F, 1000.0F, 1.0F };
    require(fbe_camera_set_grade(scene.camera, all.data()));
    CHECK(near(middle(scene.drawn()), { 255, 255, 255 }, 0));
    require(fbe_material_set_emissive(material, 2.0F, 0.5F, 0.25F));
    const std::array one{ 1.0F, 1.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1000.0F, 1.0F, 1.0F, 1.0F };
    require(fbe_camera_set_grade(scene.camera, one.data()));
    CHECK(near(middle(scene.drawn()), { 255, srgb(0.5F), srgb(0.25F) }, 1));
    fbe_material_release(material);
}

// T1: a NaN and an infinity in what blooms, drawn as the image has them, take nothing from what's
// around them.
TEST_CASE("a NaN and an infinity bloom no more than a firefly does", "[gpu][scene]") {
    constexpr std::int32_t side = 64;
    const scene_t scene;
    light_rig_t rig(scene, side, side);
    rig.show(plane_t(side, side, 2.0F));
    require(fbe_camera_set_bloom(scene.camera, 1.0F, 1.0F));
    const plane_t even = rig.drawn();
    const std::uint16_t one_half = 0x3C00;
    const std::uint16_t two = 0x4000;
    const auto with_bad = [&](std::uint16_t bad) {
        std::vector<std::uint16_t> halves(std::size_t{ side } * side * 4, two);
        for (std::size_t at = 3; at < halves.size(); at += 4) {
            halves[at] = one_half;
        }
        const std::size_t middle_texel = ((std::size_t{ side / 2 } * side) + (side / 2)) * 4;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            halves[middle_texel + channel] = bad;
        }
        const std::uint64_t texture = fbe_texture_create(side, side, FBE_TEXTURE_RGBA16F, false);
        require(texture != 0);
        std::vector<std::uint8_t> bytes(halves.size() * sizeof(std::uint16_t));
        std::memcpy(bytes.data(), halves.data(), bytes.size());
        require(fbe_texture_write(texture, 0, 0, side, side, bytes.data(), side * 8));
        require(fbe_texture_set_filter(texture, FBE_TEXTURE_NEAREST));
        require(fbe_material_set_map(rig.material, FBE_MAP_EMISSIVE, texture, 0));
        const plane_t seen = rig.drawn();
        fbe_texture_release(texture);
        return seen;
    };
    for (const std::uint16_t bad : { std::uint16_t{ 0x7E00 }, std::uint16_t{ 0x7C00 } }) {
        CAPTURE(bad);
        const plane_t seen = with_bad(bad);
        for (const std::int32_t away : { 8, 24 }) {
            const float found = seen.at((side / 2) + away - 1, side / 2);
            CAPTURE(away, found);
            CHECK(std::isfinite(found));
            CHECK(found >= even.at((side / 2) + away - 1, side / 2) * 0.85F);
            CHECK(found <= even.at((side / 2) + away - 1, side / 2) * 1.5F);
        }
    }
}

// T3: where a grade comes among the rest, each by its formula.
TEST_CASE("a grade comes after the tonemap and before the gamma", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = glowing_wall(scene, 1.0F, 0.5F, 0.25F);
    const auto seen = [&](const std::array<float, 10>& grade) {
        require(fbe_camera_set_grade(scene.camera, grade.data()));
        return middle(scene.drawn());
    };
    SECTION("Reinhard, then a slope") {
        require(fbe_camera_set_tonemap(scene.camera, FBE_TONEMAP_REINHARD));
        CHECK(near(seen({ 1.5F, 1.5F, 1.5F, 0, 0, 0, 1, 1, 1, 1 }),
                   { srgb(0.5F * 1.5F), srgb((0.5F / 1.5F) * 1.5F), srgb(0.2F * 1.5F) }, 1));
    }
    SECTION("a slope, then a gamma") {
        require(fbe_camera_set_gamma(scene.camera, 2.0F));
        const auto after = [](float light) { return srgb(std::sqrt(light * 0.5F)); };
        CHECK(near(seen({ 0.5F, 0.5F, 0.5F, 0, 0, 0, 1, 1, 1, 1 }),
                   { after(1.0F), after(0.5F), after(0.25F) }, 1));
    }
    SECTION("a power, then saturation") {
        const std::array powered{ 1.0F, 0.25F, 0.0625F };
        const float luma = (powered[0] * 0.2126F) + (powered[1] * 0.7152F) + (powered[2] * 0.0722F);
        const auto toward = [luma](float each) { return srgb(std::lerp(luma, each, 0.5F)); };
        CHECK(near(seen({ 1, 1, 1, 0, 0, 0, 2, 2, 2, 0.5F }),
                   { toward(powered[0]), toward(powered[1]), toward(powered[2]) }, 1));
    }
    SECTION("what an offset takes below 0 is 0 before the power") {
        CHECK(near(seen({ 1, 1, 1, -0.5F, -0.75F, -0.5F, 0.5F, 0.5F, 0.5F, 1 }),
                   { srgb(std::sqrt(0.5F)), 0, 0 }, 1));
    }
    SECTION("saturation past 1 stops at 0") {
        const float luma = 0.2126F + (0.5F * 0.7152F) + (0.25F * 0.0722F);
        const auto past = [luma](float each) {
            return srgb(std::max(std::lerp(luma, each, 2.0F), 0.0F));
        };
        CHECK(near(seen({ 1, 1, 1, 0, 0, 0, 1, 1, 1, 2 }), { past(1.0F), past(0.5F), past(0.25F) },
                   1));
        CHECK(pixel(scene.drawn(), frame_size / 2, frame_size / 2)[2] == 0);
    }
    fbe_material_release(material);
}

// What came of shadows meeting PBR: a surface's shadows go by the way it faces, not by its map's.
TEST_CASE("a normal map doesn't let a shadowing light past what its surface faces from",
          "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -10.0F, 30.0F, paint);
    const std::uint64_t leaning = normal_map(0.9F, 0.0F);
    require(fbe_material_set_map(paint, FBE_MAP_NORMAL, leaning, 0));
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    shine_from(sun, 0.866F, 0.0F, -0.5F);
    // Behind it, leaning to the light by its map: lit, with nothing to say otherwise.
    CHECK(middle(scene.drawn())[0] > 40);
    require(fbe_light_set_shadows(sun, true));
    CHECK(middle(scene.drawn())[0] == 0);
    // In front, leaning from the light by a little: what the map shades it, shadowed by nothing.
    require(fbe_light_set_shadows(sun, false));
    shine_from(sun, 0.5F, 0.0F, 0.866F);
    const rgba_t lit = middle(scene.drawn());
    require(fbe_light_set_shadows(sun, true));
    CHECK(near(middle(scene.drawn()), { lit[0], lit[1], lit[2] }, 1));
    fbe_material_release(paint);
    fbe_texture_release(leaning);
}

TEST_CASE("a normal map doesn't let a spot light past what its surface faces from",
          "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -10.0F, 30.0F, paint);
    const std::uint64_t leaning = normal_map(0.9F, 0.0F);
    require(fbe_material_set_map(paint, FBE_MAP_NORMAL, leaning, 0));
    const auto spot_at = [&](float x, float z) {
        const std::uint64_t spot = light(scene.world, FBE_LIGHT_SPOT, x, 0.0F, -10.0F + z, 40.0F);
        require(fbe_light_set_cone(spot, 0.5F, 0.7F));
        require(fbe_light_set_intensity(spot, 200.0F));
        shine_from(spot, x / 10.0F, 0.0F, z / 10.0F);
        return spot;
    };
    // Behind it, leaning to the light by its map: lit, with nothing to say otherwise.
    const std::uint64_t behind = spot_at(8.66F, -5.0F);
    CHECK(middle(scene.drawn())[0] > 40);
    require(fbe_light_set_shadows(behind, true));
    CHECK(middle(scene.drawn())[0] == 0);
    fbe_entity_free(behind);
    // In front, leaning from the light by a little: what the map shades it, shadowed by nothing.
    const std::uint64_t in_front = spot_at(5.0F, 8.66F);
    const rgba_t lit = middle(scene.drawn());
    CHECK(lit[0] > 40);
    require(fbe_light_set_shadows(in_front, true));
    CHECK(near(middle(scene.drawn()), { lit[0], lit[1], lit[2] }, 1));
    fbe_material_release(paint);
    fbe_texture_release(leaning);
}

namespace {

/// How open a pixel is, by a CPU's working of shaders/ssao.slang, of a wall 10 off with another
/// coming toward the eye from it at x = 2, seen head on at 128 by 128: all of it, noise and blur.
struct corner_mirror_t {
    static constexpr int side = 128;
    static constexpr std::size_t pixels = 16384;
    float radius = 3.0F;
    float scale = 1.0F / std::tan(std::numbers::pi_v<float> / 6.0F);
    std::array<float, pixels> open{};

    [[nodiscard]] static std::size_t at(int x, int y) {
        return (static_cast<std::size_t>(y) * side) + static_cast<std::size_t>(x);
    }

    [[nodiscard]] float depth_at(int x, int y) const {
        const int at_x = std::clamp(x, 0, side - 1);
        const float across = ((((static_cast<float>(at_x) + 0.5F) / side) * 2.0F) - 1.0F) / scale;
        (void)y;
        return across > 0.0F ? std::min(10.0F, 2.0F / across) : 10.0F;
    }

    [[nodiscard]] std::array<float, 3> place_at(int x, int y, float depth) const {
        const float across = (((static_cast<float>(x) + 0.5F) / side) * 2.0F) - 1.0F;
        const float down = (((static_cast<float>(y) + 0.5F) / side) * 2.0F) - 1.0F;
        return { across / scale * depth, down / -scale * depth, -depth };
    }

    [[nodiscard]] std::array<float, 3> along(int x, int y, int way_x, int way_y) const {
        const std::array<float, 3> place = place_at(x, y, depth_at(x, y));
        const float depth = depth_at(x, y);
        const float forward = depth_at(x + way_x, y + way_y);
        const float backward = depth_at(x - way_x, y - way_y);
        const std::array<float, 3> other = std::fabs(forward - depth) < std::fabs(backward - depth)
                                               ? place_at(x + way_x, y + way_y, forward)
                                               : place_at(x - way_x, y - way_y, backward);
        const float sign = std::fabs(forward - depth) < std::fabs(backward - depth) ? 1.0F : -1.0F;
        return {
            (other[0] - place[0]) * sign,
            (other[1] - place[1]) * sign,
            (other[2] - place[2]) * sign,
        };
    }

    [[nodiscard]] float raw_at(int x, int y) const {
        const float depth = depth_at(x, y);
        const std::array<float, 3> place = place_at(x, y, depth);
        const std::array<float, 3> a = along(x, y, 1, 0);
        const std::array<float, 3> b = along(x, y, 0, 1);
        std::array<float, 3> normal{
            (a[1] * b[2]) - (a[2] * b[1]),
            (a[2] * b[0]) - (a[0] * b[2]),
            (a[0] * b[1]) - (a[1] * b[0]),
        };
        const float length =
            std::sqrt((normal[0] * normal[0]) + (normal[1] * normal[1]) + (normal[2] * normal[2]));
        normal = { normal[0] / length, normal[1] / length, normal[2] / length };
        if ((normal[0] * -place[0]) + (normal[1] * -place[1]) + (normal[2] * -place[2]) < 0.0F) {
            normal = { -normal[0], -normal[1], -normal[2] };
        }
        const float reach = std::min(scale * side * 0.5F * radius / depth, side * 0.1F);
        const float seed = ((static_cast<float>(x) + 0.5F) * 0.06711056F) +
                           ((static_cast<float>(y) + 0.5F) * 0.00583715F);
        const float noise = 52.9829189F * (seed - std::floor(seed));
        const float rotated = noise - std::floor(noise);
        const float bias = (radius * 0.01F) + (depth * 0.00001F);
        float sum = 0.0F;
        for (int i = 0; i < 16; ++i) {
            const float part = (static_cast<float>(i) + 0.5F) / 16.0F;
            const float angle = ((part * 7.0F) + rotated) * 6.28318530718F;
            const int other_x =
                std::clamp(x + static_cast<int>(std::cos(angle) * part * reach), 0, side - 1);
            const int other_y =
                std::clamp(y + static_cast<int>(std::sin(angle) * part * reach), 0, side - 1);
            const std::array<float, 3> to_place =
                place_at(other_x, other_y, depth_at(other_x, other_y));
            const std::array<float, 3> to{
                to_place[0] - place[0],
                to_place[1] - place[1],
                to_place[2] - place[2],
            };
            const float apart = (to[0] * to[0]) + (to[1] * to[1]) + (to[2] * to[2]);
            const float above =
                std::max(((to[0] * normal[0]) + (to[1] * normal[1]) + (to[2] * normal[2]) - bias) /
                             std::sqrt(std::max(apart, 1e-8F)),
                         0.0F);
            sum += above * std::max(1.0F - (apart / (radius * radius)), 0.0F);
        }
        return std::clamp(1.0F - (sum * 2.0F / 16.0F), 0.0F, 1.0F);
    }

    /// One way's blur over what's there, with bytes between, as its targets have.
    void blur(int way_x, int way_y) {
        const std::array<float, pixels> from = open;
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const float depth = depth_at(x, y);
                float sum = from.at(at(x, y));
                float weights = 1.0F;
                for (int i = -4; i <= 4; ++i) {
                    if (i == 0) {
                        continue;
                    }
                    const int other_x = std::clamp(x + (way_x * i), 0, side - 1);
                    const int other_y = std::clamp(y + (way_y * i), 0, side - 1);
                    const float apart =
                        std::fabs(depth_at(other_x, other_y) - depth) / (radius * 0.5F);
                    const float weight =
                        std::exp(-static_cast<float>(i * i) / 8.0F) * std::max(1.0F - apart, 0.0F);
                    sum += from.at(at(other_x, other_y)) * weight;
                    weights += weight;
                }
                open.at(at(x, y)) = std::round(sum / weights * 255.0F) / 255.0F;
            }
        }
    }

    corner_mirror_t() {
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                open.at(at(x, y)) = std::round(raw_at(x, y) * 255.0F) / 255.0F;
            }
        }
        blur(1, 0);
        blur(0, 1);
    }
};

} // namespace

TEST_CASE("ambient occlusion in a corner is what its passes say, pixel by pixel", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_world_set_ambient(scene.world, 0.25F, 0.25F, 0.25F));
    require(fbe_camera_set_ambient_occlusion_radius(scene.camera, 3.0F));
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -10.0F, 30.0F, paint);
    const std::uint64_t side = wall(scene.world, 20.0F, 30.0F, paint);
    turn_about(side, 0.0F, 1.0F, 0.0F, -std::numbers::pi_v<float> * 0.5F);
    require(fbe_entity_set_position(side, 2.0F, 0.0F, 20.0F, FBE_SPACE_WORLD));
    require(fbe_camera_set_ambient_occlusion(scene.camera, 1.0F));
    const fbe::golden::frame_t frame = scene.drawn();
    const auto mirror = std::make_unique<corner_mirror_t>();
    int wrong = 0;
    int dark = 0;
    // The corner's fold itself is a pixel either way, as how it rounds says: it's left out.
    for (std::uint32_t y = 16; y < 112; y += 7) {
        for (std::uint32_t x = 8; x < 120; ++x) {
            if (x >= 84 && x <= 88) {
                continue;
            }
            const float open =
                mirror->open.at(corner_mirror_t::at(static_cast<int>(x), static_cast<int>(y)));
            dark += open < 0.9F ? 1 : 0;
            wrong += std::abs(pixel(frame, x, y)[0] - srgb(0.25F * open)) > 3 ? 1 : 0;
        }
    }
    CAPTURE(wrong, dark);
    CHECK(wrong == 0);
    CHECK(dark > 40);
    fbe_material_release(paint);
}

TEST_CASE("a flat wall's open at any resolution, however it's turned, however far",
          "[gpu][scene]") {
    const float degrees = GENERATE(20.0F, 45.0F);
    const float depth = GENERATE(1.0F, 10.0F, 100.0F);
    const scene_t scene;
    const std::uint64_t big = fbe_texture_create_target(1920, 1080, FBE_TEXTURE_RGBA8, 1, true);
    require(fbe_camera_set_target(scene.camera, big));
    require(fbe_camera_set_range(scene.camera, 0.1F, 10000.0F));
    require(fbe_world_set_ambient(scene.world, 0.25F, 0.25F, 0.25F));
    const std::uint64_t paint = fbe_material_create();
    const std::uint64_t flat = wall(scene.world, -depth, depth * 30.0F, paint);
    turn_about(flat, 0.0F, 1.0F, 0.0F, degrees * std::numbers::pi_v<float> / 180.0F);
    const auto seen = [&] {
        require(fbe_render_world(scene.world, 1.0F));
        fbe::golden::frame_t frame = scene_t::read(big);
        fbe_frame_flip(false);
        return frame;
    };
    const fbe::golden::frame_t without = seen();
    require(fbe_camera_set_ambient_occlusion(scene.camera, 1.0F));
    CHECK(fbe::golden::compare(without, seen(), 1).pixels == 0);
    require(fbe_camera_set_target(scene.camera, scene.target));
    fbe_texture_release(big);
    fbe_material_release(paint);
}

TEST_CASE("a real corner's dark however far off an orthographic camera is", "[gpu][scene]") {
    const scene_t scene;
    const cornered_t room(scene);
    require(fbe_camera_set_projection(scene.camera, FBE_PROJECTION_ORTHOGRAPHIC));
    require(fbe_camera_set_orthographic_height(scene.camera, 12.0F));
    require(fbe_camera_set_range(scene.camera, 1.0F, 5000.0F));
    require(fbe_entity_set_position(scene.camera, 0.0F, 0.0F, 2000.0F, FBE_SPACE_WORLD));
    // Standing off the wall by less than a radius, which a bias of depth's part would be past.
    require(fbe_camera_set_ambient_occlusion_radius(scene.camera, 2.0F));
    require(fbe_entity_set_scale(room.block, 1.0F, 1.0F, 0.4F, FBE_SPACE_WORLD));
    require(fbe_entity_set_position(room.block, 0.0F, 0.0F, -9.6F, FBE_SPACE_WORLD));
    const fbe::golden::frame_t without = scene.drawn();
    require(fbe_camera_set_ambient_occlusion(scene.camera, 1.0F));
    const std::array<std::uint32_t, 2> by_it{ 64 + 12, 64 };
    CHECK(pixel_of(scene.drawn(), by_it)[0] < pixel_of(without, by_it)[0] - 4);
}

TEST_CASE("what's thin close up still darkens what's by it", "[gpu][scene]") {
    // Its radius past all of the view: a tenth of its height's what its samples reach, so some of
    // them come upon a bar 40 pixels wide, where the first of them'd go 146 pixels without that.
    const scene_t scene;
    const std::uint64_t big = fbe_texture_create_target(1920, 1080, FBE_TEXTURE_RGBA8, 1, true);
    require(fbe_camera_set_target(scene.camera, big));
    require(fbe_camera_set_range(scene.camera, 0.01F, 100.0F));
    require(fbe_world_set_ambient(scene.world, 0.25F, 0.25F, 0.25F));
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -0.2F, 30.0F, paint);
    const std::uint64_t bar = cube(scene.world, 0.0F, 0.0F, -0.19F, paint);
    require(fbe_entity_set_scale(bar, 0.0043F, 5.0F, 0.01F, FBE_SPACE_WORLD));
    require(fbe_camera_set_ambient_occlusion(scene.camera, 1.0F));
    require(fbe_render_world(scene.world, 1.0F));
    const fbe::golden::frame_t frame = scene_t::read(big);
    fbe_frame_flip(false);
    int darkened = 0;
    for (std::uint32_t y = 500; y < 580; ++y) {
        darkened += pixel(frame, 960 + 40, y)[0] < srgb(0.25F) - 1 ? 1 : 0;
    }
    CAPTURE(darkened);
    CHECK(darkened == 80);
    require(fbe_camera_set_target(scene.camera, scene.target));
    fbe_texture_release(big);
    fbe_material_release(paint);
}

TEST_CASE("what a cut-out samples is ready for its prepass", "[gpu][scene]") {
    // A target another camera draws this frame, as the layer a cut-out's cut by.
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t layer = fbe_texture_create_target(16, 16, FBE_TEXTURE_RGBA8, 1, true);
    const std::uint64_t first = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
    require(fbe_camera_set_target(first, layer));
    require(fbe_camera_set_clear_color(first, 1.0F, 1.0F, 1.0F, 1.0F));
    require(fbe_camera_set_order(first, -1));
    const std::uint64_t paint = fbe_material_create();
    require(fbe_material_set_shading(paint, FBE_SHADING_UNLIT));
    require(fbe_material_set_alpha_cutoff(paint, 0.5F));
    require(fbe_material_set_texture(paint, 0, layer));
    cube(scene.world, 0.0F, 0.0F, -6.0F, paint);
    require(fbe_camera_set_ambient_occlusion(scene.camera, 1.0F));
    const std::int32_t errors = fbe_gpu_validation_errors();
    const fbe::golden::frame_t frame = scene.drawn();
    CHECK(near(middle(frame), { 255, 255, 255 }));
    CHECK(fbe_gpu_validation_errors() == errors);
    // In its order too: the camera's, then a barrier, then the prepass that samples it.
    (void)scene.drawn();
    const fbe::gpu::recorded_t& recorded = fbe::gpu::last_recorded();
    const auto at = [&](std::string_view name) {
        return recorded.levels.at(static_cast<std::size_t>(
            std::ranges::find(recorded.passes, name) - recorded.passes.begin()));
    };
    CHECK(at("camera") < at("prepass"));
    fbe_texture_release(layer);
    fbe_material_release(paint);
}

TEST_CASE("what a camera had goes stale when it isn't drawn, or draws elsewhere", "[gpu][scene]") {
    const scene_t scene;
    const edge_t edge(scene);
    for (int i = 0; i < 12; ++i) {
        (void)scene.drawn();
    }
    REQUIRE(linear_off(pixel(scene.drawn(), edge_at, edge_row), 0.5F) < 0.4F);
    SECTION("sixty frames undrawn, with what's in them moved") {
        edge.place(-20.0F);
        for (int i = 0; i < 60; ++i) {
            fbe_frame_flip(false);
        }
        edge.place(0.0F);
        const rgba_t back = pixel(scene.drawn(), edge_at, edge_row);
        CAPTURE(back[0]);
        CHECK((back[0] == 0 || back[0] == 255));
    }
    SECTION("every other frame isn't stale") {
        for (int i = 0; i < 12; ++i) {
            fbe_frame_flip(false);
            (void)scene.drawn();
        }
        const rgba_t kept = pixel(scene.drawn(), edge_at, edge_row);
        CAPTURE(kept[0]);
        CHECK((kept[0] != 0 && kept[0] != 255));
    }
    SECTION("another target of its size") {
        const std::uint64_t other =
            fbe_texture_create_target(frame_size, frame_size, FBE_TEXTURE_RGBA8, 1, true);
        require(fbe_camera_set_target(scene.camera, other));
        require(fbe_render_world(scene.world, 1.0F));
        const rgba_t fresh = pixel(scene_t::read(other), edge_at, edge_row);
        fbe_frame_flip(false);
        CAPTURE(fresh[0]);
        CHECK((fresh[0] == 0 || fresh[0] == 255));
        require(fbe_camera_set_target(scene.camera, scene.target));
        fbe_texture_release(other);
    }
}

TEST_CASE("what's over the scene softens only so much in a pan", "[gpu][scene]") {
    // A bar on the camera, in another order, so it has no motion, against a pan over stripes.
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    require(fbe_camera_set_hdr(scene.camera, true));
    require(fbe_camera_set_temporal_aa(scene.camera, true));
    const std::uint64_t paint = unlit(0.3F, 0.3F, 0.3F);
    wall(scene.world, -10.0F, 30.0F, paint);
    const std::uint64_t over = unlit(1.0F, 1.0F, 1.0F);
    require(fbe_material_set_order(over, 1));
    const std::uint64_t bar = cube(scene.world, 0.0F, 0.0F, -6.0F, over);
    require(fbe_entity_set_parent(bar, scene.camera, false));
    fbe::golden::frame_t panned;
    for (int i = 0; i < 12; ++i) {
        require(fbe_entity_set_position(scene.camera, static_cast<float>(i) * 0.2F, 0.0F, 0.0F,
                                        FBE_SPACE_WORLD));
        panned = scene.drawn();
    }
    require(fbe_camera_set_temporal_aa(scene.camera, false));
    const fbe::golden::frame_t still = scene.drawn();
    // Well inside it and well outside it, as it was; about its edges, a few pixels softened.
    CHECK(near(middle(panned), { 255, 255, 255 }, 2));
    const auto softened = fbe::golden::compare(still, panned, 8).pixels;
    CAPTURE(softened);
    CHECK(softened < 400);
    fbe_material_release(paint);
    fbe_material_release(over);
}

TEST_CASE("antialiasing over frames works by each camera, mirrored or of many", "[gpu][scene]") {
    const scene_t scene;
    const edge_t edge(scene);
    SECTION("through a mirror's reflection, what's flat stays") {
        wall_mirror(scene.world, -20.0F);
        fbe::golden::frame_t last;
        for (int i = 0; i < 12; ++i) {
            last = scene.drawn();
        }
        CHECK(pixel(last, edge_at + 8, edge_row)[0] == 255);
    }
    SECTION("an Instances moved by its entity") {
        require(fbe_entity_set_hidden(edge.white, true));
        const fbe_instance_t big = instance_at(0.0F, 0.0F, 0.0F, 3.0F);
        const std::uint64_t made = cubes_at(scene.world, edge.paint, { big });
        fbe::golden::frame_t last;
        for (int frame = 0; frame <= 24; ++frame) {
            const float pixels = static_cast<float>(frame - 24) * 2.0F;
            // Its front's at 7 from the eye: pixels there are 0.7 of a wall's.
            const float across = edge.pixel * 0.7F;
            require(fbe_entity_set_position(made, 3.0F + ((0.5F + pixels) * across), 0.0F, -10.0F,
                                            FBE_SPACE_WORLD));
            last = scene.drawn();
        }
        CAPTURE(pixel(last, edge_at, edge_row)[0]);
        CHECK(linear_off(pixel(last, edge_at, edge_row), 0.5F) < 0.2F);
    }
    SECTION("two cameras on one moving entity each keep their own") {
        const std::uint64_t other_target =
            fbe_texture_create_target(frame_size, frame_size, FBE_TEXTURE_RGBA8, 1, true);
        const std::uint64_t other = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
        require(fbe_camera_set_target(other, other_target));
        require(fbe_camera_set_clear_color(other, 0.0F, 0.0F, 0.0F, 1.0F));
        require(fbe_camera_set_hdr(other, true));
        require(fbe_camera_set_temporal_aa(other, true));
        require(fbe_entity_set_position(other, 0.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
        fbe::golden::frame_t first;
        fbe::golden::frame_t second;
        for (int frame = 0; frame <= 24; ++frame) {
            edge.place(static_cast<float>(frame - 24) * 2.0F);
            require(fbe_render_world(scene.world, 1.0F));
            first = scene_t::read(scene.target);
            second = scene_t::read(other_target);
            fbe_frame_flip(false);
        }
        CHECK(fbe::render::history_count() == 2);
        CHECK(linear_off(pixel(first, edge_at, edge_row), 0.5F) < 0.2F);
        CHECK(linear_off(pixel(second, edge_at, edge_row), 0.5F) < 0.2F);
        fbe_texture_release(other_target);
    }
    SECTION("jitter's none of what's said of a view but what's drawn") {
        std::array<fbe_draw_t, 4> before{};
        REQUIRE(fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, before.data(), 4) == 1);
        (void)scene.drawn();
        (void)scene.drawn();
        std::array<fbe_draw_t, 4> after{};
        REQUIRE(fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, after.data(), 4) == 1);
        CHECK(std::ranges::equal(before[0].matrix, after[0].matrix));
        fbe_vec3_t point{};
        fbe_vec3_t point_after{};
        fbe_mat4_t projection{};
        fbe_mat4_t projection_after{};
        REQUIRE(fbe_camera_unproject(scene.camera, 128.0F, 128.0F, 64.5F, 64.5F, 10.0F, &point) ==
                1);
        require(fbe_camera_draw_projection(scene.camera, 128.0F, 128.0F, &projection));
        (void)scene.drawn();
        REQUIRE(fbe_camera_unproject(scene.camera, 128.0F, 128.0F, 64.5F, 64.5F, 10.0F,
                                     &point_after) == 1);
        require(fbe_camera_draw_projection(scene.camera, 128.0F, 128.0F, &projection_after));
        CHECK(point.x == point_after.x);
        CHECK(point.y == point_after.y);
        CHECK(std::ranges::equal(projection.m, projection_after.m));
    }
    CHECK(fbe_gpu_validation_errors() == 0);
}

TEST_CASE("what's reflected or a decal has what it has of ambient occlusion", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_world_set_ambient(scene.world, 0.25F, 0.25F, 0.25F));
    require(fbe_camera_set_ambient_occlusion_radius(scene.camera, 3.0F));
    const std::uint64_t paint = fbe_material_create();
    const auto without_then_with = [&] {
        require(fbe_camera_set_ambient_occlusion(scene.camera, 0.0F));
        const fbe::golden::frame_t without = scene.drawn();
        require(fbe_camera_set_ambient_occlusion(scene.camera, 1.0F));
        return std::pair{ without, scene.drawn() };
    };
    SECTION("what's seen in a mirror takes none: all that's dark's where nothing of the scene is") {
        // A wall behind the eye, seen in a mirror ahead of it, with a block standing off the
        // mirror.
        const std::uint64_t behind = wall(scene.world, 10.0F, 30.0F, paint);
        turn_about(behind, 0.0F, 1.0F, 0.0F, std::numbers::pi_v<float>);
        wall_mirror(scene.world, -10.0F);
        cube(scene.world, 0.0F, 0.0F, -8.0F, paint);
        const auto [without, with] = without_then_with();
        const std::array<std::uint32_t, 2> reflected{ 64 + 40, 64 };
        CHECK(pixel_of(with, reflected) == pixel_of(without, reflected));
    }
    SECTION("a decal on a wall") {
        wall(scene.world, -4.0F, 30.0F, paint);
        std::array<float, 16> box{};
        box[0] = 1.0F;
        box[5] = 1.0F;
        box[10] = 1.0F;
        box[14] = -4.0F;
        box[15] = 1.0F;
        fbe_mat4_t matrix{};
        std::ranges::copy(box, matrix.m);
        const std::uint64_t geometry = fbe_geometry_decal(scene.world, &matrix, 0.01F, 1.309F);
        REQUIRE(geometry != 0);
        const std::uint64_t opaque_decal = fbe_material_create();
        const std::uint64_t decal = fbe_entity_create(scene.world, 0, FBE_ENTITY_MESH);
        require(fbe_mesh_set_geometry(decal, geometry));
        require(fbe_mesh_set_material(decal, opaque_decal));
        fbe_geometry_release(geometry);
        const auto [without, with] = without_then_with();
        CHECK(fbe::golden::compare(without, with, 1).pixels == 0);
        fbe_material_release(opaque_decal);
    }
    fbe_material_release(paint);
}

namespace {

/// How much of the next pixel over FXAA takes, on a pixel beside a straight edge between all and
/// nothing: what's around it is a third of the way to its other side, and an edge that doesn't end
/// has nothing else to say.
float fxaa_small_part() {
    const float apart = 1.0F / 3.0F;
    const float eased = ((-2.0F * apart) + 3.0F) * apart * apart;
    return eased * eased * 0.75F;
}

/// A white wall whose left edge is between pixels 63 and 64, on black, unlit.
std::uint64_t edge_wall(const scene_t& scene, std::uint64_t paint) {
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t made = wall(scene.world, -10.0F, 30.0F, paint);
    require(fbe_entity_set_position(made, 30.0F, 0.0F, -10.0F, FBE_SPACE_WORLD));
    return made;
}

} // namespace

TEST_CASE("fxaa softens an edge by what it says, and nothing else", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = unlit(1.0F, 1.0F, 1.0F);
    const std::uint64_t made = edge_wall(scene, paint);
    const fbe::golden::frame_t sharp = scene.drawn();
    CHECK(pixel(sharp, 63, 64)[0] == 0);
    CHECK(pixel(sharp, 64, 64)[0] == 255);
    CHECK_FALSE(fbe_camera_fxaa(scene.camera));
    CHECK(std::ranges::count(passes_of(scene), std::string_view("fxaa")) == 0);
    require(fbe_camera_set_fxaa(scene.camera, true));
    CHECK(fbe_camera_fxaa(scene.camera));
    const fbe::golden::frame_t soft = scene.drawn();
    const float part = fxaa_small_part();
    SECTION("each side of it takes that part of the other, and no more") {
        CAPTURE(int{ pixel(soft, 63, 64)[0] }, int{ pixel(soft, 64, 64)[0] });
        const std::uint8_t dark = srgb(part);
        const std::uint8_t light = srgb(1.0F - part);
        CHECK(near(pixel(soft, 63, 64), { dark, dark, dark }, 3));
        CHECK(near(pixel(soft, 64, 64), { light, light, light }, 3));
        CHECK(pixel(soft, 62, 64)[0] == 0);
        CHECK(pixel(soft, 65, 64)[0] == 255);
    }
    SECTION("what's flat is as it was, all of it") {
        require(fbe_entity_set_position(made, 0.0F, 0.0F, -10.0F, FBE_SPACE_WORLD));
        CHECK(near(middle(scene.drawn()), { 255, 255, 255 }, 0));
    }
    SECTION("what's under its least contrast is as it was, even a slope's stairs") {
        const std::uint64_t gray = unlit(0.5F, 0.5F, 0.5F);
        require(fbe_mesh_set_material(made, gray));
        require(fbe_camera_set_clear_color(scene.camera, 0.58F, 0.58F, 0.58F, 1.0F));
        turn_about(made, 0.0F, 0.0F, 1.0F, 0.2F);
        const fbe::golden::frame_t low = scene.drawn();
        require(fbe_camera_set_fxaa(scene.camera, false));
        CHECK(fbe::golden::compare(scene.drawn(), low, 0).pixels == 0);
        fbe_material_release(gray);
    }
    SECTION("a slope's stairs go, near each's ends") {
        turn_about(made, 0.0F, 0.0F, 1.0F, 0.2F);
        require(fbe_entity_set_position(made, 30.0F, 0.0F, -10.0F, FBE_SPACE_WORLD));
        const fbe::golden::frame_t slope = scene.drawn();
        int between = 0;
        for (std::uint32_t y = 20; y < 108; ++y) {
            for (std::uint32_t x = 20; x < 108; ++x) {
                const int red = pixel(slope, x, y)[0];
                between += red > 90 && red < 200 ? 1 : 0;
            }
        }
        CAPTURE(between);
        CHECK(between > 30);
    }
    SECTION("it's after its tonemap, its bloom, and what's over frames") {
        require(fbe_camera_set_hdr(scene.camera, true));
        require(fbe_camera_set_bloom(scene.camera, 0.1F, 2.0F));
        require(fbe_camera_set_temporal_aa(scene.camera, true));
        const std::vector<std::string_view> passes = passes_of(scene);
        const auto at = [&](std::string_view name) { return std::ranges::find(passes, name); };
        CHECK(at("temporal resolve") < at("bloom"));
        CHECK(at("bloom") < at("tonemap"));
        CHECK(at("tonemap") < at("fxaa"));
        CHECK(at("fxaa") + 1 == passes.end());
    }
    SECTION("an HDR camera's the same") {
        require(fbe_camera_set_hdr(scene.camera, true));
        const fbe::golden::frame_t hdr = scene.drawn();
        CHECK(fbe::golden::compare(soft, hdr, 2).pixels == 0);
        CHECK(fbe_gpu_validation_errors() == 0);
    }
    SECTION("needing no prepass, and no history") {
        const std::vector<std::string_view> passes = passes_of(scene);
        CHECK(std::ranges::count(passes, std::string_view("prepass")) == 0);
        CHECK(fbe::render::history_count() == 0);
    }
    SECTION("through many samples") {
        const std::uint64_t many = fbe_texture_create_target(128, 128, FBE_TEXTURE_RGBA8, 4, true);
        require(fbe_camera_set_target(scene.camera, many));
        require(fbe_render_world(scene.world, 1.0F));
        const fbe::golden::frame_t by_many = scene_t::read(many);
        fbe_frame_flip(false);
        CHECK(pixel(by_many, 60, 64)[0] == 0);
        CHECK(pixel(by_many, 68, 64)[0] == 255);
        require(fbe_camera_set_target(scene.camera, scene.target));
        fbe_texture_release(many);
    }
    SECTION("in a target of half floats, or bytes without sRGB") {
        const std::int32_t format = GENERATE(FBE_TEXTURE_RGBA16F, FBE_TEXTURE_RGBA8_LINEAR);
        const std::uint64_t other = fbe_texture_create_target(128, 128, format, 1, true);
        require(fbe_camera_set_target(scene.camera, other));
        require(fbe_render_camera(scene.camera, 1.0F));
        fbe_frame_flip(false);
        CHECK(fbe_gpu_validation_errors() == 0);
        require(fbe_camera_set_target(scene.camera, scene.target));
        fbe_texture_release(other);
    }
    SECTION("only in its viewport, over what's there if it doesn't clear") {
        // A first camera's blue under a second's wall, whose FXAA leaves what's off it alone.
        const bool hdr = GENERATE(false, true);
        const bool clears = GENERATE(false, true);
        const std::uint64_t under = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
        require(fbe_camera_set_target(under, scene.target));
        require(fbe_camera_set_clear_color(under, 0.0F, 0.0F, 1.0F, 1.0F));
        require(fbe_camera_set_order(under, -1));
        require(fbe_camera_set_hdr(scene.camera, hdr));
        require(fbe_camera_set_clear(scene.camera, clears, true));
        require(fbe_camera_set_viewport(scene.camera, 32.0F, 32.0F, 64.0F, 64.0F));
        const fbe::golden::frame_t over = scene.drawn();
        CAPTURE(hdr, clears);
        CHECK(near(pixel(over, 8, 8), { 0, 0, 255 }, 1));
        const std::uint8_t blue = clears ? 0 : 255;
        CHECK(near(pixel(over, 40, 64), { 0, 0, blue }, 1));
        CHECK(near(pixel(over, 90, 64), { 255, 255, 255 }, 1));
    }
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(paint);
}

namespace {

/// An instance at a place, of a matrix that mirrors along X, or not, and is turned, so it's seen on
/// more than one side.
fbe_instance_t mirrored_at(float x, float y, float z, float scale = 1.0F) {
    fbe_instance_t made = instance_at(x, y, z, scale);
    made.matrix.m[0] = -scale;
    return made;
}

} // namespace

TEST_CASE("a one-sided Instances that mirrors draws as a mesh does", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    turn_about(sun, 0.3F, 1.0F, 0.2F, 0.5F);
    const std::uint64_t paint = fbe_material_create();
    const std::uint64_t mesh = wall(scene.world, -3.0F, 0.75F, paint);
    require(fbe_entity_set_scale(mesh, -1.0F, 1.0F, 1.0F, FBE_SPACE_WORLD));
    const fbe::golden::frame_t as_a_mesh = scene.drawn();
    REQUIRE(middle(as_a_mesh)[0] > 20);
    require(fbe_entity_set_hidden(mesh, true));
    const std::uint64_t many = fbe_entity_create(scene.world, 0, FBE_ENTITY_INSTANCES);
    require(fbe_instances_set_geometry(many, fbe_mesh_geometry(mesh)));
    require(fbe_instances_set_material(many, paint));
    fbe_instance_t one = mirrored_at(0.0F, 0.0F, -3.0F);
    require(fbe_instances_set(many, &one, 1));
    CHECK(fbe::golden::compare(as_a_mesh, scene.drawn(), 2).pixels == 0);
    const auto pipeline = [&] {
        std::array<fbe_draw_t, 4> draws{};
        REQUIRE(fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, draws.data(), 4) == 1);
        return draws[0].pipeline;
    };
    const std::uint64_t front_bit = std::uint64_t{ 1 } << 12U;
    CHECK((pipeline() & front_bit) != 0);
    SECTION("and its entity mirroring too has it as it was") {
        require(fbe_entity_set_scale(many, -1.0F, 1.0F, 1.0F, FBE_SPACE_WORLD));
        // At -3 still, as X's nothing to it: mirrored twice, its front's where a plain one's is.
        CHECK((pipeline() & front_bit) == 0);
        CHECK(middle(scene.drawn())[0] > 20);
    }
    SECTION("an entity mirroring what doesn't has what does") {
        one = instance_at(0.0F, 0.0F, -3.0F);
        require(fbe_instances_set(many, &one, 1));
        CHECK((pipeline() & front_bit) == 0);
        require(fbe_entity_set_scale(many, -1.0F, 1.0F, 1.0F, FBE_SPACE_WORLD));
        CHECK((pipeline() & front_bit) != 0);
        CHECK(fbe::golden::compare(as_a_mesh, scene.drawn(), 2).pixels == 0);
    }
    SECTION("two-sided, seen from behind or not, lit as a mesh is") {
        const std::uint64_t both_sides = fbe_material_create();
        require(fbe_material_set_two_sided(both_sides, true));
        require(fbe_mesh_set_material(mesh, both_sides));
        require(fbe_instances_set_material(many, both_sides));
        const float turn = GENERATE(0.0F, 3.0F);
        turn_about(mesh, 0.0F, 1.0F, 0.0F, turn);
        turn_about(many, 0.0F, 0.0F, 1.0F, 0.0F);
        require(fbe_entity_set_hidden(many, true));
        require(fbe_entity_set_hidden(mesh, false));
        const fbe::golden::frame_t mesh_frame = scene.drawn();
        require(fbe_entity_set_hidden(mesh, true));
        require(fbe_entity_set_hidden(many, false));
        fbe_mat4_t matrix{};
        require(fbe_entity_matrix(mesh, FBE_SPACE_WORLD, &matrix));
        one.matrix = matrix;
        require(fbe_instances_set(many, &one, 1));
        CHECK(fbe::golden::compare(mesh_frame, scene.drawn(), 2).pixels == 0);
        fbe_material_release(both_sides);
    }
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(paint);
}

TEST_CASE("an Instances' mirrored and unmirrored go in two draws, sorted", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t paint = unlit(1.0F, 1.0F, 1.0F);
    std::vector<fbe_instance_t> all;
    all.reserve(400);
    // Indices one of each by turns, so where each goes isn't where it was given.
    for (int i = 0; i < 400; ++i) {
        const auto x = static_cast<float>(i % 20) - 10.0F;
        const float y = std::floor(static_cast<float>(i) / 20.0F) - 10.0F;
        all.push_back(i % 2 == 0 ? instance_at(x, y, -30.0F, 0.2F)
                                 : mirrored_at(x, y, -30.0F, 0.2F));
    }
    const std::uint64_t made = cubes_at(scene.world, paint, all);
    std::array<fbe_draw_t, 4> draws{};
    REQUIRE(fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, draws.data(), 4) == 2);
    const std::uint64_t front_bit = std::uint64_t{ 1 } << 12U;
    CHECK((draws[0].pipeline & front_bit) != (draws[1].pipeline & front_bit));
    CHECK(draws[0].entity == made);
    CHECK(draws[1].entity == made);
    // Every one's drawn, once.
    (void)scene.drawn();
    fbe_gpu_counters_t counters{};
    (void)scene.drawn();
    require(fbe_gpu_counters(&counters));
    CHECK(counters.triangles == std::int64_t{ 400 } * 12);
    // Each by what it was given as, wherever it went.
    for (int i = 0; i < 400; i += 37) {
        fbe_instance_t got{};
        require(fbe_instances_get(made, i, &got));
        CHECK(got.matrix.m[0] == all[static_cast<std::size_t>(i)].matrix.m[0]);
        CHECK(got.matrix.m[12] == all[static_cast<std::size_t>(i)].matrix.m[12]);
    }
    SECTION("all of one kind's one draw") {
        const std::vector<fbe_instance_t> plain(10, instance_at(0.0F, 0.0F, -30.0F));
        require(fbe_instances_set(made, plain.data(), 10));
        CHECK(fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, draws.data(), 4) == 1);
        const std::vector<fbe_instance_t> mirrored(10, mirrored_at(0.0F, 0.0F, -30.0F));
        require(fbe_instances_set(made, mirrored.data(), 10));
        CHECK(fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, draws.data(), 4) == 1);
    }
    SECTION("one set to mirror where it didn't goes with those that do") {
        const std::vector<fbe_instance_t> plain(10, instance_at(0.0F, 0.0F, -30.0F));
        require(fbe_instances_set(made, plain.data(), 10));
        const fbe_instance_t turned = mirrored_at(3.0F, 0.0F, -30.0F);
        require(fbe_instances_set_one(made, 4, &turned));
        CHECK(fbe_instances_count(made) == 10);
        CHECK(fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, draws.data(), 4) == 2);
        fbe_instance_t got{};
        require(fbe_instances_get(made, 4, &got));
        CHECK(got.matrix.m[0] == -1.0F);
        CHECK(got.matrix.m[12] == 3.0F);
        CHECK(reds_above(scene.drawn(), 0) > 0);
        // And back: one draw again, and what it was.
        const fbe_instance_t back = instance_at(0.0F, 0.0F, -30.0F);
        require(fbe_instances_set_one(made, 4, &back));
        CHECK(fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, draws.data(), 4) == 1);
        require(fbe_instances_get(made, 4, &got));
        CHECK(got.matrix.m[0] == 1.0F);
    }
    SECTION("one of those after moved takes its own cluster with it") {
        const fbe_instance_t far_off = mirrored_at(1000.0F, 0.0F, -30.0F);
        (void)scene.drawn();
        fbe_aabb_t box{};
        require(fbe_entity_bounds(made, &box));
        require(fbe_instances_set_one(made, 1, &far_off));
        require(fbe_entity_bounds(made, &box));
        CHECK(box.max.x > 999.0F);
        const fbe_instance_t back = all[1];
        require(fbe_instances_set_one(made, 1, &back));
        require(fbe_entity_bounds(made, &box));
        CHECK(box.max.x < 11.0F);
    }
    SECTION("a level for each of its kinds") {
        const std::uint64_t geometry = fbe_instances_geometry(made);
        const std::uint64_t coarse = fbe_geometry_cube();
        const float half_height = std::tan(1.0471976F / 2.0F);
        require(fbe_geometry_set_lod(geometry, 0, coarse,
                                     std::numbers::sqrt3_v<float> * 0.2F / (half_height * 2.0F)));
        std::array<fbe_draw_t, 8> found{};
        const std::int32_t count =
            fbe_render_draws(scene.camera, 128.0F, 128.0F, 1.0F, found.data(), 8);
        CHECK(count == 2);
        CHECK(found[0].geometry == coarse);
        CHECK(found[1].geometry == coarse);
        fbe_geometry_release(coarse);
    }
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(paint);
}

TEST_CASE("an Instances that mirrors casts, and is in the prepass, as a mesh is", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    const std::uint64_t mesh = cube(scene.world, 0.0F, 0.0F, -9.0F, paint);
    require(fbe_entity_set_scale(mesh, -1.0F, 1.0F, 1.0F, FBE_SPACE_WORLD));
    wall(scene.world, -10.0F, 30.0F, paint);
    const auto with_instance_for_mesh = [&](auto&& check) {
        const fbe::golden::frame_t by_mesh = scene.drawn();
        require(fbe_entity_set_hidden(mesh, true));
        const std::uint64_t made = cubes_at(scene.world, paint, { mirrored_at(0.0F, 0.0F, -9.0F) });
        const fbe::golden::frame_t by_instance = scene.drawn();
        check(by_mesh, by_instance);
        require(fbe_entity_set_hidden(mesh, false));
        require(fbe_entity_free(made));
    };
    SECTION("its shadow") {
        const std::uint64_t sun = sun_from_right(scene.world);
        require(fbe_entity_set_position(mesh, 5.0F, 0.0F, -5.0F, FBE_SPACE_WORLD));
        const fbe::golden::frame_t by_mesh = scene.drawn();
        REQUIRE(middle(by_mesh)[0] == 0);
        require(fbe_entity_set_hidden(mesh, true));
        const std::uint64_t made = cubes_at(scene.world, paint, { mirrored_at(5.0F, 0.0F, -5.0F) });
        CHECK(fbe::golden::compare(by_mesh, scene.drawn(), 2).pixels == 0);
        // Each of a mix casts once, of its own kind's.
        require(fbe_light_set_cascades(sun, 1));
        const std::array mixed{ mirrored_at(5.0F, 0.0F, -5.0F), instance_at(5.0F, 0.0F, -8.0F) };
        require(fbe_instances_set(made, mixed.data(), 2));
        fbe_gpu_counters_t counters{};
        (void)scene.drawn();
        (void)scene.drawn();
        require(fbe_gpu_counters(&counters));
        const std::int64_t casting = counters.triangles;
        require(fbe_entity_set_casts_shadows(made, false));
        (void)scene.drawn();
        (void)scene.drawn();
        require(fbe_gpu_counters(&counters));
        CHECK(casting - counters.triangles == 24);
    }
    SECTION("its ambient occlusion") {
        require(fbe_world_set_ambient(scene.world, 0.25F, 0.25F, 0.25F));
        require(fbe_camera_set_ambient_occlusion_radius(scene.camera, 3.0F));
        require(fbe_camera_set_ambient_occlusion(scene.camera, 1.0F));
        with_instance_for_mesh(
            [](const fbe::golden::frame_t& by_mesh, const fbe::golden::frame_t& by_instance) {
                REQUIRE(pixel(by_mesh, 64 + 16, 64)[0] < srgb(0.25F) - 8);
                CHECK(fbe::golden::compare(by_mesh, by_instance, 1).pixels == 0);
            });
    }
    SECTION("its motion") {
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
        require(fbe_camera_set_hdr(scene.camera, true));
        require(fbe_camera_set_temporal_aa(scene.camera, true));
        require(fbe_entity_set_hidden(mesh, true));
        const std::uint64_t white = unlit(1.0F, 1.0F, 1.0F);
        const std::uint64_t made =
            cubes_at(scene.world, white, { mirrored_at(0.0F, 0.0F, 0.0F, 3.0F) });
        // Its front's at 7 from the eye; two whole pixels a frame.
        const float across = std::tan(std::numbers::pi_v<float> / 6.0F) * 7.0F / 64.0F;
        require(fbe_entity_set_position(scene.camera, 0.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
        fbe::golden::frame_t last;
        for (int frame = 0; frame <= 24; ++frame) {
            const float pixels = static_cast<float>(frame - 24) * 2.0F;
            require(fbe_entity_set_position(made, 3.0F + ((0.5F + pixels) * across), 0.0F, -10.0F,
                                            FBE_SPACE_WORLD));
            last = scene.drawn();
        }
        const float value = static_cast<float>(pixel(last, 64, 64)[0]) / 255.0F;
        CAPTURE(value);
        CHECK(value > 0.4F);
        CHECK(value < 0.9F);
        fbe_material_release(white);
    }
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(paint);
}

namespace {

/// How many pixels of two frames are apart by more than `within`.
int frames_apart(const fbe::golden::frame_t& one, const fbe::golden::frame_t& other, int within) {
    int apart = 0;
    for (std::size_t at = 0; at < one.rgba.size(); ++at) {
        apart += std::abs(one.rgba[at] - other.rgba[at]) > within ? 1 : 0;
    }
    return apart;
}

/// A material with all of PBR's on it: a color, how it's metal and rough, what it emits, and a
/// normal map leaning off its surface's tangent.
std::uint64_t all_of_pbr(std::uint64_t leaning) {
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_color(material, 0.8F, 0.5F, 0.3F, 1.0F));
    require(fbe_material_set_metal_rough(material, 0.3F, 0.4F));
    require(fbe_material_set_emissive(material, 0.05F, 0.0F, 0.1F));
    require(fbe_material_set_map(material, FBE_MAP_NORMAL, leaning, 0));
    return material;
}

/// A sun off to one side, so what a normal map leans to shows.
void off_to_a_side(std::uint64_t world) {
    const std::uint64_t sun = light(world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 3.0F);
    shine_from(sun, 0.6F, 0.3F, 0.7416198F);
}

} // namespace

// What came of instances meeting PBR: one of an Instances is what a mesh there is, by its own
// tangents.
TEST_CASE("an Instances' PBR is a mesh's, tangents and all", "[gpu][scene]") {
    // Each of these as a mesh's turn and scale, and as an instance's matrix.
    const float c = std::cos(0.6F);
    const float n = std::sin(0.6F);
    const float half = std::sin(0.3F);
    struct placed_t {
        const char* name;
        std::array<float, 4> turn;
        std::array<float, 3> scale;
        std::array<float, 16> matrix;
    };
    const float w = std::cos(0.3F);
    const std::array cases{
        placed_t{
            .name = "where it is",
            .turn = { 0.0F, 0.0F, 0.0F, 1.0F },
            .scale = { 1.0F, 1.0F, 1.0F },
            .matrix = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -3, 1 },
        },
        placed_t{
            .name = "turned",
            .turn = { 0.0F, 0.0F, half, w },
            .scale = { 1.0F, 1.0F, 1.0F },
            .matrix = { c, n, 0, 0, -n, c, 0, 0, 0, 0, 1, 0, 0, 0, -3, 1 },
        },
        placed_t{
            .name = "mirrored",
            .turn = { 0.0F, 0.0F, 0.0F, 1.0F },
            .scale = { -1.0F, 1.0F, 1.0F },
            .matrix = { -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -3, 1 },
        },
    };
    // Those of a w of 1 are what a pixel makes of its UVs too; only those of -1 show they're sent.
    for (const auto& placed : cases) {
        for (const float way : { 1.0F, -1.0F }) {
            CAPTURE(placed.name, way);
            const scene_t scene;
            require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
            off_to_a_side(scene.world);
            const std::uint64_t leaning = normal_map(0.6F, 0.2F);
            const std::uint64_t paint = all_of_pbr(leaning);
            require(fbe_material_set_two_sided(paint, true));
            const std::uint64_t mesh = wall(scene.world, -3.0F, 0.75F, paint);
            tangents_of(mesh, way);
            require(fbe_entity_set_rotation(mesh, placed.turn[0], placed.turn[1], placed.turn[2],
                                            placed.turn[3], FBE_SPACE_WORLD));
            require(fbe_entity_set_scale(mesh, placed.scale[0], placed.scale[1], placed.scale[2],
                                         FBE_SPACE_WORLD));
            const fbe::golden::frame_t as_a_mesh = scene.drawn();
            CHECK(middle(as_a_mesh)[0] > 20);
            fbe_entity_set_hidden(mesh, true);
            const std::uint64_t many = fbe_entity_create(scene.world, 0, FBE_ENTITY_INSTANCES);
            require(fbe_instances_set_geometry(many, fbe_mesh_geometry(mesh)));
            require(fbe_instances_set_material(many, paint));
            fbe_instance_t one{};
            std::ranges::copy(placed.matrix, std::begin(one.matrix.m));
            one.color = fbe_color_t{ .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F };
            require(fbe_instances_set(many, &one, 1));
            CHECK(frames_apart(as_a_mesh, scene.drawn(), 2) == 0);
            CHECK(fbe_gpu_validation_errors() == 0);
            fbe_material_release(paint);
            fbe_texture_release(leaning);
        }
    }
}

// And levels of detail: what a level draws is PBR by its own surface's tangents and materials.
TEST_CASE("a level of detail's PBR is its own surface's, tangents and all", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    off_to_a_side(scene.world);
    const std::uint64_t leaning = normal_map(0.6F, 0.2F);
    const std::uint64_t paint = all_of_pbr(leaning);
    const std::uint64_t mesh = wall(scene.world, -3.0F, 0.75F, paint);
    tangents_of(mesh, 1.0F);
    // The level's a wall of its own, whose tangents have its bitangent the other way.
    const std::uint64_t level = wall(scene.world, -3.0F, 0.75F, paint);
    tangents_of(level, -1.0F);
    fbe_entity_set_hidden(level, true);
    const fbe::golden::frame_t by_its_own = scene.drawn();
    CHECK(middle(by_its_own)[0] > 20);
    fbe_entity_set_hidden(level, false);
    fbe_entity_set_hidden(mesh, true);
    const fbe::golden::frame_t by_the_levels = scene.drawn();
    CHECK(frames_apart(by_its_own, by_the_levels, 2) > 0);
    // Small in the view, the mesh draws its level, as that wall alone draws.
    fbe_entity_set_hidden(level, true);
    fbe_entity_set_hidden(mesh, false);
    require(fbe_geometry_set_lod(fbe_mesh_geometry(mesh), 0, fbe_mesh_geometry(level), 100.0F));
    CHECK(frames_apart(by_the_levels, scene.drawn(), 2) == 0);
    fbe_material_release(paint);
    fbe_texture_release(leaning);
}

namespace {

/// An instance a quarter turned about Z, at a place: what moves along its own X goes up the world's
/// Y.
fbe_instance_t turned_instance_at(float x, float y, float z) {
    fbe_instance_t made = instance_at(x, y, z);
    made.matrix.m[0] = 0.0F;
    made.matrix.m[1] = 1.0F;
    made.matrix.m[4] = -1.0F;
    made.matrix.m[5] = 0.0F;
    return made;
}

/// The light an sRGB byte stores.
float light_of(std::uint8_t stored) {
    const float value = static_cast<float>(stored) / 255.0F;
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

/// Whether what's in the middle comes to `linear` drawn in linear light, `stored` in stored values,
/// and `linear` again after, as a camera can go either way between frames.
void check_both_ways(const scene_t& scene, std::array<std::uint8_t, 3> linear,
                     std::array<std::uint8_t, 3> stored, int within = 1) {
    CHECK(near(middle(scene.drawn()), linear, within));
    require(fbe_camera_set_stored_values(scene.camera, true));
    CHECK(fbe_camera_stored_values(scene.camera));
    const rgba_t drawn = middle(scene.drawn());
    CAPTURE(int{ drawn[0] }, int{ drawn[1] }, int{ drawn[2] });
    CHECK(near(drawn, stored, within));
    require(fbe_camera_set_stored_values(scene.camera, false));
    CHECK_FALSE(fbe_camera_stored_values(scene.camera));
    CHECK(near(middle(scene.drawn()), linear, within));
    CHECK(fbe_gpu_validation_errors() == 0);
}

} // namespace

// What a shader bends a PBR material's normal to is what a normal map of that normal would give: as
// lit by the sun, toward it and away, and in what the shader's given as the material's color.
TEST_CASE("a shader of its own bends a normal as a normal map would", "[gpu][scene]") {
    const scene_t scene;
    sun_from_right(scene.world);
    const std::uint64_t material = by_game_shader(5.0F);
    require(fbe_material_set_metal_rough(material, 0.0F, 0.4F));
    const std::uint64_t mesh = wall(scene.world, -4.0F, 10.0F, material);
    const auto bent_to = [&](float x, float y, float z) {
        require(fbe_material_set_parameter(material, 5, x, y, z, 1.0F));
        return middle(scene.drawn());
    };
    const rgba_t straight = middle(scene.drawn());
    CHECK(near(bent_to(0.0F, 0.0F, 1.0F), { straight[0], straight[1], straight[2] }, 1));
    const rgba_t toward = bent_to(0.6F, 0.0F, 0.8F);
    const rgba_t away = bent_to(-0.6F, 0.0F, 0.8F);
    INFO(int{ straight[0] } << " " << int{ toward[0] } << " " << int{ away[0] });
    CHECK(toward[0] > straight[0] + 10);
    CHECK(away[0] + 10 < straight[0]);

    // The same normal from a map, with no shader of its own.
    const std::uint64_t mapped = fbe_material_create();
    require(fbe_material_set_metal_rough(mapped, 0.0F, 0.4F));
    const std::uint64_t normals = fbe_texture_create(1, 1, FBE_TEXTURE_RGBA8_LINEAR, false);
    REQUIRE(normals != 0);
    const std::array<std::uint8_t, 4> texel{ 204, 128, 230, 255 };
    require(fbe_texture_write(normals, 0, 0, 1, 1, texel.data(), 4));
    require(fbe_material_set_map(mapped, FBE_MAP_NORMAL, normals, 0));
    require(fbe_mesh_set_material(mesh, mapped));
    const rgba_t by_map = middle(scene.drawn());
    CHECK(near(by_map, { toward[0], toward[1], toward[2] }, 3));
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_texture_release(normals);
    fbe_material_release(mapped);
    fbe_material_release(material);
}

// Through a surface's own tangents, as a map goes, where it has them: here up the wall, not along
// its UVs, so a frame from its UVs would bend it the wrong way. Every pixel, edges and all.
TEST_CASE("a shader of its own bends through a surface's own tangents, as a map does",
          "[gpu][scene]") {
    const scene_t scene;
    sun_from_right(scene.world);
    const std::uint64_t material = by_game_shader(5.0F);
    require(fbe_material_set_metal_rough(material, 0.0F, 0.4F));
    const std::uint64_t mesh = wall(scene.world, -4.0F, 1.5F, material);
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    REQUIRE(geometry != 0);
    const std::uint64_t mapped = fbe_material_create();
    require(fbe_material_set_metal_rough(mapped, 0.0F, 0.4F));
    const std::uint64_t normals = fbe_texture_create(1, 1, FBE_TEXTURE_RGBA8_LINEAR, false);
    REQUIRE(normals != 0);
    require(fbe_material_set_map(mapped, FBE_MAP_NORMAL, normals, 0));
    // Its bitangent either way round, as a mirrored mesh's is.
    for (const float handed : { 1.0F, -1.0F }) {
        const fbe_vertex_tangent_t tangent{ .x = 0.0F, .y = 1.0F, .z = 0.0F, .w = handed };
        const std::array<fbe_vertex_tangent_t, 4> up{ tangent, tangent, tangent, tangent };
        require(fbe_surface_set_tangents(geometry, 0, up.data(), 4));
        for (const auto& [x, y, red, green] :
             { std::tuple{ 0.6F, 0.0F, 204, 128 }, std::tuple{ 0.0F, 0.6F, 128, 204 } }) {
            INFO(handed << ": " << x << ", " << y);
            require(fbe_mesh_set_material(mesh, material));
            require(fbe_material_set_parameter(material, 5, x, y, 0.8F, 1.0F));
            const fbe::golden::frame_t bent = scene.drawn();
            const std::array<std::uint8_t, 4> texel{
                static_cast<std::uint8_t>(red),
                static_cast<std::uint8_t>(green),
                230,
                255,
            };
            require(fbe_texture_write(normals, 0, 0, 1, 1, texel.data(), 4));
            require(fbe_mesh_set_material(mesh, mapped));
            const fbe::golden::frame_t by_map = scene.drawn();
            CHECK(fbe::golden::compare(by_map, bent, 3).pixels == 0);
        }
    }
    const fbe_vertex_tangent_t upward{ .x = 0.0F, .y = 1.0F, .z = 0.0F, .w = 1.0F };
    const std::array<fbe_vertex_tangent_t, 4> up{ upward, upward, upward, upward };
    require(fbe_surface_set_tangents(geometry, 0, up.data(), 4));
    // And they're turned from the frame its UVs make, or this says nothing.
    require(fbe_mesh_set_material(mesh, material));
    require(fbe_material_set_parameter(material, 5, 0.6F, 0.0F, 0.8F, 1.0F));
    const fbe::golden::frame_t through_tangents = scene.drawn();
    require(fbe_surface_set_tangents(geometry, 0, nullptr, 0));
    CHECK(fbe::golden::compare(through_tangents, scene.drawn(), 3).pixels > 100);
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_geometry_release(geometry);
    fbe_texture_release(normals);
    fbe_material_release(mapped);
    fbe_material_release(material);
}

// On a two-sided material's back, what bend's given faces the camera, as shade's does.
TEST_CASE("a shader of its own bends a two-sided back as its front", "[gpu][scene]") {
    const scene_t scene;
    sun_from_right(scene.world);
    const std::uint64_t material = by_game_shader(5.0F);
    require(fbe_material_set_metal_rough(material, 0.0F, 0.4F));
    require(fbe_material_set_two_sided(material, true));
    // Turned round, so its back's toward the camera, and the sun.
    const std::uint64_t mesh = wall(scene.world, -4.0F, 10.0F, material);
    require(fbe_entity_set_rotation(mesh, 0.0F, 1.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    require(fbe_material_set_parameter(material, 5, 0.6F, 0.0F, 0.8F, 1.0F));
    const rgba_t toward = middle(scene.drawn());
    require(fbe_material_set_parameter(material, 5, -0.6F, 0.0F, 0.8F, 1.0F));
    const rgba_t away = middle(scene.drawn());
    require(fbe_material_set_parameter(material, 5, 0.6F, 0.0F, 0.8F, 2.0F));
    const rgba_t by_facing = middle(scene.drawn());
    INFO(int{ toward[0] } << " " << int{ away[0] } << " " << int{ by_facing[0] });
    CHECK(!near(toward, { away[0], away[1], away[2] }, 6));
    CHECK(near(by_facing, { toward[0], toward[1], toward[2] }, 1));
    fbe_material_release(material);
}

// Each instance's place reaches its shader's move, so two of one geometry move apart by where they
// are: here up by how far across each is, so the right one's up and the left one's down.
TEST_CASE("a shader of its own moves each instance by where it is", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t material = by_game_shader();
    require(fbe_material_set_shading(material, FBE_SHADING_UNLIT));
    cubes_at(scene.world, material,
             { instance_at(-3.0F, 0.0F, -10.0F), instance_at(3.0F, 0.0F, -10.0F) });
    const auto red_at = [&](const fbe::golden::frame_t& frame, float x, float y) {
        return pixel_of(frame, pixel_at(scene.camera, x, y, -9.0F))[0] > 200;
    };
    const fbe::golden::frame_t level = scene.drawn();
    CHECK(red_at(level, -3.0F, 0.0F));
    CHECK(red_at(level, 3.0F, 0.0F));
    require(fbe_material_set_parameter(material, 4, 0.5F, 0.0F, 0.0F, 0.0F));
    const fbe::golden::frame_t moved = scene.drawn();
    CHECK(red_at(moved, 3.0F, 1.5F));
    CHECK(!red_at(moved, 3.0F, -1.2F));
    CHECK(red_at(moved, -3.0F, -1.5F));
    CHECK(!red_at(moved, -3.0F, 1.2F));
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(material);
}

TEST_CASE("a shader of its own moves each instance by where it is in its Instances, not the world",
          "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t material = by_game_shader();
    require(fbe_material_set_shading(material, FBE_SHADING_UNLIT));
    require(fbe_material_set_parameter(material, 4, 0.5F, 0.0F, 0.0F, 0.0F));
    const std::uint64_t made =
        cubes_at(scene.world, material,
                 { instance_at(-3.0F, 0.0F, -10.0F), instance_at(3.0F, 0.0F, -10.0F) });
    const fbe::golden::frame_t where = scene.drawn();
    // Its Instances 10 along, and the camera with them: each instance where it is in them is as
    // it was, so nothing it draws changes.
    require(fbe_entity_set_position(made, 10.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    require(fbe_entity_set_position(scene.camera, 10.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    CHECK(fbe::golden::compare(where, scene.drawn(), 1).pixels == 0);
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(material);
}

TEST_CASE("an Instances is culled by its geometry's cull box, as what its shader moves leaves "
          "its bounds",
          "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
    const std::uint64_t material = by_game_shader();
    require(fbe_material_set_shading(material, FBE_SHADING_UNLIT));
    // Past the view's left edge, until its shader moves it 30 to the middle.
    require(fbe_material_set_parameter(material, 1, 30.0F, 0.0F, 0.0F, 0.0F));
    const std::uint64_t made =
        cubes_at(scene.world, material, { instance_at(-30.0F, 0.0F, -10.0F) });
    const std::uint64_t geometry = fbe_instances_geometry(made);
    const fbe_aabb_t moved{
        .min = { .x = -1.0F, .y = -1.0F, .z = -1.0F },
        .max = { .x = 31.0F, .y = 1.0F, .z = 1.0F },
    };
    SECTION("by its bounds, it's culled, until a box is set after it's drawn") {
        CHECK(near(middle(scene.drawn()), { 0, 0, 255 }));
        // Its clusters, worked out as it was drawn, are worked out again by the box.
        require(fbe_geometry_set_cull_box(geometry, &moved));
        CHECK(near(middle(scene.drawn()), { 255, 0, 0 }));
    }
    SECTION("its Bounds and InView are by its geometry's bounds, not the box") {
        room_to_move(geometry);
        CHECK(near(middle(scene.drawn()), { 255, 0, 0 }));
        fbe_aabb_t own{};
        require(fbe_geometry_bounds(geometry, &own));
        fbe_aabb_t bounds{};
        require(fbe_entity_bounds(made, &bounds));
        CHECK(bounds.min.x == own.min.x - 30.0F);
        CHECK(bounds.max.x == own.max.x - 30.0F);
        CHECK(bounds.min.y == own.min.y);
        CHECK(bounds.max.z == own.max.z - 10.0F);
        const auto size = static_cast<float>(frame_size);
        CHECK(fbe_camera_entity_in_view(scene.camera, size, size, made) == 0);
    }
    SECTION("by a box that takes its move, it's drawn") {
        require(fbe_geometry_set_cull_box(geometry, &moved));
        CHECK(near(middle(scene.drawn()), { 255, 0, 0 }));
        // And by its bounds again once it's cleared.
        require(fbe_geometry_clear_cull_box(geometry));
        CHECK(near(middle(scene.drawn()), { 0, 0, 255 }));
    }
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(material);
}

TEST_CASE("a shader of its own draws an Instances, each moved in its own space", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = by_game_shader();
    SECTION("drawn") {
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 1.0F, 1.0F));
        // Below the middle, until its shader moves it along what's up for it.
        cubes_at(scene.world, material, { turned_instance_at(0.0F, -3.0F, -10.0F) });
        CHECK(near(middle(scene.drawn()), { 0, 0, 255 }));
        require(fbe_material_set_parameter(material, 1, 3.0F, 0.0F, 0.0F, 0.0F));
        CHECK(near(middle(scene.drawn()), { 255, 0, 0 }));
    }
    SECTION("each instance's color reaches it, and each draws by its material") {
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
        require(fbe_material_set_shading(material, FBE_SHADING_UNLIT));
        require(fbe_material_set_parameter(material, 0, 5.0F, 0.0F, 0.0F, 0.0F));
        std::vector<fbe_instance_t> all{
            instance_at(-3.0F, 0.0F, -10.0F),
            instance_at(3.0F, 0.0F, -10.0F),
        };
        all[1].color = { .r = 1.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F };
        cubes_at(scene.world, material, all);
        const std::uint64_t greens = by_game_shader();
        require(fbe_material_set_parameter(greens, 2, 0.0F, 1.0F, 0.0F, 1.0F));
        cubes_at(scene.world, greens, { instance_at(0.0F, 3.0F, -10.0F) });
        const fbe::golden::frame_t frame = scene.drawn();
        CHECK(near(pixel_of(frame, pixel_at(scene.camera, -3.0F, 0.0F, -9.0F)), { 255, 255, 255 }));
        CHECK(near(pixel_of(frame, pixel_at(scene.camera, 3.0F, 0.0F, -9.0F)), { 255, 0, 0 }));
        CHECK(near(pixel_of(frame, pixel_at(scene.camera, 0.0F, 3.0F, -9.0F)), { 0, 255, 0 }));
        fbe_material_release(greens);
    }
    SECTION("casting") {
        const std::uint64_t paint = fbe_material_create();
        wall(scene.world, -10.0F, 30.0F, paint);
        sun_from_right(scene.world);
        // Off the sun's way to the wall's middle, until its shader moves it along what's up for it.
        const std::uint64_t made =
            cubes_at(scene.world, material, { turned_instance_at(5.0F, -3.0F, -5.0F) });
        CHECK(middle(scene.drawn())[0] != 0);
        require(fbe_material_set_parameter(material, 1, 3.0F, 0.0F, 0.0F, 0.0F));
        CHECK(middle(scene.drawn())[0] == 0);
        require(fbe_entity_set_casts_shadows(made, false));
        CHECK(middle(scene.drawn())[0] != 0);
        fbe_material_release(paint);
    }
    SECTION("casting, moved by where it is in its Instances") {
        const std::uint64_t paint = fbe_material_create();
        wall(scene.world, -10.0F, 30.0F, paint);
        sun_from_right(scene.world);
        // 10 along its Instances, which are 5 back, it's off the sun's way to the wall's middle,
        // until its shader moves it up by 0.3 of that.
        const std::uint64_t made =
            cubes_at(scene.world, material, { instance_at(10.0F, -3.0F, -5.0F) });
        require(fbe_entity_set_position(made, -5.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
        CHECK(middle(scene.drawn())[0] != 0);
        require(fbe_material_set_parameter(material, 4, 0.3F, 0.0F, 0.0F, 0.0F));
        CHECK(middle(scene.drawn())[0] == 0);
        fbe_material_release(paint);
    }
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(material);
}

namespace {

/// A scene for what a shader of its own's prepass has: a wall behind, lit by a quarter of white and
/// occluded within 3, and two materials of the shader, one to move what it draws and one to draw
/// what's placed where that's moved to.
struct prepass_scene_t : scene_t {
    std::uint64_t paint = fbe_material_create();
    std::uint64_t back = wall(world, -10.0F, 30.0F, paint);
    std::uint64_t moved = by_game_shader();
    std::uint64_t placed = by_game_shader();

    explicit prepass_scene_t(bool stops) : scene_t(stops) {
        require(fbe_world_set_ambient(world, 0.25F, 0.25F, 0.25F));
        require(fbe_camera_set_ambient_occlusion_radius(camera, 3.0F));
        require(fbe_camera_set_ambient_occlusion(camera, 1.0F));
    }
    prepass_scene_t(const prepass_scene_t&) = delete;
    prepass_scene_t& operator=(const prepass_scene_t&) = delete;
    prepass_scene_t(prepass_scene_t&&) = delete;
    prepass_scene_t& operator=(prepass_scene_t&&) = delete;

    ~prepass_scene_t() {
        CHECK(fbe_gpu_validation_errors() == 0);
        fbe_material_release(placed);
        fbe_material_release(moved);
    }
};

} // namespace

TEST_CASE("a scene that keeps the GPU started leaves the next the pipelines it made",
          "[gpu][scene]") {
    std::int32_t made = 0;
    {
        const scene_t first(false);
        const std::uint64_t material = by_game_shader();
        cube(first.world, 0.0F, 0.0F, -9.0F, material);
        static_cast<void>(first.drawn());
        made = fbe_gpu_pipelines();
        CHECK(made > 0);
        fbe_material_release(material);
    }
    CHECK(fbe_gpu_started());
    {
        const scene_t second;
        const std::uint64_t material = by_game_shader();
        cube(second.world, 0.0F, 0.0F, -9.0F, material);
        static_cast<void>(second.drawn());
        CHECK(fbe_gpu_pipelines() == made);
        fbe_material_release(material);
    }
    CHECK_FALSE(fbe_gpu_started());
    // One that fails stops it, kept or not.
    try {
        const scene_t kept(false);
        throw std::runtime_error("a part failed");
    } catch (const std::runtime_error&) {
        CHECK_FALSE(fbe_gpu_started());
    }
}

TEST_CASE("a shader of its own reaches the prepass: what it moves, and what it cuts",
          "[gpu][scene]") {
    // Beside what's in front of the wall, as its occlusion darkens it.
    const auto darkened = [](const fbe::golden::frame_t& frame) {
        return pixel(frame, 64 + 16, 64)[0] < srgb(0.25F) - 8;
    };
    const auto same = [](const fbe::golden::frame_t& one, const fbe::golden::frame_t& other) {
        return fbe::golden::compare(one, other, 1).pixels == 0;
    };
    std::vector<part_t<prepass_scene_t>> parts;
    parts.emplace_back("a mesh it moves is where it's moved to", [&](const prepass_scene_t& scene) {
        const std::uint64_t by_place = cube(scene.world, 0.0F, 0.0F, -9.0F, scene.placed);
        const fbe::golden::frame_t reference = scene.drawn();
        REQUIRE(darkened(reference));
        require(fbe_entity_free(by_place));
        require(fbe_material_set_parameter(scene.moved, 1, 0.0F, 5.0F, 0.0F, 0.0F));
        cube(scene.world, 0.0F, -5.0F, -9.0F, scene.moved);
        CHECK(same(reference, scene.drawn()));
    });
    parts.emplace_back(
        "an instance it moves, in its own space, is too", [&](const prepass_scene_t& scene) {
            const std::uint64_t by_place = cube(scene.world, 0.0F, 0.0F, -9.0F, scene.placed);
            const fbe::golden::frame_t reference = scene.drawn();
            REQUIRE(darkened(reference));
            require(fbe_entity_free(by_place));
            require(fbe_material_set_parameter(scene.moved, 1, 3.0F, 0.0F, 0.0F, 0.0F));
            cubes_at(scene.world, scene.moved, { turned_instance_at(0.0F, -3.0F, -9.0F) });
            CHECK(same(reference, scene.drawn()));
        });
    parts.emplace_back(
        "an instance it moves by where it is in its Instances is too",
        [&](const prepass_scene_t& scene) {
            const std::uint64_t by_place = cube(scene.world, 0.0F, 0.0F, -9.0F, scene.placed);
            const fbe::golden::frame_t reference = scene.drawn();
            REQUIRE(darkened(reference));
            require(fbe_entity_free(by_place));
            // 6 along its Instances, which are 6 back, so 3 below; moved up by half of 6.
            require(fbe_material_set_parameter(scene.moved, 4, 0.5F, 0.0F, 0.0F, 0.0F));
            const std::uint64_t made =
                cubes_at(scene.world, scene.moved, { instance_at(6.0F, -3.0F, -9.0F) });
            require(fbe_entity_set_position(made, -6.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
            CHECK(same(reference, scene.drawn()));
        });
    parts.emplace_back(
        "it's given a morph's normal, as when it's drawn", [&](const prepass_scene_t& scene) {
            const std::uint64_t by_place = wall(scene.world, -9.0F, 1.0F, scene.placed);
            require(fbe_entity_set_position(by_place, 2.0F, 0.0F, -9.0F, FBE_SPACE_WORLD));
            const fbe::golden::frame_t reference = scene.drawn();
            require(fbe_entity_free(by_place));
            require(fbe_material_set_parameter(scene.moved, 3, 2.0F, 0.0F, 0.0F, 0.0F));
            const std::uint64_t quad = wall(scene.world, -9.0F, 1.0F, scene.moved);
            // No move of its own, and its normals +X, from +Z.
            const std::array<float, 12> still{};
            const std::array<float, 12> to_x{ 1, 0, -1, 1, 0, -1, 1, 0, -1, 1, 0, -1 };
            require(fbe_morph_add_target(fbe_mesh_geometry(quad), 0, still.data(), to_x.data(),
                                         4) == 0);
            const std::array<float, 1> one{ 1.0F };
            require(fbe_morph_set_weights(quad, one.data(), 1));
            CHECK(same(reference, scene.drawn()));
        });
    parts.emplace_back(
        "it's given a skin's normal, as when it's drawn", [&](const prepass_scene_t& scene) {
            // A joint turned an eighth around up, which has its normal at 45 degrees from +Z to +X.
            const float half_turn = std::numbers::pi_v<float> / 8.0F;
            const auto turned = [&](std::uint64_t material, float along) {
                const skinned_wall_t made = skinned_wall(scene.world, material, -9.0F, 1.0F);
                all_on(made.geometry, 1, 1.0F);
                require(fbe_entity_set_position(made.moving, along, 0.0F, -9.0F + along,
                                                FBE_SPACE_WORLD));
                require(fbe_entity_set_rotation(made.moving, 0.0F, std::sin(half_turn), 0.0F,
                                                std::cos(half_turn), FBE_SPACE_WORLD));
                return made;
            };
            const float along = 2.0F * std::numbers::sqrt2_v<float> / 2.0F;
            const skinned_wall_t by_place = turned(scene.placed, along);
            const fbe::golden::frame_t reference = scene.drawn();
            require(fbe_entity_free(by_place.mesh));
            fbe_skin_release(by_place.skin);
            require(fbe_material_set_parameter(scene.moved, 3, 2.0F, 0.0F, 0.0F, 0.0F));
            const skinned_wall_t made = turned(scene.moved, 0.0F);
            CHECK(same(reference, scene.drawn()));
            fbe_skin_release(made.skin);
        });
    parts.emplace_back(
        "what it cuts by what it returns isn't there", [&](const prepass_scene_t& scene) {
            const fbe::golden::frame_t nothing = scene.drawn();
            require(fbe_material_set_alpha_cutoff(scene.moved, 0.5F));
            require(fbe_material_set_parameter(scene.moved, 2, 1.0F, 0.0F, 0.0F, 0.0F));
            const std::uint64_t by_shader = cube(scene.world, 0.0F, 0.0F, -9.0F, scene.moved);
            CHECK(same(nothing, scene.drawn()));
            // An instance's the same.
            require(fbe_entity_free(by_shader));
            cubes_at(scene.world, scene.moved, { instance_at(0.0F, 0.0F, -9.0F) });
            CHECK(same(nothing, scene.drawn()));
        });
    parts.emplace_back("what its material's alpha would cut and it keeps is there",
                       [&](const prepass_scene_t& scene) {
                           const std::uint64_t by_place =
                               cube(scene.world, 0.0F, 0.0F, -9.0F, scene.placed);
                           const fbe::golden::frame_t reference = scene.drawn();
                           REQUIRE(darkened(reference));
                           require(fbe_entity_free(by_place));
                           require(fbe_material_set_alpha_cutoff(scene.moved, 0.5F));
                           require(fbe_material_set_color(scene.moved, 1.0F, 1.0F, 1.0F, 0.0F));
                           cube(scene.world, 0.0F, 0.0F, -9.0F, scene.moved);
                           CHECK(same(reference, scene.drawn()));
                       });
    parts.emplace_back(
        "it cuts by what it's drawn with: where, facing what, of what vertex, lit how",
        [&](const prepass_scene_t& scene) {
            // What's left of a quad cut at x 0.
            const std::uint64_t half = wall(scene.world, -9.0F, 1.0F, scene.placed);
            require(fbe_entity_set_position(half, -0.5F, 0.0F, -9.0F, FBE_SPACE_WORLD));
            require(fbe_entity_set_scale(half, 0.5F, 1.0F, 1.0F, FBE_SPACE_WORLD));
            const fbe::golden::frame_t reference = scene.drawn();
            require(fbe_entity_free(half));
            require(fbe_material_set_alpha_cutoff(scene.moved, 0.5F));
            require(fbe_material_set_parameter(scene.moved, 0, 6.0F, 0.0F, 0.0F, 0.0F));
            const std::uint64_t whole_quad = wall(scene.world, -9.0F, 1.0F, scene.moved);
            CHECK(same(reference, scene.drawn()));
            require(fbe_entity_free(whole_quad));
            // Lit by the ambient light alone, 0.25 of white, over its 0.2; without any, it's cut.
            const std::uint64_t by_place = wall(scene.world, -9.0F, 1.0F, scene.placed);
            const fbe::golden::frame_t whole_reference = scene.drawn();
            require(fbe_entity_free(by_place));
            require(fbe_material_set_shading(scene.moved, FBE_SHADING_VERTEX));
            require(fbe_material_set_parameter(scene.moved, 0, 7.0F, 0.0F, 0.0F, 0.0F));
            const std::uint64_t lit_quad = wall(scene.world, -9.0F, 1.0F, scene.moved);
            CHECK(same(whole_reference, scene.drawn()));
            require(fbe_world_set_ambient(scene.world, 0.0F, 0.0F, 0.0F));
            const fbe::golden::frame_t unlit_frame = scene.drawn();
            require(fbe_entity_free(lit_quad));
            CHECK(same(unlit_frame, scene.drawn()));
        });
    parts.emplace_back(
        "what it lights, cut in its prepass, reads no occlusion that isn't there yet",
        [&](const prepass_scene_t& scene) {
            require(fbe_material_set_alpha_cutoff(scene.moved, 0.5F));
            require(fbe_material_set_parameter(scene.moved, 0, 4.0F, 0.0F, 0.0F, 0.0F));
            cube(scene.world, 0.0F, 0.0F, -9.0F, scene.moved);
            CHECK(darkened(scene.drawn()));
        });
    parts.emplace_back(
        "what it cuts by its color, in its prepass, isn't by the occlusion of the frame before",
        [&](const prepass_scene_t& scene) {
            // The wall, 0.25 by its ambient light, cut where its occlusion takes it under 0.2,
            // which is only known after its prepass: what that's cut by is its color with none, the
            // same each frame, so its occlusion is too.
            require(fbe_material_set_alpha_cutoff(scene.moved, 0.5F));
            require(fbe_material_set_parameter(scene.moved, 0, 7.0F, 0.0F, 0.0F, 0.0F));
            require(fbe_mesh_set_material(scene.back, scene.moved));
            cube(scene.world, 0.0F, 0.0F, -9.0F, scene.placed);
            const fbe::golden::frame_t first = scene.drawn();
            const fbe::golden::frame_t second = scene.drawn();
            const fbe::golden::frame_t third = scene.drawn();
            CHECK(pixel(third, 64 + 16, 64) != pixel(third, 4, 4));
            CHECK(same(first, second));
            CHECK(same(second, third));
        });
    parts.emplace_back(
        "its motion's from where it's moved to, then and now", [&](const prepass_scene_t& scene) {
            require(fbe_entity_set_position(scene.camera, 0.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
            require(fbe_camera_set_ambient_occlusion(scene.camera, 0.0F));
            require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
            require(fbe_camera_set_hdr(scene.camera, true));
            require(fbe_camera_set_temporal_aa(scene.camera, true));
            const std::uint64_t white = by_game_shader(5.0F);
            require(fbe_material_set_shading(white, FBE_SHADING_UNLIT));
            // Moved 3 across, by its shader, in its space, scaled by 3; two whole pixels a frame.
            require(fbe_material_set_parameter(white, 1, 1.0F, 0.0F, 0.0F, 0.0F));
            const std::uint64_t made =
                cubes_at(scene.world, white, { instance_at(0.0F, 0.0F, 0.0F, 3.0F) });
            const float across = std::tan(std::numbers::pi_v<float> / 6.0F) * 7.0F / 64.0F;
            fbe::golden::frame_t last;
            for (int frame = 0; frame <= 24; ++frame) {
                const float pixels = static_cast<float>(frame - 24) * 2.0F;
                require(fbe_entity_set_position(made, (0.5F + pixels) * across, 0.0F, -10.0F,
                                                FBE_SPACE_WORLD));
                last = scene.drawn();
            }
            const float value = static_cast<float>(pixel(last, 64, 64)[0]) / 255.0F;
            CAPTURE(value);
            CHECK(value > 0.4F);
            CHECK(value < 0.9F);
            fbe_material_release(white);
        });
    // Each above the middle by what's placed there, below it by what its shader moves, going across
    // together two pixels a frame, so a row of each ends the same only if what the frame before had
    // of each is from the same place.
    const float tangent = std::tan(std::numbers::pi_v<float> / 6.0F);
    const auto alike = [&](const scene_t& scene, float depth, auto&& move_to) {
        require(fbe_entity_set_position(scene.camera, 0.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
        require(fbe_camera_set_ambient_occlusion(scene.camera, 0.0F));
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
        require(fbe_camera_set_hdr(scene.camera, true));
        require(fbe_camera_set_temporal_aa(scene.camera, true));
        const float across = tangent * depth / 64.0F;
        fbe::golden::frame_t last;
        for (int frame = 0; frame <= 24; ++frame) {
            move_to((0.5F + (static_cast<float>(frame - 24) * 2.0F)) * across);
            last = scene.drawn();
        }
        // Each one's middle row, a mirror of the other about the view's middle.
        const auto row = static_cast<std::uint32_t>(64.0F - (2.0F / (depth * tangent) * 64.0F));
        int differing = 0;
        for (std::uint32_t x = 0; x < frame_size; ++x) {
            for (std::size_t k = 0; k < 3; ++k) {
                if (std::abs(pixel(last, x, row)[k] - pixel(last, x, frame_size - 1 - row)[k]) >
                    2) {
                    ++differing;
                }
            }
        }
        return differing;
    };
    parts.emplace_back(
        "a mesh's motion is that of what's placed where it's moved to",
        [&](const prepass_scene_t& scene) {
            require(fbe_material_set_parameter(scene.moved, 1, 3.0F, 0.0F, 0.0F, 0.0F));
            const std::uint64_t there = cube(scene.world, 0.0F, 2.0F, -10.0F, scene.placed);
            const std::uint64_t from = cube(scene.world, 0.0F, -2.0F, -10.0F, scene.moved);
            CHECK(alike(scene, 10.0F, [&](float x) {
                      require(
                          fbe_entity_set_position(there, x + 3.0F, 2.0F, -10.0F, FBE_SPACE_WORLD));
                      require(fbe_entity_set_position(from, x, -2.0F, -10.0F, FBE_SPACE_WORLD));
                  }) == 0);
        });
    parts.emplace_back(
        "a skinned mesh's, along a normal its joint turns, is too",
        [&](const prepass_scene_t& scene) {
            // Turned 30 degrees about up, so 3 along its normal is 1.5 across and 2.6 nearer.
            const float half_turn = std::numbers::pi_v<float> / 12.0F;
            const float away = 3.0F * std::sin(2.0F * half_turn);
            const float toward = 3.0F * std::cos(2.0F * half_turn);
            require(fbe_material_set_parameter(scene.moved, 3, 3.0F, 0.0F, 0.0F, 0.0F));
            const skinned_wall_t skinned = skinned_wall(scene.world, scene.moved, -10.0F, 1.0F);
            all_on(skinned.geometry, 0, 1.0F);
            require(fbe_entity_set_rotation(skinned.still, 0.0F, std::sin(half_turn), 0.0F,
                                            std::cos(half_turn), FBE_SPACE_WORLD));
            const std::uint64_t there = wall(scene.world, -10.0F, 1.0F, scene.placed);
            require(fbe_entity_set_rotation(there, 0.0F, std::sin(half_turn), 0.0F,
                                            std::cos(half_turn), FBE_SPACE_WORLD));
            CHECK(alike(scene, 10.0F - toward, [&](float x) {
                      require(fbe_entity_set_position(there, x + away, 2.0F, -10.0F + toward,
                                                      FBE_SPACE_WORLD));
                      require(fbe_entity_set_position(skinned.still, x, -2.0F, -10.0F,
                                                      FBE_SPACE_WORLD));
                  }) == 0);
            fbe_skin_release(skinned.skin);
        });
    on_one_gpu(parts);
}

namespace {

/// What the GPU holds that a frame's making and freeing could leave: VMA's allocations, the
/// pipelines, and the set's free slots. Not fbe_gpu_memory_usage, which is the driver's, and
/// lavapipe's is the machine's.
struct held_t {
    fbe::gpu::allocations_t allocations;
    std::int32_t pipelines = 0;
    std::uint32_t free_samples = 0;
    bool operator==(const held_t&) const = default;
};

held_t held() {
    return {
        .allocations = fbe::gpu::allocations(),
        .pipelines = fbe_gpu_pipelines(),
        .free_samples = fbe::gpu::free_samples_slot_count(),
    };
}

} // namespace

// Each frame makes and frees a texture with mipmaps, a material of it and one of a game's shader,
// a mesh of each, and a camera drawing them into a target of its own, of 1 or 4 samples, in HDR or
// not, besides the scene's: once the first frames have made the pipelines and pools there are,
// what the GPU holds is the same, frame after frame, and nothing's left once it stops. CI runs 600
// frames, and FBE_SOAK_FRAMES asks for more.
namespace {

/// The game's shader but for the generator's word of its header, which SPIR-V's own tools say and
/// nothing reads, so it's new code for each `generation`.
std::vector<std::uint8_t> generation_of(std::uint32_t generation) {
    const std::span<const std::uint32_t> seed = fbe::test_shaders::game_shader.code;
    std::vector<std::uint32_t> words(seed.begin(), seed.end());
    words[2] = generation;
    return cooked_shader(words);
}

std::uint64_t shader_of_generation(std::uint32_t generation) {
    const std::vector<std::uint8_t> bytes = generation_of(generation);
    return fbe_shader_create(bytes.data(), static_cast<std::int64_t>(bytes.size()), "reloaded");
}

} // namespace

// A game's shader cooked again, as a game's hot reload does, each time of new code, drawn with,
// then freed or replaced: once nothing draws with an old one, its pipelines go, and its code with
// them, so there's no more of either after many than after one.
TEST_CASE("a game's shaders made again and freed leave no pipelines of them",
          "[gpu][scene][stability]") {
    const scene_t scene;
    const auto drawn = [&](std::uint64_t material) {
        const std::uint64_t mesh = cube(scene.world, 0.0F, 0.0F, -6.0F, material);
        static_cast<void>(scene.drawn());
        require(fbe_entity_free(mesh));
        // Frames on, as what's freed waits for those in flight.
        for (int frame = 0; frame < 4; ++frame) {
            static_cast<void>(scene.drawn());
        }
    };
    SECTION("made again") {
        const auto drawn_with = [&](std::uint32_t generation) {
            const std::uint64_t shader = shader_of_generation(generation);
            require(shader != 0);
            const std::uint64_t material = fbe_material_create();
            require(fbe_material_set_shader(material, shader));
            require(fbe_shader_release(shader));
            drawn(material);
            fbe_material_release(material);
        };
        drawn_with(1);
        drawn_with(1);
        const std::int32_t one = fbe_gpu_pipelines();
        CHECK(one > 0);
        for (std::uint32_t generation = 2; generation < 40; ++generation) {
            drawn_with(generation);
        }
        CHECK(fbe_gpu_pipelines() == one);
    }
    SECTION("replaced, as a reload replaces it") {
        const std::uint64_t shader = shader_of_generation(1);
        require(shader != 0);
        const std::uint64_t material = fbe_material_create();
        require(fbe_material_set_shader(material, shader));
        drawn(material);
        const std::int32_t one = fbe_gpu_pipelines();
        for (std::uint32_t generation = 2; generation < 40; ++generation) {
            const std::vector<std::uint8_t> bytes = generation_of(generation);
            require(fbe_shader_replace(shader, bytes.data(),
                                       static_cast<std::int64_t>(bytes.size()), "reloaded"));
            drawn(material);
        }
        CHECK(fbe_gpu_pipelines() == one);
        // Replaced by itself, it's kept.
        const std::vector<std::uint8_t> same = generation_of(39);
        require(fbe_shader_replace(shader, same.data(), static_cast<std::int64_t>(same.size()),
                                   "reloaded"));
        drawn(material);
        CHECK(fbe_gpu_pipelines() == one);
        fbe_material_release(material);
        require(fbe_shader_release(shader));
    }
    CHECK(fbe_gpu_validation_errors() == 0);
}

// 2f8a4c0's review: a shader made, drawn, and freed, of the same code, frame after frame with no
// readback to wait, and replaced by another and back: a free from an earlier release mustn't free
// pipelines a later frame still draws with, which validation says, nor make them again each frame.
TEST_CASE("a game's shader freed and had again, frame after frame, keeps its pipelines",
          "[gpu][scene][stability]") {
    const scene_t scene;
    const std::uint64_t mesh = cube(scene.world, 0.0F, 0.0F, -6.0F);
    const auto flip = [&scene] {
        require(fbe_render_world(scene.world, 1.0F));
        fbe_frame_flip(false);
    };
    SECTION("made and freed") {
        std::int32_t first = 0;
        for (int frame = 0; frame < 8; ++frame) {
            const std::uint64_t shader = shader_of_generation(1);
            require(shader != 0);
            const std::uint64_t material = fbe_material_create();
            require(fbe_material_set_shader(material, shader));
            require(fbe_shader_release(shader));
            require(fbe_mesh_set_material(mesh, material));
            flip();
            // Let go of by everything as soon as it's drawn.
            require(fbe_mesh_set_material(mesh, 0));
            fbe_material_release(material);
            if (frame == 0) {
                first = fbe_gpu_pipelines();
            }
            CHECK(fbe_gpu_pipelines() == first);
        }
    }
    SECTION("replaced, A to B to A") {
        const std::uint64_t shader = shader_of_generation(1);
        require(shader != 0);
        const std::uint64_t material = fbe_material_create();
        require(fbe_material_set_shader(material, shader));
        require(fbe_mesh_set_material(mesh, material));
        const std::vector<std::uint8_t> a = generation_of(1);
        const std::vector<std::uint8_t> b = generation_of(2);
        for (int frame = 0; frame < 8; ++frame) {
            const std::vector<std::uint8_t>& next = frame % 2 == 0 ? b : a;
            require(fbe_shader_replace(shader, next.data(), static_cast<std::int64_t>(next.size()),
                                       "reloaded"));
            flip();
        }
        fbe_material_release(material);
        require(fbe_shader_release(shader));
    }
    CHECK(fbe_gpu_validation_errors() == 0);
}

// As many different shaders at once as a pipeline's key numbers, 4095: one more is refused, and
// once one's freed, and its frames done, there's room for another.
TEST_CASE("as many shaders as a pipeline's key numbers are kept at once",
          "[gpu][scene][stability]") {
    // Validated, 4095 would take a Debug build a quarter of a second each.
    const unvalidated_t unvalidated;
    const scene_t scene;
    std::vector<std::uint64_t> made;
    for (std::uint32_t generation = 1; generation <= 4095; ++generation) {
        const std::uint64_t shader = shader_of_generation(generation);
        if (shader == 0) {
            break;
        }
        made.push_back(shader);
    }
    // Some may be the test program's own, made already.
    CHECK(made.size() > 4000);
    CHECK(shader_of_generation(5000) == 0);
    CHECK(std::string(fbe_error()) ==
          "there are more different shaders than a pipeline's key can number");
    require(fbe_shader_release(made.back()));
    made.pop_back();
    for (int frame = 0; frame < 4; ++frame) {
        static_cast<void>(scene.drawn());
    }
    const std::uint64_t another = shader_of_generation(5000);
    CHECK(another != 0);
    made.push_back(another);
    for (const std::uint64_t shader : made) {
        require(fbe_shader_release(shader));
    }
}

TEST_CASE("frames of making and freeing leave the GPU holding what it did",
          "[gpu][scene][stability]") {
    const int frames = asked_for("FBE_SOAK_FRAMES", 600);
    REQUIRE(frames >= 100);
    const scene_t scene;
    const std::vector<std::uint8_t> pixels(std::size_t{ 16 } * 16 * 4, 200);
    const auto frame = [&](int at) {
        const std::uint64_t texture = fbe_texture_create(16, 16, FBE_TEXTURE_RGBA8, true);
        require(texture != 0);
        require(fbe_texture_write(texture, 0, 0, 16, 16, pixels.data(), 16 * 4));
        const std::uint64_t painted = fbe_material_create();
        require(fbe_material_set_texture(painted, 0, texture));
        const std::uint64_t shaded = by_game_shader();
        const std::uint64_t first = wall(scene.world, -5.0F, 1.0F, painted);
        const std::uint64_t second = cube(scene.world, 1.0F, 0.0F, -6.0F, shaded);
        const std::uint64_t target =
            fbe_texture_create_target(32, 32, FBE_TEXTURE_RGBA8, at % 2 == 0 ? 1 : 4, true);
        require(target != 0);
        const std::uint64_t camera = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
        require(fbe_camera_set_target(camera, target));
        require(fbe_camera_set_hdr(camera, at % 3 == 0));
        static_cast<void>(scene.drawn());
        require(fbe_entity_free(camera));
        require(fbe_entity_free(first));
        require(fbe_entity_free(second));
        fbe_material_release(painted);
        fbe_material_release(shaded);
        fbe_texture_release(texture);
        fbe_texture_release(target);
    };
    // What the first frames make, of each kind, and what's kept while they're in flight.
    for (int at = 0; at < 12; ++at) {
        frame(at);
    }
    const held_t settled = held();
    CAPTURE(settled.allocations.count, settled.allocations.bytes, settled.pipelines);
    CHECK(settled.allocations.count > 0);
    int differed = 0;
    for (int at = 12; at < frames; ++at) {
        frame(at);
        // At the same place in the cycle of samples and HDR.
        if (at % 6 == 5 && held() != settled) {
            ++differed;
            const held_t now = held();
            CAPTURE(at, now.allocations.count, now.allocations.bytes, now.pipelines,
                    now.free_samples);
            CHECK(now == settled);
        }
    }
    CHECK(differed == 0);
    CHECK(fbe_gpu_validation_errors() == 0);
}

TEST_CASE("a camera in stored values blends with what's stored, as DirectX 7 did", "[gpu][scene]") {
    const scene_t scene;
    CHECK_FALSE(fbe_camera_stored_values(scene.camera));
    const float behind = light_of(100);
    SECTION("half white over black is 128, and 188 in light") {
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
        const std::uint64_t pane = unlit(1.0F, 1.0F, 1.0F, 0.5F);
        require(fbe_material_set_blend(pane, FBE_BLEND_ALPHA));
        wall(scene.world, -3.0F, 10.0F, pane);
        check_both_ways(scene, { 188, 188, 188 }, { 128, 128, 128 });
        fbe_material_release(pane);
    }
    SECTION("half green over half red over black is 64, 128, 0, and 137, 188, 0 in light") {
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
        const std::uint64_t red = unlit(1.0F, 0.0F, 0.0F, 0.5F);
        const std::uint64_t green = unlit(0.0F, 1.0F, 0.0F, 0.5F);
        require(fbe_material_set_blend(red, FBE_BLEND_ALPHA));
        require(fbe_material_set_blend(green, FBE_BLEND_ALPHA));
        wall(scene.world, -3.0F, 10.0F, green);
        wall(scene.world, -6.0F, 20.0F, red);
        check_both_ways(scene, { 137, 188, 0 }, { 64, 128, 0 });
        fbe_material_release(red);
        fbe_material_release(green);
    }
    SECTION("100 with half of itself added is 150, and 121 in light") {
        require(fbe_camera_set_clear_color(scene.camera, behind, behind, behind, 1.0F));
        const std::uint64_t pane = unlit(behind, behind, behind, 128.0F / 255.0F);
        require(fbe_material_set_blend(pane, FBE_BLEND_ADD));
        wall(scene.world, -3.0F, 10.0F, pane);
        check_both_ways(scene, { 121, 121, 121 }, { 150, 150, 150 });
        fbe_material_release(pane);
    }
    SECTION("100 multiplied by 128 is 50, and 46 in light") {
        require(fbe_camera_set_clear_color(scene.camera, behind, behind, behind, 1.0F));
        const float by = light_of(128);
        const std::uint64_t pane = unlit(by, by, by);
        require(fbe_material_set_blend(pane, FBE_BLEND_MULTIPLY));
        wall(scene.world, -3.0F, 10.0F, pane);
        check_both_ways(scene, { 46, 46, 46 }, { 50, 50, 50 });
        fbe_material_release(pane);
    }
}

TEST_CASE("what's opaque comes to the same bytes in stored values, the sky and clear too",
          "[gpu][scene]") {
    const scene_t scene;
    SECTION("its clear color") {
        const float light = light_of(100);
        require(fbe_camera_set_clear_color(scene.camera, light, light / 2.0F, 0.0F, 1.0F));
        const std::uint8_t green = srgb(light / 2.0F);
        check_both_ways(scene, { 100, green, 0 }, { 100, green, 0 });
    }
    SECTION("a clear of part of its target, and nothing past it") {
        const float light = light_of(100);
        require(fbe_camera_set_clear_color(scene.camera, light, light, light, 1.0F));
        require(fbe_camera_set_viewport(scene.camera, 32.0F, 32.0F, 64.0F, 64.0F));
        check_both_ways(scene, { 100, 100, 100 }, { 100, 100, 100 });
        require(fbe_camera_set_stored_values(scene.camera, true));
        CHECK(near(pixel(scene.drawn(), 4, 4), { 0, 0, 0 }, 0));
    }
    SECTION("a clear color past white is white") {
        require(fbe_camera_set_clear_color(scene.camera, 3.0F, 0.0F, -1.0F, 1.0F));
        check_both_ways(scene, { 255, 0, 0 }, { 255, 0, 0 });
    }
    SECTION("layers, what's doubled and added on what's stored") {
        const std::uint64_t material = unlit(1.0F, 1.0F, 1.0F);
        const std::uint64_t fifty = flat_texture(50, 50, 50, 255);
        const std::uint64_t added = flat_texture(60, 40, 30, 255);
        require(fbe_material_set_texture(material, 0, fifty));
        require(fbe_material_set_layer(material, 0, FBE_LAYER_MULTIPLY_2, 0));
        require(fbe_material_set_texture(material, 1, added));
        require(fbe_material_set_layer(material, 1, FBE_LAYER_ADD, 0));
        wall(scene.world, -3.0F, 10.0F, material);
        check_both_ways(scene, { 160, 140, 130 }, { 160, 140, 130 });
        fbe_material_release(material);
        fbe_texture_release(fifty);
        fbe_texture_release(added);
    }
    SECTION("VERTEX's ambient, half of what's stored") {
        const std::uint64_t material = fbe_material_create();
        require(fbe_material_set_shading(material, FBE_SHADING_VERTEX));
        require(fbe_world_set_ambient(scene.world, 0.214F, 0.214F, 0.214F));
        wall(scene.world, -3.0F, 10.0F, material);
        check_both_ways(scene, { 128, 128, 128 }, { 128, 128, 128 });
        fbe_material_release(material);
    }
    SECTION("fog half way, on what's stored") {
        const std::uint64_t material = unlit(1.0F, 1.0F, 1.0F);
        wall(scene.world, -5.0F, 30.0F, material);
        require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
        require(fbe_camera_set_fog_mode(scene.camera, FBE_FOG_LINEAR));
        require(fbe_camera_set_fog_color(scene.camera, 0.0F, 0.0F, 0.0F));
        require(fbe_camera_set_fog_range(scene.camera, 0.0F, 10.0F));
        check_both_ways(scene, { 128, 128, 128 }, { 128, 128, 128 });
        fbe_material_release(material);
    }
    SECTION("the sky, by its light as stored") {
        const std::uint64_t texture = flat_texture(100, 50, 200, 255);
        require(fbe_world_set_sky_texture(scene.world, texture, 1.0F));
        check_both_ways(scene, { 100, 50, 200 }, { 100, 50, 200 });
        fbe_texture_release(texture);
    }
    SECTION("the sky's light past white is white") {
        const std::uint64_t texture = flat_texture(255, 100, 0, 255);
        require(fbe_world_set_sky_texture(scene.world, texture, 2.0F));
        check_both_ways(scene, { 255, srgb(light_of(100) * 2.0F), 0 },
                        { 255, srgb(light_of(100) * 2.0F), 0 });
        fbe_texture_release(texture);
    }
    SECTION("PBR, shaded in light, and only what it ends with encoded") {
        const std::uint64_t material = fbe_material_create();
        require(fbe_material_set_shading(material, FBE_SHADING_PBR));
        require(fbe_material_set_color(material, 0.5F, 0.25F, 0.1F, 1.0F));
        wall(scene.world, -3.0F, 10.0F, material);
        light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
        const rgba_t lit = middle(scene.drawn());
        CHECK(lit[0] > 40);
        check_both_ways(scene, { lit[0], lit[1], lit[2] }, { lit[0], lit[1], lit[2] });
        fbe_material_release(material);
    }
    SECTION("PBR past white is white") {
        const std::uint64_t material = fbe_material_create();
        require(fbe_material_set_shading(material, FBE_SHADING_PBR));
        wall(scene.world, -3.0F, 10.0F, material);
        const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
        require(fbe_light_set_intensity(sun, 100.0F));
        check_both_ways(scene, { 255, 255, 255 }, { 255, 255, 255 });
        fbe_material_release(material);
    }
}

TEST_CASE("PBR that blends in stored values blends what it's encoded as", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t material = fbe_material_create();
    require(fbe_material_set_shading(material, FBE_SHADING_PBR));
    wall(scene.world, -3.0F, 10.0F, material);
    light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    const rgba_t whole = middle(scene.drawn());
    REQUIRE(whole[0] > 100);
    require(fbe_material_set_blend(material, FBE_BLEND_ALPHA));
    require(fbe_material_set_color(material, 1.0F, 1.0F, 1.0F, 0.5F));
    const auto half = static_cast<std::uint8_t>(std::lround(static_cast<float>(whole[0]) * 0.5F));
    const std::uint8_t in_light = srgb(light_of(whole[0]) * 0.5F);
    check_both_ways(scene, { in_light, in_light, in_light }, { half, half, half });
    fbe_material_release(material);
}

TEST_CASE("an HDR camera refuses stored values, and stored values HDR", "[gpu][scene]") {
    const scene_t scene;
    SECTION("an HDR camera stays one") {
        require(fbe_camera_set_hdr(scene.camera, true));
        CHECK_FALSE(fbe_camera_set_stored_values(scene.camera, true));
        CHECK(std::string_view(fbe_error()).contains("HDR"));
        CHECK_FALSE(fbe_camera_stored_values(scene.camera));
        CHECK(fbe_camera_hdr(scene.camera));
        // Saying it doesn't is no refusal.
        CHECK(fbe_camera_set_stored_values(scene.camera, false));
        require(fbe_camera_set_hdr(scene.camera, false));
        CHECK(fbe_camera_set_stored_values(scene.camera, true));
    }
    SECTION("stored values stay") {
        require(fbe_camera_set_stored_values(scene.camera, true));
        CHECK_FALSE(fbe_camera_set_hdr(scene.camera, true));
        CHECK(std::string_view(fbe_error()).contains("stored values"));
        CHECK_FALSE(fbe_camera_hdr(scene.camera));
        CHECK(fbe_camera_stored_values(scene.camera));
        CHECK(fbe_camera_set_hdr(scene.camera, false));
        require(fbe_camera_set_stored_values(scene.camera, false));
        CHECK(fbe_camera_set_hdr(scene.camera, true));
    }
    SECTION("what isn't a camera has none") {
        CHECK_FALSE(fbe_camera_set_stored_values(scene.world, true));
        CHECK_FALSE(fbe_camera_stored_values(scene.world));
    }
}

TEST_CASE("stored values have no passes of their own, and antialiasing over frames does nothing",
          "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t pane = unlit(1.0F, 1.0F, 1.0F, 0.5F);
    require(fbe_material_set_blend(pane, FBE_BLEND_ALPHA));
    wall(scene.world, -3.0F, 10.0F, pane);
    const std::vector<std::string_view> off = passes_of(scene);
    require(fbe_camera_set_stored_values(scene.camera, true));
    CHECK(passes_of(scene) == off);
    require(fbe_camera_set_temporal_aa(scene.camera, true));
    CHECK(passes_of(scene) == off);
    fbe_material_release(pane);
}

TEST_CASE("a target of linear bytes takes stored values too", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t target =
        fbe_texture_create_target(32, 32, FBE_TEXTURE_RGBA8_LINEAR, 1, true);
    REQUIRE(target != 0);
    require(fbe_camera_set_target(scene.camera, target));
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const float gray = light_of(100);
    const std::uint64_t material = unlit(gray, gray, gray);
    wall(scene.world, -3.0F, 10.0F, material);
    const auto drawn = [&] {
        require(fbe_render_world(scene.world, 1.0F));
        const fbe::golden::frame_t frame = scene_t::read(target);
        fbe_frame_flip(false);
        return middle(frame);
    };
    // Light as it is, in linear bytes, or what's stored.
    CHECK(near(drawn(), { 32, 32, 32 }, 1));
    require(fbe_camera_set_stored_values(scene.camera, true));
    CHECK(near(drawn(), { 100, 100, 100 }, 0));
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(material);
    require(fbe_camera_set_target(scene.camera, scene.target));
    fbe_texture_release(target);
}

TEST_CASE("a target of floats holds stored values, and nothing past white", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t target = fbe_texture_create_target(4, 4, FBE_TEXTURE_RGBA16F, 1, true);
    REQUIRE(target != 0);
    require(fbe_camera_set_target(scene.camera, target));
    require(fbe_camera_set_stored_values(scene.camera, true));
    const auto red_of_a_pixel = [&] {
        require(fbe_render_world(scene.world, 1.0F));
        std::array<std::uint8_t, 128> bytes{};
        require(fbe_texture_read(target, 0, bytes.data(), bytes.size()));
        fbe_frame_flip(false);
        // A half float's exponent and part: this one's positive, and not tiny.
        const std::uint32_t red = bytes[0] + (std::uint32_t{ bytes[1] } << 8U);
        return std::ldexp(1.0F + (static_cast<float>(red & 1023U) / 1024.0F),
                          static_cast<int>((red >> 10U) & 31U) - 15);
    };
    require(fbe_camera_set_clear_color(scene.camera, 3.0F, 0.0F, 0.0F, 1.0F));
    CHECK(red_of_a_pixel() == Catch::Approx(1.0F).margin(0.001F));
    require(fbe_camera_set_clear_color(scene.camera, light_of(100), 0.0F, 0.0F, 1.0F));
    CHECK(red_of_a_pixel() == Catch::Approx(100.0F / 255.0F).margin(0.001F));
    CHECK(fbe_gpu_validation_errors() == 0);
    require(fbe_camera_set_target(scene.camera, scene.target));
    fbe_texture_release(target);
}

TEST_CASE("multisampling in stored values averages what's stored", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t target = fbe_texture_create_target(32, 32, FBE_TEXTURE_RGBA8, 4, true);
    REQUIRE(target != 0);
    require(fbe_camera_set_target(scene.camera, target));
    require(fbe_camera_set_projection(scene.camera, FBE_PROJECTION_ORTHOGRAPHIC));
    require(fbe_camera_set_orthographic_height(scene.camera, 32.0F));
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t material = unlit(1.0F, 1.0F, 1.0F);
    // White from the middle of pixel 16 on, so two of its four samples.
    const std::uint64_t made = wall(scene.world, -3.0F, 100.0F, material);
    require(fbe_entity_set_position(made, 100.5F, 0.0F, -3.0F, FBE_SPACE_WORLD));
    const auto drawn = [&] {
        require(fbe_render_world(scene.world, 1.0F));
        const fbe::golden::frame_t frame = scene_t::read(target);
        fbe_frame_flip(false);
        return pixel(frame, 16, 16);
    };
    CHECK(near(drawn(), { 188, 188, 188 }, 1));
    require(fbe_camera_set_stored_values(scene.camera, true));
    CHECK(near(drawn(), { 128, 128, 128 }, 1));
    SECTION("and what blends, all four samples of it") {
        require(fbe_material_set_blend(material, FBE_BLEND_ALPHA));
        require(fbe_material_set_color(material, 1.0F, 1.0F, 1.0F, 0.5F));
        require(fbe_entity_set_position(made, 0.0F, 0.0F, -3.0F, FBE_SPACE_WORLD));
        CHECK(near(drawn(), { 128, 128, 128 }, 1));
    }
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(material);
    require(fbe_camera_set_target(scene.camera, scene.target));
    fbe_texture_release(target);
}

TEST_CASE("what's drawn in stored values is those bytes to whatever reads it", "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t pane = unlit(1.0F, 1.0F, 1.0F, 0.5F);
    require(fbe_material_set_blend(pane, FBE_BLEND_ALPHA));
    const std::uint64_t seen = wall(scene.world, -3.0F, 10.0F, pane);
    require(fbe_camera_set_stored_values(scene.camera, true));
    CHECK(near(middle(scene.drawn()), { 128, 128, 128 }, 1));
    // A camera in light, UNLIT, draws what its texture's bytes are, whatever drew them.
    const std::uint64_t shows = unlit(1.0F, 1.0F, 1.0F);
    require(fbe_material_set_texture(shows, 0, scene.target));
    const std::uint64_t other_target =
        fbe_texture_create_target(32, 32, FBE_TEXTURE_RGBA8, 1, true);
    REQUIRE(other_target != 0);
    const std::uint64_t other = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
    require(fbe_camera_set_target(other, other_target));
    require(fbe_camera_set_order(other, 1));
    require(fbe_entity_set_position(other, 100.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    const std::uint64_t shown = wall(scene.world, -3.0F, 10.0F, shows);
    require(fbe_entity_set_position(shown, 100.0F, 0.0F, -3.0F, FBE_SPACE_WORLD));
    require(fbe_render_world(scene.world, 1.0F));
    const fbe::golden::frame_t frame = scene_t::read(other_target);
    fbe_frame_flip(false);
    CHECK(near(middle(frame), { 128, 128, 128 }, 1));
    CHECK(fbe_gpu_validation_errors() == 0);
    (void)seen;
    fbe_material_release(pane);
    fbe_material_release(shows);
    fbe_texture_release(other_target);
}

TEST_CASE("between a texture's texels, one in sRGB filters by light, one linear by what's stored",
          "[gpu][scene]") {
    const scene_t scene;
    const std::int32_t format = GENERATE(FBE_TEXTURE_RGBA8, FBE_TEXTURE_RGBA8_LINEAR);
    const std::uint64_t texture = fbe_texture_create(2, 1, format, false);
    REQUIRE(texture != 0);
    const std::array<std::uint8_t, 8> pixels{ 0, 0, 0, 255, 255, 255, 255, 255 };
    require(fbe_texture_write(texture, 0, 0, 2, 1, pixels.data(), 8));
    require(fbe_texture_set_filter(texture, FBE_TEXTURE_LINEAR));
    require(fbe_texture_set_wrap(texture, FBE_TEXTURE_CLAMP, FBE_TEXTURE_CLAMP));
    const std::uint64_t material = unlit(1.0F, 1.0F, 1.0F);
    require(fbe_material_set_texture(material, 0, texture));
    wall(scene.world, -3.0F, 10.0F, material);
    require(fbe_camera_set_stored_values(scene.camera, true));
    const std::uint8_t between = format == FBE_TEXTURE_RGBA8 ? 188 : 128;
    CHECK(near(middle(scene.drawn()), { between, between, between }, 3));
    fbe_material_release(material);
    fbe_texture_release(texture);
}

TEST_CASE("ambient occlusion in stored values is what it is in light, and only on PBR",
          "[gpu][scene]") {
    const scene_t scene;
    const cornered_t cornered(scene);
    require(fbe_camera_set_ambient_occlusion(scene.camera, 1.0F));
    const fbe::golden::frame_t in_light = scene.drawn();
    require(fbe_camera_set_stored_values(scene.camera, true));
    const fbe::golden::frame_t stored = scene.drawn();
    const rgba_t open_one = pixel(stored, open_wall[0], open_wall[1]);
    const rgba_t beside = pixel(stored, beside_block[0], beside_block[1]);
    CAPTURE(int{ open_one[0] }, int{ beside[0] });
    CHECK(beside[0] + 8 < open_one[0]);
    CHECK(fbe::golden::compare(in_light, stored, 1).pixels == 0);
    SECTION("UNLIT has none") {
        const std::uint64_t flat = unlit(0.5F, 0.5F, 0.5F);
        require(fbe_mesh_set_material(cornered.back, flat));
        require(fbe_mesh_set_material(cornered.block, flat));
        const fbe::golden::frame_t drawn = scene.drawn();
        CHECK(near(pixel(drawn, beside_block[0], beside_block[1]), { 188, 188, 188 }, 1));
        fbe_material_release(flat);
    }
    CHECK(fbe_gpu_validation_errors() == 0);
}

TEST_CASE("fxaa in stored values softens by what's stored", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = unlit(1.0F, 1.0F, 1.0F);
    edge_wall(scene, paint);
    require(fbe_camera_set_stored_values(scene.camera, true));
    require(fbe_camera_set_fxaa(scene.camera, true));
    CHECK(std::ranges::count(passes_of(scene), std::string_view("fxaa")) == 1);
    const fbe::golden::frame_t soft = scene.drawn();
    const float part = fxaa_small_part();
    const auto dark = static_cast<std::uint8_t>(std::lround(part * 255.0F));
    const auto light = static_cast<std::uint8_t>(std::lround((1.0F - part) * 255.0F));
    CAPTURE(int{ pixel(soft, 63, 64)[0] }, int{ pixel(soft, 64, 64)[0] }, int{ dark });
    CHECK(near(pixel(soft, 63, 64), { dark, dark, dark }, 3));
    CHECK(near(pixel(soft, 64, 64), { light, light, light }, 3));
    CHECK(pixel(soft, 62, 64)[0] == 0);
    CHECK(pixel(soft, 65, 64)[0] == 255);
    SECTION("its least contrast is of what's stored") {
        // 200 by 255 is 0.216 apart, over its least, an eighth of the brightest; their square
        // roots, as light's looks are measured, would be 0.115.
        const float behind = light_of(200);
        require(fbe_camera_set_clear_color(scene.camera, behind, behind, behind, 1.0F));
        const fbe::golden::frame_t bright = scene.drawn();
        CAPTURE(int{ pixel(bright, 63, 64)[0] });
        CHECK(pixel(bright, 62, 64)[0] == 200);
        CHECK(pixel(bright, 63, 64)[0] >= 202);
    }
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_material_release(paint);
}

// What it returns is in light whichever way the camera draws, so half of 0.5 over black is 0.25 in
// light, stored 137, and half of 0.5's stored 188 in stored values, 94, as a material's own is.
TEST_CASE("a shader of its own blends with what's stored, in stored values", "[gpu][scene]") {
    const scene_t scene;
    const std::int32_t shading = GENERATE(FBE_SHADING_PBR, FBE_SHADING_UNLIT);
    CAPTURE(shading);
    require(fbe_camera_set_clear_color(scene.camera, 0.0F, 0.0F, 0.0F, 1.0F));
    const std::uint64_t material = by_game_shader();
    require(fbe_material_set_shading(material, shading));
    require(fbe_material_set_parameter(material, 2, 0.5F, 0.5F, 0.5F, 0.5F));
    require(fbe_material_set_blend(material, FBE_BLEND_ALPHA));
    wall(scene.world, -3.0F, 10.0F, material);
    check_both_ways(scene, { 137, 137, 137 }, { 94, 94, 94 });
    fbe_material_release(material);
}

// In stored values from its first frame, as what it falls back to goes the camera's way too.
TEST_CASE("a shader no pipeline can be made of draws as the scene's own, in stored values",
          "[gpu][scene]") {
    const scene_t scene;
    require(fbe_camera_set_stored_values(scene.camera, true));
    // Bytes of its own, so it's a code of its own, by its name.
    std::vector<std::uint32_t> words(fbe::test_shaders::game_shader.code.begin(),
                                     fbe::test_shaders::game_shader.code.end());
    // Its generator's word, which nothing reads.
    words[2] = 0xFB02;
    const std::vector<std::uint8_t> bytes = cooked_shader(words);
    const std::uint64_t shader = fbe_shader_create(
        bytes.data(), static_cast<std::int64_t>(bytes.size()), "res://refused_stored.slang");
    REQUIRE(shader != 0);
    fbe::gpu::refuse_pipelines_of("res://refused_stored.slang");
    const std::uint64_t material = unlit(0.5F, 0.5F, 0.5F);
    require(fbe_material_set_shader(material, shader));
    wall(scene.world, -3.0F, 1.0F, material);
    // Half light, encoded: 188, where linear would write 128 to these bytes.
    CHECK(near(middle(scene.drawn()), { 188, 188, 188 }));
    CHECK(fbe::gpu::pipelines_refused() == 1);
    fbe::gpu::refuse_pipelines_of("");
    CHECK(fbe_gpu_validation_errors() == 0);
    fbe_shader_release(shader);
    fbe_material_release(material);
}
