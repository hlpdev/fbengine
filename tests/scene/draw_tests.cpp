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
    // By geometry before depth: drawn of one, they go nearest first.
    fbe_mesh_set_geometry(far, fbe_mesh_geometry(close));
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

TEST_CASE("a geometry's cull box culls its meshes instead of its bounds", "[render]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t mesh = cube(world.handle, 0.0F, 0.0F, -5.0F);
    // Its vertices moved far aside, as a skinned mesh's bones can take them.
    const std::uint64_t geometry = fbe_mesh_geometry(mesh);
    fbe_mat4_t aside{};
    aside.m[0] = 1.0F;
    aside.m[5] = 1.0F;
    aside.m[10] = 1.0F;
    aside.m[15] = 1.0F;
    aside.m[12] = 100.0F;
    fbe_geometry_transform(geometry, &aside);
    CHECK(draws(camera).empty());
    const fbe_aabb_t box{
        .min = { .x = -1.0F, .y = -1.0F, .z = -1.0F },
        .max = { .x = 1.0F, .y = 1.0F, .z = 1.0F },
    };
    REQUIRE(fbe_geometry_set_cull_box(geometry, &box));
    CHECK(draws(camera).size() == 1);
    fbe_aabb_t found{};
    REQUIRE(fbe_geometry_cull_box(geometry, &found));
    CHECK(found.max.y == 1.0F);
    // A clone keeps it; cleared, the bounds cull again; a backward box is refused.
    const std::uint64_t clone = fbe_geometry_clone(geometry);
    CHECK(fbe_geometry_cull_box(clone, &found));
    fbe_geometry_release(clone);
    REQUIRE(fbe_geometry_clear_cull_box(geometry));
    CHECK_FALSE(fbe_geometry_cull_box(geometry, &found));
    CHECK(draws(camera).empty());
    const fbe_aabb_t backward{
        .min = { .x = 1.0F, .y = 0.0F, .z = 0.0F },
        .max = { .x = 0.0F, .y = 1.0F, .z = 1.0F },
    };
    CHECK_FALSE(fbe_geometry_set_cull_box(geometry, &backward));
}

TEST_CASE("a camera draws a terrain its grid shows it, and a plane unless it's under it",
          "[render]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    fbe_camera_set_range(camera, 0.1F, 100.0F);
    // A grid 10 across, from x -5 to 5 and z -20 to -10, in a material, tinted.
    const std::uint64_t terrain = fbe_entity_create(world.handle, 0, FBE_ENTITY_TERRAIN);
    REQUIRE(fbe_terrain_set_size(terrain, 10));
    fbe_entity_set_position(terrain, -5.0F, -2.0F, -20.0F, FBE_SPACE_WORLD);
    const std::uint64_t material = fbe_material_create();
    fbe_terrain_set_material(terrain, material);
    fbe_terrain_set_tint(terrain, 0.5F, 1.0F, 1.0F, 1.0F);
    std::vector<fbe_draw_t> found = draws(camera);
    REQUIRE(found.size() == 1);
    CHECK(found[0].entity == terrain);
    CHECK(found[0].kind == FBE_ENTITY_TERRAIN);
    CHECK(found[0].geometry == 0);
    CHECK(found[0].surface == -1);
    CHECK(found[0].material == material);
    CHECK(found[0].blend == FBE_BLEND_OPAQUE);
    CHECK(near(found[0].color[0], 0.5F));
    CHECK(near(found[0].matrix[14], -20.0F));
    // Its depth is its grid's middle's, not its corner's.
    CHECK(near(found[0].depth, 15.0F));
    // Its corner far off to the side, its grid still across the view: still drawn.
    REQUIRE(fbe_terrain_set_size(terrain, 200));
    fbe_entity_set_position(terrain, -150.0F, -2.0F, -60.0F, FBE_SPACE_WORLD);
    CHECK(draws(camera).size() == 1);
    // All of it behind the camera, or with no alpha left: none.
    fbe_entity_set_position(terrain, -100.0F, -2.0F, 1.0F, FBE_SPACE_WORLD);
    CHECK(draws(camera).empty());
    fbe_entity_set_position(terrain, -5.0F, -2.0F, -20.0F, FBE_SPACE_WORLD);
    fbe_terrain_set_tint(terrain, 1.0F, 1.0F, 1.0F, 0.0F);
    CHECK(draws(camera).empty());
    fbe_material_release(material);
    // A ground plane is drawn from above it, but not with the whole view under it.
    const test_world_t open;
    const std::uint64_t below = fbe_entity_create(open.handle, 0, FBE_ENTITY_CAMERA);
    fbe_camera_set_range(below, 0.1F, 50.0F);
    const std::uint64_t ground = fbe_entity_create(open.handle, 0, FBE_ENTITY_PLANE);
    fbe_entity_set_rotation(below, -0.70710678F, 0.0F, 0.0F, 0.70710678F, FBE_SPACE_WORLD);
    fbe_entity_set_position(below, 0.0F, 5.0F, 0.0F, FBE_SPACE_WORLD);
    found = draws(below);
    REQUIRE(found.size() == 1);
    CHECK(found[0].entity == ground);
    fbe_entity_set_position(below, 0.0F, -10.0F, 0.0F, FBE_SPACE_WORLD);
    CHECK(draws(below).empty());
}

