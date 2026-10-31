// Collisions through the C API, where they're subtle: corners, edges a mover rides over, moves too
// fast for their frame, movers touching each other, and a long walk over bumpy ground that must
// never sink through it.

#include "fbengine.h"
#include "scene/math.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <utility>
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
    // Riding up the edge is a rise, which a step height allows.
    fbe_entity_set_step_height(made, 0.15F);
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

namespace {

/// A box from `min` to `max` of the level, in its own space.
std::uint64_t box(std::uint64_t world, fbe_aabb_t extent) {
    const std::uint64_t made = fbe_entity_create(world, 0, FBE_ENTITY_PIVOT);
    fbe_entity_set_collision_group(made, level_group);
    fbe_entity_set_collision_shape(made, FBE_COLLISION_BOX);
    fbe_entity_set_collision_box(made, &extent);
    return made;
}

fbe_aabb_t extent(float x0, float y0, float z0, float x1, float y1, float z1) {
    return { .min = { .x = x0, .y = y0, .z = z0 }, .max = { .x = x1, .y = y1, .z = z1 } };
}

fbe::scene::vec3_t vec(fbe_vec3_t v) {
    return { v.x, v.y, v.z };
}

/// The point of a triangle nearest `p`, Ericson's way.
fbe::scene::vec3_t nearest_on(fbe::scene::vec3_t p, fbe::scene::vec3_t a, fbe::scene::vec3_t b,
                              fbe::scene::vec3_t c) {
    using fbe::scene::dot;
    const fbe::scene::vec3_t ab = b - a;
    const fbe::scene::vec3_t ac = c - a;
    const fbe::scene::vec3_t ap = p - a;
    const float d1 = dot(ab, ap);
    const float d2 = dot(ac, ap);
    if (d1 <= 0.0F && d2 <= 0.0F) {
        return a;
    }
    const fbe::scene::vec3_t bp = p - b;
    const float d3 = dot(ab, bp);
    const float d4 = dot(ac, bp);
    if (d3 >= 0.0F && d4 <= d3) {
        return b;
    }
    const float vc = (d1 * d4) - (d3 * d2);
    if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
        return a + ab * (d1 / (d1 - d3));
    }
    const fbe::scene::vec3_t cp = p - c;
    const float d5 = dot(ab, cp);
    const float d6 = dot(ac, cp);
    if (d6 >= 0.0F && d5 <= d6) {
        return c;
    }
    const float vb = (d5 * d2) - (d1 * d6);
    if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
        return a + ac * (d2 / (d2 - d6));
    }
    const float va = (d3 * d6) - (d5 * d4);
    if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F) {
        const fbe::scene::vec3_t bc = c - b;
        const float along = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + bc * along;
    }
    const float denominator = 1.0F / (va + vb + vc);
    return a + ab * (vb * denominator) + ac * (vc * denominator);
}

/// How far an ellipsoid's middle is from a box turned, scaled, and moved by `matrix`, in the
/// ellipsoid's own space, where it's a sphere of its horizontal radius: 0 when it's inside.
float apart(const fbe_mat4_t& matrix, fbe_aabb_t extent, fbe::scene::vec3_t middle, float squash) {
    fbe::scene::mat4_t m;
    std::ranges::copy(matrix.m, m.m.begin());
    std::array<fbe::scene::vec3_t, 8> corners;
    for (unsigned corner = 0; corner < 8; ++corner) {
        const fbe::scene::vec3_t local((corner & 1U) != 0 ? extent.max.x : extent.min.x,
                                       (corner & 2U) != 0 ? extent.max.y : extent.min.y,
                                       (corner & 4U) != 0 ? extent.max.z : extent.min.z);
        fbe::scene::vec3_t at = fbe::scene::transform_point(m, local);
        at.y *= squash;
        corners[corner] = at;
    }
    const fbe::scene::vec3_t p(middle.x, middle.y * squash, middle.z);
    // Inside a parallelepiped is inside all its faces' slabs.
    const fbe::scene::vec3_t origin = corners[0];
    const std::array<fbe::scene::vec3_t, 3> axes{
        corners[1] - origin,
        corners[2] - origin,
        corners[4] - origin,
    };
    bool inside = true;
    for (std::size_t k = 0; k < 3; ++k) {
        const fbe::scene::vec3_t across = fbe::scene::cross(axes[(k + 1) % 3], axes[(k + 2) % 3]);
        const float where = fbe::scene::dot(p - origin, across);
        const float far = fbe::scene::dot(axes[k], across);
        inside = inside && (far > 0.0F ? where > 0.0F && where < far : where < 0.0F && where > far);
    }
    if (inside) {
        return 0.0F;
    }
    constexpr std::array<std::array<unsigned, 3>, 12> faces{
        {
            { 0, 4, 6 },
            { 0, 6, 2 },
            { 1, 3, 7 },
            { 1, 7, 5 },
            { 0, 1, 5 },
            { 0, 5, 4 },
            { 2, 6, 7 },
            { 2, 7, 3 },
            { 0, 2, 3 },
            { 0, 3, 1 },
            { 4, 5, 7 },
            { 4, 7, 6 },
        },
    };
    float least = INFINITY;
    for (const auto& face : faces) {
        const fbe::scene::vec3_t q =
            nearest_on(p, corners[face[0]], corners[face[1]], corners[face[2]]);
        least = std::fmin(least, fbe::scene::length(p - q));
    }
    return least;
}

} // namespace

