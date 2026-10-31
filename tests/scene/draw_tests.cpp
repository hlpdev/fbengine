// What a camera draws, extracted from the scene, and the projection it draws with, through the C
// API, with no GPU.

#include "fbengine.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

/// A world of the test's own, freed at its end.
struct test_world_t {
    std::uint64_t handle = fbe_world_create();

    test_world_t() = default;
    test_world_t(const test_world_t&) = delete;
    test_world_t& operator=(const test_world_t&) = delete;
    test_world_t(test_world_t&&) = delete;
    test_world_t& operator=(test_world_t&&) = delete;

    ~test_world_t() {
        fbe_world_free(handle);
        std::array<std::uint64_t, 16> some{};
        while (fbe_entity_freed(some.data(), some.size()) != 0) {}
    }
};

bool near(float a, float b) {
    return std::fabs(a - b) < 0.001F;
}

std::vector<fbe_draw_t> draws(std::uint64_t camera, float tween = 1.0F) {
    const std::int32_t count = fbe_render_draws(camera, 1600.0F, 900.0F, tween, nullptr, 0);
    std::vector<fbe_draw_t> found(static_cast<std::size_t>(std::max(count, 0)));
    fbe_render_draws(camera, 1600.0F, 900.0F, tween, found.data(), count);
    return found;
}

/// A cube mesh at a place, in a material, or the default with 0.
std::uint64_t cube(std::uint64_t world, float x, float y, float z, std::uint64_t material = 0) {
    const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    const std::uint64_t geometry = fbe_geometry_cube();
    fbe_mesh_set_geometry(mesh, geometry);
    fbe_geometry_release(geometry);
    fbe_mesh_set_material(mesh, material);
    fbe_entity_set_position(mesh, x, y, z, FBE_SPACE_WORLD);
    return mesh;
}

} // namespace

TEST_CASE("a camera draws what it sees, opaque nearest first, then blended furthest first",
          "[render]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t glass = fbe_material_create();
    fbe_material_set_blend(glass, FBE_BLEND_ALPHA);
    const std::uint64_t far = cube(world.handle, 0.0F, 0.0F, -20.0F);
    const std::uint64_t close = cube(world.handle, 0.0F, 0.0F, -5.0F);
    const std::uint64_t pane_far = cube(world.handle, 2.0F, 0.0F, -15.0F, glass);
    const std::uint64_t pane_close = cube(world.handle, -2.0F, 0.0F, -8.0F, glass);
    // Behind the camera, hidden, in another World, faded out, or alpha 0: not drawn.
    cube(world.handle, 0.0F, 0.0F, 10.0F);
    const std::uint64_t hidden = cube(world.handle, 0.0F, 1.0F, -6.0F);
    fbe_entity_set_hidden(hidden, true);
    const test_world_t other;
    cube(other.handle, 0.0F, 0.0F, -5.0F);
    const std::uint64_t faded = cube(world.handle, 0.0F, -1.0F, -30.0F);
    fbe_entity_set_auto_fade(faded, 5.0F, 10.0F);
    const std::uint64_t clear = cube(world.handle, 1.0F, 1.0F, -7.0F);
    fbe_mesh_set_tint(clear, 1.0F, 1.0F, 1.0F, 0.0F);
    const std::vector<fbe_draw_t> found = draws(camera);
    REQUIRE(found.size() == 4);
    CHECK(found[0].entity == close);
    CHECK(found[1].entity == far);
    CHECK(found[2].entity == pane_far);
    CHECK(found[3].entity == pane_close);
    CHECK(found[0].kind == FBE_ENTITY_MESH);
    CHECK(found[0].surface == 0);
    CHECK(found[0].blend == FBE_BLEND_OPAQUE);
    CHECK(found[2].blend == FBE_BLEND_ALPHA);
    CHECK(found[2].material == glass);
    CHECK(near(found[0].depth, 5.0F));
    CHECK(near(found[0].matrix[14], -5.0F));
    // A material's order draws it before or after the rest, whatever its depth.
    const std::uint64_t sky = fbe_material_create();
    fbe_material_set_order(sky, -1);
    fbe_mesh_set_material(far, sky);
    CHECK(draws(camera)[0].entity == far);
    // A hidden camera draws nothing; a freed one, or a tween past 1, is refused.
    fbe_entity_set_hidden(camera, true);
    CHECK(draws(camera).empty());
    fbe_entity_set_hidden(camera, false);
    CHECK(fbe_render_draws(camera, 1600.0F, 900.0F, 1.5F, nullptr, 0) == -1);
    CHECK(fbe_render_draws(close, 1600.0F, 900.0F, 1.0F, nullptr, 0) == -1);
    fbe_material_release(glass);
    fbe_material_release(sky);
}

