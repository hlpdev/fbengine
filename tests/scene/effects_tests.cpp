// Particle emitters' counts and limits and decals' triangles, through the C API, with no GPU.

#include "fbengine.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

namespace {

using Catch::Approx;

/// A decal's limit, as the FB side has it by default.
constexpr float decal_limit = 1.309F;

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

std::string last_error() {
    return fbe_error();
}

/// A box of half-sizes at a place, its axes the world's.
fbe_mat4_t box_at(float x, float y, float z, float half_x, float half_y, float half_z) {
    fbe_mat4_t box{};
    box.m[0] = half_x;
    box.m[5] = half_y;
    box.m[10] = half_z;
    box.m[12] = x;
    box.m[13] = y;
    box.m[14] = z;
    box.m[15] = 1.0F;
    return box;
}

/// A mesh of a cube, -1 to 1, in a world.
std::uint64_t cube_mesh(std::uint64_t world) {
    const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    const std::uint64_t geometry = fbe_geometry_cube();
    fbe_mesh_set_geometry(mesh, geometry);
    fbe_geometry_release(geometry);
    return mesh;
}

std::vector<fbe_vertex_t> vertices_of(std::uint64_t geometry) {
    const std::int32_t count = fbe_surface_vertex_count(geometry, 0);
    std::vector<fbe_vertex_t> vertices(static_cast<std::size_t>(count));
    fbe_surface_vertices(geometry, 0, 0, vertices.data(), count);
    return vertices;
}

} // namespace

TEST_CASE("a new emitter has its defaults, and holds none alive", "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    REQUIRE(emitter != 0);
    CHECK(fbe_entity_kind(emitter) == FBE_ENTITY_PARTICLES);
    CHECK(fbe_particles_capacity(emitter) == 1024);
    CHECK(fbe_particles_rate(emitter) == 32.0F);
    CHECK(fbe_particles_alive(emitter) == 0);
    CHECK(fbe_particles_material(emitter) == 0);
}

TEST_CASE("an emitter spawns at its rate as steps go by, and what's lived its life dies",
          "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    REQUIRE(fbe_particles_set_rate(emitter, 10.0F));
    REQUIRE(fbe_particles_set_life(emitter, 1.0F, 1.0F));
    REQUIRE(fbe_world_step_particles(world.handle, 0.5F));
    CHECK(fbe_particles_alive(emitter) == 5);
    // A rate's fraction carries over: a tenth of a step at a time adds up.
    for (int i = 0; i < 2; ++i) {
        REQUIRE(fbe_world_step_particles(world.handle, 0.05F));
    }
    CHECK(fbe_particles_alive(emitter) == 6);
    // The first's born a twentieth of a second in, so a whole life on from then is only them gone.
    REQUIRE(fbe_world_step_particles(world.handle, 0.5F));
    CHECK(fbe_particles_alive(emitter) == 10);
    REQUIRE(fbe_particles_set_rate(emitter, 0.0F));
    REQUIRE(fbe_world_step_particles(world.handle, 1.0F));
    CHECK(fbe_particles_alive(emitter) == 0);
}

TEST_CASE("a burst spawns at once, up to what an emitter holds", "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    REQUIRE(fbe_particles_set_capacity(emitter, 4));
    REQUIRE(fbe_particles_set_rate(emitter, 0.0F));
    REQUIRE(fbe_particles_burst(emitter, 3));
    CHECK(fbe_particles_alive(emitter) == 3);
    REQUIRE(fbe_particles_burst(emitter, 100));
    CHECK(fbe_particles_alive(emitter) == 4);
    // Setting what it holds loses them.
    REQUIRE(fbe_particles_set_capacity(emitter, 8));
    CHECK(fbe_particles_alive(emitter) == 0);
}

TEST_CASE("a step longer than a life leaves only what could have lived through it",
          "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    REQUIRE(fbe_particles_set_rate(emitter, 100.0F));
    REQUIRE(fbe_particles_set_life(emitter, 0.5F, 0.5F));
    REQUIRE(fbe_world_step_particles(world.handle, 10.0F));
    CHECK(fbe_particles_alive(emitter) == 50);
}