TEST_CASE("a mover walking among turned boxes and cubes never gets into them", "[scene]") {
    for (const unsigned seed : { 1U, 2U, 3U }) {
        const test_world_t world;
        std::mt19937 random(seed); // NOLINT(bugprone-random-generator-seed)
        std::uniform_real_distribution<float> place(-6.0F, 6.0F);
        std::uniform_real_distribution<float> size(0.3F, 1.5F);
        std::uniform_real_distribution<float> angle(-std::numbers::pi_v<float>,
                                                    std::numbers::pi_v<float>);
        // A floor, then solids, half boxes and half cube meshes, each turned and scaled.
        box(world.handle, extent(-10.0F, -1.0F, -10.0F, 10.0F, 0.0F, 10.0F));
        std::vector<std::pair<std::uint64_t, fbe_aabb_t>> solids;
        for (int i = 0; i < 40; ++i) {
            const bool mesh = i % 2 == 1;
            const std::uint64_t solid =
                mesh ? level(world.handle, fbe_geometry_cube())
                     : box(world.handle, extent(-1.0F, -1.0F, -1.0F, 1.0F, 1.0F, 1.0F));
            const float half = angle(random) / 2.0F;
            fbe_entity_set_rotation(solid, 0.0F, std::sin(half), 0.0F, std::cos(half),
                                    FBE_SPACE_LOCAL);
            const float sx = size(random);
            const float sy = size(random);
            const float sz = size(random);
            fbe_entity_set_scale(solid, sx, sy, sz, FBE_SPACE_LOCAL);
            fbe_entity_set_position(solid, place(random), sy, place(random), FBE_SPACE_LOCAL);
            solids.emplace_back(solid, extent(-1.0F, -1.0F, -1.0F, 1.0F, 1.0F, 1.0F));
        }
        const float horizontal = 0.15F;
        const float vertical = 0.3F;
        const std::uint64_t made = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
        fbe_entity_set_collision_group(made, mover_group);
        fbe_entity_set_collision_radius(made, horizontal, vertical);
        fbe_entity_set_step_height(made, 0.1F);
        fbe_entity_set_position(made, 0.0F, 5.0F, 0.0F, FBE_SPACE_WORLD);
        fbe_world_collide(world.handle);
        std::uniform_real_distribution<float> step(-0.12F, 0.12F);
        float closest = INFINITY;
        for (int frame = 0; frame < 1500; ++frame) {
            move(world.handle, made, step(random), -0.08F, step(random));
            const fbe::scene::vec3_t at = vec(position(made));
            if (std::fabs(at.x) > 9.0F || std::fabs(at.z) > 9.0F) {
                fbe_entity_set_position(made, 0.0F, 5.0F, 0.0F, FBE_SPACE_WORLD);
                fbe_entity_reset_collisions(made);
                fbe_world_collide(world.handle);
                continue;
            }
            for (const auto& [solid, solid_extent] : solids) {
                fbe_mat4_t matrix{};
                fbe_entity_matrix(solid, FBE_SPACE_WORLD, &matrix);
                closest =
                    std::fmin(closest, apart(matrix, solid_extent, at, horizontal / vertical));
            }
        }
        // Never nearer than its radius less the skin, and some rounding.
        CHECK(closest > horizontal - 0.001F - 0.0005F);
    }
}

