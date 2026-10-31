// Collisions through the C API, where they're subtle: corners, edges a mover rides over, moves too
// fast for their frame, movers touching each other, and a long walk over bumpy ground that must
// never sink through it.

#include "fbengine.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
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

fbe_vec3_t position(std::uint64_t entity) {
    fbe_vec3_t found{};
    fbe_entity_position(entity, FBE_SPACE_WORLD, &found);
    return found;
}

std::uint64_t mover(std::uint64_t world, float x, float y, float z, float radius) {
    const std::uint64_t made = fbe_entity_create(world, 0, FBE_ENTITY_PIVOT);
    fbe_entity_set_collision_group(made, mover_group);
    fbe_entity_set_collision_radius(made, radius, radius);
    fbe_entity_set_position(made, x, y, z, FBE_SPACE_WORLD);
    fbe_world_collide(world);
    return made;
}

void move(std::uint64_t world, std::uint64_t entity, float x, float y, float z) {
    fbe_entity_translate(entity, x, y, z, FBE_SPACE_WORLD);
    fbe_world_collide(world);
}

/// A level mesh of a geometry, which it lets go of.
std::uint64_t level(std::uint64_t world, std::uint64_t geometry) {
    const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    fbe_mesh_set_geometry(mesh, geometry);
    fbe_geometry_release(geometry);
    fbe_entity_set_collision_group(mesh, level_group);
    return mesh;
}

/// A wall facing +X at x = `x`, or facing -Z at z = `z`, 20 across and high.
std::uint64_t wall_x(std::uint64_t world, float x) {
    const std::uint64_t wall = level(world, fbe_geometry_plane(1));
    const float half = 0.70710678F;
    // The plane faces up; a quarter turn about -Z faces it +X.
    fbe_entity_set_rotation(wall, 0.0F, 0.0F, -half, half, FBE_SPACE_WORLD);
    fbe_entity_set_scale(wall, 10.0F, 1.0F, 10.0F, FBE_SPACE_LOCAL);
    fbe_entity_set_position(wall, x, 0.0F, 0.0F, FBE_SPACE_WORLD);
    return wall;
}

std::uint64_t wall_z(std::uint64_t world, float z) {
    const std::uint64_t wall = level(world, fbe_geometry_plane(1));
    const float half = 0.70710678F;
    // A quarter turn about -X faces it -Z.
    fbe_entity_set_rotation(wall, -half, 0.0F, 0.0F, half, FBE_SPACE_WORLD);
    fbe_entity_set_scale(wall, 10.0F, 1.0F, 10.0F, FBE_SPACE_LOCAL);
    fbe_entity_set_position(wall, 0.0F, 0.0F, z, FBE_SPACE_WORLD);
    return wall;
}

} // namespace

TEST_CASE("a mover pushed into a corner stays in it", "[scene]") {
    const test_world_t world;
    wall_x(world.handle, -2.0F);
    wall_z(world.handle, 2.0F);
    const std::uint64_t made = mover(world.handle, 0.0F, 0.0F, 0.0F, 0.5F);
    for (int frame = 0; frame < 20; ++frame) {
        move(world.handle, made, -0.7F, 0.0F, 0.6F);
    }
    const fbe_vec3_t at = position(made);
    CHECK(std::fabs(at.x - -1.499F) < 0.002F);
    CHECK(std::fabs(at.z - 1.499F) < 0.002F);
    std::array<fbe_collision_t, 8> touched{};
    CHECK(fbe_entity_collisions(made, touched.data(), 8) >= 1);
}

TEST_CASE("a move too fast for its frame still stops at a thin wall", "[scene]") {
    const test_world_t world;
    wall_x(world.handle, -2.0F);
    const std::uint64_t made = mover(world.handle, 5.0F, 0.0F, 0.0F, 0.1F);
    move(world.handle, made, -1000.0F, 0.0F, 0.0F);
    CHECK(std::fabs(position(made).x - -1.899F) < 0.002F);
}