TEST_CASE("a hidden emitter spawns none, and no others in another World step",
          "[scene][particles]") {
    const test_world_t world;
    const test_world_t other;
    const std::uint64_t hidden = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    const std::uint64_t elsewhere = fbe_entity_create(other.handle, 0, FBE_ENTITY_PARTICLES);
    REQUIRE(fbe_entity_set_hidden(hidden, true));
    REQUIRE(fbe_world_step_particles(world.handle, 1.0F));
    CHECK(fbe_particles_alive(hidden) == 0);
    CHECK(fbe_particles_alive(elsewhere) == 0);
}

TEST_CASE("a copy of an emitter has its settings and none of its particles", "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    const std::uint64_t material = fbe_material_create();
    REQUIRE(fbe_particles_set_material(emitter, material));
    REQUIRE(fbe_particles_set_capacity(emitter, 16));
    REQUIRE(fbe_particles_set_rate(emitter, 3.0F));
    REQUIRE(fbe_particles_burst(emitter, 5));
    const std::uint64_t copy = fbe_entity_copy(emitter, world.handle, 0);
    REQUIRE(copy != 0);
    CHECK(fbe_particles_capacity(copy) == 16);
    CHECK(fbe_particles_rate(copy) == 3.0F);
    CHECK(fbe_particles_material(copy) == material);
    CHECK(fbe_particles_alive(copy) == 0);
    CHECK(fbe_particles_alive(emitter) == 5);
    // Each holds the material: freeing one leaves the other's.
    REQUIRE(fbe_entity_free(emitter));
    CHECK(fbe_particles_material(copy) == material);
    fbe_material_release(material);
    CHECK(fbe_particles_material(copy) == material);
}

TEST_CASE("an emitter's limits are checked", "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    const float nan = std::nanf("");
    const float pi = std::numbers::pi_v<float>;
    CHECK_FALSE(fbe_particles_set_capacity(emitter, 0));
    CHECK(last_error().contains("not 0"));
    CHECK_FALSE(fbe_particles_set_capacity(emitter, 1048577));
    CHECK(fbe_particles_set_capacity(emitter, 1048576));
    CHECK(fbe_particles_set_capacity(emitter, 1024));
    CHECK_FALSE(fbe_particles_set_rate(emitter, -1.0F));
    CHECK_FALSE(fbe_particles_set_rate(emitter, nan));
    CHECK_FALSE(fbe_particles_set_life(emitter, 0.0F, 1.0F));
    CHECK_FALSE(fbe_particles_set_life(emitter, 2.0F, 1.0F));
    CHECK_FALSE(fbe_particles_set_life(emitter, 1.0F, 513.0F));
    CHECK(fbe_particles_set_life(emitter, 1.0F, 512.0F));
    CHECK_FALSE(fbe_particles_set_motion(emitter, -1.0F, 1.0F, 0.0F, 0.0F));
    CHECK_FALSE(fbe_particles_set_motion(emitter, 2.0F, 1.0F, 0.0F, 0.0F));
    CHECK_FALSE(fbe_particles_set_motion(emitter, 1.0F, 1.0F, pi + 1.0F, 0.0F));
    CHECK_FALSE(fbe_particles_set_motion(emitter, 1.0F, 1.0F, 0.0F, -1.0F));
    CHECK(fbe_particles_set_motion(emitter, 1.0F, 1.0F, pi, 1.0F));
    CHECK_FALSE(fbe_particles_set_forces(emitter, nan, 0.0F, 0.0F, 0.0F));
    CHECK_FALSE(fbe_particles_set_forces(emitter, 0.0F, 0.0F, 0.0F, -1.0F));
    CHECK_FALSE(fbe_particles_set_sizes(emitter, -1.0F, 1.0F));
    CHECK_FALSE(fbe_particles_burst(emitter, -1));
    CHECK_FALSE(fbe_world_step_particles(world.handle, -1.0F));
    CHECK_FALSE(fbe_world_step_particles(world.handle, nan));
    const std::uint64_t mesh = fbe_entity_create(world.handle, 0, FBE_ENTITY_MESH);
    CHECK_FALSE(fbe_particles_set_rate(mesh, 1.0F));
    CHECK(last_error() == "the entity isn't a particle emitter");
}