TEST_CASE("a camera's draws fade, tween, and take sprites and planes", "[render]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t prop = cube(world.handle, 0.0F, 0.0F, -15.0F);
    fbe_entity_set_auto_fade(prop, 10.0F, 20.0F);
    std::vector<fbe_draw_t> found = draws(camera);
    REQUIRE(found.size() == 1);
    // Half faded, it blends.
    CHECK(near(found[0].color[3], 0.5F));
    CHECK(found[0].blend == FBE_BLEND_ALPHA);
    // Captured, then moved: halfway, it's drawn halfway.
    fbe_entity_set_auto_fade(prop, 0.0F, 0.0F);
    fbe_world_capture(world.handle);
    fbe_entity_set_position(prop, 4.0F, 0.0F, -15.0F, FBE_SPACE_WORLD);
    found = draws(camera, 0.5F);
    REQUIRE(found.size() == 1);
    CHECK(near(found[0].matrix[12], 2.0F));
    // A sprite, as the camera sees it, and a ground plane, always.
    const std::uint64_t sprite = fbe_entity_create(world.handle, 0, FBE_ENTITY_SPRITE);
    fbe_entity_set_position(sprite, 0.0F, 0.0F, -3.0F, FBE_SPACE_WORLD);
    const std::uint64_t ground = fbe_entity_create(world.handle, 0, FBE_ENTITY_PLANE);
    fbe_entity_set_position(ground, 0.0F, -2.0F, 0.0F, FBE_SPACE_WORLD);
    found = draws(camera);
    REQUIRE(found.size() == 3);
    bool saw_sprite = false;
    bool saw_plane = false;
    for (const fbe_draw_t& draw : found) {
        saw_sprite = saw_sprite || (draw.entity == sprite && draw.kind == FBE_ENTITY_SPRITE &&
                                    draw.surface == -1 && near(draw.matrix[14], -3.0F));
        saw_plane = saw_plane || (draw.entity == ground && draw.kind == FBE_ENTITY_PLANE);
    }
    CHECK(saw_sprite);
    CHECK(saw_plane);
}

TEST_CASE("the projection a camera draws with is Vulkan's, depth reversed, and agrees with Project",
          "[render]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    fbe_camera_set_range(camera, 0.5F, 100.0F);
    fbe_entity_set_position(camera, 1.0F, 2.0F, 3.0F, FBE_SPACE_WORLD);
    fbe_entity_set_rotation(camera, 0.1F, 0.3F, 0.0F, 0.94868F, FBE_SPACE_WORLD);
    for (const std::int32_t projection :
         { FBE_PROJECTION_PERSPECTIVE, FBE_PROJECTION_ORTHOGRAPHIC }) {
        fbe_camera_set_projection(camera, projection);
        fbe_mat4_t p{};
        fbe_mat4_t v{};
        REQUIRE(fbe_camera_draw_projection(camera, 1600.0F, 900.0F, &p));
        REQUIRE(fbe_camera_view_matrix(camera, &v));
        const auto clip = [&](float x, float y, float z) {
            std::array<float, 4> seen{};
            std::array<float, 4> out{};
            const std::array<float, 4> in{ x, y, z, 1.0F };
            for (std::size_t row = 0; row < 4; ++row) {
                for (std::size_t k = 0; k < 4; ++k) {
                    seen[row] += v.m[(k * 4) + row] * in[k];
                }
            }
            for (std::size_t row = 0; row < 4; ++row) {
                for (std::size_t k = 0; k < 4; ++k) {
                    out[row] += p.m[(k * 4) + row] * seen[k];
                }
            }
            return out;
        };
        // Points in front of the camera land where Project puts them, y down, depth reversed.
        for (const std::array<float, 3> point : {
                 std::array{ 2.0F, 2.5F, -5.0F },
                 std::array{ -1.0F, 1.0F, -20.0F },
                 std::array{ 4.0F, 3.0F, -40.0F },
             }) {
            fbe_vec3_t projected{};
            REQUIRE(fbe_camera_project(camera, 1600.0F, 900.0F, point[0], point[1], point[2],
                                       &projected) == 1);
            const std::array<float, 4> c = clip(point[0], point[1], point[2]);
            const float x = ((c[0] / c[3]) + 1.0F) * 0.5F * 1600.0F;
            const float y = ((c[1] / c[3]) + 1.0F) * 0.5F * 900.0F;
            const float depth = c[2] / c[3];
            CHECK(std::fabs(x - projected.x) < 0.05F);
            CHECK(std::fabs(y - projected.y) < 0.05F);
            CHECK(depth > 0.0F);
            CHECK(depth < 1.0F);
        }
    }
    // Depth is 1 at the near plane, 0 at the far one: straight ahead of an unturned camera.
    fbe_entity_set_rotation(camera, 0.0F, 0.0F, 0.0F, 1.0F, FBE_SPACE_WORLD);
    fbe_entity_set_position(camera, 0.0F, 0.0F, 0.0F, FBE_SPACE_WORLD);
    fbe_camera_set_projection(camera, FBE_PROJECTION_PERSPECTIVE);
    fbe_mat4_t p{};
    REQUIRE(fbe_camera_draw_projection(camera, 1600.0F, 900.0F, &p));
    const auto depth_at = [&](float distance) {
        const float z = -distance;
        return ((p.m[10] * z) + p.m[14]) / ((p.m[11] * z) + p.m[15]);
    };
    CHECK(near(depth_at(0.5F), 1.0F));
    CHECK(near(depth_at(100.0F), 0.0F));
    CHECK(depth_at(1.0F) > depth_at(2.0F));
    // Up in the world is up the screen: a point above the middle has a negative clip y.
    CHECK(p.m[5] < 0.0F);
}