TEST_CASE("a camera's draws are seen from where it is between the steps, sprites too", "[render]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    // A gun held 1 ahead of the camera, as its child, and a sprite 20 ahead.
    const std::uint64_t gun = cube(world.handle, 0.0F, 0.0F, 0.0F);
    REQUIRE(fbe_entity_set_parent(gun, camera, false));
    fbe_entity_set_scale(gun, 0.1F, 0.1F, 0.1F, FBE_SPACE_LOCAL);
    fbe_entity_set_position(gun, 0.0F, 0.0F, -1.0F, FBE_SPACE_LOCAL);
    const std::uint64_t sprite = fbe_entity_create(world.handle, 0, FBE_ENTITY_SPRITE);
    fbe_entity_set_position(sprite, -2.0F, 0.0F, -20.0F, FBE_SPACE_WORLD);
    REQUIRE(fbe_world_capture(world.handle));
    // A step: the camera walks 2 ahead, the sprite 4 to the right.
    fbe_entity_translate(camera, 0.0F, 0.0F, -2.0F, FBE_SPACE_WORLD);
    fbe_entity_translate(sprite, 4.0F, 0.0F, 0.0F, FBE_SPACE_WORLD);
    const std::vector<fbe_draw_t> found = draws(camera, 0.5F);
    REQUIRE(found.size() == 2);
    for (const fbe_draw_t& draw : found) {
        if (draw.entity == gun) {
            // Still 1 ahead, from where the camera is half way.
            CHECK(near(draw.matrix[14], -2.0F));
            CHECK(near(draw.depth, 1.0F));
        } else {
            CHECK(draw.entity == sprite);
            CHECK(near(draw.matrix[12], 0.0F));
            CHECK(near(draw.depth, 19.0F));
        }
    }
    // The view matrix half way puts the camera at z -1; now, at -2.
    fbe_mat4_t view{};
    REQUIRE(fbe_camera_tweened_view_matrix(camera, 0.5F, &view));
    CHECK(near(view.m[14], 1.0F));
    REQUIRE(fbe_camera_view_matrix(camera, &view));
    CHECK(near(view.m[14], 2.0F));
    CHECK_FALSE(fbe_camera_tweened_view_matrix(camera, 1.5F, &view));
    // A billboard faces the camera as it's turned half way: 30 degrees of 60.
    const test_world_t turning;
    const std::uint64_t eye = fbe_entity_create(turning.handle, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t ahead = fbe_entity_create(turning.handle, 0, FBE_ENTITY_SPRITE);
    fbe_entity_set_position(ahead, 0.0F, 0.0F, -10.0F, FBE_SPACE_WORLD);
    REQUIRE(fbe_world_capture(turning.handle));
    fbe_entity_set_rotation(eye, 0.0F, 0.5F, 0.0F, 0.8660254F, FBE_SPACE_WORLD);
    const std::vector<fbe_draw_t> seen = draws(eye, 0.5F);
    REQUIRE(seen.size() == 1);
    CHECK(near(seen[0].matrix[8], 0.5F));
    CHECK(near(seen[0].matrix[10], 0.8660254F));
}

TEST_CASE("an entity captured on its own is drawn from where it is now, its children with it, "
          "and the rest still tween",
          "[render]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    // A crate teleported, with a cube on it, and another crate that only walks, under a pivot that
    // walks too.
    const std::uint64_t carried = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    const std::uint64_t teleported = cube(world.handle, 0.0F, 0.0F, -20.0F);
    REQUIRE(fbe_entity_set_parent(teleported, carried, true));
    const std::uint64_t on_it = cube(world.handle, 0.0F, 1.0F, -20.0F);
    REQUIRE(fbe_entity_set_parent(on_it, teleported, true));
    const std::uint64_t walking = cube(world.handle, -4.0F, 0.0F, -20.0F);
    // And one at the top of the World, cut from one place to another.
    const std::uint64_t cut = cube(world.handle, -8.0F, 0.0F, -20.0F);
    REQUIRE(fbe_world_capture(world.handle));
    fbe_entity_translate(carried, 2.0F, 0.0F, 0.0F, FBE_SPACE_WORLD);
    fbe_entity_set_position(teleported, 6.0F, 0.0F, -20.0F, FBE_SPACE_WORLD);
    fbe_entity_translate(walking, 2.0F, 0.0F, 0.0F, FBE_SPACE_WORLD);
    // What's on it slides along it too, in the same step.
    fbe_entity_translate(on_it, 0.0F, 0.0F, 3.0F, FBE_SPACE_LOCAL);
    REQUIRE(fbe_entity_capture(teleported));
    fbe_entity_set_position(cut, -8.0F, 5.0F, -20.0F, FBE_SPACE_WORLD);
    REQUIRE(fbe_entity_capture(cut));
    const auto at_of = [&](std::uint64_t entity, float tween, std::size_t axis) {
        for (const fbe_draw_t& draw : draws(camera, tween)) {
            if (draw.entity == entity) {
                return draw.matrix[12 + axis];
            }
        }
        return -1000.0F;
    };
    const auto x_of = [&](std::uint64_t entity, float tween) { return at_of(entity, tween, 0); };
    for (const float tween : { 0.0F, 0.5F, 1.0F }) {
        INFO("at " << tween);
        CHECK(near(x_of(teleported, tween), 6.0F));
        CHECK(near(x_of(on_it, tween), 6.0F));
        CHECK(near(at_of(on_it, tween, 2), -17.0F));
        CHECK(near(at_of(cut, tween, 1), 5.0F));
        CHECK(near(x_of(walking, tween), -4.0F + (2.0F * tween)));
    }
    // Moved after, it tweens from where it was captured.
    fbe_entity_set_position(teleported, 8.0F, 0.0F, -20.0F, FBE_SPACE_WORLD);
    CHECK(near(x_of(teleported, 0.0F), 6.0F));
    CHECK(near(x_of(teleported, 0.5F), 7.0F));
    CHECK_FALSE(fbe_entity_capture(0));
}

TEST_CASE("an entity's place, its turn, both, or neither is tweened, as it's set", "[render]") {
    const test_world_t world;
    // The player's eyes: the tick walks them 4 to the right, and the mouse turns them a quarter
    // turn to the left in the same step, and they grow, to see its scale's own way.
    const std::uint64_t eyes = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t held = fbe_entity_create(world.handle, eyes, FBE_ENTITY_PIVOT);
    fbe_entity_set_position(held, 0.0F, 0.0F, -1.0F, FBE_SPACE_LOCAL);
    CHECK(fbe_entity_tweened(eyes) == FBE_TWEENED_ALL);
    REQUIRE(fbe_world_capture(world.handle));
    fbe_entity_set_position(eyes, 4.0F, 0.0F, 0.0F, FBE_SPACE_WORLD);
    fbe_entity_set_scale(eyes, 3.0F, 3.0F, 3.0F, FBE_SPACE_LOCAL);
    const float half = std::sqrt(0.5F);
    fbe_entity_set_rotation(eyes, 0.0F, half, 0.0F, half, FBE_SPACE_WORLD);
    struct drawn_t {
        float x;
        float ahead_x;
        float ahead_z;
        float size;
    };
    // Where it's drawn half way, which way its -Z points, and how long its X axis is.
    const auto drawn = [&](std::uint64_t entity) {
        fbe_mat4_t matrix{};
        REQUIRE(fbe_entity_tweened_matrix(entity, 0.5F, &matrix));
        const float size = std::sqrt((matrix.m[0] * matrix.m[0]) + (matrix.m[1] * matrix.m[1]) +
                                     (matrix.m[2] * matrix.m[2]));
        return drawn_t{
            .x = matrix.m[12],
            .ahead_x = -matrix.m[8] / size,
            .ahead_z = -matrix.m[10] / size,
            .size = size,
        };
    };
    // All of it, as ever: half way there, an eighth of a turn, twice the size.
    drawn_t found = drawn(eyes);
    CHECK(near(found.x, 2.0F));
    CHECK(near(found.ahead_x, -half));
    CHECK(near(found.ahead_z, -half));
    CHECK(near(found.size, 2.0F));
    // Its place alone: half way there, looking where it looks now.
    REQUIRE(fbe_entity_set_tweened(eyes, FBE_TWEENED_PLACE));
    CHECK(fbe_entity_tweened(eyes) == FBE_TWEENED_PLACE);
    found = drawn(eyes);
    CHECK(near(found.x, 2.0F));
    CHECK(near(found.ahead_x, -1.0F));
    CHECK(near(found.ahead_z, 0.0F));
    CHECK(near(found.size, 2.0F));
    // What it holds is 1 ahead of that, at its drawn size: to its left.
    CHECK(near(drawn(held).x, 2.0F - 2.0F));
    // Its turn alone: where it is now, an eighth of a turn.
    REQUIRE(fbe_entity_set_tweened(eyes, FBE_TWEENED_TURN));
    found = drawn(eyes);
    CHECK(near(found.x, 4.0F));
    CHECK(near(found.ahead_x, -half));
    CHECK(near(found.size, 2.0F));
    // Neither: as it is, size and all.
    REQUIRE(fbe_entity_set_tweened(eyes, FBE_TWEENED_NONE));
    found = drawn(eyes);
    CHECK(near(found.x, 4.0F));
    CHECK(near(found.ahead_x, -1.0F));
    CHECK(near(found.size, 3.0F));
    CHECK(near(drawn(held).x, 4.0F - 3.0F));
    // A copy keeps it; one made new tweens all.
    const std::uint64_t copy = fbe_entity_copy(eyes, 0, 0);
    CHECK(fbe_entity_tweened(copy) == FBE_TWEENED_NONE);
    fbe_entity_free(copy);
    std::array<std::uint64_t, 16> gone{};
    while (fbe_entity_freed(gone.data(), gone.size()) != 0) {}
    // Whichever of these is made where the copy was.
    for (int i = 0; i < 4; ++i) {
        CHECK(fbe_entity_tweened(fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT)) ==
              FBE_TWEENED_ALL);
    }
    CHECK_FALSE(fbe_entity_set_tweened(eyes, 4));
    CHECK(std::string_view(fbe_error()) == "4 isn't a Tweened");
    CHECK_FALSE(fbe_entity_set_tweened(eyes, -1));
    CHECK(fbe_entity_tweened(0) == -1);
}

