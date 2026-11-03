// Drawing the scene on the GPU, through the C API: materials, their layers and shading models,
// lights, fog, order, cameras into textures, sprites, planes, terrains, and mirrors, each checked
// by the pixels its formula says it draws, and a golden image of each.

#include "fbengine.h"
#include "golden/golden.hpp"
#include "gpu/vulkan.hpp"
#include "gpu_testing.hpp"
#include "render/drawing.hpp"
#include "render/render.hpp"
#include "texture/texture.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
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

/// How tight the scene shader's highlight is.
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
    const float expected = 2.0F * falloff(2.0F, 8.0F);
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

TEST_CASE("a highlight's where a surface turns its light to the eye, as its shininess says",
          "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t material = fbe_material_create();
    wall(scene.world, -3.0F, 10.0F, material);
    // A sun from the eye's side, whose reflection comes back to the eye at the wall's middle, past
    // white with a highlight, and falls away from it at the wall's edge of the view.
    const std::uint64_t sun = light(scene.world, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    require(fbe_light_set_intensity(sun, 0.25F));
    const float plain = 0.25F;
    CHECK(near(middle(scene.drawn()), { srgb(plain), srgb(plain), srgb(plain) }));
    require(fbe_material_set_shininess(material, 1.0F));
    const fbe::golden::frame_t shiny = scene.drawn();
    const float highlight = 0.25F + 0.25F;
    CHECK(near(middle(shiny), { srgb(highlight), srgb(highlight), srgb(highlight) }));
    // At the view's edge, the way halfway to the eye is 15 degrees off the normal, and 100 powers
    // of that cosine are about 0.03 of it.
    const float off = 0.25F * std::pow(std::cos(0.2618F), highlight_power);
    const std::uint8_t edge = srgb(plain + off);
    CHECK(near(pixel(shiny, 1, frame_size / 2), { edge, edge, edge }, 3));
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
    require(fbe_light_set_intensity(sun, 0.5F));
    CHECK(near(middle(scene.drawn()), { srgb(0.5F), srgb(0.5F), srgb(0.5F) }));
    // From behind the wall's front: nothing.
    require(fbe_entity_set_rotation(sun, 0.0F, 1.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    CHECK(near(middle(scene.drawn()), { 0, 0, 0 }));
    fbe_entity_free(sun);
    // A spot straight at the middle from 1 away: whole inside its inner cone, out past its outer.
    const std::uint64_t spot = light(scene.world, FBE_LIGHT_SPOT, 0.0F, 0.0F, -2.0F, 100.0F);
    require(fbe_light_set_cone(spot, 0.1F, 0.2F));
    const fbe::golden::frame_t frame = scene.drawn();
    const std::uint8_t whole = srgb(falloff(1.0F, 100.0F));
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
    CHECK(near(middle(scene.drawn()), { srgb(facing), srgb(facing), srgb(facing) }));
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

constexpr float sun_facing = 0.70710678F;

} // namespace

TEST_CASE("a directional light's shadow falls where it should, and nowhere else", "[gpu][scene]") {
    const scene_t scene;
    const std::uint64_t paint = fbe_material_create();
    wall(scene.world, -10.0F, 30.0F, paint);
    const std::uint64_t sun = sun_from_right(scene.world);
    // Out of the camera's sight, off to the right, on the sun's way to the wall's middle.
    cube(scene.world, 5.0F, 0.0F, -5.0F, paint);
    const std::uint8_t lit = srgb(sun_facing);
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
        const float facing = std::cos((std::numbers::pi_v<float> / 4.0F) - turn);
        CAPTURE(turn);
        CHECK(near(middle(frame), { srgb(facing), srgb(facing), srgb(facing) }));
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
    const std::uint8_t lit = srgb(sun_facing);
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
    require(fbe_light_set_intensity(spot, 30.0F));
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
    const std::uint8_t lit = srgb(sun_facing);
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
    const std::uint8_t lit = srgb(sun_facing);
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
    const std::uint64_t geometry = fbe_geometry_decal(scene.world, &matrix, 0.01F);
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
            count += static_cast<int>(pixel(frame, x, y)[0]) > above ? 1 : 0;
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
                         std::uint64_t material) {
    const std::uint64_t geometry = fbe_geometry_create();
    const std::int32_t surface = fbe_geometry_add_surface(geometry);
    const auto corner = [&](float across, float up) {
        return fbe_vertex_t{
            .position = { .x = across * half, .y = up * half, .z = 0.0F },
            .normal = { .x = 0.0F, .y = 0.0F, .z = 1.0F },
            .uv0 = { .x = 0.0F, .y = 0.0F },
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
        return middle(scene.drawn())[0] < srgb(sun_facing) / 2;
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
        CHECK(
            near(middle(scene.drawn()), { srgb(sun_facing), srgb(sun_facing), srgb(sun_facing) }));
        require(fbe_light_set_shadows(second, false));
        CHECK(near(middle(scene.drawn()),
                   { srgb(sun_facing * 2.0F), srgb(sun_facing * 2.0F), srgb(sun_facing * 2.0F) }));
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
        CHECK(reds_above(frame, srgb(sun_facing) - 5) == frame_size * frame_size);
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
        CHECK((head_on[1] == 0) == drawn);
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
    const int expected = srgb(std::sin(radians));
    const fbe::golden::frame_t frame = scene.drawn();
    int wrong = 0;
    for (std::uint32_t y = frame.height / 2; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            wrong += std::abs(pixel(frame, x, y)[0] - expected) >= 3 ? 1 : 0;
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
    CHECK(near(middle(far_off), { srgb(sun_facing), srgb(sun_facing), srgb(sun_facing) }));
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
        const int lit = srgb(std::cos(0.35F));
        int wrong = 0;
        for (std::uint32_t y = 0; y < frame.height; ++y) {
            for (std::uint32_t x = 0; x < frame.width; ++x) {
                wrong += std::abs(pixel(frame, x, y)[0] - lit) > 3 ? 1 : 0;
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
