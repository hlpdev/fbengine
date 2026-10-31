// Picks through the C API: each pick mode, from the front and from behind, with and without a
// radius, the nearest of several, and what a pick skips.

#include "fbengine.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <random>
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

fbe_vec3_t at(float x, float y, float z) {
    return { .x = x, .y = y, .z = z };
}

bool near(float a, float b) {
    return std::fabs(a - b) < 0.001F;
}

/// The nearest hit from one point to another, or entity 0 for none.
fbe_pick_t pick(std::uint64_t world, fbe_vec3_t from, fbe_vec3_t to, float radius = 0.0F,
                std::uint64_t ignore = 0) {
    fbe_pick_t found{};
    if (fbe_world_pick(world, &from, &to, radius, ignore, &found) != 1) {
        found = {};
    }
    return found;
}

/// A mesh of a geometry, picked by its triangles, which it lets go of.
std::uint64_t mesh(std::uint64_t world, std::uint64_t geometry) {
    const std::uint64_t made = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    fbe_mesh_set_geometry(made, geometry);
    fbe_geometry_release(geometry);
    fbe_entity_set_pick_mode(made, FBE_PICK_MESH);
    return made;
}

/// A pivot at a place, picked as `mode`.
std::uint64_t pivot(std::uint64_t world, float x, float y, float z, std::int32_t mode) {
    const std::uint64_t made = fbe_entity_create(world, 0, FBE_ENTITY_PIVOT);
    fbe_entity_set_position(made, x, y, z, FBE_SPACE_WORLD);
    fbe_entity_set_pick_mode(made, mode);
    return made;
}

} // namespace

TEST_CASE("a line picks a mesh's triangles from their fronts only", "[scene]") {
    const test_world_t world;
    // A plane at y = 0, facing up, 2 across.
    const std::uint64_t floor = mesh(world.handle, fbe_geometry_plane(1));
    const fbe_pick_t down = pick(world.handle, at(0.5F, 2.0F, 0.25F), at(0.5F, -2.0F, 0.25F));
    CHECK(down.entity == floor);
    CHECK(near(down.point.x, 0.5F));
    CHECK(near(down.point.y, 0.0F));
    CHECK(near(down.point.z, 0.25F));
    CHECK(near(down.normal.y, 1.0F));
    CHECK(near(down.time, 0.5F));
    CHECK(down.geometry != 0);
    CHECK(down.surface == 0);
    CHECK(down.triangle >= 0);
    // From under it, the line meets its back and goes on.
    CHECK(pick(world.handle, at(0.5F, -2.0F, 0.25F), at(0.5F, 2.0F, 0.25F)).entity == 0);
    // Past its edge, or short of it, nothing.
    CHECK(pick(world.handle, at(1.5F, 2.0F, 0.0F), at(1.5F, -2.0F, 0.0F)).entity == 0);
    CHECK(pick(world.handle, at(0.0F, 2.0F, 0.0F), at(0.0F, 0.5F, 0.0F)).entity == 0);
    // Scaled, turned over, and moved, it's picked where it is.
    fbe_entity_set_scale(floor, 10.0F, 1.0F, 10.0F, FBE_SPACE_LOCAL);
    fbe_entity_set_rotation(floor, 1.0F, 0.0F, 0.0F, 0.0F, FBE_SPACE_LOCAL);
    fbe_entity_set_position(floor, 0.0F, 3.0F, 0.0F, FBE_SPACE_LOCAL);
    const fbe_pick_t up = pick(world.handle, at(5.0F, 0.0F, 5.0F), at(5.0F, 10.0F, 5.0F));
    CHECK(up.entity == floor);
    CHECK(near(up.point.y, 3.0F));
    CHECK(near(up.normal.y, -1.0F));
    // Mirrored, its front is still the side it faces.
    fbe_entity_set_scale(floor, -10.0F, 1.0F, 10.0F, FBE_SPACE_LOCAL);
    CHECK(pick(world.handle, at(5.0F, 0.0F, 5.0F), at(5.0F, 10.0F, 5.0F)).entity == floor);
    CHECK(pick(world.handle, at(5.0F, 10.0F, 5.0F), at(5.0F, 0.0F, 5.0F)).entity == 0);
}