TEST_CASE("a camera on its place alone draws what's ahead of its new turn from its tweened place",
          "[render]") {
    const test_world_t world;
    const std::uint64_t eyes = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    REQUIRE(fbe_entity_set_tweened(eyes, FBE_TWEENED_PLACE));
    // One cube 20 ahead of where it starts, and one 20 to the left of where it's half way.
    const std::uint64_t ahead = cube(world.handle, 0.0F, 0.0F, -20.0F);
    const std::uint64_t left = cube(world.handle, -18.0F, 0.0F, 0.0F);
    REQUIRE(fbe_world_capture(world.handle));
    fbe_entity_set_position(eyes, 4.0F, 0.0F, 0.0F, FBE_SPACE_WORLD);
    const float half = std::sqrt(0.5F);
    fbe_entity_set_rotation(eyes, 0.0F, half, 0.0F, half, FBE_SPACE_WORLD);
    const std::vector<fbe_draw_t> found = draws(eyes, 0.5F);
    REQUIRE(found.size() == 1);
    CHECK(found[0].entity == left);
    CHECK(found[0].entity != ahead);
    // 20 from where it's drawn, half way, not 22 from where it is.
    CAPTURE(found[0].depth);
    CHECK(near(found[0].depth, 20.0F));
}

TEST_CASE("a camera's draw projection holds with its far plane near float's top", "[render]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    REQUIRE(fbe_camera_set_range(camera, 10.0F, 1e38F));
    fbe_mat4_t p{};
    REQUIRE(fbe_camera_draw_projection(camera, 1600.0F, 900.0F, &p));
    for (const float value : p.m) {
        CHECK(std::isfinite(value));
    }
    // At the near plane, depth 1.
    const float z = (p.m[10] * -10.0F) + p.m[14];
    const float w = (p.m[11] * -10.0F) + p.m[15];
    CHECK(near(z / w, 1.0F));
}

