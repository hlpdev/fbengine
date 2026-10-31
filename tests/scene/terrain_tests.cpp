// Terrains through the C API: heights, the surface at a point, collisions and picks against the
// grid, and changes in place.

#include "fbengine.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
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

/// A mesh of a terrain's own triangles, in its own space, as its squares make them.
std::uint64_t twin(std::uint64_t world, std::uint64_t terrain, std::int32_t size) {
    const std::uint64_t geometry = fbe_geometry_create();
    const std::int32_t surface = fbe_geometry_add_surface(geometry);
    std::vector<fbe_vertex_t> vertices;
    for (std::int32_t z = 0; z <= size; ++z) {
        for (std::int32_t x = 0; x <= size; ++x) {
            fbe_vertex_t vertex{};
            vertex.position =
                at(static_cast<float>(x), fbe_terrain_height(terrain, x, z), static_cast<float>(z));
            vertices.push_back(vertex);
        }
    }
    fbe_surface_add_vertices(geometry, surface, vertices.data(),
                             static_cast<std::int32_t>(vertices.size()));
    const auto corner = [&](std::int32_t x, std::int32_t z) {
        return static_cast<std::uint32_t>((z * (size + 1)) + x);
    };
    std::vector<std::uint32_t> indices;
    for (std::int32_t z = 0; z < size; ++z) {
        for (std::int32_t x = 0; x < size; ++x) {
            indices.insert(indices.end(), {
                                              corner(x, z),
                                              corner(x, z + 1),
                                              corner(x + 1, z + 1),
                                              corner(x, z),
                                              corner(x + 1, z + 1),
                                              corner(x + 1, z),
                                          });
        }
    }
    fbe_surface_add_triangles(geometry, surface, indices.data(),
                              static_cast<std::int32_t>(indices.size()));
    const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    fbe_mesh_set_geometry(mesh, geometry);
    fbe_geometry_release(geometry);
    return mesh;
}

