// Terrains through the C API: heights, the surface at a point, collisions and picks against the
// grid, and changes in place.

#include "fbengine.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

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
    return std::fabs(a - b) < 0.002F;
}

fbe_vec3_t position(std::uint64_t entity) {
    fbe_vec3_t found{};
    fbe_entity_position(entity, FBE_SPACE_WORLD, &found);
    return found;
}

/// A terrain of `size` squares, sloping up 0.5 a square along X, in the level.
std::uint64_t slope(std::uint64_t world, std::int32_t size) {
    const std::uint64_t terrain = fbe_entity_create(world, 0, FBE_ENTITY_TERRAIN);
    fbe_terrain_set_size(terrain, size);
    for (std::int32_t z = 0; z <= size; ++z) {
        for (std::int32_t x = 0; x <= size; ++x) {
            fbe_terrain_set_height(terrain, x, z, static_cast<float>(x) * 0.5F);
        }
    }
    fbe_entity_set_collision_group(terrain, level_group);
    return terrain;
}

} // namespace

TEST_CASE("a terrain's heights and surface are its grid's", "[scene]") {
    const test_world_t world;
    const std::uint64_t terrain = fbe_entity_create(world.handle, 0, FBE_ENTITY_TERRAIN);
    CHECK(fbe_entity_kind(terrain) == FBE_ENTITY_TERRAIN);
    CHECK(fbe_terrain_size(terrain) == 1);
    REQUIRE(fbe_terrain_set_size(terrain, 4));
    CHECK(fbe_terrain_height(terrain, 4, 4) == 0.0F);
    CHECK(std::isnan(fbe_terrain_height(terrain, 5, 0)));
    // A bump at (2, 2): halfway to it along X, the surface is halfway up.
    REQUIRE(fbe_terrain_set_height(terrain, 2, 2, 2.0F));
    fbe_vec3_t point{};
    fbe_vec3_t normal{};
    REQUIRE(fbe_terrain_surface(terrain, 2.0F, 10.0F, 2.0F, &point, &normal) == 1);
    CHECK(near(point.y, 2.0F));
    REQUIRE(fbe_terrain_surface(terrain, 1.5F, -3.0F, 1.5F, &point, &normal) == 1);
    CHECK(near(point.y, 1.0F));
    CHECK(normal.y > 0.0F);
    CHECK(fbe_terrain_surface(terrain, -1.0F, 0.0F, 1.0F, &point, &normal) == 0);
    // Moved and scaled, it's where its entity is.
    fbe_entity_set_position(terrain, 10.0F, 1.0F, 10.0F, FBE_SPACE_WORLD);
    fbe_entity_set_scale(terrain, 2.0F, 3.0F, 2.0F, FBE_SPACE_LOCAL);
    REQUIRE(fbe_terrain_surface(terrain, 14.0F, 0.0F, 14.0F, &point, &normal) == 1);
    CHECK(near(point.y, 7.0F));
    // All at once, row by row; the wrong count, or a height that isn't finite, is refused.
    std::vector<float> heights(25, 1.0F);
    REQUIRE(fbe_terrain_set_heights(terrain, heights.data(), 25));
    CHECK(fbe_terrain_height(terrain, 2, 2) == 1.0F);
    CHECK_FALSE(fbe_terrain_set_heights(terrain, heights.data(), 24));
    heights[3] = NAN;
    CHECK_FALSE(fbe_terrain_set_heights(terrain, heights.data(), 25));
    CHECK_FALSE(fbe_terrain_set_size(terrain, 0));
    CHECK_FALSE(fbe_terrain_set_size(terrain, 4097));
    // A copy has its own heights.
    const std::uint64_t copy = fbe_entity_copy(terrain, world.handle, 0);
    fbe_terrain_set_height(copy, 0, 0, 9.0F);
    CHECK(fbe_terrain_height(terrain, 0, 0) == 1.0F);
    CHECK(fbe_terrain_size(copy) == 4);
}

TEST_CASE("movers walk on a terrain and picks hit it", "[scene]") {
    const test_world_t world;
    const std::uint64_t terrain = slope(world.handle, 64);
    // A mover falls onto the slope, and walking up it, stays on it.
    const std::uint64_t made = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    fbe_entity_set_collision_group(made, mover_group);
    fbe_entity_set_collision_radius(made, 0.5F, 0.5F);
    fbe_entity_set_position(made, 10.0F, 20.0F, 10.0F, FBE_SPACE_WORLD);
    fbe_world_collide(world.handle);
    fbe_entity_translate(made, 0.0F, -30.0F, 0.0F, FBE_SPACE_WORLD);
    fbe_world_collide(world.handle);
    fbe_vec3_t point{};
    fbe_vec3_t normal{};
    const fbe_vec3_t fallen = position(made);
    REQUIRE(fbe_terrain_surface(terrain, fallen.x, fallen.y, fallen.z, &point, &normal) == 1);
    // On a slope of 0.5, its middle is a radius out along the normal from the surface.
    CHECK(fallen.y > point.y + 0.5F);
    CHECK(fallen.y < point.y + 0.6F);
    for (int frame = 0; frame < 20; ++frame) {
        fbe_entity_translate(made, 0.2F, -0.05F, 0.0F, FBE_SPACE_WORLD);
        fbe_world_collide(world.handle);
    }
    const fbe_vec3_t walked = position(made);
    CHECK(walked.x > fallen.x + 2.0F);
    REQUIRE(fbe_terrain_surface(terrain, walked.x, walked.y, walked.z, &point, &normal) == 1);
    CHECK(walked.y > point.y + 0.5F);
    // Picks hit it from above, even one far across it, and not from below.
    fbe_entity_set_pick_mode(terrain, FBE_PICK_MESH);
    fbe_pick_t hit{};
    const fbe_vec3_t from = at(30.0F, 100.0F, 30.0F);
    const fbe_vec3_t to = at(30.0F, -100.0F, 30.0F);
    REQUIRE(fbe_world_pick(world.handle, &from, &to, 0.0F, 0, &hit) == 1);
    CHECK(hit.entity == terrain);
    CHECK(near(hit.point.y, 15.0F));
    CHECK(fbe_world_pick(world.handle, &to, &from, 0.0F, 0, &hit) == 0);
    const fbe_vec3_t start = at(-10.0F, 40.0F, 5.0F);
    const fbe_vec3_t end = at(100.0F, 5.0F, 50.0F);
    REQUIRE(fbe_world_pick(world.handle, &start, &end, 0.0F, 0, &hit) == 1);
    CHECK(hit.entity == terrain);
    REQUIRE(fbe_terrain_surface(terrain, hit.point.x, hit.point.y, hit.point.z, &point, &normal) ==
            1);
    CHECK(near(hit.point.y, point.y));
    // Changed in place, the pick follows.
    fbe_terrain_set_height(terrain, 30, 30, 40.0F);
    REQUIRE(fbe_world_pick(world.handle, &from, &to, 0.0F, 0, &hit) == 1);
    CHECK(near(hit.point.y, 40.0F));
    // Its bounds are its grid's.
    fbe_aabb_t bounds{};
    REQUIRE(fbe_entity_bounds(terrain, &bounds));
    CHECK(near(bounds.max.x, 64.0F));
    CHECK(near(bounds.max.y, 40.0F));
}