TEST_CASE("draws that tie keep their order when something else is freed", "[render]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    // Made first, so freeing it moves the meshes' last into its place in their pool.
    const std::uint64_t gone = cube(world.handle, 0.0F, 0.0F, -50.0F);
    std::array<std::uint64_t, 3> tied{};
    // Drawn of one, as they go by geometry before anything that can tie.
    const std::uint64_t shared = fbe_mesh_geometry(gone);
    for (std::uint64_t& made : tied) {
        made = cube(world.handle, 0.0F, 0.0F, -5.0F);
        fbe_mesh_set_geometry(made, shared);
    }
    const auto order = [&] {
        std::vector<std::uint64_t> found;
        for (const fbe_draw_t& draw : draws(camera)) {
            if (draw.entity != gone) {
                found.push_back(draw.entity);
            }
        }
        return found;
    };
    const std::vector<std::uint64_t> made_order(tied.begin(), tied.end());
    CHECK(order() == made_order);
    REQUIRE(fbe_entity_free(gone));
    CHECK(order() == made_order);
}

TEST_CASE("a draw's depth mode and pipeline say how it's drawn, and order the scene's",
          "[render]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    fbe_camera_set_fog_mode(camera, FBE_FOG_LINEAR);
    // A sky behind the scene and an overlay over it, both nearer than the scene's cubes.
    const std::uint64_t behind = fbe_material_create();
    fbe_material_set_order(behind, -1);
    const std::uint64_t over = fbe_material_create();
    fbe_material_set_order(over, 1);
    const std::uint64_t sky_near = cube(world.handle, 0.0F, 0.0F, -3.0F, behind);
    const std::uint64_t sky_far = cube(world.handle, 2.0F, 0.0F, -9.0F, behind);
    const std::uint64_t overlay = cube(world.handle, -2.0F, 0.0F, -4.0F, over);
    // In the scene: plain cubes near and far, one in an unlit material between, and glass.
    const std::uint64_t unlit = fbe_material_create();
    fbe_material_set_shading(unlit, FBE_SHADING_UNLIT);
    const std::uint64_t near_cube = cube(world.handle, 0.0F, 0.0F, -6.0F);
    const std::uint64_t middle = cube(world.handle, 1.0F, 1.0F, -7.0F, unlit);
    const std::uint64_t far_cube = cube(world.handle, -1.0F, 0.0F, -8.0F);
    const std::uint64_t glass = fbe_material_create();
    fbe_material_set_blend(glass, FBE_BLEND_ADD);
    fbe_material_set_alpha_cutoff(glass, 0.5F);
    fbe_material_set_vertex_colors(glass, true);
    fbe_material_set_two_sided(glass, true);
    fbe_material_set_fogged(glass, false);
    const std::uint64_t pane = cube(world.handle, 3.0F, 0.0F, -10.0F, glass);
    const std::vector<fbe_draw_t> found = draws(camera);
    REQUIRE(found.size() == 7);
    // The sky far first, then the scene's opaque by pipeline, the default material's before the
    // unlit one's, nearest first, then the glass, then the overlay.
    const std::array order{ sky_far, sky_near, near_cube, far_cube, middle, pane, overlay };
    for (std::size_t i = 0; i < order.size(); ++i) {
        CAPTURE(i);
        CHECK(found[i].entity == order[i]);
    }
    CHECK(found[0].depth_mode == FBE_DEPTH_NONE);
    CHECK(found[2].depth_mode == FBE_DEPTH_TEST_AND_WRITE);
    CHECK(found[5].depth_mode == FBE_DEPTH_TEST);
    CHECK(found[6].depth_mode == FBE_DEPTH_NONE);
    // A new material's pipeline: PBR, opaque, fogged linearly, testing and writing depth.
    const auto bits = [](std::int32_t value, unsigned at) {
        return static_cast<std::uint64_t>(value) << at;
    };
    const std::uint64_t plain = bits(FBE_SHADING_PBR, 0) | bits(FBE_BLEND_OPAQUE, 2) |
                                bits(FBE_FOG_LINEAR, 8) | bits(FBE_DEPTH_TEST_AND_WRITE, 10);
    CHECK(found[2].pipeline == plain);
    CHECK(found[4].pipeline == (plain | bits(FBE_SHADING_UNLIT, 0)));
    // The glass: added, cut off, vertex colored, two-sided, no fog, and depth tested only.
    const std::uint64_t added = bits(FBE_SHADING_PBR, 0) | bits(FBE_BLEND_ADD, 2) | bits(1, 5) |
                                bits(1, 6) | bits(1, 7) | bits(FBE_FOG_NONE, 8) |
                                bits(FBE_DEPTH_TEST, 10);
    CHECK(found[5].pipeline == added);
    for (const std::uint64_t material : { behind, over, unlit, glass }) {
        fbe_material_release(material);
    }
}