TEST_CASE("a mover starting over an edge or a corner doesn't go into it", "[scene]") {
    // A box filling x < 0 and z < 0, and a mover of radius 1 over its upright edge, moving in.
    const test_world_t edged;
    box(edged.handle, extent(-5.0F, -50.0F, -5.0F, 0.0F, 50.0F, 0.0F));
    const std::uint64_t first = mover(edged.handle, 0.5F, 0.0F, 0.5F, 1.0F);
    // How far its middle is from the box, across.
    const auto from_box = [first] {
        const fbe_vec3_t at = position(first);
        const float x = std::fmax(std::fmax(-5.0F - at.x, at.x), 0.0F);
        const float z = std::fmax(std::fmax(-5.0F - at.z, at.z), 0.0F);
        return std::sqrt((x * x) + (z * z));
    };
    // Pushed out of the edge, then never nearer it than its radius less the skin, going on round.
    move(edged.handle, first, -3.0F, 0.0F, -2.0F);
    CHECK(from_box() > 1.0F - 0.0011F);
    for (int i = 0; i < 10; ++i) {
        move(edged.handle, first, -1.0F, 0.0F, 0.0F);
        CHECK(from_box() > 1.0F - 0.0011F);
    }

    // A cube from -2 to 2, and a mover of radius 1 a little off its corner, moving in.
    const test_world_t cornered;
    const std::uint64_t cube = level(cornered.handle, fbe_geometry_cube());
    fbe_entity_set_scale(cube, 2.0F, 2.0F, 2.0F, FBE_SPACE_LOCAL);
    const std::uint64_t second = mover(cornered.handle, 2.4F, 2.4F, 2.4F, 1.0F);
    move(cornered.handle, second, -2.0F, -2.5F, -1.5F);
    const fbe_vec3_t then = position(second);
    CHECK_FALSE((std::fabs(then.x) < 2.0F && std::fabs(then.y) < 2.0F && std::fabs(then.z) < 2.0F));
}

TEST_CASE("a step up is no higher than the step height", "[scene]") {
    for (const float high : { 0.2F, 0.4F }) {
        const test_world_t world;
        box(world.handle, extent(-10.0F, -1.0F, -10.0F, 10.0F, 0.0F, 10.0F));
        box(world.handle, extent(1.0F, 0.0F, -5.0F, 5.0F, high, 5.0F));
        const std::uint64_t made = mover(world.handle, 0.74F, 0.251F, 0.0F, 0.25F);
        fbe_entity_set_step_height(made, 0.3F);
        move(world.handle, made, 1.0F, -0.01F, 0.0F);
        const fbe_vec3_t at = position(made);
        if (high < 0.3F) {
            CHECK(std::fabs(at.y - (high + 0.251F)) < 0.003F);
            CHECK(at.x > 1.5F);
        } else {
            // Too high: it stays on the floor at the ledge.
            CHECK(std::fabs(at.y - 0.251F) < 0.003F);
            CHECK(at.x < 0.751F);
        }
    }
}

TEST_CASE("a slide never throws a tall mover up over a low edge", "[scene]") {
    for (const std::int32_t response : { FBE_RESPONSE_SLIDE, FBE_RESPONSE_SLIDE_HORIZONTAL }) {
        for (const float step : { 0.0F, 0.25F }) {
            const test_world_t world;
            fbe_world_set_collision(world.handle, mover_group, level_group, response);
            box(world.handle, extent(-10.0F, -1.0F, -10.0F, 10.0F, 0.0F, 10.0F));
            box(world.handle, extent(1.0F, 0.0F, -5.0F, 5.0F, 0.2F, 5.0F));
            const std::uint64_t made = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
            fbe_entity_set_collision_group(made, mover_group);
            fbe_entity_set_collision_radius(made, 0.3F, 0.9F);
            fbe_entity_set_step_height(made, step);
            fbe_entity_set_position(made, 0.69F, 0.901F, 0.0F, FBE_SPACE_WORLD);
            fbe_world_collide(world.handle);
            // One move into the edge rises no more than the step height.
            move(world.handle, made, 1.0F, -0.01F, 0.0F);
            CHECK(position(made).y <= 0.901F + step + 0.002F);
            // Walking on, falling where it isn't held up, it never rises above resting on top.
            float fall = 0.0F;
            float highest = 0.0F;
            for (int frame = 0; frame < 60; ++frame) {
                fall = std::min(fall + 0.01F, 0.5F);
                move(world.handle, made, 0.1F, -fall, 0.0F);
                std::array<fbe_collision_t, 16> touched{};
                const std::int32_t count = fbe_entity_collisions(made, touched.data(), 16);
                for (std::int32_t i = 0; i < std::min(count, 16); ++i) {
                    if (touched[static_cast<std::size_t>(i)].normal.y > 0.7F) {
                        fall = 0.0F;
                    }
                }
                highest = std::max(highest, position(made).y);
            }
            CHECK(highest <= (step > 0.0F ? 1.101F : 0.901F) + 0.003F);
        }
    }
}

