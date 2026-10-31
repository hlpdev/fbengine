// Ground planes, mirrors, fading from a camera, and meshes crossing, through the C API.

#include "fbengine.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>

namespace {

constexpr std::int32_t mover_group = 1;
constexpr std::int32_t level_group = 2;

/// A world of the test's own, freed at its end, where movers slide on the level.
struct test_world_t {
    std::uint64_t handle = fbe_world_create();

    test_world_t() {
        fbe_world_set_collision(handle, mover_group, level_group, FBE_RESPONSE_SLIDE);
    }
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

fbe_vec3_t at(float x, float y, float z) {
    return { .x = x, .y = y, .z = z };
}

bool near(float a, float b) {
    return std::fabs(a - b) < 0.001F;
}

fbe_vec3_t position(std::uint64_t entity) {
    fbe_vec3_t found{};
    fbe_entity_position(entity, FBE_SPACE_WORLD, &found);
    return found;
}

std::uint64_t mover(std::uint64_t world, float x, float y, float z) {
    const std::uint64_t made = fbe_entity_create(world, 0, FBE_ENTITY_PIVOT);
    fbe_entity_set_collision_group(made, mover_group);
    fbe_entity_set_collision_radius(made, 0.5F, 0.5F);
    fbe_entity_set_position(made, x, y, z, FBE_SPACE_WORLD);
    fbe_world_collide(world);
    return made;
}

void move(std::uint64_t world, std::uint64_t entity, float x, float y, float z) {
    fbe_entity_translate(entity, x, y, z, FBE_SPACE_WORLD);
    fbe_world_collide(world);
}

/// A cube mesh, 2 across, at a place.
std::uint64_t cube(std::uint64_t world, float x, float y, float z) {
    const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    const std::uint64_t geometry = fbe_geometry_cube();
    fbe_mesh_set_geometry(mesh, geometry);
    fbe_geometry_release(geometry);
    fbe_entity_set_position(mesh, x, y, z, FBE_SPACE_WORLD);
    return mesh;
}

} // namespace

TEST_CASE("a ground plane holds movers up and stops picks from above, however far", "[scene]") {
    const test_world_t world;
    const std::uint64_t plane = fbe_entity_create(world.handle, 0, FBE_ENTITY_PLANE);
    CHECK(fbe_entity_kind(plane) == FBE_ENTITY_PLANE);
    fbe_entity_set_collision_group(plane, level_group);
    fbe_entity_set_position(plane, 0.0F, 1.0F, 0.0F, FBE_SPACE_WORLD);
    // Far out, a mover falls onto it and stays.
    const std::uint64_t made = mover(world.handle, 5000.0F, 4.0F, -3000.0F);
    move(world.handle, made, 0.0F, -10.0F, 0.0F);
    CHECK(near(position(made).y, 1.501F));
    move(world.handle, made, 2.0F, -0.5F, 0.0F);
    CHECK(near(position(made).y, 1.501F));
    CHECK(near(position(made).x, 5002.0F));
    // From under it, a mover goes up through.
    const std::uint64_t under = mover(world.handle, -700.0F, -3.0F, 0.0F);
    move(world.handle, under, 0.0F, 10.0F, 0.0F);
    CHECK(near(position(under).y, 7.0F));
    // Picks hit it from above, anywhere, and pass up through it.
    fbe_entity_set_pick_mode(plane, FBE_PICK_MESH);
    fbe_pick_t hit{};
    const fbe_vec3_t from = at(-9000.0F, 5.0F, 9000.0F);
    const fbe_vec3_t to = at(-9000.0F, -5.0F, 9000.0F);
    REQUIRE(fbe_world_pick(world.handle, &from, &to, 0.0F, 0, &hit) == 1);
    CHECK(hit.entity == plane);
    CHECK(near(hit.point.y, 1.0F));
    CHECK(near(hit.normal.y, 1.0F));
    CHECK(fbe_world_pick(world.handle, &to, &from, 0.0F, 0, &hit) == 0);
    // Tilted a quarter about X, it faces +Z: a mover walking -Z stops at it.
    fbe_entity_set_rotation(plane, 0.70710678F, 0.0F, 0.0F, 0.70710678F, FBE_SPACE_WORLD);
    fbe_entity_set_position(plane, 0.0F, 0.0F, -2.0F, FBE_SPACE_WORLD);
    const std::uint64_t walker = mover(world.handle, 300.0F, 50.0F, 0.0F);
    move(world.handle, walker, 0.0F, 0.0F, -5.0F);
    CHECK(near(position(walker).z, -1.499F));
}

TEST_CASE("a ground plane's settings take what they can be, and copy", "[scene]") {
    const test_world_t world;
    const std::uint64_t ground = fbe_entity_create(world.handle, 0, FBE_ENTITY_PLANE);
    CHECK(fbe_plane_segments(ground) == 1);
    CHECK(fbe_plane_set_segments(ground, 16));
    CHECK_FALSE(fbe_plane_set_segments(ground, 17));
    CHECK_FALSE(fbe_plane_set_segments(ground, 0));
    const std::uint64_t material = fbe_material_create();
    REQUIRE(fbe_plane_set_material(ground, material));
    fbe_material_release(material);
    CHECK(fbe_material_alive(material));
    CHECK(fbe_plane_set_tint(ground, 0.5F, 0.5F, 0.5F, 1.0F));
    CHECK_FALSE(fbe_plane_set_tint(ground, 1.0F, 1.0F, 1.0F, -0.5F));
    const std::uint64_t copy = fbe_entity_copy(ground, world.handle, 0);
    CHECK(fbe_plane_segments(copy) == 16);
    CHECK(fbe_plane_material(copy) == material);
    fbe_color_t tint{};
    fbe_plane_tint(copy, &tint);
    CHECK(tint.r == 0.5F);
    fbe_entity_free(ground);
    CHECK(fbe_material_alive(material));
    fbe_entity_free(copy);
    CHECK_FALSE(fbe_material_alive(material));
    // A pivot isn't a plane.
    const std::uint64_t pivot = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    CHECK(fbe_plane_segments(pivot) == -1);
}

TEST_CASE("a mirror reflects the world through its own plane", "[scene]") {
    const test_world_t world;
    const std::uint64_t mirror = fbe_entity_create(world.handle, 0, FBE_ENTITY_MIRROR);
    fbe_entity_set_position(mirror, 0.0F, 2.0F, 0.0F, FBE_SPACE_WORLD);
    fbe_mat4_t m{};
    REQUIRE(fbe_mirror_reflection(mirror, &m));
    // (3, 5, 1) reflects to (3, -1, 1).
    const auto reflect = [&](float x, float y, float z) {
        return at((m.m[0] * x) + (m.m[4] * y) + (m.m[8] * z) + m.m[12],
                  (m.m[1] * x) + (m.m[5] * y) + (m.m[9] * z) + m.m[13],
                  (m.m[2] * x) + (m.m[6] * y) + (m.m[10] * z) + m.m[14]);
    };
    const fbe_vec3_t a = reflect(3.0F, 5.0F, 1.0F);
    CHECK((near(a.x, 3.0F) && near(a.y, -1.0F) && near(a.z, 1.0F)));
    // Upright on a wall at x = 4, facing -X: (1, 0, 0) reflects to (7, 0, 0).
    fbe_entity_set_rotation(mirror, 0.0F, 0.0F, 0.70710678F, 0.70710678F, FBE_SPACE_WORLD);
    fbe_entity_set_position(mirror, 4.0F, 0.0F, 0.0F, FBE_SPACE_WORLD);
    REQUIRE(fbe_mirror_reflection(mirror, &m));
    const fbe_vec3_t b = reflect(1.0F, 0.0F, 0.0F);
    CHECK((near(b.x, 7.0F) && near(b.y, 0.0F) && near(b.z, 0.0F)));
    // A pivot reflects nothing.
    const std::uint64_t pivot = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    CHECK_FALSE(fbe_mirror_reflection(pivot, &m));
}

TEST_CASE("an entity fades out between two distances from a camera", "[scene]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t prop = cube(world.handle, 0.0F, 0.0F, -5.0F);
    CHECK(fbe_entity_fade(prop, camera) == 1.0F);
    REQUIRE(fbe_entity_set_auto_fade(prop, 10.0F, 20.0F));
    fbe_vec2_t fade{};
    fbe_entity_auto_fade(prop, &fade);
    CHECK((fade.x == 10.0F && fade.y == 20.0F));
    CHECK(fbe_entity_fade(prop, camera) == 1.0F);
    fbe_entity_set_position(prop, 0.0F, 0.0F, -15.0F, FBE_SPACE_WORLD);
    CHECK(near(fbe_entity_fade(prop, camera), 0.5F));
    fbe_entity_set_position(prop, 0.0F, 0.0F, -25.0F, FBE_SPACE_WORLD);
    CHECK(fbe_entity_fade(prop, camera) == 0.0F);
    // Copies fade alike; 0 and 0 is never; backward or negative is refused.
    const std::uint64_t copy = fbe_entity_copy(prop, world.handle, 0);
    CHECK(fbe_entity_fade(copy, camera) == 0.0F);
    CHECK(fbe_entity_set_auto_fade(prop, 0.0F, 0.0F));
    CHECK(fbe_entity_fade(prop, camera) == 1.0F);
    CHECK_FALSE(fbe_entity_set_auto_fade(prop, 20.0F, 10.0F));
    CHECK_FALSE(fbe_entity_set_auto_fade(prop, -1.0F, 10.0F));
    CHECK_FALSE(fbe_entity_set_auto_fade(prop, 1.0F, INFINITY));
}

TEST_CASE("meshes intersect where their triangles cross, not where one holds the other",
          "[scene]") {
    const test_world_t world;
    const std::uint64_t one = cube(world.handle, 0.0F, 0.0F, 0.0F);
    const std::uint64_t two = cube(world.handle, 1.5F, 0.5F, 0.25F);
    CHECK(fbe_mesh_intersects(one, two) == 1);
    CHECK(fbe_mesh_intersects(two, one) == 1);
    fbe_entity_set_position(two, 2.5F, 0.0F, 0.0F, FBE_SPACE_WORLD);
    CHECK(fbe_mesh_intersects(one, two) == 0);
    // Turned a quarter-ish so a corner pokes into the other.
    fbe_entity_set_rotation(two, 0.0F, 0.38268343F, 0.0F, 0.92387953F, FBE_SPACE_WORLD);
    fbe_entity_set_position(two, 2.2F, 0.0F, 0.0F, FBE_SPACE_WORLD);
    CHECK(fbe_mesh_intersects(one, two) == 1);
    // One wholly inside the other has no triangles crossing.
    fbe_entity_set_rotation(two, 0.0F, 0.0F, 0.0F, 1.0F, FBE_SPACE_WORLD);
    fbe_entity_set_scale(two, 0.25F, 0.25F, 0.25F, FBE_SPACE_LOCAL);
    fbe_entity_set_position(two, 0.0F, 0.0F, 0.0F, FBE_SPACE_WORLD);
    CHECK(fbe_mesh_intersects(one, two) == 0);
    // Not a mesh, or one with nothing to draw.
    const std::uint64_t pivot = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    CHECK(fbe_mesh_intersects(one, pivot) == -1);
    const std::uint64_t empty = fbe_entity_create(world.handle, 0, FBE_ENTITY_MESH);
    CHECK(fbe_mesh_intersects(one, empty) == 0);
}