TEST_CASE("a mirrored draw keys and sorts apart from an unmirrored one", "[render]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t plain = cube(world.handle, -2.0F, 0.0F, -6.0F);
    const std::uint64_t mirrored = cube(world.handle, 2.0F, 0.0F, -9.0F);
    fbe_entity_set_scale(mirrored, -1.0F, 1.0F, 1.0F, FBE_SPACE_WORLD);
    const std::uint64_t twice = cube(world.handle, 0.0F, 0.0F, -12.0F);
    fbe_entity_set_scale(twice, -1.0F, -1.0F, 1.0F, FBE_SPACE_WORLD);
    const std::uint64_t nearer = cube(world.handle, 0.0F, 2.0F, -4.0F);
    fbe_entity_set_scale(nearer, 1.0F, 1.0F, -1.0F, FBE_SPACE_WORLD);
    // By geometry before depth: drawn of one, each pipeline's nearest first.
    const std::uint64_t shared = fbe_mesh_geometry(plain);
    fbe_mesh_set_geometry(mirrored, shared);
    fbe_mesh_set_geometry(twice, shared);
    fbe_mesh_set_geometry(nearer, shared);
    const std::vector<fbe_draw_t> found = draws(camera);
    REQUIRE(found.size() == 4);
    // Unmirrored first though further, each pipeline nearest first; two flips aren't a mirror.
    CHECK(found[0].entity == plain);
    CHECK(found[1].entity == twice);
    CHECK(found[2].entity == nearer);
    CHECK(found[3].entity == mirrored);
    constexpr std::uint64_t bit = 1ULL << 12U;
    CHECK(found[1].pipeline == found[0].pipeline);
    CHECK(found[3].pipeline == found[2].pipeline);
    CHECK(found[2].pipeline == (found[0].pipeline | bit));
    CHECK((found[0].pipeline & bit) == 0);
}

