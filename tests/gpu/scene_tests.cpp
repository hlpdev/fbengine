// Drawing the scene on the GPU, through the C API: materials, their layers and shading models,
// lights, fog, order, cameras into textures, sprites, planes, terrains, and mirrors, each checked
// by the pixels its formula says it draws, and a golden image of each.

#include "engine_shaders.hpp"
#include "fbengine.h"
#include "golden/golden.hpp"
#include "gpu_testing.hpp"
#include "render/render.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <gpu/graph.hpp>
#include <limits>
#include <numbers>
#include <span>
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

    scene_t() {
        require(target != 0);
        require(fbe_camera_set_target(camera, target));
        // Nothing but what a test lights.
        require(fbe_world_set_ambient(world, 0.0F, 0.0F, 0.0F));
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
        stop_gpu();
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

TEST_CASE("a layer's UVs turn and scale as Blitz's did", "[gpu][scene]") {
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

// Its culling is the scene's as it is now, so that follows an edit at once; what's drawn waits.
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
    CHECK(halves[((std::size_t{ 4 } * 8) + 4) * 4] == 0x4400);
    CHECK(halves[(((std::size_t{ 4 } * 8) + 4) * 4) + 1] == 0x3800);
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
TEST_CASE("an HDR camera's picture is what one without it draws", "[gpu][scene]") {
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
            require(fbe_camera_set_hdr(scene.camera, hdr));
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
            require(fbe_camera_set_hdr(scene.camera, hdr));
            require(fbe_camera_set_viewport(scene.camera, 0.0F, 0.0F, 100.0F, 100.0F));
            const std::uint64_t over = fbe_entity_create(scene.world, 0, FBE_ENTITY_CAMERA);
            require(fbe_camera_set_target(over, scene.target));
            require(fbe_camera_set_order(over, 1));
            require(fbe_camera_set_hdr(over, hdr));
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
            require(fbe_camera_set_hdr(scene.camera, hdr));
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
            require(fbe_camera_set_hdr(over, hdr));
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
            require(fbe_camera_set_hdr(scene.camera, hdr));
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
            require(fbe_camera_set_hdr(monitor, hdr));
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
            require(fbe_camera_set_hdr(scene.camera, hdr));
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