TEST_CASE("a sky's sun is a directional light of its World, and a brightness 0 or more",
          "[scene][sky]") {
    const test_world_t world;
    const test_world_t other;
    const std::uint64_t sun = fbe_entity_create(world.handle, 0, FBE_ENTITY_LIGHT);
    REQUIRE(fbe_light_set_type(sun, FBE_LIGHT_DIRECTIONAL));
    CHECK(fbe_world_set_sky_atmosphere(world.handle, sun, 20.0F));
    CHECK(fbe_world_set_sky_atmosphere(world.handle, 0, 20.0F));
    CHECK_FALSE(fbe_world_set_sky_atmosphere(world.handle, 0, -1.0F));
    CHECK_FALSE(fbe_world_set_sky_atmosphere(other.handle, sun, 1.0F));
    CHECK(last_error() == "a sky's sun is in its World");
    REQUIRE(fbe_light_set_type(sun, FBE_LIGHT_POINT));
    CHECK_FALSE(fbe_world_set_sky_atmosphere(world.handle, sun, 1.0F));
    CHECK(last_error() == "a sky's sun is a directional light");
    CHECK(fbe_world_clear_sky(world.handle));
}

TEST_CASE("a decal is what's in its box on faces toward it, clipped to it", "[scene][decal]") {
    const test_world_t world;
    cube_mesh(world.handle);
    // Over the cube's +Z face, whose corners it cuts in half.
    const fbe_mat4_t box = box_at(0.0F, 0.0F, 1.0F, 0.5F, 0.5F, 0.5F);
    const std::uint64_t made = fbe_geometry_decal(world.handle, &box, 0.01F, decal_limit);
    REQUIRE(made != 0);
    const std::vector<fbe_vertex_t> vertices = vertices_of(made);
    REQUIRE(!vertices.empty());
    for (const fbe_vertex_t& vertex : vertices) {
        CHECK(vertex.position.z == Approx(1.01F));
        CHECK(std::fabs(vertex.position.x) <= 0.5F + 0.0001F);
        CHECK(std::fabs(vertex.position.y) <= 0.5F + 0.0001F);
        CHECK(vertex.normal.z == Approx(1.0F));
        // Across its X and down its Y.
        CHECK(vertex.uv0.x == Approx(vertex.position.x + 0.5F));
        CHECK(vertex.uv0.y == Approx(0.5F - vertex.position.y));
        CHECK(vertex.uv1.x == vertex.uv0.x);
    }
    fbe_geometry_release(made);
}

TEST_CASE("a decal leaves out what faces away, or isn't in its box", "[scene][decal]") {
    const test_world_t world;
    cube_mesh(world.handle);
    // Turned to look at the cube's far side, none of it faces the box's +Z.
    const fbe_mat4_t away = box_at(0.0F, 0.0F, -1.0F, 0.5F, 0.5F, -0.5F);
    const std::uint64_t behind = fbe_geometry_decal(world.handle, &away, 0.0F, decal_limit);
    REQUIRE(behind != 0);
    const std::vector<fbe_vertex_t> back = vertices_of(behind);
    REQUIRE(!back.empty());
    for (const fbe_vertex_t& vertex : back) {
        CHECK(vertex.position.z == Approx(-1.0F));
    }
    fbe_geometry_release(behind);
    const fbe_mat4_t far = box_at(10.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F);
    const std::uint64_t nothing = fbe_geometry_decal(world.handle, &far, 0.0F, decal_limit);
    REQUIRE(nothing != 0);
    CHECK(fbe_surface_vertex_count(nothing, 0) == 0);
    fbe_geometry_release(nothing);
}