TEST_CASE("a mover squeezed between closing walls stays between them", "[scene]") {
    const test_world_t world;
    // Walls facing each other across x, closing a little each frame.
    const std::uint64_t left = box(world.handle, extent(-5.0F, -5.0F, -5.0F, 0.0F, 5.0F, 5.0F));
    const std::uint64_t right = box(world.handle, extent(0.0F, -5.0F, -5.0F, 5.0F, 5.0F, 5.0F));
    const std::uint64_t made = mover(world.handle, 0.0F, 0.0F, 0.0F, 0.5F);
    for (int frame = 0; frame <= 40; ++frame) {
        const float gap = 1.2F - (static_cast<float>(frame) * 0.025F);
        fbe_entity_set_position(left, -gap / 2.0F, 0.0F, 0.0F, FBE_SPACE_WORLD);
        fbe_entity_set_position(right, gap / 2.0F, 0.0F, 0.0F, FBE_SPACE_WORLD);
        move(world.handle, made, 0.02F, 0.0F, 0.05F);
        const fbe_vec3_t at = position(made);
        CHECK(at.x > -gap / 2.0F);
        CHECK(at.x < gap / 2.0F);
    }
}

TEST_CASE("a mover squeezed by one closing wall against a still one stays between them",
          "[scene]") {
    // Walls 1 thick in x as boxes or cube meshes, the still one's inner face at -1, the other's
    // closing from 1 to -0.7, a gap of 0.3, below the mover's radius of 0.4.
    struct case_t {
        bool meshes;
        float walk;
        float vertical;
    };
    for (const case_t& c : {
             case_t{ .meshes = false, .walk = 0.02F, .vertical = 0.0F },
             case_t{ .meshes = false, .walk = -0.02F, .vertical = 0.0F },
             case_t{ .meshes = true, .walk = 0.02F, .vertical = 0.0F },
             case_t{ .meshes = true, .walk = 0.02F, .vertical = 0.8F },
         }) {
        const test_world_t world;
        const auto wall = [&] {
            if (!c.meshes) {
                return box(world.handle, extent(-0.5F, -5.0F, -5.0F, 0.5F, 5.0F, 5.0F));
            }
            const std::uint64_t made = level(world.handle, fbe_geometry_cube());
            fbe_entity_set_scale(made, 0.5F, 5.0F, 5.0F, FBE_SPACE_LOCAL);
            return made;
        };
        const bool falls = c.vertical > 0.0F;
        if (falls) {
            const std::uint64_t floor = level(world.handle, fbe_geometry_plane(4));
            fbe_entity_set_scale(floor, 20.0F, 1.0F, 20.0F, FBE_SPACE_LOCAL);
        }
        const std::uint64_t still = wall();
        const std::uint64_t door = wall();
        fbe_entity_set_position(still, -1.5F, 0.0F, 0.0F, FBE_SPACE_WORLD);
        const std::uint64_t made = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
        fbe_entity_set_collision_group(made, mover_group);
        fbe_entity_set_collision_radius(made, 0.4F, falls ? c.vertical : 0.4F);
        fbe_entity_set_step_height(made, falls ? 0.1F : 0.0F);
        fbe_entity_set_position(made, 0.0F, falls ? c.vertical + 0.01F : 0.0F, 0.0F,
                                FBE_SPACE_WORLD);
        float face = 1.0F;
        fbe_entity_set_position(door, face + 0.5F, 0.0F, 0.0F, FBE_SPACE_WORLD);
        fbe_world_collide(world.handle);
        for (int frame = 0; frame < 80; ++frame) {
            face = std::fmax(face - 0.05F, -0.7F);
            fbe_entity_set_position(door, face + 0.5F, 0.0F, 0.0F, FBE_SPACE_WORLD);
            move(world.handle, made, c.walk, falls ? -0.05F : 0.0F, 0.0F);
            const fbe_vec3_t at = position(made);
            CHECK(at.x > -1.0F);
            CHECK(at.x < face);
            CHECK(at.y >= (falls ? 0.0F : -0.001F));
        }
        // Squeezed below its diameter, it's in the middle of the gap.
        CHECK(std::fabs(position(made).x - -0.85F) < 0.002F);
    }
}