TEST_CASE("a mover rides over an edge lower than its radius", "[scene]") {
    const test_world_t world;
    level(world.handle, fbe_geometry_plane(1));
    fbe_entity_set_scale(fbe_world_find(world.handle, ""), 20.0F, 1.0F, 20.0F, FBE_SPACE_LOCAL);
    // A ledge 0.1 high from x = 1 on.
    const std::uint64_t ledge = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    fbe_entity_set_collision_group(ledge, level_group);
    fbe_entity_set_collision_shape(ledge, FBE_COLLISION_BOX);
    const fbe_aabb_t box{
        .min = { .x = 1.0F, .y = 0.0F, .z = -5.0F },
        .max = { .x = 9.0F, .y = 0.1F, .z = 5.0F },
    };
    fbe_entity_set_collision_box(ledge, &box);
    const std::uint64_t made = mover(world.handle, 0.0F, 0.301F, 0.0F, 0.3F);
    for (int frame = 0; frame < 40; ++frame) {
        move(world.handle, made, 0.1F, -0.05F, 0.0F);
    }
    const fbe_vec3_t at = position(made);
    CHECK(at.x > 3.0F);
    CHECK(std::fabs(at.y - 0.401F) < 0.003F);
}

TEST_CASE("movers touch each other where their last moves ended", "[scene]") {
    const test_world_t world;
    fbe_world_set_collision(world.handle, mover_group, mover_group, FBE_RESPONSE_STOP);
    const std::uint64_t first = mover(world.handle, 0.0F, 0.0F, 0.0F, 0.5F);
    const std::uint64_t second = mover(world.handle, 3.0F, 0.0F, 0.0F, 0.5F);
    // Both move toward each other at once: each meets the other where it was, so neither
    // passes through, whatever their order.
    fbe_entity_translate(first, 2.5F, 0.0F, 0.0F, FBE_SPACE_WORLD);
    fbe_entity_translate(second, -2.5F, 0.0F, 0.0F, FBE_SPACE_WORLD);
    fbe_world_collide(world.handle);
    CHECK(std::fabs(position(first).x - 1.999F) < 0.002F);
    CHECK(std::fabs(position(second).x - 1.001F) < 0.002F);
    std::array<fbe_collision_t, 4> touched{};
    // Each touched the other as a mover, and was touched by it.
    CHECK(fbe_entity_collisions(first, touched.data(), 4) == 2);
}

TEST_CASE("a long walk over bumpy ground never sinks through it", "[scene]") {
    const test_world_t world;
    // Ground of 3,200 triangles, its heights rough.
    const std::uint64_t geometry = fbe_geometry_create();
    fbe_geometry_add_surface(geometry);
    std::mt19937 random(3); // NOLINT(bugprone-random-generator-seed)
    std::uniform_real_distribution<float> bump(0.0F, 0.3F);
    std::vector<fbe_vertex_t> vertices;
    for (int z = 0; z <= 40; ++z) {
        for (int x = 0; x <= 40; ++x) {
            fbe_vertex_t vertex{};
            const float height = bump(random);
            const float across = static_cast<float>(x) - 20.0F;
            const float along = static_cast<float>(z) - 20.0F;
            vertex.position = { .x = across, .y = height, .z = along };
            vertices.push_back(vertex);
        }
    }
    std::vector<std::uint32_t> indices;
    for (std::uint32_t z = 0; z < 40; ++z) {
        for (std::uint32_t x = 0; x < 40; ++x) {
            const std::uint32_t at = (z * 41) + x;
            indices.insert(indices.end(), { at, at + 41, at + 42, at, at + 42, at + 1 });
        }
    }
    fbe_surface_add_vertices(geometry, 0, vertices.data(),
                             static_cast<std::int32_t>(vertices.size()));
    fbe_surface_add_triangles(geometry, 0, indices.data(),
                              static_cast<std::int32_t>(indices.size()));
    level(world.handle, geometry);
    const std::uint64_t made = mover(world.handle, 0.0F, 1.0F, 0.0F, 0.2F);
    std::uniform_real_distribution<float> step(-0.15F, 0.15F);
    float lowest = 1.0F;
    for (int frame = 0; frame < 3000; ++frame) {
        move(world.handle, made, step(random), -0.1F, step(random));
        const fbe_vec3_t at = position(made);
        if (std::fabs(at.x) < 19.0F && std::fabs(at.z) < 19.0F) {
            lowest = std::fmin(lowest, at.y);
        }
    }
    // The ground is nowhere below 0, so a mover of radius 0.2 never gets below 0.2.
    CHECK(lowest > 0.199F);
}