TEST_CASE("an ellipsoid and a box are picked by their own shapes", "[scene]") {
    const test_world_t world;
    const std::uint64_t ball = pivot(world.handle, 0.0F, 0.0F, -5.0F, FBE_PICK_ELLIPSOID);
    fbe_entity_set_collision_radius(ball, 0.5F, 2.0F);
    const fbe_pick_t side = pick(world.handle, at(0.0F, 0.0F, 0.0F), at(0.0F, 0.0F, -10.0F));
    CHECK(side.entity == ball);
    CHECK(near(side.point.z, -4.5F));
    CHECK(near(side.normal.z, 1.0F));
    CHECK(side.geometry == 0);
    CHECK(side.triangle == -1);
    // Its vertical radius is 2.
    const fbe_pick_t top = pick(world.handle, at(0.0F, 5.0F, -5.0F), at(0.0F, -5.0F, -5.0F));
    CHECK(near(top.point.y, 2.0F));
    CHECK(near(top.normal.y, 1.0F));

    // A box from -1 to 1, scaled by 2 and turned a quarter about Y, at x = 10.
    const std::uint64_t crate = pivot(world.handle, 10.0F, 0.0F, 0.0F, FBE_PICK_BOX);
    fbe_entity_set_scale(crate, 2.0F, 1.0F, 1.0F, FBE_SPACE_LOCAL);
    const float half = 0.70710678F;
    fbe_entity_set_rotation(crate, 0.0F, half, 0.0F, half, FBE_SPACE_LOCAL);
    // Turned, its long side runs along Z: 1 wide across X, 2 along Z.
    const fbe_pick_t across = pick(world.handle, at(5.0F, 0.0F, 1.5F), at(15.0F, 0.0F, 1.5F));
    CHECK(across.entity == crate);
    CHECK(near(across.point.x, 9.0F));
    CHECK(near(across.normal.x, -1.0F));
    CHECK(pick(world.handle, at(5.0F, 0.0F, 2.5F), at(15.0F, 0.0F, 2.5F)).entity == 0);
    // From inside it, a pick goes out.
    CHECK(pick(world.handle, at(10.0F, 0.0F, 0.0F), at(15.0F, 0.0F, 0.0F)).entity == 0);
}

TEST_CASE("a sphere picks what a line would pass", "[scene]") {
    const test_world_t world;
    // A plane at y = 0, 2 across.
    const std::uint64_t floor = mesh(world.handle, fbe_geometry_plane(1));
    // A line past its edge misses; a sphere of 0.5 along it touches the edge.
    const fbe_vec3_t from = at(1.25F, 2.0F, 0.0F);
    const fbe_vec3_t to = at(1.25F, -2.0F, 0.0F);
    CHECK(pick(world.handle, from, to).entity == 0);
    const fbe_pick_t edge = pick(world.handle, from, to, 0.5F);
    CHECK(edge.entity == floor);
    CHECK(near(edge.point.x, 1.0F));
    CHECK(near(edge.point.y, 0.0F));
    // Where the sphere touches it: its middle is 0.5 from the edge.
    const float middle = 2.0F - (edge.time * 4.0F);
    CHECK(near(std::hypot(1.25F - 1.0F, middle), 0.5F));
    // Over the plane, it touches the face a radius above it.
    const fbe_pick_t face = pick(world.handle, at(0.0F, 2.0F, 0.0F), at(0.0F, -2.0F, 0.0F), 0.5F);
    CHECK(near(face.point.y, 0.0F));
    CHECK(near(face.time, 1.5F / 4.0F));
    // A sphere against an ellipsoid touches it a radius sooner, here above the plane.
    const std::uint64_t ball = pivot(world.handle, 0.0F, 5.0F, -5.0F, FBE_PICK_ELLIPSOID);
    fbe_entity_set_collision_radius(ball, 1.0F, 1.0F);
    const fbe_pick_t round = pick(world.handle, at(0.0F, 5.0F, 0.0F), at(0.0F, 5.0F, -10.0F), 0.5F);
    CHECK(round.entity == ball);
    CHECK(near(round.time, 3.5F / 10.0F));
    CHECK(near(round.point.z, -4.0F));
    // Starting in the plane, a sphere touches it at once.
    CHECK(pick(world.handle, at(0.0F, 0.0F, 0.0F), at(0.0F, 0.0F, -10.0F), 0.5F).time == 0.0F);
    // A radius below 0, or a point that isn't finite, is refused.
    fbe_pick_t found{};
    const fbe_vec3_t nowhere = at(NAN, 0.0F, 0.0F);
    CHECK(fbe_world_pick(world.handle, &from, &to, -1.0F, 0, &found) == -1);
    CHECK(fbe_world_pick(world.handle, &from, &nowhere, 0.0F, 0, &found) == -1);
}

