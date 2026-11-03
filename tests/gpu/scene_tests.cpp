// Drawing the scene on the GPU, through the C API: materials, their layers and shading models,
// lights, fog, order, cameras into textures, sprites, planes, terrains, and mirrors, each checked
// by the pixels its formula says it draws, and a golden image of each.

#include "fbengine.h"
#include "golden/golden.hpp"
#include "gpu_testing.hpp"
#include "render/render.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
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

TEST_CASE("more shadows than the atlas holds lose the last, with no error", "[gpu][scene]") {
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