TEST_CASE("a mover over a refitted mesh ends where one over a fresh copy of it does", "[scene]") {
    std::mt19937 random(4); // NOLINT(bugprone-random-generator-seed)
    std::uniform_real_distribution<float> nudge(-0.3F, 0.3F);
    std::uniform_real_distribution<float> place(-6.0F, 6.0F);
    const test_world_t refitted;
    // A sphere and a copy of it a little aside, overlapping, in two surfaces.
    const std::uint64_t geometry = fbe_geometry_sphere(24);
    fbe_geometry_add_surface(geometry);
    const fbe_mat4_t aside{ .m = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5F, 0.5F, 0, 1 } };
    fbe_geometry_add(geometry, geometry, &aside);
    const std::uint64_t mesh = fbe_entity_create(refitted.handle, 0, FBE_ENTITY_MESH);
    fbe_mesh_set_geometry(mesh, geometry);
    fbe_entity_set_collision_group(mesh, level_group);
    fbe_entity_set_scale(mesh, 3.0F, 3.0F, 3.0F, FBE_SPACE_LOCAL);
    const std::int32_t surfaces = fbe_geometry_surface_count(geometry);
    for (int round = 0; round < 40; ++round) {
        // Every vertex moved, flipped now and then, or transformed: refits, not builds.
        for (std::int32_t surface = 0; surface < surfaces; ++surface) {
            const std::int32_t count = fbe_surface_vertex_count(geometry, surface);
            std::vector<fbe_vertex_t> vertices(static_cast<std::size_t>(count));
            fbe_surface_vertices(geometry, surface, 0, vertices.data(), count);
            for (fbe_vertex_t& vertex : vertices) {
                vertex.position.x += nudge(random);
                vertex.position.y += nudge(random);
                vertex.position.z += nudge(random);
            }
            fbe_surface_set_vertices(geometry, surface, 0, vertices.data(), count);
        }
        if (round % 7 == 3) {
            fbe_geometry_flip(geometry);
        }
        if (round % 5 == 2) {
            const fbe_mat4_t skew{
                .m = { 1.1F, 0.2F, 0, 0, 0, 0.9F, 0, 0, 0.1F, 0, 1.05F, 0, 0.2F, 0, -0.1F, 1 },
            };
            fbe_geometry_transform(geometry, &skew);
        }
        // The same mesh on a fresh copy, whose tree is built, in a World of its own.
        const test_world_t fresh;
        const std::uint64_t other = level(fresh.handle, fbe_geometry_clone(geometry));
        fbe_entity_set_scale(other, 3.0F, 3.0F, 3.0F, FBE_SPACE_LOCAL);
        const float x = place(random);
        const float z = place(random);
        const std::uint64_t one = mover(refitted.handle, x, 8.0F, z, 0.3F);
        const std::uint64_t two = mover(fresh.handle, x, 8.0F, z, 0.3F);
        for (int i = 0; i < 30; ++i) {
            const float dx = nudge(random);
            const float dz = nudge(random);
            move(refitted.handle, one, dx, -0.4F, dz);
            move(fresh.handle, two, dx, -0.4F, dz);
            const fbe_vec3_t a = position(one);
            const fbe_vec3_t b = position(two);
            REQUIRE((a.x == b.x && a.y == b.y && a.z == b.z));
        }
        fbe_entity_free(one);
    }
    fbe_geometry_release(geometry);
}