TEST_CASE("a decal goes on a moved, scaled, or mirrored mesh, as it shows", "[scene][decal]") {
    const test_world_t world;
    const std::uint64_t mesh = cube_mesh(world.handle);
    REQUIRE(fbe_entity_set_position(mesh, 5.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    REQUIRE(fbe_entity_set_scale(mesh, 2.0F, 2.0F, 2.0F, FBE_SPACE_WORLD));
    const fbe_mat4_t box = box_at(5.0F, 0.0F, 2.0F, 0.5F, 0.5F, 0.5F);
    const std::uint64_t scaled = fbe_geometry_decal(world.handle, &box, 0.0F, decal_limit);
    const std::vector<fbe_vertex_t> on_scaled = vertices_of(scaled);
    REQUIRE(!on_scaled.empty());
    CHECK(on_scaled[0].position.z == Approx(2.0F));
    fbe_geometry_release(scaled);
    // Mirrored in x: its faces still face out.
    REQUIRE(fbe_entity_set_scale(mesh, -2.0F, 2.0F, 2.0F, FBE_SPACE_WORLD));
    const std::uint64_t mirrored = fbe_geometry_decal(world.handle, &box, 0.0F, decal_limit);
    const std::vector<fbe_vertex_t> on_mirrored = vertices_of(mirrored);
    REQUIRE(!on_mirrored.empty());
    CHECK(on_mirrored[0].normal.z == Approx(1.0F));
    fbe_geometry_release(mirrored);
}

TEST_CASE("a decal goes on what's opaque, so never on another", "[scene][decal]") {
    const test_world_t world;
    const std::uint64_t mesh = cube_mesh(world.handle);
    const std::uint64_t glass = fbe_material_create();
    REQUIRE(fbe_material_set_blend(glass, FBE_BLEND_ALPHA));
    REQUIRE(fbe_mesh_set_material(mesh, glass));
    const fbe_mat4_t box = box_at(0.0F, 0.0F, 1.0F, 0.5F, 0.5F, 0.5F);
    const std::uint64_t on_glass = fbe_geometry_decal(world.handle, &box, 0.0F, decal_limit);
    CHECK(fbe_surface_vertex_count(on_glass, 0) == 0);
    fbe_geometry_release(on_glass);
    REQUIRE(fbe_mesh_set_material(mesh, 0));
    const std::uint64_t first = fbe_geometry_decal(world.handle, &box, 0.01F, decal_limit);
    const std::int32_t alone = fbe_surface_vertex_count(first, 0);
    REQUIRE(alone > 0);
    const std::uint64_t drawn = fbe_entity_create(world.handle, 0, FBE_ENTITY_MESH);
    REQUIRE(fbe_mesh_set_geometry(drawn, first));
    REQUIRE(fbe_mesh_set_material(drawn, glass));
    const std::uint64_t second = fbe_geometry_decal(world.handle, &box, 0.01F, decal_limit);
    CHECK(fbe_surface_vertex_count(second, 0) == alone);
    fbe_geometry_release(first);
    fbe_geometry_release(second);
    fbe_material_release(glass);
}

TEST_CASE("a decal goes on a surface by the material its mesh has for it", "[scene][decal]") {
    const test_world_t world;
    const std::uint64_t mesh = cube_mesh(world.handle);
    const std::uint64_t glass = fbe_material_create();
    REQUIRE(fbe_material_set_blend(glass, FBE_BLEND_ALPHA));
    const std::uint64_t solid = fbe_material_create();
    const fbe_mat4_t box = box_at(0.0F, 0.0F, 1.0F, 0.5F, 0.5F, 0.5F);
    const auto landed = [&] {
        const std::uint64_t decal = fbe_geometry_decal(world.handle, &box, 0.01F, decal_limit);
        const std::int32_t count = fbe_surface_vertex_count(decal, 0);
        fbe_geometry_release(decal);
        return count;
    };
    REQUIRE(landed() > 0);
    // Glass for that surface alone: it skips it.
    REQUIRE(fbe_mesh_set_surface_material(mesh, 0, glass));
    CHECK(landed() == 0);
    REQUIRE(fbe_mesh_set_surface_material(mesh, 0, 0));
    CHECK(landed() > 0);
    // Glass for all, and solid for that surface: it lands.
    REQUIRE(fbe_mesh_set_material(mesh, glass));
    CHECK(landed() == 0);
    REQUIRE(fbe_mesh_set_surface_material(mesh, 0, solid));
    CHECK(landed() > 0);
    fbe_material_release(glass);
    fbe_material_release(solid);
}

TEST_CASE("a decal goes on a terrain and a ground plane", "[scene][decal]") {
    const test_world_t world;
    // Facing up: its +Z is the world's +Y.
    fbe_mat4_t box{};
    box.m[0] = 1.0F;
    box.m[6] = -1.0F;
    box.m[9] = 1.0F;
    box.m[12] = 1.5F;
    box.m[13] = 0.0F;
    box.m[14] = 1.5F;
    box.m[15] = 1.0F;
    const std::uint64_t terrain = fbe_entity_create(world.handle, 0, FBE_ENTITY_TERRAIN);
    REQUIRE(fbe_terrain_set_size(terrain, 4));
    const std::uint64_t on_terrain = fbe_geometry_decal(world.handle, &box, 0.0F, decal_limit);
    const std::vector<fbe_vertex_t> over_terrain = vertices_of(on_terrain);
    REQUIRE(!over_terrain.empty());
    for (const fbe_vertex_t& vertex : over_terrain) {
        CHECK(vertex.position.y == Approx(0.0F).margin(0.0001));
        CHECK(vertex.normal.y == Approx(1.0F));
        CHECK(std::fabs(vertex.position.x - 1.5F) <= 1.0001F);
    }
    fbe_geometry_release(on_terrain);
    REQUIRE(fbe_entity_free(terrain));
    const std::uint64_t plane = fbe_entity_create(world.handle, 0, FBE_ENTITY_PLANE);
    REQUIRE(plane != 0);
    const std::uint64_t on_plane = fbe_geometry_decal(world.handle, &box, 0.0F, decal_limit);
    const std::vector<fbe_vertex_t> over_plane = vertices_of(on_plane);
    REQUIRE(!over_plane.empty());
    for (const fbe_vertex_t& vertex : over_plane) {
        CHECK(vertex.position.y == Approx(0.0F).margin(0.0001));
        CHECK(vertex.normal.y == Approx(1.0F));
    }
    fbe_geometry_release(on_plane);
}

TEST_CASE("a decal's box has an inverse, its lift's 0 or more, and its World's live",
          "[scene][decal]") {
    const test_world_t world;
    const fbe_mat4_t flat = box_at(0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F);
    CHECK(fbe_geometry_decal(world.handle, &flat, 0.0F, decal_limit) == 0);
    CHECK(last_error() == "a decal's box has an inverse");
    const fbe_mat4_t box = box_at(0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F);
    CHECK(fbe_geometry_decal(world.handle, &box, -1.0F, decal_limit) == 0);
    CHECK(fbe_geometry_decal(0, &box, 0.0F, decal_limit) == 0);
}

TEST_CASE("a long step leaves as many as a life's worth, up to what it holds",
          "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    REQUIRE(fbe_particles_set_rate(emitter, 1000.0F));
    REQUIRE(fbe_particles_set_life(emitter, 0.2F, 0.2F));
    REQUIRE(fbe_particles_set_capacity(emitter, 200));
    REQUIRE(fbe_world_step_particles(world.handle, 1.0F));
    CHECK(fbe_particles_alive(emitter) == 200);
}

TEST_CASE("a huge step and a huge rate come to nothing strange", "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    REQUIRE(fbe_particles_set_rate(emitter, 1e9F));
    REQUIRE(fbe_particles_set_life(emitter, 1.0F, 1.0F));
    REQUIRE(fbe_world_step_particles(world.handle, 1e30F));
    CHECK(fbe_particles_alive(emitter) == 1024);
    REQUIRE(fbe_particles_set_rate(emitter, 0.0F));
    REQUIRE(fbe_world_step_particles(world.handle, 2.0F));
    CHECK(fbe_particles_alive(emitter) == 0);
    REQUIRE(fbe_particles_set_rate(emitter, 3e38F));
    REQUIRE(fbe_world_step_particles(world.handle, 1e30F));
    REQUIRE(fbe_particles_set_rate(emitter, 0.0F));
    REQUIRE(fbe_world_step_particles(world.handle, 2.0F));
    CHECK(fbe_particles_alive(emitter) == 0);
}

TEST_CASE("a particle's colors and roll are checked", "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    const fbe_color_t fine{ .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F };
    const fbe_color_t nan_red{ .r = std::nanf(""), .g = 1.0F, .b = 1.0F, .a = 1.0F };
    const fbe_color_t two_alpha{ .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 2.0F };
    CHECK(fbe_particles_set_colors(emitter, &fine, &fine));
    CHECK_FALSE(fbe_particles_set_colors(emitter, &nan_red, &fine));
    CHECK_FALSE(fbe_particles_set_colors(emitter, &fine, &two_alpha));
    CHECK_FALSE(fbe_particles_set_roll(emitter, -1.0F));
    CHECK_FALSE(fbe_particles_set_roll(emitter, 4.0F));
    CHECK_FALSE(fbe_particles_set_roll(emitter, std::nanf("")));
    CHECK(fbe_particles_set_roll(emitter, 3.0F));
}

TEST_CASE("a decal on what's curved has its normals, and neighbors meet", "[scene][decal]") {
    const test_world_t world;
    const std::uint64_t mesh = fbe_entity_create(world.handle, 0, FBE_ENTITY_MESH);
    const std::uint64_t sphere = fbe_geometry_sphere(12);
    fbe_mesh_set_geometry(mesh, sphere);
    fbe_geometry_release(sphere);
    const fbe_mat4_t box = box_at(0.0F, 0.0F, 1.0F, 0.6F, 0.6F, 0.6F);
    const std::uint64_t made = fbe_geometry_decal(world.handle, &box, 0.0F, decal_limit);
    const std::vector<fbe_vertex_t> vertices = vertices_of(made);
    REQUIRE(vertices.size() > 8);
    for (const fbe_vertex_t& one : vertices) {
        const float apart =
            std::sqrt((one.position.x * one.position.x) + (one.position.y * one.position.y) +
                      (one.position.z * one.position.z));
        const float along = ((one.position.x * one.normal.x) + (one.position.y * one.normal.y) +
                             (one.position.z * one.normal.z)) /
                            apart;
        CHECK(along > 0.99F);
        for (const fbe_vertex_t& other : vertices) {
            if (std::fabs(one.position.x - other.position.x) < 1e-5F &&
                std::fabs(one.position.y - other.position.y) < 1e-5F &&
                std::fabs(one.position.z - other.position.z) < 1e-5F) {
                CHECK(std::fabs(one.normal.x - other.normal.x) < 1e-3F);
                CHECK(std::fabs(one.normal.y - other.normal.y) < 1e-3F);
                CHECK(std::fabs(one.normal.z - other.normal.z) < 1e-3F);
            }
        }
    }
    fbe_geometry_release(made);
}

TEST_CASE("a decal stops at its limit, fading on the way", "[scene][decal]") {
    const test_world_t world;
    const std::uint64_t mesh = cube_mesh(world.handle);
    const auto turned_by = [&](float angle) {
        const float half = angle * 0.5F;
        REQUIRE(fbe_entity_set_rotation(mesh, 0.0F, std::sin(half), 0.0F, std::cos(half),
                                        FBE_SPACE_WORLD));
        const fbe_mat4_t box = box_at(0.0F, 0.0F, 0.0F, 2.0F, 2.0F, 2.0F);
        const std::uint64_t made = fbe_geometry_decal(world.handle, &box, 0.0F, decal_limit);
        std::vector<fbe_vertex_t> vertices = vertices_of(made);
        fbe_geometry_release(made);
        return vertices;
    };
    const float degrees = std::numbers::pi_v<float> / 180.0F;
    // Past the limit, on neither of what's at the box: 85 off, and the other at 5, which it keeps.
    const auto faces_off = [](const std::vector<fbe_vertex_t>& vertices, float at_least) {
        return std::ranges::any_of(
            vertices, [&](const fbe_vertex_t& one) { return one.normal.z < at_least; });
    };
    CHECK_FALSE(faces_off(turned_by(85.0F * degrees), std::cos(80.0F * degrees)));
    const std::vector<fbe_vertex_t> at_seventy = turned_by(70.0F * degrees);
    REQUIRE(!at_seventy.empty());
    bool fading = false;
    for (const fbe_vertex_t& one : at_seventy) {
        fading = fading || (one.color.a > 0.0F && one.color.a < 1.0F);
    }
    CHECK(fading);
    const std::vector<fbe_vertex_t> head_on = turned_by(0.0F);
    REQUIRE(!head_on.empty());
    CHECK(head_on[0].color.a == 1.0F);
    const fbe_mat4_t box = box_at(0.0F, 0.0F, 0.0F, 2.0F, 2.0F, 2.0F);
    CHECK(fbe_geometry_decal(world.handle, &box, 0.0F, 0.0F) == 0);
    CHECK(fbe_geometry_decal(world.handle, &box, 0.0F, 2.0F) == 0);
    CHECK(fbe_geometry_decal(world.handle, &box, 0.0F, std::nanf("")) == 0);
}

TEST_CASE("a decal goes on a two-sided surface's back, facing the box", "[scene][decal]") {
    const test_world_t world;
    const std::uint64_t mesh = fbe_entity_create(world.handle, 0, FBE_ENTITY_MESH);
    const std::uint64_t quad = fbe_geometry_create();
    const std::int32_t surface = fbe_geometry_add_surface(quad);
    const auto corner = [](float x, float y) {
        return fbe_vertex_t{
            .position = { .x = x, .y = y, .z = 0.0F },
            .normal = { .x = 0.0F, .y = 0.0F, .z = 1.0F },
            .uv0 = {},
            .uv1 = {},
            .color = { .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F },
        };
    };
    const fbe_vertex_t low_left = corner(-1.0F, -1.0F);
    const fbe_vertex_t low_right = corner(1.0F, -1.0F);
    const fbe_vertex_t high_right = corner(1.0F, 1.0F);
    const fbe_vertex_t high_left = corner(-1.0F, 1.0F);
    const std::array corners{ low_left, low_right, high_right, high_left };
    fbe_surface_add_vertices(quad, surface, corners.data(), 4);
    const std::array<std::uint32_t, 6> indices{ 0, 1, 2, 0, 2, 3 };
    fbe_surface_add_triangles(quad, surface, indices.data(), 6);
    fbe_mesh_set_geometry(mesh, quad);
    fbe_geometry_release(quad);
    // Behind it, looking at its back: its +Z is the world's -Z.
    const fbe_mat4_t box = box_at(0.0F, 0.0F, 0.0F, 0.5F, 0.5F, -0.5F);
    const std::uint64_t one_sided = fbe_geometry_decal(world.handle, &box, 0.0F, decal_limit);
    CHECK(fbe_surface_vertex_count(one_sided, 0) == 0);
    fbe_geometry_release(one_sided);
    const std::uint64_t both = fbe_material_create();
    REQUIRE(fbe_material_set_two_sided(both, true));
    REQUIRE(fbe_mesh_set_material(mesh, both));
    const std::uint64_t made = fbe_geometry_decal(world.handle, &box, 0.01F, decal_limit);
    const std::vector<fbe_vertex_t> vertices = vertices_of(made);
    REQUIRE(!vertices.empty());
    for (const fbe_vertex_t& one : vertices) {
        CHECK(one.normal.z == Approx(-1.0F));
        CHECK(one.position.z == Approx(-0.01F));
    }
    std::array<std::uint32_t, 3> triangle{};
    REQUIRE(fbe_surface_triangles(made, 0, 0, triangle.data(), 1));
    const fbe_vertex_t& a = vertices[triangle[0]];
    const fbe_vertex_t& b = vertices[triangle[1]];
    const fbe_vertex_t& c = vertices[triangle[2]];
    const float winding = ((b.position.x - a.position.x) * (c.position.y - a.position.y)) -
                          ((b.position.y - a.position.y) * (c.position.x - a.position.x));
    // Counterclockwise from the side it faces, so clockwise from +Z.
    CHECK(winding < 0.0F);
    fbe_geometry_release(made);
    fbe_material_release(both);
}