TEST_CASE("the nearest hit wins, and a pick skips what it's told to", "[scene]") {
    const test_world_t world;
    const std::uint64_t first = pivot(world.handle, 0.0F, 0.0F, -3.0F, FBE_PICK_ELLIPSOID);
    const std::uint64_t second = pivot(world.handle, 0.0F, 0.0F, -6.0F, FBE_PICK_ELLIPSOID);
    const fbe_vec3_t from = at(0.0F, 0.0F, 0.0F);
    const fbe_vec3_t to = at(0.0F, 0.0F, -10.0F);
    CHECK(pick(world.handle, from, to).entity == first);
    CHECK(pick(world.handle, to, from).entity == second);
    // Ignored, hidden, with no pick mode, or in another World, it isn't hit.
    CHECK(pick(world.handle, from, to, 0.0F, first).entity == second);
    fbe_entity_set_hidden(first, true);
    CHECK(pick(world.handle, from, to).entity == second);
    fbe_entity_set_hidden(first, false);
    fbe_entity_set_pick_mode(first, FBE_PICK_NONE);
    CHECK(pick(world.handle, from, to).entity == second);
    fbe_entity_set_pick_mode(first, FBE_PICK_ELLIPSOID);
    const test_world_t other;
    const std::uint64_t there = fbe_entity_create(other.handle, 0, FBE_ENTITY_PIVOT);
    fbe_entity_set_parent(first, there, true);
    CHECK(pick(world.handle, from, to).entity == second);
    CHECK(pick(other.handle, from, to).entity == first);
    // A hidden parent hides it too.
    fbe_entity_set_hidden(there, true);
    CHECK(pick(other.handle, from, to).entity == 0);
    // A mesh mode on what has no triangles, a pivot, is never hit.
    fbe_entity_set_pick_mode(second, FBE_PICK_MESH);
    CHECK(pick(world.handle, from, to).entity == 0);
    CHECK(fbe_entity_pick_mode(second) == FBE_PICK_MESH);
    CHECK_FALSE(fbe_entity_set_pick_mode(second, 4));
    // A copy picks as its original does.
    fbe_entity_set_pick_mode(second, FBE_PICK_ELLIPSOID);
    const std::uint64_t copy = fbe_entity_copy(second, world.handle, 0);
    CHECK(fbe_entity_pick_mode(copy) == FBE_PICK_ELLIPSOID);
}

TEST_CASE("picks through a big mesh find the triangle a line crosses first", "[scene]") {
    const test_world_t world;
    // A sphere of 3000 triangles, picked from all round toward its middle.
    const std::uint64_t ball = mesh(world.handle, fbe_geometry_sphere(40));
    fbe_entity_set_scale(ball, 3.0F, 2.0F, 1.0F, FBE_SPACE_LOCAL);
    std::mt19937 random(7); // NOLINT(bugprone-random-generator-seed)
    std::normal_distribution<float> direction(0.0F, 1.0F);
    for (int i = 0; i < 200; ++i) {
        float x = direction(random);
        float y = direction(random);
        float z = direction(random);
        const float size = std::sqrt((x * x) + (y * y) + (z * z));
        x = x / size * 10.0F;
        y = y / size * 10.0F;
        z = z / size * 10.0F;
        const fbe_pick_t hit = pick(world.handle, at(x, y, z), at(0.0F, 0.0F, 0.0F));
        REQUIRE(hit.entity == ball);
        // On the ellipsoid's surface, to within the sphere's facets.
        const float on = (hit.point.x * hit.point.x / 9.0F) + (hit.point.y * hit.point.y / 4.0F) +
                         (hit.point.z * hit.point.z);
        CHECK(on > 0.98F);
        CHECK(on < 1.01F);
        // The normal faces back along the pick.
        CHECK((hit.normal.x * x) + (hit.normal.y * y) + (hit.normal.z * z) > 0.0F);
    }
}