TEST_CASE("a long walk among turned solids never gets 0.005 into them", "[scene]") {
    // The review's long walk at the worst two of 40 seeds of 20,000 frames, just past their worst
    // frames: 40 solids, boxes and cube meshes, turned every way, and a 0.3 by 0.6 mover falling
    // and walking among them. Its worst, 0.0045, is between two solids nearly facing each other.
    for (const auto [seed, frames] : { std::pair{ 6U, 1400 }, std::pair{ 34U, 4900 } }) {
        std::mt19937 random(seed); // NOLINT(bugprone-random-generator-seed)
        const test_world_t world;
        const std::uint64_t floor = level(world.handle, fbe_geometry_plane(1));
        fbe_entity_set_scale(floor, 30.0F, 1.0F, 30.0F, FBE_SPACE_LOCAL);
        std::uniform_real_distribution<float> place(-12.0F, 12.0F);
        std::uniform_real_distribution<float> unit(-1.0F, 1.0F);
        std::uniform_real_distribution<float> size(0.3F, 2.0F);
        std::vector<std::uint64_t> solids;
        for (int i = 0; i < 40; ++i) {
            const std::uint64_t solid =
                i % 2 == 0 ? box(world.handle, extent(-1.0F, -1.0F, -1.0F, 1.0F, 1.0F, 1.0F))
                           : level(world.handle, fbe_geometry_cube());
            const float sx = size(random);
            const float sy = size(random);
            const float sz = size(random);
            fbe_entity_set_scale(solid, sx, sy, sz, FBE_SPACE_LOCAL);
            const float qx = unit(random);
            const float qy = unit(random);
            const float qz = unit(random);
            const float qw = unit(random);
            fbe_entity_set_rotation(solid, qx, qy, qz, qw, FBE_SPACE_WORLD);
            const float px = place(random);
            const float py = size(random) - 0.5F;
            const float pz = place(random);
            fbe_entity_set_position(solid, px, py, pz, FBE_SPACE_WORLD);
            solids.push_back(solid);
        }
        const auto inside_any = [&](fbe_vec3_t p) {
            return std::ranges::any_of(solids, [&](std::uint64_t solid) {
                fbe_vec3_t local{};
                fbe_entity_transform(0, solid, FBE_TRANSFORM_POINT, p.x, p.y, p.z, &local);
                return std::fabs(local.x) < 1.0F && std::fabs(local.y) < 1.0F &&
                       std::fabs(local.z) < 1.0F;
            });
        };
        const auto clear = [&](fbe_vec3_t p) {
            return std::ranges::all_of(solids, [&](std::uint64_t solid) {
                fbe_mat4_t matrix{};
                fbe_entity_matrix(solid, FBE_SPACE_WORLD, &matrix);
                return apart(matrix, extent(-1.0F, -1.0F, -1.0F, 1.0F, 1.0F, 1.0F), vec(p), 0.5F) >
                       0.3F;
            });
        };
        fbe_vec3_t start{};
        do {
            const float sx = place(random);
            const float sz = place(random);
            start = { .x = sx, .y = 3.0F, .z = sz };
        } while (inside_any(start) || !clear(start));
        const std::uint64_t made = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
        fbe_entity_set_collision_group(made, mover_group);
        fbe_entity_set_collision_radius(made, 0.3F, 0.6F);
        fbe_entity_set_position(made, start.x, start.y, start.z, FBE_SPACE_WORLD);
        fbe_world_collide(world.handle);
        fbe_entity_set_step_height(made, 0.2F);
        std::uniform_real_distribution<float> speed(0.0F, 0.6F);
        float fall = 0.0F;
        float dx = 0.0F;
        float dz = 0.0F;
        float worst = 0.0F;
        for (int frame = 0; frame < frames; ++frame) {
            if (frame % 30 == 0) {
                const float pace = speed(random);
                // The review's walk's own constant, so its seeds walk as they did.
                const float angle = unit(random) * 3.14159F; // NOLINT(modernize-use-std-numbers)
                dx = std::cos(angle) * pace;
                dz = std::sin(angle) * pace;
            }
            fall = std::min(fall + 0.02F, 0.5F);
            move(world.handle, made, dx, -fall, dz);
            std::array<fbe_collision_t, 32> touched{};
            const std::int32_t count = fbe_entity_collisions(made, touched.data(), 32);
            for (std::int32_t i = 0; i < std::min(count, 32); ++i) {
                if (touched[static_cast<std::size_t>(i)].normal.y > 0.7F) {
                    fall = 0.0F;
                }
            }
            const fbe::scene::vec3_t at = vec(position(made));
            for (const std::uint64_t solid : solids) {
                fbe_mat4_t matrix{};
                fbe_entity_matrix(solid, FBE_SPACE_WORLD, &matrix);
                worst = std::fmax(
                    worst,
                    0.3F - apart(matrix, extent(-1.0F, -1.0F, -1.0F, 1.0F, 1.0F, 1.0F), at, 0.5F));
            }
            if (std::fabs(at.x) > 14.0F || std::fabs(at.z) > 14.0F) {
                fbe_entity_set_position(made, start.x, 3.0F, start.z, FBE_SPACE_WORLD);
                fbe_entity_reset_collisions(made);
                fbe_world_collide(world.handle);
            }
        }
        CHECK(worst < 0.005F);
    }
}