/// The nearest hit from one point to another of a sphere of `radius`, or entity 0 for none.
fbe_pick_t pick(std::uint64_t world, fbe_vec3_t from, fbe_vec3_t to, float radius) {
    fbe_pick_t found{};
    if (fbe_world_pick(world, &from, &to, radius, 0, &found) != 1) {
        found = {};
    }
    return found;
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

TEST_CASE("a terrain's sphere picks find what a mesh of its triangles does", "[scene]") {
    // Spiky, turned, and scaled, as a level's is: picks slanting down, and long and low across
    // it, the sphere's first touch the same as the mesh's, however long the pick.
    const test_world_t world;
    const std::int32_t size = 16;
    const std::uint64_t terrain = fbe_entity_create(world.handle, 0, FBE_ENTITY_TERRAIN);
    REQUIRE(fbe_terrain_set_size(terrain, size));
    std::mt19937 random(7); // NOLINT(bugprone-random-generator-seed)
    std::uniform_real_distribution<float> unit(0.0F, 1.0F);
    for (std::int32_t z = 0; z <= size; ++z) {
        for (std::int32_t x = 0; x <= size; ++x) {
            const float spike = unit(random) < 0.08F ? 6.0F : 0.0F;
            fbe_terrain_set_height(terrain, x, z, unit(random) + spike);
        }
    }
    const std::uint64_t mesh = twin(world.handle, terrain, size);
    for (const std::uint64_t entity : { terrain, mesh }) {
        fbe_entity_set_rotation(entity, 0.0F, 0.247404F, 0.0F, 0.968912F, FBE_SPACE_WORLD);
        fbe_entity_set_scale(entity, 3.0F, 1.0F, 3.0F, FBE_SPACE_LOCAL);
        fbe_entity_set_position(entity, 3.0F, -1.0F, 5.0F, FBE_SPACE_WORLD);
    }
    const auto place = [&](float x, float y, float z) {
        fbe_vec3_t found{};
        fbe_entity_transform(terrain, 0, FBE_TRANSFORM_POINT, x, y, z, &found);
        return found;
    };
    const auto span = static_cast<float>(size);
    int differ = 0;
    for (int i = 0; i < 1200; ++i) {
        const bool low = i % 2 == 0;
        const float y = 1.0F + unit(random);
        const fbe_vec3_t from = low ? place(-0.5F, y, unit(random) * span)
                                    : place(unit(random) * span, 20.0F, unit(random) * span);
        const fbe_vec3_t to = low ? place(span + 0.5F, y, unit(random) * span)
                                  : place(unit(random) * span, -5.0F, unit(random) * span);
        fbe_entity_set_pick_mode(terrain, FBE_PICK_MESH);
        fbe_entity_set_pick_mode(mesh, FBE_PICK_NONE);
        const fbe_pick_t first = pick(world.handle, from, to, 0.5F);
        fbe_entity_set_pick_mode(terrain, FBE_PICK_NONE);
        fbe_entity_set_pick_mode(mesh, FBE_PICK_MESH);
        const fbe_pick_t second = pick(world.handle, from, to, 0.5F);
        const float length = std::hypot(to.x - from.x, to.y - from.y, to.z - from.z);
        if ((first.entity != 0) != (second.entity != 0) ||
            std::fabs(first.time - second.time) * length > 0.01F) {
            ++differ;
        }
    }
    CHECK(differ == 0);
}

TEST_CASE("a terrain's picks find it nearer than what's found first", "[scene]") {
    // A ball beyond a ridge, made first, so it's tried first: the ridge still stops the pick.
    const test_world_t world;
    const std::uint64_t ball = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    fbe_entity_set_position(ball, 12.0F, 1.0F, 4.0F, FBE_SPACE_WORLD);
    fbe_entity_set_pick_mode(ball, FBE_PICK_ELLIPSOID);
    const std::uint64_t terrain = fbe_entity_create(world.handle, 0, FBE_ENTITY_TERRAIN);
    REQUIRE(fbe_terrain_set_size(terrain, 8));
    for (std::int32_t z = 0; z <= 8; ++z) {
        fbe_terrain_set_height(terrain, 6, z, 3.0F);
    }
    fbe_entity_set_pick_mode(terrain, FBE_PICK_MESH);
    for (const float radius : { 0.0F, 0.3F }) {
        CHECK(pick(world.handle, at(0.5F, 1.0F, 4.0F), at(14.0F, 1.0F, 4.0F), radius).entity ==
              terrain);
    }
}

TEST_CASE("a line straight down on a terrain's far edges hits it", "[scene]") {
    const test_world_t world;
    const std::uint64_t terrain = slope(world.handle, 8);
    fbe_entity_set_pick_mode(terrain, FBE_PICK_MESH);
    for (const auto& [x, z] : {
             std::pair{ 8.0F, 3.5F },
             std::pair{ 2.5F, 8.0F },
             std::pair{ 8.0F, 8.0F },
             std::pair{ 0.0F, 0.0F },
         }) {
        const fbe_pick_t hit = pick(world.handle, at(x, 20.0F, z), at(x, -20.0F, z), 0.0F);
        CAPTURE(x, z);
        CHECK(hit.entity == terrain);
        CHECK(near(hit.point.y, x * 0.5F));
    }
}

TEST_CASE("a terrain's bounds keep up with its heights, without reading them all", "[scene]") {
    const test_world_t world;
    const std::uint64_t terrain = fbe_entity_create(world.handle, 0, FBE_ENTITY_TERRAIN);
    REQUIRE(fbe_terrain_set_size(terrain, 4));
    fbe_aabb_t box{};
    const auto heights = [&](float low, float high) {
        REQUIRE(fbe_entity_bounds(terrain, &box));
        return near(box.min.y, low) && near(box.max.y, high);
    };
    CHECK(heights(0.0F, 0.0F));
    // Raised, then the highest lowered, below another; the lowest raised, then lowered past.
    fbe_terrain_set_height(terrain, 1, 1, 5.0F);
    fbe_terrain_set_height(terrain, 2, 2, 3.0F);
    CHECK(heights(0.0F, 5.0F));
    fbe_terrain_set_height(terrain, 1, 1, 1.0F);
    CHECK(heights(0.0F, 3.0F));
    std::vector<float> all(25, 2.0F);
    all[7] = -4.0F;
    REQUIRE(fbe_terrain_set_heights(terrain, all.data(), 25));
    CHECK(heights(-4.0F, 2.0F));
    fbe_terrain_set_height(terrain, 2, 1, 2.0F);
    CHECK(heights(2.0F, 2.0F));
    fbe_terrain_set_height(terrain, 0, 0, -1.0F);
    CHECK(heights(-1.0F, 2.0F));
    REQUIRE(fbe_terrain_set_size(terrain, 2));
    CHECK(heights(0.0F, 0.0F));
    // A big one's bounds, asked for again and again, don't read its 4 million heights each time.
    REQUIRE(fbe_terrain_set_size(terrain, 2048));
    fbe_terrain_set_height(terrain, 100, 100, 7.0F);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        fbe_entity_bounds(terrain, &box);
    }
    const auto took = std::chrono::steady_clock::now() - start;
    CHECK(near(box.max.y, 7.0F));
    CHECK(took < std::chrono::milliseconds(500));
}

TEST_CASE("a terrain that blocks sight blocks it", "[scene]") {
    const test_world_t world;
    const std::uint64_t terrain = fbe_entity_create(world.handle, 0, FBE_ENTITY_TERRAIN);
    REQUIRE(fbe_terrain_set_size(terrain, 16));
    for (std::int32_t z = 0; z <= 16; ++z) {
        fbe_terrain_set_height(terrain, 8, z, 10.0F);
    }
    const std::uint64_t eye = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    fbe_entity_set_position(eye, 2.0F, 1.0F, 8.0F, FBE_SPACE_WORLD);
    CHECK(fbe_entity_can_see_point(eye, 14.0F, 1.0F, 8.0F) == 1);
    REQUIRE(fbe_entity_set_blocks_sight(terrain, true));
    CHECK(fbe_entity_can_see_point(eye, 14.0F, 1.0F, 8.0F) == 0);
    // Over the ridge, it sees.
    CHECK(fbe_entity_can_see_point(eye, 14.0F, 25.0F, 8.0F) == 1);
    // Scaled and far, a long line over a big one, through its ridge.
    REQUIRE(fbe_terrain_set_size(terrain, 1024));
    for (std::int32_t z = 0; z <= 1024; ++z) {
        fbe_terrain_set_height(terrain, 512, z, 10.0F);
    }
    fbe_entity_set_scale(terrain, 4.0F, 1.0F, 4.0F, FBE_SPACE_LOCAL);
    fbe_entity_set_position(eye, 10.0F, 1.0F, 20.0F, FBE_SPACE_WORLD);
    CHECK(fbe_entity_can_see_point(eye, 4000.0F, 1.0F, 3000.0F) == 0);
    CHECK(fbe_entity_can_see_point(eye, 1000.0F, 1.0F, 1000.0F) == 1);
}