TEST_CASE("in an order but 0, what's opaque draws before what blends at its depth", "[render]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t geometry = fbe_geometry_cube();
    fbe_mat4_t identity{};
    identity.m[0] = 1.0F;
    identity.m[5] = 1.0F;
    identity.m[10] = 1.0F;
    identity.m[15] = 1.0F;
    fbe_geometry_add(geometry, geometry, &identity);
    const std::uint64_t clouds = fbe_material_create();
    fbe_material_set_blend(clouds, FBE_BLEND_ALPHA);
    fbe_material_set_order(clouds, -1);
    const std::uint64_t sky = fbe_material_create();
    fbe_material_set_order(sky, -1);
    fbe_surface_set_material(geometry, 0, clouds);
    fbe_surface_set_material(geometry, 1, sky);
    const std::uint64_t dome = fbe_entity_create(world.handle, 0, FBE_ENTITY_MESH);
    fbe_mesh_set_geometry(dome, geometry);
    fbe_geometry_release(geometry);
    fbe_entity_set_position(dome, 0.0F, 0.0F, -5.0F, FBE_SPACE_WORLD);
    const std::vector<fbe_draw_t> found = draws(camera);
    REQUIRE(found.size() == 2);
    CHECK(found[0].surface == 1);
    CHECK(found[0].blend == FBE_BLEND_OPAQUE);
    CHECK(found[1].surface == 0);
    CHECK(found[1].blend == FBE_BLEND_ALPHA);
    CHECK(found[0].depth_mode == FBE_DEPTH_NONE);
    CHECK(found[1].depth_mode == FBE_DEPTH_NONE);
    fbe_material_release(clouds);
    fbe_material_release(sky);
}