TEST_CASE("a pick follows whatever changes where an entity is hit", "[scene]") {
    const test_world_t world;
    const fbe_vec3_t from = at(1.5F, 5.0F, -5.0F);
    const fbe_vec3_t to = at(1.5F, -5.0F, -5.0F);
    // A ball of radius 1 a line past it misses, then grown, then boxed.
    const std::uint64_t ball = pivot(world.handle, 0.0F, 0.0F, -5.0F, FBE_PICK_ELLIPSOID);
    CHECK(pick(world.handle, from, to).entity == 0);
    fbe_entity_set_collision_radius(ball, 2.0F, 1.0F);
    CHECK(pick(world.handle, from, to).entity == ball);
    fbe_entity_set_pick_mode(ball, FBE_PICK_BOX);
    const fbe_aabb_t small{ .min = at(-1.0F, -1.0F, -1.0F), .max = at(1.0F, 1.0F, 1.0F) };
    fbe_entity_set_collision_box(ball, &small);
    CHECK(pick(world.handle, from, to).entity == 0);
    const fbe_aabb_t wide{ .min = at(-2.0F, -1.0F, -1.0F), .max = at(2.0F, 1.0F, 1.0F) };
    fbe_entity_set_collision_box(ball, &wide);
    CHECK(pick(world.handle, from, to).entity == ball);
    // Its parent moved, it moves.
    const std::uint64_t parent = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    fbe_entity_set_parent(ball, parent, true);
    fbe_entity_set_position(parent, 10.0F, 0.0F, 0.0F, FBE_SPACE_WORLD);
    CHECK(pick(world.handle, from, to).entity == 0);
    CHECK(pick(world.handle, at(11.5F, 5.0F, -5.0F), at(11.5F, -5.0F, -5.0F)).entity == ball);

    // A mesh whose geometry's vertices move is hit where they are.
    const std::uint64_t floor = mesh(world.handle, fbe_geometry_plane(1));
    fbe_entity_set_position(floor, -10.0F, 0.0F, 0.0F, FBE_SPACE_WORLD);
    const fbe_vec3_t above = at(-10.0F, 5.0F, 0.0F);
    const fbe_vec3_t below = at(-10.0F, -5.0F, 0.0F);
    CHECK(pick(world.handle, above, below).entity == floor);
    fbe_mat4_t aside{};
    aside.m[0] = 1.0F;
    aside.m[5] = 1.0F;
    aside.m[10] = 1.0F;
    aside.m[15] = 1.0F;
    aside.m[12] = 5.0F;
    const std::uint64_t geometry = fbe_mesh_geometry(floor);
    fbe_geometry_transform(geometry, &aside);
    CHECK(pick(world.handle, above, below).entity == 0);
    CHECK(pick(world.handle, at(-5.0F, 5.0F, 0.0F), at(-5.0F, -5.0F, 0.0F)).entity == floor);

    // Many made and most freed, the rest are still each hit where they are.
    std::vector<std::uint64_t> many;
    many.reserve(300);
    for (int i = 0; i < 300; ++i) {
        many.push_back(
            pivot(world.handle, static_cast<float>(i) * 3.0F, 50.0F, 0.0F, FBE_PICK_ELLIPSOID));
    }
    for (std::size_t i = 0; i < many.size(); ++i) {
        if (i % 10 != 0) {
            fbe_entity_free(many[i]);
        }
    }
    for (std::size_t i = 0; i < many.size(); ++i) {
        const float x = static_cast<float>(i) * 3.0F;
        const std::uint64_t hit = pick(world.handle, at(x, 60.0F, 0.0F), at(x, 40.0F, 0.0F)).entity;
        CHECK(hit == (i % 10 == 0 ? many[i] : 0));
    }
}
